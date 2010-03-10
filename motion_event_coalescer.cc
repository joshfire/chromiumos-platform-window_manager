// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/motion_event_coalescer.h"

#include "base/logging.h"
#include "window_manager/event_loop.h"

namespace window_manager {

MotionEventCoalescer::MotionEventCoalescer(EventLoop* event_loop,
                                           Closure* cb,
                                           int timeout_ms)
    : event_loop_(event_loop),
      timeout_id_(-1),
      timeout_ms_(timeout_ms),
      have_queued_position_(false),
      x_(-1),
      y_(-1),
      cb_(cb),
      synchronous_(false) {
  CHECK(cb);
  CHECK(timeout_ms > 0);
}

MotionEventCoalescer::~MotionEventCoalescer() {
  if (IsRunning())
    StopInternal(false);
}

void MotionEventCoalescer::Start() {
  if (timeout_id_ >= 0) {
    LOG(WARNING) << "Ignoring request to start coalescer while timer "
                 << "is already running";
    return;
  }
  if (!synchronous_) {
    timeout_id_ = event_loop_->AddTimeout(
        NewPermanentCallback(this, &MotionEventCoalescer::HandleTimeout),
        0, timeout_ms_);
  }
  have_queued_position_ = false;
  x_ = -1;
  y_ = -1;
}

void MotionEventCoalescer::Stop() {
  if (!synchronous_)
    StopInternal(true);
}

void MotionEventCoalescer::StorePosition(int x, int y) {
  if (x == x_ && y == y_)
    return;
  x_ = x;
  y_ = y;
  have_queued_position_ = true;
  if (synchronous_)
    HandleTimeout();
}

void MotionEventCoalescer::StopInternal(bool maybe_run_callback) {
  if (timeout_id_ < 0) {
    LOG(WARNING) << "Ignoring request to stop coalescer while timer "
                 << "isn't running";
    return;
  }
  event_loop_->RemoveTimeout(timeout_id_);
  timeout_id_ = -1;

  // Invoke the handler one last time to catch any events that came in
  // after the final run.
  if (maybe_run_callback)
    HandleTimeout();
}

void MotionEventCoalescer::HandleTimeout() {
  if (have_queued_position_) {
    cb_->Run();
    have_queued_position_ = false;
  }
}

}  // namespace window_manager
