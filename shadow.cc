// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/shadow.h"

#include <cmath>
#include <utility>

#include "unistd.h"

#include <gflags/gflags.h>

#include "base/singleton.h"

DEFINE_string(panel_content_shadow_image_dir,
              "../assets/images/panel_content_shadow",
              "Directory containing images for panel content shadows");
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

void Shadow::Show() {
  is_shown_ = true;
  group_->Show();
}

void Shadow::Hide() {
  is_shown_ = false;
  group_->Hide();
}

void Shadow::Move(int x, int y, int anim_ms) {
  group_->Move(x, y, anim_ms);
}

void Shadow::MoveX(int x, int anim_ms) {
  group_->MoveX(x, anim_ms);
}

void Shadow::MoveY(int y, int anim_ms) {
  group_->MoveY(y, anim_ms);
}

void Shadow::Resize(int width, int height, int anim_ms) {
  width_ = width;
  height_ = height;

  // TODO: Figure out what to do for windows that are too small for these
  // images -- currently, we'll try to scale them to negative values.
  if (top_actor_.get()) {
    top_actor_->Move(left_inset_, -top_height_, anim_ms);
    top_actor_->Scale(width - left_inset_ - right_inset_, 1.0, anim_ms);
  }
  if (bottom_actor_.get()) {
    bottom_actor_->Move(left_inset_, height, anim_ms);
    bottom_actor_->Scale(width - left_inset_ - right_inset_, 1.0, anim_ms);
  }
  if (left_actor_.get()) {
    left_actor_->Move(-left_width_, top_inset_, anim_ms);
    left_actor_->Scale(1.0, height - top_inset_ - bottom_inset_, anim_ms);
  }
  if (right_actor_.get()) {
    right_actor_->Move(width, top_inset_, anim_ms);
    right_actor_->Scale(1.0, height - top_inset_ - bottom_inset_, anim_ms);
  }

  if (top_left_actor_.get())
    top_left_actor_->Move(-left_width_, -top_height_, anim_ms);
  if (top_right_actor_.get())
    top_right_actor_->Move(width - right_inset_, -top_height_, anim_ms);
  if (bottom_left_actor_.get())
    bottom_left_actor_->Move(-left_width_, height - bottom_inset_, anim_ms);
  if (bottom_right_actor_.get())
    bottom_right_actor_->Move(
        width - right_inset_, height - bottom_inset_, anim_ms);
}

void Shadow::SetOpacity(double opacity, int anim_ms) {
  opacity_ = opacity;
  group_->SetOpacity(opacity, anim_ms);
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
      default:
        NOTREACHED() << "Unknown shadow type " << type;
    }
    shared_ptr<Shadow> prototype(new Shadow(compositor));
    prototype->InitAsPrototypeFromDisk(images_dir);
    it = prototypes_.insert(make_pair(type, prototype)).first;
  }

  Shadow* shadow = new Shadow(compositor);
  shadow->InitFromPrototype(it->second.get());
  return shadow;
}


Shadow::Shadow(Compositor* compositor)
    : compositor_(compositor),
      is_shown_(false),
      opacity_(1.0),
      width_(0),
      height_(0) {
}

void Shadow::InitAsPrototypeFromDisk(const string& images_dir) {
  top_actor_.reset(CreateActor(images_dir, "top.png"));
  bottom_actor_.reset(CreateActor(images_dir, "bottom.png"));
  left_actor_.reset(CreateActor(images_dir, "left.png"));
  right_actor_.reset(CreateActor(images_dir, "right.png"));
  top_left_actor_.reset(CreateActor(images_dir, "top_left.png"));
  top_right_actor_.reset(CreateActor(images_dir, "top_right.png"));
  bottom_left_actor_.reset(CreateActor(images_dir, "bottom_left.png"));
  bottom_right_actor_.reset(CreateActor(images_dir, "bottom_right.png"));

  top_height_ = top_actor_.get() ? top_actor_->GetHeight() : 0;
  bottom_height_ = bottom_actor_.get() ? bottom_actor_->GetHeight() : 0;
  left_width_ = left_actor_.get() ? left_actor_->GetWidth() : 0;
  right_width_ = right_actor_.get() ? right_actor_->GetWidth() : 0;

  // If the scalable actors were supplied, make sure that they're just one
  // pixel across in the dimension that we'll be scaling them.
  CHECK(!top_actor_.get() || top_actor_->GetWidth() == 1);
  CHECK(!bottom_actor_.get() || bottom_actor_->GetWidth() == 1);
  CHECK(!left_actor_.get() || left_actor_->GetHeight() == 1);
  CHECK(!right_actor_.get() || right_actor_->GetHeight() == 1);

  // If any two adjacent corner actors were loaded, make sure that they're
  // the same size on the side that they share.
  if (top_left_actor_.get() && top_right_actor_.get())
    CHECK(top_left_actor_->GetHeight() == top_right_actor_->GetHeight());
  if (bottom_left_actor_.get() && bottom_right_actor_.get())
    CHECK(bottom_left_actor_->GetHeight() == bottom_right_actor_->GetHeight());
  if (top_left_actor_.get() && bottom_left_actor_.get())
    CHECK(top_left_actor_->GetWidth() == bottom_left_actor_->GetWidth());
  if (top_right_actor_.get() && bottom_right_actor_.get())
    CHECK(top_right_actor_->GetWidth() == bottom_right_actor_->GetWidth());

  // Now figure out how many pixels of the corner images overlap with the
  // window on each side.
  top_inset_ = bottom_inset_ = left_inset_ = right_inset_ = 0;

  if (top_actor_.get() && left_actor_.get()) {
    CHECK(top_left_actor_.get());
    top_inset_ = top_left_actor_->GetHeight() - top_height_;
    left_inset_ = top_left_actor_->GetWidth() - left_width_;
  } else {
    CHECK(!top_left_actor_.get());
  }

  if (top_actor_.get() && right_actor_.get()) {
    CHECK(top_right_actor_.get());
    top_inset_ = top_right_actor_->GetHeight() - top_height_;
    right_inset_ = top_right_actor_->GetWidth() - right_width_;
  } else {
    CHECK(!top_right_actor_.get());
  }

  if (bottom_actor_.get() && left_actor_.get()) {
    CHECK(bottom_left_actor_.get());
    bottom_inset_ = bottom_left_actor_->GetHeight() - bottom_height_;
    left_inset_ = bottom_left_actor_->GetWidth() - left_width_;
  } else {
    CHECK(!bottom_left_actor_.get());
  }

  if (bottom_actor_.get() && right_actor_.get()) {
    CHECK(bottom_right_actor_.get());
    bottom_inset_ = bottom_right_actor_->GetHeight() - bottom_height_;
    right_inset_ = bottom_right_actor_->GetWidth() - right_width_;
  } else {
    CHECK(!bottom_right_actor_.get());
  }
}

void Shadow::InitFromPrototype(Shadow* prototype) {
  DCHECK(prototype);

  group_.reset(compositor_->CreateGroup());
  group_->SetName("shadow group");

  if (prototype->top_actor_.get()) {
    top_actor_.reset(compositor_->CloneActor(prototype->top_actor_.get()));
    top_actor_->SetName("shadow top");
    top_actor_->Show();
    group_->AddActor(top_actor_.get());
  }
  if (prototype->bottom_actor_.get()) {
    bottom_actor_.reset(
        compositor_->CloneActor(prototype->bottom_actor_.get()));
    bottom_actor_->SetName("shadow bottom");
    bottom_actor_->Show();
    group_->AddActor(bottom_actor_.get());
  }
  if (prototype->left_actor_.get()) {
    left_actor_.reset(compositor_->CloneActor(prototype->left_actor_.get()));
    left_actor_->SetName("shadow left");
    left_actor_->Show();
    group_->AddActor(left_actor_.get());
  }
  if (prototype->right_actor_.get()) {
    right_actor_.reset(compositor_->CloneActor(prototype->right_actor_.get()));
    right_actor_->SetName("shadow right");
    right_actor_->Show();
    group_->AddActor(right_actor_.get());
  }
  if (prototype->top_left_actor_.get()) {
    top_left_actor_.reset(
        compositor_->CloneActor(prototype->top_left_actor_.get()));
    top_left_actor_->SetName("shadow top left");
    top_left_actor_->Show();
    group_->AddActor(top_left_actor_.get());
  }
  if (prototype->top_right_actor_.get()) {
    top_right_actor_.reset(
        compositor_->CloneActor(prototype->top_right_actor_.get()));
    top_right_actor_->SetName("shadow top right");
    top_right_actor_->Show();
    group_->AddActor(top_right_actor_.get());
  }
  if (prototype->bottom_left_actor_.get()) {
    bottom_left_actor_.reset(
        compositor_->CloneActor(prototype->bottom_left_actor_.get()));
    bottom_left_actor_->SetName("shadow bottom left");
    bottom_left_actor_->Show();
    group_->AddActor(bottom_left_actor_.get());
  }
  if (prototype->bottom_right_actor_.get()) {
    bottom_right_actor_.reset(
        compositor_->CloneActor(prototype->bottom_right_actor_.get()));
    bottom_right_actor_->SetName("shadow bottom right");
    bottom_right_actor_->Show();
    group_->AddActor(bottom_right_actor_.get());
  }

  top_height_ = prototype->top_height_;
  bottom_height_ = prototype->bottom_height_;
  left_width_ = prototype->left_width_;
  right_width_ = prototype->right_width_;

  top_inset_ = prototype->top_inset_;
  bottom_inset_ = prototype->bottom_inset_;
  left_inset_ = prototype->left_inset_;
  right_inset_ = prototype->right_inset_;

  // Resize the shadow arbitrarily to initialize the positions of the actors.
  Resize(10, 10, 0);
  SetOpacity(1.0, 0);
  Hide();
}

Compositor::ImageActor* Shadow::CreateActor(const string& images_dir,
                                            const string& filename) {
  const string path = images_dir + "/" + filename;
  if (access(path.c_str(), R_OK) != 0)
    return NULL;
  return compositor_->CreateImageFromFile(path);
}

}  // namespace window_manager
