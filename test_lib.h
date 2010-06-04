// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_TEST_LIB_H_
#define WINDOW_MANAGER_TEST_LIB_H_

#include <string>

#include <gtest/gtest.h>
extern "C" {
#include <X11/Xlib.h>
}

#include "base/file_path.h"
#include "base/scoped_ptr.h"
#include "base/logging.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/key_bindings.h"
#include "window_manager/wm_ipc.h"
#include "window_manager/x_types.h"

namespace window_manager {

class EventLoop;
class MockCompositor;
class MockXConnection;
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

// A basic test that sets up fake X and compositor interfaces and creates a
// WindowManager object.  Also includes several methods that tests can use
// for convenience.
class BasicWindowManagerTest : public ::testing::Test {
 protected:
  // Simple RAII class for creating and deleting a temporary directory.
  class ScopedTempDirectory {
   public:
    ScopedTempDirectory();
    ~ScopedTempDirectory();
    const FilePath& path() const { return path_; }

   private:
    FilePath path_;
  };

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
  // |index| is the index of the snapshot within the given toplevel
  // window.
  XWindow CreateSimpleSnapshotWindow(XWindow toplevel_xid, int index);

  // Create a panel titlebar or content window.
  XWindow CreatePanelTitlebarWindow(int width, int height);
  XWindow CreatePanelContentWindow(int width, int height,
                                   XWindow titlebar_xid,
                                   bool expanded,
                                   bool take_focus);

  // Invoke CreatePanel() with some default parameters to open an expanded
  // panel.
  Panel* CreateSimplePanel(int width,
                           int titlebar_height,
                           int content_height) {
    return CreatePanel(width, titlebar_height, content_height, true, true);
  }

  // Create titlebar and content windows for a panel, show them, and return
  // a pointer to the Panel object.
  Panel* CreatePanel(int width,
                     int titlebar_height,
                     int content_height,
                     bool expanded,
                     bool take_focus);

  // Simulates a change in the selected tab and tab count in a chrome
  // toplevel window.
  void ChangeTabInfo(XWindow toplevel_xid,
                     int tab_count,
                     int selected_tab,
                     uint32 timestamp);

  // Make the window manager handle a CreateNotify event and, if the window
  // isn't override-redirect, a MapRequest.  If it's mapped after this
  // (expected if we sent a MapRequest), send a MapNotify event.  After
  // each event, we send a ConfigureNotify if the window manager changed
  // something about the window using a ConfigureWindow request.
  void SendInitialEventsForWindow(XWindow xid);

  // Send a property change notification for the Chrome window type.
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

  // Send a WM_IPC_MESSAGE_WM_SET_LOGIN_STATE message telling the window
  // manager that the login entries should be selectable or not.
  void SendSetLoginStateMessage(bool entries_selectable);

  // Send a key press and release to the given xid.
  void SendKey(XWindow xid,
               KeyBindings::KeyCombo key,
               XTime press_timestamp,
               XTime release_timestamp);

  // Send a _NET_ACTIVE_WINDOW message asking the window manager to focus a
  // window.
  void SendActiveWindowMessage(XWindow xid);

  // Invoke Window::HandleConfigureNotify() using the client window's size.
  // The Window class defers resizing its actor until it sees a
  // ConfigureNotify event; this can be used to make sure that the actor's
  // size matches the current client size.
  void NotifyWindowAboutSize(Window* win);

  // Set the _CHROME_LOGGED_IN property on the root window to describe
  // whether Chrome is logged in or not, and send a PropertyNotify event to
  // the window manager (if it's non-NULL).
  void SetLoggedInState(bool logged_in);

  // Get the current value of the _NET_ACTIVE_WINDOW property on the root
  // window.
  XWindow GetActiveWindowProperty();

  // Are the passed-in window's composited and client windows stacked
  // between the passed-in layer and the layer underneath it?
  bool WindowIsInLayer(Window* win, StackingManager::Layer layer);

  // Is the passed-in client window entirely offscreen?
  bool WindowIsOffscreen(XWindow xid);

  // Fetch an int array property on a window and check that it contains the
  // expected values.  'num_values' is the number of expected values passed
  // as varargs.
  void TestIntArrayProperty(XWindow xid, XAtom atom, int num_values, ...);

  // Test the bounds of a panel's content window.
  void TestPanelContentBounds(
      Panel* panel, int x, int y, int width, int height);

  // Decode the message from 'event' into 'msg'.  Returns false on failure.
  bool DecodeWmIpcMessage(const XClientMessageEvent& event,
                          WmIpc::Message* msg_out);

  // Get the mock actor for the passed-in window.
  MockCompositor::TexturePixmapActor* GetMockActorForWindow(Window* win);

  scoped_ptr<EventLoop> event_loop_;
  scoped_ptr<MockXConnection> xconn_;
  scoped_ptr<MockCompositor> compositor_;
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
