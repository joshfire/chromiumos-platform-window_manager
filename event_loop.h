// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_EVENT_LOOP_H_
#define WINDOW_MANAGER_EVENT_LOOP_H_

#include <map>
#include <tr1/memory>

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "chromeos/callback.h"
#include "window_manager/x_types.h"

namespace window_manager {

class EventLoopSubscriber;
class XConnection;

// EventLoop provides an interface for fetching X events and setting
// timeouts.
class EventLoop {
 public:
  explicit EventLoop(XConnection* xconn);
  ~EventLoop();

  XConnection* xconn() { return xconn_; }

  // Specify the EventLoopSubscriber object that should receive X events.
  void SetSubscriber(EventLoopSubscriber* subscriber) {
    subscriber_ = subscriber;
  }

  // Loop until Exit() is called, waiting for events from the X server and
  // for timeouts.  SetWindowManager() must be called before this method to
  // set a destination for X events.
  void Run();

  // Exit the loop the next time we're about to wait for events or
  // timeouts.
  void Exit() { exit_requested_ = true; }

  // Run 'cb' in 'initial_timeout_ms' milliseconds, returning a
  // non-negative ID that can be used to refer the timeout later.  A
  // timeout of 0 will result in the callback being invoked in the next
  // iteration of the event loop.
  //
  // Takes ownership of 'cb', which must be a repeatable
  // (non-self-deleting) callback.  If 'recurring_timeout_ms' is non-zero,
  // the timeout will be repeated every 'recurring_timeout_ms' milliseconds
  // after the initial run; otherwise it will only be run once.  Note that
  // even non-recurring timeouts must be removed using RemoveTimeout() for
  // their resources to be freed.
  int AddTimeout(chromeos::Closure* cb,
                 int initial_timeout_ms,
                 int recurring_timeout_ms);

  // Remove a timeout.  It is safe to call this from within the callback of
  // the timeout that's being removed.
  void RemoveTimeout(int id);

  // Suspend a previously-registered timeout.  Use ResetTimeout() to
  // unsuspend it.
  void SuspendTimeout(int fd);

  // Modify a previously-registered timeout.  The timeout arguments are
  // interpreted in the same manner as in AddTimeout().
  void ResetTimeout(int id, int initial_timeout_ms, int recurring_timeout_ms);

 private:
  typedef std::map<int, std::tr1::shared_ptr<chromeos::Closure> > TimeoutMap;

  XConnection* xconn_;  // not owned

  // Object that will receive X events.
  EventLoopSubscriber* subscriber_;  // not owned

  // Should we exit the loop?
  bool exit_requested_;

  // File descriptor that we're using for epoll_wait().
  int epoll_fd_;

  // Map from timerfd file descriptors to the corresponding callbacks.
  TimeoutMap timeouts_;

  DISALLOW_COPY_AND_ASSIGN(EventLoop);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_EVENT_LOOP_H_
