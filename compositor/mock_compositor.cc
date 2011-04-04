// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor/mock_compositor.h"

#include "base/logging.h"
#include "window_manager/image_container.h"
#include "window_manager/util.h"
#include "window_manager/x11/x_connection.h"

using std::list;
using std::string;
using window_manager::util::GetMonotonicTime;

namespace window_manager {

MockCompositor::Actor::~Actor() {
  if (parent_) {
    parent_->stacked_children()->Remove(this);
    parent_ = NULL;
  }
}

void MockCompositor::Actor::Move(int x, int y, int anim_ms) {
  x_ = x;
  y_ = y;
  num_moves_++;
  position_was_animated_ = (anim_ms > 0);
}

AnimationPair* MockCompositor::Actor::CreateMoveAnimation() {
  return new AnimationPair(
      new Animation(x_, GetMonotonicTime()),
      new Animation(y_, GetMonotonicTime()));
}

void MockCompositor::Actor::SetMoveAnimation(AnimationPair* animations) {
  CHECK(animations);
  delete animations;
}

void MockCompositor::Actor::Raise(Compositor::Actor* other) {
  CHECK(parent_);
  CHECK(other);
  MockCompositor::Actor* cast_other =
      dynamic_cast<MockCompositor::Actor*>(other);
  CHECK(cast_other);
  CHECK(parent_->stacked_children()->Contains(this));
  CHECK(parent_->stacked_children()->Contains(cast_other));
  parent_->stacked_children()->Remove(this);
  parent_->stacked_children()->AddAbove(this, cast_other);
}

void MockCompositor::Actor::Lower(Compositor::Actor* other) {
  CHECK(parent_);
  CHECK(other);
  MockCompositor::Actor* cast_other =
      dynamic_cast<MockCompositor::Actor*>(other);
  CHECK(cast_other);
  CHECK(parent_->stacked_children()->Contains(this));
  CHECK(parent_->stacked_children()->Contains(cast_other));
  parent_->stacked_children()->Remove(this);
  parent_->stacked_children()->AddBelow(this, cast_other);
}

void MockCompositor::Actor::RaiseToTop() {
  CHECK(parent_);
  CHECK(parent_->stacked_children()->Contains(this));
  parent_->stacked_children()->Remove(this);
  parent_->stacked_children()->AddOnTop(this);
}

void MockCompositor::Actor::LowerToBottom() {
  CHECK(parent_);
  CHECK(parent_->stacked_children()->Contains(this));
  parent_->stacked_children()->Remove(this);
  parent_->stacked_children()->AddOnBottom(this);
}

string MockCompositor::Actor::GetDebugString(int indent_level) {
  string out;
  out.assign(indent_level * 2, ' ');
  out += (!name_.empty() ? name_ : "unnamed actor") + "\n";
  return out;
}


MockCompositor::ContainerActor::ContainerActor()
    : stacked_children_(new Stacker<Actor*>) {
}

MockCompositor::ContainerActor::~ContainerActor() {
  typedef list<Actor*>::const_iterator iterator;

  for (iterator it = stacked_children_->items().begin();
       it != stacked_children_->items().end(); ++it) {
    (*it)->set_parent(NULL);
  }
}

string MockCompositor::ContainerActor::GetDebugString(int indent_level) {
  string out = Actor::GetDebugString(indent_level);
  for (list<Actor*>::const_iterator it = stacked_children_->items().begin();
       it != stacked_children_->items().end(); ++it) {
    out += (*it)->GetDebugString(indent_level + 1);
  }
  return out;
}

void MockCompositor::ContainerActor::AddActor(Compositor::Actor* actor) {
  MockCompositor::Actor* cast_actor =
      dynamic_cast<MockCompositor::Actor*>(actor);
  CHECK(cast_actor);
  CHECK(cast_actor->parent() == static_cast<ContainerActor*>(NULL));
  cast_actor->set_parent(this);
  CHECK(!stacked_children_->Contains(cast_actor));
  stacked_children_->AddOnTop(cast_actor);
}

int MockCompositor::ContainerActor::GetStackingIndex(Compositor::Actor* actor) {
  CHECK(actor);
  MockCompositor::Actor* cast_actor =
      dynamic_cast<MockCompositor::Actor*>(actor);
  CHECK(cast_actor);
  return stacked_children_->GetIndex(cast_actor);
}


MockCompositor::ColoredBoxActor::ColoredBoxActor(int width, int height,
                                                 const Compositor::Color& color)
    : color_(color) {
  SetSizeInternal(width, height);
}


MockCompositor::ImageActor::ImageActor() {
  SetSizeInternal(0, 0);
}

void MockCompositor::ImageActor::SetImageData(
    const ImageContainer& image_container) {
  SetSizeInternal(image_container.width(), image_container.height());
}


MockCompositor::TexturePixmapActor::TexturePixmapActor(XConnection* xconn)
    : xconn_(xconn),
      pixmap_(0),
      num_texture_updates_(0),
      damaged_region_() {
  SetSizeInternal(0, 0);
}

MockCompositor::TexturePixmapActor::~TexturePixmapActor() {
}

void MockCompositor::TexturePixmapActor::SetPixmap(XWindow pixmap) {
  pixmap_ = pixmap;
  XConnection::WindowGeometry geometry;
  if (xconn_->GetWindowGeometry(pixmap_, &geometry)) {
    SetSizeInternal(geometry.bounds.width, geometry.bounds.height);
  } else {
    SetSizeInternal(0, 0);
  }
}

void MockCompositor::TexturePixmapActor::SetAlphaMask(
    const uint8_t* bytes, int width, int height) {
  ClearAlphaMask();
  size_t size = width * height;
  alpha_mask_bytes_.reset(new uint8_t[size]);
  memcpy(alpha_mask_bytes_.get(), bytes, size);
}

void MockCompositor::TexturePixmapActor::ClearAlphaMask() {
  alpha_mask_bytes_.reset();
}

MockCompositor::ImageActor* MockCompositor::CreateImageFromFile(
    const std::string& filename) {
  ImageActor* actor = new ImageActor;
  InMemoryImageContainer container(
      new uint8_t[1], 1, 1, IMAGE_FORMAT_RGBA_32, false);  // malloc=false
  actor->SetImageData(container);
  return actor;
}

void MockCompositor::TexturePixmapActor::MergeDamagedRegion(
    const Rect& region) {
  damaged_region_.merge(region);
}

const Rect& MockCompositor::TexturePixmapActor::GetDamagedRegion() const {
  return damaged_region_;
}

void MockCompositor::TexturePixmapActor::ResetDamagedRegion() {
  damaged_region_.reset(0, 0, 0, 0);
}

}  // namespace window_manager
