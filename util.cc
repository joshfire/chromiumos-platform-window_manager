// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/util.h"

#include <algorithm>
#include <cstring>

#include "base/string_util.h"

using std::max;
using std::min;
using std::string;

namespace window_manager {

ByteMap::ByteMap(int width, int height)
    : width_(width),
      height_(height) {
  CHECK(width > 0);
  CHECK(height > 0);
  bytes_ = new unsigned char[width * height];
  Clear(0);
}

ByteMap::~ByteMap() {
  delete[] bytes_;
  bytes_ = NULL;
}

void ByteMap::Copy(const ByteMap& other) {
  CHECK(width_ == other.width_);
  CHECK(height_ == other.height_);
  memcpy(bytes_, other.bytes_, width_ * height_);
}

void ByteMap::Clear(unsigned char value) {
  memset(bytes_, value, width_ * height_);
}

void ByteMap::SetRectangle(int rect_x, int rect_y,
                           int rect_width, int rect_height,
                           unsigned char value) {
  const int limit_x = min(rect_x + rect_width, width_);
  const int limit_y = min(rect_y + rect_height, height_);
  rect_x = max(rect_x, 0);
  rect_y = max(rect_y, 0);

  if (rect_x >= limit_x)
    return;

  for (int y = rect_y; y < limit_y; ++y)
    memset(bytes_ + y * width_ + rect_x, value, limit_x - rect_x);
}


string XidStr(unsigned long xid) {
  return StringPrintf("0x%lx", xid);
}

string GetTimeAsString(time_t utime) {
  struct tm tm;
  CHECK(localtime_r(&utime, &tm) == &tm);
  char str[16];
  CHECK(strftime(str, sizeof(str), "%Y%m%d-%H%M%S", &tm) == 15);
  return string(str);
}

}  // namespace window_manager
