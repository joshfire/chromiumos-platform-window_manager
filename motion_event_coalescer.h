// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_MOTION_EVENT_COALESCER_H_
#define WINDOW_MANAGER_MOTION_EVENT_COALESCER_H_

#include "base/memory/scoped_ptr.h"
#include "window_manager/callback.h"
#include "window_manager/geometry.h"

namespace window_manager {

class EventLoop;

// Rate-limits how quickly motion events are processed by saving them as
// they're generated and then periodically invoking a callback (but only if
// new motion events have been received).
class MotionEventCoalescer {
 public:
  // The constructor takes ownership of |cb|.
  MotionEventCoalescer(EventLoop* event_loop, Closure* cb, int timeout_ms);
  ~MotionEventCoalescer();

  const Point& position() const { return position_; }
  int x() const { return position_.x; }
  int y() const { return position_.y; }

  void set_synchronous(bool synchronous) {
    synchronous_ = synchronous;
  }

  // Start or stop the timer.
  void Start();
  void Stop();

  // Is the timer currently running?
  bool IsRunning() {
    return timeout_id_ >= 0;
  }

  // Store a position.  This should be called in response to each motion
  // event.
  void StorePosition(const Point& pos);

 private:
  // Invoked by Stop() and by the destructor to remove the timer.  If
  // |maybe_run_callback| is true, the callback will be invoked one last
  // time if a new position has been received but not yet handled (the
  // destructor passes false here; running the callback may be dangerous if
  // parts of the owning class have already been destroyed).
  void StopInternal(bool maybe_run_callback);

  // Handle the timer firing.  Runs the callback if we have a queued
  // position.
  void HandleTimeout();

  EventLoop* event_loop_;  // not owned

  // Timeout ID, or -1 if the timeout isn't active.
  int timeout_id_;

  // Frequency for invoking the callback, in milliseconds.
  int timeout_ms_;

  // Have we received a position since the last time the callback was
  // invoked?
  bool have_queued_position_;

  // The most-recently-received position.
  Point position_;

  // Callback that gets periodically invoked when there's a new position to
  // handle.
  // TODO: When we're using a callback library that supports parameters, we
  // should just pass the position directly to the callback.
  scoped_ptr<Closure> cb_;

  // Should we just invoke the callback in response to each StorePosition()
  // call instead of using a timer?  Useful for tests.
  bool synchronous_;
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_MOTION_EVENT_COALESCER_H_
