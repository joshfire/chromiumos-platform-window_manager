// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/event_loop.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <algorithm>
#include <vector>

extern "C" {
#include <X11/Xlib.h>
}

#include "base/eintr_wrapper.h"
#include "base/logging.h"

using std::hex;
using std::make_pair;
using std::set;
using std::tr1::shared_ptr;
using std::vector;

namespace window_manager {

static void FillTimerSpec(struct itimerspec* spec,
                          int initial_timeout_ms,
                          int recurring_timeout_ms) {
  DCHECK(spec);
  memset(spec, 0, sizeof(*spec));
  spec->it_value.tv_sec = initial_timeout_ms / 1000;
  // timerfd interprets 0 values as disabling the timer; set it to run in
  // one nanosecond instead.
  spec->it_value.tv_nsec = initial_timeout_ms ?
      (initial_timeout_ms % 1000) * 1000000 :
      1;
  spec->it_interval.tv_sec = recurring_timeout_ms / 1000;
  spec->it_interval.tv_nsec = (recurring_timeout_ms % 1000) * 1000000;
}

EventLoop::EventLoop()
    : exit_requested_(false),
      epoll_fd_(-1),
      timerfd_supported_(IsTimerFdSupported()) {
  epoll_fd_ = epoll_create(10);  // argument is ignored since 2.6.8
  PCHECK(epoll_fd_ != -1) << "epoll_create() failed";
  if (!timerfd_supported_) {
    LOG(ERROR) << "timerfd doesn't work on this system (perhaps your kernel "
               << "doesn't support it).  EventLoop::Run() will crash if "
               << "called.";
  }
}

EventLoop::~EventLoop() {
  close(epoll_fd_);
  for (set<int>::iterator it = timeout_fds_.begin();
       it != timeout_fds_.end(); ++it) {
    PCHECK(HANDLE_EINTR(close(*it)) == 0);
  }
}

void EventLoop::Run() {
  CHECK(timerfd_supported_)
      << "timerfd is unsupported -- look for earlier errors";

  static const int kMaxEpollEvents = 256;
  struct epoll_event epoll_events[kMaxEpollEvents];
  vector<shared_ptr<Closure> > callbacks_to_run;
  callbacks_to_run.reserve(kMaxEpollEvents);

  while (true) {
    for (CallbackVector::iterator it = pre_poll_callbacks_.begin();
         it != pre_poll_callbacks_.end(); ++it) {
      (*it)->Run();
    }

    if (exit_requested_) {
      LOG(INFO) << "Exiting event loop as requested";
      exit_requested_ = false;
      break;
    }

    CHECK(!callbacks_.empty())
        << "No event sources for event loop; would sleep forever";
    const int num_events = HANDLE_EINTR(
        epoll_wait(epoll_fd_, epoll_events, kMaxEpollEvents, -1));
    PCHECK(num_events != -1) << "epoll_wait() failed";

    for (int i = 0; i < num_events; ++i) {
      const int event_fd = epoll_events[i].data.fd;
      FdCallbackMap::iterator it = callbacks_.find(event_fd);
      CHECK(it != callbacks_.end()) << "Got event for unknown fd " << event_fd;

      if (!(epoll_events[i].events & EPOLLIN)) {
        LOG(WARNING) << "Got unexpected event mask for fd " << event_fd
                     << ": 0x" << hex << epoll_events[i].events;
        continue;
      }

      // We have to read from timer fds to reset their ready state.
      if (timeout_fds_.count(event_fd) > 0) {
        uint64_t num_expirations = 0;
        CHECK(HANDLE_EINTR(read(event_fd,
                                &num_expirations,
                                sizeof(num_expirations))) ==
              sizeof(num_expirations)) << "Short read on fd " << event_fd;
      }

      // Save all the callbacks so we can run them later -- they may add or
      // remove FDs, and we don't want things to be changed underneath us.
      callbacks_to_run.push_back(it->second);
    }

    for (vector<shared_ptr<Closure> >::iterator it = callbacks_to_run.begin();
         it != callbacks_to_run.end(); ++it) {
      (*it)->Run();
    }
    callbacks_to_run.clear();
  }
}

void EventLoop::AddFileDescriptor(int fd, Closure* cb) {
  struct epoll_event epoll_event;
  epoll_event.events = EPOLLIN;
  epoll_event.data.fd = fd;
  PCHECK(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &epoll_event) != -1);
  CHECK(callbacks_.insert(make_pair(fd, shared_ptr<Closure>(cb))).second)
      << "fd " << fd << " is already being watched";
}

void EventLoop::RemoveFileDescriptor(int fd) {
  FdCallbackMap::iterator it = callbacks_.find(fd);
  CHECK(it != callbacks_.end()) << "Got request to remove unknown fd " << fd;
  callbacks_.erase(it);
  PCHECK(epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, NULL) != -1);
}

void EventLoop::AddPrePollCallback(Closure* cb) {
  pre_poll_callbacks_.push_back(shared_ptr<Closure>(cb));
}

int EventLoop::AddTimeout(Closure* cb,
                          int initial_timeout_ms,
                          int recurring_timeout_ms) {
  DCHECK(cb);
  DCHECK_GE(initial_timeout_ms, 0);
  DCHECK_GE(recurring_timeout_ms, 0);

  if (!timerfd_supported_) {
    // If we previously established that timerfd doesn't work on this
    // system, just return an arbitrary fake descriptor -- we'll crash
    // before we'd try to use it in Run().
    delete cb;
    static int next_timer_fd = 0;
    return next_timer_fd++;
  }

  // Use a monotonically-increasing clock -- we don't want to be affected
  // by changes to the system time.
  const int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  PCHECK(timer_fd != -1) << "timerfd_create() failed";

  AddFileDescriptor(timer_fd, cb);
  CHECK(timeout_fds_.insert(timer_fd).second);

  struct itimerspec new_timer_spec;
  struct itimerspec old_timer_spec;
  FillTimerSpec(&new_timer_spec, initial_timeout_ms, recurring_timeout_ms);
  PCHECK(timerfd_settime(timer_fd, 0, &new_timer_spec, &old_timer_spec) == 0);
  return timer_fd;
}

void EventLoop::RemoveTimeout(int id) {
  if (!timerfd_supported_)
    return;

  RemoveFileDescriptor(id);
  CHECK(timeout_fds_.erase(id) == 1);
  PCHECK(HANDLE_EINTR(close(id)) == 0);
}

void EventLoop::SuspendTimeout(int id) {
  if (!timerfd_supported_)
    return;

  struct itimerspec new_timer_spec;
  struct itimerspec old_timer_spec;
  memset(&new_timer_spec, 0, sizeof(new_timer_spec));
  PCHECK(timerfd_settime(id, 0, &new_timer_spec, &old_timer_spec) == 0);
}

void EventLoop::ResetTimeout(int id,
                             int initial_timeout_ms,
                             int recurring_timeout_ms) {
  if (!timerfd_supported_)
    return;

  struct itimerspec new_timer_spec;
  struct itimerspec old_timer_spec;
  FillTimerSpec(&new_timer_spec, initial_timeout_ms, recurring_timeout_ms);
  PCHECK(timerfd_settime(id, 0, &new_timer_spec, &old_timer_spec) == 0);
}

// static
bool EventLoop::IsTimerFdSupported() {
  // Try creating a timeout (which we'll throw away immediately) to test
  // whether the kernel that we're running on suports timerfd.
  int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (timer_fd == -1) {
    PLOG(ERROR) << "timerfd_create() failed";
    return false;
  } else {
    PCHECK(HANDLE_EINTR(close(timer_fd)) != -1);
    return true;
  }
}

}  // namespace window_manager
