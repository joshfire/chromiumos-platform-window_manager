// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/tidy_interface.h"

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
using std::string;
using std::tr1::shared_ptr;

// Turn this on if you want to debug the visitor traversal.
#undef EXTRA_LOGGING

namespace window_manager {

const float kMaxDimmedOpacity = 0.6f;

const float RealCompositor::LayerVisitor::kMinDepth = 0.0f;
const float RealCompositor::LayerVisitor::kMaxDepth =
    4096.0f + RealCompositor::LayerVisitor::kMinDepth;

// Minimum amount of time in milliseconds between scene redraws.
static const int64_t kDrawTimeoutMs = 16;

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

void RealCompositor::LayerVisitor::VisitQuad(RealCompositor::QuadActor* actor) {
  // Do all the regular actor stuff.
  this->VisitActor(actor);

#if defined(COMPOSITOR_OPENGL)
  OpenGlTextureData* data  = static_cast<OpenGlTextureData*>(
      actor->GetDrawingData(OpenGlDrawVisitor::TEXTURE_DATA).get());
  if (data) {
    actor->set_is_opaque(actor->is_opaque() && !data->has_alpha());
  }
#endif
}

void RealCompositor::LayerVisitor::VisitTexturePixmap(
    RealCompositor::TexturePixmapActor* actor) {
  // Do all the regular Quad stuff.
  this->VisitQuad(actor);

#if defined(COMPOSITOR_OPENGL)
  OpenGlPixmapData* data  = static_cast<OpenGlPixmapData*>(
      actor->GetDrawingData(OpenGlDrawVisitor::PIXMAP_DATA).get());
  if (data) {
    actor->set_is_opaque(actor->is_opaque() && !data->has_alpha());
  } else {
    // If there is no pixmap data yet for a texture pixmap, let's
    // assume it'll be transparent so that the transparent bits don't
    // flash opaque on the first pass.
    actor->set_is_opaque(false);
  }
#endif
}

void RealCompositor::LayerVisitor::VisitActor(RealCompositor::Actor* actor) {
  actor->set_z(depth_);
  depth_ += layer_thickness_;
  actor->set_is_opaque(actor->opacity() > 0.999f);
}

void RealCompositor::LayerVisitor::VisitContainer(
    RealCompositor::ContainerActor* actor) {
  CHECK(actor);
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

  VisitContainer(actor);
}

RealCompositor::Actor::~Actor() {
  if (parent_) {
    parent_->RemoveActor(this);
  }
  compositor_->RemoveActor(this);
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
      is_opaque_(false),
      has_children_(false),
      visible_(true),
      dimmed_opacity_(0.f) {
  compositor_->AddActor(this);
}

RealCompositor::Actor* RealCompositor::Actor::Clone() {
  Actor* new_instance = new Actor(compositor_);
  CloneImpl(new_instance);
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
  clone->visible_ = visible_;
  clone->name_ = name_;

  // This copies all the drawing data, but they're all tr1::shared_ptr's,
  // so it all works out great.
  clone->drawing_data_ = drawing_data_;
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
  CHECK(parent_) << "Tried to raise an actor that has no parent.";
  RealCompositor::Actor* other_nc =
      dynamic_cast<RealCompositor::Actor*>(other);
  CHECK(other_nc) << "Failed to cast to an Actor in Raise";
  parent_->RaiseChild(this, other_nc);
  SetDirty();
}

void RealCompositor::Actor::Lower(Compositor::Actor* other) {
  CHECK(parent_) << "Tried to lower an actor that has no parent.";
  RealCompositor::Actor* other_nc =
      dynamic_cast<RealCompositor::Actor*>(other);
  CHECK(other_nc) << "Failed to cast to an Actor in Lower";
  parent_->LowerChild(this, other_nc);
  SetDirty();
}

void RealCompositor::Actor::RaiseToTop() {
  CHECK(parent_) << "Tried to raise an actor to top that has no parent.";
  parent_->RaiseChild(this, NULL);
  SetDirty();
}

void RealCompositor::Actor::LowerToBottom() {
  CHECK(parent_) << "Tried to lower an actor to bottom that has no parent.";
  parent_->LowerChild(this, NULL);
  SetDirty();
}

string RealCompositor::Actor::GetDebugStringInternal(const string& type_name,
                                                    int indent_level) {
  string out;
  for (int i = 0; i < indent_level; ++i)
    out += "  ";

  out += StringPrintf("\"%s\" %p (%s%s) (%d, %d) %dx%d "
                        "scale=(%.2f, %.2f) %.2f%% tilt=%0.2f\n",
                      !name_.empty() ? name_.c_str() : "",
                      this,
                      visible_ ? "" : "inv ",
                      type_name.c_str(),
                      x_, y_,
                      width_, height_,
                      scale_x_, scale_y_,
                      opacity_,
                      tilt_);
  return out;
}

RealCompositor::DrawingDataPtr RealCompositor::Actor::GetDrawingData(
    int32 id) const {
  DrawingDataMap::const_iterator iterator = drawing_data_.find(id);
  if (iterator != drawing_data_.end()) {
    return (*iterator).second;
  }
  return DrawingDataPtr();
}

void RealCompositor::Actor::Update(int* count, AnimationTime now) {
  (*count)++;
  if (int_animations_.empty() && float_animations_.empty())
    return;

  SetDirty();
  UpdateInternal(&int_animations_, now);
  UpdateInternal(&float_animations_, now);
}

void RealCompositor::Actor::ShowDimmed(bool dimmed, int anim_ms) {
  AnimateField(&float_animations_, &dimmed_opacity_,
               dimmed ? kMaxDimmedOpacity : 0.f, anim_ms);
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
    dynamic_cast<RealCompositor::Actor*>(*iterator)->set_parent(NULL);
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
    dynamic_cast<RealCompositor::Actor*>(*iterator)->Update(count, now);
  }
  RealCompositor::Actor::Update(count, now);
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
  CloneImpl(new_instance);
  return static_cast<Actor*>(new_instance);
}

void RealCompositor::QuadActor::CloneImpl(QuadActor* clone) {
  Actor::CloneImpl(static_cast<RealCompositor::Actor*>(clone));
  clone->SetColor(color_, border_color_, border_width_);
}


RealCompositor::TexturePixmapActor::TexturePixmapActor(
    RealCompositor* compositor)
    : RealCompositor::QuadActor(compositor),
      window_(0),
      pixmap_invalid_(true) {
}

void RealCompositor::TexturePixmapActor::SetSizeImpl(int* width, int* height) {
  DestroyPixmap();
  SetDirty();
  set_pixmap_invalid(true);
}

bool RealCompositor::TexturePixmapActor::SetTexturePixmapWindow(
    XWindow xid) {
  Reset();
  window_ = xid;
  compositor()->StartMonitoringWindowForChanges(window_, this);
  SetDirty();
  return true;
}

void RealCompositor::TexturePixmapActor::Reset() {
  if (window_)
    compositor()->StopMonitoringWindowForChanges(window_, this);
  window_ = None;
  DestroyPixmap();
  SetDirty();
}

void RealCompositor::TexturePixmapActor::DestroyPixmap() {
#if defined(COMPOSITOR_OPENGL)
  EraseDrawingData(OpenGlDrawVisitor::PIXMAP_DATA);
#elif defined(COMPOSITOR_OPENGLES)
  EraseDrawingData(OpenGlesDrawVisitor::kEglImageData);
#endif
}

RealCompositor::Actor* RealCompositor::TexturePixmapActor::Clone() {
  TexturePixmapActor* new_instance = new TexturePixmapActor(compositor());
  CloneImpl(new_instance);
  return static_cast<Actor*>(new_instance);
}

void RealCompositor::TexturePixmapActor::CloneImpl(TexturePixmapActor* clone) {
  QuadActor::CloneImpl(static_cast<RealCompositor::QuadActor*>(clone));
  clone->window_ = window_;
}

bool RealCompositor::TexturePixmapActor::HasPixmapDrawingData() {
#if defined(COMPOSITOR_OPENGL)
  return GetDrawingData(OpenGlDrawVisitor::PIXMAP_DATA) != NULL;
#elif defined(COMPOSITOR_OPENGLES)
  return GetDrawingData(OpenGlesDrawVisitor::kEglImageData) != NULL;
#endif
}

void RealCompositor::TexturePixmapActor::RefreshPixmap() {
  // TODO: Lift common damage and pixmap creation code to RealCompositor
#if defined(COMPOSITOR_OPENGL)
  OpenGlPixmapData* data  = dynamic_cast<OpenGlPixmapData*>(
      GetDrawingData(OpenGlDrawVisitor::PIXMAP_DATA).get());
  if (data)
    data->Refresh();
#elif defined(COMPOSITOR_OPENGLES)
  OpenGlesEglImageData* data = dynamic_cast<OpenGlesEglImageData*>(
      GetDrawingData(OpenGlesDrawVisitor::kEglImageData).get());
  if (data)
    data->Refresh();
#endif
  SetDirty();
}

RealCompositor::StageActor::StageActor(RealCompositor* the_compositor,
                                       int width, int height)
    : RealCompositor::ContainerActor(the_compositor),
      window_(0),
      was_resized_(true),
      stage_color_(1.f, 1.f, 1.f) {
  window_ = compositor()->x_conn()->CreateSimpleWindow(
      compositor()->x_conn()->GetRootWindow(),
      0, 0, width, height);
  compositor()->x_conn()->MapWindow(window_);
  SetDirty();
}

RealCompositor::StageActor::~StageActor() {
  compositor()->x_conn()->DestroyWindow(window_);
}

void RealCompositor::StageActor::SetStageColor(const Compositor::Color& color) {
  stage_color_ = color;
}

void RealCompositor::StageActor::SetSizeImpl(int* width, int* height) {
  // Have to resize the window to match the stage.
  CHECK(window_) << "Missing window in StageActor::SetSizeImpl.";
  compositor()->x_conn()->ResizeWindow(window_, *width, *height);
  was_resized_ = true;
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
      draw_timeout_enabled_(false) {
  CHECK(event_loop_);
  XWindow root = x_conn()->GetRootWindow();
  XConnection::WindowGeometry geometry;
  x_conn()->GetWindowGeometry(root, &geometry);
  default_stage_.reset(new RealCompositor::StageActor(this,
                                                     geometry.width,
                                                     geometry.height));

#if defined(COMPOSITOR_OPENGL)
  draw_visitor_ =
      new OpenGlDrawVisitor(gl_interface, this, default_stage_.get());
#elif defined(COMPOSITOR_OPENGLES)
  draw_visitor_ =
      new OpenGlesDrawVisitor(gl_interface, this, default_stage_.get());
#endif

  draw_timeout_id_ = event_loop_->AddTimeout(
      NewPermanentCallback(this, &RealCompositor::Draw), 0, kDrawTimeoutMs);
  draw_timeout_enabled_ = true;
}

RealCompositor::~RealCompositor() {
  delete draw_visitor_;
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

RealCompositor::Actor* RealCompositor::CreateImage(const string& filename) {
  QuadActor* actor = new QuadActor(this);
  scoped_ptr<ImageContainer> container(
      ImageContainer::CreateContainer(filename));
  if (container.get() &&
      container->LoadImage() == ImageContainer::IMAGE_LOAD_SUCCESS) {
    draw_visitor_->BindImage(container.get(), actor);
    actor->SetSize(container->width(), container->height());
  } else {
    Compositor::Color color(1.f, 0.f, 1.f);
    actor->SetColor(color, color, 0);
  }

  return actor;
}

RealCompositor::TexturePixmapActor* RealCompositor::CreateTexturePixmap() {
  return new TexturePixmapActor(this);
}

RealCompositor::Actor* RealCompositor::CreateText(
    const string& font_name,
    const string& text,
    const Compositor::Color& color) {
  QuadActor* actor = new QuadActor(this);
  // TODO: Actually create the text.
  actor->SetColor(color, color, 0);
  actor->SetOpacity(.5f, 0);
  return actor;
}

RealCompositor::Actor* RealCompositor::CloneActor(Compositor::Actor* orig) {
  RealCompositor::Actor* actor = dynamic_cast<RealCompositor::Actor*>(orig);
  CHECK(actor);
  return actor->Clone();
}

void RealCompositor::HandleWindowDamaged(XWindow xid) {
  TexturePixmapActor* actor =
      FindWithDefault(texture_pixmaps_,
                      xid,
                      static_cast<TexturePixmapActor*>(NULL));
  if (actor)
    actor->RefreshPixmap();
}

void RealCompositor::RemoveActor(Actor* actor) {
  ActorVector::iterator iterator = find(actors_.begin(), actors_.end(), actor);
  if (iterator != actors_.end()) {
    actors_.erase(iterator);
  }
}

void RealCompositor::StartMonitoringWindowForChanges(
    XWindow xid, TexturePixmapActor* actor) {
  texture_pixmaps_[xid] = actor;
}

void RealCompositor::StopMonitoringWindowForChanges(
    XWindow xid, TexturePixmapActor* actor) {
  texture_pixmaps_.erase(xid);
}

RealCompositor::AnimationTime RealCompositor::GetCurrentTimeMs() {
  if (current_time_ms_for_testing_ >= 0)
    return current_time_ms_for_testing_;

  struct timeval tv;
  gettimeofday(&tv, NULL);
  return 1000ULL * tv.tv_sec + tv.tv_usec / 1000ULL;
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
  int64_t now = GetCurrentTimeMs();
  if (num_animations_ > 0 || dirty_) {
    actor_count_ = 0;
    default_stage_->Update(&actor_count_, now);
  }
  if (dirty_) {
    last_draw_time_ms_ = now;
    default_stage_->Accept(draw_visitor_);
    dirty_ = false;
  }
  if (num_animations_ == 0)
    DisableDrawTimeout();
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
