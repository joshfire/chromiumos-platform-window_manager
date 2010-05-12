// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cstdarg>
#include <vector>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

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

using std::find;
using std::string;
using std::vector;

namespace window_manager {

class WindowManagerTest : public BasicWindowManagerTest {};

class TestEventConsumer : public EventConsumer {
 public:
  TestEventConsumer()
      : EventConsumer(),
        num_mapped_windows_(0),
        num_unmapped_windows_(0),
        num_button_presses_(0) {
  }

  int num_mapped_windows() const { return num_mapped_windows_; }
  int num_unmapped_windows() const { return num_unmapped_windows_; }
  int num_button_presses() const { return num_button_presses_; }
  const vector<WmIpc::Message>& chrome_messages() const {
    return chrome_messages_;
  }

  // Begin overridden EventConsumer virtual methods.
  bool IsInputWindow(XWindow xid) { return false; }
  void HandleScreenResize() {}
  bool HandleWindowMapRequest(Window* win) { return false; }
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
  EXPECT_TRUE(dynamic_cast<const MockCompositor::Actor*>(
                  win->actor())->visible());

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
  EXPECT_FALSE(dynamic_cast<const MockCompositor::Actor*>(
                   win->actor())->visible());

  XEvent event;
  MockXConnection::InitMapRequestEvent(&event, *info);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(info->mapped);

  MockXConnection::InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(win->mapped());
  EXPECT_TRUE(dynamic_cast<const MockCompositor::Actor*>(
                  win->actor())->visible());

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

  MockXConnection::InitCreateWindowEvent(&event, *info);
  wm_->HandleEvent(&event);
  EXPECT_FALSE(info->mapped);
  win = wm_->GetWindowOrDie(xid);
  EXPECT_FALSE(win->mapped());
  EXPECT_FALSE(dynamic_cast<const MockCompositor::Actor*>(
                   win->actor())->visible());

  MockXConnection::InitMapRequestEvent(&event, *info);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(info->mapped);
  EXPECT_FALSE(win->mapped());

  MockXConnection::InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(win->mapped());
  EXPECT_TRUE(dynamic_cast<const MockCompositor::Actor*>(
                  win->actor())->visible());
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
  MockXConnection::InitCreateWindowEvent(&event, *info);
  wm_->HandleEvent(&event);
  MockXConnection::InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);

  // Now test the other possibility, where the window isn't mapped on the X
  // server yet when we receive the CreateNotify event.
  Window* win = wm_->GetWindowOrDie(xid);
  EXPECT_TRUE(dynamic_cast<const MockCompositor::Actor*>(
                  win->actor())->visible());

  XWindow xid2 = xconn_->CreateWindow(
        xconn_->GetRootWindow(),
        10, 20,  // x, y
        30, 40,  // width, height
        true,    // override redirect
        false,   // input only
        0);      // event mask
  MockXConnection::WindowInfo* info2 = xconn_->GetWindowInfoOrDie(xid2);

  MockXConnection::InitCreateWindowEvent(&event, *info2);
  wm_->HandleEvent(&event);
  xconn_->MapWindow(xid2);
  ASSERT_TRUE(info2->mapped);
  MockXConnection::InitMapEvent(&event, xid2);
  wm_->HandleEvent(&event);

  Window* win2 = wm_->GetWindowOrDie(xid2);
  EXPECT_TRUE(dynamic_cast<const MockCompositor::Actor*>(
                  win2->actor())->visible());
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

  // This window needs to have override redirect set; otherwise the
  // LayoutManager will claim ownership of the button press in the mistaken
  // belief that it's the result of a button grab on an unfocused window.
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  info->override_redirect = true;
  wm_->RegisterEventConsumerForWindowEvents(xid, &ec);

  XEvent event;
  MockXConnection::InitCreateWindowEvent(&event, *info);
  wm_->HandleEvent(&event);

  // Send various events to the WindowManager object and check that they
  // get forwarded to our EventConsumer.
  MockXConnection::InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);

  MockXConnection::InitButtonPressEvent(&event, *info, 5, 5, 1);
  wm_->HandleEvent(&event);

  MockXConnection::InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
  wm_->UnregisterEventConsumerForWindowEvents(xid, &ec);

  EXPECT_EQ(1, ec.num_mapped_windows());
  EXPECT_EQ(1, ec.num_button_presses());
  EXPECT_EQ(1, ec.num_unmapped_windows());

  // It's a bit of a stretch to include this in this test, but check that the
  // window manager didn't do anything to the window (since it's an
  // override-redirect window).
  EXPECT_FALSE(info->changed);

  // Create a second window.
  XWindow xid2 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info2 = xconn_->GetWindowInfoOrDie(xid2);
  info2->override_redirect = true;

  // Send events similar to the first window's.
  MockXConnection::InitCreateWindowEvent(&event, *info2);
  wm_->HandleEvent(&event);
  MockXConnection::InitMapEvent(&event, xid2);
  wm_->HandleEvent(&event);
  MockXConnection::InitButtonPressEvent(&event, *info2, 5, 5, 1);
  wm_->HandleEvent(&event);
  MockXConnection::InitUnmapEvent(&event, xid2);
  wm_->HandleEvent(&event);

  // The event consumer should've heard about the second window being
  // mapped and unmapped, but not about the button press (since it never
  // registered interest in the window).
  EXPECT_EQ(2, ec.num_mapped_windows());
  EXPECT_EQ(1, ec.num_button_presses());
  EXPECT_EQ(2, ec.num_unmapped_windows());
}

// Check that windows that get reparented away from the root (like Flash
// plugin windows) get unredirected.
TEST_F(WindowManagerTest, Reparent) {
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  ASSERT_TRUE(info->redirected);

  XEvent event;
  MockXConnection::InitCreateWindowEvent(&event, *info);
  wm_->HandleEvent(&event);
  MockXConnection::InitMapRequestEvent(&event, *info);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(info->mapped);
  MockXConnection::InitMapEvent(&event, xid);
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
  XEvent event;

  // Create two override-redirect windows and map them both.
  XWindow xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      10, 20,  // x, y
      30, 40,  // width, height
      true,    // override redirect
      false,   // input only
      0);      // event mask
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  MockXConnection::InitCreateWindowEvent(&event, *info);
  wm_->HandleEvent(&event);
  xconn_->MapWindow(xid);
  MockXConnection::InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  Window* win = wm_->GetWindowOrDie(xid);

  XWindow xid2 = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      10, 20,  // x, y
      30, 40,  // width, height
      true,    // override redirect
      false,   // input only
      0);      // event mask
  MockXConnection::WindowInfo* info2 = xconn_->GetWindowInfoOrDie(xid2);
  MockXConnection::InitCreateWindowEvent(&event, *info2);
  wm_->HandleEvent(&event);
  xconn_->MapWindow(xid2);
  MockXConnection::InitMapEvent(&event, xid2);
  wm_->HandleEvent(&event);
  Window* win2 = wm_->GetWindowOrDie(xid2);

  // Send a ConfigureNotify saying that the second window has been stacked
  // on top of the first and then make sure that the compositing actors are
  // stacked in the same manner.
  MockXConnection::InitConfigureNotifyEvent(&event, *info2);
  event.xconfigure.above = xid;
  wm_->HandleEvent(&event);
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  EXPECT_LT(stage->GetStackingIndex(win2->actor()),
            stage->GetStackingIndex(win->actor()));

  // Now send a message saying that the first window is on top of the second.
  MockXConnection::InitConfigureNotifyEvent(&event, *info);
  event.xconfigure.above = xid2;
  wm_->HandleEvent(&event);
  EXPECT_LT(stage->GetStackingIndex(win->actor()),
            stage->GetStackingIndex(win2->actor()));
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
  MockXConnection::InitCreateWindowEvent(&event, *info);
  wm_->HandleEvent(&event);

  // Send a ConfigureRequest event with its width and height fields masked
  // out, and check that the new width and height values are ignored.
  const int new_width = orig_width * 2;
  const int new_height = orig_height * 2;
  MockXConnection::InitConfigureRequestEvent(
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

  // Setup a background Actor.
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
  MockXConnection::InitConfigureNotifyEvent(&event, *root_info);
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
      static_cast<int>(new_width * WindowManager::kBackgroundExpansionFactor),
      wm_->background_->GetWidth());
  EXPECT_EQ(
      static_cast<int>(new_height * WindowManager::kBackgroundExpansionFactor),
      wm_->background_->GetHeight());

  // Now check that background config works with different aspects.
  background->SetSize(root_info->width * 2, root_info->height);
  wm_->ConfigureBackground(new_width, new_height);
  EXPECT_EQ(
      static_cast<int>(
          new_width * WindowManager::kBackgroundExpansionFactor * 2),
      wm_->background_->GetWidth());
  EXPECT_EQ(
      static_cast<int>(new_height * WindowManager::kBackgroundExpansionFactor),
      wm_->background_->GetHeight());

  background->SetSize(root_info->width, root_info->height * 2);
  wm_->ConfigureBackground(new_width, new_height);
  EXPECT_EQ(
      static_cast<int>(new_width * WindowManager::kBackgroundExpansionFactor),
      wm_->background_->GetWidth());
  EXPECT_EQ(
      static_cast<int>(
          new_height * WindowManager::kBackgroundExpansionFactor * 2),
      wm_->background_->GetHeight());
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
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
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
  MockXConnection::WindowInfo* override_redirect_info =
      xconn_->GetWindowInfoOrDie(override_redirect_xid);
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
  MockXConnection::InitConfigureNotifyEvent(&event, *override_redirect_info);
  event.xconfigure.above = xid2;
  wm_->HandleEvent(&event);

  // The properties should be unchanged.
  TestIntArrayProperty(root_xid, list_atom, 2, xid, xid2);
  TestIntArrayProperty(root_xid, stacking_atom, 2, xid, xid2);

  // Raise the first window on top of the second window.
  ASSERT_TRUE(xconn_->StackWindow(xid, xid2, true));
  MockXConnection::InitConfigureNotifyEvent(&event, *info);
  event.xconfigure.above = xid2;
  wm_->HandleEvent(&event);

  // The list property should be unchanged, but the second window should
  // appear first in the stacking property since it's now on the bottom.
  TestIntArrayProperty(root_xid, list_atom, 2, xid, xid2);
  TestIntArrayProperty(root_xid, stacking_atom, 2, xid2, xid);

  // Destroy the first window.
  ASSERT_TRUE(xconn_->DestroyWindow(xid));
  MockXConnection::InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
  MockXConnection::InitDestroyWindowEvent(&event, xid);
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

  // We should assume version 0 if we haven't received a message from Chrome.
  EXPECT_EQ(0, wm_->wm_ipc_version());

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
      dynamic_cast<MockCompositor::TexturePixmapActor*>(existing_win->actor());
  CHECK(existing_mock_actor);
  EXPECT_EQ(existing_xid, existing_mock_actor->xid());

  // Now, create a new window, but don't map it yet.  The window manager
  // should've already told the X server to automatically redirect toplevel
  // windows.
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  EXPECT_TRUE(info->redirected);

  XEvent event;
  MockXConnection::InitCreateWindowEvent(&event, *info);
  wm_->HandleEvent(&event);
  Window* win = wm_->GetWindowOrDie(xid);
  MockCompositor::TexturePixmapActor* mock_actor =
      dynamic_cast<MockCompositor::TexturePixmapActor*>(win->actor());
  CHECK(mock_actor);
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
  MockXConnection::InitCreateWindowEvent(&event, *override_redirect_info);
  wm_->HandleEvent(&event);
  Window* override_redirect_win = wm_->GetWindowOrDie(override_redirect_xid);
  MockCompositor::TexturePixmapActor* override_redirect_mock_actor =
      dynamic_cast<MockCompositor::TexturePixmapActor*>(
          override_redirect_win->actor());
  CHECK(override_redirect_mock_actor);
  EXPECT_EQ(override_redirect_xid, override_redirect_mock_actor->xid());
}

// This tests against a bug where the window manager would fail to handle
// existing panel windows at startup -- see http://crosbug.com/1591.
TEST_F(WindowManagerTest, KeepPanelsAfterRestart) {
  // Create a panel and check that the window manager handles it.
  Panel* panel = CreatePanel(200, 20, 400, true);
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

// Makes sure the user is logged in by default, and that constructing a
// WindowManager object with 'logged_in' set to false disables some key
// bindings.
TEST_F(WindowManagerTest, LoggedIn) {
  EXPECT_TRUE(wm_->logged_in());
  EXPECT_TRUE(wm_->logged_in_key_bindings_group_->enabled());
  EXPECT_TRUE(wm_->layout_manager_->key_bindings_enabled());

  wm_.reset(new WindowManager(event_loop_.get(),
                              xconn_.get(),
                              compositor_.get(),
                              false));  // logged_in=false
  CHECK(wm_->Init());
  EXPECT_FALSE(wm_->logged_in());
  EXPECT_FALSE(wm_->logged_in_key_bindings_group_->enabled());
  EXPECT_FALSE(wm_->layout_manager_->key_bindings_enabled());
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
  MockCompositor::TexturePixmapActor* actor =
      dynamic_cast<MockCompositor::TexturePixmapActor*>(win->actor());
  CHECK(actor);
  int initial_pixmap_discards = actor->num_pixmap_discards();

  // Check that the pixmap gets discarded when the window gets resized.
  XEvent event;
  info->width += 10;
  MockXConnection::InitConfigureNotifyEvent(&event, *info);
  wm_->HandleEvent(&event);
  EXPECT_EQ(initial_pixmap_discards + 1, actor->num_pixmap_discards());

  // We should try to discard it when the window is unmapped, too.
  MockXConnection::InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(initial_pixmap_discards + 2, actor->num_pixmap_discards());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
