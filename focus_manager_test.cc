// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <vector>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "window_manager/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/focus_manager.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/test_lib.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

namespace window_manager {

class FocusManagerTest : public BasicWindowManagerTest {
 protected:
  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();
    focus_manager_ = wm_->focus_manager();
  }

  FocusManager* focus_manager_;  // instance belonging to 'wm_'
};

// Helper class used by the FocusChangeListener test.
struct TestFocusChangeListener : public FocusChangeListener {
  TestFocusChangeListener() : num_changes(0) {}

  // Begin FocusChangeListener implementation.
  virtual void HandleFocusChange() { num_changes++; }
  // End FocusChangeListener implementation.

  // Number of times that HandleFocusChange() has been called.
  int num_changes;
};

// Test that the class focuses windows when we ask it to and updates the
// _NET_ACTIVE_WINDOW property.
TEST_F(FocusManagerTest, Basic) {
  EXPECT_TRUE(focus_manager_->focused_win() == NULL);

  XTime timestamp = 123;  // arbitrary
  XWindow xid = CreateSimpleWindow();
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);

  focus_manager_->FocusWindow(&win, timestamp++);
  EXPECT_EQ(xid, xconn_->focused_xid());
  EXPECT_EQ(&win, focus_manager_->focused_win());
  EXPECT_EQ(xid, GetActiveWindowProperty());

  focus_manager_->FocusWindow(NULL, timestamp++);
  EXPECT_EQ(xconn_->GetRootWindow(), xconn_->focused_xid());
  EXPECT_TRUE(focus_manager_->focused_win() == NULL);
  EXPECT_EQ(0, GetActiveWindowProperty());
}

// Test that click-to-focus is implemented properly.
TEST_F(FocusManagerTest, ClickToFocus) {
  XTime timestamp = 123;  // arbitrary
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);

  // After we tell the focus manager that we want to use click-to-focus, it
  // should install a button grab on the window.
  focus_manager_->UseClickToFocusForWindow(&win);
  EXPECT_TRUE(info->button_is_grabbed(0));

  // Grab the pointer as if a button had been pressed and then make sure
  // that the focus manager automatically terminates the grab.
  xconn_->set_pointer_grab_xid(xid);
  focus_manager_->HandleButtonPressInWindow(&win, timestamp++);
  EXPECT_EQ(0, xconn_->pointer_grab_xid());

  // Create a second window and focus it.
  XWindow xid2 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info2 = xconn_->GetWindowInfoOrDie(xid2);
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid2, &geometry));
  Window win2(wm_.get(), xid2, false, geometry);
  focus_manager_->FocusWindow(&win2, timestamp++);
  ASSERT_EQ(xid2, xconn_->focused_xid());

  // The focus manager shouldn't install a button grab when enabling
  // click-to-focus for the second window, since it currently has the
  // focus.
  focus_manager_->UseClickToFocusForWindow(&win2);
  EXPECT_FALSE(info2->button_is_grabbed(0));

  // If we focus the first window, its button grab should be removed and
  // one should be added on the second window.
  focus_manager_->FocusWindow(&win, timestamp++);
  EXPECT_FALSE(info->button_is_grabbed(0));
  EXPECT_TRUE(info2->button_is_grabbed(0));

  // When the second window is unmapped, the button grab should be removed.
  focus_manager_->HandleWindowUnmap(&win2);
  EXPECT_FALSE(info2->button_is_grabbed(0));
}

// Test that we notify FocusChangeListeners when the focus changes.
TEST_F(FocusManagerTest, FocusChangeListener) {
  XTime timestamp = 123;  // arbitrary
  XWindow xid = CreateSimpleWindow();
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);

  TestFocusChangeListener listener;
  focus_manager_->RegisterFocusChangeListener(&listener);
  EXPECT_EQ(0, listener.num_changes);

  focus_manager_->FocusWindow(&win, timestamp++);
  EXPECT_EQ(1, listener.num_changes);

  // We shouldn't get called if the focus didn't actually change.
  focus_manager_->FocusWindow(&win, timestamp++);
  EXPECT_EQ(1, listener.num_changes);

  focus_manager_->FocusWindow(NULL, timestamp++);
  EXPECT_EQ(2, listener.num_changes);
}

// Test that we don't let the timestamps that we use when focusing
// windows move backwards.
TEST_F(FocusManagerTest, AdjustTimestamp) {
  XTime timestamp = 123;  // arbitrary

  // We need two windows, since FocusManager will ignore attempts to
  // focus the already-focused window.
  XWindow xid = CreateSimpleWindow();
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  XWindow xid2 = CreateSimpleWindow();
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid2, &geometry));
  Window win2(wm_.get(), xid2, false, geometry);

  focus_manager_->FocusWindow(&win, timestamp);
  EXPECT_EQ(xid, xconn_->focused_xid());
  EXPECT_EQ(timestamp, xconn_->last_focus_timestamp());

  timestamp += 5;
  focus_manager_->FocusWindow(&win2, timestamp);
  EXPECT_EQ(xid2, xconn_->focused_xid());
  EXPECT_EQ(timestamp, xconn_->last_focus_timestamp());

  focus_manager_->FocusWindow(&win, timestamp - 5);
  EXPECT_EQ(xid, xconn_->focused_xid());
  EXPECT_EQ(timestamp, xconn_->last_focus_timestamp());
}

// Test that the focus manager handles modal windows correctly.
// Specifically, when it sees a button press and the currently-focused
// window is modal, the button press should be dropped instead of getting
// replayed.
TEST_F(FocusManagerTest, Modality) {
  // Create a modal window and focus it.
  XTime timestamp = 123;  // arbitrary
  XWindow xid = CreateSimpleWindow();
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  focus_manager_->UseClickToFocusForWindow(&win);
  win.wm_state_modal_ = true;
  focus_manager_->FocusWindow(&win, timestamp++);
  ASSERT_EQ(xid, xconn_->focused_xid());

  // Create a second window.
  XWindow xid2 = CreateSimpleWindow();
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid2, &geometry));
  Window win2(wm_.get(), xid2, false, geometry);
  focus_manager_->UseClickToFocusForWindow(&win2);

  // When the focus manager sees a button press in the second window, it
  // should avoid replaying the event.
  int initial_num_replays = xconn_->num_pointer_ungrabs_with_replayed_events();
  xconn_->set_pointer_grab_xid(xid);
  focus_manager_->HandleButtonPressInWindow(&win, timestamp++);
  EXPECT_EQ(0, xconn_->pointer_grab_xid());
  EXPECT_EQ(initial_num_replays,
            xconn_->num_pointer_ungrabs_with_replayed_events());

  // Now make the first window non-modal and check that clicks are replayed.
  win.wm_state_modal_ = false;
  initial_num_replays = xconn_->num_pointer_ungrabs_with_replayed_events();
  xconn_->set_pointer_grab_xid(xid);
  focus_manager_->HandleButtonPressInWindow(&win, timestamp++);
  EXPECT_EQ(0, xconn_->pointer_grab_xid());
  EXPECT_EQ(initial_num_replays + 1,
            xconn_->num_pointer_ungrabs_with_replayed_events());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
