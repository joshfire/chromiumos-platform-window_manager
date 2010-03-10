// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/event_loop.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/epoll.h>
#include <sys/timerfd.h>

extern "C" {
#include <X11/Xlib.h>
}

#include "base/eintr_wrapper.h"
#include "base/logging.h"
#include "window_manager/event_loop_subscriber.h"
#include "window_manager/x_connection.h"

namespace window_manager {

using std::make_pair;
using std::map;
using std::max;
using std::pop_heap;
using std::tr1::shared_ptr;

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

EventLoop::EventLoop(XConnection* xconn)
    : xconn_(xconn),
      subscriber_(NULL),
      exit_requested_(false),
      epoll_fd_(epoll_create(10)),  // argument is ignored since 2.6.8
      timerfd_supported_(IsTimerFdSupported()) {
  DCHECK(xconn_);
  CHECK(epoll_fd_ != -1) << "epoll_create: " << strerror(errno);
  if (!timerfd_supported_) {
    LOG(ERROR) << "timerfd doesn't work on this system (perhaps your kernel "
               << "doesn't support it).  EventLoop::Run() will crash if "
               << "called.";
  }
}

EventLoop::~EventLoop() {
  close(epoll_fd_);
}

void EventLoop::Run() {
  CHECK(subscriber_) << "SetSubscriber() must be called before the event loop "
                     << "is started";
  CHECK(timerfd_supported_) << "timerfd is unsupported -- look for earlier "
                            << "errors";

  int x11_fd = xconn_->GetConnectionFileDescriptor();
  LOG(INFO) << "X11 connection is on fd " << x11_fd;
  // TODO: Need to also use XAddConnectionWatch()?

  struct epoll_event x11_epoll_event;
  x11_epoll_event.events = EPOLLIN;
  x11_epoll_event.data.fd = x11_fd;
  CHECK(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, x11_fd, &x11_epoll_event) != -1)
      << strerror(errno);

  static const int kMaxEpollEvents = 256;
  struct epoll_event epoll_events[kMaxEpollEvents];

  while (true) {
    if (exit_requested_) {
      LOG(INFO) << "Exiting event loop";
      exit_requested_ = false;
      break;
    }

    const int num_events = HANDLE_EINTR(
        epoll_wait(epoll_fd_, epoll_events, kMaxEpollEvents, -1));
    CHECK(num_events != -1) << "epoll_wait: " << strerror(errno);

    for (int i = 0; i < num_events; ++i) {
      const int event_fd = epoll_events[i].data.fd;
      TimeoutMap::iterator it = timeouts_.find(event_fd);
      if (it == timeouts_.end())
        continue;

      if (epoll_events[i].events & EPOLLIN) {
        uint64_t num_expirations = 0;
        CHECK(HANDLE_EINTR(read(event_fd,
                                &num_expirations,
                                sizeof(num_expirations))) ==
              sizeof(num_expirations)) << "Short read on fd " << event_fd;
        // Make a copy of the callback in case it removes its own timeout.
        shared_ptr<Closure> cb = it->second;
        cb->Run();
      } else {
        LOG(WARNING) << "Got unexpected event mask for timer fd " << event_fd
                     << ": 0x" << std::hex << epoll_events[i].events;
      }
    }

    while (xconn_->IsEventPending()) {
      XEvent event;
      xconn_->GetNextEvent(&event);
      subscriber_->HandleEvent(&event);
    }
  }

  CHECK(epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, x11_fd, NULL) != -1)
      << strerror(errno);
}

int EventLoop::AddTimeout(Closure* cb,
                          int initial_timeout_ms,
                          int recurring_timeout_ms) {
  DCHECK(cb);
  DCHECK(initial_timeout_ms >= 0);
  DCHECK(recurring_timeout_ms >= 0);

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
  CHECK(timer_fd != 1) << "timerfd_create: " << strerror(errno);
  struct epoll_event event;
  event.events = EPOLLIN;
  event.data.fd = timer_fd;
  CHECK(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd, &event) != -1)
      << strerror(errno);

  struct itimerspec new_timer_spec;
  struct itimerspec old_timer_spec;
  FillTimerSpec(&new_timer_spec, initial_timeout_ms, recurring_timeout_ms);
  CHECK(timerfd_settime(timer_fd, 0, &new_timer_spec, &old_timer_spec) == 0)
      << strerror(errno);

  CHECK(timeouts_.insert(make_pair(timer_fd, shared_ptr<Closure>(cb))).second)
      << "timer fd " << timer_fd << " already exists";
  return timer_fd;
}

void EventLoop::RemoveTimeout(int id) {
  if (!timerfd_supported_)
    return;

  TimeoutMap::iterator it = timeouts_.find(id);
  CHECK(it != timeouts_.end())
      << "Got request to add nonexistent timeout with ID " << id;

  CHECK(epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, id, NULL) != -1) << strerror(errno);
  timeouts_.erase(it);
  CHECK(HANDLE_EINTR(close(id)) == 0) << strerror(errno);
}

void EventLoop::SuspendTimeout(int id) {
  if (!timerfd_supported_)
    return;

  struct itimerspec new_timer_spec;
  struct itimerspec old_timer_spec;
  memset(&new_timer_spec, 0, sizeof(new_timer_spec));
  CHECK(timerfd_settime(id, 0, &new_timer_spec, &old_timer_spec) == 0)
      << strerror(errno);
}

void EventLoop::ResetTimeout(int id,
                             int initial_timeout_ms,
                             int recurring_timeout_ms) {
  if (!timerfd_supported_)
    return;

  struct itimerspec new_timer_spec;
  struct itimerspec old_timer_spec;
  FillTimerSpec(&new_timer_spec, initial_timeout_ms, recurring_timeout_ms);
  CHECK(timerfd_settime(id, 0, &new_timer_spec, &old_timer_spec) == 0)
      << strerror(errno);
}

// static
bool EventLoop::IsTimerFdSupported() {
  // Try creating a timeout (which we'll throw away immediately) to test
  // whether the kernel that we're running on suports timerfd.
  int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (timer_fd == -1) {
    LOG(ERROR) << "timerfd_create: " << strerror(errno);
    return false;
  } else {
    CHECK(HANDLE_EINTR(close(timer_fd)) != -1) << strerror(errno);
    return true;
  }
}

}  // namespace window_manager
