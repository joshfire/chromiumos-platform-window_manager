// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_PROFILER_DATA_H_
#define WINDOW_MANAGER_PROFILER_DATA_H_

#include <stdint.h>

namespace window_manager {
namespace profiler {

enum MarkFlag {
  MARK_FLAG_TAP = 0,
  MARK_FLAG_BEGIN,
  MARK_FLAG_END
};

struct Symbol {
  char name[50];
};

struct Sample {
  int16_t symbol_id;
  int16_t flag;  // MarkFlag
  int64_t time;
};

}  // namespace profiler
}  // namespace window_manager

#endif  // WINDOW_MANAGER_PROFILER_DATA_H_
