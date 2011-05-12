// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/panels/panel.h"
#include "window_manager/panels/panel_dock.h"
#include "window_manager/panels/panel_manager.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window_manager.h"
#include "window_manager/x11/mock_x_connection.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

namespace window_manager {

class PanelDockTest : public BasicWindowManagerTest {
 protected:
  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();
    left_dock_ = wm_->panel_manager_->left_panel_dock_.get();
    right_dock_ = wm_->panel_manager_->right_panel_dock_.get();
  }

  PanelDock* left_dock_;   // instance belonging to wm_->panel_manager_
  PanelDock* right_dock_;  // instance belonging to wm_->panel_manager_
};

// Test that panels can be attached and detached from docks.
TEST_F(PanelDockTest, AttachAndDetach) {
  Panel* panel = CreatePanel(200, 20, 400);

  // Drag the panel up first, to get it out of the panel bar.
  SendPanelDraggedMessage(panel, 500, 100);
  EXPECT_EQ(500, panel->right());
  EXPECT_EQ(100, panel->titlebar_y());

  // Now drag the panel to the left, within the threshold for attaching it
  // to the left dock.  It should snap to the edge but not get resized yet.
  int drag_right = 200 + PanelDock::kAttachThresholdPixels - 10;
  SendPanelDraggedMessage(panel, drag_right, 100);
  EXPECT_EQ(200, panel->right());
  EXPECT_EQ(100, panel->titlebar_y());
  EXPECT_EQ(200, panel->width());

  // After the drag finishes, the panel should be resized to match the
  // dock's width, and it should slide up to the top of the dock.
  SendPanelDragCompleteMessage(panel);
  EXPECT_EQ(PanelManager::kPanelDockWidth, panel->right());
  EXPECT_EQ(0, panel->titlebar_y());
  EXPECT_EQ(PanelManager::kPanelDockWidth, panel->width());

  // Drag the panel into the right dock.
  SendPanelDraggedMessage(
      panel, wm_->width() - PanelDock::kAttachThresholdPixels + 10, 200);
  EXPECT_EQ(wm_->width(), panel->right());
  EXPECT_EQ(200, panel->titlebar_y());
  EXPECT_EQ(PanelManager::kPanelDockWidth, panel->width());

  SendPanelDragCompleteMessage(panel);
  EXPECT_EQ(wm_->width(), panel->right());
  EXPECT_EQ(0, panel->titlebar_y());
  EXPECT_EQ(PanelManager::kPanelDockWidth, panel->width());

  // Test that panel drags within the dock get capped at the top and bottom
  // of the screen.
  SendPanelDraggedMessage(panel, wm_->width(), -10);
  EXPECT_EQ(wm_->width(), panel->right());
  EXPECT_EQ(0, panel->titlebar_y());
  SendPanelDraggedMessage(panel, wm_->width(), wm_->height() + 10);
  EXPECT_EQ(wm_->width(), panel->right());
  EXPECT_EQ(wm_->height() - panel->total_height(), panel->titlebar_y());

  // The panel should get packed back to the top of the dock when the drag
  // ends.
  SendPanelDragCompleteMessage(panel);
  EXPECT_EQ(0, panel->titlebar_y());
  EXPECT_EQ(wm_->width(), panel->right());
}

// Test that we reorder panels correctly while they're being dragged within
// a dock.
TEST_F(PanelDockTest, ReorderPanels) {
  const int initial_width = 200;
  Panel* panel1 = CreatePanel(initial_width, 20, 300);
  Panel* panel2 = CreatePanel(initial_width, 20, 200);

  // Drag the first panel into the left dock.
  int drag_right = initial_width + PanelDock::kAttachThresholdPixels - 10;
  SendPanelDraggedMessage(panel1, drag_right, 50);
  SendPanelDragCompleteMessage(panel1);
  EXPECT_EQ(0, panel1->titlebar_y());

  // Now drag the second panel to the top of the left dock and check that
  // it displaces the first panel.
  SendPanelDraggedMessage(panel2, drag_right, 10);
  EXPECT_EQ(panel2->total_height(), panel1->titlebar_y());
  EXPECT_EQ(10, panel2->titlebar_y());

  // Drag the second panel down, but not far enough to displace the first
  // panel.
  int drag_y = 0.5 * panel1->total_height();
  SendPanelDraggedMessage(panel2, drag_right, drag_y);
  EXPECT_EQ(panel2->total_height(), panel1->titlebar_y());
  EXPECT_EQ(drag_y, panel2->titlebar_y());

  // After we drag the second panel so its bottom edge hits the halfway
  // point on the first panel, the first panel should move back to the top
  // position.
  drag_y++;
  SendPanelDraggedMessage(panel2, drag_right, drag_y);
  EXPECT_EQ(0, panel1->titlebar_y());
  EXPECT_EQ(drag_y, panel2->titlebar_y());

  // Dragging one pixel to the right shouldn't do anything.
  SendPanelDraggedMessage(panel2, drag_right, drag_y);
  EXPECT_EQ(0, panel1->titlebar_y());
  EXPECT_EQ(drag_y, panel2->titlebar_y());

  // After we drag one pixel back up, the first panel should move back to
  // the bottom position.
  drag_y--;
  SendPanelDraggedMessage(panel2, drag_right, drag_y);
  EXPECT_EQ(panel2->total_height(), panel1->titlebar_y());
  EXPECT_EQ(drag_y, panel2->titlebar_y());

  // Drag the second panel out of the dock and check that the first panel
  // snaps back to the top position.
  SendPanelDraggedMessage(panel2, 500, 200);
  EXPECT_EQ(0, panel1->titlebar_y());
  EXPECT_EQ(500, panel2->right());
  EXPECT_EQ(200, panel2->titlebar_y());

  // Now attach the second panel into the dock's bottom position.
  SendPanelDraggedMessage(panel2, drag_right, 400);
  EXPECT_EQ(0, panel1->titlebar_y());
  EXPECT_EQ(400, panel2->titlebar_y());
  SendPanelDragCompleteMessage(panel2);
  EXPECT_EQ(0, panel1->titlebar_y());
  EXPECT_EQ(panel1->total_height(), panel2->titlebar_y());
}

// Test that resize requests for docked panels are handled correctly.
// Specifically, check that we ignore requests to change panels' widths
// while they're docked and that we repack all of the docked panels after a
// height change.
TEST_F(PanelDockTest, HandleResizeRequests) {
  const int initial_width = 300;
  const int initial_height = 400;
  const int initial_title_height = 20;
  Panel* panel1 = CreatePanel(
      initial_width, initial_title_height, initial_height);
  Panel* panel2 = CreatePanel(
      initial_width, initial_title_height, initial_height);

  // Drag both panels into the dock.
  int drag_right = wm_->width();
  SendPanelDraggedMessage(panel1, drag_right, 0);
  SendPanelDragCompleteMessage(panel1);
  SendPanelDraggedMessage(panel2, drag_right, 0);
  SendPanelDraggedMessage(panel2, drag_right, 400);
  SendPanelDragCompleteMessage(panel2);

  EXPECT_EQ(0, panel1->titlebar_y());
  EXPECT_EQ(wm_->width(), panel1->right());
  EXPECT_EQ(right_dock_->width(), panel1->width());
  EXPECT_EQ(initial_height, panel1->content_height());

  EXPECT_EQ(initial_title_height + initial_height, panel2->titlebar_y());
  EXPECT_EQ(wm_->width(), panel2->right());
  EXPECT_EQ(right_dock_->width(), panel2->width());
  EXPECT_EQ(initial_height, panel2->content_height());

  // Now request a size change for the first panel.
  const int new_height = 250;
  XEvent event;
  xconn_->InitConfigureRequestEvent(
      &event, panel1->content_xid(), Rect(0, 0, initial_width, new_height));
  wm_->HandleEvent(&event);

  EXPECT_EQ(0, panel1->titlebar_y());
  EXPECT_EQ(wm_->width(), panel1->right());
  EXPECT_EQ(right_dock_->width(), panel1->width());
  EXPECT_EQ(new_height, panel1->content_height());

  EXPECT_EQ(initial_title_height + new_height, panel2->titlebar_y());
  EXPECT_EQ(wm_->width(), panel2->right());
  EXPECT_EQ(right_dock_->width(), panel2->width());
  EXPECT_EQ(initial_height, panel2->content_height());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
