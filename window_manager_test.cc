// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cstdarg>
#include <set>
#include <tr1/memory>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/file_util.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/compositor.h"
#include "window_manager/event_consumer.h"
#include "window_manager/event_loop.h"
#include "window_manager/geometry.h"
#include "window_manager/layout_manager.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/panel.h"
#include "window_manager/panel_bar.h"
#include "window_manager/panel_manager.h"
#include "window_manager/shadow.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

// From window_manager.cc.
DECLARE_bool(unredirect_fullscreen_window);
DECLARE_string(logged_in_log_dir);
DECLARE_string(logged_out_log_dir);

using file_util::FileEnumerator;
using std::find;
using std::set;
using std::string;
using std::tr1::shared_ptr;
using std::vector;
using window_manager::util::GetCurrentTimeSec;
using window_manager::util::SetCurrentTimeForTest;

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
  SetLoggedInState(true);
  RegisterCommonKeySyms();
  event_loop_.reset(new EventLoop);
  compositor_.reset(new MockCompositor(xconn_.get()));
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  xconn_->MapWindow(xid);

  CreateAndInitNewWm();
  Window* win = wm_->GetWindowOrDie(xid);
  EXPECT_TRUE(win->mapped());
  EXPECT_TRUE(GetMockActorForWindow(win)->is_shown());

  // Now test the case where the window starts out unmapped and
  // WindowManager misses the CreateNotify event but receives the
  // MapRequest (and subsequent MapNotify).
  wm_.reset(NULL);
  xconn_.reset(new MockXConnection);
  SetLoggedInState(true);
  RegisterCommonKeySyms();
  event_loop_.reset(new EventLoop);
  compositor_.reset(new MockCompositor(xconn_.get()));
  xid = CreateSimpleWindow();
  info = xconn_->GetWindowInfoOrDie(xid);

  CreateAndInitNewWm();
  EXPECT_FALSE(info->mapped);
  win = wm_->GetWindowOrDie(xid);
  EXPECT_FALSE(win->mapped());
  EXPECT_FALSE(GetMockActorForWindow(win)->is_shown());

  XEvent event;
  xconn_->InitMapRequestEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(info->mapped);

  xconn_->InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(win->mapped());
  EXPECT_TRUE(GetMockActorForWindow(win)->is_shown());

  // Finally, test the typical case where a window is created after
  // WindowManager has been initialized.
  wm_.reset(NULL);
  xconn_.reset(new MockXConnection);
  SetLoggedInState(true);
  RegisterCommonKeySyms();
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
  EXPECT_FALSE(GetMockActorForWindow(win)->is_shown());

  xconn_->InitMapRequestEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(info->mapped);
  EXPECT_TRUE(win->mapped());

  xconn_->InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(GetMockActorForWindow(win)->is_shown());
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
        0, 0);   // event mask, visual
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
  EXPECT_TRUE(GetMockActorForWindow(win)->is_shown());

  XWindow xid2 = xconn_->CreateWindow(
        xconn_->GetRootWindow(),
        10, 20,  // x, y
        30, 40,  // width, height
        true,    // override redirect
        false,   // input only
        0, 0);   // event mask, visual
  MockXConnection::WindowInfo* info2 = xconn_->GetWindowInfoOrDie(xid2);

  xconn_->InitCreateWindowEvent(&event, xid2);
  wm_->HandleEvent(&event);
  xconn_->MapWindow(xid2);
  ASSERT_TRUE(info2->mapped);
  xconn_->InitMapEvent(&event, xid2);
  wm_->HandleEvent(&event);

  Window* win2 = wm_->GetWindowOrDie(xid2);
  EXPECT_TRUE(GetMockActorForWindow(win2)->is_shown());
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

  // Create a window that'll get mapped by LayoutManager.  Send two
  // MapRequests for it (see http://crosbug.com/4176), and check that our
  // event consumer only gets notified about the first one.
  ec.reset_stats();
  XWindow xid4 = CreateSimpleWindow();
  xconn_->InitCreateWindowEvent(&event, xid4);
  wm_->HandleEvent(&event);
  xconn_->InitMapRequestEvent(&event, xid4);
  wm_->HandleEvent(&event);
  xconn_->InitMapRequestEvent(&event, xid4);
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

  // Use the _NET_WM_WINDOW_TYPE_MENU hint to make the windows have shadows.
  const XAtom win_type_xatom = xconn_->GetAtomOrDie("_NET_WM_WINDOW_TYPE");
  const XAtom atom_xatom = xconn_->GetAtomOrDie("ATOM");
  const XAtom menu_xatom = xconn_->GetAtomOrDie("_NET_WM_WINDOW_TYPE_MENU");

  // Create two override-redirect windows and map them both.
  XWindow xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      10, 20,  // x, y
      30, 40,  // width, height
      true,    // override redirect
      false,   // input only
      0, 0);   // event mask, visual
  xconn_->SetIntProperty(xid, win_type_xatom, atom_xatom, menu_xatom);
  xconn_->MapWindow(xid);
  SendInitialEventsForWindow(xid);
  Window* win = wm_->GetWindowOrDie(xid);
  ASSERT_TRUE(win->shadow() != NULL);

  XWindow xid2 = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      10, 20,  // x, y
      30, 40,  // width, height
      true,    // override redirect
      false,   // input only
      0, 0);   // event mask, visual
  xconn_->SetIntProperty(xid2, win_type_xatom, atom_xatom, menu_xatom);
  xconn_->MapWindow(xid2);
  SendInitialEventsForWindow(xid2);
  Window* win2 = wm_->GetWindowOrDie(xid2);
  ASSERT_TRUE(win2->shadow() != NULL);

  // The second window should initially be stacked above the first, and
  // each window's shadow should be stacked under the window.
  EXPECT_LT(stage->GetStackingIndex(win2->actor()),
            stage->GetStackingIndex(win2->shadow()->group()));
  EXPECT_LT(stage->GetStackingIndex(win2->shadow()->group()),
            stage->GetStackingIndex(win->actor()));
  EXPECT_LT(stage->GetStackingIndex(win->actor()),
            stage->GetStackingIndex(win->shadow()->group()));

  // Send a message saying that the first window is on top of the second.
  xconn_->StackWindow(xid, xid2, true);
  xconn_->InitConfigureNotifyEvent(&event, xid);
  event.xconfigure.above = xid2;
  wm_->HandleEvent(&event);

  EXPECT_LT(stage->GetStackingIndex(win->actor()),
            stage->GetStackingIndex(win->shadow()->group()));
  EXPECT_LT(stage->GetStackingIndex(win->shadow()->group()),
            stage->GetStackingIndex(win2->actor()));
  EXPECT_LT(stage->GetStackingIndex(win2->actor()),
            stage->GetStackingIndex(win2->shadow()->group()));
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
      0, 0);   // event mask, visual
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
          StackingManager::LAYER_FULLSCREEN_WINDOW];
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
  Panel* panel = CreatePanel(200, 20, 400);
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
          0, 0);     // event mask, visual
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
  SetLoggedInState(true);
  RegisterCommonKeySyms();
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
  EXPECT_TRUE(existing_mock_actor->pixmap() != 0);

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
  EXPECT_EQ(0, mock_actor->pixmap());

  xconn_->InitMapRequestEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(win->mapped());
  EXPECT_TRUE(existing_mock_actor->pixmap() != 0);

  // There won't be a MapRequest event for override-redirect windows, but they
  // should still get redirected automatically.
  XWindow override_redirect_xid = xconn_->CreateWindow(
        xconn_->GetRootWindow(),
        10, 20,  // x, y
        30, 40,  // width, height
        true,    // override redirect
        false,   // input only
        0, 0);   // event mask, visual
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
  EXPECT_EQ(0, override_redirect_mock_actor->pixmap());
  EXPECT_FALSE(override_redirect_win->mapped());

  xconn_->InitMapEvent(&event, override_redirect_xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(override_redirect_win->mapped());
  EXPECT_TRUE(override_redirect_mock_actor->pixmap() != 0);
}

// This tests against a bug where the window manager would fail to handle
// existing panel windows at startup -- see http://crosbug.com/1591.
TEST_F(WindowManagerTest, KeepPanelsAfterRestart) {
  // Create a panel and check that the window manager handles it.
  Panel* panel = CreatePanel(200, 20, 400);
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
  XAtom logged_in_xatom = xconn_->GetAtomOrDie("_CHROME_LOGGED_IN");
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

  // We should ignore logged-in to not-logged-in transitions.
  ec.reset_stats();
  SetLoggedInState(false);
  EXPECT_TRUE(wm_->logged_in());
  EXPECT_TRUE(wm_->logged_in_key_bindings_group_->enabled());
  EXPECT_EQ(0, ec.num_logged_in_state_changes());
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

// Check that the window manager tells the Window class to tell the
// compositor to discard the pixmap for a window when the window is resized
// or remapped.  See http://crosbug.com/3159.
TEST_F(WindowManagerTest, FetchNewPixmap) {
  XWindow xid = xconn_->CreateWindow(
        xconn_->GetRootWindow(),
        10, 20,  // x, y
        30, 40,  // width, height
        true,    // override redirect
        false,   // input only
        0, 0);   // event mask, visual
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  xconn_->MapWindow(xid);
  ASSERT_TRUE(info->mapped);
  SendInitialEventsForWindow(xid);

  Window* win = wm_->GetWindowOrDie(xid);
  MockCompositor::TexturePixmapActor* actor = GetMockActorForWindow(win);
  EXPECT_TRUE(actor->pixmap() != 0);
  MockXConnection::PixmapInfo* pixmap_info =
      xconn_->GetPixmapInfo(actor->pixmap());
  ASSERT_TRUE(pixmap_info != NULL);
  EXPECT_EQ(info->width, pixmap_info->width);
  EXPECT_EQ(info->height, pixmap_info->height);

  // Check that the pixmap gets reset when the window gets resized.
  XID prev_pixmap = actor->pixmap();
  ASSERT_TRUE(xconn_->ResizeWindow(xid, info->width + 10, info->height));
  XEvent event;
  xconn_->InitConfigureNotifyEvent(&event, xid);
  wm_->HandleEvent(&event);

  EXPECT_NE(prev_pixmap, actor->pixmap());
  pixmap_info = xconn_->GetPixmapInfo(actor->pixmap());
  ASSERT_TRUE(pixmap_info != NULL);
  EXPECT_EQ(info->width, pixmap_info->width);
  EXPECT_EQ(info->height, pixmap_info->height);

  // We should reset it when the window is remapped, too (but we should
  // continue using the old pixmap until we actually see the window get
  // mapped again).
  prev_pixmap = actor->pixmap();
  xconn_->InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(prev_pixmap, actor->pixmap());

  xconn_->InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_NE(prev_pixmap, actor->pixmap());
  pixmap_info = xconn_->GetPixmapInfo(actor->pixmap());
  ASSERT_TRUE(pixmap_info != NULL);
  EXPECT_EQ(info->width, pixmap_info->width);
  EXPECT_EQ(info->height, pixmap_info->height);
}

// Test that we switch log files after the user logs in.
TEST_F(WindowManagerTest, StartNewLogAfterLogin) {
  wm_.reset(NULL);

  ScopedTempDirectory logged_in_dir;
  AutoReset<string> logged_in_flag_resetter(
      &FLAGS_logged_in_log_dir, logged_in_dir.path().value());

  ScopedTempDirectory logged_out_dir;
  AutoReset<string> logged_out_flag_resetter(
      &FLAGS_logged_out_log_dir, logged_out_dir.path().value());

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

// Check that we don't display drop shadows for most types of
// override-redirect windows.
TEST_F(WindowManagerTest, OverrideRedirectShadows) {
  XAtom win_type_xatom = xconn_->GetAtomOrDie("_NET_WM_WINDOW_TYPE");
  XAtom atom_xatom = xconn_->GetAtomOrDie("ATOM");
  XAtom menu_xatom = xconn_->GetAtomOrDie("_NET_WM_WINDOW_TYPE_MENU");
  XAtom popup_xatom = xconn_->GetAtomOrDie("_NET_WM_WINDOW_TYPE_POPUP_MENU");

  // An override-redirect window with no _NET_WM_WINDOW_TYPE property
  // shouldn't get a shadow.
  const XWindow root = xconn_->GetRootWindow();
  XWindow xid1 = xconn_->CreateWindow(root, 0, 0, 10, 10, true, false, 0, 0);
  ASSERT_TRUE(xconn_->MapWindow(xid1));
  SendInitialEventsForWindow(xid1);
  EXPECT_TRUE(wm_->GetWindowOrDie(xid1)->shadow() == NULL);

  // _NET_WM_WINDOW_TYPE_MENU (or several other menu-related types) should
  // result in a shadow getting shown.
  XWindow xid2 = xconn_->CreateWindow(root, 0, 0, 10, 10, true, false, 0, 0);
  xconn_->SetIntProperty(xid2, win_type_xatom, atom_xatom, menu_xatom);
  ASSERT_TRUE(xconn_->MapWindow(xid2));
  SendInitialEventsForWindow(xid2);
  ASSERT_TRUE(wm_->GetWindowOrDie(xid2)->shadow() != NULL);
  EXPECT_TRUE(wm_->GetWindowOrDie(xid2)->shadow()->is_shown());

  XAtom normal_xatom = 0;
  ASSERT_TRUE(xconn_->GetAtom("_NET_WM_WINDOW_TYPE_NORMAL", &normal_xatom));

  // A non-menu type should result in no shadow getting shown...
  XWindow xid3 = xconn_->CreateWindow(root, 0, 0, 10, 10, true, false, 0, 0);
  xconn_->SetIntProperty(xid3, win_type_xatom, atom_xatom, normal_xatom);
  ASSERT_TRUE(xconn_->MapWindow(xid3));
  SendInitialEventsForWindow(xid3);
  EXPECT_TRUE(wm_->GetWindowOrDie(xid3)->shadow() == NULL);

  // ...unless there's another menu type in the property.
  XWindow xid4 = xconn_->CreateWindow(root, 0, 0, 10, 10, true, false, 0, 0);
  vector<int> values;
  values.push_back(normal_xatom);
  values.push_back(popup_xatom);
  xconn_->SetIntArrayProperty(xid4, win_type_xatom, atom_xatom, values);
  ASSERT_TRUE(xconn_->MapWindow(xid4));
  SendInitialEventsForWindow(xid4);
  ASSERT_TRUE(wm_->GetWindowOrDie(xid4)->shadow() != NULL);
  EXPECT_TRUE(wm_->GetWindowOrDie(xid4)->shadow()->is_shown());
}

// Check that we try to guess when is a video is playing by looking at the
// rate and size of damage events, and that we set the _CHROME_VIDEO_TIME
// property on the root window accordingly.
TEST_F(WindowManagerTest, VideoTimeProperty) {
  const time_t start_time = 1000;
  SetCurrentTimeForTest(start_time, 0);
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);

  const XAtom atom = xconn_->GetAtomOrDie("_CHROME_VIDEO_TIME");
  int video_time = 0;
  EXPECT_FALSE(xconn_->GetIntProperty(xconn_->GetRootWindow(),
                                      atom, &video_time));

  // First send damage events at a high-enough framerate, but for regions
  // that are too small to trigger the code.
  XEvent event;
  xconn_->InitDamageNotifyEvent(
      &event, xid, 0, 0,
      Window::kVideoMinWidth - 1, Window::kVideoMinHeight - 1);
  for (int i = 0; i < Window::kVideoMinFramerate + 3; ++i)
    wm_->HandleEvent(&event);
  EXPECT_FALSE(xconn_->GetIntProperty(xconn_->GetRootWindow(),
                                      atom, &video_time));

  // Now send events with larger regions, but send one fewer than the
  // required number of frames.
  xconn_->InitDamageNotifyEvent(
      &event, xid, 0, 0, Window::kVideoMinWidth, Window::kVideoMinHeight);
  for (int i = 0; i < Window::kVideoMinFramerate - 1; ++i)
    wm_->HandleEvent(&event);
  EXPECT_FALSE(xconn_->GetIntProperty(xconn_->GetRootWindow(),
                                      atom, &video_time));

  // After one more frame, we should set the property.
  wm_->HandleEvent(&event);
  EXPECT_TRUE(xconn_->GetIntProperty(xconn_->GetRootWindow(),
                                     atom, &video_time));
  EXPECT_EQ(start_time, video_time);

  // Send a bunch more frames the next second.  We should leave the
  // property alone, since not enough time has passed for us to update it.
  ASSERT_GT(WindowManager::kVideoTimePropertyUpdateSec, 1);
  SetCurrentTimeForTest(start_time + 1, 0);
  for (int i = 0; i < Window::kVideoMinFramerate + 10; ++i)
    wm_->HandleEvent(&event);
  EXPECT_TRUE(xconn_->GetIntProperty(xconn_->GetRootWindow(),
                                     atom, &video_time));
  EXPECT_EQ(start_time, video_time);

  // Wait the minimum required time to update the property and send more
  // frames, but spread them out across two seconds so that the per-second
  // rate isn't high enough.  We should still leave the property alone.
  SetCurrentTimeForTest(
      start_time + WindowManager::kVideoTimePropertyUpdateSec, 0);
  for (int i = 0; i < Window::kVideoMinFramerate - 5; ++i)
    wm_->HandleEvent(&event);
  SetCurrentTimeForTest(
      start_time + WindowManager::kVideoTimePropertyUpdateSec + 1, 0);
  for (int i = 0; i < Window::kVideoMinFramerate - 5; ++i)
    wm_->HandleEvent(&event);
  EXPECT_TRUE(xconn_->GetIntProperty(xconn_->GetRootWindow(),
                                     atom, &video_time));
  EXPECT_EQ(start_time, video_time);

  // Now send some more frames and check that the property is updated.
  for (int i = 0; i < 5; ++i)
    wm_->HandleEvent(&event);
  EXPECT_TRUE(xconn_->GetIntProperty(xconn_->GetRootWindow(),
                                     atom, &video_time));
  EXPECT_EQ(start_time + WindowManager::kVideoTimePropertyUpdateSec + 1,
            video_time);

  // Create a second window, which should move the first window offscreen.
  // Check that we no longer update the property in response to damage
  // events for the offscreen window.
  XWindow xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid2);
  ASSERT_TRUE(WindowIsOffscreen(xid));
  SetCurrentTimeForTest(
      GetCurrentTimeSec() + WindowManager::kVideoTimePropertyUpdateSec + 5, 0);
  for (int i = 0; i < 30; ++i)
    wm_->HandleEvent(&event);
  EXPECT_TRUE(xconn_->GetIntProperty(xconn_->GetRootWindow(),
                                     atom, &video_time));
  EXPECT_EQ(start_time + WindowManager::kVideoTimePropertyUpdateSec + 1,
            video_time);
}

// Test the unredirect fullscreen window optimization.  Check the windows
// get properly directed/unredirected when the fullscreen actor changes.
TEST_F(WindowManagerTest, HandleTopFullscreenActorChange) {
  XWindow xwin1 = xconn_->CreateWindow(
        xconn_->GetRootWindow(),
        0, 0,    // x, y
        wm_->width(), wm_->height(),
        true,    // override redirect
        false,   // input only
        0, 0);   // event mask, visual

  XWindow xwin2 = xconn_->CreateWindow(
        xconn_->GetRootWindow(),
        0, 0,    // x, y
        wm_->width(), wm_->height(),
        true,    // override redirect
        false,   // input only
        0, 0);   // event mask, visual

  MockXConnection::WindowInfo* info1 = xconn_->GetWindowInfoOrDie(xwin1);
  MockXConnection::WindowInfo* info2 = xconn_->GetWindowInfoOrDie(xwin2);
  SendInitialEventsForWindow(xwin1);
  SendInitialEventsForWindow(xwin2);

  MockCompositor::TexturePixmapActor* actor1 = GetMockActorForWindow(
      wm_->GetWindowOrDie(xwin1));
  MockCompositor::TexturePixmapActor* actor2 = GetMockActorForWindow(
      wm_->GetWindowOrDie(xwin2));

  // Move and scale the two windows to fit the screen.
  xconn_->ConfigureWindow(xwin1, 0, 0, wm_->width(), wm_->height());
  xconn_->ConfigureWindow(xwin2, 0, 0, wm_->width(), wm_->height());
  XEvent event;
  xconn_->InitConfigureNotifyEvent(&event, xwin1);
  wm_->HandleEvent(&event);
  xconn_->InitConfigureNotifyEvent(&event, xwin2);
  wm_->HandleEvent(&event);

  // Set up overlay regions for comparison.
  MockXConnection::WindowInfo* overlay_info =
    xconn_->GetWindowInfoOrDie(wm_->overlay_xid_);
  scoped_ptr<ByteMap> expected_overlay(new ByteMap(overlay_info->width,
                                                   overlay_info->height));
  scoped_ptr<ByteMap> actual_overlay(new ByteMap(overlay_info->width,
                                                 overlay_info->height));

  // Make sure no window is unredirected.
  FLAGS_unredirect_fullscreen_window = true;
  EXPECT_TRUE(wm_->unredirected_fullscreen_xid_ == 0);
  EXPECT_TRUE(info1->redirected);
  EXPECT_TRUE(info2->redirected);
  expected_overlay->Clear(0xff);
  xconn_->GetWindowBoundingRegion(wm_->overlay_xid_, actual_overlay.get());
  EXPECT_TRUE(*expected_overlay.get() == *actual_overlay.get());

  // Test transition from no fullscreen actor to have fullscreen actor.
  wm_->HandleTopFullscreenActorChange(actor1);
  EXPECT_EQ(wm_->unredirected_fullscreen_xid_, xwin1);
  // We would expect this method to be posted to the event loop via
  // HandleTopFullscreenActorChange(), but it is called manually here since
  // the event loop isn't started in the tests.
  wm_->DisableCompositing();
  EXPECT_FALSE(info1->redirected);
  EXPECT_TRUE(info2->redirected);
  expected_overlay->Clear(0);
  xconn_->GetWindowBoundingRegion(wm_->overlay_xid_, actual_overlay.get());
  EXPECT_TRUE(*expected_overlay.get() == *actual_overlay.get());

  // Test change from one to another top fullscreen actor.
  wm_->HandleTopFullscreenActorChange(actor2);
  EXPECT_EQ(wm_->unredirected_fullscreen_xid_, xwin2);
  wm_->DisableCompositing();
  EXPECT_TRUE(info1->redirected);
  EXPECT_FALSE(info2->redirected);
  xconn_->GetWindowBoundingRegion(wm_->overlay_xid_, actual_overlay.get());
  EXPECT_TRUE(*expected_overlay.get() == *actual_overlay.get());

  // Test transition from having fullscreen actor to not.
  wm_->HandleTopFullscreenActorChange(NULL);
  EXPECT_TRUE(wm_->unredirected_fullscreen_xid_ == 0);
  EXPECT_TRUE(info1->redirected);
  EXPECT_TRUE(info2->redirected);
  expected_overlay->Clear(0xff);
  xconn_->GetWindowBoundingRegion(wm_->overlay_xid_, actual_overlay.get());
  EXPECT_TRUE(*expected_overlay.get() == *actual_overlay.get());
}

// Test that the window manager forwards F9 ("volume down") to Chrome, and
// that it does so in response to autorepeated events in addition to the
// initial key press.
TEST_F(WindowManagerTest, ForwardSystemKeysToChrome) {
  XWindow toplevel_xid = CreateToplevelWindow(2, 0, 0, 0, 200, 200);
  SendInitialEventsForWindow(toplevel_xid);
  MockXConnection::WindowInfo* toplevel_info =
      xconn_->GetWindowInfoOrDie(toplevel_xid);
  toplevel_info->client_messages.clear();

  XTime timestamp = 10;
  XEvent event;
  xconn_->InitKeyPressEvent(&event,
                            xconn_->GetRootWindow(),
                            xconn_->GetKeyCodeFromKeySym(XK_F9),
                            0,  // modifiers
                            timestamp);
  wm_->HandleEvent(&event);

  event.xkey.time++;
  wm_->HandleEvent(&event);
  event.xkey.time++;
  wm_->HandleEvent(&event);

  event.type = KeyRelease;
  event.xkey.time++;
  wm_->HandleEvent(&event);

  ASSERT_EQ(3, toplevel_info->client_messages.size());
  for (int i = 0; i < 3; ++i) {
    WmIpc::Message msg;
    ASSERT_TRUE(DecodeWmIpcMessage(toplevel_info->client_messages[i], &msg));
    EXPECT_EQ(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_SYSKEY_PRESSED,
              msg.type());
    EXPECT_EQ(chromeos::WM_IPC_SYSTEM_KEY_VOLUME_DOWN, msg.param(0));
  }
}

// Check that WindowManager passes ownership of destroyed windows to
// EventConsumers who asked for them.
TEST_F(WindowManagerTest, DestroyedWindows) {
  TestEventConsumer ec;
  XWindow xid = CreateSimpleWindow();
  wm_->RegisterEventConsumerForDestroyedWindow(xid, &ec);

  SendInitialEventsForWindow(xid);
  Window* win = wm_->GetWindowOrDie(xid);
  win->SetShadowType(Shadow::TYPE_RECTANGULAR);

  Compositor::TexturePixmapActor* actor = win->actor();
  const Shadow* shadow = win->shadow();
  ASSERT_TRUE(shadow != NULL);

  XEvent event;
  xconn_->InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
  xconn_->InitDestroyWindowEvent(&event, xid);
  wm_->HandleEvent(&event);

  // After we destroy the X window, WindowManager should no longer have a
  // Window object tracking it, but our EventConsumer should've received a
  // DestroyedWindow object containing the original actor and shadow.
  EXPECT_TRUE(wm_->GetWindow(xid) == NULL);
  ASSERT_EQ(static_cast<size_t>(1), ec.destroyed_windows().size());
  DestroyedWindow* destroyed_win = ec.destroyed_windows().begin()->get();
  EXPECT_EQ(actor, destroyed_win->actor());
  EXPECT_EQ(shadow, destroyed_win->shadow());
}

// Test that we defer fetching a window's initial pixmap until the client
// tells us that it's been painted, and that we notify EventConsumers when
// we've fetched the pixmap.
TEST_F(WindowManagerTest, NotifyAboutInitialPixmap) {
  TestEventConsumer ec;

  // Create a window that doesn't support the _NET_WM_SYNC_REQUEST
  // protocol.  We should fetch its pixmap as soon as it gets mapped.
  XWindow xid = CreateSimpleWindow();
  wm_->RegisterEventConsumerForWindowEvents(xid, &ec);
  XEvent event;
  xconn_->InitCreateWindowEvent(&event, xid);
  wm_->HandleEvent(&event);
  xconn_->InitMapRequestEvent(&event, xid);
  wm_->HandleEvent(&event);
  ASSERT_TRUE(xconn_->GetWindowInfoOrDie(xid)->mapped);
  EXPECT_TRUE(wm_->GetWindowOrDie(xid)->has_initial_pixmap());
  xconn_->InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(0, ec.num_initial_pixmaps());

  // Create a window that supports _NET_WM_SYNC_REQUEST.
  // Window::has_initial_pixmap() should return false after it's mapped
  // (since we should defer fetching the pixmap until the window says that
  // it's painted it).
  ec.reset_stats();
  XWindow sync_xid = CreateSimpleWindow();
  wm_->RegisterEventConsumerForWindowEvents(sync_xid, &ec);
  ConfigureWindowForSyncRequestProtocol(sync_xid);
  xconn_->InitCreateWindowEvent(&event, sync_xid);
  wm_->HandleEvent(&event);
  Window* sync_win = wm_->GetWindowOrDie(sync_xid);
  xconn_->InitMapRequestEvent(&event, sync_xid);
  wm_->HandleEvent(&event);
  ASSERT_TRUE(xconn_->GetWindowInfoOrDie(sync_xid)->mapped);
  xconn_->InitMapEvent(&event, sync_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(0, ec.num_initial_pixmaps());
  EXPECT_FALSE(sync_win->has_initial_pixmap());

  // Notify the window manager that the pixmap has been painted.
  // has_initial_pixmap() should return true now, and our event consumer
  // should be notified that the pixmap was received.
  SendSyncRequestProtocolAlarm(sync_xid);
  EXPECT_TRUE(sync_win->has_initial_pixmap());
  EXPECT_EQ(1, ec.num_initial_pixmaps());

  // Resize the window and mimic the client syncing with the window manager
  // again, and make sure that we don't re-notify the event consumer about
  // the pixmap.
  ec.reset_stats();
  sync_win->ResizeClient(600, 500, GRAVITY_NORTHWEST);
  SendSyncRequestProtocolAlarm(sync_xid);
  EXPECT_TRUE(sync_win->has_initial_pixmap());
  EXPECT_EQ(0, ec.num_initial_pixmaps());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
