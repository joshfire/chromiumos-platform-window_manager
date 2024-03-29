// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/pointer_position_watcher.h"

#include "window_manager/event_loop.h"
#include "window_manager/x11/x_connection.h"

namespace window_manager {

// How frequently should we query the pointer position, in milliseconds?
static const int kTimeoutMs = 200;

PointerPositionWatcher::PointerPositionWatcher(
    EventLoop* event_loop,
    XConnection* xconn,
    Closure* cb,
    bool watch_for_entering_target,
    const Rect& target_bounds)
    : event_loop_(event_loop),
      xconn_(xconn),
      cb_(cb),
      watch_for_entering_target_(watch_for_entering_target),
      target_bounds_(target_bounds),
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
  Point pointer_pos;
  if (!xconn_->QueryPointerPosition(&pointer_pos))
    return;

  // Bail out if we're not in the desired state yet.
  const bool in_target = target_bounds_.contains_point(pointer_pos);
  if ((watch_for_entering_target_ && !in_target) ||
      (!watch_for_entering_target_ && in_target))
    return;

  // Otherwise, run the callback.  Cancel the timeout first, in case the
  // callback deletes this object.
  CancelTimeoutIfActive();
  cb_->Run();
}

}  // namespace window_manager
