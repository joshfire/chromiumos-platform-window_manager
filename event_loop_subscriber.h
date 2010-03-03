// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_EVENT_LOOP_SUBSCRIBER_H_
#define WINDOW_MANAGER_EVENT_LOOP_SUBSCRIBER_H_

extern "C" {
#include <X11/Xlib.h>
}

#include "base/basictypes.h"

namespace window_manager {

// Interface for classes that receive events from an EventLoop (currently,
// the WindowManager class).
class EventLoopSubscriber {
 public:
  EventLoopSubscriber() {}
  virtual ~EventLoopSubscriber() {}

  // Handle an event from the X server.
  virtual void HandleEvent(XEvent* event) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(EventLoopSubscriber);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_EVENT_LOOP_SUBSCRIBER_H_
