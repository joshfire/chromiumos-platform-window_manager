// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor/compositor.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <gflags/gflags.h>

#include "base/logging.h"
#include "base/string_util.h"
#include "window_manager/callback.h"
#if defined(COMPOSITOR_OPENGL)
#include "window_manager/compositor/gl/opengl_visitor.h"
#elif defined(COMPOSITOR_OPENGLES)
#include "window_manager/compositor/gles/opengles_visitor.h"
#endif
#include "window_manager/compositor/layer_visitor.h"
#include "window_manager/event_loop.h"
#include "window_manager/image_container.h"
#include "window_manager/profiler.h"
#include "window_manager/util.h"
#include "window_manager/x11/x_connection.h"

DEFINE_bool(compositor_display_debug_needle, false,
            "Specify this to turn on a debugging aid for seeing when "
            "frames are being drawn.");

DEFINE_int64(draw_timeout_ms, 16,
             "Minimum time in milliseconds between scene redraws.");

using base::TimeDelta;
using base::TimeTicks;
using std::find;
using std::make_pair;
using std::map;
using std::max;
using std::min;
using std::set;
using std::string;
using std::tr1::shared_ptr;
using std::tr1::unordered_set;
using window_manager::util::FindWithDefault;
using window_manager::util::GetMonotonicTime;
using window_manager::util::XidStr;

// Turn this on if you want to debug the visitor traversal.
#undef EXTRA_LOGGING

namespace window_manager {

static const float kDimmedOpacityBegin = 0.2f;
static const float kDimmedOpacityEnd = 0.6f;
// Project layers to depths between 0 and 1.
static const float kProjectedDepthMin = 0.0f;
static const float kProjectedDepthMax = 1.0f;

// Template used to round float values returned by animations to integers when
// applied to integer properties (read: position).
template<class T>
static T MaybeRoundFloat(float value) {
  return static_cast<T>(value);
}

template<>
int MaybeRoundFloat(float value) {
  return static_cast<int>(roundf(value));
}

void RealCompositor::ActorVisitor::VisitContainer(ContainerActor* actor) {
  CHECK(actor);
  this->VisitActor(actor);
  ActorVector children = actor->GetChildren();
  ActorVector::const_iterator iterator = children.begin();
  while (iterator != children.end()) {
    if (*iterator) {
      (*iterator)->Accept(this);
    }
    ++iterator;
  }
}


RealCompositor::Actor::Actor(RealCompositor* compositor)
    : compositor_(compositor),
      parent_(NULL),
      x_(0),
      y_(0),
      width_(1),
      height_(1),
      z_(0.f),
      scale_x_(1.f),
      scale_y_(1.f),
      opacity_(1.f),
      tilt_(0.f),
      culled_(false),
      model_view_(Matrix4::identity()),
      is_opaque_(false),
      has_children_(false),
      is_shown_(true),
      dimmed_opacity_begin_(0.f),
      dimmed_opacity_end_(0.f) {
  compositor_->AddActor(this);
}

RealCompositor::Actor::~Actor() {
  if (parent_) {
    parent_->RemoveActor(this);
  }
  compositor_->RemoveActor(this);
}

RealCompositor::Actor* RealCompositor::Actor::Clone() {
  Actor* new_instance = new Actor(compositor_);
  Actor::CloneImpl(new_instance);
  return new_instance;
}

void RealCompositor::Actor::CloneImpl(RealCompositor::Actor* clone) {
  clone->x_ = x_;
  clone->y_ = y_;
  clone->width_ = width_;
  clone->height_ = height_;
  clone->parent_ = NULL;
  clone->z_ = 0.0f;
  clone->scale_x_ = scale_x_;
  clone->scale_y_ = scale_y_;
  clone->opacity_ = opacity_;
  clone->tilt_ = tilt_;
  clone->is_opaque_ = is_opaque_;
  clone->has_children_ = has_children_;
  clone->is_shown_ = is_shown_;
  clone->name_ = name_;
}

void RealCompositor::Actor::Move(int x, int y, int duration_ms) {
  MoveX(x, duration_ms);
  MoveY(y, duration_ms);
}

void RealCompositor::Actor::MoveX(int x, int duration_ms) {
  AnimateField(&int_animations_, &x_, x,
               TimeDelta::FromMilliseconds(duration_ms));
}

void RealCompositor::Actor::MoveY(int y, int duration_ms) {
  AnimateField(&int_animations_, &y_, y,
               TimeDelta::FromMilliseconds(duration_ms));
}

AnimationPair* RealCompositor::Actor::CreateMoveAnimation() {
  Animation* x_anim = CreateAnimationForField(&x_);
  Animation* y_anim = CreateAnimationForField(&y_);
  return new AnimationPair(x_anim, y_anim);
}

void RealCompositor::Actor::SetMoveAnimation(AnimationPair* animations) {
  DCHECK(animations);
  SetAnimationForField(
      &int_animations_, &x_, animations->release_first_animation());
  SetAnimationForField(
      &int_animations_, &y_, animations->release_second_animation());
  delete animations;
}

void RealCompositor::Actor::Scale(double scale_x, double scale_y,
                                  int duration_ms) {
  TimeDelta duration = TimeDelta::FromMilliseconds(duration_ms);
  AnimateField(&float_animations_, &scale_x_, static_cast<float>(scale_x),
               duration);
  AnimateField(&float_animations_, &scale_y_, static_cast<float>(scale_y),
               duration);
}

void RealCompositor::Actor::SetOpacity(double opacity, int duration_ms) {
  AnimateField(&float_animations_, &opacity_, static_cast<float>(opacity),
               TimeDelta::FromMilliseconds(duration_ms));
}

void RealCompositor::Actor::SetTilt(double tilt, int duration_ms) {
  AnimateField(&float_animations_, &tilt_,
               static_cast<float>(tilt),
               TimeDelta::FromMilliseconds(duration_ms));
}

void RealCompositor::Actor::Raise(Compositor::Actor* other) {
  CHECK(parent_) << "Raising actor " << this << ", which has no parent";
  if (other == this) {
    LOG(DFATAL) << "Got request to raise actor " << this << " above itself";
    return;
  }
  RealCompositor::Actor* other_nc =
      dynamic_cast<RealCompositor::Actor*>(other);
  CHECK(other_nc) << "Failed to cast " << other << " to an Actor in Raise()";
  parent_->RaiseChild(this, other_nc);
  SetDirty();
}

void RealCompositor::Actor::Lower(Compositor::Actor* other) {
  CHECK(parent_) << "Lowering actor " << this << ", which has no parent";
  if (other == this) {
    LOG(DFATAL) << "Got request to lower actor " << this << " below itself";
    return;
  }
  RealCompositor::Actor* other_nc =
      dynamic_cast<RealCompositor::Actor*>(other);
  CHECK(other_nc) << "Failed to cast " << other << " to an Actor in Lower()";
  parent_->LowerChild(this, other_nc);
  SetDirty();
}

void RealCompositor::Actor::RaiseToTop() {
  CHECK(parent_) << "Raising actor " << this << ", which has no parent, to top";
  parent_->RaiseChild(this, NULL);
  SetDirty();
}

void RealCompositor::Actor::LowerToBottom() {
  CHECK(parent_) << "Lowering actor " << this << ", which has no parent, "
                 << "to bottom";
  parent_->LowerChild(this, NULL);
  SetDirty();
}

void RealCompositor::Actor::ShowDimmed(bool dimmed, int anim_ms) {
  TimeDelta duration = TimeDelta::FromMilliseconds(anim_ms);
  AnimateField(&float_animations_, &dimmed_opacity_begin_,
               dimmed ? kDimmedOpacityBegin : 0.f, duration);
  AnimateField(&float_animations_, &dimmed_opacity_end_,
               dimmed ? kDimmedOpacityEnd : 0.f, duration);
}

void RealCompositor::Actor::AddToVisibilityGroup(int group_id) {
  visibility_groups_.insert(group_id);
  if (compositor_->using_visibility_groups())
    SetDirty();
}

void RealCompositor::Actor::RemoveFromVisibilityGroup(int group_id) {
  visibility_groups_.erase(group_id);
  if (compositor_->using_visibility_groups())
    SetDirty();
}

string RealCompositor::Actor::GetDebugStringInternal(const string& type_name,
                                                    int indent_level) {
  string out;
  out.assign(indent_level * 2, ' ');
  out += StringPrintf("\"%s\" %p (%s%s) (%d, %d) %dx%d "
                        "scale=(%.2f, %.2f) %.2f%% tilt=%0.2f\n",
                      !name_.empty() ? name_.c_str() : "",
                      this,
                      is_shown_ ? "" : "hidden ",
                      type_name.c_str(),
                      x_, y_,
                      width_, height_,
                      scale_x_, scale_y_,
                      opacity_,
                      tilt_);
  return out;
}

bool RealCompositor::Actor::IsInActiveVisibilityGroup() const {
  if (!compositor_->using_visibility_groups())
    return true;

  const unordered_set<int>& active_groups =
      compositor_->active_visibility_groups();
  for (set<int>::const_iterator it = visibility_groups_.begin();
       it != visibility_groups_.end(); ++it) {
    if (active_groups.count(*it))
      return true;
  }
  return false;
}

void RealCompositor::Actor::Update(int* count, const TimeTicks& now) {
  (*count)++;
  if (int_animations_.empty() && float_animations_.empty())
    return;

  SetDirty();
  UpdateInternal(&int_animations_, now);
  UpdateInternal(&float_animations_, now);
}

void RealCompositor::Actor::UpdateModelView() {
  if (parent() != NULL) {
    model_view_ = parent()->model_view();
  } else {
    model_view_ = Matrix4::identity();
  }
  model_view_ *= Matrix4::translation(Vector3(x(), y(), z()));
  model_view_ *= Matrix4::scale(Vector3(width() * scale_x(),
                                        height() * scale_y(),
                                        1.f));

  if (tilt() > 0.001f) {
    // Post-multiply a perspective matrix onto the model view matrix, and
    // a rotation in Y so that all the other model view ops happen
    // outside of the perspective transform.

    // This matrix is the result of a translate by 0.5 in Y, followed
    // by a simple perspective transform, followed by a translate in
    // -0.5 in Y, so that the perspective foreshortening is centered
    // vertically on the quad.
    static Matrix4 tilt_matrix(Vector4(1.0f, 0.0f, 0.0f, 0.0f),
                               Vector4(0.0f, 1.0f, 0.0f, 0.0f),
                               Vector4(0.0f, -0.2f, 0.0f, -0.4f),
                               Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    model_view_ *= tilt_matrix;
    model_view_ *= Matrix4::rotationY(tilt() * M_PI_2);
  }
}

bool RealCompositor::Actor::IsTransformed() const {
  const Vector4 c0 = model_view_[0];
  const Vector4 c1 = model_view_[1];
  const Vector4 c2 = model_view_[2];
  const Vector4 c3 = model_view_[3];

  // Most entries must be from identity matrix
  if (c0[1] != 0.f || c0[2] != 0.f || c0[3] != 0.f ||
      c1[0] != 0.f || c1[2] != 0.f || c1[3] != 0.f ||
      c2[0] != 0.f || c2[1] != 0.f || c2[2] != 1.f || c2[3] != 0.f ||
      c3[3] != 1.f)
    return true;

  // Check for scale by actor dimensions
  if (c0[0] != width() ||
      c1[1] != height())
    return true;

  // Check for transform by actor origin
  if (c3[0] != x() || c3[1] != y() || c3[2] != z())
    return true;

  return false;
}

template<class T> void RealCompositor::Actor::AnimateField(
    map<T*, shared_ptr<Animation> >* animation_map,
    T* field, T value, const TimeDelta& duration) {
  typeof(animation_map->begin()) iterator = animation_map->find(field);
  // If we're not currently animating the field and it's already at the
  // right value, there's no reason to do anything.
  if (iterator == animation_map->end() && value == *field)
    return;

  if (duration.InMilliseconds() > 0) {
    shared_ptr<Animation> animation(new Animation(*field, GetMonotonicTime()));
    animation->AppendKeyframe(value, duration);
    if (iterator != animation_map->end()) {
      iterator->second = animation;
    } else {
      animation_map->insert(make_pair(field, animation));
      compositor_->IncrementNumAnimations();
    }
  } else {
    if (iterator != animation_map->end()) {
      animation_map->erase(iterator);
      compositor_->DecrementNumAnimations();
    }
    *field = value;
    SetDirty();
  }
}

template<class T> Animation* RealCompositor::Actor::CreateAnimationForField(
    T* field) {
  DCHECK(field);
  return new Animation(*field, GetMonotonicTime());
}

template<class T> void RealCompositor::Actor::SetAnimationForField(
    std::map<T*, std::tr1::shared_ptr<Animation> >* animation_map,
    T* field,
    Animation* new_animation) {
  DCHECK(field);
  DCHECK(new_animation);

  shared_ptr<Animation> animation(new_animation);
  typeof(animation_map->begin()) iterator = animation_map->find(field);
  if (iterator != animation_map->end()) {
    iterator->second = animation;
  } else {
    animation_map->insert(make_pair(field, animation));
    compositor_->IncrementNumAnimations();
  }
}

template<class T> void RealCompositor::Actor::UpdateInternal(
    map<T*, shared_ptr<Animation> >* animation_map, const TimeTicks& now) {
  typeof(animation_map->begin()) iterator = animation_map->begin();
  while (iterator != animation_map->end()) {
    *(iterator->first) = MaybeRoundFloat<T>(iterator->second->GetValue(now));
    if (iterator->second->IsDone(now)) {
      typeof(iterator) old_iterator = iterator;
      ++iterator;
      animation_map->erase(old_iterator);
      compositor_->DecrementNumAnimations();
    } else {
      ++iterator;
    }
  }
}


RealCompositor::ContainerActor::~ContainerActor() {
  for (ActorVector::iterator iterator = children_.begin();
       iterator != children_.end(); ++iterator) {
    (*iterator)->set_parent(NULL);
  }
}

string RealCompositor::ContainerActor::GetDebugString(int indent_level) {
  string out = GetDebugStringInternal("ContainerActor", indent_level);
  for (ActorVector::iterator iterator = children_.begin();
       iterator != children_.end(); ++iterator) {
    out += (*iterator)->GetDebugString(indent_level + 1);
  }
  return out;
}

void RealCompositor::ContainerActor::AddActor(Compositor::Actor* actor) {
  RealCompositor::Actor* cast_actor = dynamic_cast<Actor*>(actor);
  CHECK(cast_actor) << "Unable to down-cast actor.";
  cast_actor->set_parent(this);
  children_.insert(children_.begin(), cast_actor);
  set_has_children(true);
  SetDirty();
}

// Note that the passed-in Actors might be partially destroyed (the
// Actor destructor calls RemoveActor on its parent), so we shouldn't
// rely on the contents of the Actor.
void RealCompositor::ContainerActor::RemoveActor(Compositor::Actor* actor) {
  ActorVector::iterator iterator =
      find(children_.begin(), children_.end(), actor);
  if (iterator != children_.end()) {
    children_.erase(iterator);
    set_has_children(!children_.empty());
    SetDirty();
  }
}

void RealCompositor::ContainerActor::Update(int* count, const TimeTicks& now) {
  for (ActorVector::iterator iterator = children_.begin();
       iterator != children_.end(); ++iterator) {
    (*iterator)->Update(count, now);
  }
  RealCompositor::Actor::Update(count, now);
}

void RealCompositor::ContainerActor::UpdateModelView() {
  if (parent() != NULL) {
    set_model_view(parent()->model_view());
  } else {
    set_model_view(Matrix4::identity());
  }

  // Don't translate by Z because the actors already have their
  // absolute Z values from the layer calculation.
  set_model_view(model_view() * Matrix4::translation(Vector3(x(), y(), 0.0f)));
  set_model_view(model_view() * Matrix4::scale(Vector3(width() * scale_x(),
                                                       height() * scale_y(),
                                                       1.f)));
}

void RealCompositor::ContainerActor::RaiseChild(
    RealCompositor::Actor* child, RealCompositor::Actor* above) {
  CHECK(child) << "Tried to raise a NULL child.";
  if (child == above) {
    // Do nothing if we're raising a child above itself.
    return;
  }
  ActorVector::iterator iterator =
      find(children_.begin(), children_.end(), child);
  if (iterator == children_.end()) {
    LOG(WARNING) << "Attempted to raise a child (" << child
                 << ") that isn't a child of this container (" << this << ")";
    return;
  }
  if (above) {
    // Check and make sure |above| is an existing child.
    ActorVector::iterator iterator_above =
        find(children_.begin(), children_.end(), above);
    if (iterator_above == children_.end()) {
      LOG(WARNING) << "Attempted to raise a child (" << child
                   << ") above a sibling (" << above << ") that isn't "
                   << "a child of this container (" << this << ").";
      return;
    }
    CHECK(iterator_above != iterator);
    if (iterator_above > iterator) {
      children_.erase(iterator);
      // Find the above child again after erasing, because the old
      // iterator is invalid.
      iterator_above = find(children_.begin(), children_.end(), above);
    } else {
      children_.erase(iterator);
    }
    // Re-insert the child.
    children_.insert(iterator_above, child);
  } else {  // above is NULL, move child to top.
    children_.erase(iterator);
    children_.insert(children_.begin(), child);
  }
}

void RealCompositor::ContainerActor::LowerChild(
    RealCompositor::Actor* child, RealCompositor::Actor* below) {
  CHECK(child) << "Tried to lower a NULL child.";
  if (child == below) {
    // Do nothing if we're lowering a child below itself,
    // or it's NULL.
    return;
  }
  ActorVector::iterator iterator =
      find(children_.begin(), children_.end(), child);
  if (iterator == children_.end()) {
    LOG(WARNING) << "Attempted to lower a child (" << child
                 << ") that isn't a child of this container (" << this << ")";
    return;
  }
  if (below) {
    // Check and make sure |below| is an existing child.
    ActorVector::iterator iterator_below =
        find(children_.begin(), children_.end(), below);
    if (iterator_below == children_.end()) {
      LOG(WARNING) << "Attempted to lower a child (" << child
                   << ") below a sibling (" << below << ") that isn't "
                   << "a child of this container (" << this << ").";
      return;
    }
    CHECK(iterator_below != iterator);
    if (iterator_below > iterator) {
      children_.erase(iterator);
      // Find the below child again after erasing, because the old
      // iterator is invalid.
      iterator_below = find(children_.begin(), children_.end(), below);
    } else {
      children_.erase(iterator);
    }
    ++iterator_below;
    // Re-insert the child.
    children_.insert(iterator_below, child);
  } else {  // below is NULL, move child to bottom.
    children_.erase(iterator);
    children_.push_back(child);
  }
}


RealCompositor::QuadActor::QuadActor(RealCompositor* compositor)
    : RealCompositor::Actor(compositor),
      color_(1.f, 1.f, 1.f) {
}

RealCompositor::Actor* RealCompositor::QuadActor::Clone() {
  QuadActor* new_instance = new QuadActor(compositor());
  QuadActor::CloneImpl(new_instance);
  return static_cast<Actor*>(new_instance);
}

void RealCompositor::QuadActor::CloneImpl(QuadActor* clone) {
  Actor::CloneImpl(static_cast<RealCompositor::Actor*>(clone));
  clone->color_ = color_;
  clone->texture_data_ = texture_data_;
}

void RealCompositor::QuadActor::SetColorInternal(
    const Compositor::Color& color) {
  color_ = color;
  SetDirty();
}


RealCompositor::ColoredBoxActor::ColoredBoxActor(RealCompositor* compositor,
                                                 int width, int height,
                                                 const Compositor::Color& color)
    : QuadActor(compositor) {
  SetSizeInternal(width, height);
  SetColorInternal(color);
}


RealCompositor::ImageActor::ImageActor(RealCompositor* compositor)
    : QuadActor(compositor) {
  SetSizeInternal(0, 0);
}

bool RealCompositor::ImageActor::IsImageOpaque() const {
  DCHECK(texture_data());
  return !texture_data()->has_alpha();
}

RealCompositor::Actor* RealCompositor::ImageActor::Clone() {
  ImageActor* new_instance = new ImageActor(compositor());
  QuadActor::CloneImpl(new_instance);
  return static_cast<Actor*>(new_instance);
}

void RealCompositor::ImageActor::SetImageData(
    const ImageContainer& image_container) {
  compositor()->draw_visitor()->BindImage(&image_container, this);
  SetSizeInternal(image_container.width(), image_container.height());
  SetDirty();
}


RealCompositor::TexturePixmapActor::TexturePixmapActor(
    RealCompositor* compositor)
    : RealCompositor::QuadActor(compositor),
      pixmap_(0),
      pixmap_is_opaque_(false) {
  SetSizeInternal(0, 0);
}

RealCompositor::TexturePixmapActor::~TexturePixmapActor() {
  set_texture_data(NULL);
  pixmap_ = 0;
}

void RealCompositor::TexturePixmapActor::SetPixmap(XID pixmap) {
  set_texture_data(NULL);
  pixmap_ = pixmap;
  pixmap_is_opaque_ = false;

  if (pixmap_) {
    XConnection::WindowGeometry geometry;
    if (compositor()->x_conn()->GetWindowGeometry(pixmap_, &geometry)) {
      SetSizeInternal(geometry.bounds.width, geometry.bounds.height);
      pixmap_is_opaque_ = (geometry.depth != 32);
    } else {
      LOG(WARNING) << "Unable to get geometry for pixmap " << XidStr(pixmap_);
      pixmap_ = 0;
    }
  }

  if (!pixmap_)
    SetSizeInternal(0, 0);

  SetDirty();
}

void RealCompositor::TexturePixmapActor::UpdateTexture() {
  if (texture_data())
    texture_data()->Refresh();

  // Note that culled flag is one frame behind, but it is still valid for the
  // update here, because the stage will be set dirty if object is moving into
  // or out of view.
  if (is_shown() && !culled())
    compositor()->SetPartiallyDirty();
}


RealCompositor::StageActor::StageActor(RealCompositor* the_compositor,
                                       XWindow window,
                                       int width, int height)
    : RealCompositor::ContainerActor(the_compositor),
      window_(window),
      projection_(Matrix4::identity()),
      stage_color_changed_(true),
      was_resized_(true),
      stage_color_(0.f, 0.f, 0.f) {
  SetSizeInternal(width, height);
  SetDirty();
}

RealCompositor::StageActor::~StageActor() {
  compositor()->x_conn()->DestroyWindow(window_);
}

void RealCompositor::StageActor::SetSize(int width, int height) {
  // Have to resize the window to match the stage.
  CHECK(window_) << "Missing window in StageActor::SetSize()";
  SetSizeInternal(width, height);
  compositor()->x_conn()->ResizeWindow(window_, Size(width, height));
  was_resized_ = true;
}

void RealCompositor::StageActor::SetStageColor(const Compositor::Color& color) {
  stage_color_ = color;
  stage_color_changed_ = true;
}

void RealCompositor::StageActor::UpdateProjection() {
  // If this method is ever changed to use something besides an orthographic
  // pass-through projection matrix, update using_passthrough_projection()
  // accordingly.
  projection_ = Matrix4::orthographic(
                    0, width(), height(), 0, 
                    -kProjectedDepthMin, -kProjectedDepthMax);
}

bool RealCompositor::StageActor::using_passthrough_projection() const {
  return true;
}


RealCompositor::RealCompositor(EventLoop* event_loop,
                               XConnection* xconn,
#if defined(COMPOSITOR_OPENGL)
                               GLInterface* gl_interface
#elif defined(COMPOSITOR_OPENGLES)
                               Gles2Interface* gl_interface
#endif
                              )
    : event_loop_(event_loop),
      x_conn_(xconn),
      dirty_(true),
      partially_dirty_(false),
      num_animations_(0),
      actor_count_(0),
      draw_timeout_id_(-1),
      draw_timeout_enabled_(false),
      texture_pixmap_actor_uses_fast_path_(true),
      prev_top_fullscreen_actor_(NULL) {
  CHECK(event_loop_);
  XWindow root = x_conn()->GetRootWindow();
  XConnection::WindowGeometry geometry;
  x_conn()->GetWindowGeometry(root, &geometry);
  XVisualID visual_id = 0;
#if defined(COMPOSITOR_OPENGL)
  visual_id = gl_interface->GetVisual();
#endif
  XWindow window = x_conn()->CreateWindow(
      root, geometry.bounds, false, false, 0, visual_id);
  x_conn()->MapWindow(window);
  default_stage_.reset(
      new RealCompositor::StageActor(this,
                                     window,
                                     geometry.bounds.width,
                                     geometry.bounds.height));

  draw_visitor_.reset(
#if defined(COMPOSITOR_OPENGL)
      new OpenGlDrawVisitor(gl_interface, this, default_stage_.get())
#elif defined(COMPOSITOR_OPENGLES)
      new OpenGlesDrawVisitor(gl_interface, this, default_stage_.get())
#endif
      );

#if defined(COMPOSITOR_OPENGL)
  if (!gl_interface->HasTextureFromPixmapExtension())
    texture_pixmap_actor_uses_fast_path_ = false;
#endif

  draw_timeout_id_ = event_loop_->AddTimeout(
      NewPermanentCallback(this, &RealCompositor::Draw), 0,
      FLAGS_draw_timeout_ms);
  draw_timeout_enabled_ = true;
}


RealCompositor::~RealCompositor() {
  draw_visitor_.reset();
  if (draw_timeout_id_ >= 0) {
    event_loop_->RemoveTimeout(draw_timeout_id_);
    draw_timeout_id_ = -1;
  }
}

void RealCompositor::RegisterCompositionChangeListener(
      CompositionChangeListener* listener) {
  DCHECK(listener);
  bool added = composition_change_listeners_.insert(listener).second;
  DCHECK(added) << "Listener " << listener << " was already registered";
}

void RealCompositor::UnregisterCompositionChangeListener(
      CompositionChangeListener* listener) {
  int num_removed = composition_change_listeners_.erase(listener);
  DCHECK_EQ(num_removed, 1) << "Listener " << listener << " wasn't registered";
}

RealCompositor::ContainerActor* RealCompositor::CreateGroup() {
  return new ContainerActor(this);
}

RealCompositor::ColoredBoxActor* RealCompositor::CreateColoredBox(
    int width, int height, const Compositor::Color& color) {
  return new ColoredBoxActor(this, width, height, color);
}

RealCompositor::ImageActor* RealCompositor::CreateImage() {
  return new ImageActor(this);
}

RealCompositor::ImageActor* RealCompositor::CreateImageFromFile(
    const string& filename) {
  ImageActor* actor = CreateImage();
  scoped_ptr<ImageContainer> container(
      ImageContainer::CreateContainerFromFile(filename));
  CHECK(container.get() &&
        container->LoadImage() == ImageContainer::IMAGE_LOAD_SUCCESS);
  actor->SetImageData(*(container.get()));
  return actor;
}

RealCompositor::TexturePixmapActor* RealCompositor::CreateTexturePixmap() {
  return new TexturePixmapActor(this);
}

RealCompositor::Actor* RealCompositor::CloneActor(Compositor::Actor* orig) {
  RealCompositor::Actor* actor = dynamic_cast<RealCompositor::Actor*>(orig);
  CHECK(actor);
  return actor->Clone();
}

void RealCompositor::SetActiveVisibilityGroups(
    const unordered_set<int>& groups) {
  if (groups.empty() && active_visibility_groups_.empty())
    return;

  active_visibility_groups_ = groups;
  SetDirty();
}

void RealCompositor::RemoveActor(Actor* actor) {
  ActorVector::iterator iterator = find(actors_.begin(), actors_.end(), actor);
  if (iterator != actors_.end()) {
    actors_.erase(iterator);
  }
}

void RealCompositor::SetDirty() {
  if (!dirty_ && !partially_dirty_)
    EnableDrawTimeout();
  dirty_ = true;
}

void RealCompositor::SetPartiallyDirty() {
  if (dirty_ || partially_dirty_)
    return;
  EnableDrawTimeout();
  partially_dirty_ = true;
}

void RealCompositor::UpdateTopFullscreenActor(
    const RealCompositor::TexturePixmapActor* top_fullscreen_actor) {
  // The top fullscreen actor has not changed from previous frame to
  // current frame, no need to notify the listeners.
  if (prev_top_fullscreen_actor_ == top_fullscreen_actor)
    return;
  prev_top_fullscreen_actor_ = top_fullscreen_actor;

  for (unordered_set<CompositionChangeListener*>::const_iterator it =
         composition_change_listeners_.begin();
       it != composition_change_listeners_.end(); ++it)
    (*it)->HandleTopFullscreenActorChange(top_fullscreen_actor);
}

void RealCompositor::IncrementNumAnimations() {
  num_animations_++;
  if (num_animations_ == 1)
    EnableDrawTimeout();
}

void RealCompositor::DecrementNumAnimations() {
  num_animations_--;
  DCHECK_GE(num_animations_, 0) << "Decrementing animation count below zero";
}

void RealCompositor::Draw() {
  PROFILER_MARKER_BEGIN(RealCompositor_Draw);
  TimeTicks now = GetMonotonicTime();
  if (num_animations_ > 0 || dirty_) {
    actor_count_ = 0;
    PROFILER_MARKER_BEGIN(RealCompositor_Draw_Update);
    default_stage_->Update(&actor_count_, now);
    PROFILER_MARKER_END(RealCompositor_Draw_Update);
  }
  if (dirty_ || partially_dirty_) {
    last_draw_time_ = now;

    const bool use_partial_updates = !dirty_ && partially_dirty_;
    LayerVisitor layer_visitor(actor_count(), use_partial_updates);
    default_stage_->Accept(&layer_visitor);
    UpdateTopFullscreenActor(layer_visitor.top_fullscreen_actor());
    Rect damaged_region = layer_visitor.GetDamagedRegion(
        default_stage_->width(), default_stage_->height());

    // It is possible to receive partially_dirty_ notifications for actors
    // that are covered or offscreen, in which case, the damaged_region_
    // will be empty, and we can only check that after the LayerVisitor has
    // traversed through the tree.
    if ((!use_partial_updates || !damaged_region.empty()) &&
        should_draw_frame()) {
      PROFILER_MARKER_BEGIN(RealCompositor_Draw_Render);
      draw_visitor_->set_damaged_region(damaged_region);
      draw_visitor_->set_has_fullscreen_actor(
          layer_visitor.has_fullscreen_actor());
      default_stage_->Accept(draw_visitor_.get());
      PROFILER_MARKER_END(RealCompositor_Draw_Render);
    }
    dirty_ = false;
    partially_dirty_ = false;
  }
  if (num_animations_ == 0)
    DisableDrawTimeout();
  PROFILER_MARKER_END(RealCompositor_Draw);
}

void RealCompositor::EnableDrawTimeout() {
  if (!draw_timeout_enabled_) {
    TimeDelta time_since_draw =
        !last_draw_time_.is_null() ?
            GetMonotonicTime() - last_draw_time_ :
            TimeDelta();
    int ms_until_draw =
        max(FLAGS_draw_timeout_ms - time_since_draw.InMilliseconds(), 0LL);
    event_loop_->ResetTimeout(draw_timeout_id_, ms_until_draw,
                              FLAGS_draw_timeout_ms);
    draw_timeout_enabled_ = true;
  }
}

void RealCompositor::DisableDrawTimeout() {
  if (draw_timeout_enabled_) {
    event_loop_->SuspendTimeout(draw_timeout_id_);
    draw_timeout_enabled_ = false;
  }
}

}  // namespace window_manager
