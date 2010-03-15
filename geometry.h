// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_GEOMETRY_H_
#define WINDOW_MANAGER_GEOMETRY_H_

namespace window_manager {

// This file defines a few simple types for storing geometry-related data.
// We define our own instead of using Chrome's since its base/gfx/rect.h
// file references some GDK types that we don't want.

// This enum is used for resize operations in a manner similar to X's
// concept of gravity -- it describes which corner of an object will be
// kept at a fixed position as the object is resized.  For example, if a
// 10x10 rectangle with its top-left corner at (20, 20) is resized to 5x5
// with GRAVITY_SOUTHEAST, the resulting 5x5 rectangle will be located at
// (25, 25).
enum Gravity {
  GRAVITY_NORTHWEST = 0,
  GRAVITY_NORTHEAST,
  GRAVITY_SOUTHWEST,
  GRAVITY_SOUTHEAST,
};

struct Point {
  Point() : x(0), y(0) {}
  Point(int x, int y) : x(x), y(y) {}

  int x;
  int y;
};

struct Rect {
  Rect() : x(0), y(0), width(0), height(0) {}
  Rect(int x, int y, int w, int h) : x(x), y(y), width(w), height(h) {}

  void resize(int w, int h, Gravity gravity) {
    if (gravity == GRAVITY_NORTHEAST || gravity == GRAVITY_SOUTHEAST)
      x += (width - w);
    if (gravity == GRAVITY_SOUTHWEST || gravity == GRAVITY_SOUTHEAST)
      y += (height - h);

    width = w;
    height = h;
  }

  int x;
  int y;
  int width;
  int height;
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_GEOMETRY_H_
