// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_TEST_LIB_H_
#define WINDOW_MANAGER_TEST_LIB_H_

#include <gtest/gtest.h>

#include "base/scoped_ptr.h"
#include "base/logging.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/wm_ipc.h"
#include "window_manager/x_types.h"

namespace window_manager {

class EventLoop;
class MockXConnection;
class MockClutterInterface;
class Panel;
class WindowManager;

// Test that two bytes sequences are equal, pretty-printing the difference
// otherwise.  Invoke as:
//
//   EXPECT_PRED_FORMAT3(BytesAreEqual, expected, actual, length);
//
testing::AssertionResult BytesAreEqual(
    const char* expected_expr,
    const char* actual_expr,
    const char* size_expr,
    const unsigned char* expected,
    const unsigned char* actual,
    size_t size);

// Called from tests' main() functions to handle a bunch of boilerplate.
// Its return value should be returned from main().  We initialize the
// flag-parsing code, so if the caller wants to set 'log_to_stderr' based
// on a flag, a pointer to the flag's variable should be passed here (e.g.
// '&FLAGS_logtostderr').
int InitAndRunTests(int* argc, char** argv, bool* log_to_stderr);

// A basic test that sets up fake X and Clutter interfaces and creates a
// WindowManager object.  Also includes several methods that tests can use
// for convenience.
class BasicWindowManagerTest : public ::testing::Test {
 protected:
  virtual void SetUp();

  // Create a new WindowManager object with a logged-in state and store it
  // in 'wm_'.  Helper method for a bunch of tests that need to do this
  // repeatedly.
  void CreateAndInitNewWm();

  // Creates a basic window with no special type.
  XWindow CreateBasicWindow(int x, int y, int width, int height);

  // Create a toplevel client window with the passed-in position and
  // dimensions.  It has type WINDOW_TYPE_CHROME_TOPLEVEL.
  XWindow CreateToplevelWindow(int tab_count, int selected_tab,
                               int x, int y, int width, int height);

  // Create a snapshot client window with the passed-in position and
  // dimensions and associated parent toplevel window.
  XWindow CreateSnapshotWindow(XWindow parent_xid, int index,
                               int x, int y,
                               int width, int height);

  // Creates a toplevel client window with an arbitrary size.
  XWindow CreateSimpleWindow();

  // Creates a snapshot client window with an arbitrary size.
  // |toplevel_xid| is the id of the associated toplevel window.
  XWindow CreateSimpleSnapshotWindow(XWindow toplevel_xid, int index);

  // Create a panel titlebar or content window.
  XWindow CreatePanelTitlebarWindow(int width, int height);
  XWindow CreatePanelContentWindow(
      int width, int height, XWindow titlebar_xid, bool expanded);

  // Create titlebar and content windows for a panel, show them, and return
  // a pointer to the Panel object.
  Panel* CreatePanel(int width,
                     int titlebar_height,
                     int content_height,
                     bool expanded);

  // Simulates a change in the selected tab and tab count in a chrome
  // toplevel window.
  void ChangeTabInfo(XWindow toplevel_xid,
                     int tab_count,
                     int selected_tab);

  // Make the window manager handle a CreateNotify event and, if the window
  // isn't override-redirect, a MapRequest.  If it's mapped after this
  // (expected if we sent a MapRequest), send a MapNotify event.
  void SendInitialEventsForWindow(XWindow xid);

  // Make the window manager handle FocusNotify events saying that the
  // focus was passed from 'out_xid' to 'in_xid'.  Events are only sent for
  // windows that are neither None nor the root window.
  void SendFocusEvents(XWindow out_xid, XWindow in_xid);

  // Send a property change notification for the chrome window type.
  void SendWindowTypeEvent(XWindow xid);

  // Send a WmIpc message.
  void SendWmIpcMessage(const WmIpc::Message& msg);

  // Send a WM_NOTIFY_IPC_VERSION message.
  void SendNotifyIpcVersionMessage(int version);

  // Send a WM_SET_PANEL_STATE message.
  void SendSetPanelStateMessage(Panel* panel, bool expanded);

  // Send a WM_NOTIFY_PANEL_DRAGGED message.
  void SendPanelDraggedMessage(Panel* panel, int x, int y);

  // Send a WM_NOTIFY_PANEL_DRAG_COMPLETE message.
  void SendPanelDragCompleteMessage(Panel* panel);

  // Send a _NET_ACTIVE_WINDOW message asking the window manager to focus a
  // window.
  void SendActiveWindowMessage(XWindow xid);

  // Invoke Window::HandleConfigureNotify() using the client window's size.
  // The Window class defers resizing its actor until it sees a
  // ConfigureNotify event; this can be used to make sure that the actor's
  // size matches the current client size.
  void NotifyWindowAboutSize(Window* win);

  // Get the current value of the _NET_ACTIVE_WINDOW property on the root
  // window.
  XWindow GetActiveWindowProperty();

  // Are the passed-in window's composited and client windows stacked
  // between the passed-in layer and the layer underneath it?
  bool WindowIsInLayer(Window* win, StackingManager::Layer layer);

  // Fetch an int array property on a window and check that it contains the
  // expected values.  'num_values' is the number of expected values passed
  // as varargs.
  void TestIntArrayProperty(XWindow xid, XAtom atom, int num_values, ...);

  // Test the bounds of a panel's content window.
  void TestPanelContentBounds(
      Panel* panel, int x, int y, int width, int height);

  scoped_ptr<EventLoop> event_loop_;
  scoped_ptr<MockXConnection> xconn_;
  scoped_ptr<MockClutterInterface> clutter_;
  scoped_ptr<WindowManager> wm_;
};

// Simple class that can be used to test callback invocation.
class TestCallbackCounter {
 public:
  TestCallbackCounter() : num_calls_(0) {}
  int num_calls() const { return num_calls_; }
  void Reset() { num_calls_ = 0; }
  void Increment() { num_calls_++; }
 private:
  // Number of times that Increment() has been invoked.
  int num_calls_;
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_TEST_LIB_H_
