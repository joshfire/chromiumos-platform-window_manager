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
#include "chromeos/obsolete_logging.h"
#include "window_manager/event_loop_subscriber.h"
#include "window_manager/x_connection.h"

namespace window_manager {

using chromeos::Closure;
using std::make_pair;
using std::map;
using std::max;
using std::pop_heap;
using std::tr1::shared_ptr;

EventLoop::EventLoop(XConnection* xconn)
    : xconn_(xconn),
      subscriber_(NULL),
      exit_requested_(false),
      epoll_fd_(epoll_create(10)) {  // argument is ignored since 2.6.8
  DCHECK(xconn_);
  CHECK(epoll_fd_ != -1) << "epoll_create: " << strerror(errno);
}

EventLoop::~EventLoop() {
  close(epoll_fd_);
}

void EventLoop::Run() {
  CHECK(subscriber_) << "SetSubscriber() must be called before the event loop "
                     << "is started";

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
    CHECK_NE(num_events, -1) << "epoll_wait: " << strerror(errno);

    for (int i = 0; i < num_events; ++i) {
      const int event_fd = epoll_events[i].data.fd;
      TimeoutMap::iterator it = timeouts_.find(event_fd);
      if (it == timeouts_.end())
        continue;

      if (epoll_events[i].events & EPOLLIN) {
        uint64_t num_expirations = 0;
        CHECK_EQ(HANDLE_EINTR(read(event_fd,
                                   &num_expirations,
                                   sizeof(num_expirations))),
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

int EventLoop::AddTimeout(Closure* cb, bool recurring, int timeout_ms) {
  DCHECK(cb);
  DCHECK(timeout_ms >= 0);

  // Use a monotonically-increasing clock -- we don't want to be affected
  // by changes to the system time.
  const int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  struct epoll_event event;
  event.events = EPOLLIN;
  event.data.fd = timer_fd;
  CHECK(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd, &event) != -1)
      << strerror(errno);

  struct itimerspec new_timer_spec;
  memset(&new_timer_spec, 0, sizeof(new_timer_spec));
  new_timer_spec.it_value.tv_sec = timeout_ms / 1000;
  new_timer_spec.it_value.tv_nsec = (timeout_ms % 1000) * 1000000;
  if (recurring) {
    new_timer_spec.it_interval.tv_sec = new_timer_spec.it_value.tv_sec;
    new_timer_spec.it_interval.tv_nsec = new_timer_spec.it_value.tv_nsec;
  }
  struct itimerspec old_timer_spec;
  timerfd_settime(timer_fd, 0, &new_timer_spec, &old_timer_spec);

  CHECK(timeouts_.insert(make_pair(timer_fd, shared_ptr<Closure>(cb))).second)
      << "timer fd " << timer_fd << " already exists";
  return timer_fd;
}

void EventLoop::RemoveTimeout(int id) {
  TimeoutMap::iterator it = timeouts_.find(id);
  CHECK(it != timeouts_.end())
      << "Got request to add nonexistent timeout with ID " << id;

  CHECK(epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, id, NULL) != -1) << strerror(errno);
  timeouts_.erase(it);
  CHECK_EQ(HANDLE_EINTR(close(id)), 0) << strerror(errno);
}

}  // namespace window_manager
