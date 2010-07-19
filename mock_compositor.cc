// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/mock_compositor.h"

#include "base/logging.h"
#include "window_manager/image_container.h"
#include "window_manager/util.h"
#include "window_manager/x_connection.h"

using std::list;
using std::string;

namespace window_manager {

MockCompositor::Actor::~Actor() {
  if (parent_) {
    parent_->stacked_children()->Remove(this);
    parent_ = NULL;
  }
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


MockCompositor::ImageActor::ImageActor() {
  Actor::SetSize(0, 0);
}

void MockCompositor::ImageActor::SetImageData(
    const ImageContainer& image_container) {
  Actor::SetSize(image_container.width(), image_container.height());
}


MockCompositor::TexturePixmapActor::TexturePixmapActor(XConnection* xconn)
    : xconn_(xconn),
      alpha_mask_bytes_(NULL),
      pixmap_(0),
      num_texture_updates_(0),
      damaged_region_() {
  Actor::SetSize(0, 0);
}

MockCompositor::TexturePixmapActor::~TexturePixmapActor() {
  ClearAlphaMask();
}

void MockCompositor::TexturePixmapActor::SetPixmap(XWindow pixmap) {
  pixmap_ = pixmap;
  XConnection::WindowGeometry geometry;
  if (xconn_->GetWindowGeometry(pixmap_, &geometry)) {
    width_ = geometry.width;
    height_ = geometry.height;
  } else {
    width_ = 0;
    height_ = 0;
  }
}

void MockCompositor::TexturePixmapActor::SetAlphaMask(
    const uint8_t* bytes, int width, int height) {
  ClearAlphaMask();
  size_t size = width * height;
  alpha_mask_bytes_ = new unsigned char[size];
  memcpy(alpha_mask_bytes_, bytes, size);
}

void MockCompositor::TexturePixmapActor::ClearAlphaMask() {
  delete[] alpha_mask_bytes_;
  alpha_mask_bytes_ = NULL;
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
