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
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/event_consumer.h"
#include "window_manager/event_loop.h"
#include "window_manager/key_bindings.h"
#include "window_manager/mock_compositor.h"
#include "window_manager/mock_dbus_interface.h"
#include "window_manager/mock_gl_interface.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/real_compositor.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"
#include "window_manager/x_types.h"

namespace window_manager {

class Panel;

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
// flag-parsing code, so if the caller wants to set |log_to_stderr| based
// on a flag, a pointer to the flag's variable should be passed here (e.g.
// |&FLAGS_logtostderr|).
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

  // Register keycodes corresponding to common keysyms.
  void RegisterCommonKeySyms();

  // Create a new WindowManager object using the existing X connection,
  // compositor, etc. and store it in |wm_|.
  void CreateNewWm();

  // Call CreateNewWm() and then call its Init() method and ensure that it
  // succeeds.
  void CreateAndInitNewWm();

  // Create a basic window with no special type.
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

  // Creates a fav icon window for the associated snapshot.
  XWindow CreateFavIconWindow(XWindow snapshot_xid,
                              int width, int height);

  // Creates a title window for the associated snapshot.
  XWindow CreateTitleWindow(XWindow snapshot_xid,
                            int width, int height);

  // Creates a decoration window (favicon or title) for the associated
  // snapshot window.
  XWindow CreateDecorationWindow(XWindow snapshot_xid,
                                 chromeos::WmIpcWindowType type,
                                 int width, int height);

  // Creates a toplevel client window with an arbitrary size.
  XWindow CreateSimpleWindow();

  // Creates a snapshot client window with an arbitrary size.
  // |toplevel_xid| is the id of the associated toplevel window.
  // |index| is the index of the snapshot within the given toplevel
  // window.
  XWindow CreateSimpleSnapshotWindow(XWindow toplevel_xid, int index);

  // Create a panel titlebar or content window.  Muck around with the
  // |*new_panel*| members below to change content window parameters.
  XWindow CreatePanelTitlebarWindow(int width, int height);
  XWindow CreatePanelContentWindow(int width, int height, XWindow titlebar_xid);

  // Create titlebar and content windows for a panel, show them, and return
  // a pointer to the Panel object.
  Panel* CreatePanel(int width, int titlebar_height, int content_height);

  // Simulates a change in the selected tab and tab count in a chrome
  // toplevel window.
  void ChangeTabInfo(XWindow toplevel_xid,
                     int tab_count,
                     int selected_tab,
                     uint32_t timestamp);

  // Make the window manager handle a CreateNotify event and, if the window
  // isn't override-redirect, a MapRequest.  If it's mapped after this
  // (expected if we sent a MapRequest), send a MapNotify event.  After
  // each event, we send a ConfigureNotify if the window manager changed
  // something about the window using a ConfigureWindow request.
  void SendInitialEventsForWindow(XWindow xid);

  // Send UnmapNotify and DestroyWindow events to the window manager.
  void SendUnmapAndDestroyEventsForWindow(XWindow xid);

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

  // Append an atom to an integer property on a window.
  void AppendAtomToProperty(
      XWindow xid, XAtom property_atom, XAtom atom_to_add);

  // Configure a window to use the _NET_WM_SYNC_REQUEST protocol to
  // synchronize repaints in response to resizes.  Adds the
  // _NET_WM_SYNC_REQUEST hint to the window's WM_PROTOCOLS property and
  // sets its _NET_WM_SYNC_REQUEST_COUNTER property.
  void ConfigureWindowForSyncRequestProtocol(XWindow xid);

  // Send the window manager an event telling it that the alarm that it's
  // using to wait for notification that a client has finished repainting a
  // window has fired.
  void SendSyncRequestProtocolAlarm(XWindow xid);

  // Get the current value of the _NET_ACTIVE_WINDOW property on the root
  // window.
  XWindow GetActiveWindowProperty();

  // Get the number of WM_DELETE_WINDOW messages that have been sent to a
  // window.
  int GetNumDeleteWindowMessagesForWindow(XWindow xid);

  // Get the first WmIpc message of a particular type received by a window.
  // Returns false if no messages of that type were found.
  bool GetFirstWmIpcMessageOfType(XWindow xid,
                                  chromeos::WmIpcMessageType type,
                                  WmIpc::Message* msg_out);

  // Are the passed-in window's composited and client windows stacked
  // between the passed-in layer and the layer underneath it?
  bool WindowIsInLayer(Window* win, StackingManager::Layer layer);

  // Is the passed-in client window entirely offscreen?
  bool WindowIsOffscreen(XWindow xid);

  // Fetch an int array property on a window and check that it contains the
  // expected values.  |num_values| is the number of expected values passed
  // as varargs.
  void TestIntArrayProperty(XWindow xid, XAtom atom, int num_values, ...);

  // Test the bounds of a panel's content window.
  void TestPanelContentBounds(
      Panel* panel, int x, int y, int width, int height);

  // Are a panel's client and composited windows at the same spot?
  bool PanelClientAndCompositedWindowsHaveSamePositions(Panel* panel);

  // Decode the message from |event| into |msg|.  Returns false on failure.
  bool DecodeWmIpcMessage(const XClientMessageEvent& event,
                          WmIpc::Message* msg_out);

  // Get the mock actor for the passed-in window.
  MockCompositor::TexturePixmapActor* GetMockActorForWindow(Window* win);

  // Fills the rect specified with bounds of composited window attached to
  // specified xid.
  void GetCompositedWindowBounds(XWindow xid, Rect* bounds) const;

  scoped_ptr<EventLoop> event_loop_;
  scoped_ptr<MockXConnection> xconn_;
  scoped_ptr<MockCompositor> compositor_;
  scoped_ptr<MockDBusInterface> dbus_;
  scoped_ptr<WindowManager> wm_;

  // Settings used for subsequent windows created by
  // CreatePanelContentWindow() and CreatePanel().  These are here so that
  // new parameters can be added without having to update a bunch of
  // testing code (tests for the new parameters can change these settings,
  // while existing tests can just use reasonably-chosen defaults).
  bool new_panels_should_be_expanded_;
  bool new_panels_should_take_focus_;
  XWindow creator_content_xid_for_new_panels_;
  chromeos::WmIpcPanelUserResizeType resize_type_for_new_panels_;
};


// Base class for compositing-related tests.
class BasicCompositingTest : public ::testing::Test {
 public:
  BasicCompositingTest() {}
  virtual ~BasicCompositingTest();

  virtual void SetUp();
  virtual void TearDown() {}

 protected:
  scoped_ptr<MockGLInterface> gl_;
  scoped_ptr<MockXConnection> xconn_;
  scoped_ptr<EventLoop> event_loop_;
  scoped_ptr<RealCompositor> compositor_;
};

class BasicCompositingTreeTest : public BasicCompositingTest {
 public:
  BasicCompositingTreeTest() {}
  virtual ~BasicCompositingTreeTest();

  virtual void SetUp();
  virtual void TearDown() { BasicCompositingTest::TearDown(); }

 protected:
  //      stage
  //      |   |
  // group1   group3
  //    |       |
  // group2   group4
  //    |     |    |
  // rect1 rect2  rect3
  //
  // A container (with the exception of the stage)'s depth is further away
  // than that of its children, and earlier-added children within each
  // container will be further away than later-added children.
  //
  // Given the order in which these actors are added in test_lib.cc, the
  // depths (in nearest to furthest order) should be:
  //
  // stage   0
  // rect3   256
  // rect2   (culled)
  // group4  512
  // group3  768
  // rect1   (culled)
  // group2  1024
  // group1  1280
  RealCompositor::StageActor* stage_;
  scoped_ptr<RealCompositor::ContainerActor> group1_;
  scoped_ptr<RealCompositor::ContainerActor> group2_;
  scoped_ptr<RealCompositor::ContainerActor> group3_;
  scoped_ptr<RealCompositor::ContainerActor> group4_;
  scoped_ptr<RealCompositor::ColoredBoxActor> rect1_;
  scoped_ptr<RealCompositor::ColoredBoxActor> rect2_;
  scoped_ptr<RealCompositor::ColoredBoxActor> rect3_;
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


class TestEventConsumer : public EventConsumer {
 public:
  TestEventConsumer()
      : EventConsumer(),
        should_return_true_for_map_requests_(false) {
    reset_stats();
  }

  void reset_stats() {
    num_logged_in_state_changes_ = 0;
    num_map_requests_ = 0;
    num_mapped_windows_ = 0;
    num_unmapped_windows_ = 0;
    num_initial_pixmaps_ = 0;
    num_button_presses_ = 0;
  }

  void set_should_return_true_for_map_requests(bool return_true) {
    should_return_true_for_map_requests_ = return_true;
  }

  int num_logged_in_state_changes() const {
    return num_logged_in_state_changes_;
  }
  int num_map_requests() const { return num_map_requests_; }
  int num_mapped_windows() const { return num_mapped_windows_; }
  int num_unmapped_windows() const { return num_unmapped_windows_; }
  int num_initial_pixmaps() const { return num_initial_pixmaps_; }
  int num_button_presses() const { return num_button_presses_; }
  const std::vector<WmIpc::Message>& chrome_messages() const {
    return chrome_messages_;
  }
  const std::set<std::tr1::shared_ptr<DestroyedWindow> >&
      destroyed_windows() const {
    return destroyed_windows_;
  }

  // Begin overridden EventConsumer virtual methods.
  virtual bool IsInputWindow(XWindow xid) { return false; }
  virtual void HandleScreenResize() {}
  virtual void HandleLoggedInStateChange() { num_logged_in_state_changes_++; }
  virtual bool HandleWindowMapRequest(Window* win) {
    num_map_requests_++;
    return should_return_true_for_map_requests_;
  }
  virtual void HandleWindowMap(Window* win) { num_mapped_windows_++; }
  virtual void HandleWindowUnmap(Window* win) { num_unmapped_windows_++; }
  virtual void HandleWindowInitialPixmap(Window* win) {
    num_initial_pixmaps_++;
  }
  virtual void HandleWindowConfigureRequest(Window* win,
                                            int req_x, int req_y,
                                            int req_width, int req_height) {}
  virtual void HandleButtonPress(XWindow xid,
                                 int x, int y,
                                 int x_root, int y_root,
                                 int button,
                                 XTime timestamp) {
    num_button_presses_++;
  }
  virtual void HandleButtonRelease(XWindow xid,
                                   int x, int y,
                                   int x_root, int y_root,
                                   int button,
                                   XTime timestamp) {}
  virtual void HandlePointerEnter(XWindow xid,
                                  int x, int y,
                                  int x_root, int y_root,
                                  XTime timestamp) {}
  virtual void HandlePointerLeave(XWindow xid,
                                  int x, int y,
                                  int x_root, int y_root,
                                  XTime timestamp) {}
  virtual void HandlePointerMotion(XWindow xid,
                                   int x, int y,
                                   int x_root, int y_root,
                                   XTime timestamp) {}
  virtual void HandleChromeMessage(const WmIpc::Message& msg) {
    chrome_messages_.push_back(msg);
  }
  virtual void HandleClientMessage(XWindow xid,
                                   XAtom message_type,
                                   const long data[5]) {}
  virtual void HandleFocusChange(XWindow xid, bool focus_in) {}
  virtual void HandleWindowPropertyChange(XWindow xid, XAtom xatom) {}
  virtual void OwnDestroyedWindow(DestroyedWindow* destroyed_win, XWindow xid) {
    destroyed_windows_.insert(
        std::tr1::shared_ptr<DestroyedWindow>(destroyed_win));
  }
  // End overridden EventConsumer virtual methods.

 private:
  // Should we return true when HandleWindowMapRequest() is invoked?
  bool should_return_true_for_map_requests_;

  int num_logged_in_state_changes_;
  int num_map_requests_;
  int num_mapped_windows_;
  int num_unmapped_windows_;
  int num_initial_pixmaps_;
  int num_button_presses_;

  // Messages received via HandleChromeMessage().
  std::vector<WmIpc::Message> chrome_messages_;

  // DestroyedWindow objects that WindowManager has given to us.
  std::set<std::tr1::shared_ptr<DestroyedWindow> > destroyed_windows_;
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_TEST_LIB_H_
