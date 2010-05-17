// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "window_manager/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/layout_manager.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/panel.h"
#include "window_manager/snapshot_window.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/test_lib.h"
#include "window_manager/toplevel_window.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::vector;
using std::tr1::shared_ptr;

namespace window_manager {

class LayoutManagerTest : public BasicWindowManagerTest {
 protected:
  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();

    lm_ = wm_->layout_manager_.get();
  }
  LayoutManager* lm_;  // points to wm_'s copy
};

TEST_F(LayoutManagerTest, Basic) {
  XWindow xid1 = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      100, 100,  // x, y
      640, 480,  // width, height
      false,     // override redirect
      false,     // input only
      0);        // event mask
  wm_->TrackWindow(xid1, false);  // override_redirect=false

  Window* win1 = wm_->GetWindowOrDie(xid1);
  win1->MapClient();

  lm_->SetMode(LayoutManager::MODE_ACTIVE);
  lm_->HandleWindowMap(win1);
  int x = lm_->x() + 0.5 * (lm_->width() - win1->client_width());
  int y = lm_->y() + 0.5 * (lm_->height() - win1->client_height());
  EXPECT_EQ(x, win1->client_x());
  EXPECT_EQ(y, win1->client_y());
  EXPECT_EQ(x, win1->composited_x());
  EXPECT_EQ(y, win1->composited_y());
  EXPECT_DOUBLE_EQ(1.0, win1->composited_scale_x());
  EXPECT_DOUBLE_EQ(1.0, win1->composited_scale_y());
  EXPECT_DOUBLE_EQ(1.0, win1->composited_opacity());

  // Now create two more windows and map them.
  XWindow xid2 = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      100, 100,  // x, y
      640, 480,  // width, height
      false,     // override redirect
      false,     // input only
      0);        // event mask
  wm_->TrackWindow(xid2, false);  // override_redirect=false
  Window* win2 = wm_->GetWindowOrDie(xid2);
  win2->MapClient();
  lm_->HandleWindowMap(win2);

  XWindow xid3 = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      100, 100,  // x, y
      640, 480,  // width, height
      false,     // override redirect
      false,     // input only
      0);        // event mask
  wm_->TrackWindow(xid3, false);  // override_redirect=false
  Window* win3 = wm_->GetWindowOrDie(xid3);
  win3->MapClient();
  lm_->HandleWindowMap(win3);

  // The third window should be onscreen now, and the first and second
  // windows should be offscreen.
  EXPECT_EQ(wm_->width(), win1->client_x());
  EXPECT_EQ(wm_->height(), win1->client_y());
  EXPECT_EQ(wm_->width(), win2->client_x());
  EXPECT_EQ(wm_->height(), win2->client_y());
  EXPECT_EQ(x, win3->client_x());
  EXPECT_EQ(y, win3->client_y());
  EXPECT_EQ(x, win3->composited_x());
  EXPECT_EQ(y, win3->composited_y());
  // TODO: Test composited position.  Maybe just check that it's offscreen?

  // After cycling the windows, the second and third windows should be
  // offscreen and the first window should be centered.
  lm_->CycleCurrentToplevelWindow(true);
  EXPECT_EQ(x, win1->client_x());
  EXPECT_EQ(y, win1->client_y());
  EXPECT_EQ(x, win1->composited_x());
  EXPECT_EQ(y, win1->composited_y());
  EXPECT_EQ(wm_->width(), win2->client_x());
  EXPECT_EQ(wm_->height(), win2->client_y());
  EXPECT_EQ(wm_->width(), win3->client_x());
  EXPECT_EQ(wm_->height(), win3->client_y());
}

TEST_F(LayoutManagerTest, Focus) {
  // Create a window.
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  EXPECT_EQ(None, xconn_->focused_xid());

  // Send a CreateNotify event to the window manager.
  XEvent event;
  MockXConnection::InitCreateWindowEvent(&event, *info);
  wm_->HandleEvent(&event);
  EXPECT_EQ(None, xconn_->focused_xid());
  EXPECT_TRUE(lm_->current_toplevel_ == NULL);

  // The layout manager should activate and focus the window when it gets
  // mapped.  Because the window is focused, it shouldn't have a button
  // grab installed.
  MockXConnection::InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid, xconn_->focused_xid());
  ASSERT_TRUE(lm_->current_toplevel_ != NULL);
  EXPECT_EQ(xid, lm_->current_toplevel_->win()->xid());
  EXPECT_EQ(xid, GetActiveWindowProperty());
  EXPECT_FALSE(info->button_is_grabbed(AnyButton));

  // Now create a second window.
  XWindow xid2 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info2 = xconn_->GetWindowInfoOrDie(xid2);

  // When the second window is created, the first should still be active.
  MockXConnection::InitCreateWindowEvent(&event, *info2);
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid, xconn_->focused_xid());
  ASSERT_TRUE(lm_->current_toplevel_ != NULL);
  EXPECT_EQ(xid, lm_->current_toplevel_->win()->xid());

  // When the second window is mapped, it should become the active window.
  MockXConnection::InitMapEvent(&event, xid2);
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid2, xconn_->focused_xid());
  EXPECT_EQ(xid2, GetActiveWindowProperty());
  ASSERT_TRUE(lm_->current_toplevel_ != NULL);
  EXPECT_EQ(xid2, lm_->current_toplevel_->win()->xid());
  EXPECT_TRUE(info->button_is_grabbed(AnyButton));
  EXPECT_FALSE(info2->button_is_grabbed(AnyButton));

  // Now send a _NET_ACTIVE_WINDOW message asking the window manager to
  // focus the first window.
  MockXConnection::InitClientMessageEvent(
      &event,
      xid,   // window to focus
      wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW),
      1,     // source indication: client app
      CurrentTime,
      xid2,  // currently-active window
      None,
      None);
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid, xconn_->focused_xid());
  ASSERT_TRUE(lm_->current_toplevel_ != NULL);
  EXPECT_EQ(xid, lm_->current_toplevel_->win()->xid());
  EXPECT_EQ(xid, GetActiveWindowProperty());
  EXPECT_FALSE(info->button_is_grabbed(AnyButton));
  EXPECT_TRUE(info2->button_is_grabbed(AnyButton));

  // Unmap the first window and check that the second window gets focused.
  MockXConnection::InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid2, xconn_->focused_xid());
  ASSERT_TRUE(lm_->current_toplevel_ != NULL);
  EXPECT_EQ(xid2, lm_->current_toplevel_->win()->xid());
  EXPECT_EQ(xid2, GetActiveWindowProperty());
  EXPECT_FALSE(info2->button_is_grabbed(AnyButton));
}

TEST_F(LayoutManagerTest, ConfigureTransient) {
  XEvent event;

  // Create and map a toplevel window.
  XWindow owner_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* owner_info =
      xconn_->GetWindowInfoOrDie(owner_xid);
  SendInitialEventsForWindow(owner_xid);
  MockXConnection::InitConfigureNotifyEvent(&event, *owner_info);
  wm_->HandleEvent(&event);

  EXPECT_EQ(0, owner_info->x);
  EXPECT_EQ(0, owner_info->y);
  EXPECT_EQ(lm_->width(), owner_info->width);
  EXPECT_EQ(lm_->height(), owner_info->height);

  // Now create and map a transient window.
  XWindow transient_xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      60, 70,    // x, y
      320, 240,  // width, height
      false,     // override redirect
      false,     // input only
      0);        // event mask
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = owner_xid;
  SendInitialEventsForWindow(transient_xid);

  // The transient window should initially be centered over its owner.
  EXPECT_EQ(owner_info->x + 0.5 * (owner_info->width - transient_info->width),
            transient_info->x);
  EXPECT_EQ(owner_info->y + 0.5 * (owner_info->height - transient_info->height),
            transient_info->y);
  MockXConnection::InitConfigureNotifyEvent(&event, *owner_info);
  wm_->HandleEvent(&event);

  // Now resize the transient window and make sure that it gets re-centered.
  MockXConnection::InitConfigureRequestEvent(
      &event, transient_xid, 0, 0, 400, 300);
  event.xconfigurerequest.value_mask = CWWidth | CWHeight;
  wm_->HandleEvent(&event);
  EXPECT_EQ(400, transient_info->width);
  EXPECT_EQ(300, transient_info->height);
  EXPECT_EQ(owner_info->x + 0.5 * (owner_info->width - transient_info->width),
            transient_info->x);
  EXPECT_EQ(owner_info->y + 0.5 * (owner_info->height - transient_info->height),
            transient_info->y);

  // Send a ConfigureRequest event to move and resize the transient window
  // and make sure that it gets applied.
  MockXConnection::InitConfigureRequestEvent(
      &event, transient_xid, owner_info->x + 20, owner_info->y + 10, 200, 150);
  wm_->HandleEvent(&event);
  EXPECT_EQ(owner_info->x + 20, transient_info->x);
  EXPECT_EQ(owner_info->y + 10, transient_info->y);
  EXPECT_EQ(200, transient_info->width);
  EXPECT_EQ(150, transient_info->height);

  // If we resize the transient window again now, it shouldn't get
  // re-centered (since we explicitly moved it previously).
  MockXConnection::InitConfigureRequestEvent(
      &event, transient_xid, 0, 0, 40, 30);
  event.xconfigurerequest.value_mask = CWWidth | CWHeight;
  wm_->HandleEvent(&event);
  EXPECT_EQ(owner_info->x + 20, transient_info->x);
  EXPECT_EQ(owner_info->y + 10, transient_info->y);
  EXPECT_EQ(40, transient_info->width);
  EXPECT_EQ(30, transient_info->height);

  // Create and map an info bubble window.
  int bubble_x = owner_info->x + 40;
  int bubble_y = owner_info->y + 30;
  XWindow bubble_xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      bubble_x, bubble_y,
      320, 240,  // width, height
      false,     // override redirect
      false,     // input only
      0);        // event mask
  ASSERT_TRUE(wm_->wm_ipc()->SetWindowType(
                  bubble_xid,
                  chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE,
                  NULL));
  MockXConnection::WindowInfo* bubble_info =
      xconn_->GetWindowInfoOrDie(bubble_xid);
  bubble_info->transient_for = owner_xid;
  SendInitialEventsForWindow(bubble_xid);

  // The bubble's initial position should be preserved.
  EXPECT_EQ(bubble_x, bubble_info->x);
  EXPECT_EQ(bubble_y, bubble_info->y);
  MockXConnection::InitConfigureNotifyEvent(&event, *owner_info);
  wm_->HandleEvent(&event);
}

TEST_F(LayoutManagerTest, FocusTransient) {
  // Create a window.
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);

  // Send CreateNotify, MapNotify, and FocusNotify events.
  XEvent event;
  SendInitialEventsForWindow(xid);
  EXPECT_EQ(xid, xconn_->focused_xid());
  EXPECT_FALSE(info->button_is_grabbed(AnyButton));
  EXPECT_EQ(xid, GetActiveWindowProperty());
  EXPECT_TRUE(wm_->GetWindowOrDie(xid)->IsFocused());

  // Now create a transient window.
  XWindow transient_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = xid;

  // Send CreateNotify and MapNotify events for the transient window.
  SendInitialEventsForWindow(transient_xid);

  // We should ask the X server to focus the transient window as soon as it
  // gets mapped.  Also check that we add a passive button grab on the
  // owner window and remove the grab on the transient.
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_TRUE(info->button_is_grabbed(AnyButton));
  EXPECT_FALSE(transient_info->button_is_grabbed(AnyButton));
  EXPECT_FALSE(wm_->GetWindowOrDie(xid)->IsFocused());
  EXPECT_TRUE(wm_->GetWindowOrDie(transient_xid)->IsFocused());

  // _NET_ACTIVE_WINDOW should also be set to the transient window (EWMH is
  // vague about this, but it seems to match what other WMs do).
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());

  // Now simulate a button press on the owner window.
  xconn_->set_pointer_grab_xid(xid);
  MockXConnection::InitButtonPressEvent(
      &event, *info, 0, 0, 1);  // x, y, button
  wm_->HandleEvent(&event);

  // LayoutManager should remove the active pointer grab and try to focus
  // the owner window.  The button grabs should also be updated again.
  EXPECT_EQ(None, xconn_->pointer_grab_xid());
  EXPECT_EQ(xid, xconn_->focused_xid());
  EXPECT_FALSE(info->button_is_grabbed(AnyButton));
  EXPECT_TRUE(transient_info->button_is_grabbed(AnyButton));
  EXPECT_EQ(xid, GetActiveWindowProperty());
  EXPECT_TRUE(wm_->GetWindowOrDie(xid)->IsFocused());
  EXPECT_FALSE(wm_->GetWindowOrDie(transient_xid)->IsFocused());

  // Give the focus back to the transient window.
  xconn_->set_pointer_grab_xid(transient_xid);
  MockXConnection::InitButtonPressEvent(&event, *transient_info, 0, 0, 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());
  EXPECT_FALSE(wm_->GetWindowOrDie(xid)->IsFocused());
  EXPECT_TRUE(wm_->GetWindowOrDie(transient_xid)->IsFocused());

  // Set the transient window as modal.
  MockXConnection::InitClientMessageEvent(
      &event, transient_xid, wm_->GetXAtom(ATOM_NET_WM_STATE),
      1, wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL), None, None, None);
  wm_->HandleEvent(&event);

  // Since it's modal, the transient window should still keep the focus
  // after a button press in the owner window.
  xconn_->set_pointer_grab_xid(xid);
  MockXConnection::InitButtonPressEvent(&event, *info, 0, 0, 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());
  EXPECT_FALSE(wm_->GetWindowOrDie(xid)->IsFocused());
  EXPECT_TRUE(wm_->GetWindowOrDie(transient_xid)->IsFocused());

  // Now create another toplevel window, which we'll switch to
  // automatically.
  XWindow xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid2);
  EXPECT_EQ(xid2, xconn_->focused_xid());
  EXPECT_EQ(xid2, GetActiveWindowProperty());
  EXPECT_FALSE(wm_->GetWindowOrDie(xid)->IsFocused());
  EXPECT_FALSE(wm_->GetWindowOrDie(transient_xid)->IsFocused());
  EXPECT_TRUE(wm_->GetWindowOrDie(xid2)->IsFocused());

  // When we cycle to the first toplevel window, its modal transient
  // window, rather than the toplevel itself, should get the focus.
  lm_->CycleCurrentToplevelWindow(false);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());
  EXPECT_FALSE(wm_->GetWindowOrDie(xid)->IsFocused());
  EXPECT_TRUE(wm_->GetWindowOrDie(transient_xid)->IsFocused());
  EXPECT_FALSE(wm_->GetWindowOrDie(xid2)->IsFocused());

  // Switch back to the second toplevel window.
  lm_->CycleCurrentToplevelWindow(false);
  EXPECT_EQ(xid2, xconn_->focused_xid());
  EXPECT_EQ(xid2, GetActiveWindowProperty());
  EXPECT_FALSE(wm_->GetWindowOrDie(xid)->IsFocused());
  EXPECT_FALSE(wm_->GetWindowOrDie(transient_xid)->IsFocused());
  EXPECT_TRUE(wm_->GetWindowOrDie(xid2)->IsFocused());

  // Make the transient window non-modal.
  MockXConnection::InitClientMessageEvent(
      &event, transient_xid, wm_->GetXAtom(ATOM_NET_WM_STATE),
      0, wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL), None, None, None);
  wm_->HandleEvent(&event);

  // Now send a _NET_ACTIVE_WINDOW message asking to focus the transient.
  // We should switch back to the first toplevel, and the transient should
  // get the focus.
  MockXConnection::InitClientMessageEvent(
      &event, transient_xid, wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW),
      1, 21321, 0, None, None);
  wm_->HandleEvent(&event);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());
  EXPECT_FALSE(wm_->GetWindowOrDie(xid)->IsFocused());
  EXPECT_TRUE(wm_->GetWindowOrDie(transient_xid)->IsFocused());
  EXPECT_FALSE(wm_->GetWindowOrDie(xid2)->IsFocused());

  // Switch to overview mode.  We should give the focus back to the root
  // window (we don't want the transient to receive keypresses at this
  // point).
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);
  EXPECT_EQ(xconn_->GetRootWindow(), xconn_->focused_xid());
  EXPECT_EQ(None, GetActiveWindowProperty());
  EXPECT_FALSE(wm_->GetWindowOrDie(xid)->IsFocused());
  EXPECT_FALSE(wm_->GetWindowOrDie(transient_xid)->IsFocused());
  EXPECT_FALSE(wm_->GetWindowOrDie(xid2)->IsFocused());
}

TEST_F(LayoutManagerTest, MultipleTransients) {
  // Create a window.
  XWindow owner_xid = CreateSimpleWindow();

  // Send CreateNotify and MapNotify events.
  XEvent event;
  SendInitialEventsForWindow(owner_xid);
  EXPECT_EQ(owner_xid, xconn_->focused_xid());

  // Create a transient window, send CreateNotify and MapNotify events for
  // it, and check that it has the focus.
  XWindow first_transient_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* first_transient_info =
      xconn_->GetWindowInfoOrDie(first_transient_xid);
  first_transient_info->transient_for = owner_xid;
  SendInitialEventsForWindow(first_transient_xid);
  EXPECT_EQ(first_transient_xid, xconn_->focused_xid());

  // The transient window should be stacked on top of its owner (in terms
  // of both its composited and client windows).
  Window* owner_win = wm_->GetWindowOrDie(owner_xid);
  Window* first_transient_win = wm_->GetWindowOrDie(first_transient_xid);
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  EXPECT_LT(stage->GetStackingIndex(first_transient_win->actor()),
            stage->GetStackingIndex(owner_win->actor()));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(first_transient_xid),
            xconn_->stacked_xids().GetIndex(owner_xid));

  // Now create a second transient window, which should get the focus when
  // it's mapped.
  XWindow second_transient_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* second_transient_info =
      xconn_->GetWindowInfoOrDie(second_transient_xid);
  second_transient_info->transient_for = owner_xid;
  SendInitialEventsForWindow(second_transient_xid);
  EXPECT_EQ(second_transient_xid, xconn_->focused_xid());

  // The second transient should be on top of the first, which should be on
  // top of the owner.
  Window* second_transient_win = wm_->GetWindowOrDie(second_transient_xid);
  EXPECT_LT(stage->GetStackingIndex(second_transient_win->actor()),
            stage->GetStackingIndex(first_transient_win->actor()));
  EXPECT_LT(stage->GetStackingIndex(first_transient_win->actor()),
            stage->GetStackingIndex(owner_win->actor()));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(second_transient_xid),
            xconn_->stacked_xids().GetIndex(first_transient_xid));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(first_transient_xid),
            xconn_->stacked_xids().GetIndex(owner_xid));

  // Click on the first transient.  It should get the focused and be moved to
  // the top of the stack.
  xconn_->set_pointer_grab_xid(first_transient_xid);
  MockXConnection::InitButtonPressEvent(&event, *first_transient_info, 0, 0, 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(first_transient_xid, xconn_->focused_xid());
  EXPECT_LT(stage->GetStackingIndex(first_transient_win->actor()),
            stage->GetStackingIndex(second_transient_win->actor()));
  EXPECT_LT(stage->GetStackingIndex(second_transient_win->actor()),
            stage->GetStackingIndex(owner_win->actor()));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(first_transient_xid),
            xconn_->stacked_xids().GetIndex(second_transient_xid));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(second_transient_xid),
            xconn_->stacked_xids().GetIndex(owner_xid));

  // Unmap the first transient.  The second transient should be focused.
  MockXConnection::InitUnmapEvent(&event, first_transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(second_transient_xid, xconn_->focused_xid());
  EXPECT_LT(stage->GetStackingIndex(second_transient_win->actor()),
            stage->GetStackingIndex(owner_win->actor()));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(second_transient_xid),
            xconn_->stacked_xids().GetIndex(owner_xid));

  // After we unmap the second transient, the owner should get the focus.
  MockXConnection::InitUnmapEvent(&event, second_transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(owner_xid, xconn_->focused_xid());
}

TEST_F(LayoutManagerTest, SetWmStateMaximized) {
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);

  vector<int> atoms;
  ASSERT_TRUE(xconn_->GetIntArrayProperty(
                  xid, wm_->GetXAtom(ATOM_NET_WM_STATE), &atoms));
  ASSERT_EQ(2, atoms.size());
  EXPECT_EQ(wm_->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_HORZ), atoms[0]);
  EXPECT_EQ(wm_->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_VERT), atoms[1]);
}

TEST_F(LayoutManagerTest, Resize) {
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  SendInitialEventsForWindow(xid);

  Window* win = wm_->GetWindowOrDie(xid);

  const XWindow root_xid = xconn_->GetRootWindow();
  MockXConnection::WindowInfo* root_info = xconn_->GetWindowInfoOrDie(root_xid);
  EXPECT_EQ(0, lm_->x());
  EXPECT_EQ(0, lm_->y());
  EXPECT_EQ(root_info->width, lm_->width());
  EXPECT_EQ(root_info->height, lm_->height());

  // The client window and its composited counterpart should be resized to
  // take up all the space onscreen.
  EXPECT_EQ(lm_->x(), info->x);
  EXPECT_EQ(lm_->y(), info->y);
  EXPECT_EQ(lm_->width(), info->width);
  EXPECT_EQ(lm_->height(), info->height);
  EXPECT_EQ(lm_->x(), win->composited_x());
  EXPECT_EQ(lm_->y(), win->composited_y());
  EXPECT_DOUBLE_EQ(1.0, win->composited_scale_x());
  EXPECT_DOUBLE_EQ(1.0, win->composited_scale_y());

  // Now resize the screen and check that both the layout manager and
  // client are also resized.
  const int new_width = root_info->width / 2;
  const int new_height = root_info->height / 2;
  xconn_->ResizeWindow(root_xid, new_width, new_height);

  XEvent event;
  MockXConnection::InitConfigureNotifyEvent(&event, *root_info);
  wm_->HandleEvent(&event);

  EXPECT_EQ(new_width, lm_->width());
  EXPECT_EQ(new_height, lm_->height());
  EXPECT_EQ(lm_->width(), info->width);
  EXPECT_EQ(lm_->height(), info->height);
}

// Test that we let clients resize toplevel windows after they've been
// mapped.  This isn't what we actually want to do (why would a client even
// care?  Their window is maximized), but is required to avoid triggering
// issue 449, where Chrome's option window seems to stop redrawing itself
// if it doesn't get the size that it asks for.
TEST_F(LayoutManagerTest, ConfigureToplevel) {
  // Create and map a toplevel window.
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);

  SendInitialEventsForWindow(xid);

  // The window should initially be maximized to fit the area available to
  // the layout manager.
  EXPECT_EQ(lm_->x(), info->x);
  EXPECT_EQ(lm_->y(), info->y);
  EXPECT_EQ(lm_->width(), info->width);
  EXPECT_EQ(lm_->height(), info->height);

  // Now ask for a new position and larger size.
  int new_x = 20;
  int new_y = 40;
  int new_width = lm_->x() + 10;
  int new_height = lm_->y() + 5;
  XEvent event;
  MockXConnection::InitConfigureRequestEvent(
      &event, xid, new_x, new_y, new_width, new_height);
  wm_->HandleEvent(&event);

  // The position change should be ignored, but the window should be
  // resized.
  EXPECT_EQ(lm_->x(), info->x);
  EXPECT_EQ(lm_->y(), info->y);
  EXPECT_EQ(new_width, info->width);
  EXPECT_EQ(new_height, info->height);
}

TEST_F(LayoutManagerTest, OverviewFocus) {
  // Create and map a toplevel window.
  XWindow toplevel_xid = CreateToplevelWindow(2, 0, 0, 0, 640, 480);
  SendInitialEventsForWindow(toplevel_xid);
  MockXConnection::WindowInfo* toplevel_info =
      xconn_->GetWindowInfoOrDie(toplevel_xid);

  // The toplevel window should get the focus, the active window
  // property should be updated, and there shouldn't be a button grab
  // on the window.
  EXPECT_EQ(toplevel_xid, xconn_->focused_xid());
  EXPECT_EQ(toplevel_xid, GetActiveWindowProperty());
  EXPECT_FALSE(toplevel_info->button_is_grabbed(AnyButton));

  // Create an associated snapshot window.
  XWindow xid = CreateSimpleSnapshotWindow(toplevel_xid, 0);
  SendInitialEventsForWindow(xid);

  // The toplevel window should still have the focus, the active
  // window property should be the same, and there still shouldn't be
  // a button grab on the window.
  EXPECT_EQ(toplevel_xid, xconn_->focused_xid());
  EXPECT_EQ(toplevel_xid, GetActiveWindowProperty());
  EXPECT_FALSE(toplevel_info->button_is_grabbed(AnyButton));

  // Now create and map a second snapshot window.
  XWindow xid2 = CreateSimpleSnapshotWindow(toplevel_xid, 1);
  SendInitialEventsForWindow(xid2);
  ChangeTabInfo(toplevel_xid, 2, 1);
  SendWindowTypeEvent(toplevel_xid);

  // The second snapshot window should be current after being created.
  EXPECT_NE(lm_->GetSnapshotWindowByXid(xid), lm_->current_snapshot_);
  EXPECT_EQ(lm_->GetSnapshotWindowByXid(xid2), lm_->current_snapshot_);

  // Now switch to overview mode.  The toplevel window should not have
  // the focus, it should have a button grab, and the active window
  // property should be unset.
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);
  EXPECT_EQ(xconn_->GetRootWindow(), xconn_->focused_xid());
  XEvent event;

  // The second snapshot window should still be current after being
  // created second.
  EXPECT_EQ(lm_->GetSnapshotWindowByXid(xid2), lm_->current_snapshot_);

  // Make sure that unselected snapshots are tilted, and selected ones
  // are not.
  EXPECT_EQ(lm_->current_snapshot_->win()->actor()->GetTilt(), 0.0);
  EXPECT_EQ(
      lm_->GetSnapshotWindowByXid(xid)->win()->actor()->GetTilt(),
      static_cast<double>(LayoutManager::SnapshotWindow::kUnselectedTilt));

  // The second snapshot window should be current.
  EXPECT_EQ(lm_->GetSnapshotWindowByXid(xid2), lm_->current_snapshot_);

  // Click on the first window's input window to make it current.
  XWindow input_xid = lm_->GetInputXidForWindow(*(wm_->GetWindowOrDie(xid)));
  MockXConnection::WindowInfo* input_info =
      xconn_->GetWindowInfoOrDie(input_xid);
  MockXConnection::InitButtonPressEvent(&event, *input_info, 0, 0, 1);
  wm_->HandleEvent(&event);
  MockXConnection::InitButtonReleaseEvent(&event, *input_info, 0, 0, 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(lm_->GetSnapshotWindowByXid(xid), lm_->current_snapshot_);

  // Now click on it again to activate it.  The first window should be
  // focused and set as the active window, and only the second window
  // should still have a button grab.
  MockXConnection::InitButtonPressEvent(&event, *input_info, 0, 0, 1);
  wm_->HandleEvent(&event);
  MockXConnection::InitButtonReleaseEvent(&event, *input_info, 0, 0, 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(lm_->GetToplevelWindowByXid(toplevel_xid), lm_->current_toplevel_);
  EXPECT_EQ(toplevel_xid, xconn_->focused_xid());
  EXPECT_EQ(toplevel_xid, GetActiveWindowProperty());
  EXPECT_FALSE(toplevel_info->button_is_grabbed(AnyButton));
}

TEST_F(LayoutManagerTest, OverviewScrolling) {
  const int window_width = 640;
  const int window_height = 480;

  // Create a background actor.
  Compositor::Actor* background = compositor_->CreateRectangle(
      Compositor::Color(0xff, 0xff, 0xff),
      Compositor::Color(0xff, 0xff, 0xff), 0);
  background->SetSize(window_width, window_height);
  wm_->SetBackgroundActor(background);

  // Create and map a toplevel window.
  XWindow toplevel_xid = CreateToplevelWindow(2, 0, 0, 0,
                                              window_width, window_height);
  SendInitialEventsForWindow(toplevel_xid);

  // Create an associated snapshot window.
  XWindow xid = CreateSnapshotWindow(toplevel_xid, 0, 0, 0,
                                     window_width / 2, window_height / 2);
  SendInitialEventsForWindow(xid);

  // This is the vertical offset to center the background.
  int centering_offset = -(MockXConnection::kDisplayHeight *
                           WindowManager::kBackgroundExpansionFactor -
                           MockXConnection::kDisplayHeight)/2;

  // The background should not be scrolled horizontally yet.
  EXPECT_EQ(0, background->GetX());
  EXPECT_EQ(centering_offset, background->GetY());

  // Now switch to overview mode.
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);

  // Now create and map a second snapshot window.
  XWindow xid2 = CreateSnapshotWindow(toplevel_xid, 1, 0, 0,
                                      window_width / 2, window_height / 2);
  SendInitialEventsForWindow(xid2);
  ChangeTabInfo(toplevel_xid, 2, 1);
  SendWindowTypeEvent(toplevel_xid);

  XEvent event;
  XWindow input_xid = lm_->GetInputXidForWindow(*(wm_->GetWindowOrDie(xid2)));
  MockXConnection::WindowInfo* input_info =
      xconn_->GetWindowInfoOrDie(input_xid);
  MockXConnection::InitButtonPressEvent(&event, *input_info, 0, 0, 1);
  wm_->HandleEvent(&event);
  MockXConnection::InitButtonReleaseEvent(&event, *input_info, 0, 0, 1);
  wm_->HandleEvent(&event);

  EXPECT_EQ(static_cast<int>(MockXConnection::kDisplayWidth *
                             LayoutManager::kOverviewExposedWindowRatio),
            lm_->snapshots_.back()->overview_x());
  EXPECT_EQ(0, lm_->snapshots_.front()->overview_x());
  EXPECT_EQ(static_cast<int>(0.5 + LayoutManager::kOverviewSelectedScale *
                             MockXConnection::kDisplayWidth / 2.f),
            lm_->snapshots_.back()->overview_width());
  EXPECT_EQ(MockXConnection::kDisplayWidth / 2,
            lm_->snapshots_.front()->overview_width());
  EXPECT_EQ(-(lm_->current_snapshot_->overview_x() +
              (lm_->current_snapshot_->overview_width() -
               MockXConnection::kDisplayWidth) / 2),
            lm_->overview_panning_offset_);

  const float kMargin = MockXConnection::kDisplayWidth *
                        LayoutManager::kSideMarginRatio;
  const int overview_width_of_snapshots =
      MockXConnection::kDisplayWidth *
      LayoutManager::kOverviewExposedWindowRatio +
      lm_->snapshots_.back()->overview_tilted_width();
  int min_x = kMargin;
  int max_x = MockXConnection::kDisplayWidth - overview_width_of_snapshots -
              kMargin;
  int background_overage = wm_->background()->GetWidth() - wm_->width();
  float scroll_percent = static_cast<float>(lm_->overview_panning_offset_ -
                                            min_x)/(max_x - min_x);
  scroll_percent = std::max(0.f, scroll_percent);
  scroll_percent = std::min(scroll_percent, 1.f);
  EXPECT_EQ(static_cast<int>(-scroll_percent * background_overage),
            background->GetX());
  EXPECT_EQ(centering_offset, background->GetY());
}

// Test that already-existing windows get stacked correctly.
TEST_F(LayoutManagerTest, InitialWindowStacking) {
  // Reset everything so we can start from scratch.
  wm_.reset(NULL);
  xconn_.reset(new MockXConnection);
  event_loop_.reset(new EventLoop);
  compositor_.reset(new MockCompositor(xconn_.get()));
  lm_ = NULL;

  // Create and map a toplevel window.
  XWindow xid = CreateSimpleWindow();
  xconn_->MapWindow(xid);

  // Now create a new WindowManager object that will see the toplevel
  // window as already existing.
  CreateAndInitNewWm();

  // Get the stacking reference points for toplevel windows and for the
  // layer beneath them.
  XWindow toplevel_stacking_xid = FindWithDefault(
      wm_->stacking_manager()->layer_to_xid_,
      StackingManager::LAYER_TOPLEVEL_WINDOW,
      static_cast<XWindow>(None));
  ASSERT_TRUE(toplevel_stacking_xid != None);
  Compositor::Actor* toplevel_stacking_actor = FindWithDefault(
      wm_->stacking_manager()->layer_to_actor_,
      StackingManager::LAYER_TOPLEVEL_WINDOW,
      shared_ptr<Compositor::Actor>()).get();
  ASSERT_TRUE(toplevel_stacking_actor != None);

  XWindow lower_stacking_xid = FindWithDefault(
      wm_->stacking_manager()->layer_to_xid_,
      static_cast<StackingManager::Layer>(
          StackingManager::LAYER_TOPLEVEL_WINDOW + 1),
      static_cast<XWindow>(None));
  ASSERT_TRUE(lower_stacking_xid != None);
  Compositor::Actor* lower_stacking_actor = FindWithDefault(
      wm_->stacking_manager()->layer_to_actor_,
      static_cast<StackingManager::Layer>(
          StackingManager::LAYER_TOPLEVEL_WINDOW + 1),
      shared_ptr<Compositor::Actor>()).get();
  ASSERT_TRUE(lower_stacking_actor != None);

  // Check that the toplevel window is stacked between the two reference
  // points.
  EXPECT_LT(xconn_->stacked_xids().GetIndex(toplevel_stacking_xid),
            xconn_->stacked_xids().GetIndex(xid));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(xid),
            xconn_->stacked_xids().GetIndex(lower_stacking_xid));

  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  Window* win = wm_->GetWindowOrDie(xid);
  EXPECT_LT(stage->GetStackingIndex(toplevel_stacking_actor),
            stage->GetStackingIndex(win->actor()));
  EXPECT_LT(stage->GetStackingIndex(win->actor()),
            stage->GetStackingIndex(lower_stacking_actor));
}

TEST_F(LayoutManagerTest, StackTransientsAbovePanels) {
  // Create a toplevel window and two transient windows.
  XWindow toplevel_xid = CreateSimpleWindow();
  SendInitialEventsForWindow(toplevel_xid);
  Window* toplevel_win = wm_->GetWindowOrDie(toplevel_xid);

  XWindow first_transient_xid = CreateSimpleWindow();
  xconn_->GetWindowInfoOrDie(first_transient_xid)->transient_for = toplevel_xid;
  SendInitialEventsForWindow(first_transient_xid);
  Window* first_transient_win = wm_->GetWindowOrDie(first_transient_xid);

  XWindow second_transient_xid = CreateSimpleWindow();
  xconn_->GetWindowInfoOrDie(second_transient_xid)->transient_for =
      toplevel_xid;
  SendInitialEventsForWindow(second_transient_xid);
  Window* second_transient_win = wm_->GetWindowOrDie(second_transient_xid);

  // Open a panel.  The transient windows should be stacked above the
  // panel, but the panel should be stacked above the toplevel.
  Panel* panel = CreatePanel(200, 20, 400, true);
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  EXPECT_LT(stage->GetStackingIndex(second_transient_win->actor()),
            stage->GetStackingIndex(first_transient_win->actor()));
  EXPECT_LT(stage->GetStackingIndex(first_transient_win->actor()),
            stage->GetStackingIndex(panel->content_win()->actor()));
  EXPECT_LT(stage->GetStackingIndex(panel->content_win()->actor()),
            stage->GetStackingIndex(toplevel_win->actor()));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(second_transient_xid),
            xconn_->stacked_xids().GetIndex(first_transient_xid));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(first_transient_xid),
            xconn_->stacked_xids().GetIndex(panel->content_xid()));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(panel->content_xid()),
            xconn_->stacked_xids().GetIndex(toplevel_xid));

  // After switching to overview mode, the panel should be above the transients.
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);
  EXPECT_LT(stage->GetStackingIndex(panel->content_win()->actor()),
            stage->GetStackingIndex(second_transient_win->actor()));
  EXPECT_LT(stage->GetStackingIndex(second_transient_win->actor()),
            stage->GetStackingIndex(first_transient_win->actor()));
  EXPECT_LT(stage->GetStackingIndex(first_transient_win->actor()),
            stage->GetStackingIndex(toplevel_win->actor()));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(panel->content_xid()),
            xconn_->stacked_xids().GetIndex(second_transient_xid));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(second_transient_xid),
            xconn_->stacked_xids().GetIndex(first_transient_xid));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(first_transient_xid),
            xconn_->stacked_xids().GetIndex(toplevel_xid));
}

// Test that when a transient window is unmapped, we immediately store its
// owner's XID in the active window property, rather than storing any
// intermediate values like None there.  (Otherwise, we'll see jitter in
// toplevel Chrome windows' active window states.)
TEST_F(LayoutManagerTest, ActiveWindowHintOnTransientUnmap) {
  // Create a toplevel window.
  XWindow toplevel_xid = CreateSimpleWindow();
  SendInitialEventsForWindow(toplevel_xid);
  EXPECT_EQ(toplevel_xid, xconn_->focused_xid());

  // Create a transient window, which should take the focus.
  XWindow transient_xid = CreateSimpleWindow();
  SendInitialEventsForWindow(transient_xid);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());

  // Now register a callback to count how many times the active window
  // property is changed.
  TestCallbackCounter counter;
  xconn_->RegisterPropertyCallback(
      xconn_->GetRootWindow(),
      wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW),
      NewPermanentCallback(&counter, &TestCallbackCounter::Increment));

  // Unmap the transient window and check that the toplevel window is
  // focused.
  XEvent event;
  MockXConnection::InitUnmapEvent(&event, transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(toplevel_xid, xconn_->focused_xid());
  EXPECT_EQ(toplevel_xid, GetActiveWindowProperty());

  // The active window property should've only been updated once.
  EXPECT_EQ(1, counter.num_calls());
}

// Check that we don't dim windows in active mode, to guard against a
// regression of http://crosbug.com/2278.
TEST_F(LayoutManagerTest, NoDimmingInActiveMode) {
  // Create two toplevel windows.
  const XWindow xid1 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid1);
  EXPECT_EQ(xid1, xconn_->focused_xid());

  const XWindow xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid2);
  EXPECT_EQ(xid2, xconn_->focused_xid());

  // Switch to overview mode and then back to active mode.
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);
  lm_->SetMode(LayoutManager::MODE_ACTIVE);

  // Check that the second window is focused and not dimmed.
  EXPECT_EQ(xid2, xconn_->focused_xid());
  MockCompositor::Actor* actor2 =
      dynamic_cast<MockCompositor::Actor*>(wm_->GetWindow(xid2)->actor());
  CHECK(actor2);
  EXPECT_FALSE(actor2->is_dimmed());

  // Now switch back to the first window (which was dimmed when we displayed
  // it in overview mode) and check that it's not dimmed in active mode.
  lm_->CycleCurrentToplevelWindow(true);
  EXPECT_EQ(xid1, xconn_->focused_xid());
  MockCompositor::Actor* actor1 =
      dynamic_cast<MockCompositor::Actor*>(wm_->GetWindow(xid1)->actor());
  CHECK(actor1);
  EXPECT_FALSE(actor1->is_dimmed());
}

// Check that we ignore _NET_ACTIVE_WINDOW messages asking us to focus the
// current window (as it should already have the focus), to guard against a
// regression of http://crosbug.com/2992.
TEST_F(LayoutManagerTest, AvoidMovingCurrentWindow) {
  // Create a window and check that it gets focused.
  const XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);
  EXPECT_EQ(xid, xconn_->focused_xid());

  MockCompositor::Actor* actor =
      dynamic_cast<MockCompositor::Actor*>(wm_->GetWindow(xid)->actor());
  int initial_num_moves = actor->num_moves();

  // Now send a _NET_ACTIVE_WINDOW message asking the window manager to
  // focus the window (even though it's already current).
  XEvent net_active_win_event;
  MockXConnection::InitClientMessageEvent(
      &net_active_win_event,
      xid,   // window to focus
      wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW),
      1,     // source indication: client app
      CurrentTime,
      xid,   // currently-active window
      None,
      None);
  wm_->HandleEvent(&net_active_win_event);

  // Check that we didn't animate the actor's position.
  EXPECT_EQ(initial_num_moves, actor->num_moves());

  // Switch to overview mode.
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);
  EXPECT_EQ(xconn_->GetRootWindow(), xconn_->focused_xid());

  // Send the window manager the _NET_ACTIVE_WINDOW message again and check
  // that it switches back to active mode.
  wm_->HandleEvent(&net_active_win_event);
  EXPECT_EQ(LayoutManager::MODE_ACTIVE, lm_->mode());
  EXPECT_EQ(xid, xconn_->focused_xid());
}

// Test that LayoutManager resizes non-Chrome and toplevel Chrome windows
// to fill the screen as soon as it gets MapRequest events about them.
TEST_F(LayoutManagerTest, ResizeWindowsBeforeMapping) {
  // Create a small non-Chrome window and check that it gets resized to the
  // layout manager's dimensions on MapRequest.
  const XWindow nonchrome_xid = CreateBasicWindow(0, 0, 50, 40);
  MockXConnection::WindowInfo* nonchrome_info =
      xconn_->GetWindowInfoOrDie(nonchrome_xid);
  XEvent event;
  MockXConnection::InitCreateWindowEvent(&event, *nonchrome_info);
  wm_->HandleEvent(&event);
  MockXConnection::InitMapRequestEvent(&event, *nonchrome_info);
  wm_->HandleEvent(&event);
  EXPECT_EQ(lm_->width(), nonchrome_info->width);
  EXPECT_EQ(lm_->height(), nonchrome_info->height);

  // We should do the same thing with toplevel Chrome windows.
  const XWindow toplevel_xid = CreateToplevelWindow(1, 0, 0, 0, 50, 40);
  MockXConnection::WindowInfo* toplevel_info =
      xconn_->GetWindowInfoOrDie(toplevel_xid);
  MockXConnection::InitCreateWindowEvent(&event, *toplevel_info);
  wm_->HandleEvent(&event);
  MockXConnection::InitMapRequestEvent(&event, *toplevel_info);
  wm_->HandleEvent(&event);
  EXPECT_EQ(lm_->width(), toplevel_info->width);
  EXPECT_EQ(lm_->height(), toplevel_info->height);

  // Snapshot windows should retain their original dimensions.
  const int orig_width = 50, orig_height = 40;
  const XWindow snapshot_xid =
      CreateSnapshotWindow(toplevel_xid, 0, 0, 0, orig_width, orig_height);
  MockXConnection::WindowInfo* snapshot_info =
      xconn_->GetWindowInfoOrDie(snapshot_xid);
  MockXConnection::InitCreateWindowEvent(&event, *snapshot_info);
  wm_->HandleEvent(&event);
  MockXConnection::InitMapRequestEvent(&event, *snapshot_info);
  wm_->HandleEvent(&event);
  EXPECT_EQ(orig_width, snapshot_info->width);
  EXPECT_EQ(orig_height, snapshot_info->height);

  // Transient windows should, too.
  const XWindow transient_xid =
      CreateBasicWindow(0, 0, orig_width, orig_height);
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = toplevel_xid;
  MockXConnection::InitCreateWindowEvent(&event, *transient_info);
  wm_->HandleEvent(&event);
  MockXConnection::InitMapRequestEvent(&event, *transient_info);
  wm_->HandleEvent(&event);
  EXPECT_EQ(orig_width, transient_info->width);
  EXPECT_EQ(orig_height, transient_info->height);
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
