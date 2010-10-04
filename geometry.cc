// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/geometry.h"

#include <ostream>

std::ostream& operator<<(std::ostream& out,
                         const window_manager::Point& point) {
  return out << "(" << point.x << ", " << point.y << ")";
}

std::ostream& operator<<(std::ostream& out, const window_manager::Size& size) {
  return out << size.width << "x" << size.height;
}

std::ostream& operator<<(std::ostream& out, const window_manager::Rect& rect) {
  return out << rect.position() << " " << rect.size();
}
