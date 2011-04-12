// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/shadow.h"

#include <utility>

#include <gflags/gflags.h>

#include "base/singleton.h"
#include "window_manager/image_grid.h"

DEFINE_string(panel_content_shadow_image_dir,
              "../assets/images/panel_content_shadow",
              "Directory containing images for panel content shadows");
DEFINE_string(panel_separator_shadow_image_dir,
              "../assets/images/panel_separator_shadow",
              "Directory containing images for shadow cast by panel "
              "titlebars onto their content windows");
DEFINE_string(panel_titlebar_shadow_image_dir,
              "../assets/images/panel_titlebar_shadow",
              "Directory containing images for panel titlebar shadows");
DEFINE_string(rectangular_shadow_image_dir,
              "../assets/images/rectangular_shadow",
              "Directory containing images for rectangular shadows");

using std::make_pair;
using std::string;
using std::tr1::shared_ptr;

namespace window_manager {

// static
Shadow* Shadow::Create(Compositor* compositor, Type type) {
  return Singleton<Shadow::Factory>::get()->CreateShadow(compositor, type);
}

Shadow::~Shadow() {}

Compositor::Actor* Shadow::group() const {
  return grid_->group();
}

void Shadow::Show() {
  is_shown_ = true;
  grid_->group()->Show();
}

void Shadow::Hide() {
  is_shown_ = false;
  grid_->group()->Hide();
}

void Shadow::Move(int x, int y, int anim_ms) {
  x_ = x;
  y_ = y;
  grid_->group()->Move(x - grid_->left_actor_width(),
                       y - grid_->top_actor_height(), anim_ms);
}

void Shadow::MoveX(int x, int anim_ms) {
  x_ = x;
  grid_->group()->MoveX(x - grid_->left_actor_width(), anim_ms);
}

void Shadow::MoveY(int y, int anim_ms) {
  y_ = y;
  grid_->group()->MoveY(y - grid_->top_actor_height(), anim_ms);
}

void Shadow::Resize(int width, int height, int anim_ms) {
  width_ = width;
  height_ = height;
  grid_->Resize(
      Size(width + grid_->left_actor_width() + grid_->right_actor_width(),
           height + grid_->top_actor_height() + grid_->bottom_actor_height()),
      anim_ms);
}

void Shadow::SetOpacity(double opacity, int anim_ms) {
  opacity_ = opacity;
  grid_->group()->SetOpacity(opacity, anim_ms);
}

int Shadow::GetMinWidth() const {
  // Return the minimum width of the ImageGrid (that is, the width of its left
  // column plus the width of its right column) minus the number of pixels that
  // should hang outside of the window (that is, the width of the left side
  // actor plus the width of the right side actor).
  return (grid_->left_width() + grid_->right_width()) -
         (grid_->left_actor_width() + grid_->right_actor_width());
}

int Shadow::GetMinHeight() const {
  return (grid_->top_height() + grid_->bottom_height()) -
         (grid_->top_actor_height() + grid_->bottom_actor_height());
}


Shadow* Shadow::Factory::CreateShadow(Compositor* compositor, Type type) {
  PrototypeMap::iterator it = prototypes_.find(type);
  if (it == prototypes_.end()) {
    string images_dir;
    switch (type) {
      case TYPE_RECTANGULAR:
        images_dir = FLAGS_rectangular_shadow_image_dir;
        break;
      case TYPE_PANEL_TITLEBAR:
        images_dir = FLAGS_panel_titlebar_shadow_image_dir;
        break;
      case TYPE_PANEL_CONTENT:
        images_dir = FLAGS_panel_content_shadow_image_dir;
        break;
      case TYPE_PANEL_SEPARATOR:
        images_dir = FLAGS_panel_separator_shadow_image_dir;
        break;
      default:
        NOTREACHED() << "Unknown shadow type " << type;
    }
    shared_ptr<Shadow> prototype(new Shadow(compositor));
    prototype->InitFromFiles(images_dir);
    it = prototypes_.insert(make_pair(type, prototype)).first;
  }

  Shadow* shadow = new Shadow(compositor);
  shadow->InitFromExisting(*(it->second.get()));
  return shadow;
}


Shadow::Shadow(Compositor* compositor)
    : compositor_(compositor),
      is_shown_(true),
      opacity_(1.0),
      x_(0),
      y_(0),
      width_(0),
      height_(0),
      grid_(new ImageGrid(compositor)) {
}

void Shadow::InitFromFiles(const std::string& images_dir) {
  grid_->InitFromFiles(images_dir);
}

void Shadow::InitFromExisting(const Shadow& shadow) {
  grid_->InitFromExisting(*(shadow.grid_.get()));
}

}  // namespace window_manager
