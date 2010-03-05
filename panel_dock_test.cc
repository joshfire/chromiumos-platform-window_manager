// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "window_manager/clutter_interface.h"
#include "window_manager/event_loop.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/panel.h"
#include "window_manager/panel_dock.h"
#include "window_manager/panel_manager.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window_manager.h"

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
  Panel* panel = CreatePanel(200, 20, 400, true);

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
  Panel* panel1 = CreatePanel(initial_width, 20, 300, true);
  Panel* panel2 = CreatePanel(initial_width, 20, 200, true);

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

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
