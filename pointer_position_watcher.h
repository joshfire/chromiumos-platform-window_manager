// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_POINTER_POSITION_WATCHER_H_
#define WINDOW_MANAGER_POINTER_POSITION_WATCHER_H_

#include "base/memory/scoped_ptr.h"
#include "window_manager/callback.h"
#include "window_manager/geometry.h"

namespace window_manager {

class EventLoop;
class XConnection;

// This class periodically queries the mouse pointer's position and invokes
// a callback once the pointer has moved into or out of a target rectangle.
//
// This is primarily useful for:
// a) avoiding race conditions in cases where we want to open a new window
//    under the pointer and then do something when the pointer leaves the
//    window -- it's possible that the pointer will have already been moved
//    away by the time that window is created
// b) getting notified when the pointer enters or leaves a region without
//    creating a window that will steal events from windows underneath it
//
// With that being said, repeatedly waking up to poll the X server over
// long periods of time is a bad idea from a power consumption perspective,
// so this should only be used in cases where the user is likely to
// enter/leave the target region soon.
class PointerPositionWatcher {
 public:
  // The constructor takes ownership of |cb|.
  PointerPositionWatcher(
      EventLoop* event_loop,
      XConnection* xconn,
      Closure* cb,
      bool watch_for_entering_target,  // as opposed to leaving it
      const Rect& target_bounds);
  ~PointerPositionWatcher();

  // Useful for testing.
  int timeout_id() const { return timeout_id_; }

  // Invoke RunCallbackIfConditionIsSatisfied() and remove the current
  // timeout if needed.
  void TriggerTimeout();

 private:
  // If |timeout_id_| is set, clear it and remove the timeout.
  void CancelTimeoutIfActive();

  // Check the pointer's position, running the callback and removing the
  // timeout if the condition has been satisfied.
  void HandleTimeout();

  EventLoop* event_loop_;  // not owned
  XConnection* xconn_;     // not owned

  // Callback that gets invoked when the pointer enters/exits the target
  // rectangle.
  scoped_ptr<Closure> cb_;

  // Should we watch for the pointer entering the target rectangle, as
  // opposed to leaving it?
  bool watch_for_entering_target_;

  // Target rectangle.
  Rect target_bounds_;

  // Timeout ID, or -1 if the timeout isn't active.
  int timeout_id_;
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_MOTION_POINTER_POSITION_WATCHER_H_
