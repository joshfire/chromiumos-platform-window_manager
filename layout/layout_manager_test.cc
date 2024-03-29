// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <tr1/memory>
#include <vector>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/layout/layout_manager.h"
#include "window_manager/layout/snapshot_window.h"
#include "window_manager/layout/toplevel_window.h"
#include "window_manager/panels/panel.h"
#include "window_manager/shadow.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"
#include "window_manager/x11/mock_x_connection.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

DECLARE_bool(enable_overview_mode);  // from layout_manager.cc
DECLARE_string(background_image);    // from layout_manager.cc

using std::string;
using std::vector;
using std::tr1::shared_ptr;
using window_manager::util::FindWithDefault;

namespace window_manager {

class LayoutManagerTest : public BasicWindowManagerTest {
 protected:
  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();
    lm_ = wm_->layout_manager_.get();
  }

  // Read a WM_IPC_MESSAGE_CHROME_NOTIFY_TAB_SELECT message sent to a
  // window and return the index from it.  If the message isn't the only
  // client message stored in the WindowInfo struct, or it's of a different
  // type, -1 is returned.  The client message is deleted from the
  // WindowInfo.
  int ConsumeTabSelectMessage(XWindow xid) {
    MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
    if (info->client_messages.size() != static_cast<size_t>(1)) {
      LOG(ERROR) << "WindowInfo for window " << xid << " has "
                 << info->client_messages.size() << " client messages; "
                 << "expected just 1";
      return -1;
    }

    WmIpc::Message msg;
    if (!DecodeWmIpcMessage(info->client_messages[0], &msg))
      return -1;
    if (msg.type() != chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_TAB_SELECT)
      return -1;
    info->client_messages.clear();
    return msg.param(0);
  }

  LayoutManager* lm_;  // points to wm_'s copy
};

TEST_F(LayoutManagerTest, Basic) {
  XWindow xid1 = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      Rect(100, 100, 640, 480),
      false,     // override redirect
      false,     // input only
      0, 0);     // event mask, visual
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid1, &geometry));
  wm_->TrackWindow(xid1, false, geometry);  // override_redirect=false

  Window* win1 = wm_->GetWindowOrDie(xid1);
  win1->MapClient();
  win1->HandleMapNotify();

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
      Rect(100, 100, 640, 480),
      false,     // override redirect
      false,     // input only
      0, 0);     // event mask, visual
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid2, &geometry));
  wm_->TrackWindow(xid2, false, geometry);  // override_redirect=false
  Window* win2 = wm_->GetWindowOrDie(xid2);
  win2->MapClient();
  win2->HandleMapNotify();
  lm_->HandleWindowMap(win2);

  XWindow xid3 = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      Rect(100, 100, 640, 480),
      false,     // override redirect
      false,     // input only
      0, 0);     // event mask, visual
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid3, &geometry));
  wm_->TrackWindow(xid3, false, geometry);  // override_redirect=false
  Window* win3 = wm_->GetWindowOrDie(xid3);
  win3->MapClient();
  win3->HandleMapNotify();
  lm_->HandleWindowMap(win3);

  // The third window should be onscreen now, and the first and second
  // windows should be offscreen.
  EXPECT_TRUE(WindowIsOffscreen(xid1));
  EXPECT_TRUE(WindowIsOffscreen(xid2));
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
  EXPECT_TRUE(WindowIsOffscreen(xid2));
  EXPECT_TRUE(WindowIsOffscreen(xid3));

  // After cycling the windows again, the first and third windows
  // should be offscreen and the second window should be onscreen.
  // Cycle the windows with a chrome message to test message handling.
  WmIpc::Message message_forward(chromeos::WM_IPC_MESSAGE_WM_CYCLE_WINDOWS);
  message_forward.set_param(0, true);
  lm_->HandleChromeMessage(message_forward);
  EXPECT_TRUE(WindowIsOffscreen(xid1));
  EXPECT_FALSE(WindowIsOffscreen(xid2));
  EXPECT_TRUE(WindowIsOffscreen(xid3));

  // After cycling the windows back, the second and third windows
  // should be offscreen and the first window should be onscreen.
  // Cycle the windows with a chrome message to test message handling.
  WmIpc::Message message_back(chromeos::WM_IPC_MESSAGE_WM_CYCLE_WINDOWS);
  message_back.set_param(0, false);
  lm_->HandleChromeMessage(message_back);
  EXPECT_FALSE(WindowIsOffscreen(xid1));
  EXPECT_TRUE(WindowIsOffscreen(xid2));
  EXPECT_TRUE(WindowIsOffscreen(xid3));

  // After cycling the windows back again, the first and second
  // windows should be offscreen and the third window should be
  // onscreen.  Cycle the windows with a chrome message to test
  // message handling.
  lm_->HandleChromeMessage(message_back);
  EXPECT_TRUE(WindowIsOffscreen(xid1));
  EXPECT_TRUE(WindowIsOffscreen(xid2));
  EXPECT_FALSE(WindowIsOffscreen(xid3));
}

TEST_F(LayoutManagerTest, Focus) {
  // Create a window.
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  EXPECT_EQ(None, xconn_->focused_xid());

  // Send a CreateNotify event to the window manager.
  XEvent event;
  xconn_->InitCreateWindowEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(None, xconn_->focused_xid());
  EXPECT_TRUE(lm_->current_toplevel_ == NULL);

  // The layout manager should activate and focus the window when it gets
  // mapped.  Because the window is focused, it shouldn't have a button
  // grab installed.
  xconn_->InitMapEvent(&event, xid);
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
  xconn_->InitCreateWindowEvent(&event, xid2);
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid, xconn_->focused_xid());
  ASSERT_TRUE(lm_->current_toplevel_ != NULL);
  EXPECT_EQ(xid, lm_->current_toplevel_->win()->xid());

  // When the second window is mapped, it should become the active window.
  xconn_->InitMapEvent(&event, xid2);
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid2, xconn_->focused_xid());
  EXPECT_EQ(xid2, GetActiveWindowProperty());
  ASSERT_TRUE(lm_->current_toplevel_ != NULL);
  EXPECT_EQ(xid2, lm_->current_toplevel_->win()->xid());
  EXPECT_TRUE(info->button_is_grabbed(AnyButton));
  EXPECT_FALSE(info2->button_is_grabbed(AnyButton));

  // Now send a _NET_ACTIVE_WINDOW message asking the window manager to
  // focus the first window.
  xconn_->InitClientMessageEvent(
      &event,
      xid,   // window to focus
      xconn_->GetAtomOrDie("_NET_ACTIVE_WINDOW"),
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
  xconn_->InitUnmapEvent(&event, xid);
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

  EXPECT_EQ(0, owner_info->bounds.x);
  EXPECT_EQ(0, owner_info->bounds.y);
  EXPECT_EQ(lm_->width(), owner_info->bounds.width);
  EXPECT_EQ(lm_->height(), owner_info->bounds.height);

  // Now create and map a transient window.
  XWindow transient_xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      Rect(60, 70, 320, 240),
      false,     // override redirect
      false,     // input only
      0, 0);     // event mask, visual
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = owner_xid;
  SendInitialEventsForWindow(transient_xid);

  // The transient window should initially be centered over its owner.
  EXPECT_EQ(owner_info->bounds.x +
              0.5 * (owner_info->bounds.width - transient_info->bounds.width),
            transient_info->bounds.x);
  EXPECT_EQ(owner_info->bounds.y +
              0.5 * (owner_info->bounds.height - transient_info->bounds.height),
            transient_info->bounds.y);

  // Now try to move and resize the transient window.  The move request
  // should be ignored, but the window should be resized and re-centered.
  xconn_->InitConfigureRequestEvent(
      &event,
      transient_xid,
      Rect(owner_info->bounds.x + 20, owner_info->bounds.y + 10, 400, 300));
  event.xconfigurerequest.value_mask = CWWidth | CWHeight;
  wm_->HandleEvent(&event);
  EXPECT_EQ(400, transient_info->bounds.width);
  EXPECT_EQ(300, transient_info->bounds.height);
  EXPECT_EQ(owner_info->bounds.x +
              0.5 * (owner_info->bounds.width - transient_info->bounds.width),
            transient_info->bounds.x);
  EXPECT_EQ(owner_info->bounds.y +
              0.5 * (owner_info->bounds.height - transient_info->bounds.height),
            transient_info->bounds.y);
  xconn_->InitConfigureNotifyEvent(&event, owner_xid);
  wm_->HandleEvent(&event);

  // The transient window's actor should be hidden after the window is
  // unmapped.
  xconn_->InitUnmapEvent(&event, transient_xid);
  wm_->HandleEvent(&event);
  MockCompositor::TexturePixmapActor* transient_actor =
      GetMockActorForWindow(wm_->GetWindowOrDie(transient_xid));
  EXPECT_FALSE(transient_actor->is_shown());
  xconn_->InitDestroyWindowEvent(&event, transient_xid);
  wm_->HandleEvent(&event);

  // Create and map an info bubble window.
  int bubble_x = owner_info->bounds.x + 40;
  int bubble_y = owner_info->bounds.y + 30;
  XWindow bubble_xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      Rect(bubble_x, bubble_y, 320, 240),
      false,     // override redirect
      false,     // input only
      0, 0);     // event mask, visual
  ASSERT_TRUE(wm_->wm_ipc()->SetWindowType(
      bubble_xid,
      chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE,
      NULL));
  MockXConnection::WindowInfo* bubble_info =
      xconn_->GetWindowInfoOrDie(bubble_xid);
  bubble_info->transient_for = owner_xid;
  SendInitialEventsForWindow(bubble_xid);

  // The bubble's initial position should be preserved.
  EXPECT_EQ(bubble_x, bubble_info->bounds.x);
  EXPECT_EQ(bubble_y, bubble_info->bounds.y);

  // Now switch to overview mode and check that the bubble's client window
  // is moved offscreen and its compositing actor is hidden.
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);
  EXPECT_TRUE(WindowIsOffscreen(bubble_xid));
  MockCompositor::TexturePixmapActor* bubble_actor =
      GetMockActorForWindow(wm_->GetWindowOrDie(bubble_xid));
  EXPECT_FALSE(bubble_actor->is_shown());

  // We shouldn't move the client window in response to configure requests
  // while the transient is hidden, but we should save the offset.
  const Point kBubbleOffset(20, 30);
  xconn_->InitConfigureRequestEvent(
      &event,
      bubble_xid,
      Rect(Point(owner_info->bounds.x + kBubbleOffset.x,
                 owner_info->bounds.y + kBubbleOffset.y),
           bubble_info->bounds.size()));
  event.xconfigurerequest.value_mask = CWX | CWY | CWWidth | CWHeight;
  wm_->HandleEvent(&event);
  EXPECT_TRUE(WindowIsOffscreen(bubble_xid));

  // After switching back to active mode, the transient window should be at the
  // expected offset from its owner (which will be at (0, 0)).
  lm_->SetMode(LayoutManager::MODE_ACTIVE);
  EXPECT_EQ(kBubbleOffset, bubble_info->bounds.position());
  EXPECT_EQ(kBubbleOffset, bubble_actor->GetBounds().position());
  EXPECT_TRUE(bubble_actor->is_shown());
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
  xconn_->InitButtonPressEvent(&event, xid, Point(0, 0), 1);  // x, y, button
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
  xconn_->InitButtonPressEvent(&event, transient_xid, Point(0, 0), 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());
  EXPECT_FALSE(wm_->GetWindowOrDie(xid)->IsFocused());
  EXPECT_TRUE(wm_->GetWindowOrDie(transient_xid)->IsFocused());

  // Set the transient window as modal.
  xconn_->InitClientMessageEvent(
      &event, transient_xid, xconn_->GetAtomOrDie("_NET_WM_STATE"),
      1, xconn_->GetAtomOrDie("_NET_WM_STATE_MODAL"), None, None, None);
  wm_->HandleEvent(&event);

  // Since it's modal, the transient window should still keep the focus
  // after a button press in the owner window.
  xconn_->set_pointer_grab_xid(xid);
  xconn_->InitButtonPressEvent(&event, xid, Point(0, 0), 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());
  EXPECT_FALSE(wm_->GetWindowOrDie(xid)->IsFocused());
  EXPECT_TRUE(wm_->GetWindowOrDie(transient_xid)->IsFocused());

  // Now create another toplevel window.  We shouldn't switch to it since
  // there's a modal dialog open.
  XWindow xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid2);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());
  EXPECT_FALSE(WindowIsOffscreen(xid));
  EXPECT_TRUE(WindowIsOffscreen(xid2));

  // Make the transient window non-modal.
  xconn_->InitClientMessageEvent(
      &event, transient_xid, xconn_->GetAtomOrDie("_NET_WM_STATE"),
      0, xconn_->GetAtomOrDie("_NET_WM_STATE_MODAL"), None, None, None);
  wm_->HandleEvent(&event);

  // Send a _NET_ACTIVE_WINDOW message asking to focus the second window.
  // We should switch to it.
  xconn_->InitClientMessageEvent(
      &event, xid2, xconn_->GetAtomOrDie("_NET_ACTIVE_WINDOW"),
      1, 21320, 0, None, None);  // arbitrary timestamp
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid2, xconn_->focused_xid());
  EXPECT_EQ(xid2, GetActiveWindowProperty());
  EXPECT_TRUE(WindowIsOffscreen(xid));
  EXPECT_TRUE(WindowIsOffscreen(transient_xid));
  EXPECT_FALSE(WindowIsOffscreen(xid2));

  // Now send a _NET_ACTIVE_WINDOW message asking to focus the transient.
  // We should switch back to the first toplevel and the transient should
  // get the focus.
  xconn_->InitClientMessageEvent(
      &event, transient_xid, xconn_->GetAtomOrDie("_NET_ACTIVE_WINDOW"),
      1, 21321, 0, None, None);  // arbitrary timestamp
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
  xconn_->InitButtonPressEvent(&event, first_transient_xid, Point(0, 0), 1);
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
  xconn_->InitUnmapEvent(&event, first_transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(second_transient_xid, xconn_->focused_xid());
  EXPECT_LT(stage->GetStackingIndex(second_transient_win->actor()),
            stage->GetStackingIndex(owner_win->actor()));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(second_transient_xid),
            xconn_->stacked_xids().GetIndex(owner_xid));

  // After we unmap the second transient, the owner should get the focus.
  xconn_->InitUnmapEvent(&event, second_transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(owner_xid, xconn_->focused_xid());
}

TEST_F(LayoutManagerTest, SetWmStateMaximized) {
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);

  vector<int> atoms;
  ASSERT_TRUE(xconn_->GetIntArrayProperty(
      xid, xconn_->GetAtomOrDie("_NET_WM_STATE"), &atoms));
  ASSERT_EQ(2, atoms.size());
  EXPECT_EQ(xconn_->GetAtomOrDie("_NET_WM_STATE_MAXIMIZED_HORZ"), atoms[0]);
  EXPECT_EQ(xconn_->GetAtomOrDie("_NET_WM_STATE_MAXIMIZED_VERT"), atoms[1]);
}

TEST_F(LayoutManagerTest, Resize) {
  const XWindow root_xid = xconn_->GetRootWindow();
  MockXConnection::WindowInfo* root_info = xconn_->GetWindowInfoOrDie(root_xid);

  // Set up a background Actor.
  Compositor::ColoredBoxActor* background = compositor_->CreateColoredBox(
      root_info->bounds.width, root_info->bounds.height, Compositor::Color());
  lm_->SetBackground(background);
  ASSERT_EQ(root_info->bounds.width, background->GetWidth());
  ASSERT_EQ(root_info->bounds.height, background->GetHeight());

  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  SendInitialEventsForWindow(xid);

  Window* win = wm_->GetWindowOrDie(xid);

  EXPECT_EQ(0, lm_->x());
  EXPECT_EQ(0, lm_->y());
  EXPECT_EQ(root_info->bounds.width, lm_->width());
  EXPECT_EQ(root_info->bounds.height, lm_->height());

  // The client window and its composited counterpart should be resized to
  // take up all the space onscreen.
  EXPECT_EQ(lm_->x(), info->bounds.x);
  EXPECT_EQ(lm_->y(), info->bounds.y);
  EXPECT_EQ(lm_->width(), info->bounds.width);
  EXPECT_EQ(lm_->height(), info->bounds.height);
  EXPECT_EQ(lm_->x(), win->composited_x());
  EXPECT_EQ(lm_->y(), win->composited_y());
  EXPECT_DOUBLE_EQ(1.0, win->composited_scale_x());
  EXPECT_DOUBLE_EQ(1.0, win->composited_scale_y());

  // Now resize the screen and check that both the layout manager and
  // client are also resized.
  const int new_width = root_info->bounds.width / 2;
  const int new_height = root_info->bounds.height / 2;
  xconn_->ResizeWindow(root_xid, Size(new_width, new_height));

  XEvent event;
  xconn_->InitConfigureNotifyEvent(&event, root_xid);
  wm_->HandleEvent(&event);

  EXPECT_EQ(new_width, lm_->width());
  EXPECT_EQ(new_height, lm_->height());
  EXPECT_EQ(lm_->width(), info->bounds.width);
  EXPECT_EQ(lm_->height(), info->bounds.height);

  // The background window should be resized too.
  MockXConnection::WindowInfo* background_info =
      xconn_->GetWindowInfoOrDie(lm_->background_xid_);
  EXPECT_EQ(0, background_info->bounds.x);
  EXPECT_EQ(0, background_info->bounds.y);
  EXPECT_EQ(new_width, background_info->bounds.width);
  EXPECT_EQ(new_height, background_info->bounds.height);
  EXPECT_EQ(
      static_cast<int>(
          new_width * LayoutManager::kBackgroundExpansionFactor + 0.5f),
      static_cast<int>(
          background->GetWidth() * background->GetXScale() + 0.5f));
  EXPECT_EQ(
      static_cast<int>(
          new_height * LayoutManager::kBackgroundExpansionFactor + 0.5f),
      static_cast<int>(
          background->GetHeight() * background->GetYScale() + 0.5f));

  // Now check that background config works with different aspects.
  background->SetSize(root_info->bounds.width * 2, root_info->bounds.height);
  lm_->ConfigureBackground(new_width, new_height);
  EXPECT_EQ(new_width * 2, background->GetWidth());
  EXPECT_EQ(new_height, background->GetHeight());

  background->SetSize(root_info->bounds.width, root_info->bounds.height * 2);
  lm_->ConfigureBackground(new_width, new_height);
  EXPECT_EQ(
      static_cast<int>(
          new_width * LayoutManager::kBackgroundExpansionFactor + 0.5f),
      static_cast<int>(
          background->GetWidth() * background->GetXScale() + 0.5f));
  EXPECT_EQ(
      static_cast<int>(
          new_height * LayoutManager::kBackgroundExpansionFactor * 2 + 0.5f),
      static_cast<int>(
          background->GetHeight() * background->GetYScale() + 0.5f));
}

// Test that we don't let clients resize toplevel windows after they've been
// mapped.
TEST_F(LayoutManagerTest, ConfigureToplevel) {
  // Create and map a toplevel window.
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);

  SendInitialEventsForWindow(xid);

  // The window should initially be maximized to fit the area available to
  // the layout manager.
  EXPECT_EQ(lm_->x(), info->bounds.x);
  EXPECT_EQ(lm_->y(), info->bounds.y);
  EXPECT_EQ(lm_->width(), info->bounds.width);
  EXPECT_EQ(lm_->height(), info->bounds.height);

  // Now ask for a new position and larger size.
  int new_x = 20;
  int new_y = 40;
  int new_width = lm_->x() + 10;
  int new_height = lm_->y() + 5;
  XEvent event;
  xconn_->InitConfigureRequestEvent(
      &event, xid, Rect(new_x, new_y, new_width, new_height));
  info->configure_notify_events.clear();
  wm_->HandleEvent(&event);

  // The window should have the same position and size as before.
  EXPECT_EQ(lm_->x(), info->bounds.x);
  EXPECT_EQ(lm_->y(), info->bounds.y);
  EXPECT_EQ(lm_->width(), info->bounds.width);
  EXPECT_EQ(lm_->height(), info->bounds.height);

  // We should've sent it a synthetic ConfigureNotify event containing its
  // current position and size.
  ASSERT_EQ(static_cast<size_t>(1), info->configure_notify_events.size());
  EXPECT_EQ(lm_->x(), info->configure_notify_events[0].x);
  EXPECT_EQ(lm_->y(), info->configure_notify_events[0].y);
  EXPECT_EQ(lm_->width(), info->configure_notify_events[0].width);
  EXPECT_EQ(lm_->height(), info->configure_notify_events[0].height);
}

TEST_F(LayoutManagerTest, ChangeCurrentSnapshot) {
  XWindow toplevel1_xid = CreateToplevelWindow(2, 0, Rect(0, 0, 640, 480));
  SendInitialEventsForWindow(toplevel1_xid);
  MockXConnection::WindowInfo* info1 =
      xconn_->GetWindowInfoOrDie(toplevel1_xid);
  XWindow toplevel2_xid = CreateToplevelWindow(2, 0, Rect(0, 0, 640, 480));
  SendInitialEventsForWindow(toplevel2_xid);
  MockXConnection::WindowInfo* info2 =
      xconn_->GetWindowInfoOrDie(toplevel2_xid);

  // Create some snapshot windows for the first toplevel.
  XWindow xid11 = CreateSimpleSnapshotWindow(toplevel1_xid, 0);
  SendInitialEventsForWindow(xid11);
  XWindow xid12 = CreateSimpleSnapshotWindow(toplevel1_xid, 1);
  SendInitialEventsForWindow(xid12);
  ChangeTabInfo(toplevel1_xid, 2, 1, wm_->GetCurrentTimeFromServer());
  SendWindowTypeEvent(toplevel1_xid);
  XWindow xid13 = CreateSimpleSnapshotWindow(toplevel1_xid, 2);
  SendInitialEventsForWindow(xid13);
  ChangeTabInfo(toplevel1_xid, 3, 2, wm_->GetCurrentTimeFromServer());
  SendWindowTypeEvent(toplevel1_xid);

  // Create some snapshot windows for the second toplevel.
  XWindow xid21 = CreateSimpleSnapshotWindow(toplevel2_xid, 0);
  SendInitialEventsForWindow(xid21);
  XWindow xid22 = CreateSimpleSnapshotWindow(toplevel2_xid, 1);
  SendInitialEventsForWindow(xid22);
  ChangeTabInfo(toplevel2_xid, 2, 1, wm_->GetCurrentTimeFromServer());
  SendWindowTypeEvent(toplevel2_xid);

  // OK, now we make sure we have two toplevels, the first one has
  // three snapshots, and the second has two.
  EXPECT_EQ(2, lm_->toplevels_.size());
  EXPECT_EQ(5, lm_->snapshots_.size());
  EXPECT_EQ(lm_->toplevels_[0].get(), lm_->snapshots_[0]->toplevel());
  EXPECT_EQ(lm_->toplevels_[0].get(), lm_->snapshots_[1]->toplevel());
  EXPECT_EQ(lm_->toplevels_[0].get(), lm_->snapshots_[2]->toplevel());
  EXPECT_EQ(lm_->toplevels_[1].get(), lm_->snapshots_[3]->toplevel());
  EXPECT_EQ(lm_->toplevels_[1].get(), lm_->snapshots_[4]->toplevel());

  // Now let's go into overview mode.
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);

  // The second toplevel window should be current.
  EXPECT_EQ(lm_->GetToplevelWindowByXid(toplevel2_xid), lm_->current_toplevel_);

  // The fifth (second one on second toplevel) snapshot window should
  // be current.
  EXPECT_EQ(lm_->GetSnapshotWindowByXid(xid22), lm_->current_snapshot_);

  // Now change snapshots by moving "back" one using the left arrow key.
  long event_time = wm_->GetCurrentTimeFromServer();
  KeyBindings::KeyCombo left_key(XK_Left, 0);
  SendKey(xconn_->GetRootWindow(), left_key, event_time - 1, event_time);

  EXPECT_EQ(xconn_->GetAtomOrDie("_CHROME_WM_MESSAGE"),
            info2->client_messages.back().message_type);
  EXPECT_EQ(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_TAB_SELECT,
            info2->client_messages.back().data.l[0]);
  EXPECT_EQ(0, info2->client_messages.back().data.l[1]);

  // Normally this would now be sent by Chrome, so we simulate it.
  ChangeTabInfo(toplevel2_xid, 2, 0, event_time);
  SendWindowTypeEvent(toplevel2_xid);

  // The second toplevel window should be current.
  EXPECT_EQ(lm_->GetToplevelWindowByXid(toplevel2_xid), lm_->current_toplevel_);
  EXPECT_EQ(lm_->toplevels_[1].get(), lm_->current_toplevel_);

  // The fourth snapshot (first one on second toplevel) should now be current.
  EXPECT_EQ(lm_->GetSnapshotWindowByXid(xid21), lm_->current_snapshot_);
  EXPECT_EQ(lm_->snapshots_[3].get(), lm_->current_snapshot_);

  // Now change snapshots by moving "back" again using the left arrow key.
  event_time = wm_->GetCurrentTimeFromServer();
  SendKey(xconn_->GetRootWindow(), left_key, event_time - 1, event_time);

  // Now we do NOT expect to see a tab select message sent to the
  // first toplevel, since during the creation process, the third
  // snapshot should already by selected in that toplevel, so there's
  // no need to send one.
  EXPECT_EQ(xconn_->GetAtomOrDie("_CHROME_WM_MESSAGE"),
            info1->client_messages.back().message_type);
  EXPECT_EQ(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_LAYOUT_MODE,
            info1->client_messages.back().data.l[0]);

  // The first toplevel window should now be current.
  EXPECT_EQ(lm_->GetToplevelWindowByXid(toplevel1_xid), lm_->current_toplevel_);
  EXPECT_EQ(lm_->toplevels_[0].get(), lm_->current_toplevel_);

  // The third snapshot (third one on first toplevel) should now be current.
  EXPECT_EQ(lm_->GetSnapshotWindowByXid(xid13), lm_->current_snapshot_);
  EXPECT_EQ(lm_->snapshots_[2].get(), lm_->current_snapshot_);

  // Now go "back" again using the left arrow key, but this time
  // inject some changes with earlier timestamps (ostensibly generated
  // from Chrome instead of the WM), that should be ignored.
  event_time = wm_->GetCurrentTimeFromServer();
  SendKey(xconn_->GetRootWindow(), left_key, event_time - 1, event_time);

  EXPECT_EQ(xconn_->GetAtomOrDie("_CHROME_WM_MESSAGE"),
            info1->client_messages.back().message_type);
  EXPECT_EQ(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_TAB_SELECT,
            info1->client_messages.back().data.l[0]);
  EXPECT_EQ(1, info1->client_messages.back().data.l[1]);

  // This is a simulated change by Chrome with an earlier event time.
  ChangeTabInfo(toplevel1_xid, 3, 2, event_time - 1);
  SendWindowTypeEvent(toplevel1_xid);

  // Normally this would now be sent by Chrome in response to our
  // message, so we simulate it.
  ChangeTabInfo(toplevel1_xid, 3, 1, event_time);
  SendWindowTypeEvent(toplevel1_xid);

  // The first toplevel window should now be current.
  EXPECT_EQ(lm_->GetToplevelWindowByXid(toplevel1_xid), lm_->current_toplevel_);
  EXPECT_EQ(lm_->toplevels_[0].get(), lm_->current_toplevel_);

  // The second snapshot (second one on first toplevel) should now be current.
  EXPECT_EQ(lm_->GetSnapshotWindowByXid(xid12), lm_->current_snapshot_);
  EXPECT_EQ(lm_->snapshots_[1].get(), lm_->current_snapshot_);

  // Now go "back" again using the left arrow key, but this time
  // inject some changes with later timestamps (ostensibly generated
  // from Chrome instead of the WM), that should override ours.
  event_time = wm_->GetCurrentTimeFromServer();
  SendKey(xconn_->GetRootWindow(), left_key, event_time - 1, event_time);

  EXPECT_EQ(xconn_->GetAtomOrDie("_CHROME_WM_MESSAGE"),
            info1->client_messages.back().message_type);
  EXPECT_EQ(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_TAB_SELECT,
            info1->client_messages.back().data.l[0]);
  EXPECT_EQ(0, info1->client_messages.back().data.l[1]);

  // This is a simulated change by Chrome with an later event time.
  ChangeTabInfo(toplevel1_xid, 3, 2, event_time + 1);
  SendWindowTypeEvent(toplevel1_xid);

  // Normally this would now be sent by Chrome in response to our
  // message, so we simulate it.  It should be ignored.
  ChangeTabInfo(toplevel1_xid, 3, 0, event_time);
  SendWindowTypeEvent(toplevel1_xid);

  // The first toplevel window should now be current.
  EXPECT_EQ(lm_->GetToplevelWindowByXid(toplevel1_xid), lm_->current_toplevel_);
  EXPECT_EQ(lm_->toplevels_[0].get(), lm_->current_toplevel_);

  // The first snapshot (first one on first toplevel) should NOT be current.
  EXPECT_NE(lm_->GetSnapshotWindowByXid(xid11), lm_->current_snapshot_);
  EXPECT_NE(lm_->snapshots_[0].get(), lm_->current_snapshot_);

  // The third snapshot (third one on first toplevel) should now be current.
  EXPECT_EQ(lm_->GetSnapshotWindowByXid(xid13), lm_->current_snapshot_);
  EXPECT_EQ(lm_->snapshots_[2].get(), lm_->current_snapshot_);
}

TEST_F(LayoutManagerTest, OverviewFocus) {
  // Create and map a toplevel window.
  XWindow toplevel_xid = CreateToplevelWindow(2, 0, Rect(0, 0, 640, 480));
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
  ChangeTabInfo(toplevel_xid, 2, 1, wm_->GetCurrentTimeFromServer());
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
  xconn_->InitButtonPressEvent(&event, input_xid, Point(0, 0), 1);
  wm_->HandleEvent(&event);
  xconn_->InitButtonReleaseEvent(&event, input_xid, Point(0, 0), 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(lm_->GetSnapshotWindowByXid(xid), lm_->current_snapshot_);

  // Now click on it again to activate it.  The first window should be
  // focused and set as the active window, and only the second window
  // should still have a button grab.
  xconn_->InitButtonPressEvent(&event, input_xid, Point(0, 0), 1);
  wm_->HandleEvent(&event);
  xconn_->InitButtonReleaseEvent(&event, input_xid, Point(0, 0), 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(lm_->GetToplevelWindowByXid(toplevel_xid), lm_->current_toplevel_);
  EXPECT_EQ(toplevel_xid, xconn_->focused_xid());
  EXPECT_EQ(toplevel_xid, GetActiveWindowProperty());
  EXPECT_FALSE(toplevel_info->button_is_grabbed(AnyButton));
}

TEST_F(LayoutManagerTest, OverviewSpacing) {
  const int window_width = 640;
  const int window_height = 480;

  // Create a background actor.
  Compositor::ColoredBoxActor* background = compositor_->CreateColoredBox(
      window_width, window_height, Compositor::Color());
  lm_->SetBackground(background);

  // Create and map a toplevel window.
  XWindow toplevel_xid =
      CreateToplevelWindow(2, 0, Rect(0, 0, window_width, window_height));
  SendInitialEventsForWindow(toplevel_xid);

  // Create and map a second toplevel window.
  XWindow toplevel_xid2 =
      CreateToplevelWindow(1, 0, Rect(0, 0, window_width, window_height));
  SendInitialEventsForWindow(toplevel_xid2);

  // Create an associated snapshot window with some "realistic"
  // values.  (The numbers here don't represent the values that Chrome
  // is using to make the snapshots, they're just reasonable values.)
  const int snapshot_height = MockXConnection::kDisplayHeight / 2;
  const int snapshot_width = snapshot_height * 1024 / 1280;
  XWindow snapshot =
      CreateSnapshotWindow(
          toplevel_xid, 0, Rect(0, 0, snapshot_width, snapshot_height));
  SendInitialEventsForWindow(snapshot);
  XWindow snapshot_title =
      CreateTitleWindow(snapshot, Size(snapshot_width, 16));
  SendInitialEventsForWindow(snapshot_title);
  XWindow snapshot_fav_icon = CreateFavIconWindow(snapshot, Size(16, 16));
  SendInitialEventsForWindow(snapshot_fav_icon);

  // This is the vertical offset to center the background.
  int centering_offset = -(MockXConnection::kDisplayHeight *
                           LayoutManager::kBackgroundExpansionFactor -
                           MockXConnection::kDisplayHeight) / 2;

  // The background should not be scrolled horizontally yet.
  EXPECT_EQ(0, background->GetX());
  EXPECT_EQ(centering_offset, background->GetY());

  // Now switch to overview mode.
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);

  // Now create and map a second snapshot window.
  XWindow snapshot2 =
      CreateSnapshotWindow(
          toplevel_xid, 1, Rect(0, 0, snapshot_width, snapshot_height));
  SendInitialEventsForWindow(snapshot2);
  XWindow snapshot2_title =
      CreateTitleWindow(snapshot2, Size(snapshot_width, 16));
  SendInitialEventsForWindow(snapshot2_title);
  XWindow snapshot2_fav_icon = CreateFavIconWindow(snapshot2, Size(16, 16));
  SendInitialEventsForWindow(snapshot2_fav_icon);
  ChangeTabInfo(toplevel_xid, 2, 1, wm_->GetCurrentTimeFromServer());
  SendWindowTypeEvent(toplevel_xid);

  // Now create and map a third snapshot window, with the second
  // toplevel as its parent.
  XWindow snapshot3 =
      CreateSnapshotWindow(
          toplevel_xid2, 0, Rect(0, 0, snapshot_width, snapshot_height));
  SendInitialEventsForWindow(snapshot3);
  XWindow snapshot3_title =
      CreateTitleWindow(snapshot3, Size(snapshot_width, 16));
  SendInitialEventsForWindow(snapshot3_title);
  XWindow snapshot3_fav_icon = CreateFavIconWindow(snapshot3, Size(16, 16));
  SendInitialEventsForWindow(snapshot3_fav_icon);
  ChangeTabInfo(toplevel_xid2, 1, 0, wm_->GetCurrentTimeFromServer());
  SendWindowTypeEvent(toplevel_xid2);

  EXPECT_EQ(-(lm_->current_snapshot_->overview_x() +
              (lm_->current_snapshot_->overview_width() -
               lm_->width_) / 2),
            lm_->overview_panning_offset_);

  // Make sure the fav icon and title got hooked up correctly.
  EXPECT_EQ(lm_->current_snapshot_->fav_icon(),
            wm_->GetWindow(snapshot3_fav_icon));
  EXPECT_EQ(lm_->current_snapshot_->title(),
            wm_->GetWindow(snapshot3_title));

  // Make sure the title and fav icon ended up in the right place.
  EXPECT_EQ(lm_->current_snapshot_->win()->composited_x(),
            lm_->current_snapshot_->fav_icon()->composited_x());
  EXPECT_EQ(lm_->current_snapshot_->win()->composited_y() +
            lm_->current_snapshot_->win()->composited_height() +
            LayoutManager::SnapshotWindow::kTitlePadding,
            lm_->current_snapshot_->fav_icon()->composited_y());
  EXPECT_EQ(lm_->current_snapshot_->fav_icon()->composited_x() +
            lm_->current_snapshot_->fav_icon()->composited_width() +
            LayoutManager::SnapshotWindow::kFavIconPadding,
            lm_->current_snapshot_->title()->composited_x());
  EXPECT_EQ(lm_->current_snapshot_->overview_y() +
            lm_->current_snapshot_->win()->composited_height() +
            LayoutManager::SnapshotWindow::kTitlePadding,
            lm_->current_snapshot_->title()->composited_y());

  // Make sure the input window region includes the snapshot window, title,
  // and fav icon regions.
  XWindow input_xid = lm_->GetInputXidForWindow(*lm_->current_snapshot_->win());

  MockXConnection::WindowInfo* win_info = xconn_->GetWindowInfo(input_xid);
  EXPECT_TRUE(win_info != NULL);
  EXPECT_EQ(win_info->bounds.height,
            lm_->current_snapshot_->win()->composited_height() +
            lm_->current_snapshot_->title()->composited_height() +
            LayoutManager::SnapshotWindow::kTitlePadding);

  // Now click on the second window and make sure things move appropriately.
  XEvent event;
  input_xid = lm_->GetInputXidForWindow(*wm_->GetWindowOrDie(snapshot2));
  xconn_->InitButtonPressEvent(&event, input_xid, Point(0, 0), 1);
  wm_->HandleEvent(&event);
  xconn_->InitButtonReleaseEvent(&event, input_xid, Point(0, 0), 1);
  wm_->HandleEvent(&event);

  int second_snapshot_x = snapshot_width *
                          LayoutManager::kOverviewExposedWindowRatio /
                          LayoutManager::kOverviewWindowMaxSizeRatio;

  int third_snapshot_x =
      second_snapshot_x + snapshot_width +
      LayoutManager::kOverviewSelectedPadding +
      lm_->width_ * LayoutManager::kOverviewGroupSpacing + 0.5f;

  EXPECT_EQ(0, lm_->snapshots_.front()->overview_x());
  EXPECT_EQ(second_snapshot_x, lm_->snapshots_[1]->overview_x());
  EXPECT_EQ(third_snapshot_x, lm_->snapshots_[2]->overview_x());
  EXPECT_EQ(snapshot_width, lm_->snapshots_[1]->overview_width());
  EXPECT_EQ(static_cast<int>(snapshot_width *
                             LayoutManager::kOverviewNotSelectedScale),
            lm_->snapshots_.front()->overview_width());

  // Now make sure the background moved appropriately.
  const int overview_width_of_snapshots =
      third_snapshot_x +
      lm_->snapshots_.back()->overview_tilted_width();
  EXPECT_EQ(overview_width_of_snapshots, lm_->overview_width_of_snapshots_);
  int min_x = -overview_width_of_snapshots;
  int max_x = MockXConnection::kDisplayWidth;
  int background_overage = background->GetWidth() - wm_->width();
  float scroll_percent = 1.0f -
                         static_cast<float>(lm_->overview_panning_offset_ -
                                            min_x)/(max_x - min_x);
  scroll_percent = std::max(0.f, scroll_percent);
  scroll_percent = std::min(scroll_percent, 1.f);
  EXPECT_EQ(static_cast<int>(-background_overage * scroll_percent),
            background->GetX());
  EXPECT_EQ(centering_offset, background->GetY());
}

// Test that already-existing windows get stacked correctly.
TEST_F(LayoutManagerTest, InitialWindowStacking) {
  // Reset everything so we can start from scratch.
  wm_.reset(NULL);
  xconn_.reset(new MockXConnection);
  RegisterCommonKeySyms();
  event_loop_.reset(new EventLoop);
  compositor_.reset(new MockCompositor(xconn_.get()));
  lm_ = NULL;

  // Create and map a toplevel window.
  XWindow xid = CreateSimpleWindow();
  xconn_->MapWindow(xid);

  // Now create a new WindowManager object that will see the toplevel
  // window as already existing.
  SetLoggedInState(true);  // MockXConnection was reset
  CreateAndInitNewWm();
  lm_ = wm_->layout_manager_.get();

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
  Panel* panel = CreatePanel(200, 20, 400);
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
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = toplevel_xid;
  SendInitialEventsForWindow(transient_xid);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());

  // Now register a callback to count how many times the active window
  // property is changed.
  TestCallbackCounter counter;
  xconn_->RegisterPropertyCallback(
      xconn_->GetRootWindow(),
      xconn_->GetAtomOrDie("_NET_ACTIVE_WINDOW"),
      NewPermanentCallback(&counter, &TestCallbackCounter::Increment));

  // Unmap the transient window and check that the toplevel window is
  // focused.
  XEvent event;
  xconn_->InitUnmapEvent(&event, transient_xid);
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
      GetMockActorForWindow(wm_->GetWindowOrDie(xid2));
  EXPECT_FALSE(actor2->is_dimmed());

  // Now switch back to the first window (which was dimmed when we displayed
  // it in overview mode) and check that it's not dimmed in active mode.
  lm_->CycleCurrentToplevelWindow(true);
  EXPECT_EQ(xid1, xconn_->focused_xid());
  MockCompositor::Actor* actor1 =
      GetMockActorForWindow(wm_->GetWindowOrDie(xid1));
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
      GetMockActorForWindow(wm_->GetWindowOrDie(xid));
  int initial_num_moves = actor->num_moves();

  // Now send a _NET_ACTIVE_WINDOW message asking the window manager to
  // focus the window (even though it's already current).
  XEvent net_active_win_event;
  xconn_->InitClientMessageEvent(
      &net_active_win_event,
      xid,   // window to focus
      xconn_->GetAtomOrDie("_NET_ACTIVE_WINDOW"),
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
  const XWindow nonchrome_xid = CreateBasicWindow(Rect(0, 0, 50, 40));
  MockXConnection::WindowInfo* nonchrome_info =
      xconn_->GetWindowInfoOrDie(nonchrome_xid);
  XEvent event;
  xconn_->InitCreateWindowEvent(&event, nonchrome_xid);
  wm_->HandleEvent(&event);
  xconn_->InitMapRequestEvent(&event, nonchrome_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(lm_->width(), nonchrome_info->bounds.width);
  EXPECT_EQ(lm_->height(), nonchrome_info->bounds.height);

  // We should do the same thing with toplevel Chrome windows.
  const XWindow toplevel_xid =
      CreateToplevelWindow(1, 0, Rect(0, 0, 50, 40));
  MockXConnection::WindowInfo* toplevel_info =
      xconn_->GetWindowInfoOrDie(toplevel_xid);
  xconn_->InitCreateWindowEvent(&event, toplevel_xid);
  wm_->HandleEvent(&event);
  xconn_->InitMapRequestEvent(&event, toplevel_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(lm_->width(), toplevel_info->bounds.width);
  EXPECT_EQ(lm_->height(), toplevel_info->bounds.height);

  // Snapshot windows should retain their original dimensions.
  const int orig_width = 50, orig_height = 40;
  const XWindow snapshot_xid =
      CreateSnapshotWindow(
          toplevel_xid, 0, Rect(0, 0, orig_width, orig_height));
  MockXConnection::WindowInfo* snapshot_info =
      xconn_->GetWindowInfoOrDie(snapshot_xid);
  xconn_->InitCreateWindowEvent(&event, snapshot_xid);
  wm_->HandleEvent(&event);
  xconn_->InitMapRequestEvent(&event, snapshot_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(orig_width, snapshot_info->bounds.width);
  EXPECT_EQ(orig_height, snapshot_info->bounds.height);

  // Transient windows should, too.
  const XWindow transient_xid =
      CreateBasicWindow(Rect(0, 0, orig_width, orig_height));
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = toplevel_xid;
  xconn_->InitCreateWindowEvent(&event, transient_xid);
  wm_->HandleEvent(&event);
  xconn_->InitMapRequestEvent(&event, transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(orig_width, transient_info->bounds.width);
  EXPECT_EQ(orig_height, transient_info->bounds.height);
}

// Test that the layout manager handles windows that claim to be transient
// for already-transient windows reasonably -- see http://crosbug.com/3316.
TEST_F(LayoutManagerTest, NestedTransients) {
  // Create a toplevel window.
  XWindow toplevel_xid = CreateSimpleWindow();
  SendInitialEventsForWindow(toplevel_xid);
  LayoutManager::ToplevelWindow* toplevel =
      lm_->GetToplevelWindowByWindow(*(wm_->GetWindowOrDie(toplevel_xid)));
  ASSERT_TRUE(toplevel != NULL);

  // Create a transient window.
  const int initial_width = 300, initial_height = 200;
  XWindow transient_xid =
      CreateBasicWindow(Rect(0, 0, initial_width, initial_height));
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = toplevel_xid;
  SendInitialEventsForWindow(transient_xid);

  // Check that its initial size is preserved.
  EXPECT_EQ(initial_width, transient_info->bounds.width);
  EXPECT_EQ(initial_height, transient_info->bounds.height);
  EXPECT_TRUE(lm_->GetToplevelWindowOwningTransientWindow(
      *(wm_->GetWindowOrDie(transient_xid))) == toplevel);;

  // Now create a second transient window that says it's transient for the
  // first transient window.
  XWindow nested_transient_xid =
      CreateBasicWindow(Rect(0, 0, initial_width, initial_height));
  MockXConnection::WindowInfo* nested_transient_info =
      xconn_->GetWindowInfoOrDie(nested_transient_xid);
  nested_transient_info->transient_for = transient_xid;
  SendInitialEventsForWindow(nested_transient_xid);

  // The second transient window should be treated as a transient of the
  // toplevel instead.  We check that it keeps its initial size rather than
  // being maximized.
  EXPECT_EQ(initial_width, nested_transient_info->bounds.width);
  EXPECT_EQ(initial_height, nested_transient_info->bounds.height);
  EXPECT_TRUE(lm_->GetToplevelWindowOwningTransientWindow(
      *(wm_->GetWindowOrDie(nested_transient_xid))) == toplevel);;

  // For good measure, do it all again with another transient window nested
  // one level deeper.
  XWindow another_transient_xid =
      CreateBasicWindow(Rect(0, 0, initial_width, initial_height));
  MockXConnection::WindowInfo* another_transient_info =
      xconn_->GetWindowInfoOrDie(another_transient_xid);
  another_transient_info->transient_for = nested_transient_xid;
  SendInitialEventsForWindow(another_transient_xid);
  EXPECT_EQ(initial_width, another_transient_info->bounds.width);
  EXPECT_EQ(initial_height, another_transient_info->bounds.height);
  EXPECT_TRUE(lm_->GetToplevelWindowOwningTransientWindow(
      *(wm_->GetWindowOrDie(another_transient_xid))) == toplevel);;
}

// Check that the initial Chrome window appears onscreen immediately
// instead of sliding in from the side.
TEST_F(LayoutManagerTest, NoSlideForInitialWindow) {
  // Create a window and check that it's in the expected location.
  XWindow xid =
      CreateToplevelWindow(0, 0, Rect(0, 0, 640, 480));
  SendInitialEventsForWindow(xid);
  Window* win = wm_->GetWindowOrDie(xid);
  EXPECT_EQ(0, win->client_x());
  EXPECT_EQ(0, win->client_y());
  EXPECT_EQ(0, win->composited_x());
  EXPECT_EQ(0, win->composited_y());

  // The actor should've been moved immediately to its current location
  // instead of getting animated.
  MockCompositor::Actor* actor = GetMockActorForWindow(win);
  EXPECT_FALSE(actor->position_was_animated());

  // Now create a second window and check that it *does* get animated.
  XWindow xid2 =
      CreateToplevelWindow(0, 0, Rect(0, 0, 640, 480));
  SendInitialEventsForWindow(xid2);
  Window* win2 = wm_->GetWindowOrDie(xid2);
  EXPECT_EQ(0, win2->client_x());
  EXPECT_EQ(0, win2->client_y());
  EXPECT_EQ(0, win2->composited_x());
  EXPECT_EQ(0, win2->composited_y());
  MockCompositor::Actor* actor2 = GetMockActorForWindow(win2);
  EXPECT_TRUE(actor2->position_was_animated());
}

// Check that key bindings get enabled and disabled appropriately.
TEST_F(LayoutManagerTest, KeyBindings) {
  // We should start out in active mode.
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);
  EXPECT_TRUE(lm_->active_mode_key_bindings_group_->enabled());
  EXPECT_FALSE(lm_->overview_mode_key_bindings_group_->enabled());

  // After switching to overview mode, we should switch key binding groups.
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);
  EXPECT_FALSE(lm_->active_mode_key_bindings_group_->enabled());
  EXPECT_TRUE(lm_->overview_mode_key_bindings_group_->enabled());

  // The layout manager just shouldn't be created when we're not logged in.
  SetLoggedInState(false);
  CreateAndInitNewWm();
  EXPECT_TRUE(wm_->layout_manager_.get() == NULL);
}

// Test our handling of requests to toggle the fullscreen state on toplevel
// windows.
TEST_F(LayoutManagerTest, Fullscreen) {
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);
  Window* win = wm_->GetWindowOrDie(xid);
  EXPECT_FALSE(win->wm_state_fullscreen());
  EXPECT_TRUE(WindowIsInLayer(win, StackingManager::LAYER_TOPLEVEL_WINDOW));

  // When a window asks to be fullscreened, its fullscreen property should
  // be set and it should be moved to the fullscreen stacking layer.
  XEvent fullscreen_event;
  xconn_->InitClientMessageEvent(
      &fullscreen_event, xid, xconn_->GetAtomOrDie("_NET_WM_STATE"),
      1, xconn_->GetAtomOrDie("_NET_WM_STATE_FULLSCREEN"), None, None, None);
  wm_->HandleEvent(&fullscreen_event);
  EXPECT_TRUE(win->wm_state_fullscreen());
  EXPECT_TRUE(WindowIsInLayer(win, StackingManager::LAYER_FULLSCREEN_WINDOW));

  // When we map a second toplevel window, it should get the focus and the
  // first window should be automatically unfullscreened.
  XWindow xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid2);
  Window* win2 = wm_->GetWindowOrDie(xid2);
  ASSERT_EQ(xid2, xconn_->focused_xid());
  EXPECT_FALSE(win->wm_state_fullscreen());
  EXPECT_FALSE(win2->wm_state_fullscreen());
  EXPECT_TRUE(WindowIsInLayer(win, StackingManager::LAYER_TOPLEVEL_WINDOW));
  EXPECT_TRUE(WindowIsInLayer(win2, StackingManager::LAYER_TOPLEVEL_WINDOW));

  // Check that the first window is automatically focused if it requests to
  // be fullscreened again.
  wm_->HandleEvent(&fullscreen_event);
  EXPECT_EQ(xid, xconn_->focused_xid());
  EXPECT_TRUE(win->wm_state_fullscreen());
  EXPECT_TRUE(WindowIsInLayer(win, StackingManager::LAYER_FULLSCREEN_WINDOW));

  // Now open a panel that'll take the focus and check that the toplevel
  // window is again unfullscreened.
  Panel* panel = CreatePanel(200, 20, 400);
  EXPECT_FALSE(win->wm_state_fullscreen());
  EXPECT_TRUE(WindowIsInLayer(win, StackingManager::LAYER_TOPLEVEL_WINDOW));

  // Make the window fullscreen again and check that it stays that way if a
  // transient window is opened for it.
  wm_->HandleEvent(&fullscreen_event);
  EXPECT_EQ(xid, xconn_->focused_xid());
  EXPECT_TRUE(win->wm_state_fullscreen());
  EXPECT_TRUE(WindowIsInLayer(win, StackingManager::LAYER_FULLSCREEN_WINDOW));

  XWindow transient_xid = CreateBasicWindow(Rect(0, 0, 300, 300));
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = xid;
  SendInitialEventsForWindow(transient_xid);
  Window* transient_win = wm_->GetWindowOrDie(transient_xid);
  EXPECT_TRUE(win->wm_state_fullscreen());
  EXPECT_TRUE(WindowIsInLayer(win, StackingManager::LAYER_FULLSCREEN_WINDOW));
  EXPECT_TRUE(WindowIsInLayer(transient_win,
                              StackingManager::LAYER_FULLSCREEN_WINDOW));

  // Now ask to make the toplevel non-fullscreen.
  XEvent unfullscreen_event;
  xconn_->InitClientMessageEvent(
      &unfullscreen_event, xid, xconn_->GetAtomOrDie("_NET_WM_STATE"),
      0, xconn_->GetAtomOrDie("_NET_WM_STATE_FULLSCREEN"), None, None, None);
  wm_->HandleEvent(&unfullscreen_event);
  EXPECT_FALSE(win->wm_state_fullscreen());
  EXPECT_TRUE(WindowIsInLayer(win, StackingManager::LAYER_TOPLEVEL_WINDOW));
  EXPECT_TRUE(WindowIsInLayer(transient_win,
                              StackingManager::LAYER_ACTIVE_TRANSIENT_WINDOW));

  // Dock the panel on the left side of the screen and check that the
  // window gets resized and shifted to the right.
  SendPanelDraggedMessage(panel, 0, 0);
  SendPanelDragCompleteMessage(panel);
  EXPECT_EQ(PanelManager::kPanelDockWidth, win->client_x());
  EXPECT_EQ(wm_->width() - PanelManager::kPanelDockWidth, win->client_width());

  // When we make the window fullscreen, it should be resized and moved to
  // cover the whole screen.
  wm_->HandleEvent(&fullscreen_event);
  EXPECT_EQ(0, win->client_x());
  EXPECT_EQ(0, win->composited_x());
  EXPECT_EQ(wm_->width(), win->client_width());
  EXPECT_EQ(wm_->width(), win->composited_width());

  // Now resize the screen and check that the window is resized to cover it.
  const XWindow root_xid = xconn_->GetRootWindow();
  MockXConnection::WindowInfo* root_info = xconn_->GetWindowInfoOrDie(root_xid);
  const int new_width = root_info->bounds.width + 20;
  const int new_height = root_info->bounds.height + 20;
  xconn_->ResizeWindow(root_xid, Size(new_width, new_height));
  XEvent resize_event;
  xconn_->InitConfigureNotifyEvent(&resize_event, root_xid);
  wm_->HandleEvent(&resize_event);
  EXPECT_EQ(0, win->client_x());
  EXPECT_EQ(0, win->composited_x());
  EXPECT_EQ(new_width, win->client_width());
  EXPECT_EQ(new_width, win->composited_width());
  EXPECT_EQ(new_height, win->client_height());
  EXPECT_EQ(new_height, win->composited_height());

  // The smaller size should be restored when it's made non-fullscreen.
  wm_->HandleEvent(&unfullscreen_event);
  EXPECT_EQ(PanelManager::kPanelDockWidth, win->client_x());
  EXPECT_EQ(PanelManager::kPanelDockWidth, win->composited_x());
  EXPECT_EQ(new_width - PanelManager::kPanelDockWidth, win->client_width());
  EXPECT_EQ(new_width - PanelManager::kPanelDockWidth, win->composited_width());

  // If the fullscreen hint is already set on a window when it's mapped
  // (Flash does this), we should honor it.
  XWindow initially_fullscreen_xid = CreateSimpleWindow();
  xconn_->SetIntProperty(
      initially_fullscreen_xid,
      xconn_->GetAtomOrDie("_NET_WM_STATE"),
      xconn_->GetAtomOrDie("ATOM"),
      xconn_->GetAtomOrDie("_NET_WM_STATE_FULLSCREEN"));
  SendInitialEventsForWindow(initially_fullscreen_xid);
  Window* initially_fullscreen_win =
      wm_->GetWindowOrDie(initially_fullscreen_xid);
  EXPECT_TRUE(initially_fullscreen_win->wm_state_fullscreen());
  EXPECT_TRUE(WindowIsInLayer(initially_fullscreen_win,
                              StackingManager::LAYER_FULLSCREEN_WINDOW));
  // Also check that the window is visible -- we forgo initializing the
  // layout of fullscreen windows, so it's easy to miss doing this.
  EXPECT_TRUE(initially_fullscreen_win->composited_shown());
  EXPECT_DOUBLE_EQ(1.0, initially_fullscreen_win->composited_opacity());
}

// This just checks that we don't crash when changing modes while there
// aren't any toplevel windows (http://crosbug.com/4167).
TEST_F(LayoutManagerTest, ChangeModeWithNoWindows) {
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);
  EXPECT_EQ(LayoutManager::MODE_OVERVIEW, lm_->mode());
  lm_->SetMode(LayoutManager::MODE_ACTIVE);
  EXPECT_EQ(LayoutManager::MODE_ACTIVE, lm_->mode());
}

// Check that we switch backgrounds after the initial Chrome window gets
// mapped.
TEST_F(LayoutManagerTest, ChangeBackgroundsAfterInitialWindow) {
  SetLoggedInState(false);
  // The mock compositor doesn't actually load images.
  AutoReset<string> background_image_flag_resetter(
      &FLAGS_background_image, "bogus_bg.png");
  // We avoid loading the background if overview mode is disabled.
  AutoReset<bool> enable_overview_mode_flag_resetter(
      &FLAGS_enable_overview_mode, true);
  CreateAndInitNewWm();
  lm_ = NULL;

  // We should start out showing just the startup background.
  ASSERT_TRUE(wm_->startup_background_.get() != NULL);
  MockCompositor::Actor* cast_startup_background =
      dynamic_cast<MockCompositor::Actor*>(wm_->startup_background_.get());
  CHECK(cast_startup_background);
  EXPECT_TRUE(cast_startup_background->is_shown());
  EXPECT_TRUE(wm_->layout_manager_.get() == NULL);

  // After the user logs in, the window manager should've dropped the
  // startup background, and the layout manager should've also loaded the
  // logged-in background.
  SetLoggedInState(true);
  EXPECT_TRUE(wm_->startup_background_.get() == NULL);
  lm_ = wm_->layout_manager_.get();
  ASSERT_TRUE(lm_ != NULL);
  ASSERT_TRUE(lm_->background_.get() != NULL);
  MockCompositor::Actor* cast_lm_background =
      dynamic_cast<MockCompositor::Actor*>(lm_->background_.get());
  CHECK(cast_lm_background);
  EXPECT_FALSE(cast_lm_background->is_shown());

  // After the first Chrome window gets mapped, we should show the layout
  // manager background.
  XWindow toplevel_xid =
      CreateToplevelWindow(2, 0, Rect(0, 0, 640, 480));
  SendInitialEventsForWindow(toplevel_xid);
  ASSERT_TRUE(lm_->background_.get() != NULL);
  EXPECT_TRUE(cast_lm_background->is_shown());

  // And after the window gets closed, we should hide the layout manager
  // background.
  XEvent event;
  xconn_->InitUnmapEvent(&event, toplevel_xid);
  wm_->HandleEvent(&event);
  ASSERT_TRUE(lm_->background_.get() != NULL);
  EXPECT_FALSE(cast_lm_background->is_shown());
}

// Test that we grab the back and forward keys in overview mode, but not in
// active mode (Chrome needs to receive events when they're pressed).  This
// guards against a regression of the problem described in comment #13 in
// http://crosbug.com/101.
TEST_F(LayoutManagerTest, DontGrabBackAndForwardKeysInActiveMode) {
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);
  EXPECT_TRUE(xconn_->KeyIsGrabbed(xconn_->GetKeyCodeFromKeySym(XK_F1), 0));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(xconn_->GetKeyCodeFromKeySym(XK_F2), 0));

  lm_->SetMode(LayoutManager::MODE_ACTIVE);
  EXPECT_FALSE(xconn_->KeyIsGrabbed(xconn_->GetKeyCodeFromKeySym(XK_F1), 0));
  EXPECT_FALSE(xconn_->KeyIsGrabbed(xconn_->GetKeyCodeFromKeySym(XK_F2), 0));
}

// Check that shadows only get displayed for transient windows.
TEST_F(LayoutManagerTest, Shadows) {
  // Chrome toplevel windows shouldn't have shadows.
  XWindow toplevel_xid =
      CreateToplevelWindow(2, 0, Rect(0, 0, 200, 200));
  SendInitialEventsForWindow(toplevel_xid);
  EXPECT_TRUE(wm_->GetWindowOrDie(toplevel_xid)->shadow() == NULL);

  // Neither should non-Chrome toplevel windows.
  XWindow other_xid = CreateSimpleWindow();
  SendInitialEventsForWindow(other_xid);
  EXPECT_TRUE(wm_->GetWindowOrDie(other_xid)->shadow() == NULL);

  // Or snapshot windows, or their title and fav icons windows.
  XWindow snapshot_xid = CreateSimpleSnapshotWindow(toplevel_xid, 0);
  SendInitialEventsForWindow(snapshot_xid);
  EXPECT_TRUE(wm_->GetWindowOrDie(snapshot_xid)->shadow() == NULL);

  XWindow title_xid = CreateTitleWindow(snapshot_xid, Size(200, 16));
  SendInitialEventsForWindow(title_xid);
  EXPECT_TRUE(wm_->GetWindowOrDie(title_xid)->shadow() == NULL);

  XWindow fav_icon_xid = CreateFavIconWindow(snapshot_xid, Size(16, 16));
  SendInitialEventsForWindow(fav_icon_xid);
  EXPECT_TRUE(wm_->GetWindowOrDie(fav_icon_xid)->shadow() == NULL);

  // Transient windows should get shadows, though...
  XWindow transient_xid = CreateSimpleWindow();
  xconn_->GetWindowInfoOrDie(transient_xid)->transient_for = toplevel_xid;
  SendInitialEventsForWindow(transient_xid);
  ASSERT_TRUE(wm_->GetWindowOrDie(transient_xid)->shadow() != NULL);
  EXPECT_TRUE(wm_->GetWindowOrDie(transient_xid)->shadow()->is_shown());

  // ...unless they're info bubbles...
  XWindow info_bubble_xid = CreateSimpleWindow();
  xconn_->GetWindowInfoOrDie(info_bubble_xid)->transient_for = toplevel_xid;
  ASSERT_TRUE(wm_->wm_ipc()->SetWindowType(
      info_bubble_xid,
      chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE,
      NULL));
  SendInitialEventsForWindow(info_bubble_xid);
  EXPECT_TRUE(wm_->GetWindowOrDie(info_bubble_xid)->shadow() == NULL);

  // ...or RGBA.
  XWindow rgba_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* rgba_info = xconn_->GetWindowInfoOrDie(rgba_xid);
  rgba_info->transient_for = toplevel_xid;
  rgba_info->depth = 32;
  SendInitialEventsForWindow(rgba_xid);
  EXPECT_TRUE(wm_->GetWindowOrDie(rgba_xid)->shadow() == NULL);
}

// Check that we defer animating new windows onscreen until the client says
// that they've been painted.
TEST_F(LayoutManagerTest, DeferAnimationsUntilPainted) {
  // Create and map two windows.  Make the second one say that it supports
  // the _NET_WM_SYNC_REQUEST protocol.
  XWindow xid1 =
      CreateToplevelWindow(2, 0, Rect(0, 0, 200, 200));
  SendInitialEventsForWindow(xid1);
  XWindow xid2 =
      CreateToplevelWindow(2, 0, Rect(0, 0, 200, 200));
  ConfigureWindowForSyncRequestProtocol(xid2);
  SendInitialEventsForWindow(xid2);

  // Check that the second window got the focus, but that the first still
  // has its client window onscreen (since the second window hasn't said
  // that it's been painted yet).
  EXPECT_FALSE(WindowIsOffscreen(xid1));
  EXPECT_TRUE(WindowIsOffscreen(xid2));
  EXPECT_EQ(xid2, xconn_->focused_xid());

  // Tell the window manager that the second window has been painted and
  // check that it moves it onscreen.
  SendSyncRequestProtocolAlarm(xid2);
  EXPECT_TRUE(WindowIsOffscreen(xid1));
  EXPECT_FALSE(WindowIsOffscreen(xid2));
  EXPECT_EQ(xid2, xconn_->focused_xid());
}

// Check that we switch toplevel windows as needed when a modal transient
// window gets mapped, or when the modal hint is set on an existing
// transient window.
TEST_F(LayoutManagerTest, SwitchToToplevelWithModalTransient) {
  // Create two toplevel windows.
  XWindow xid1 =
      CreateToplevelWindow(2, 0, Rect(0, 0, 200, 200));
  SendInitialEventsForWindow(xid1);
  XWindow xid2 =
      CreateToplevelWindow(2, 0, Rect(0, 0, 200, 200));
  SendInitialEventsForWindow(xid2);

  // The second toplevel should be focused initially.
  EXPECT_TRUE(WindowIsOffscreen(xid1));
  EXPECT_EQ(xid2, xconn_->focused_xid());
  EXPECT_EQ(xid2, GetActiveWindowProperty());
  EXPECT_FALSE(WindowIsOffscreen(xid2));

  // At first, the key bindings should be enabled.
  EXPECT_TRUE(lm_->active_mode_key_bindings_group_->enabled());

  // Create an already-modal transient window for the first toplevel.  We should
  // switch to the first toplevel and focus its transient, along with disabling
  // key bindings.
  XWindow transient_xid1 = CreateSimpleWindow();
  xconn_->GetWindowInfoOrDie(transient_xid1)->transient_for = xid1;
  AppendAtomToProperty(transient_xid1,
                       xconn_->GetAtomOrDie("_NET_WM_STATE"),
                       xconn_->GetAtomOrDie("_NET_WM_STATE_MODAL"));
  SendInitialEventsForWindow(transient_xid1);
  EXPECT_FALSE(WindowIsOffscreen(xid1));
  EXPECT_EQ(transient_xid1, xconn_->focused_xid());
  EXPECT_EQ(transient_xid1, GetActiveWindowProperty());
  EXPECT_TRUE(WindowIsOffscreen(xid2));
  EXPECT_FALSE(lm_->active_mode_key_bindings_group_->enabled());

  // Create a non-modal transient for the second toplevel.  We should still
  // be showing the first toplevel.
  XWindow transient_xid2 = CreateSimpleWindow();
  xconn_->GetWindowInfoOrDie(transient_xid2)->transient_for = xid2;
  SendInitialEventsForWindow(transient_xid2);
  EXPECT_FALSE(WindowIsOffscreen(xid1));
  EXPECT_TRUE(WindowIsOffscreen(xid2));

  // Send a message making the second toplevel's transient modal.  We
  // should switch to the second toplevel and focus its transient.
  XEvent event;
  xconn_->InitClientMessageEvent(
      &event, transient_xid2, xconn_->GetAtomOrDie("_NET_WM_STATE"),
      1, xconn_->GetAtomOrDie("_NET_WM_STATE_MODAL"), None, None, None);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(WindowIsOffscreen(xid1));
  EXPECT_EQ(transient_xid2, xconn_->focused_xid());
  EXPECT_EQ(transient_xid2, GetActiveWindowProperty());
  EXPECT_FALSE(WindowIsOffscreen(xid2));
  EXPECT_FALSE(lm_->active_mode_key_bindings_group_->enabled());

  // Destroy the second transient window and check that we switch back to
  // the first one, since it's still modal.
  SendUnmapAndDestroyEventsForWindow(transient_xid2);
  EXPECT_FALSE(WindowIsOffscreen(xid1));
  EXPECT_EQ(transient_xid1, xconn_->focused_xid());
  EXPECT_EQ(transient_xid1, GetActiveWindowProperty());
  EXPECT_TRUE(WindowIsOffscreen(xid2));
  EXPECT_FALSE(lm_->active_mode_key_bindings_group_->enabled());

  // Now destroy the first window, check that re-enabled key bindings, and then
  // switch to overview mode.
  SendUnmapAndDestroyEventsForWindow(transient_xid1);
  EXPECT_TRUE(lm_->active_mode_key_bindings_group_->enabled());
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);
  ASSERT_TRUE(WindowIsOffscreen(xid1));
  ASSERT_TRUE(WindowIsOffscreen(xid2));

  // Create a transient for the first toplevel that already has the modal
  // hint set when it's mapped.
  XWindow transient_xid3 = CreateSimpleWindow();
  xconn_->GetWindowInfoOrDie(transient_xid3)->transient_for = xid1;
  AppendAtomToProperty(transient_xid3,
                       xconn_->GetAtomOrDie("_NET_WM_STATE"),
                       xconn_->GetAtomOrDie("_NET_WM_STATE_MODAL"));
  SendInitialEventsForWindow(transient_xid3);

  // Check that we switched back to active mode and focused the new
  // transient.
  EXPECT_FALSE(WindowIsOffscreen(xid1));
  EXPECT_EQ(transient_xid3, xconn_->focused_xid());
  EXPECT_EQ(transient_xid3, GetActiveWindowProperty());
  EXPECT_TRUE(WindowIsOffscreen(xid2));
  EXPECT_FALSE(lm_->active_mode_key_bindings_group_->enabled());
  SendUnmapAndDestroyEventsForWindow(transient_xid3);

  // Switch back to overview mode, create a non-modal transient for the
  // second window, and check that we don't exit overview mode.
  lm_->SetMode(LayoutManager::MODE_OVERVIEW);
  XWindow transient_xid4 = CreateSimpleWindow();
  xconn_->GetWindowInfoOrDie(transient_xid4)->transient_for = xid2;
  SendInitialEventsForWindow(transient_xid4);
  EXPECT_TRUE(WindowIsOffscreen(xid1));
  EXPECT_TRUE(WindowIsOffscreen(xid2));

  // Set the modal hint on the transient and check that we switch to its
  // toplevel window.
  xconn_->InitClientMessageEvent(
      &event, transient_xid4, xconn_->GetAtomOrDie("_NET_WM_STATE"),
      1, xconn_->GetAtomOrDie("_NET_WM_STATE_MODAL"), None, None, None);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(WindowIsOffscreen(xid1));
  EXPECT_EQ(transient_xid4, xconn_->focused_xid());
  EXPECT_EQ(transient_xid4, GetActiveWindowProperty());
  EXPECT_FALSE(WindowIsOffscreen(xid2));
  EXPECT_FALSE(lm_->active_mode_key_bindings_group_->enabled());

  // Now unmap the toplevel window owning the transient and check that we
  // undim the screen and re-enable bindings again.
  SendUnmapAndDestroyEventsForWindow(xid2);
  EXPECT_FALSE(WindowIsOffscreen(xid1));
  EXPECT_TRUE(lm_->active_mode_key_bindings_group_->enabled());
}

// Test that when we see a transient window claim to be owned by a
// non-toplevel window, we walk up the window tree until we find a toplevel
// window.  See http://crosbug.com/5846.
TEST_F(LayoutManagerTest, TransientOwnedByChildWindow) {
  XWindow toplevel_xid = CreateSimpleWindow();
  SendInitialEventsForWindow(toplevel_xid);

  XWindow first_child_xid = xconn_->CreateWindow(
      toplevel_xid,  // parent
      Rect(0, 0, 10, 10),
      false,         // override_redirect
      false,         // input_only
      0,             // event_mask
      0);            // visual
  XWindow second_child_xid = xconn_->CreateWindow(
      first_child_xid,  // parent
      Rect(0, 0, 10, 10),
      false,            // override_redirect
      false,            // input_only
      0,                // event_mask
      0);               // visual

  XWindow transient_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = second_child_xid;
  SendInitialEventsForWindow(transient_xid);

  // Check that the transient window's actor is shown and that the
  // transient window is correctly associated with the toplevel window.
  MockCompositor::TexturePixmapActor* transient_actor =
      GetMockActorForWindow(wm_->GetWindowOrDie(transient_xid));
  EXPECT_TRUE(transient_actor->is_shown());
  LayoutManager::ToplevelWindow* toplevel =
      lm_->GetToplevelWindowByXid(toplevel_xid);
  ASSERT_TRUE(toplevel != NULL);
  EXPECT_EQ(toplevel,
            lm_->GetToplevelWindowOwningTransientWindow(
                *(wm_->GetWindowOrDie(transient_xid))));
}

// Test that we close transient windows when their owners are unmapped.
TEST_F(LayoutManagerTest, CloseTransientWindowsWhenOwnerIsUnmapped) {
  XWindow owner_xid =
      CreateToplevelWindow(1, 0, Rect(0, 0, 640, 480));
  SendInitialEventsForWindow(owner_xid);

  XWindow transient_xid = CreateSimpleWindow();
  // Say that we support the WM_DELETE_WINDOW protocol.
  AppendAtomToProperty(transient_xid,
                       xconn_->GetAtomOrDie("WM_PROTOCOLS"),
                       xconn_->GetAtomOrDie("WM_DELETE_WINDOW"));
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = owner_xid;
  SendInitialEventsForWindow(transient_xid);

  // After we unmap the owner, the transient should receive a delete request.
  ASSERT_EQ(0, GetNumDeleteWindowMessagesForWindow(transient_xid));
  XEvent event;
  xconn_->InitUnmapEvent(&event, owner_xid);
  wm_->HandleEvent(&event);
  ASSERT_EQ(1, GetNumDeleteWindowMessagesForWindow(transient_xid));
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
