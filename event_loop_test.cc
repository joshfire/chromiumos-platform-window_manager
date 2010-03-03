// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "chromeos/callback.h"
#include "window_manager/event_loop.h"
#include "window_manager/event_loop_subscriber.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/test_lib.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

namespace window_manager {

using chromeos::NewPermanentCallback;

class EventLoopTest : public ::testing::Test {};

// Helper class that receives X events and uses them to manipulate the
// event loop.  See the comment near the end of the "Basic" test for
// details about what's going on here.
class TestEventLoopSubscriber : public EventLoopSubscriber {
 public:
  TestEventLoopSubscriber(EventLoop* event_loop, MockXConnection* xconn)
      : event_loop_(event_loop),
        xconn_(xconn),
        timeout_id_(-1),
        num_times_timeout_invoked_(0) {
    event_loop->SetSubscriber(this);
  }

  // Begin EventLoopSubscriber implementation.
  void HandleEvent(XEvent* event) {
    switch (event->type) {
      case ButtonPress: {
        // Make HandleTimeout() get run every five milliseconds.
        timeout_id_ = event_loop_->AddTimeout(
            NewPermanentCallback(this, &TestEventLoopSubscriber::HandleTimeout),
            true, 5);  // recurring=true
        break;
      }
      case ButtonRelease: {
        event_loop_->Exit();
        break;
      }
      default:
        CHECK(false) << "Got unexpected event of type " << event->type;
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
      xconn_->AppendEventToQueue(event);
    }
  }

  EventLoop* event_loop_;   // not owned
  MockXConnection* xconn_;  // not owned

  // ID for a recurring timeout that invokes HandleTimeout().
  int timeout_id_;

  // Number of times that HandleTimeout() has been called.
  int num_times_timeout_invoked_;
};

// Perform a somewhat-tricky test of the event loop.
TEST_F(EventLoopTest, Basic) {
  MockXConnection xconn;
  EventLoop event_loop(&xconn);
  TestEventLoopSubscriber subscriber(&event_loop, &xconn);

  // Add a button press event to the X connection's event queue.
  XEvent event;
  memset(&event, 0, sizeof(event));
  event.type = ButtonPress;
  xconn.AppendEventToQueue(event);

  // Now start the event loop.  The subscriber's button press handler will
  // register a recurring timeout with the event loop.  The second time
  // that the timeout is invoked, it will enqueue a button release event.
  // The button release handler tells the event loop to exit.  If all goes
  // well, we should return in about 10 milliseconds!  If it doesn't, we
  // will hang forever. :-(
  event_loop.Run();
}

}  // namespace window_manager

int main(int argc, char **argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
