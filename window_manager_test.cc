// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cstdarg>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/file_util.h"
#include "base/scoped_ptr.h"
#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/compositor.h"
#include "window_manager/event_consumer.h"
#include "window_manager/event_loop.h"
#include "window_manager/layout_manager.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/panel.h"
#include "window_manager/panel_bar.h"
#include "window_manager/panel_manager.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

// From window_manager.cc.
DECLARE_string(logged_in_log_dir);
DECLARE_string(logged_out_log_dir);

using file_util::FileEnumerator;
using std::find;
using std::string;
using std::vector;

namespace window_manager {

class WindowManagerTest : public BasicWindowManagerTest {
 protected:
  // Recursively walk a directory and return the total size of all files
  // within it.
  off_t GetTotalFileSizeInDirectory(const FilePath& dir_path) {
    off_t total_size = 0;
    FileEnumerator enumerator(dir_path, true, FileEnumerator::FILES);
    while (true) {
      FilePath file_path = enumerator.Next();
      if (file_path.value().empty())
        break;
      FileEnumerator::FindInfo info;
      enumerator.GetFindInfo(&info);
      total_size += info.stat.st_size;
    }
    return total_size;
  }
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
  int num_button_presses() const { return num_button_presses_; }
  const vector<WmIpc::Message>& chrome_messages() const {
    return chrome_messages_;
  }

  // Begin overridden EventConsumer virtual methods.
  bool IsInputWindow(XWindow xid) { return false; }
  void HandleScreenResize() {}
  void HandleLoggedInStateChange() { num_logged_in_state_changes_++; }
  bool HandleWindowMapRequest(Window* win) {
    num_map_requests_++;
    return should_return_true_for_map_requests_;
  }
  void HandleWindowMap(Window* win) { num_mapped_windows_++; }
  void HandleWindowUnmap(Window* win) { num_unmapped_windows_++; }
  void HandleWindowConfigureRequest(Window* win,
                                    int req_x, int req_y,
                                    int req_width, int req_height) {}
  void HandleButtonPress(XWindow xid,
                         int x, int y,
                         int x_root, int y_root,
                         int button,
                         XTime timestamp) {
    num_button_presses_++;
  }
  void HandleButtonRelease(XWindow xid,
                           int x, int y,
                           int x_root, int y_root,
                           int button,
                           XTime timestamp) {}
  void HandlePointerEnter(XWindow xid,
                          int x, int y,
                          int x_root, int y_root,
                          XTime timestamp) {}
  void HandlePointerLeave(XWindow xid,
                          int x, int y,
                          int x_root, int y_root,
                          XTime timestamp) {}
  void HandlePointerMotion(XWindow xid,
                           int x, int y,
                           int x_root, int y_root,
                           XTime timestamp) {}
  void HandleChromeMessage(const WmIpc::Message& msg) {
    chrome_messages_.push_back(msg);
  }
  void HandleClientMessage(XWindow xid,
                           XAtom message_type,
                           const long data[5]) {}
  void HandleFocusChange(XWindow xid, bool focus_in) {}
  void HandleWindowPropertyChange(XWindow xid, XAtom xatom) {}
  // End overridden EventConsumer virtual methods.

 private:
  // Should we return true when HandleWindowMapRequest() is invoked?
  bool should_return_true_for_map_requests_;

  int num_logged_in_state_changes_;
  int num_map_requests_;
  int num_mapped_windows_;
  int num_unmapped_windows_;
  int num_button_presses_;

  // Messages received via HandleChromeMessage().
  vector<WmIpc::Message> chrome_messages_;
};

TEST_F(WindowManagerTest, RegisterExistence) {
  // First, make sure that the window manager created a window and gave it
  // a title.
  XAtom title_atom = None;
  ASSERT_TRUE(xconn_->GetAtom("_NET_WM_NAME", &title_atom));
  string window_title;
  EXPECT_TRUE(
      xconn_->GetStringProperty(wm_->wm_xid_, title_atom, &window_title));
  EXPECT_EQ(WindowManager::GetWmName(), window_title);

  // Check that the window and compositing manager selections are owned by
  // the window manager's window.
  XAtom wm_atom = None, cm_atom = None;
  ASSERT_TRUE(xconn_->GetAtom("WM_S0", &wm_atom));
  ASSERT_TRUE(xconn_->GetAtom("_NET_WM_CM_S0", &cm_atom));
  EXPECT_EQ(wm_->wm_xid_, xconn_->GetSelectionOwner(wm_atom));
  EXPECT_EQ(wm_->wm_xid_, xconn_->GetSelectionOwner(cm_atom));

  XAtom manager_atom = None;
  ASSERT_TRUE(xconn_->GetAtom("MANAGER", &manager_atom));

  // Client messages should be sent to the root window announcing the
  // window manager's existence.
  MockXConnection::WindowInfo* root_info =
      xconn_->GetWindowInfoOrDie(xconn_->GetRootWindow());
  ASSERT_GE(root_info->client_messages.size(), 2);

  EXPECT_EQ(ClientMessage, root_info->client_messages[0].type);
  EXPECT_EQ(manager_atom, root_info->client_messages[0].message_type);
  EXPECT_EQ(XConnection::kLongFormat, root_info->client_messages[0].format);
  EXPECT_EQ(wm_atom, root_info->client_messages[0].data.l[1]);
  EXPECT_EQ(wm_->wm_xid_, root_info->client_messages[0].data.l[2]);

  EXPECT_EQ(ClientMessage, root_info->client_messages[1].type);
  EXPECT_EQ(manager_atom, root_info->client_messages[1].message_type);
  EXPECT_EQ(XConnection::kLongFormat, root_info->client_messages[1].format);
  EXPECT_EQ(cm_atom, root_info->client_messages[1].data.l[1]);
  EXPECT_EQ(wm_->wm_xid_, root_info->client_messages[0].data.l[2]);
}

// Test different race conditions where a client window is created and/or
// mapped while WindowManager::Init() is running.
TEST_F(WindowManagerTest, ExistingWindows) {
  // First, test the case where a window has already been mapped before the
  // WindowManager object is initialized, so no CreateNotify or MapNotify
  // event is sent.
  wm_.reset(NULL);
  xconn_.reset(new MockXConnection);
  event_loop_.reset(new EventLoop);
  compositor_.reset(new MockCompositor(xconn_.get()));
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  xconn_->MapWindow(xid);

  CreateAndInitNewWm();
  Window* win = wm_->GetWindowOrDie(xid);
  EXPECT_TRUE(win->mapped());
  EXPECT_TRUE(GetMockActorForWindow(win)->visible());

  // Now test the case where the window starts out unmapped and
  // WindowManager misses the CreateNotify event but receives the
  // MapRequest (and subsequent MapNotify).
  wm_.reset(NULL);
  xconn_.reset(new MockXConnection);
  event_loop_.reset(new EventLoop);
  compositor_.reset(new MockCompositor(xconn_.get()));
  xid = CreateSimpleWindow();
  info = xconn_->GetWindowInfoOrDie(xid);

  CreateAndInitNewWm();
  EXPECT_FALSE(info->mapped);
  win = wm_->GetWindowOrDie(xid);
  EXPECT_FALSE(win->mapped());
  EXPECT_FALSE(GetMockActorForWindow(win)->visible());

  XEvent event;
  xconn_->InitMapRequestEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(info->mapped);

  xconn_->InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(win->mapped());
  EXPECT_TRUE(GetMockActorForWindow(win)->visible());

  // Finally, test the typical case where a window is created after
  // WindowManager has been initialized.
  wm_.reset(NULL);
  xconn_.reset(new MockXConnection);
  event_loop_.reset(new EventLoop);
  compositor_.reset(new MockCompositor(xconn_.get()));
  xid = None;
  info = NULL;

  CreateAndInitNewWm();

  xid = CreateSimpleWindow();
  info = xconn_->GetWindowInfoOrDie(xid);

  xconn_->InitCreateWindowEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_FALSE(info->mapped);
  win = wm_->GetWindowOrDie(xid);
  EXPECT_FALSE(win->mapped());
  EXPECT_FALSE(GetMockActorForWindow(win)->visible());

  xconn_->InitMapRequestEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(info->mapped);
  EXPECT_TRUE(win->mapped());

  xconn_->InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(GetMockActorForWindow(win)->visible());
}

// Test that we display override-redirect windows onscreen regardless of
// whether they're mapped or not by the time that we learn about them.
TEST_F(WindowManagerTest, OverrideRedirectMapping) {
  // Test the case where a client has already mapped an override-redirect
  // window by the time that we receive the CreateNotify event about it.
  // We should still pay attention to the MapNotify event that comes
  // afterwards and display the window.
  XWindow xid = xconn_->CreateWindow(
        xconn_->GetRootWindow(),
        10, 20,  // x, y
        30, 40,  // width, height
        true,    // override redirect
        false,   // input only
        0);      // event mask
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  xconn_->MapWindow(xid);
  ASSERT_TRUE(info->mapped);

  XEvent event;
  xconn_->InitCreateWindowEvent(&event, xid);
  wm_->HandleEvent(&event);
  xconn_->InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);

  // Now test the other possibility, where the window isn't mapped on the X
  // server yet when we receive the CreateNotify event.
  Window* win = wm_->GetWindowOrDie(xid);
  EXPECT_TRUE(GetMockActorForWindow(win)->visible());

  XWindow xid2 = xconn_->CreateWindow(
        xconn_->GetRootWindow(),
        10, 20,  // x, y
        30, 40,  // width, height
        true,    // override redirect
        false,   // input only
        0);      // event mask
  MockXConnection::WindowInfo* info2 = xconn_->GetWindowInfoOrDie(xid2);

  xconn_->InitCreateWindowEvent(&event, xid2);
  wm_->HandleEvent(&event);
  xconn_->MapWindow(xid2);
  ASSERT_TRUE(info2->mapped);
  xconn_->InitMapEvent(&event, xid2);
  wm_->HandleEvent(&event);

  Window* win2 = wm_->GetWindowOrDie(xid2);
  EXPECT_TRUE(GetMockActorForWindow(win2)->visible());
}

TEST_F(WindowManagerTest, InputWindows) {
  // Check that CreateInputWindow() creates windows as requested.
  int event_mask = ButtonPressMask | ButtonReleaseMask;
  XWindow xid = wm_->CreateInputWindow(100, 200, 300, 400, event_mask);
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfo(xid);
  ASSERT_TRUE(info != NULL);
  EXPECT_EQ(100, info->x);
  EXPECT_EQ(200, info->y);
  EXPECT_EQ(300, info->width);
  EXPECT_EQ(400, info->height);
  EXPECT_EQ(true, info->mapped);
  EXPECT_EQ(true, info->override_redirect);
  EXPECT_EQ(event_mask, info->event_mask);

  // Move and resize the window.
  EXPECT_TRUE(wm_->ConfigureInputWindow(xid, 500, 600, 700, 800));
  EXPECT_EQ(500, info->x);
  EXPECT_EQ(600, info->y);
  EXPECT_EQ(700, info->width);
  EXPECT_EQ(800, info->height);
  EXPECT_EQ(true, info->mapped);
}

TEST_F(WindowManagerTest, EventConsumer) {
  TestEventConsumer ec;
  wm_->event_consumers_.insert(&ec);

  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  wm_->RegisterEventConsumerForWindowEvents(xid, &ec);

  XEvent event;
  xconn_->InitCreateWindowEvent(&event, xid);
  wm_->HandleEvent(&event);

  // Send various events to the WindowManager object and check that they
  // get forwarded to our EventConsumer.
  xconn_->InitMapRequestEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(info->mapped);
  xconn_->InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  xconn_->InitButtonPressEvent(&event, xid, 5, 5, 1);
  wm_->HandleEvent(&event);
  xconn_->InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);

  wm_->UnregisterEventConsumerForWindowEvents(xid, &ec);

  // We don't know whether we'll get the MapRequest event for this window;
  // the LayoutManager might've handled it before us.
  EXPECT_EQ(1, ec.num_mapped_windows());
  EXPECT_EQ(1, ec.num_button_presses());
  EXPECT_EQ(1, ec.num_unmapped_windows());

  // Create a second window.
  ec.reset_stats();
  XWindow xid2 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info2 = xconn_->GetWindowInfoOrDie(xid2);
  info2->override_redirect = true;

  // Send events appropriate for an override-redirect window.
  xconn_->InitCreateWindowEvent(&event, xid2);
  wm_->HandleEvent(&event);
  xconn_->InitMapEvent(&event, xid2);
  wm_->HandleEvent(&event);
  xconn_->InitButtonPressEvent(&event, xid2, 5, 5, 1);
  wm_->HandleEvent(&event);
  xconn_->InitUnmapEvent(&event, xid2);
  wm_->HandleEvent(&event);

  // The event consumer should've heard about the second window being
  // mapped and unmapped, but not about the button press (since it never
  // registered interest in the window).
  EXPECT_EQ(1, ec.num_mapped_windows());
  EXPECT_EQ(0, ec.num_button_presses());
  EXPECT_EQ(1, ec.num_unmapped_windows());

  // It's a bit of a stretch to include this in this test, but check that the
  // window manager didn't do anything to the window (since it's an
  // override-redirect window).
  EXPECT_FALSE(info2->changed);

  // Create a third window.  Set a big, bogus window type on it so that
  // none of the standard event consumers try to do anything with it.
  ec.reset_stats();
  ec.set_should_return_true_for_map_requests(true);
  XWindow xid3 = CreateSimpleWindow();
  wm_->wm_ipc()->SetWindowType(
      xid3, static_cast<chromeos::WmIpcWindowType>(4243289), NULL);

  xconn_->InitCreateWindowEvent(&event, xid3);
  wm_->HandleEvent(&event);
  xconn_->InitMapRequestEvent(&event, xid3);
  wm_->HandleEvent(&event);

  // We should get a map request for this window, and we should immediately
  // get notified that it was mapped (since we returned true in response to
  // the request).
  EXPECT_EQ(1, ec.num_map_requests());
  EXPECT_EQ(1, ec.num_mapped_windows());
  EXPECT_TRUE(wm_->GetWindowOrDie(xid3)->mapped());

  // Check that we don't get notified again when the window manager
  // receives notification that the window was mapped.
  xconn_->InitMapEvent(&event, xid3);
  wm_->HandleEvent(&event);
  EXPECT_EQ(1, ec.num_mapped_windows());
}

// Check that windows that get reparented away from the root (like Flash
// plugin windows) get unredirected.
TEST_F(WindowManagerTest, Reparent) {
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  ASSERT_TRUE(info->redirected);

  XEvent event;
  xconn_->InitCreateWindowEvent(&event, xid);
  wm_->HandleEvent(&event);
  xconn_->InitMapRequestEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(info->mapped);
  xconn_->InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);

  XReparentEvent* reparent_event = &(event.xreparent);
  memset(reparent_event, 0, sizeof(*reparent_event));
  reparent_event->type = ReparentNotify;
  reparent_event->window = xid;
  reparent_event->parent = 324324;  // arbitrary number
  wm_->HandleEvent(&event);

  // After the window gets reparented away from the root, WindowManager
  // should've unredirected it and should no longer be tracking it.
  EXPECT_TRUE(wm_->GetWindow(xid) == NULL);
  EXPECT_FALSE(info->redirected);
}

TEST_F(WindowManagerTest, RestackOverrideRedirectWindows) {
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  XEvent event;

  // Create two override-redirect windows and map them both.
  XWindow xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      10, 20,  // x, y
      30, 40,  // width, height
      true,    // override redirect
      false,   // input only
      0);      // event mask
  xconn_->MapWindow(xid);
  SendInitialEventsForWindow(xid);
  Window* win = wm_->GetWindowOrDie(xid);

  XWindow xid2 = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      10, 20,  // x, y
      30, 40,  // width, height
      true,    // override redirect
      false,   // input only
      0);      // event mask
  xconn_->MapWindow(xid2);
  SendInitialEventsForWindow(xid2);
  Window* win2 = wm_->GetWindowOrDie(xid2);

  // The second window should initially be stacked above the first.
  EXPECT_LT(stage->GetStackingIndex(win2->actor()),
            stage->GetStackingIndex(win->actor()));

  // Send a message saying that the first window is on top of the second.
  xconn_->StackWindow(xid, xid2, true);
  xconn_->InitConfigureNotifyEvent(&event, xid);
  event.xconfigure.above = xid2;
  wm_->HandleEvent(&event);
  EXPECT_LT(stage->GetStackingIndex(win->actor()),
            stage->GetStackingIndex(win2->actor()));
}

TEST_F(WindowManagerTest, StackOverrideRedirectWindowsAboveLayers) {
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  XEvent event;

  // Create a normal, non-override-redirect window.
  XWindow normal_xid = CreateSimpleWindow();
  SendInitialEventsForWindow(normal_xid);
  Window* normal_win = wm_->GetWindowOrDie(normal_xid);

  // Create an override-redirect window and map it.
  XWindow xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      10, 20,  // x, y
      30, 40,  // width, height
      true,    // override redirect
      false,   // input only
      0);      // event mask
  xconn_->MapWindow(xid);
  SendInitialEventsForWindow(xid);
  Window* win = wm_->GetWindowOrDie(xid);

  // The override-redirect window's actor should initially be stacked above
  // the actor for the top stacking layer (and the normal window's actor,
  // of course).
  Compositor::Actor* debugging_layer_actor =
      wm_->stacking_manager()->layer_to_actor_[
          StackingManager::LAYER_DEBUGGING].get();
  ASSERT_TRUE(debugging_layer_actor != NULL);
  EXPECT_LT(stage->GetStackingIndex(win->actor()),
            stage->GetStackingIndex(debugging_layer_actor));
  EXPECT_LT(stage->GetStackingIndex(win->actor()),
            stage->GetStackingIndex(normal_win->actor()));

  // Stack the override-redirect window slightly lower, but still above the
  // normal window.
  XWindow fullscreen_layer_xid =
      wm_->stacking_manager()->layer_to_xid_[
          StackingManager::LAYER_FULLSCREEN_PANEL];
  xconn_->StackWindow(xid, fullscreen_layer_xid, true);
  xconn_->InitConfigureNotifyEvent(&event, xid);
  event.xconfigure.above = fullscreen_layer_xid;
  wm_->HandleEvent(&event);

  // Create a second normal window and check that the override-redirect
  // window is above it.  This protects against a regression of the issue
  // described at http://crosbug.com/3451.
  XWindow normal_xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(normal_xid2);
  Window* normal_win2 = wm_->GetWindowOrDie(normal_xid2);
  EXPECT_LT(stage->GetStackingIndex(win->actor()),
            stage->GetStackingIndex(normal_win->actor()));
  EXPECT_LT(stage->GetStackingIndex(win->actor()),
            stage->GetStackingIndex(normal_win2->actor()));
}

// Test that we honor ConfigureRequest events that change an unmapped
// window's size, and that we ignore fields that are unset in its
// 'value_mask' field.
TEST_F(WindowManagerTest, ConfigureRequestResize) {
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  const int orig_width = info->width;
  const int orig_height = info->height;

  XEvent event;
  xconn_->InitCreateWindowEvent(&event, xid);
  wm_->HandleEvent(&event);

  // Send a ConfigureRequest event with its width and height fields masked
  // out, and check that the new width and height values are ignored.
  const int new_width = orig_width * 2;
  const int new_height = orig_height * 2;
  xconn_->InitConfigureRequestEvent(
      &event, xid, info->x, info->y, new_width, new_height);
  event.xconfigurerequest.value_mask &= ~(CWWidth | CWHeight);
  wm_->HandleEvent(&event);
  EXPECT_EQ(orig_width, info->width);
  EXPECT_EQ(orig_height, info->height);

  // Now turn on the width bit and check that it gets applied.
  event.xconfigurerequest.value_mask |= CWWidth;
  wm_->HandleEvent(&event);
  EXPECT_EQ(new_width, info->width);
  EXPECT_EQ(orig_height, info->height);

  // Turn on the height bit as well.
  event.xconfigurerequest.value_mask |= CWHeight;
  wm_->HandleEvent(&event);
  EXPECT_EQ(new_width, info->width);
  EXPECT_EQ(new_height, info->height);
}

TEST_F(WindowManagerTest, ResizeScreen) {
  // Look up EWMH atoms relating to the screen size.
  XAtom geometry_atom = None;
  ASSERT_TRUE(xconn_->GetAtom("_NET_DESKTOP_GEOMETRY", &geometry_atom));
  XAtom workarea_atom = None;
  ASSERT_TRUE(xconn_->GetAtom("_NET_WORKAREA", &workarea_atom));

  XWindow root_xid = xconn_->GetRootWindow();
  MockXConnection::WindowInfo* root_info = xconn_->GetWindowInfoOrDie(root_xid);

  // Check that they're set correctly.
  TestIntArrayProperty(root_xid, geometry_atom, 2,
                       root_info->width, root_info->height);
  TestIntArrayProperty(root_xid, workarea_atom, 4,
                       0, 0, root_info->width, root_info->height);

  // Set up a background Actor.
  Compositor::Actor* background = compositor_->CreateRectangle(
      Compositor::Color(0xff, 0xff, 0xff),
      Compositor::Color(0xff, 0xff, 0xff), 0);
  background->SetSize(root_info->width, root_info->height);
  wm_->SetBackgroundActor(background);

  int new_width = root_info->width / 2;
  int new_height = root_info->height / 2;

  // Resize the root and compositing overlay windows to half their size.
  root_info->width = new_width;
  root_info->height = new_height;
  MockXConnection::WindowInfo* composite_info =
      xconn_->GetWindowInfoOrDie(xconn_->GetCompositingOverlayWindow(root_xid));
  composite_info->width = new_width;
  composite_info->height = new_height;

  // Send the WM an event saying that the screen has been resized.
  XEvent event;
  xconn_->InitConfigureNotifyEvent(&event, root_xid);
  wm_->HandleEvent(&event);

  EXPECT_EQ(new_width, wm_->width());
  EXPECT_EQ(new_height, wm_->height());
  EXPECT_EQ(new_width, wm_->stage()->GetWidth());
  EXPECT_EQ(new_height, wm_->stage()->GetHeight());

  EXPECT_EQ(0, wm_->layout_manager_->x());
  EXPECT_EQ(0, wm_->layout_manager_->y());
  EXPECT_EQ(new_width, wm_->layout_manager_->width());
  EXPECT_EQ(new_height, wm_->layout_manager_->height());

  // EWMH properties on the root window should be updated as well.
  TestIntArrayProperty(root_xid, geometry_atom, 2, new_width, new_height);
  TestIntArrayProperty(root_xid, workarea_atom, 4,
                       0, 0, new_width, new_height);

  // The background window should be resized too.
  MockXConnection::WindowInfo* background_info =
      xconn_->GetWindowInfoOrDie(wm_->background_xid());
  EXPECT_EQ(0, background_info->x);
  EXPECT_EQ(0, background_info->y);
  EXPECT_EQ(new_width, background_info->width);
  EXPECT_EQ(new_height, background_info->height);
  EXPECT_EQ(
      static_cast<int>(
          new_width * WindowManager::kBackgroundExpansionFactor + 0.5f),
      wm_->background_->GetWidth());
  EXPECT_EQ(
      static_cast<int>(
          new_height * WindowManager::kBackgroundExpansionFactor + 0.5f),
      wm_->background_->GetHeight());

  // Now check that background config works with different aspects.
  background->SetSize(root_info->width * 2, root_info->height);
  wm_->ConfigureBackground(new_width, new_height);
  EXPECT_EQ(new_width * 2, wm_->background_->GetWidth());
  EXPECT_EQ(new_height, wm_->background_->GetHeight());

  background->SetSize(root_info->width, root_info->height * 2);
  wm_->ConfigureBackground(new_width, new_height);
  EXPECT_EQ(
      static_cast<int>(
          new_width * WindowManager::kBackgroundExpansionFactor + 0.5f),
      wm_->background_->GetWidth());
  EXPECT_EQ(
      static_cast<int>(
          new_height * WindowManager::kBackgroundExpansionFactor * 2 + 0.5f),
      wm_->background_->GetHeight());
}

// Test that the _NET_WORKAREA property on the root window excludes areas
// used for panel docks.
TEST_F(WindowManagerTest, SubtractPanelDocksFromNetWorkareaProperty) {
  // The _NET_WORKAREA property should initially cover the dimensions of
  // the screen.
  XAtom workarea_atom = None;
  ASSERT_TRUE(xconn_->GetAtom("_NET_WORKAREA", &workarea_atom));
  XWindow root_xid = xconn_->GetRootWindow();
  MockXConnection::WindowInfo* root_info = xconn_->GetWindowInfoOrDie(root_xid);
  TestIntArrayProperty(root_xid, workarea_atom, 4,
                       0, 0, root_info->width, root_info->height);

  // Create a panel and drag it to the left so it's attached to the left
  // dock.  The workarea property should leave room on the left side of the
  // screen for the dock.
  Panel* panel = CreateSimplePanel(200, 20, 400);
  SendPanelDraggedMessage(panel, 0, 0);
  SendPanelDragCompleteMessage(panel);
  TestIntArrayProperty(root_xid, workarea_atom, 4,
                       PanelManager::kPanelDockWidth, 0,
                       root_info->width - PanelManager::kPanelDockWidth,
                       root_info->height);

  // Now dock it on the right.
  SendPanelDraggedMessage(panel, root_info->width - 1, 0);
  SendPanelDragCompleteMessage(panel);
  TestIntArrayProperty(root_xid, workarea_atom, 4,
                       0, 0,
                       root_info->width - PanelManager::kPanelDockWidth,
                       root_info->height);

  // After the screen gets resized, the dock should still be taken into
  // account.
  root_info->width += 20;
  root_info->height += 10;
  XEvent event;
  xconn_->InitConfigureNotifyEvent(&event, root_xid);
  wm_->HandleEvent(&event);
  TestIntArrayProperty(root_xid, workarea_atom, 4,
                       0, 0,
                       root_info->width - PanelManager::kPanelDockWidth,
                       root_info->height);
}

// Test that the _NET_CLIENT_LIST and _NET_CLIENT_LIST_STACKING properties
// on the root window get updated correctly.
TEST_F(WindowManagerTest, ClientListProperties) {
  XWindow root_xid = xconn_->GetRootWindow();
  XAtom list_atom = None, stacking_atom = None;
  ASSERT_TRUE(xconn_->GetAtom("_NET_CLIENT_LIST", &list_atom));
  ASSERT_TRUE(xconn_->GetAtom("_NET_CLIENT_LIST_STACKING", &stacking_atom));

  // Both properties should be unset when there aren't any client windows.
  TestIntArrayProperty(root_xid, list_atom, 0);
  TestIntArrayProperty(root_xid, stacking_atom, 0);

  // Create and map a regular window.
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);

  // Both properties should contain just this window.
  TestIntArrayProperty(root_xid, list_atom, 1, xid);
  TestIntArrayProperty(root_xid, stacking_atom, 1, xid);

  // Create and map an override-redirect window.
  XWindow override_redirect_xid =
      xconn_->CreateWindow(
          root_xid,  // parent
          0, 0,      // x, y
          200, 200,  // width, height
          true,      // override_redirect
          false,     // input_only
          0);        // event_mask
  SendInitialEventsForWindow(override_redirect_xid);

  // The override-redirect window shouldn't be included.
  TestIntArrayProperty(root_xid, list_atom, 1, xid);
  TestIntArrayProperty(root_xid, stacking_atom, 1, xid);

  // Create and map a second regular window.
  XWindow xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid2);

  // The second window should appear after the first in _NET_CLIENT_LIST,
  // since it was mapped after it, and after the first in
  // _NET_CLIENT_LIST_STACKING, since it's stacked above it (new windows
  // get stacked above their siblings).
  TestIntArrayProperty(root_xid, list_atom, 2, xid, xid2);
  TestIntArrayProperty(root_xid, stacking_atom, 2, xid, xid2);

  // Raise the override-redirect window above the others.
  ASSERT_TRUE(xconn_->RaiseWindow(override_redirect_xid));
  XEvent event;
  xconn_->InitConfigureNotifyEvent(&event, override_redirect_xid);
  event.xconfigure.above = xid2;
  wm_->HandleEvent(&event);

  // The properties should be unchanged.
  TestIntArrayProperty(root_xid, list_atom, 2, xid, xid2);
  TestIntArrayProperty(root_xid, stacking_atom, 2, xid, xid2);

  // Raise the first window on top of the second window.
  ASSERT_TRUE(xconn_->StackWindow(xid, xid2, true));
  xconn_->InitConfigureNotifyEvent(&event, xid);
  event.xconfigure.above = xid2;
  wm_->HandleEvent(&event);

  // The list property should be unchanged, but the second window should
  // appear first in the stacking property since it's now on the bottom.
  TestIntArrayProperty(root_xid, list_atom, 2, xid, xid2);
  TestIntArrayProperty(root_xid, stacking_atom, 2, xid2, xid);

  // Destroy the first window.
  ASSERT_TRUE(xconn_->DestroyWindow(xid));
  xconn_->InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
  xconn_->InitDestroyWindowEvent(&event, xid);
  wm_->HandleEvent(&event);

  // Both properties should just contain the second window now.
  TestIntArrayProperty(root_xid, list_atom, 1, xid2);
  TestIntArrayProperty(root_xid, stacking_atom, 1, xid2);

  // Tell the window manager that the second window was reparented away.
  XReparentEvent* reparent_event = &(event.xreparent);
  memset(reparent_event, 0, sizeof(*reparent_event));
  reparent_event->type = ReparentNotify;
  reparent_event->window = xid2;
  reparent_event->parent = 324324;  // arbitrary number
  wm_->HandleEvent(&event);

  // The properties should be unset.
  TestIntArrayProperty(root_xid, list_atom, 0);
  TestIntArrayProperty(root_xid, stacking_atom, 0);
}

TEST_F(WindowManagerTest, WmIpcVersion) {
  // BasicWindowManagerTest::SetUp() sends a WM_NOTIFY_IPC_VERSION message
  // automatically, since most tests want something reasonable there.
  // Create a new WindowManager object to work around this.
  CreateAndInitNewWm();

  // We should assume version 1 if we haven't received a message from Chrome.
  EXPECT_EQ(1, wm_->wm_ipc_version());

  // Now send the WM a message telling it that Chrome is using version 3.
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_IPC_VERSION);
  msg.set_param(0, 3);
  SendWmIpcMessage(msg);
  EXPECT_EQ(3, wm_->wm_ipc_version());
}

// Test that all windows get redirected when they're created.
TEST_F(WindowManagerTest, RedirectWindows) {
  // First, create a window that's already mapped when the window manager is
  // started.
  wm_.reset(NULL);
  xconn_.reset(new MockXConnection);
  event_loop_.reset(new EventLoop);
  compositor_.reset(new MockCompositor(xconn_.get()));
  XWindow existing_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* existing_info =
      xconn_->GetWindowInfoOrDie(existing_xid);
  xconn_->MapWindow(existing_xid);
  EXPECT_FALSE(existing_info->redirected);
  CreateAndInitNewWm();

  // Check that the window manager redirected it.
  EXPECT_TRUE(existing_info->redirected);
  Window* existing_win = wm_->GetWindowOrDie(existing_xid);
  MockCompositor::TexturePixmapActor* existing_mock_actor =
      GetMockActorForWindow(existing_win);
  EXPECT_EQ(existing_xid, existing_mock_actor->xid());

  // Now, create a new window, but don't map it yet.  The window manager
  // should've already told the X server to automatically redirect toplevel
  // windows.
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  EXPECT_TRUE(info->redirected);

  XEvent event;
  xconn_->InitCreateWindowEvent(&event, xid);
  wm_->HandleEvent(&event);
  Window* win = wm_->GetWindowOrDie(xid);
  MockCompositor::TexturePixmapActor* mock_actor = GetMockActorForWindow(win);
  EXPECT_EQ(xid, mock_actor->xid());

  // There won't be a MapRequest event for override-redirect windows, but they
  // should still get redirected automatically.
  XWindow override_redirect_xid = xconn_->CreateWindow(
        xconn_->GetRootWindow(),
        10, 20,  // x, y
        30, 40,  // width, height
        true,    // override redirect
        false,   // input only
        0);      // event mask
  MockXConnection::WindowInfo* override_redirect_info =
      xconn_->GetWindowInfoOrDie(override_redirect_xid);
  EXPECT_TRUE(override_redirect_info->redirected);
  xconn_->MapWindow(override_redirect_xid);
  ASSERT_TRUE(override_redirect_info->mapped);

  // Send a CreateNotify event to the window manager.
  xconn_->InitCreateWindowEvent(&event, override_redirect_xid);
  wm_->HandleEvent(&event);
  Window* override_redirect_win = wm_->GetWindowOrDie(override_redirect_xid);
  MockCompositor::TexturePixmapActor* override_redirect_mock_actor =
      GetMockActorForWindow(override_redirect_win);
  EXPECT_EQ(override_redirect_xid, override_redirect_mock_actor->xid());
}

// This tests against a bug where the window manager would fail to handle
// existing panel windows at startup -- see http://crosbug.com/1591.
TEST_F(WindowManagerTest, KeepPanelsAfterRestart) {
  // Create a panel and check that the window manager handles it.
  Panel* panel = CreateSimplePanel(200, 20, 400);
  const XWindow titlebar_xid = panel->titlebar_xid();
  const XWindow content_xid = panel->content_xid();
  const Window* win = wm_->GetWindow(content_xid);
  ASSERT_TRUE(win != NULL);
  EXPECT_EQ(panel, wm_->panel_manager_->panel_bar_->GetPanelByWindow(*win));
  wm_.reset();

  // XConnection::GetChildWindows() returns windows in bottom-to-top order.
  // We want to make sure that the window manager is able to deal with
  // seeing the content window show up before the titlebar window when it
  // asks for all of the existing windows at startup, so stack the content
  // window beneath the titlebar window.
  ASSERT_TRUE(xconn_->StackWindow(content_xid, titlebar_xid, false));

  // Call GetChildWindows() to make sure that the windows are stacked as we
  // intended.
  vector<XWindow> windows;
  ASSERT_TRUE(xconn_->GetChildWindows(xconn_->GetRootWindow(), &windows));
  vector<XWindow>::iterator titlebar_it =
      find(windows.begin(), windows.end(), titlebar_xid);
  ASSERT_TRUE(titlebar_it != windows.end());
  vector<XWindow>::iterator content_it =
      find(windows.begin(), windows.end(), content_xid);
  ASSERT_TRUE(content_it != windows.end());
  ASSERT_TRUE(content_it < titlebar_it);

  // Now create and initialize a new window manager and check that it
  // creates a new Panel object.
  CreateAndInitNewWm();
  win = wm_->GetWindow(content_xid);
  ASSERT_TRUE(win != NULL);
  ASSERT_TRUE(wm_->panel_manager_->panel_bar_->GetPanelByWindow(*win) != NULL);
}

// Makes sure the _CHROME_LOGGED_IN property is interpreted correctly.
TEST_F(WindowManagerTest, LoggedIn) {
  EXPECT_TRUE(wm_->logged_in());
  EXPECT_TRUE(wm_->logged_in_key_bindings_group_->enabled());

  // When the _CHROME_LOGGED_IN property doesn't exist, the window manager
  // should assume that we're not logged in.
  XAtom logged_in_xatom = wm_->GetXAtom(ATOM_CHROME_LOGGED_IN);
  xconn_->DeletePropertyIfExists(xconn_->GetRootWindow(), logged_in_xatom);
  wm_.reset(new WindowManager(event_loop_.get(),
                              xconn_.get(),
                              compositor_.get()));
  CHECK(wm_->Init());
  EXPECT_FALSE(wm_->logged_in());
  EXPECT_FALSE(wm_->logged_in_key_bindings_group_->enabled());

  // Ditto for when it exists but is set to 0.
  SetLoggedInState(false);
  wm_.reset(new WindowManager(event_loop_.get(),
                              xconn_.get(),
                              compositor_.get()));
  CHECK(wm_->Init());
  EXPECT_FALSE(wm_->logged_in());
  EXPECT_FALSE(wm_->logged_in_key_bindings_group_->enabled());

  // Check that we handle property changes too.
  TestEventConsumer ec;
  wm_->event_consumers_.insert(&ec);
  SetLoggedInState(true);
  EXPECT_TRUE(wm_->logged_in());
  EXPECT_TRUE(wm_->logged_in_key_bindings_group_->enabled());
  EXPECT_EQ(1, ec.num_logged_in_state_changes());

  ec.reset_stats();
  SetLoggedInState(false);
  EXPECT_FALSE(wm_->logged_in());
  EXPECT_FALSE(wm_->logged_in_key_bindings_group_->enabled());
  EXPECT_EQ(1, ec.num_logged_in_state_changes());
}

// Test that the window manager refreshes the keyboard map when it gets a
// MappingNotify event.
TEST_F(WindowManagerTest, HandleMappingNotify) {
  // Check that a grab has been installed for an arbitrary key binding
  // (Ctrl-Alt-l).
  EXPECT_EQ(0, xconn_->num_keymap_refreshes());
  const KeyCode old_keycode = xconn_->GetKeyCodeFromKeySym(XK_l);
  EXPECT_TRUE(xconn_->KeyIsGrabbed(old_keycode, ControlMask | Mod1Mask));

  // Now remap the 'l' key and give the window manager a MappingNotify event.
  const KeyCode new_keycode = 255;
  EXPECT_FALSE(xconn_->KeyIsGrabbed(new_keycode, ControlMask | Mod1Mask));
  xconn_->RemoveKeyMapping(old_keycode, XK_l);
  xconn_->AddKeyMapping(new_keycode, XK_l);

  XEvent event;
  XMappingEvent* mapping_event = &(event.xmapping);
  memset(mapping_event, 0, sizeof(mapping_event));
  mapping_event->type = MappingNotify;
  mapping_event->request = MappingKeyboard;
  mapping_event->first_keycode = 1;
  mapping_event->count = 6;
  wm_->HandleEvent(&event);

  // The XConnection should've been told to refresh its keymap, and the
  // keyboard grab should be updated (there are more-extensive tests of the
  // latter behavior in KeyBindingsTest).
  EXPECT_EQ(1, xconn_->num_keymap_refreshes());
  EXPECT_TRUE(xconn_->KeyIsGrabbed(new_keycode, ControlMask | Mod1Mask));
  EXPECT_FALSE(xconn_->KeyIsGrabbed(old_keycode, ControlMask | Mod1Mask));
}

// Check that the window manager tells the compositor to discard the pixmap
// for a window when the window is resized or unmapped.  See
// http://crosbug.com/3159.
TEST_F(WindowManagerTest, DiscardPixmapOnUnmap) {
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  SendInitialEventsForWindow(xid);

  Window* win = wm_->GetWindowOrDie(xid);
  MockCompositor::TexturePixmapActor* actor = GetMockActorForWindow(win);
  int initial_pixmap_discards = actor->num_pixmap_discards();

  // Check that the pixmap gets discarded when the window gets resized.
  XEvent event;
  info->width += 10;
  xconn_->InitConfigureNotifyEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(initial_pixmap_discards + 1, actor->num_pixmap_discards());

  // We should try to discard it when the window is unmapped, too.
  xconn_->InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(initial_pixmap_discards + 2, actor->num_pixmap_discards());
}

// Test that we switch log files after the user logs in.
TEST_F(WindowManagerTest, StartNewLogAfterLogin) {
  wm_.reset(NULL);

  ScopedTempDirectory logged_in_dir;
  FLAGS_logged_in_log_dir = logged_in_dir.path().value();
  ScopedTempDirectory logged_out_dir;
  FLAGS_logged_out_log_dir = logged_out_dir.path().value();

  // Make sure that logging is turned on, and pretend like we just started
  // while not logged in.
  SetLoggedInState(false);
  wm_.reset(new WindowManager(event_loop_.get(),
                              xconn_.get(),
                              compositor_.get()));
  wm_->set_initialize_logging(true);
  CHECK(wm_->Init());
  ASSERT_FALSE(wm_->logged_in());

  // The logged-in directory should be empty, but the logged-out directory
  // should contain data.
  EXPECT_EQ(static_cast<off_t>(0),
            GetTotalFileSizeInDirectory(logged_in_dir.path()));
  EXPECT_GT(GetTotalFileSizeInDirectory(logged_out_dir.path()),
            static_cast<off_t>(0));

  // After we log in and send some events, both directories should have data.
  SetLoggedInState(true);
  ASSERT_TRUE(wm_->logged_in());
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);
  off_t logged_in_size = GetTotalFileSizeInDirectory(logged_in_dir.path());
  off_t logged_out_size = GetTotalFileSizeInDirectory(logged_out_dir.path());
  EXPECT_GT(logged_in_size, static_cast<off_t>(0));

  // Send some more events to give the window manager more information to
  // log, and check that the logged-in directory increased in size but the
  // logged-out one remained the same.
  XWindow xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid2);
  EXPECT_GT(GetTotalFileSizeInDirectory(logged_in_dir.path()), logged_in_size);
  EXPECT_EQ(logged_out_size,
            GetTotalFileSizeInDirectory(logged_out_dir.path()));
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
