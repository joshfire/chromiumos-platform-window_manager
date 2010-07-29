// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/scoped_ptr.h"
#include "base/logging.h"
#include "window_manager/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/panel.h"
#include "window_manager/panel_bar.h"
#include "window_manager/panel_manager.h"
#include "window_manager/pointer_position_watcher.h"
#include "window_manager/shadow.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

namespace window_manager {

class PanelBarTest : public BasicWindowManagerTest {
 protected:
  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();
    panel_bar_ = wm_->panel_manager_->panel_bar_.get();
  }

  PanelBar* panel_bar_;  // instance belonging to wm_->panel_manager_
};

TEST_F(PanelBarTest, Basic) {
  // First, create a toplevel window.
  XWindow toplevel_xid = CreateSimpleWindow();
  SendInitialEventsForWindow(toplevel_xid);

  // It should be initially focused.
  EXPECT_EQ(toplevel_xid, xconn_->focused_xid());
  EXPECT_EQ(toplevel_xid, GetActiveWindowProperty());

  // Now create a panel titlebar, and then the content window.
  const int initial_titlebar_height = 16;
  XWindow titlebar_xid =
      CreatePanelTitlebarWindow(100, initial_titlebar_height);
  MockXConnection::WindowInfo* titlebar_info =
      xconn_->GetWindowInfoOrDie(titlebar_xid);
  SendInitialEventsForWindow(titlebar_xid);

  const int initial_content_width = 250;
  const int initial_content_height = 400;
  XWindow content_xid = CreatePanelContentWindow(
      initial_content_width, initial_content_height, titlebar_xid);
  MockXConnection::WindowInfo* content_info =
      xconn_->GetWindowInfoOrDie(content_xid);
  SendInitialEventsForWindow(content_xid);

  // The panel's content window should take the focus, and no button grab
  // should be installed yet.
  EXPECT_EQ(content_xid, xconn_->focused_xid());
  EXPECT_EQ(content_xid, GetActiveWindowProperty());

  // Click on the toplevel window to give it the focus again.  A button
  // grab should be installed on the panel's content window.
  xconn_->set_pointer_grab_xid(toplevel_xid);
  XEvent event;
  xconn_->InitButtonPressEvent(&event, toplevel_xid, 0, 0, 1);  // x, y, button
  wm_->HandleEvent(&event);
  EXPECT_EQ(None, xconn_->pointer_grab_xid());
  EXPECT_EQ(toplevel_xid, xconn_->focused_xid());
  EXPECT_TRUE(content_info->button_is_grabbed(AnyButton));
  EXPECT_EQ(toplevel_xid, GetActiveWindowProperty());

  // The titlebar should keep its initial height but be stretched to the
  // panel's width.  The content window's initial width and height should be
  // preserved.
  EXPECT_EQ(initial_content_width, titlebar_info->width);
  EXPECT_EQ(initial_titlebar_height, titlebar_info->height);
  EXPECT_EQ(initial_content_width, content_info->width);
  EXPECT_EQ(initial_content_height, content_info->height);

  // The titlebar and content client windows should be stacked above the
  // toplevel window's client window.
  EXPECT_LT(xconn_->stacked_xids().GetIndex(titlebar_xid),
            xconn_->stacked_xids().GetIndex(toplevel_xid));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(content_xid),
            xconn_->stacked_xids().GetIndex(toplevel_xid));

  Window* toplevel_win = wm_->GetWindowOrDie(toplevel_xid);
  Window* titlebar_win = wm_->GetWindowOrDie(titlebar_xid);
  Window* content_win = wm_->GetWindowOrDie(content_xid);

  // The titlebar and content actors and their shadows should all be stacked
  // on top of the toplevel window's actor.
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  EXPECT_LT(stage->GetStackingIndex(titlebar_win->actor()),
            stage->GetStackingIndex(toplevel_win->actor()));
  EXPECT_LT(stage->GetStackingIndex(titlebar_win->shadow()->group()),
            stage->GetStackingIndex(toplevel_win->actor()));
  EXPECT_LT(stage->GetStackingIndex(content_win->actor()),
            stage->GetStackingIndex(toplevel_win->actor()));
  EXPECT_LT(stage->GetStackingIndex(content_win->shadow()->group()),
            stage->GetStackingIndex(toplevel_win->actor()));

  // The titlebar and content windows shouldn't cast shadows on each other.
  EXPECT_LT(stage->GetStackingIndex(content_win->actor()),
            stage->GetStackingIndex(titlebar_win->shadow()->group()));
  EXPECT_LT(stage->GetStackingIndex(titlebar_win->actor()),
            stage->GetStackingIndex(content_win->shadow()->group()));

  // After a button press on the content window, its active and passive grabs
  // should be removed and it should be focused.
  xconn_->set_pointer_grab_xid(content_xid);
  xconn_->InitButtonPressEvent(&event, content_xid, 0, 0, 1);  // x, y, button
  wm_->HandleEvent(&event);
  EXPECT_EQ(None, xconn_->pointer_grab_xid());
  EXPECT_EQ(content_xid, xconn_->focused_xid());
  EXPECT_FALSE(content_info->button_is_grabbed(AnyButton));
  EXPECT_EQ(content_xid, GetActiveWindowProperty());

  // Create a second toplevel window.
  XWindow toplevel_xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(toplevel_xid2);
  Window* toplevel_win2 = wm_->GetWindowOrDie(toplevel_xid2);

  // Check that the new toplevel window takes the focus (note that this is
  // testing LayoutManager code).
  EXPECT_EQ(toplevel_xid2, xconn_->focused_xid());
  EXPECT_EQ(toplevel_xid2, GetActiveWindowProperty());

  // The panel's and titlebar's client and composited windows should be
  // stacked above those of the new toplevel window.
  EXPECT_LT(xconn_->stacked_xids().GetIndex(titlebar_xid),
            xconn_->stacked_xids().GetIndex(toplevel_xid2));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(content_xid),
            xconn_->stacked_xids().GetIndex(toplevel_xid2));
  EXPECT_LT(stage->GetStackingIndex(titlebar_win->actor()),
            stage->GetStackingIndex(toplevel_win2->actor()));
  EXPECT_LT(stage->GetStackingIndex(content_win->actor()),
            stage->GetStackingIndex(toplevel_win2->actor()));

  // Create a second, collapsed panel.
  new_panels_should_be_expanded_ = false;
  new_panels_should_take_focus_ = false;
  CreatePanel(200, 20, 400);

  // The collapsed panel shouldn't have taken the focus.
  EXPECT_EQ(toplevel_xid2, xconn_->focused_xid());
  EXPECT_EQ(toplevel_xid2, GetActiveWindowProperty());
}

// Test that we expand and focus panels in response to _NET_ACTIVE_WINDOW
// client messages.
TEST_F(PanelBarTest, ActiveWindowMessage) {
  // Create a collapsed panel.
  new_panels_should_be_expanded_ = false;
  new_panels_should_take_focus_ = false;
  Panel* panel = CreatePanel(200, 20, 400);

  // Make sure that it starts out collapsed.
  EXPECT_FALSE((panel)->is_expanded());
  EXPECT_NE(panel->content_xid(), xconn_->focused_xid());

  // After sending a _NET_ACTIVE_WINDOW message asking the window manager
  // to focus the panel, it should be expanded and get the focus, and the
  // _NET_ACTIVE_WINDOW property should contain its ID.
  XEvent event;
  xconn_->InitClientMessageEvent(
      &event,
      panel->content_xid(),  // window to focus
      wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW),
      1,                     // source indication: client app
      CurrentTime,
      None,                  // currently-active window
      None,
      None);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(panel->is_expanded());
  EXPECT_EQ(panel->content_xid(), xconn_->focused_xid());
  EXPECT_EQ(panel->content_xid(), GetActiveWindowProperty());
}

// Regression test for bug 540, a crash caused by PanelBar's window-unmap
// code calling WindowManager::TakeFocus() before the panel had been
// completely destroyed, resulting in PanelBar::TakeFocus() trying to
// refocus the partially-destroyed panel.
TEST_F(PanelBarTest, FocusNewPanel) {
  // Create an expanded panel.
  Panel* panel = CreatePanel(200, 20, 400);

  // It should be focused initially.
  EXPECT_EQ(panel->content_xid(), xconn_->focused_xid());
  EXPECT_EQ(panel->content_xid(), GetActiveWindowProperty());

  // The panel's address should be contained in 'desired_panel_to_focus_'.
  ASSERT_EQ(1, panel_bar_->packed_panels_.size());
  EXPECT_EQ(panel_bar_->packed_panels_[0], panel_bar_->desired_panel_to_focus_);

  // Now send an unmap event for the content window.  The panel object
  // should be destroyed, and 'desired_panel_to_focus_' shouldn't refer to
  // it anymore.
  XEvent event;
  xconn_->InitUnmapEvent(&event, panel->content_xid());
  wm_->HandleEvent(&event);
  EXPECT_TRUE(panel_bar_->packed_panels_.empty());
  EXPECT_EQ(NULL, panel_bar_->desired_panel_to_focus_);
}

// Basic tests of PanelBar's code for hiding all but the very top of
// collapsed panels' titlebars.
TEST_F(PanelBarTest, HideCollapsedPanels) {
  // Move the pointer to the top of the screen and create a collapsed panel.
  xconn_->SetPointerPosition(0, 0);
  new_panels_should_be_expanded_ = false;
  new_panels_should_take_focus_ = false;
  Panel* panel = CreatePanel(200, 20, 400);

  // Check that some constants make sense in light of our titlebar's height.
  ASSERT_LT(PanelBar::kHiddenCollapsedPanelHeightPixels,
            panel->titlebar_height());
  ASSERT_GT(PanelBar::kHideCollapsedPanelsDistancePixels,
            panel->titlebar_height());

  // Figure out where the top of hidden and shown panels should be.
  const int hidden_panel_y =
      wm_->height() - PanelBar::kHiddenCollapsedPanelHeightPixels;
  const int shown_panel_y = wm_->height() - panel->titlebar_height();

  // The panel should be initially hidden, and we shouldn't have a timer to
  // show the panels or be monitoring the pointer to hide them.
  EXPECT_EQ(hidden_panel_y, panel->titlebar_y());
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_HIDDEN,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(-1, panel_bar_->show_collapsed_panels_timeout_id_);
  EXPECT_TRUE(
      !panel_bar_->hide_collapsed_panels_pointer_watcher_.get() ||
      panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id() == -1);

  // Check that the show-collapsed-panels input window covers the bottom
  // row of pixels.
  XWindow input_xid = panel_bar_->show_collapsed_panels_input_xid_;
  MockXConnection::WindowInfo* input_info = xconn_->GetWindowInfoOrDie(
      panel_bar_->show_collapsed_panels_input_xid_);
  const int input_x = 0;
  const int input_y =
      wm_->height() - PanelBar::kShowCollapsedPanelsDistancePixels;
  const int input_width = wm_->width();
  const int input_height = PanelBar::kShowCollapsedPanelsDistancePixels;
  EXPECT_EQ(input_x, input_info->x);
  EXPECT_EQ(input_y, input_info->y);
  EXPECT_EQ(input_width, input_info->width);
  EXPECT_EQ(input_height, input_info->height);

  // Move the pointer to the bottom of the screen and send an event saying
  // that it's entered the input window.
  xconn_->SetPointerPosition(0, wm_->height() - 1);
  XEvent event;
  xconn_->InitEnterWindowEvent(&event, input_xid, 0, 0);
  wm_->HandleEvent(&event);

  // The panel should still be hidden, but we should be waiting to show it.
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_WAITING_TO_SHOW,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(hidden_panel_y, panel->titlebar_y());
  // TODO: We don't have a good way to trigger a GLib timer, so just check
  // that the timer has been set to show the panels.
  EXPECT_GE(panel_bar_->show_collapsed_panels_timeout_id_, 0);
  EXPECT_TRUE(
      !panel_bar_->hide_collapsed_panels_pointer_watcher_.get() ||
      panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id() == -1);

  // The input window still be in the same place.
  EXPECT_EQ(input_x, input_info->x);
  EXPECT_EQ(input_y, input_info->y);
  EXPECT_EQ(input_width, input_info->width);
  EXPECT_EQ(input_height, input_info->height);

  // Move the pointer back up immediately and send a leave notify event.
  xconn_->SetPointerPosition(
      0, wm_->height() - PanelBar::kShowCollapsedPanelsDistancePixels - 1);
  xconn_->InitLeaveWindowEvent(&event, input_xid, 0, 0);
  wm_->HandleEvent(&event);

  // The timer should be cancelled.
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_HIDDEN,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(hidden_panel_y, panel->titlebar_y());
  EXPECT_EQ(-1, panel_bar_->show_collapsed_panels_timeout_id_);
  EXPECT_TRUE(
      !panel_bar_->hide_collapsed_panels_pointer_watcher_.get() ||
      panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id() == -1);

  // The input window should also still be there.
  EXPECT_EQ(input_x, input_info->x);
  EXPECT_EQ(input_y, input_info->y);
  EXPECT_EQ(input_width, input_info->width);
  EXPECT_EQ(input_height, input_info->height);

  // Now move the pointer into the panel's titlebar.
  xconn_->SetPointerPosition(panel->titlebar_x(), panel->titlebar_y());
  xconn_->InitEnterWindowEvent(&event, panel->titlebar_xid(), 0, 0);
  wm_->HandleEvent(&event);

  // The panel should be shown immediately, and we should now be monitoring
  // the pointer's position so we can hide the panel if the pointer moves up.
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_SHOWN,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(shown_panel_y, panel->titlebar_y());
  EXPECT_EQ(-1, panel_bar_->show_collapsed_panels_timeout_id_);
  ASSERT_TRUE(panel_bar_->hide_collapsed_panels_pointer_watcher_.get() != NULL);
  ASSERT_GE(panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id(),
            0);

  // The input window should be offscreen.
  EXPECT_EQ(-1, input_info->x);
  EXPECT_EQ(-1, input_info->y);
  EXPECT_EQ(1, input_info->width);
  EXPECT_EQ(1, input_info->height);

  // Move the pointer to the left of the panel and one pixel above it.
  xconn_->SetPointerPosition(panel->titlebar_x() - 20, panel->titlebar_y() - 1);
  panel_bar_->hide_collapsed_panels_pointer_watcher_->TriggerTimeout();

  // We should still be showing the panel and watching the pointer's position.
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_SHOWN,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(shown_panel_y, panel->titlebar_y());
  EXPECT_EQ(-1, panel_bar_->show_collapsed_panels_timeout_id_);
  ASSERT_TRUE(panel_bar_->hide_collapsed_panels_pointer_watcher_.get() != NULL);
  ASSERT_GE(panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id(),
            0);

  // Move the pointer further up.
  xconn_->SetPointerPosition(
      panel->titlebar_x() - 20,
      wm_->height() - PanelBar::kHideCollapsedPanelsDistancePixels - 1);
  panel_bar_->hide_collapsed_panels_pointer_watcher_->TriggerTimeout();

  // The panel should be hidden now.
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_HIDDEN,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(hidden_panel_y, panel->titlebar_y());
  EXPECT_EQ(-1, panel_bar_->show_collapsed_panels_timeout_id_);
  EXPECT_TRUE(
      !panel_bar_->hide_collapsed_panels_pointer_watcher_.get() ||
      panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id() == -1);

  // The input window should also be moved back.
  EXPECT_EQ(input_x, input_info->x);
  EXPECT_EQ(input_y, input_info->y);
  EXPECT_EQ(input_width, input_info->width);
  EXPECT_EQ(input_height, input_info->height);

  // Move the pointer into the input window without passing through the
  // panel's titlebar again, but this time make it end up in the region
  // underneath the titlebar.
  xconn_->SetPointerPosition(input_x + input_width - 4, wm_->height() - 1);
  xconn_->InitEnterWindowEvent(&event, input_xid, input_width - 4, 0);
  wm_->HandleEvent(&event);

  // We should show the panel immediately in this case instead of using a timer.
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_SHOWN,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(shown_panel_y, panel->titlebar_y());
  ASSERT_TRUE(panel_bar_->hide_collapsed_panels_pointer_watcher_.get() != NULL);
  ASSERT_GE(panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id(),
            0);
}

// Test that we defer hiding collapsed panels if we're in the middle of a
// drag.
TEST_F(PanelBarTest, DeferHidingDraggedCollapsedPanel) {
  new_panels_should_be_expanded_ = false;
  new_panels_should_take_focus_ = false;
  Panel* panel = CreatePanel(200, 20, 400);

  const int hidden_panel_y =
      wm_->height() - PanelBar::kHiddenCollapsedPanelHeightPixels;
  const int shown_panel_y = wm_->height() - panel->titlebar_height();

  // Show the panel.
  xconn_->SetPointerPosition(panel->titlebar_x(), panel->titlebar_y());
  XEvent event;
  xconn_->InitEnterWindowEvent(&event, panel->titlebar_xid(), 0, 0);
  wm_->HandleEvent(&event);
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_SHOWN,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(shown_panel_y, panel->titlebar_y());
  ASSERT_TRUE(panel_bar_->hide_collapsed_panels_pointer_watcher_.get() != NULL);
  ASSERT_GE(panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id(),
            0);
  panel_bar_->hide_collapsed_panels_pointer_watcher_->TriggerTimeout();

  // Drag the panel to the left.
  SendPanelDraggedMessage(panel, 300, shown_panel_y);
  EXPECT_EQ(300, panel->right());

  // We should still show the panel and be monitoring the pointer's position.
  xconn_->SetPointerPosition(300, shown_panel_y);
  ASSERT_TRUE(panel_bar_->hide_collapsed_panels_pointer_watcher_.get() != NULL);
  ASSERT_GE(panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id(),
            0);
  panel_bar_->hide_collapsed_panels_pointer_watcher_->TriggerTimeout();
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_SHOWN,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(shown_panel_y, panel->titlebar_y());

  // Now drag up above the threshold to hide the panel.  We should still
  // be showing it since we're in a drag, but we should be ready to hide it.
  const int hide_pointer_y =
      wm_->height() - PanelBar::kHideCollapsedPanelsDistancePixels - 1;
  SendPanelDraggedMessage(panel, 300, hide_pointer_y);

  // The watcher should run as soon as it sees the position, but we
  // shouldn't hide the dragged panel yet.
  xconn_->SetPointerPosition(300, hide_pointer_y);
  ASSERT_TRUE(panel_bar_->hide_collapsed_panels_pointer_watcher_.get() != NULL);
  ASSERT_GE(panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id(),
            0);
  panel_bar_->hide_collapsed_panels_pointer_watcher_->TriggerTimeout();
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_WAITING_TO_HIDE,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(shown_panel_y, panel->titlebar_y());

  // When we complete the drag, the panel should be hidden.
  SendPanelDragCompleteMessage(panel);
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_HIDDEN,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(hidden_panel_y, panel->titlebar_y());
  EXPECT_TRUE(
      !panel_bar_->hide_collapsed_panels_pointer_watcher_.get() ||
      panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id() == -1);

  // Show the panel again.
  xconn_->SetPointerPosition(panel->titlebar_x(), panel->titlebar_y());
  xconn_->InitEnterWindowEvent(&event, panel->titlebar_xid(), 0, 0);
  wm_->HandleEvent(&event);
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_SHOWN,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(shown_panel_y, panel->titlebar_y());

  // Drag up again (drag to the left first to make sure that we're moving
  // the panel within the bar rather than detaching it).
  SendPanelDraggedMessage(panel, panel->right() - 200, panel->titlebar_y());
  SendPanelDraggedMessage(panel, 300, hide_pointer_y);
  xconn_->SetPointerPosition(300, hide_pointer_y);
  ASSERT_TRUE(panel_bar_->hide_collapsed_panels_pointer_watcher_.get() != NULL);
  ASSERT_GE(panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id(),
            0);
  panel_bar_->hide_collapsed_panels_pointer_watcher_->TriggerTimeout();
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_WAITING_TO_HIDE,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(shown_panel_y, panel->titlebar_y());

  // Now move the pointer back down before ending the drag.  The bar should
  // see that the pointer is back within the threshold and avoid hiding the
  // panel.  We should be monitoring the pointer position again.
  xconn_->SetPointerPosition(300, shown_panel_y);
  SendPanelDragCompleteMessage(panel);
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_SHOWN,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(shown_panel_y, panel->titlebar_y());
  ASSERT_TRUE(panel_bar_->hide_collapsed_panels_pointer_watcher_.get() != NULL);
  ASSERT_GE(panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id(),
            0);

  // Move the pointer up again without dragging and check that the panel is
  // hidden.
  xconn_->SetPointerPosition(300, hide_pointer_y);
  panel_bar_->hide_collapsed_panels_pointer_watcher_->TriggerTimeout();
  EXPECT_EQ(PanelBar::COLLAPSED_PANEL_STATE_HIDDEN,
            panel_bar_->collapsed_panel_state_);
  EXPECT_EQ(hidden_panel_y, panel->titlebar_y());
  EXPECT_TRUE(
      !panel_bar_->hide_collapsed_panels_pointer_watcher_.get() ||
      panel_bar_->hide_collapsed_panels_pointer_watcher_->timeout_id() == -1);
}

TEST_F(PanelBarTest, ReorderPanels) {
  // Create two 200-pixel-wide panels.
  const int width = 200;
  new_panels_should_be_expanded_ = false;
  new_panels_should_take_focus_ = false;
  Panel* panel1 = CreatePanel(width, 20, 400);
  Panel* panel2 = CreatePanel(width, 20, 400);

  // Initially, panel1 should be on the right and panel2 to its left.
  const int rightmost_right_edge =
      wm_->width() - PanelBar::kRightPaddingPixels;
  const int leftmost_right_edge =
      rightmost_right_edge - width - PanelBar::kPixelsBetweenPanels;
  EXPECT_EQ(rightmost_right_edge, panel1->right());
  EXPECT_EQ(leftmost_right_edge, panel2->right());

  // Drag panel1 to the right and check that nothing happens to panel2.
  const int drag_y = wm_->height() - 1;
  int drag_x = rightmost_right_edge + width;
  SendPanelDraggedMessage(panel1, drag_x, drag_y);
  EXPECT_EQ(drag_x, panel1->right());
  EXPECT_EQ(leftmost_right_edge, panel2->right());

  // Drag panel1 almost far enough to displace panel2, which should remain
  // in the leftmost position.
  drag_x = leftmost_right_edge + 0.5 * width + 1;
  SendPanelDraggedMessage(panel1, drag_x, drag_y);
  EXPECT_EQ(drag_x, panel1->right());
  EXPECT_EQ(leftmost_right_edge, panel2->right());

  // If we drag it one pixel further, panel2 should snap over to the
  // rightmost position.
  drag_x -= 1;
  SendPanelDraggedMessage(panel1, drag_x, drag_y);
  EXPECT_EQ(drag_x, panel1->right());
  EXPECT_EQ(rightmost_right_edge, panel2->right());

  // It should stay there if we drag panel1 way over to the left.
  SendPanelDraggedMessage(panel1, 40, drag_y);
  EXPECT_EQ(40, panel1->right());
  EXPECT_EQ(rightmost_right_edge, panel2->right());

  // Now drag back to the right (so that we don't end up with a floating
  // panel), end the drag, and check that panel1 snaps to the leftmost
  // position.
  SendPanelDraggedMessage(panel1, drag_x - 0.5 * width, drag_y);
  SendPanelDragCompleteMessage(panel1);
  EXPECT_EQ(leftmost_right_edge, panel1->right());
  EXPECT_EQ(rightmost_right_edge, panel2->right());
}

// Check that we do something at least halfway reasonable when reordering
// differently-sized panels.
TEST_F(PanelBarTest, ReorderDifferentlySizedPanels) {
  new_panels_should_be_expanded_ = false;
  new_panels_should_take_focus_ = false;
  const int small_width = 200;
  Panel* small_panel = CreatePanel(small_width, 20, 400);
  const int big_width = 500;
  Panel* big_panel = CreatePanel(big_width, 20, 400);

  const int rightmost_right_edge =
      wm_->width() - PanelBar::kRightPaddingPixels;
  const int leftmost_right_edge_for_small =
      rightmost_right_edge - big_width - PanelBar::kPixelsBetweenPanels;
  const int leftmost_right_edge_for_big =
      rightmost_right_edge - small_width - PanelBar::kPixelsBetweenPanels;
  EXPECT_EQ(rightmost_right_edge, small_panel->right());
  EXPECT_EQ(leftmost_right_edge_for_big, big_panel->right());

  // Drag the small panel partway to the left, but not enough to swap it
  // with the big panel.
  const int drag_y = wm_->height() - 1;
  int drag_x = leftmost_right_edge_for_big - 0.5 * big_width + small_width + 1;
  SendPanelDraggedMessage(small_panel, drag_x, drag_y);
  EXPECT_EQ(drag_x, small_panel->right());
  EXPECT_EQ(leftmost_right_edge_for_big, big_panel->right());

  // If we drag it one pixel further, the big panel should move to the right.
  drag_x -= 1;
  SendPanelDraggedMessage(small_panel, drag_x, drag_y);
  EXPECT_EQ(drag_x, small_panel->right());
  EXPECT_EQ(rightmost_right_edge, big_panel->right());

  // Drag it one pixel further to make sure that nothing funny happens (in
  // a previous implementation, the reordering code was unstable in some
  // cases and could make the big panel jump back here).
  drag_x -= 1;
  SendPanelDraggedMessage(small_panel, drag_x, drag_y);
  EXPECT_EQ(drag_x, small_panel->right());
  EXPECT_EQ(rightmost_right_edge, big_panel->right());

  // If we drag it back to the right, the big panel should move back to the
  // left (we need to move it further back to account for the additional
  // padding that's introduced due to the big panel's current position on
  // the right).
  drag_x = rightmost_right_edge - 0.5 * big_width + 1;
  SendPanelDraggedMessage(small_panel, drag_x, drag_y);
  EXPECT_EQ(drag_x, small_panel->right());
  EXPECT_EQ(leftmost_right_edge_for_big, big_panel->right());

  // Drag it far to the left and check that the big panel moves to the right.
  SendPanelDraggedMessage(small_panel, 10, drag_y);
  EXPECT_EQ(10, small_panel->right());
  EXPECT_EQ(rightmost_right_edge, big_panel->right());

  // After ending the drag, the small panel should jump to the leftmost
  // position.
  SendPanelDraggedMessage(small_panel, leftmost_right_edge_for_small, drag_y);
  SendPanelDragCompleteMessage(small_panel);
  EXPECT_EQ(leftmost_right_edge_for_small, small_panel->right());
  EXPECT_EQ(rightmost_right_edge, big_panel->right());

  // Now drag the big panel to the left, but not far enough to displace the
  // small panel.
  drag_x = leftmost_right_edge_for_small - 0.5 * small_width + big_width + 1;
  SendPanelDraggedMessage(big_panel, drag_x, drag_y);
  EXPECT_EQ(leftmost_right_edge_for_small, small_panel->right());
  EXPECT_EQ(drag_x, big_panel->right());

  // The small panel should jump to the right after we drag another pixel.
  drag_x -= 1;
  SendPanelDraggedMessage(big_panel, drag_x, drag_y);
  EXPECT_EQ(rightmost_right_edge, small_panel->right());
  EXPECT_EQ(drag_x, big_panel->right());

  // It should go back to the left if we drag back.
  drag_x = rightmost_right_edge - 0.5 * small_width + 1;
  SendPanelDraggedMessage(big_panel, drag_x, drag_y);
  EXPECT_EQ(leftmost_right_edge_for_small, small_panel->right());
  EXPECT_EQ(drag_x, big_panel->right());

  // The big panel should snap to the right after the drag ends.
  SendPanelDragCompleteMessage(big_panel);
  EXPECT_EQ(leftmost_right_edge_for_small, small_panel->right());
  EXPECT_EQ(rightmost_right_edge, big_panel->right());
}

TEST_F(PanelBarTest, PackPanelsAfterPanelResize) {
  // Create three 200-pixel-wide panels.
  new_panels_should_be_expanded_ = false;
  new_panels_should_take_focus_ = false;
  Panel* panel1 = CreatePanel(200, 20, 400);
  Panel* panel2 = CreatePanel(200, 20, 400);
  Panel* panel3 = CreatePanel(200, 20, 400);

  // The panels should be crammed together on the right initially.
  EXPECT_EQ(wm_->width() - PanelBar::kRightPaddingPixels, panel1->right());
  EXPECT_EQ(wm_->width() - PanelBar::kRightPaddingPixels -
              PanelBar::kPixelsBetweenPanels - 200,
            panel2->right());
  EXPECT_EQ(wm_->width() - PanelBar::kRightPaddingPixels -
              2 * PanelBar::kPixelsBetweenPanels - 2 * 200,
            panel3->right());

  // Drag the middle window's upper-left resize handle to resize it to
  // (400, 600).
  XWindow input_xid = panel2->top_left_input_xid_;
  XEvent event;
  xconn_->InitButtonPressEvent(&event, input_xid, 0, 0, 1);
  wm_->HandleEvent(&event);
  xconn_->InitMotionNotifyEvent(&event, input_xid, -200, -200);
  wm_->HandleEvent(&event);
  xconn_->InitButtonReleaseEvent(&event, input_xid, -200, -200, 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(400, panel2->width());
  EXPECT_EQ(600, panel2->content_height());

  // The right edges of panel1 and panel2 should be in the same place as
  // before, but panel3 should be pushed to the left to make room for panel2.
  EXPECT_EQ(wm_->width() - PanelBar::kRightPaddingPixels, panel1->right());
  EXPECT_EQ(wm_->width() - PanelBar::kRightPaddingPixels -
              PanelBar::kPixelsBetweenPanels - 200,
            panel2->right());
  EXPECT_EQ(wm_->width() - PanelBar::kRightPaddingPixels -
              2 * PanelBar::kPixelsBetweenPanels - 200 - 400,
            panel3->right());
}

TEST_F(PanelBarTest, UrgentPanel) {
  // Move the pointer to the top of the screen and create a collapsed panel.
  xconn_->SetPointerPosition(0, 0);
  new_panels_should_be_expanded_ = false;
  new_panels_should_take_focus_ = false;
  Panel* panel = CreatePanel(200, 20, 400);

  // Figure out where the top of the panel should be in various states.
  const int hidden_panel_y =
      wm_->height() - PanelBar::kHiddenCollapsedPanelHeightPixels;
  const int shown_panel_y = wm_->height() - panel->titlebar_height();
  const int expanded_panel_y = wm_->height() - panel->total_height();

  // The panel should be hidden initially.
  EXPECT_EQ(hidden_panel_y, panel->titlebar_y());

  // Set the urgency hint on the panel's content window, notify the window
  // manager, and check that the panel is shown now.
  const XAtom wm_hints_atom = wm_->GetXAtom(ATOM_WM_HINTS);
  const int urgency_hint = 256;  // UrgencyHint flag from ICCCM 4.1.2.4
  xconn_->SetIntProperty(panel->content_xid(),
                         wm_hints_atom,  // atom
                         wm_hints_atom,  // type
                         urgency_hint);
  XEvent notify_event;
  xconn_->InitPropertyNotifyEvent(
      &notify_event, panel->content_xid(), wm_hints_atom);
  wm_->HandleEvent(&notify_event);
  EXPECT_EQ(shown_panel_y, panel->titlebar_y());

  // Now unset the hint and check that the panel is hidden again.
  xconn_->SetIntProperty(panel->content_xid(), wm_hints_atom, wm_hints_atom, 0);
  wm_->HandleEvent(&notify_event);
  EXPECT_EQ(hidden_panel_y, panel->titlebar_y());

  // Tell the window manager to expand the panel.
  SendSetPanelStateMessage(panel, true);
  EXPECT_EQ(expanded_panel_y, panel->titlebar_y());

  // Nothing should happen if we set or unset the hint on an expanded panel.
  xconn_->SetIntProperty(
      panel->content_xid(), wm_hints_atom, wm_hints_atom, urgency_hint);
  wm_->HandleEvent(&notify_event);
  EXPECT_EQ(expanded_panel_y, panel->titlebar_y());
  xconn_->SetIntProperty(
      panel->content_xid(), wm_hints_atom, wm_hints_atom, 0);
  wm_->HandleEvent(&notify_event);
  EXPECT_EQ(expanded_panel_y, panel->titlebar_y());

  // Set the hint again and collapse the panel.  It shouldn't be hidden.
  xconn_->SetIntProperty(
      panel->content_xid(), wm_hints_atom, wm_hints_atom, urgency_hint);
  wm_->HandleEvent(&notify_event);
  SendSetPanelStateMessage(panel, false);
  EXPECT_EQ(shown_panel_y, panel->titlebar_y());
}

TEST_F(PanelBarTest, DragPanelVertically) {
  // Create an expanded panel.
  Panel* panel = CreatePanel(200, 20, 400);

  const int right_edge = wm_->width() - PanelBar::kRightPaddingPixels;
  EXPECT_EQ(right_edge, panel->right());
  const int expanded_y = wm_->height() - panel->total_height();
  EXPECT_EQ(expanded_y, panel->titlebar_y());
  const int hidden_y =
      wm_->height() - PanelBar::kHiddenCollapsedPanelHeightPixels;

  // Drag the panel a bit to the left and further down.  We should start a
  // vertical drag and ignore the horizontal change.
  SendPanelDraggedMessage(panel, right_edge - 6, expanded_y + 10);
  EXPECT_EQ(right_edge, panel->right());
  EXPECT_EQ(expanded_y + 10, panel->titlebar_y());

  // As long as we're in the drag, we should ignore horizontal movement.
  SendPanelDraggedMessage(panel, right_edge + 30, expanded_y + 15);
  EXPECT_EQ(right_edge, panel->right());
  EXPECT_EQ(expanded_y + 15, panel->titlebar_y());

  // When the drag ends, the panel should snap back to the expanded position.
  SendPanelDragCompleteMessage(panel);
  EXPECT_EQ(right_edge, panel->right());
  EXPECT_EQ(expanded_y, panel->titlebar_y());
  EXPECT_TRUE(panel->is_expanded());

  // Now drag it more than halfway down and check that we collapse the
  // panel when the drag ends.
  SendPanelDraggedMessage(panel, right_edge,
                          expanded_y + 0.5 * panel->total_height() + 1);
  SendPanelDragCompleteMessage(panel);
  EXPECT_EQ(right_edge, panel->right());
  EXPECT_EQ(hidden_y, panel->titlebar_y());
  EXPECT_FALSE(panel->is_expanded());

  // Check that we can expand the panel by dragging it up, too.
  SendPanelDraggedMessage(panel, right_edge,
                          expanded_y + 0.5 * panel->total_height() - 1);
  SendPanelDragCompleteMessage(panel);
  EXPECT_EQ(right_edge, panel->right());
  EXPECT_EQ(expanded_y, panel->titlebar_y());
  EXPECT_TRUE(panel->is_expanded());

  // Drag the panel up to the top of the screen to detach it (center it
  // horizontally to make sure that other panel containers, namely
  // PanelDock, don't swallow the panel).
  const int drag_x = 0.5 * (wm_->width() + panel->width());
  SendPanelDraggedMessage(panel, drag_x, 0);
  EXPECT_EQ(drag_x, panel->right());
  EXPECT_EQ(0, panel->titlebar_y());

  // Now drag the panel straight down, a bit below the expanded position.
  // It should stop at the expanded position, since we always enter a
  // horizontal drag as soon as we attach.
  SendPanelDraggedMessage(panel, drag_x, expanded_y + 20);
  EXPECT_EQ(drag_x, panel->right());
  EXPECT_EQ(expanded_y, panel->titlebar_y());
  SendPanelDraggedMessage(panel, drag_x + 20, expanded_y + 30);
  EXPECT_EQ(drag_x + 20, panel->right());
  EXPECT_EQ(expanded_y, panel->titlebar_y());
  SendPanelDragCompleteMessage(panel);
}

// Test that Chrome can request that a panel not be initially focused.
TEST_F(PanelBarTest, AvoidInitialFocus) {
  // If there are no other windows present, the panel should be focused
  // even if it asks not to be.
  new_panels_should_take_focus_ = false;
  Panel* initial_panel = CreatePanel(200, 20, 300);
  EXPECT_EQ(initial_panel->content_xid(), xconn_->focused_xid());
  EXPECT_EQ(initial_panel->content_xid(), GetActiveWindowProperty());

  // Create a second panel that also asks not to be focused and check that
  // it isn't.
  CreatePanel(200, 20, 300);
  EXPECT_EQ(initial_panel->content_xid(), xconn_->focused_xid());
  EXPECT_EQ(initial_panel->content_xid(), GetActiveWindowProperty());

  // Now create one that asks for the focus and check that it gets it.
  new_panels_should_take_focus_ = true;
  Panel* new_panel = CreatePanel(200, 20, 300);
  EXPECT_EQ(new_panel->content_xid(), xconn_->focused_xid());
  EXPECT_EQ(new_panel->content_xid(), GetActiveWindowProperty());
}

// Test that a panel that defines the XID of its creator gets placed to the
// immediate left of it.
TEST_F(PanelBarTest, OpenPanelNextToCreator) {
  const int kPanelWidth = 200, kTitlebarHeight = 20, kContentHeight = 300;
  const int kRightPadding = PanelBar::kRightPaddingPixels;
  const int kSpacing = PanelBar::kPixelsBetweenPanels;

  // Create a few panels, which will be arranged as: 3 2 1.
  Panel* panel1 = CreatePanel(kPanelWidth, kTitlebarHeight, kContentHeight);
  Panel* panel2 = CreatePanel(kPanelWidth, kTitlebarHeight, kContentHeight);
  Panel* panel3 = CreatePanel(kPanelWidth, kTitlebarHeight, kContentHeight);

  // Now create a panel that asks to be to the immediate left of 'panel1'
  // and check that it ends up there: 3 2 4 1.
  creator_content_xid_for_new_panels_ = panel1->content_xid();
  Panel* panel4 = CreatePanel(kPanelWidth, kTitlebarHeight, kContentHeight);
  EXPECT_EQ(wm_->width() - kRightPadding, panel1->right());
  EXPECT_EQ(panel1->content_x() - kSpacing, panel4->right());
  EXPECT_EQ(panel4->content_x() - kSpacing, panel2->right());
  EXPECT_EQ(panel2->content_x() - kSpacing, panel3->right());

  // Now create a panel to the left of 2: 3 5 2 3 1.
  creator_content_xid_for_new_panels_ = panel2->content_xid();
  Panel* panel5 = CreatePanel(kPanelWidth, kTitlebarHeight, kContentHeight);
  EXPECT_EQ(wm_->width() - kRightPadding, panel1->right());
  EXPECT_EQ(panel1->content_x() - kSpacing, panel4->right());
  EXPECT_EQ(panel4->content_x() - kSpacing, panel2->right());
  EXPECT_EQ(panel2->content_x() - kSpacing, panel5->right());
  EXPECT_EQ(panel5->content_x() - kSpacing, panel3->right());

  // Create one that asks to be to the left of 3: 6 3 5 2 3 1.
  creator_content_xid_for_new_panels_ = panel3->content_xid();
  Panel* panel6 = CreatePanel(kPanelWidth, kTitlebarHeight, kContentHeight);
  EXPECT_EQ(wm_->width() - kRightPadding, panel1->right());
  EXPECT_EQ(panel1->content_x() - kSpacing, panel4->right());
  EXPECT_EQ(panel4->content_x() - kSpacing, panel2->right());
  EXPECT_EQ(panel2->content_x() - kSpacing, panel5->right());
  EXPECT_EQ(panel5->content_x() - kSpacing, panel3->right());
  EXPECT_EQ(panel3->content_x() - kSpacing, panel6->right());

  // Finally, create one with a bogus creator XID and check that it just
  // gets placed on the left: 7 6 3 5 2 3 1.
  creator_content_xid_for_new_panels_ = 4324322;  // bogus creator XID
  Panel* panel7 = CreatePanel(kPanelWidth, kTitlebarHeight, kContentHeight);
  EXPECT_EQ(wm_->width() - kRightPadding, panel1->right());
  EXPECT_EQ(panel1->content_x() - kSpacing, panel4->right());
  EXPECT_EQ(panel4->content_x() - kSpacing, panel2->right());
  EXPECT_EQ(panel2->content_x() - kSpacing, panel5->right());
  EXPECT_EQ(panel5->content_x() - kSpacing, panel3->right());
  EXPECT_EQ(panel3->content_x() - kSpacing, panel6->right());
  EXPECT_EQ(panel6->content_x() - kSpacing, panel7->right());
}

TEST_F(PanelBarTest, FloatingPanels) {
  const int kPanelWidth = 200, kTitlebarHeight = 20, kContentHeight = 300;
  const int kFloatingThreshold = PanelBar::kFloatingPanelThresholdPixels;
  const int kRightPadding = PanelBar::kRightPaddingPixels;
  const int kSpacing = PanelBar::kPixelsBetweenPanels;

  // Create a few panels.
  //             +---++---++---+
  //             | 3 || 2 || 1 |
  // ------------+---++---++---+
  Panel* panel1 = CreatePanel(kPanelWidth, kTitlebarHeight, kContentHeight);
  Panel* panel2 = CreatePanel(kPanelWidth, kTitlebarHeight, kContentHeight);
  Panel* panel3 = CreatePanel(kPanelWidth, kTitlebarHeight, kContentHeight);

  // If we don't drag panel3 far enough to the left, it should snap back to
  // the main group of packed panels.
  //            +---+ +---++---+
  //            | 3 | | 2 || 1 |
  // -----------+---+-+---++---+
  int right_edge = panel3->right();
  int drag_y = panel3->titlebar_y();
  SendPanelDraggedMessage(panel3, right_edge - kFloatingThreshold, drag_y);
  SendPanelDragCompleteMessage(panel3);
  EXPECT_EQ(right_edge, panel3->right());

  // If we drag it further but return it before ending the drag, it should
  // also snap back.
  SendPanelDraggedMessage(panel3, right_edge - kFloatingThreshold - 20, drag_y);
  SendPanelDraggedMessage(panel3, right_edge - kFloatingThreshold, drag_y);
  SendPanelDragCompleteMessage(panel3);
  EXPECT_EQ(right_edge, panel3->right());

  // If we drag it further without returning it, it should stay there.
  //           +---+  +---++---+
  //           | 3 |  | 2 || 1 |
  // ----------+---+--+---++---+
  int drag_x = right_edge - kFloatingThreshold - 1;
  SendPanelDraggedMessage(panel3, drag_x, drag_y);
  SendPanelDragCompleteMessage(panel3);
  EXPECT_EQ(drag_x, panel3->right());
  EXPECT_TRUE(PanelClientAndCompositedWindowsHaveSamePositions(panel3));

  // Drag panel2 to the left so it overlaps the right edge of panel3, and
  // check that panel3 gets pushed to the left to make room.
  //          +---++---+   +---+
  //          | 3 || 2 |   | 1 |
  // ---------+---++---+---+---+
  drag_x = panel2->right() - kFloatingThreshold - 10;
  SendPanelDraggedMessage(panel2, drag_x, drag_y);
  SendPanelDragCompleteMessage(panel2);
  EXPECT_EQ(drag_x, panel2->right());
  EXPECT_EQ(panel2->content_x() - kSpacing, panel3->right());
  EXPECT_TRUE(PanelClientAndCompositedWindowsHaveSamePositions(panel2));
  EXPECT_TRUE(PanelClientAndCompositedWindowsHaveSamePositions(panel3));

  // Drag panel2 further to the left, and check that panel3 jumps over to
  // its right (where there should be enough room for it -- panel2 is 1.5
  // panel widths from the left edge of panel 1).
  //           +---++---+  +---+
  //           | 2 || 3 |  | 1 |
  // ----------+---++---+--+---+
  drag_x = panel1->content_x() - 1.5 * kPanelWidth;
  SendPanelDraggedMessage(panel2, drag_x, drag_y);
  SendPanelDragCompleteMessage(panel2);
  EXPECT_EQ(drag_x, panel2->right());
  EXPECT_EQ(panel2->right() + kSpacing, panel3->content_x());

  // Open another panel.  It should appear to the left of panel1 (the only
  // packed panel) and push the two floating panels to the left.
  //        +---++---++---++---+
  //        | 2 || 3 || 4 || 1 |
  // -------+---++---++---++---+
  int prev_panel2_right = drag_x;
  Panel* panel4 = CreatePanel(kPanelWidth, kTitlebarHeight, kContentHeight);
  EXPECT_EQ(wm_->width() - kRightPadding, panel1->right());
  EXPECT_EQ(panel1->content_x() - kSpacing, panel4->right());
  EXPECT_EQ(panel4->content_x() - kSpacing, panel3->right());
  EXPECT_EQ(panel3->content_x() - kSpacing, panel2->right());

  // If panel4 is dragged to the far left, panel2 and panel3 should snap
  // back to their previous locations.
  // +---+     +---++---+  +---+
  // | 4 |     | 2 || 3 |  | 1 |
  // +---+-----+---++---+--+---+
  SendPanelDraggedMessage(panel4, kPanelWidth, drag_y);
  SendPanelDragCompleteMessage(panel4);
  EXPECT_EQ(kPanelWidth, panel4->right());
  EXPECT_EQ(prev_panel2_right, panel2->right());
  EXPECT_EQ(panel2->right() + kSpacing, panel3->content_x());

  // Drag panel3 to the right and check that it becomes a packed panel,
  // displacing panel1 even before the drag has completed.
  // +---+     +---+  +---++---+
  // | 4 |     | 2 |  | 1 || 3 |
  // +---+-----+---+--+---++---+
  drag_x = wm_->width() - 0.25 * kPanelWidth;
  SendPanelDraggedMessage(panel3, drag_x, drag_y);
  EXPECT_EQ(drag_x, panel3->right());
  EXPECT_EQ(wm_->width() - kRightPadding - kPanelWidth - kSpacing,
            panel1->right());
  EXPECT_EQ(prev_panel2_right, panel2->right());
  SendPanelDragCompleteMessage(panel3);
  EXPECT_EQ(wm_->width() - kRightPadding, panel3->right());

  // Close panel4, and then start dragging panel1 to the left in small
  // increments.  It should initially push panel2 to the left, but when
  // panel1's left edge has passed the midpoint of panel2's desired
  // position, panel2 should jump to the right.
  //           +---++---+  +---+
  //           | 1 || 2 |  | 3 |
  // ----------+---++---+--+---+
  XEvent event;
  xconn_->InitUnmapEvent(&event, panel4->content_xid());
  wm_->HandleEvent(&event);

  drag_x = prev_panel2_right + 0.5 * kPanelWidth + 1;
  SendPanelDraggedMessage(panel1, drag_x, drag_y);
  SendPanelDragCompleteMessage(panel1);
  EXPECT_EQ(drag_x, panel1->right());
  EXPECT_EQ(panel1->content_x() - kSpacing, panel2->right());

  drag_x = prev_panel2_right;
  SendPanelDraggedMessage(panel1, drag_x, drag_y);
  SendPanelDragCompleteMessage(panel1);
  EXPECT_EQ(drag_x, panel1->right());
  EXPECT_EQ(panel1->right() + kSpacing, panel2->content_x());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
