// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_RESIZE_BOX_H_
#define WINDOW_MANAGER_RESIZE_BOX_H_

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"

namespace window_manager {

class ImageGrid;
class Rect;

// ResizeBox is a simple wrapper around an ImageGrid.  It can be drawn onscreen
// to show the size of an object while the user is resizing it (opaque resizing
// of web content can be janky).
class ResizeBox {
 public:
  explicit ResizeBox(Compositor* compositor);
  ~ResizeBox();

  // Get the ImageGrid's group actor.  This is provided for adding the grid to a
  // stage or setting its opacity; SetBounds() should be used to move or resize
  // the grid.
  Compositor::Actor* actor();

  // Configure the ImageGrid's bounds.  The grid is actually made slightly
  // larger than |bounds| to accomodate its borders.
  void SetBounds(const Rect& bounds, int anim_ms);

 private:
  scoped_ptr<ImageGrid> image_grid_;

  DISALLOW_COPY_AND_ASSIGN(ResizeBox);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_RESIZE_BOX_H_
