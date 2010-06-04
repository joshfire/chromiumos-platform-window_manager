// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "window_manager/callback.h"
#include "window_manager/event_loop.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/test_lib.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using window_manager::EventLoop;

namespace window_manager {

class EventLoopTest : public ::testing::Test {};

// Helper class that receives X events and uses them to manipulate the
// event loop.  See the comment near the end of the "Basic" test for
// details about what's going on here.
class TestEventLoopSubscriber {
 public:
  TestEventLoopSubscriber(EventLoop* event_loop, MockXConnection* xconn)
      : event_loop_(event_loop),
        xconn_(xconn),
        timeout_id_(-1),
        num_times_timeout_invoked_(0) {
  }

  void ProcessPendingEvents() {
    while (xconn_->IsEventPending()) {
      XEvent event;
      xconn_->GetNextEvent(&event);

      switch (event.type) {
        case ButtonPress: {
          // Make HandleTimeout() get run every five milliseconds.
          timeout_id_ = event_loop_->AddTimeout(
              NewPermanentCallback(
                  this, &TestEventLoopSubscriber::HandleTimeout),
              5, 5);
          break;
        }
        case ButtonRelease: {
          event_loop_->Exit();
          break;
        }
        default:
          CHECK(false) << "Got unexpected event of type " << event.type;
      }
    }
  }

 private:
  void HandleTimeout() {
    num_times_timeout_invoked_++;
    if (num_times_timeout_invoked_ > 1) {
      // The second time that we're called, remove our timeout and put a
      // button release event on the queue.
      event_loop_->RemoveTimeout(timeout_id_);
      XEvent event;
      memset(&event, 0, sizeof(event));
      event.type = ButtonRelease;
      // Intentionally don't make the FD readable here, to simulate the
      // case where Xlib pulls an event into its queue before we see that
      // it's readable.
      xconn_->AppendEventToQueue(event, false);  // write_to_fd=false
    }
  }

  EventLoop* event_loop_;   // not owned
  MockXConnection* xconn_;  // not owned

  // ID for a recurring timeout that invokes HandleTimeout().
  int timeout_id_;

  // Number of times that HandleTimeout() has been called.
  int num_times_timeout_invoked_;
};

// Struct passed in to RemoveTimeout() (the protobuf callback library only
// supports binding two arguments, but we need to pass in three, so we wrap
// them in a struct).
struct RemoveTimeoutData {
  RemoveTimeoutData(EventLoop* event_loop)
      : event_loop(event_loop),
        timeout_id_to_remove(0),
        called(false) {
  }
  EventLoop* event_loop;
  int timeout_id_to_remove;
  bool called;
};

// Helper function for the RemoveScheduledTimeout test.
static void RemoveTimeout(RemoveTimeoutData* data) {
  DCHECK(data);
  data->event_loop->RemoveTimeout(data->timeout_id_to_remove);
  data->called = true;
  data->event_loop->Exit();
}

// Perform a somewhat-tricky test of the event loop.
TEST_F(EventLoopTest, Basic) {
  EventLoop event_loop;
  MockXConnection xconn;
  TestEventLoopSubscriber subscriber(&event_loop, &xconn);
  event_loop.AddFileDescriptor(
      xconn.GetConnectionFileDescriptor(),
      NewPermanentCallback(
          &subscriber, &TestEventLoopSubscriber::ProcessPendingEvents));
  event_loop.AddPrePollCallback(
      NewPermanentCallback(
          &subscriber, &TestEventLoopSubscriber::ProcessPendingEvents));

  // Add a button press event to the X connection's event queue.
  XEvent event;
  memset(&event, 0, sizeof(event));
  event.type = ButtonPress;
  xconn.AppendEventToQueue(event, true);  // write_to_fd=true

  // Now start the event loop.  The subscriber's button press handler will
  // register a recurring timeout with the event loop.  The second time
  // that the timeout is invoked, it will enqueue a button release event.
  // The button release handler tells the event loop to exit.  If all goes
  // well, we should return in about 10 milliseconds!  If it doesn't, we
  // will hang forever. :-(
  event_loop.Run();
}

// Test that if two timeouts are scheduled in the same poll cycle and one
// of them removes the other, the second one doesn't get invoked.
TEST_F(EventLoopTest, RemoveScheduledTimeout) {
  EventLoop event_loop;
  RemoveTimeoutData first_timeout_data(&event_loop);
  RemoveTimeoutData second_timeout_data(&event_loop);

  // We don't know which timeout's callback will be invoked first, so we
  // make each remove the other.
  second_timeout_data.timeout_id_to_remove = event_loop.AddTimeout(
      NewPermanentCallback(RemoveTimeout, &first_timeout_data), 0, 0);
  first_timeout_data.timeout_id_to_remove = event_loop.AddTimeout(
      NewPermanentCallback(RemoveTimeout, &second_timeout_data), 0, 0);
  event_loop.Run();

  // At the end, exactly one of the callbacks should've been called.
  EXPECT_TRUE(first_timeout_data.called ^ second_timeout_data.called)
      << "first=" << first_timeout_data.called
      << " second=" << second_timeout_data.called;
}

}  // namespace window_manager

int main(int argc, char** argv) {
  if (!EventLoop::IsTimerFdSupported()) {
    LOG(ERROR) << "timerfd isn't supported on this system; skipping tests";
    return 0;
  }
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
