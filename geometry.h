// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_GEOMETRY_H_
#define WINDOW_MANAGER_GEOMETRY_H_

#include <algorithm>
#include <iosfwd>

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

  void reset(int new_x, int new_y) {
    x = new_x;
    y = new_y;
  }

  bool operator==(const Point& o) const {
    return x == o.x && y == o.y;
  }
  bool operator!=(const Point& o) const {
    return x != o.x || y != o.y;
  }

  int x;
  int y;
};

struct Size {
  Size() : width(0), height(0) {}
  Size(int width, int height) : width(width), height(height) {}

  void reset(int new_width, int new_height) {
    width = new_width;
    height = new_height;
  }

  bool empty() const { return width <= 0 || height <= 0; }
  int area() const { return empty() ? 0 : width * height; }

  bool operator==(const Size& o) const {
    return width == o.width && height == o.height;
  }
  bool operator!=(const Size& o) const {
    return width != o.width || height != o.height;
  }

  int width;
  int height;
};

struct Rect {
  Rect() : x(0), y(0), width(0), height(0) {}
  Rect(int x, int y, int w, int h) : x(x), y(y), width(w), height(h) {}
  Rect(const Point& pos, const Size& size) { reset(pos, size); }

  Point position() const { return Point(x, y); }
  Size size() const { return Size(width, height); }

  void reset(const Point& pos, const Size& size) {
    reset(pos.x, pos.y, size.width, size.height);
  }
  void reset(int new_x, int new_y, int new_width, int new_height) {
    x = new_x;
    y = new_y;
    width = new_width;
    height = new_height;
  }

  void move(const Point& pos) { move(pos.x, pos.y); }
  void move(int new_x, int new_y) {
    x = new_x;
    y = new_y;
  }

  void resize(const Size& size, Gravity gravity) {
    resize(size.width, size.height, gravity);
  }
  void resize(int w, int h, Gravity gravity) {
    if (gravity == GRAVITY_NORTHEAST || gravity == GRAVITY_SOUTHEAST)
      x += (width - w);
    if (gravity == GRAVITY_SOUTHWEST || gravity == GRAVITY_SOUTHEAST)
      y += (height - h);

    width = w;
    height = h;
  }

  bool empty() const { return width <= 0 || height <= 0; }

  void merge(const Rect& other) {
    if (other.empty())
      return;
    if (empty()) {
      *this = other;
    } else {
      int x_max = std::max(x + width, other.x + other.width);
      x = std::min(x, other.x);
      width = x_max - x;
      int y_max = std::max(y + height, other.y + other.height);
      y = std::min(y, other.y);
      height = y_max - y;
    }
  }

  void intersect(const Rect& other) {
    if (other.empty() || empty()) {
      width = height = 0;
      return;
    }

    int max_x = std::min(x + width, other.x + other.width);
    x = std::max(x, other.x);
    width = std::max(0, max_x - x);
    int max_y = std::min(y + height, other.y + other.height);
    y = std::max(y, other.y);
    height = std::max(0, max_y - y);
  }

  bool contains_rect(const Rect& rect) const {
    return !rect.empty() &&
           rect.x >= x && rect.right() <= right() &&
           rect.y >= y && rect.bottom() <= bottom();
  }

  bool contains_point(const Point& point) const {
    return point.x >= x &&
           point.x < x + width &&
           point.y >= y &&
           point.y < y + height;
  }

  bool operator==(const Rect& o) const {
    return x == o.x && y == o.y && width == o.width && height == o.height;
  }
  bool operator!=(const Rect& o) const {
    return x != o.x || y != o.y || width != o.width || height != o.height;
  }

  int left() const { return x; }
  int top() const { return y; }
  int right() const { return x + width; }
  int bottom() const { return y + height; }
  unsigned area() const { return width * height; }

  int x;
  int y;
  int width;
  int height;
};

}  // namespace window_manager

std::ostream& operator<<(std::ostream& out, const window_manager::Point& point);
std::ostream& operator<<(std::ostream& out, const window_manager::Size& size);
std::ostream& operator<<(std::ostream& out, const window_manager::Rect& rect);

#endif  // WINDOW_MANAGER_GEOMETRY_H_
