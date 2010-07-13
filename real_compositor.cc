// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor.h"

#include <algorithm>
#include <ctime>
#include <string>

#include <gflags/gflags.h>
#include <sys/time.h>

#include "base/logging.h"
#include "base/string_util.h"
#include "window_manager/callback.h"
#include "window_manager/event_loop.h"
#include "window_manager/image_container.h"
#if defined(COMPOSITOR_OPENGL)
#include "window_manager/opengl_visitor.h"
#elif defined(COMPOSITOR_OPENGLES)
#include "window_manager/gles/opengles_visitor.h"
#endif
#include "window_manager/profiler.h"
#include "window_manager/util.h"
#include "window_manager/x_connection.h"

DEFINE_bool(compositor_display_debug_needle, false,
            "Specify this to turn on a debugging aid for seeing when "
            "frames are being drawn.");

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
using window_manager::util::NextPowerOfTwo;
using window_manager::util::XidStr;

// Turn this on if you want to debug the visitor traversal.
#undef EXTRA_LOGGING

namespace window_manager {

enum CullingResult {
  CULLING_WINDOW_OFFSCREEN,
  CULLING_WINDOW_ONSCREEN,
  CULLING_WINDOW_FULLSCREEN
};

// Struct used for visibility and culling tests.
// The X and Y axes are the same as in OpenGL. Where positive X is right, and
// positive Y is top. Both corners are exclusive, so two bounding boxes do not
// intersect if their sides overlap.
struct BoundingBox {
  float top_left_x, top_left_y;
  float bottom_right_x, bottom_right_y;
};

const float kMaxDimmedOpacity = 0.6f;

const float RealCompositor::LayerVisitor::kMinDepth = 0.0f;
const float RealCompositor::LayerVisitor::kMaxDepth =
    4096.0f + RealCompositor::LayerVisitor::kMinDepth;

// Minimum amount of time in milliseconds between scene redraws.
static const int64_t kDrawTimeoutMs = 16;

static inline bool IsBoxOnScreen(const BoundingBox& a) {
  // The window has corners top left (-1, 1) and bottom right (1, -1).
  return !(a.bottom_right_x <= -1.0 || a.top_left_x >= 1.0 ||
           a.top_left_y <= -1.0 || a.bottom_right_y >= 1.0);
}

static inline bool IsBoxFullScreen(const BoundingBox& a) {
  // The bounding box must be equal or greater than the area (-1, 1) - (1, -1)
  // in case of full screen.
  return a.bottom_right_x >= 1.0 && a.top_left_x <= -1.0 &&
         a.top_left_y >= 1.0 && a.bottom_right_y <= -1.0;
}

static inline float min4(float a, float b, float c, float d) {
  return min(min(min(a, b), c), d);
}

static inline float max4(float a, float b, float c, float d) {
  return max(max(max(a, b), c), d);
}

static CullingResult PerformActorCullingTest(
    RealCompositor::StageActor* stage, RealCompositor::QuadActor* actor) {
  static const Vector4 bottom_left(0, 0, 0, 1);
  static const Vector4 top_left(0, 1, 0, 1);
  static const Vector4 top_right(1, 1, 0, 1);
  static const Vector4 bottom_right(1, 0, 0, 1);

  const Matrix4& transform = stage->projection() * actor->model_view();

  const Vector4 tl = transform * top_left;
  const Vector4 tr = transform * top_right;
  const Vector4 bl = transform * bottom_left;
  const Vector4 br = transform * bottom_right;

  BoundingBox box = { min4(tl[0], tr[0], bl[0], br[0]),   // top left x
                      max4(tl[1], tr[1], bl[1], br[1]),   // top left y
                      max4(tl[0], tr[0], bl[0], br[0]),   // bottom right x
                      min4(tl[1], tr[1], bl[1], br[1]) }; // bottom right y

  if (!IsBoxOnScreen(box))
    return CULLING_WINDOW_OFFSCREEN;

  if (IsBoxFullScreen(box))
    return CULLING_WINDOW_FULLSCREEN;

  return CULLING_WINDOW_ONSCREEN;
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

void RealCompositor::LayerVisitor::VisitActor(RealCompositor::Actor* actor) {
  actor->set_z(depth_);
  depth_ += layer_thickness_;
  actor->set_is_opaque(actor->opacity() > 0.999f);
}

void RealCompositor::LayerVisitor::VisitStage(
    RealCompositor::StageActor* actor) {
  // This calculates the next power of two for the actor count, so
  // that we can avoid roundoff errors when computing the depth.
  // Also, add two empty layers at the front and the back that we
  // won't use in order to avoid issues at the extremes.  The eventual
  // plan here is to have three depth ranges, one in the front that is
  // 4096 deep, one in the back that is 4096 deep, and the remaining
  // in the middle for drawing 3D UI elements.  Currently, this code
  // represents just the front layer range.  Note that the number of
  // layers is NOT limited to 4096 (this is an arbitrary value that is
  // a power of two) -- the maximum number of layers depends on the
  // number of actors and the bit-depth of the hardware's z-buffer.
  uint32 count = NextPowerOfTwo(static_cast<uint32>(count_ + 2));
  layer_thickness_ = (kMaxDepth - kMinDepth) / count;

  // Don't start at the very edge of the z-buffer depth.
  depth_ = kMinDepth + layer_thickness_;

  has_fullscreen_actor_ = false;

  actor->UpdateProjection();
  VisitContainer(actor);
}

void RealCompositor::LayerVisitor::VisitContainer(
    RealCompositor::ContainerActor* actor) {
  CHECK(actor);
  if (!actor->IsVisible())
    return;

  // No culling test for ContainerActor because the container does not bound
  // its children actors.  No need to set_z first because container doesn't
  // use z in its model view matrix.
  actor->UpdateModelView();

  ActorVector children = actor->GetChildren();
  RealCompositor::ActorVector::const_iterator iterator = children.begin();
  while (iterator != children.end()) {
    if (*iterator) {
      (*iterator)->Accept(this);
    }
    ++iterator;
  }

  // The containers should be "closer" than all their children.
  this->VisitActor(actor);
}

void RealCompositor::LayerVisitor::VisitTexturedQuadActor(
    RealCompositor::QuadActor* actor, bool is_texture_opaque) {
  // Reset culled_ state so that IsVisible will not use the state from
  // previous frame.
  actor->set_culled(false);
  if (!actor->IsVisible())
    return;

  if (has_fullscreen_actor_) {
    actor->set_culled(true);
    return;
  }

  VisitActor(actor);
  actor->set_is_opaque(actor->is_opaque() && is_texture_opaque);

  // Must update model view matrix before culling test.
  actor->UpdateModelView();
  CullingResult result =
      PerformActorCullingTest(actor->compositor()->GetDefaultStage(), actor);

  if (actor->is_opaque() && result == CULLING_WINDOW_FULLSCREEN)
    has_fullscreen_actor_ = true;

  actor->set_culled(result == CULLING_WINDOW_OFFSCREEN);
}

void RealCompositor::LayerVisitor::VisitQuad(
    RealCompositor::QuadActor* actor) {
  DCHECK(actor->texture_data() == NULL);
  VisitTexturedQuadActor(actor, true);
}

void RealCompositor::LayerVisitor::VisitImage(
    RealCompositor::ImageActor* actor) {
  VisitTexturedQuadActor(
      actor,
      actor->texture_data() ? !actor->texture_data()->has_alpha() : true);
}

void RealCompositor::LayerVisitor::VisitTexturePixmap(
    RealCompositor::TexturePixmapActor* actor) {
  // OpenGlPixmapData is not created until OpenGlDrawVisitor has traversed
  // through the tree, which happens after the LayerVisitor, so we cannot rely
  // on actor->texture_data()->has_alpha() because texture_data() is NULL
  // in the beginning.
  // TODO: combine VisitQuad and VisitTexturePixmap.
  VisitTexturedQuadActor(actor, actor->pixmap_is_opaque());
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
      dimmed_opacity_(0.f) {
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
  AnimateField(&int_animations_, &x_, x, duration_ms);
}

void RealCompositor::Actor::MoveY(int y, int duration_ms) {
  AnimateField(&int_animations_, &y_, y, duration_ms);
}

void RealCompositor::Actor::Scale(double scale_x, double scale_y,
                                 int duration_ms) {
  AnimateField(&float_animations_, &scale_x_, static_cast<float>(scale_x),
               duration_ms);
  AnimateField(&float_animations_, &scale_y_, static_cast<float>(scale_y),
               duration_ms);
}

void RealCompositor::Actor::SetOpacity(double opacity, int duration_ms) {
  AnimateField(&float_animations_, &opacity_, static_cast<float>(opacity),
               duration_ms);
}

void RealCompositor::Actor::SetTilt(double tilt, int duration_ms) {
  AnimateField(&float_animations_, &tilt_,
               static_cast<float>(tilt),
               duration_ms);
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
  AnimateField(&float_animations_, &dimmed_opacity_,
               dimmed ? kMaxDimmedOpacity : 0.f, anim_ms);
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

void RealCompositor::Actor::Update(int* count, AnimationTime now) {
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

template<class T> void RealCompositor::Actor::AnimateField(
    map<T*, shared_ptr<Animation<T> > >* animation_map,
    T* field, T value, int duration_ms) {
  typeof(animation_map->begin()) iterator = animation_map->find(field);
  // If we're not currently animating the field and it's already at the
  // right value, there's no reason to do anything.
  if (iterator == animation_map->end() && value == *field)
    return;

  if (duration_ms > 0) {
    AnimationTime now = compositor_->GetCurrentTimeMs();
    if (iterator != animation_map->end()) {
      Animation<T>* animation = iterator->second.get();
      animation->Reset(value, now, now + duration_ms);
    } else {
      shared_ptr<Animation<T> > animation(
          new Animation<T>(field, value, now, now + duration_ms));
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

template<class T> void RealCompositor::Actor::UpdateInternal(
    map<T*, shared_ptr<Animation<T> > >* animation_map,
    AnimationTime now) {
  typeof(animation_map->begin()) iterator = animation_map->begin();
  while (iterator != animation_map->end()) {
    bool done = iterator->second->Eval(now);
    if (done) {
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

void RealCompositor::ContainerActor::Update(int* count, AnimationTime now) {
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
    // Check and make sure 'above' is an existing child.
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
    // Check and make sure 'below' is an existing child.
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
      color_(1.f, 1.f, 1.f),
      border_color_(1.f, 1.f, 1.f),
      border_width_(0) {
}

RealCompositor::Actor* RealCompositor::QuadActor::Clone() {
  QuadActor* new_instance = new QuadActor(compositor());
  QuadActor::CloneImpl(new_instance);
  return static_cast<Actor*>(new_instance);
}

void RealCompositor::QuadActor::CloneImpl(QuadActor* clone) {
  Actor::CloneImpl(static_cast<RealCompositor::Actor*>(clone));
  clone->SetColor(color_, border_color_, border_width_);
  clone->texture_data_ = texture_data_;
}


RealCompositor::ImageActor::ImageActor(RealCompositor* compositor)
    : QuadActor(compositor) {
  Actor::SetSize(0, 0);
}

RealCompositor::Actor* RealCompositor::ImageActor::Clone() {
  ImageActor* new_instance = new ImageActor(compositor());
  QuadActor::CloneImpl(new_instance);
  return static_cast<Actor*>(new_instance);
}

void RealCompositor::ImageActor::SetImageData(
    const ImageContainer& image_container) {
  compositor()->draw_visitor()->BindImage(&image_container, this);
  Actor::SetSize(image_container.width(), image_container.height());
  SetDirty();
}


RealCompositor::TexturePixmapActor::TexturePixmapActor(
    RealCompositor* compositor)
    : RealCompositor::QuadActor(compositor),
      pixmap_(0),
      pixmap_is_opaque_(false) {
  Actor::SetSize(0, 0);
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
      Actor::SetSize(geometry.width, geometry.height);
      pixmap_is_opaque_ = (geometry.depth != 32);
    } else {
      LOG(WARNING) << "Unable to get geometry for pixmap " << XidStr(pixmap_);
      pixmap_ = 0;
    }
  }

  if (!pixmap_)
    Actor::SetSize(0, 0);

  SetDirty();
}

void RealCompositor::TexturePixmapActor::UpdateTexture() {
  if (texture_data())
    texture_data()->Refresh();

  // Note that culled flag is one frame behind, but it is still valid for the
  // update here, because the stage will be set dirty if object is moving into
  // or out of view.
  if (is_shown() && !culled())
    SetDirty();
}


RealCompositor::StageActor::StageActor(RealCompositor* the_compositor,
                                       int width, int height)
    : RealCompositor::ContainerActor(the_compositor),
      window_(0),
      projection_(Matrix4::identity()),
      stage_color_changed_(true),
      was_resized_(true),
      stage_color_(0.f, 0.f, 0.f) {
  window_ = compositor()->x_conn()->CreateSimpleWindow(
      compositor()->x_conn()->GetRootWindow(),
      0, 0, width, height);
  compositor()->x_conn()->MapWindow(window_);
  Actor::SetSize(width, height);
  SetDirty();
}

RealCompositor::StageActor::~StageActor() {
  compositor()->x_conn()->DestroyWindow(window_);
}

void RealCompositor::StageActor::SetSize(int width, int height) {
  // Have to resize the window to match the stage.
  CHECK(window_) << "Missing window in StageActor::SetSize()";
  Actor::SetSize(width, height);
  compositor()->x_conn()->ResizeWindow(window_, width, height);
  was_resized_ = true;
}

void RealCompositor::StageActor::SetStageColor(const Compositor::Color& color) {
  stage_color_ = color;
  stage_color_changed_ = true;
}

void RealCompositor::StageActor::UpdateProjection() {
  projection_ = Matrix4::orthographic(
                    0, width(), height(), 0,
                    -RealCompositor::LayerVisitor::kMinDepth,
                    -RealCompositor::LayerVisitor::kMaxDepth);
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
      num_animations_(0),
      actor_count_(0),
      current_time_ms_for_testing_(-1),
      last_draw_time_ms_(-1),
      draw_timeout_id_(-1),
      draw_timeout_enabled_(false),
      texture_pixmap_actor_uses_fast_path_(true) {
  CHECK(event_loop_);
  XWindow root = x_conn()->GetRootWindow();
  XConnection::WindowGeometry geometry;
  x_conn()->GetWindowGeometry(root, &geometry);
  default_stage_.reset(new RealCompositor::StageActor(this,
                                                      geometry.width,
                                                      geometry.height));

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
      NewPermanentCallback(this, &RealCompositor::Draw), 0, kDrawTimeoutMs);
  draw_timeout_enabled_ = true;
}


RealCompositor::~RealCompositor() {
  draw_visitor_.reset();
  if (draw_timeout_id_ >= 0) {
    event_loop_->RemoveTimeout(draw_timeout_id_);
    draw_timeout_id_ = -1;
  }
}

RealCompositor::ContainerActor* RealCompositor::CreateGroup() {
  return new ContainerActor(this);
}

RealCompositor::Actor* RealCompositor::CreateRectangle(
    const Compositor::Color& color,
    const Compositor::Color& border_color,
    int border_width) {
  QuadActor* actor = new QuadActor(this);
  actor->SetColor(color, border_color, border_width);
  return actor;
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

RealCompositor::AnimationTime RealCompositor::GetCurrentTimeMs() {
  if (current_time_ms_for_testing_ >= 0)
    return current_time_ms_for_testing_;

  return util::GetCurrentTimeMs();
}

void RealCompositor::SetDirty() {
  if (!dirty_)
    EnableDrawTimeout();
  dirty_ = true;
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
  int64_t now = GetCurrentTimeMs();
  if (num_animations_ > 0 || dirty_) {
    actor_count_ = 0;
    PROFILER_MARKER_BEGIN(RealCompositor_Draw_Update);
    default_stage_->Update(&actor_count_, now);
    PROFILER_MARKER_END(RealCompositor_Draw_Update);
  }
  if (dirty_) {
    last_draw_time_ms_ = now;
    PROFILER_MARKER_BEGIN(RealCompositor_Draw_Render);
    default_stage_->Accept(draw_visitor_.get());
    PROFILER_MARKER_END(RealCompositor_Draw_Render);
    dirty_ = false;
  }
  if (num_animations_ == 0)
    DisableDrawTimeout();
  PROFILER_MARKER_END(RealCompositor_Draw);
}

void RealCompositor::EnableDrawTimeout() {
  if (!draw_timeout_enabled_) {
    int64_t ms_since_draw = max(GetCurrentTimeMs() - last_draw_time_ms_,
                                static_cast<int64_t>(0));
    int ms_until_draw = kDrawTimeoutMs - min(ms_since_draw, kDrawTimeoutMs);
    event_loop_->ResetTimeout(draw_timeout_id_, ms_until_draw, kDrawTimeoutMs);
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
