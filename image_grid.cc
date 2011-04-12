// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/image_grid.h"

#include <algorithm>

#include "unistd.h"

using std::max;
using std::string;

namespace window_manager {

namespace {

int GetActorWidth(const Compositor::Actor* actor) {
  return actor ? actor->GetWidth() : 0;
}

int GetActorHeight(const Compositor::Actor* actor) {
  return actor ? actor->GetHeight() : 0;
}

}  // namespace

const char ImageGrid::kTopFilename[] = "top.png";
const char ImageGrid::kBottomFilename[] = "bottom.png";
const char ImageGrid::kLeftFilename[] = "left.png";
const char ImageGrid::kRightFilename[] = "right.png";
const char ImageGrid::kTopLeftFilename[] = "top_left.png";
const char ImageGrid::kTopRightFilename[] = "top_right.png";
const char ImageGrid::kBottomLeftFilename[] = "bottom_left.png";
const char ImageGrid::kBottomRightFilename[] = "bottom_right.png";
const char ImageGrid::kCenterFilename[] = "center.png";

ImageGrid::ImageGrid(Compositor* compositor)
    : compositor_(compositor),
      initialized_(false),
      top_height_(0),
      bottom_height_(0),
      left_width_(0),
      right_width_(0),
      group_(compositor_->CreateGroup()) {
  group_->SetName("image grid group");
  group_->Show();
}

ImageGrid::~ImageGrid() {}

void ImageGrid::InitFromFiles(const string& images_dir) {
  DCHECK(!initialized_);

  top_actor_.reset(CreateActor(images_dir, kTopFilename));
  bottom_actor_.reset(CreateActor(images_dir, kBottomFilename));
  left_actor_.reset(CreateActor(images_dir, kLeftFilename));
  right_actor_.reset(CreateActor(images_dir, kRightFilename));
  top_left_actor_.reset(CreateActor(images_dir, kTopLeftFilename));
  top_right_actor_.reset(CreateActor(images_dir, kTopRightFilename));
  bottom_left_actor_.reset(CreateActor(images_dir, kBottomLeftFilename));
  bottom_right_actor_.reset(CreateActor(images_dir, kBottomRightFilename));
  center_actor_.reset(CreateActor(images_dir, kCenterFilename));

  top_height_ = max(GetActorHeight(top_actor_.get()),
                    max(GetActorHeight(top_left_actor_.get()),
                        GetActorHeight(top_right_actor_.get())));
  bottom_height_ = max(GetActorHeight(bottom_actor_.get()),
                       max(GetActorHeight(bottom_left_actor_.get()),
                           GetActorHeight(bottom_right_actor_.get())));
  left_width_ = max(GetActorWidth(left_actor_.get()),
                    max(GetActorWidth(top_left_actor_.get()),
                        GetActorWidth(bottom_left_actor_.get())));
  right_width_ = max(GetActorWidth(right_actor_.get()),
                     max(GetActorWidth(top_right_actor_.get()),
                         GetActorWidth(bottom_right_actor_.get())));

  initialized_ = true;
}

void ImageGrid::InitFromExisting(const ImageGrid& src) {
  DCHECK(!initialized_);

  top_actor_.reset(CloneActor(src.top_actor_.get()));
  bottom_actor_.reset(CloneActor(src.bottom_actor_.get()));
  left_actor_.reset(CloneActor(src.left_actor_.get()));
  right_actor_.reset(CloneActor(src.right_actor_.get()));
  top_left_actor_.reset(CloneActor(src.top_left_actor_.get()));
  top_right_actor_.reset(CloneActor(src.top_right_actor_.get()));
  bottom_left_actor_.reset(CloneActor(src.bottom_left_actor_.get()));
  bottom_right_actor_.reset(CloneActor(src.bottom_right_actor_.get()));
  center_actor_.reset(CloneActor(src.center_actor_.get()));

  top_height_ = src.top_height_;
  bottom_height_ = src.bottom_height_;
  left_width_ = src.left_width_;
  right_width_ = src.right_width_;

  initialized_ = true;
}

void ImageGrid::Resize(const Size& size, int anim_ms) {
  size_ = size;

  // TODO: Figure out what to do with sizes that are too small for these images
  // -- currently, we'll try to scale the images to negative values.
  double center_width = size.width - left_width_ - right_width_;
  double center_height = size.height - top_height_ - bottom_height_;

  if (top_actor_.get()) {
    top_actor_->Move(left_width_, 0, anim_ms);
    top_actor_->Scale(center_width / top_actor_->GetWidth(), 1.0, anim_ms);
  }
  if (bottom_actor_.get()) {
    bottom_actor_->Move(
        left_width_, size.height - bottom_actor_->GetHeight(), anim_ms);
    bottom_actor_->Scale(
        center_width / bottom_actor_->GetWidth(), 1.0, anim_ms);
  }
  if (left_actor_.get()) {
    left_actor_->Move(0, top_height_, anim_ms);
    left_actor_->Scale(1.0, center_height / left_actor_->GetHeight(), anim_ms);
  }
  if (right_actor_.get()) {
    right_actor_->Move(
        size.width - right_actor_->GetWidth(), top_height_, anim_ms);
    right_actor_->Scale(
        1.0, center_height / right_actor_->GetHeight(), anim_ms);
  }

  if (top_left_actor_.get()) {
    top_left_actor_->Move(0, 0, anim_ms);
  }
  if (top_right_actor_.get()) {
    top_right_actor_->Move(
        size.width - top_right_actor_->GetWidth(), 0, anim_ms);
  }
  if (bottom_left_actor_.get()) {
    bottom_left_actor_->Move(
        0, size.height - bottom_left_actor_->GetHeight(), anim_ms);
  }
  if (bottom_right_actor_.get()) {
    bottom_right_actor_->Move(
        size.width - bottom_right_actor_->GetWidth(),
        size.height - bottom_right_actor_->GetHeight(),
        anim_ms);
  }

  if (center_actor_.get()) {
    center_actor_->Move(left_width_, top_height_, anim_ms);
    center_actor_->Scale(center_width / center_actor_->GetWidth(),
                         center_height / center_actor_->GetHeight(),
                         anim_ms);
  }
}

Compositor::Actor* ImageGrid::CreateActor(const string& images_dir,
                                               const string& filename) {
  const string path = images_dir + "/" + filename;
  if (access(path.c_str(), R_OK) != 0)
    return NULL;

  Compositor::Actor* actor = compositor_->CreateImageFromFile(path);
  group_->AddActor(actor);
  actor->SetName(filename);
  actor->Show();
  return actor;
}

Compositor::Actor* ImageGrid::CloneActor(Compositor::Actor* src) {
  if (!src)
    return NULL;

  Compositor::Actor* actor = compositor_->CloneActor(src);
  group_->AddActor(actor);
  actor->Show();
  return actor;
}

}  // namespace window_manager
