// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "window_manager/callback.h"
#include "window_manager/event_loop.h"
#include "window_manager/pointer_position_watcher.h"
#include "window_manager/test_lib.h"
#include "window_manager/x11/mock_x_connection.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

namespace window_manager {

class PointerPositionWatcherTest : public ::testing::Test {
};

// Struct that contains a watcher and has a method to delete it.
// Used by the DeleteFromCallback test.
struct WatcherContainer {
  void set_watcher(PointerPositionWatcher* new_watcher) {
    watcher.reset(new_watcher);
  }
  scoped_ptr<PointerPositionWatcher> watcher;
};

TEST_F(PointerPositionWatcherTest, Basic) {
  EventLoop event_loop;
  MockXConnection xconn;
  xconn.SetPointerPosition(Point(0, 0));

  // Watch for the pointer moving into a 20x30 rectangle at (50, 100).
  TestCallbackCounter counter;
  scoped_ptr<PointerPositionWatcher> watcher(
      new PointerPositionWatcher(
          &event_loop,
          &xconn,
          NewPermanentCallback(&counter, &TestCallbackCounter::Increment),
          true,  // watch_for_entering_target
          Rect(50, 100, 20, 30)));
  EXPECT_GE(watcher->timeout_id(), 0);

  // Check that the callback doesn't get run and the timer stays active as
  // long as the pointer is outside of the rectangle.
  watcher->TriggerTimeout();
  EXPECT_EQ(0, counter.num_calls());
  EXPECT_GE(watcher->timeout_id(), 0);

  xconn.SetPointerPosition(Point(49, 105));
  watcher->TriggerTimeout();
  EXPECT_EQ(0, counter.num_calls());
  EXPECT_GE(watcher->timeout_id(), 0);

  // As soon as the pointer moves into the rectangle, the callback should
  // be run and the timer should be destroyed.
  xconn.SetPointerPosition(Point(50, 105));
  watcher->TriggerTimeout();
  EXPECT_EQ(1, counter.num_calls());
  EXPECT_EQ(-1, watcher->timeout_id());

  // Now create a new watcher that waits for the pointer to move *outside*
  // of the same region.
  watcher.reset(
      new PointerPositionWatcher(
          &event_loop,
          &xconn,
          NewPermanentCallback(&counter, &TestCallbackCounter::Increment),
          false,  // watch_for_entering_target=false
          Rect(50, 100, 20, 30)));
  EXPECT_GE(watcher->timeout_id(), 0);
  counter.Reset();

  watcher->TriggerTimeout();
  EXPECT_EQ(0, counter.num_calls());
  EXPECT_GE(watcher->timeout_id(), 0);

  xconn.SetPointerPosition(Point(69, 129));
  watcher->TriggerTimeout();
  EXPECT_EQ(0, counter.num_calls());
  EXPECT_GE(watcher->timeout_id(), 0);

  xconn.SetPointerPosition(Point(69, 130));
  watcher->TriggerTimeout();
  EXPECT_EQ(1, counter.num_calls());
  EXPECT_EQ(-1, watcher->timeout_id());
}

// Test that we don't crash if a callback deletes the watcher that ran it.
TEST_F(PointerPositionWatcherTest, DeleteFromCallback) {
  EventLoop event_loop;
  MockXConnection xconn;
  xconn.SetPointerPosition(Point(0, 0));

  // Register a callback that deletes its own watcher.
  WatcherContainer container;
  container.set_watcher(
      new PointerPositionWatcher(
          &event_loop,
          &xconn,
          NewPermanentCallback(
              &container,
              &WatcherContainer::set_watcher,
              static_cast<PointerPositionWatcher*>(NULL)),
          true,      // watch_for_entering_target
          Rect(0, 0, 10, 10)));

  container.watcher->TriggerTimeout();
  EXPECT_TRUE(container.watcher.get() == NULL);
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
