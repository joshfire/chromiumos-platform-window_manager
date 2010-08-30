// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/util.h"

#include <algorithm>
#include <cstring>
#include <ctime>

#include <sys/time.h>
#include <unistd.h>

#include "base/string_util.h"
#include "base/time.h"

using base::TimeTicks;
using std::max;
using std::min;
using std::string;

namespace window_manager {

// If non-negative, contains a hardcoded time to be returned by
// GetCurrentTimeSecs() and GetCurrentTimeMs().
static int64_t current_time_ms_for_test = -1;

// If non-negative, contains a hardcoded time to be returned by
// GetMonotonicTimeMs().
static int64_t monotonic_time_ms_for_test = -1;

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

bool ByteMap::operator==(const ByteMap& other) {
  if (width_ != other.width_ || height_ != other.height_)
    return false;
  return memcmp(bytes_, other.bytes_, width_ * height_) == 0;
}


namespace util {

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

time_t GetCurrentTimeSec() {
  if (current_time_ms_for_test >= 0)
    return static_cast<time_t>(current_time_ms_for_test / 1000);
  return time(NULL);
}

int64_t GetCurrentTimeMs() {
  if (current_time_ms_for_test >= 0)
    return current_time_ms_for_test;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return 1000ULL * tv.tv_sec + tv.tv_usec / 1000ULL;
}

void SetCurrentTimeForTest(time_t sec, int ms) {
  current_time_ms_for_test =
      (sec < 0) ? -1 : static_cast<int64_t>(sec) * 1000 + ms;
}

int64_t GetMonotonicTimeMs() {
  if (monotonic_time_ms_for_test >= 0)
    return monotonic_time_ms_for_test;
  // Chrome's TimeTicks class stores times as milliseconds internally.
  return TimeTicks::Now().ToInternalValue() / 1000;
}

void SetMonotonicTimeMsForTest(int64_t ms) {
  monotonic_time_ms_for_test = (ms < 0) ? -1 : ms;
}

bool SetUpLogSymlink(const std::string& symlink_path,
                     const std::string& log_basename) {
  if (access(symlink_path.c_str(), F_OK) == 0 &&
      unlink(symlink_path.c_str()) == -1) {
    PLOG(ERROR) << "Unable to unlink " << symlink_path;
    return false;
  }
  if (symlink(log_basename.c_str(), symlink_path.c_str()) == -1) {
    PLOG(ERROR) << "Unable to create symlink " << symlink_path
                << " pointing at " << log_basename;
    return false;
  }
  return true;
}

}  // namespace util

}  // namespace window_manager
