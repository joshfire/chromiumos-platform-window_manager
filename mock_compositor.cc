// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor.h"

#include "base/logging.h"
#include "window_manager/util.h"
#include "window_manager/x_connection.h"

using std::list;

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

void MockCompositor::ContainerActor::AddActor(Compositor::Actor* actor) {
  MockCompositor::Actor* cast_actor =
      dynamic_cast<MockCompositor::Actor*>(actor);
  CHECK(cast_actor);
  CHECK(cast_actor->parent() == static_cast<ContainerActor*>(NULL));
  cast_actor->set_parent(this);
  CHECK(!stacked_children_->Contains(cast_actor));
  stacked_children_->AddOnBottom(cast_actor);
}

int MockCompositor::ContainerActor::GetStackingIndex(Compositor::Actor* actor) {
  CHECK(actor);
  MockCompositor::Actor* cast_actor =
      dynamic_cast<MockCompositor::Actor*>(actor);
  CHECK(cast_actor);
  return stacked_children_->GetIndex(cast_actor);
}


bool MockCompositor::TexturePixmapActor::SetAlphaMask(
    const unsigned char* bytes, int width, int height) {
  ClearAlphaMask();
  size_t size = width * height;
  alpha_mask_bytes_ = new unsigned char[size];
  memcpy(alpha_mask_bytes_, bytes, size);
  return true;
}

void MockCompositor::TexturePixmapActor::ClearAlphaMask() {
  delete[] alpha_mask_bytes_;
  alpha_mask_bytes_ = NULL;
}

}  // namespace window_manager