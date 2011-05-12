// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_EVENT_LOOP_H_
#define WINDOW_MANAGER_EVENT_LOOP_H_

#include <map>
#include <set>
#include <tr1/memory>
#include <vector>

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "window_manager/callback.h"

namespace window_manager {

// EventLoop provides an interface for fetching X events and setting
// timeouts.
class EventLoop {
 public:
  EventLoop();
  ~EventLoop();

  // Get the number of current-registered timeouts.  Used for testing.
  int num_timeouts() const { return timeout_fds_.size(); }

  // Loop until Exit() is called, waiting for FDs to become readable or
  // timeouts to fire.
  void Run();

  // Exit the loop the next time we're about to wait for FDs or timeouts.
  void Exit() { exit_requested_ = true; }

  // Start watching a file descriptor, invoking a callback when it becomes
  // readable.  Takes ownership of |cb|, which must be a repeatable
  // (non-self-deleting) callback.
  void AddFileDescriptor(int fd, Closure* cb);

  // Stop watching a file descriptor.
  void RemoveFileDescriptor(int fd);

  // Register a callback that will always be invoked before we wait for
  // changes to file descriptors.  This is needed for e.g. Xlib, which can
  // sidestep us and read its own FD at inopportune times to add events to
  // its internal queue.  For example, a callback can send a request to the
  // X server that generates a response.  While reading from the FD to find
  // the response, Xlib will store any intervening events in its queue.  We
  // need to make sure that those events are handled before we wait on the
  // (now non-readable) Xlib FD our next time through the loop.
  void AddPrePollCallback(Closure* cb);

  // Run |cb| in |initial_timeout_ms| milliseconds, returning a
  // non-negative ID that can be used to refer the timeout later.  A
  // timeout of 0 will result in the callback being invoked in the next
  // iteration of the event loop.
  //
  // Takes ownership of |cb|, which must be a repeatable
  // (non-self-deleting) callback.  If |recurring_timeout_ms| is non-zero,
  // the timeout will be repeated every |recurring_timeout_ms| milliseconds
  // after the initial run; otherwise it will only be run once.  Note that
  // even non-recurring timeouts must be removed using RemoveTimeout() for
  // their resources to be freed.
  int AddTimeout(Closure* cb,
                 int64_t initial_timeout_ms,
                 int64_t recurring_timeout_ms);

  // Remove a timeout.  It is safe to call this from within the callback of
  // the timeout that's being removed.  Crashes if the timeout doesn't exist.
  void RemoveTimeout(int id);

  // If the variable pointed at by |id| contains a timeout, remove the timeout
  // and set the variable to -1.
  void RemoveTimeoutIfSet(int* id) {
    if (*id >= 0) {
      RemoveTimeout(*id);
      *id = -1;
    }
  }

  // Run |cb| once immediately after control is returned to the event loop.
  //
  // Takes ownership of |cb|, which must be a repeatable
  // (non-self-deleting) callback.  Note that other not-yet-run tasks
  // previously posted via PostTask() will be run before this one.
  void PostTask(Closure* cb);

  // Suspend a previously-registered timeout.  Use ResetTimeout() to
  // unsuspend it.
  void SuspendTimeout(int fd);

  // Modify a previously-registered timeout.  The timeout arguments are
  // interpreted in the same manner as in AddTimeout().
  void ResetTimeout(int id,
                    int64_t initial_timeout_ms,
                    int64_t recurring_timeout_ms);

  // Does the system that we're currently running on support the latest
  // timerfd interface (the one with timerfd_create())?  This was
  // introduced in Linux 2.6.25 and glibc 2.8.  This is static so that
  // EventLoopTest can skip out early on older systems.
  static bool IsTimerFdSupported();

  // Run an already-registered timeout.  This should only be used by
  // testing code that wants to manually run a timeout's callback.
  void RunTimeoutForTesting(int id);

 private:
  typedef std::vector<std::tr1::shared_ptr<Closure> > CallbackVector;
  typedef std::map<int, std::tr1::shared_ptr<Closure> > FdCallbackMap;

  // Run all callbacks from |posted_tasks_| and clear the vector.
  // If the existing callbacks post additional tasks, they will be run as
  // well.
  void RunAllPostedTasks();

  // Should we exit the loop?
  bool exit_requested_;

  // File descriptor that we're using for epoll_wait().
  int epoll_fd_;

  // Map from file descriptors to the corresponding callbacks.
  FdCallbackMap callbacks_;

  // Callbacks that get called before we poll.  See AddPrePollCallback()
  // for details.
  CallbackVector pre_poll_callbacks_;

  // Callbacks that have been posted via PostTask() to be run immediately
  // after control is returned to the event loop, in the order in which
  // they'll be run.
  CallbackVector posted_tasks_;

  // timerfd file descriptors that we've created (a subset of the keys in
  // |callbacks_|.
  std::set<int> timeout_fds_;

  // Does the kernel support timerfd?  If it doesn't, timeout-related calls
  // are no-ops, and we'll crash if Run() is ever called.
  bool timerfd_supported_;

  // Callbacks that have been scheduled to run during the current poll
  // cycle.  If two timeouts A and B fire during the same cycle and A's
  // callback happens to get executed first and removes B, we want avoid
  // running B's callback afterwards.  We store the set here so that
  // RemoveFileDescriptor() can remove items from it.
  std::set<std::tr1::shared_ptr<Closure> > callbacks_to_run_;

  DISALLOW_COPY_AND_ASSIGN(EventLoop);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_EVENT_LOOP_H_
