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

using base::TimeDelta;
using base::TimeTicks;
using std::max;
using std::min;
using std::string;

namespace window_manager {

// If non-negative, contains a hardcoded time to be returned by
// GetCurrentTimeSecs() and GetCurrentTimeMs().
static int64_t current_time_ms_for_test = -1;

// If non-zero, contains a hardcoded time to be returned by
// GetMonotonicTimeMs().
static TimeTicks monotonic_time_for_test;

ByteMap::ByteMap(const Size& size) : size_(size) {
  CHECK(!size_.empty());
  bytes_ = new unsigned char[size_.area()];
  Clear(0);
}

ByteMap::~ByteMap() {
  delete[] bytes_;
  bytes_ = NULL;
}

void ByteMap::Copy(const ByteMap& other) {
  CHECK(size_ == other.size_);
  memcpy(bytes_, other.bytes_, size_.area());
}

void ByteMap::Clear(unsigned char value) {
  memset(bytes_, value, size_.width * size_.height);
}

void ByteMap::SetRectangle(const Rect& rect, unsigned char value) {
  if (rect.empty())
    return;

  const int limit_x = min(rect.x + rect.width, size_.width);
  const int limit_y = min(rect.y + rect.height, size_.height);
  const int capped_x = max(rect.x, 0);
  const int capped_y = max(rect.y, 0);

  if (capped_x >= limit_x)
    return;

  for (int y = capped_y; y < limit_y; ++y)
    memset(bytes_ + y * size_.width + capped_x, value, limit_x - capped_x);
}

bool ByteMap::operator==(const ByteMap& other) {
  if (size_ != other.size_)
    return false;
  return memcmp(bytes_, other.bytes_, size_.area()) == 0;
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

TimeTicks GetMonotonicTime() {
  if (!monotonic_time_for_test.is_null())
    return monotonic_time_for_test;
  return TimeTicks::Now();
}

void SetMonotonicTimeForTest(const TimeTicks& now) {
  monotonic_time_for_test = now;
}

TimeTicks CreateTimeTicksFromMs(int64_t time_ms) {
  TimeTicks t;
  int64_t diff_usec = time_ms * 1000 - t.ToInternalValue();
  t += TimeDelta::FromMicroseconds(diff_usec);
  return t;
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

string GetHostname() {
  char hostname[256];
  if (gethostname(hostname, sizeof(hostname)) != 0) {
    PLOG(ERROR) << "Unable to look up hostname";
    return string();
  }
  hostname[sizeof(hostname) - 1] = '\0';
  return string(hostname);
}

void RunCommandInBackground(string command) {
  if (command.empty())
    return;

  command += " &";
  DLOG(INFO) << "Running command \"" << command << "\"";
  if (system(command.c_str()) < 0)
    LOG(WARNING) << "Got error while running \"" << command << "\"";
}

}  // namespace util

}  // namespace window_manager
