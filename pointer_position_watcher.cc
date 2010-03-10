// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/pointer_position_watcher.h"

#include "window_manager/event_loop.h"
#include "window_manager/x_connection.h"

namespace window_manager {

// How frequently should we query the pointer position, in milliseconds?
static const int kTimeoutMs = 200;

PointerPositionWatcher::PointerPositionWatcher(
    EventLoop* event_loop,
    XConnection* xconn,
    Closure* cb,
    bool watch_for_entering_target,
    int target_x, int target_y, int target_width, int target_height)
    : event_loop_(event_loop),
      xconn_(xconn),
      cb_(cb),
      watch_for_entering_target_(watch_for_entering_target),
      target_x_(target_x),
      target_y_(target_y),
      target_width_(target_width),
      target_height_(target_height),
      timeout_id_(-1) {
  DCHECK(event_loop);
  DCHECK(xconn);
  DCHECK(cb);
  timeout_id_ =
      event_loop_->AddTimeout(
          NewPermanentCallback(this, &PointerPositionWatcher::HandleTimeout),
          0, kTimeoutMs);  // recurring=true
}

PointerPositionWatcher::~PointerPositionWatcher() {
  CancelTimeoutIfActive();
}

void PointerPositionWatcher::TriggerTimeout() {
  HandleTimeout();
}

void PointerPositionWatcher::CancelTimeoutIfActive() {
  if (timeout_id_ >= 0) {
    event_loop_->RemoveTimeout(timeout_id_);
    timeout_id_ = -1;
  }
}

void PointerPositionWatcher::HandleTimeout() {
  int pointer_x = 0, pointer_y = 0;
  if (!xconn_->QueryPointerPosition(&pointer_x, &pointer_y))
    return;

  // Bail out if we're not in the desired state yet.
  bool in_target = pointer_x >= target_x_ &&
                   pointer_x < target_x_ + target_width_ &&
                   pointer_y >= target_y_ &&
                   pointer_y < target_y_ + target_height_;
  if ((watch_for_entering_target_ && !in_target) ||
      (!watch_for_entering_target_ && in_target))
    return;

  // Otherwise, run the callback.  Cancel the timeout first, in case the
  // callback deletes this object.
  CancelTimeoutIfActive();
  cb_->Run();
}

}  // namespace window_manager
