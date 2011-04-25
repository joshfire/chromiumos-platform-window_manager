// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/resize_box.h"

#include <gflags/gflags.h>

#include "base/singleton.h"
#include "window_manager/geometry.h"
#include "window_manager/image_grid.h"

DEFINE_string(resize_box_image_dir,
              "../assets/images/resize_box",
              "Directory containing images for resize boxes");

namespace window_manager {

namespace {

// How many pixels wide is the grid's border?
const int kBorderPixels = 2;

// Simple singleton container for an ImageGrid that we use for cloning (so we
// don't need to load the images off disk every time a new ResizeBox is
// created).
struct ResizeBoxPrototype {
 public:
  ResizeBoxPrototype() {}
  ~ResizeBoxPrototype() {}

  scoped_ptr<ImageGrid> image_grid;

 private:
  DISALLOW_COPY_AND_ASSIGN(ResizeBoxPrototype);
};

}  // namespace

ResizeBox::ResizeBox(Compositor* compositor) {
  ResizeBoxPrototype* proto = Singleton<ResizeBoxPrototype>::get();
  if (!proto->image_grid.get()) {
    proto->image_grid.reset(new ImageGrid(compositor));
    proto->image_grid->InitFromFiles(FLAGS_resize_box_image_dir);
  }

  image_grid_.reset(new ImageGrid(compositor));
  image_grid_->InitFromExisting(*(proto->image_grid.get()));
}

ResizeBox::~ResizeBox() {
}

Compositor::Actor* ResizeBox::actor() { return image_grid_->group(); }

void ResizeBox::SetBounds(const Rect& bounds, int anim_ms) {
  actor()->Move(bounds.x - kBorderPixels, bounds.y - kBorderPixels, anim_ms);
  image_grid_->Resize(Size(bounds.width + 2 * kBorderPixels,
                           bounds.height + 2 * kBorderPixels),
                      anim_ms);
}

}  // namespace window_manager
