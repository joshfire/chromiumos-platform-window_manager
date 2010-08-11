// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "window_manager/callback.h"
#include "window_manager/event_loop.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/test_lib.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::vector;
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

// Data used for the RemoveScheduledTimeout test.
struct RemoveScheduledTimeoutData {
  RemoveScheduledTimeoutData(EventLoop* event_loop)
      : event_loop(event_loop),
        timeout_id_to_remove(0),
        called(false) {
  }

  void RemoveTimeout() {
    event_loop->RemoveTimeout(timeout_id_to_remove);
    called = true;
    event_loop->Exit();
  }

  EventLoop* event_loop;
  int timeout_id_to_remove;
  bool called;
};

// Data used for the PostTask test.
struct PostTaskData {
  PostTaskData(EventLoop* event_loop)
      : event_loop(event_loop),
        prepoll_called(false),
        timeout_called(false) {
    event_loop->AddPrePollCallback(
        NewPermanentCallback(this, &PostTaskData::HandlePrePollCallback));
    event_loop->AddTimeout(
        NewPermanentCallback(this, &PostTaskData::HandleTimeout), 0, 0);
    event_loop->AddTimeout(
        NewPermanentCallback(this, &PostTaskData::HandleTimeout), 0, 0);
  }

  // These values represent the various Handle*() methods defined below.
  // We use them to record the order in which the callbacks were invoked.
  enum CallbackType {
    PRE_POLL_CALLBACK = 0,
    TIMEOUT,
    TASK_PRE_POLL,
    TASK_TIMEOUT_A,
    TASK_TIMEOUT_B,
    TASK_REPOSTED_A,
    TASK_REPOSTED_B,
  };

  // Post HandlePrePollTask() the first time and make the event loop exit
  // the second.
  void HandlePrePollCallback() {
    called_types.push_back(PRE_POLL_CALLBACK);
    if (prepoll_called) {
      event_loop->Exit();
      return;
    }
    event_loop->PostTask(
        NewPermanentCallback(this, &PostTaskData::HandlePrePollTask));
    prepoll_called = true;
  }

  // Post HandleTimeoutTaskA() the first time and HandleTimeoutTaskB() the
  // second.
  void HandleTimeout() {
    called_types.push_back(TIMEOUT);
    event_loop->PostTask(
        NewPermanentCallback(this, !timeout_called ?
                                   &PostTaskData::HandleTimeoutTaskA :
                                   &PostTaskData::HandleTimeoutTaskB));
    timeout_called = true;
  }

  // Post HandleRepostedTaskA() and HandleRepostedTaskB().
  void HandleTimeoutTaskA() {
    called_types.push_back(TASK_TIMEOUT_A);
    event_loop->PostTask(
        NewPermanentCallback(this, &PostTaskData::HandleRepostedTaskA));
    event_loop->PostTask(
        NewPermanentCallback(this, &PostTaskData::HandleRepostedTaskB));
  }

  // These methods just record that they were called.
  void HandlePrePollTask()   { called_types.push_back(TASK_PRE_POLL); }
  void HandleTimeoutTaskB()  { called_types.push_back(TASK_TIMEOUT_B); }
  void HandleRepostedTaskA() { called_types.push_back(TASK_REPOSTED_A); }
  void HandleRepostedTaskB() { called_types.push_back(TASK_REPOSTED_B); }

  EventLoop* event_loop;  // not owned

  // The order in which various callbacks were executed.
  vector<CallbackType> called_types;

  // Have HandlePrePollCallback() and HandleTimeout() been called yet?
  bool prepoll_called;
  bool timeout_called;
};


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
  RemoveScheduledTimeoutData first_timeout_data(&event_loop);
  RemoveScheduledTimeoutData second_timeout_data(&event_loop);

  // We don't know which timeout's callback will be invoked first, so we
  // make each remove the other.
  second_timeout_data.timeout_id_to_remove = event_loop.AddTimeout(
      NewPermanentCallback(&first_timeout_data,
                           &RemoveScheduledTimeoutData::RemoveTimeout), 0, 0);
  first_timeout_data.timeout_id_to_remove = event_loop.AddTimeout(
      NewPermanentCallback(&second_timeout_data,
                           &RemoveScheduledTimeoutData::RemoveTimeout), 0, 0);
  event_loop.Run();

  // At the end, exactly one of the callbacks should've been called.
  EXPECT_TRUE(first_timeout_data.called ^ second_timeout_data.called)
      << "first=" << first_timeout_data.called
      << " second=" << second_timeout_data.called;
}

// Test that tasks posted via the PostTask() method always get called as
// soon as control is returned to the event loop.
TEST_F(EventLoopTest, PostTask) {
  EventLoop event_loop;
  PostTaskData data(&event_loop);
  event_loop.Run();

  // The pre-poll callback should run first and post a task that will get
  // called immediately afterwards.
  ASSERT_EQ(static_cast<size_t>(9), data.called_types.size());
  EXPECT_EQ(PostTaskData::PRE_POLL_CALLBACK, data.called_types[0]);
  EXPECT_EQ(PostTaskData::TASK_PRE_POLL, data.called_types[1]);

  // The timeout that gets called first should post two more tasks, which
  // should be run in the order that they were posted.
  EXPECT_EQ(PostTaskData::TIMEOUT, data.called_types[2]);
  EXPECT_EQ(PostTaskData::TASK_TIMEOUT_A, data.called_types[3]);
  EXPECT_EQ(PostTaskData::TASK_REPOSTED_A, data.called_types[4]);
  EXPECT_EQ(PostTaskData::TASK_REPOSTED_B, data.called_types[5]);

  // The second timeout should post another task, which should also be
  // called immediately.
  EXPECT_EQ(PostTaskData::TIMEOUT, data.called_types[6]);
  EXPECT_EQ(PostTaskData::TASK_TIMEOUT_B, data.called_types[7]);

  // When the pre-poll callback is called for a second time, it should exit.
  EXPECT_EQ(PostTaskData::PRE_POLL_CALLBACK, data.called_types[8]);
}

}  // namespace window_manager

int main(int argc, char** argv) {
  if (!EventLoop::IsTimerFdSupported()) {
    LOG(ERROR) << "timerfd isn't supported on this system; skipping tests";
    return 0;
  }
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
