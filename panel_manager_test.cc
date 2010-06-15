// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/scoped_ptr.h"
#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/event_loop.h"
#include "window_manager/layout_manager.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/panel.h"
#include "window_manager/panel_bar.h"
#include "window_manager/panel_dock.h"
#include "window_manager/panel_manager.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/test_lib.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::min;

namespace window_manager {

class PanelManagerTest : public BasicWindowManagerTest {
 protected:
  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();
    panel_manager_ = wm_->panel_manager_.get();
    panel_bar_ = panel_manager_->panel_bar_.get();
    left_panel_dock_ = panel_manager_->left_panel_dock_.get();
    right_panel_dock_ = panel_manager_->right_panel_dock_.get();
    layout_manager_ = wm_->layout_manager_.get();
  }

  PanelManager* panel_manager_;    // instance belonging to 'wm_'
  PanelBar* panel_bar_;            // instance belonging to 'panel_manager_'
  PanelDock* left_panel_dock_;     // instance belonging to 'panel_manager_'
  PanelDock* right_panel_dock_;    // instance belonging to 'panel_manager_'
  LayoutManager* layout_manager_;  // instance belonging to 'wm_'
};

// Test dragging a panel around to detach it and reattach it to the panel
// bar and panel docks.
TEST_F(PanelManagerTest, AttachAndDetach) {
  XConnection::WindowGeometry root_geometry;
  ASSERT_TRUE(
      xconn_->GetWindowGeometry(xconn_->GetRootWindow(), &root_geometry));

  const int titlebar_height = 20;
  const int content_width = 200;
  const int content_height = 400;
  Panel* panel =
      CreateSimplePanel(content_width, titlebar_height, content_height);

  // Get the position of the top of the expanded panel when it's in the bar.
  const int panel_y_in_bar = wm_->height() - content_height - titlebar_height;

  // Drag the panel to the left, keeping it in line with the panel bar.
  SendPanelDraggedMessage(panel, 600, panel_y_in_bar);
  EXPECT_EQ(600, panel->right());
  EXPECT_EQ(panel_y_in_bar, panel->titlebar_y());

  // Drag it up a bit, but not enough to detach it.
  SendPanelDraggedMessage(panel, 600, panel_y_in_bar - 5);
  EXPECT_EQ(600, panel->right());
  EXPECT_EQ(panel_y_in_bar, panel->titlebar_y());

  // Now drag it up near the top of the screen.  It should get detached and
  // move to the same position as the mouse pointer.
  SendPanelDraggedMessage(panel, 500, 50);
  EXPECT_EQ(500, panel->right());
  EXPECT_EQ(50, panel->titlebar_y());

  // Drag the panel to a different spot near the top of the screen.
  SendPanelDraggedMessage(panel, 700, 25);
  EXPECT_EQ(700, panel->right());
  EXPECT_EQ(25, panel->titlebar_y());

  // Drag the panel all the way down to reattach it.
  SendPanelDraggedMessage(panel, 700, wm_->height() - 1);
  EXPECT_EQ(700, panel->right());
  EXPECT_EQ(panel_y_in_bar, panel->titlebar_y());

  // Detach the panel again.
  SendPanelDraggedMessage(panel, 700, 20);
  EXPECT_EQ(700, panel->right());
  EXPECT_EQ(20, panel->titlebar_y());

  // Move the panel to the right side of the screen so it gets attached to
  // one of the panel docks.
  SendPanelDraggedMessage(panel, root_geometry.width - 10, 200);
  EXPECT_EQ(root_geometry.width, panel->right());
  EXPECT_EQ(200, panel->titlebar_y());

  // Move it left so it's attached to the other dock.
  SendPanelDraggedMessage(panel, 10, 300);
  EXPECT_EQ(panel->content_width(), panel->right());
  EXPECT_EQ(300, panel->titlebar_y());

  // Detach it again.
  SendPanelDraggedMessage(panel, 700, 300);
  EXPECT_EQ(700, panel->right());
  EXPECT_EQ(300, panel->titlebar_y());

  // Now finish the drag and check that the panel ends up back in the bar.
  SendPanelDragCompleteMessage(panel);
  EXPECT_EQ(wm_->width() - PanelBar::kPixelsBetweenPanels, panel->right());
  EXPECT_EQ(panel_y_in_bar, panel->titlebar_y());
}

// Check that panels retain the focus when they get dragged out of the
// panel bar and reattached to it, and also that we assign the focus to a
// new panel when one with the focus gets destroyed.
TEST_F(PanelManagerTest, DragFocusedPanel) {
  // Create a panel and check that it has the focus.
  Panel* old_panel = CreateSimplePanel(200, 20, 300);
  ASSERT_EQ(old_panel->content_xid(), xconn_->focused_xid());

  // Create a second panel, which should take the focus.
  Panel* panel = CreateSimplePanel(200, 20, 300);
  ASSERT_EQ(panel->content_xid(), xconn_->focused_xid());
  EXPECT_EQ(panel->content_xid(), GetActiveWindowProperty());

  // Drag the second panel out of the panel bar and check that it still has
  // the focus.
  SendPanelDraggedMessage(panel, 400, 50);
  ASSERT_TRUE(panel_manager_->GetContainerForPanel(*panel) == NULL);
  EXPECT_EQ(panel->content_xid(), xconn_->focused_xid());
  EXPECT_EQ(panel->content_xid(), GetActiveWindowProperty());

  // Now reattach it and check that it still has the focus.
  SendPanelDraggedMessage(panel, 400, wm_->height() - 1);
  ASSERT_EQ(panel_manager_->GetContainerForPanel(*panel), panel_bar_);
  EXPECT_EQ(panel->content_xid(), xconn_->focused_xid());
  EXPECT_EQ(panel->content_xid(), GetActiveWindowProperty());

  // Destroy the second panel.
  const XWindow content_xid = panel->content_xid();
  const XWindow titlebar_xid = panel->titlebar_xid();
  XEvent event;
  ASSERT_TRUE(xconn_->DestroyWindow(content_xid));
  xconn_->InitUnmapEvent(&event, content_xid);
  wm_->HandleEvent(&event);
  xconn_->InitDestroyWindowEvent(&event, content_xid);
  wm_->HandleEvent(&event);

  ASSERT_TRUE(xconn_->DestroyWindow(titlebar_xid));
  xconn_->InitUnmapEvent(&event, titlebar_xid);
  wm_->HandleEvent(&event);
  xconn_->InitDestroyWindowEvent(&event, titlebar_xid);
  wm_->HandleEvent(&event);

  // The first panel should be focused now.
  ASSERT_EQ(old_panel->content_xid(), xconn_->focused_xid());
  EXPECT_EQ(old_panel->content_xid(), GetActiveWindowProperty());
}

TEST_F(PanelManagerTest, ChromeInitiatedPanelResize) {
  // Create a panel with a 200x400 content window.
  Panel* panel = CreateSimplePanel(200, 20, 400);
  EXPECT_EQ(200, panel->width());
  EXPECT_EQ(20, panel->titlebar_height());
  EXPECT_EQ(400, panel->content_height());
  const int initial_right = panel->right();
  const int initial_titlebar_y = panel->titlebar_y();

  // We should ignore requests to resize the titlebar.
  XEvent event;
  xconn_->InitConfigureRequestEvent(
      &event, panel->titlebar_xid(), 0, 0, 300, 30);
  wm_->HandleEvent(&event);
  EXPECT_EQ(200, panel->width());
  EXPECT_EQ(20, panel->titlebar_height());
  EXPECT_EQ(400, panel->content_height());
  EXPECT_EQ(initial_right, panel->right());
  EXPECT_EQ(initial_titlebar_y, panel->titlebar_y());

  // A request to resize the content to 300x500 should be honored, though.
  xconn_->InitConfigureRequestEvent(
      &event, panel->content_xid(), 0, 0, 300, 500);
  wm_->HandleEvent(&event);
  EXPECT_EQ(300, panel->width());
  EXPECT_EQ(20, panel->titlebar_height());
  EXPECT_EQ(500, panel->content_height());
  // The panel should grow up and to the left.
  EXPECT_EQ(initial_right, panel->right());
  EXPECT_EQ(initial_titlebar_y - 100, panel->titlebar_y());

  // Test that shrinking the content works too.
  xconn_->InitConfigureRequestEvent(
      &event, panel->content_xid(), 0, 0, 100, 300);
  wm_->HandleEvent(&event);
  EXPECT_EQ(100, panel->width());
  EXPECT_EQ(20, panel->titlebar_height());
  EXPECT_EQ(300, panel->content_height());
  EXPECT_EQ(initial_right, panel->right());
  EXPECT_EQ(initial_titlebar_y + 100, panel->titlebar_y());

  // We should ignore requests if the user is already resizing the panel.
  XWindow input_xid = panel->top_left_input_xid_;
  xconn_->InitButtonPressEvent(&event, input_xid, 0, 0, 1);
  wm_->HandleEvent(&event);
  xconn_->InitMotionNotifyEvent(&event, input_xid, -200, -200);
  wm_->HandleEvent(&event);

  // We should have the same values as before.
  xconn_->InitConfigureRequestEvent(
      &event, panel->content_xid(), 0, 0, 200, 400);
  wm_->HandleEvent(&event);
  EXPECT_EQ(100, panel->width());
  EXPECT_EQ(20, panel->titlebar_height());
  EXPECT_EQ(300, panel->content_height());
  EXPECT_EQ(initial_right, panel->right());
  EXPECT_EQ(initial_titlebar_y + 100, panel->titlebar_y());

  // Finish the user-initiated resize and check that it's applied.
  xconn_->InitButtonReleaseEvent(&event, input_xid, -200, -200, 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(300, panel->width());
  EXPECT_EQ(20, panel->titlebar_height());
  EXPECT_EQ(500, panel->content_height());
  EXPECT_EQ(initial_right, panel->right());
  EXPECT_EQ(initial_titlebar_y - 100, panel->titlebar_y());
}

TEST_F(PanelManagerTest, Fullscreen) {
  const int titlebar_height = 20;
  const int content_width = 200;
  const int content_height = 400;

  // Create three panels.
  Panel* panel1 =
      CreateSimplePanel(content_width, titlebar_height, content_height);
  EXPECT_EQ(panel1->content_xid(), xconn_->focused_xid());

  Panel* panel2 =
      CreateSimplePanel(content_width, titlebar_height, content_height);
  EXPECT_EQ(panel2->content_xid(), xconn_->focused_xid());

  Panel* panel3 =
      CreateSimplePanel(content_width, titlebar_height, content_height);
  EXPECT_EQ(panel3->content_xid(), xconn_->focused_xid());

  // Check that they're positioned as expecded.
  const int rightmost_panel_right =
      wm_->width() - PanelBar::kPixelsBetweenPanels;
  const int middle_panel_right =
      rightmost_panel_right - content_width - PanelBar::kPixelsBetweenPanels;
  const int leftmost_panel_right =
      middle_panel_right - content_width - PanelBar::kPixelsBetweenPanels;
  EXPECT_EQ(rightmost_panel_right, panel1->right());
  EXPECT_EQ(middle_panel_right, panel2->right());
  EXPECT_EQ(leftmost_panel_right, panel3->right());
  EXPECT_TRUE(WindowIsInLayer(panel1->content_win(),
                              StackingManager::LAYER_STATIONARY_PANEL_IN_BAR));
  EXPECT_TRUE(WindowIsInLayer(panel2->content_win(),
                              StackingManager::LAYER_STATIONARY_PANEL_IN_BAR));
  EXPECT_TRUE(WindowIsInLayer(panel3->content_win(),
                              StackingManager::LAYER_STATIONARY_PANEL_IN_BAR));

  const XAtom wm_state_atom = wm_->GetXAtom(ATOM_NET_WM_STATE);
  const XAtom fullscreen_atom = wm_->GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN);

  // Ask the window manager to make the second (middle) panel fullscreen.
  XEvent fullscreen_event;
  xconn_->InitClientMessageEvent(
      &fullscreen_event,
      panel2->content_xid(),
      wm_state_atom,
      1,  // add
      fullscreen_atom, 0, 0, 0);
  wm_->HandleEvent(&fullscreen_event);
  NotifyWindowAboutSize(panel2->content_win());

  // Check that the second panel is focused automatically, covering the
  // whole screen, and stacked above the other panels.
  EXPECT_TRUE(panel2->is_fullscreen());
  EXPECT_EQ(panel2->content_xid(), xconn_->focused_xid());
  TestPanelContentBounds(panel2, 0, 0, wm_->width(), wm_->height());
  EXPECT_TRUE(WindowIsInLayer(panel2->content_win(),
                              StackingManager::LAYER_FULLSCREEN_WINDOW));
  TestIntArrayProperty(
      panel2->content_xid(), wm_state_atom, 1, fullscreen_atom);

  // Now send a message making the third (leftmost) panel fullscreen.  The
  // second panel should be made non-fullscreen.
  fullscreen_event.xclient.window = panel3->content_xid();
  wm_->HandleEvent(&fullscreen_event);
  NotifyWindowAboutSize(panel2->content_win());
  NotifyWindowAboutSize(panel3->content_win());

  EXPECT_TRUE(panel3->is_fullscreen());
  EXPECT_EQ(panel3->content_xid(), xconn_->focused_xid());
  TestPanelContentBounds(panel3, 0, 0, wm_->width(), wm_->height());
  EXPECT_TRUE(WindowIsInLayer(panel3->content_win(),
                              StackingManager::LAYER_FULLSCREEN_WINDOW));
  TestIntArrayProperty(
      panel3->content_xid(), wm_state_atom, 1, fullscreen_atom);

  EXPECT_FALSE(panel2->is_fullscreen());
  TestPanelContentBounds(panel2,
                         middle_panel_right - content_width,  // x
                         wm_->height() - content_height,      // y
                         content_width, content_height);
  EXPECT_TRUE(WindowIsInLayer(panel2->content_win(),
                              StackingManager::LAYER_STATIONARY_PANEL_IN_BAR));
  TestIntArrayProperty(panel2->content_xid(), wm_state_atom, 0);

  // Unmap the first (rightmost) panel.  The third panel's content window
  // should still be fullscreened, but its stored position should be
  // updated in response to the panel closure -- it should move to the
  // middle position.
  XEvent event;
  xconn_->InitUnmapEvent(&event, panel1->content_xid());
  wm_->HandleEvent(&event);
  EXPECT_TRUE(panel3->is_fullscreen());
  TestPanelContentBounds(panel3, 0, 0, wm_->width(), wm_->height());
  EXPECT_TRUE(WindowIsInLayer(panel3->content_win(),
                              StackingManager::LAYER_FULLSCREEN_WINDOW));
  EXPECT_EQ(rightmost_panel_right, panel2->right());
  EXPECT_EQ(middle_panel_right, panel3->right());

  // Now send a message asking to unfullscreen the third panel and check
  // that it gets restored to its regular middle position.  It should still
  // keep the focus.
  fullscreen_event.xclient.data.l[0] = 0;  // remove
  wm_->HandleEvent(&fullscreen_event);
  NotifyWindowAboutSize(panel3->content_win());
  EXPECT_FALSE(panel3->is_fullscreen());
  EXPECT_EQ(panel3->content_xid(), xconn_->focused_xid());
  TestPanelContentBounds(panel3,
                         middle_panel_right - content_width,  // x
                         wm_->height() - content_height,      // y
                         content_width, content_height);
  EXPECT_TRUE(WindowIsInLayer(panel3->content_win(),
                              StackingManager::LAYER_STATIONARY_PANEL_IN_BAR));
  TestIntArrayProperty(panel3->content_xid(), wm_state_atom, 0);

  // Fullscreen the second panel and then unmap one of its windows.  Check
  // that the panel manager's fullscreen panel pointer is cleared.
  fullscreen_event.xclient.window = panel2->content_xid();
  fullscreen_event.xclient.data.l[0] = 1;  // add
  wm_->HandleEvent(&fullscreen_event);
  EXPECT_TRUE(panel2->is_fullscreen());
  EXPECT_EQ(panel2->content_xid(), xconn_->focused_xid());

  xconn_->InitUnmapEvent(&event, panel2->content_xid());
  wm_->HandleEvent(&event);
  EXPECT_TRUE(panel_manager_->fullscreen_panel_ == NULL);
  EXPECT_EQ(panel3->content_xid(), xconn_->focused_xid());
}

// Test that panels in the dock take the focus when they get the chance.
// Otherwise, we can get in a state where the root window has the focus
// but it gets transferred to a docked panel when the pointer moves over
// it.  See http://crosbug.com/1619.
TEST_F(PanelManagerTest, FocusPanelInDock) {
  Panel* panel_in_bar = CreateSimplePanel(20, 200, 400);
  Panel* panel_in_dock = CreateSimplePanel(20, 200, 400);

  XConnection::WindowGeometry root_geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xconn_->GetRootWindow(),
                                        &root_geometry));

  // Drag the second panel to the dock and check that it sticks there.
  SendPanelDraggedMessage(panel_in_dock, root_geometry.width - 1, 0);
  SendPanelDragCompleteMessage(panel_in_dock);
  EXPECT_EQ(root_geometry.width, panel_in_dock->right());
  EXPECT_EQ(0, panel_in_dock->titlebar_y());

  // The docked panel should have the focus, since it was opened second.
  // Send a message asking the WM to focus the panel in the bar.
  EXPECT_EQ(panel_in_dock->content_xid(), xconn_->focused_xid());
  SendActiveWindowMessage(panel_in_bar->content_xid());
  EXPECT_EQ(panel_in_bar->content_xid(), xconn_->focused_xid());

  // Now unmap the panel in the bar and check that the docked panel gets
  // the focus.
  XEvent event;
  xconn_->InitUnmapEvent(&event, panel_in_bar->content_xid());
  wm_->HandleEvent(&event);
  EXPECT_EQ(panel_in_dock->content_xid(), xconn_->focused_xid());
}

// Test that panel docks are made visible when they contain panels and
// invisible when they don't, and that the layout manager gets resized as
// needed to make room for the docks.
TEST_F(PanelManagerTest, DockVisibilityAndResizing) {
  XWindow root_xid = xconn_->GetRootWindow();
  MockXConnection::WindowInfo* root_info = xconn_->GetWindowInfoOrDie(root_xid);

  Panel* panel1 = CreateSimplePanel(20, 200, 400);
  Panel* panel2 = CreateSimplePanel(20, 200, 400);

  // Both panel docks should initially be invisible.
  EXPECT_FALSE(left_panel_dock_->is_visible());
  EXPECT_FALSE(right_panel_dock_->is_visible());

  // The layout manager should initially fill the whole screen.
  EXPECT_EQ(0, layout_manager_->x());
  EXPECT_EQ(0, layout_manager_->y());
  EXPECT_EQ(root_info->width, layout_manager_->width());
  EXPECT_EQ(root_info->height, layout_manager_->height());

  // Drag the first panel to the left dock.
  SendPanelDraggedMessage(panel1, 0, 0);
  SendPanelDragCompleteMessage(panel1);

  // The left dock should become visible.
  EXPECT_TRUE(left_panel_dock_->is_visible());
  EXPECT_EQ(0, left_panel_dock_->x());
  EXPECT_EQ(0, left_panel_dock_->y());

  // The layout manager should move to the right and get narrower to make
  // room for the left dock.
  EXPECT_EQ(PanelManager::kPanelDockWidth, layout_manager_->x());
  EXPECT_EQ(0, layout_manager_->y());
  EXPECT_EQ(root_info->width - PanelManager::kPanelDockWidth,
            layout_manager_->width());
  EXPECT_EQ(root_info->height, layout_manager_->height());

  // Dock the second panel on the right.
  SendPanelDraggedMessage(panel2, root_info->width - 1, 0);
  SendPanelDragCompleteMessage(panel2);

  // The right dock should become visible.
  EXPECT_TRUE(right_panel_dock_->is_visible());
  EXPECT_EQ(root_info->width - PanelManager::kPanelDockWidth,
            right_panel_dock_->x());
  EXPECT_EQ(0, right_panel_dock_->y());

  // The layout manager should get narrower to make room for the right dock.
  EXPECT_EQ(PanelManager::kPanelDockWidth, layout_manager_->x());
  EXPECT_EQ(0, layout_manager_->y());
  EXPECT_EQ(root_info->width - 2 * PanelManager::kPanelDockWidth,
            layout_manager_->width());
  EXPECT_EQ(root_info->height, layout_manager_->height());

  // Make the screen a bit smaller and send a ConfigureNotify event about it.
  root_info->width -= 40;
  root_info->height -= 30;
  XEvent event;
  xconn_->InitConfigureNotifyEvent(&event, root_xid);
  wm_->HandleEvent(&event);

  // The left dock should still be in the same place.  The right one should
  // shift over as needed.
  EXPECT_EQ(0, left_panel_dock_->x());
  EXPECT_EQ(0, left_panel_dock_->y());
  EXPECT_EQ(root_info->width - PanelManager::kPanelDockWidth,
            right_panel_dock_->x());
  EXPECT_EQ(0, right_panel_dock_->y());

  // The layout manager should shrink accordingly (and it should still
  // leave room for the panel docks).
  EXPECT_EQ(PanelManager::kPanelDockWidth, layout_manager_->x());
  EXPECT_EQ(0, layout_manager_->y());
  EXPECT_EQ(root_info->width - 2 * PanelManager::kPanelDockWidth,
            layout_manager_->width());
  EXPECT_EQ(root_info->height, layout_manager_->height());

  // Undock the left panel and check that the dock becomes invisible.
  SendPanelDraggedMessage(
      panel1, root_info->width * 0.5, root_info->height - 1);
  SendPanelDragCompleteMessage(panel1);
  EXPECT_FALSE(left_panel_dock_->is_visible());

  // The layout manager should move back to the left edge of the screen and
  // get a bit wider, so that it's just leaving room for the right dock.
  EXPECT_EQ(0, layout_manager_->x());
  EXPECT_EQ(0, layout_manager_->y());
  EXPECT_EQ(root_info->width - PanelManager::kPanelDockWidth,
            layout_manager_->width());
  EXPECT_EQ(root_info->height, layout_manager_->height());
}

// Test that we support transient windows for panels.
TEST_F(PanelManagerTest, TransientWindows) {
  XConnection::WindowGeometry root_geometry;
  ASSERT_TRUE(
      xconn_->GetWindowGeometry(xconn_->GetRootWindow(), &root_geometry));

  Panel* panel = CreatePanel(20, 200, 400, true, true, 0);

  // Create a transient window owned by the panel.
  const int transient_x = 30, transient_y = 40;
  const int transient_width = 300, transient_height = 200;
  XWindow transient_xid = CreateBasicWindow(transient_x, transient_y,
                                            transient_width, transient_height);
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  // Say that we support the WM_DELETE_WINDOW protocol so that the window
  // manager will try to close us when needed.
  transient_info->int_properties[wm_->GetXAtom(ATOM_WM_PROTOCOLS)].push_back(
      wm_->GetXAtom(ATOM_WM_DELETE_WINDOW));
  transient_info->transient_for = panel->content_xid();
  SendInitialEventsForWindow(transient_xid);
  Window* transient_win = wm_->GetWindowOrDie(transient_xid);

  // We should try to center the transient window over the panel (at least
  // to the degree that we can while still keeping the transient onscreen).
  EXPECT_EQ(min(panel->content_x() +
                  (panel->content_width() - transient_width) / 2,
                root_geometry.width - transient_width),
            transient_info->x);
  EXPECT_EQ(min(panel->content_win()->client_y() +
                  (panel->content_height() - transient_height) / 2,
                root_geometry.height - transient_height),
            transient_info->y);
  EXPECT_EQ(transient_width, transient_info->width);
  EXPECT_EQ(transient_height, transient_info->height);

  // Check that the transient is stacked within the same layer as the
  // panel, and that it's stacked above the content window.
  EXPECT_TRUE(WindowIsInLayer(transient_win,
                              StackingManager::LAYER_STATIONARY_PANEL_IN_BAR));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(transient_xid),
            xconn_->stacked_xids().GetIndex(panel->content_xid()));
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  EXPECT_LT(stage->GetStackingIndex(transient_win->actor()),
            stage->GetStackingIndex(panel->content_win()->actor()));

  // If we move the panel, the window manager should try to close the
  // transient window.
  EXPECT_EQ(0, GetNumDeleteWindowMessagesForWindow(transient_xid));
  SendPanelDraggedMessage(panel, panel->right() - 2, panel->titlebar_y());
  EXPECT_GT(GetNumDeleteWindowMessagesForWindow(transient_xid), 0);
  SendPanelDragCompleteMessage(panel);

  // Ditto if the panel is collapsed.
  int initial_num_delete_messages =
      GetNumDeleteWindowMessagesForWindow(transient_xid);
  SendSetPanelStateMessage(panel, false);  // expanded=false
  EXPECT_GT(GetNumDeleteWindowMessagesForWindow(transient_xid),
            initial_num_delete_messages);

  // Unmap the transient window.
  xconn_->UnmapWindow(transient_xid);
  XEvent event;
  xconn_->InitUnmapEvent(&event, transient_xid);
  wm_->HandleEvent(&event);

  // Create another transient with the CHROME_INFO_BUBBLE type, which
  // should allow us to place it wherever we want.
  const int infobubble_x = 40, infobubble_y = 50;
  const int infobubble_width = 200, infobubble_height = 20;
  XWindow infobubble_xid = CreateBasicWindow(
      infobubble_x, infobubble_y, infobubble_width, infobubble_height);
  MockXConnection::WindowInfo* infobubble_info =
      xconn_->GetWindowInfoOrDie(infobubble_xid);
  wm_->wm_ipc()->SetWindowType(
      infobubble_xid, chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE, NULL);
  infobubble_info->transient_for = panel->content_xid();
  SendInitialEventsForWindow(infobubble_xid);

  EXPECT_EQ(infobubble_x, infobubble_info->x);
  EXPECT_EQ(infobubble_y, infobubble_info->y);
  EXPECT_EQ(infobubble_width, infobubble_info->width);
  EXPECT_EQ(infobubble_height, infobubble_info->height);

  // Check that we'll honor a request to make the infobubble modal.
  xconn_->InitClientMessageEvent(
      &event, infobubble_xid, wm_->GetXAtom(ATOM_NET_WM_STATE),
      1, wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL), None, None, None);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(wm_->GetWindowOrDie(infobubble_xid)->wm_state_modal());

  // Now create a toplevel window and check that it gets focused, and then
  // send a _NET_ACTIVE_WINDOW message asking the WM to focus the infobubble.
  XWindow toplevel_xid = CreateToplevelWindow(1, 0, 0, 0, 1024, 768);
  SendInitialEventsForWindow(toplevel_xid);
  ASSERT_EQ(toplevel_xid, xconn_->focused_xid());
  xconn_->InitClientMessageEvent(
      &event, infobubble_xid, wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW),
      1, wm_->GetCurrentTimeFromServer() + 1, 0, None, None);
  wm_->HandleEvent(&event);
  EXPECT_EQ(infobubble_xid, xconn_->focused_xid());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
