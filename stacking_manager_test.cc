// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "window_manager/event_loop.h"
#include "window_manager/shadow.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x11/mock_x_connection.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

namespace window_manager {

class StackingManagerTest : public BasicWindowManagerTest {
 protected:
  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();
    stacking_manager_ = wm_->stacking_manager();
  }

  StackingManager* stacking_manager_;  // points at wm_'s object
};

TEST_F(StackingManagerTest, StackXidAtTopOfLayer) {
  // Create two windows.
  XWindow xid = CreateSimpleWindow();
  XWindow xid2 = CreateSimpleWindow();

  // Tell the stacking manager to stack them in different layers and then
  // make sure that they were restacked correctly.
  stacking_manager_->StackXidAtTopOfLayer(
      xid, StackingManager::LAYER_TOPLEVEL_WINDOW);
  stacking_manager_->StackXidAtTopOfLayer(
      xid2, StackingManager::LAYER_PACKED_PANEL_IN_BAR);
  EXPECT_LT(xconn_->stacked_xids().GetIndex(xid2),
            xconn_->stacked_xids().GetIndex(xid));

  // Now move the lower window to the top of the other window's layer.
  stacking_manager_->StackXidAtTopOfLayer(
      xid, StackingManager::LAYER_PACKED_PANEL_IN_BAR);
  EXPECT_LT(xconn_->stacked_xids().GetIndex(xid),
            xconn_->stacked_xids().GetIndex(xid2));
}

TEST_F(StackingManagerTest, StackActorAtTopOfLayer) {
  // Create two actors and add them to the stage.
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  scoped_ptr<MockCompositor::Actor> actor(compositor_->CreateGroup());
  stage->AddActor(actor.get());
  scoped_ptr<MockCompositor::Actor> actor2(compositor_->CreateGroup());
  stage->AddActor(actor2.get());

  // Check that the actors get stacked correctly.
  stacking_manager_->StackActorAtTopOfLayer(
      actor.get(), StackingManager::LAYER_BACKGROUND);
  stacking_manager_->StackActorAtTopOfLayer(
      actor2.get(), StackingManager::LAYER_TOPLEVEL_WINDOW);
  EXPECT_LT(stage->GetStackingIndex(actor2.get()),
            stage->GetStackingIndex(actor.get()));

  // Now restack them.
  stacking_manager_->StackActorAtTopOfLayer(
      actor.get(), StackingManager::LAYER_TOPLEVEL_WINDOW);
  EXPECT_LT(stage->GetStackingIndex(actor.get()),
            stage->GetStackingIndex(actor2.get()));
}

TEST_F(StackingManagerTest, StackWindowAtTopOfLayer) {
  // Create two windows.
  XWindow xid = CreateSimpleWindow();
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  win.SetShadowType(Shadow::TYPE_RECTANGULAR);

  XWindow xid2 = CreateSimpleWindow();
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid2, &geometry));
  Window win2(wm_.get(), xid2, false, geometry);
  win2.SetShadowType(Shadow::TYPE_RECTANGULAR);

  // Stack both of the windows in the same layer and make sure that their
  // relative positions are correct.
  stacking_manager_->StackWindowAtTopOfLayer(
      &win,
      StackingManager::LAYER_TOPLEVEL_WINDOW,
      StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
  stacking_manager_->StackWindowAtTopOfLayer(
      &win2,
      StackingManager::LAYER_TOPLEVEL_WINDOW,
      StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
  EXPECT_LT(xconn_->stacked_xids().GetIndex(xid2),
            xconn_->stacked_xids().GetIndex(xid));

  // Their actors should've been restacked as well, and the shadows should
  // be stacked at the bottom of the layer, beneath both windows.
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  EXPECT_LT(stage->GetStackingIndex(win2.actor()),
            stage->GetStackingIndex(win.actor()));
  EXPECT_LT(stage->GetStackingIndex(win.actor()),
            stage->GetStackingIndex(win2.shadow()->group()));
  EXPECT_LT(stage->GetStackingIndex(win2.actor()),
            stage->GetStackingIndex(win.shadow()->group()));

  // Now stack the first window on a higher layer.  Their client windows
  // should be restacked as expected, and the first window's shadow should
  // be stacked above the second window.
  stacking_manager_->StackWindowAtTopOfLayer(
      &win,
      StackingManager::LAYER_PACKED_PANEL_IN_BAR,
      StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
  EXPECT_LT(xconn_->stacked_xids().GetIndex(xid),
            xconn_->stacked_xids().GetIndex(xid2));
  EXPECT_LT(stage->GetStackingIndex(win.actor()),
            stage->GetStackingIndex(win2.actor()));
  EXPECT_LT(stage->GetStackingIndex(win.actor()),
            stage->GetStackingIndex(win.shadow()->group()));
  EXPECT_LT(stage->GetStackingIndex(win.shadow()->group()),
            stage->GetStackingIndex(win2.actor()));
  EXPECT_LT(stage->GetStackingIndex(win2.actor()),
            stage->GetStackingIndex(win2.shadow()->group()));

  // Stack the second window on the higher layer as well, but with its shadow
  // stacked directly below it.  Check that the second window's shadow is above
  // the first window now.
  stacking_manager_->StackWindowAtTopOfLayer(
      &win2,
      StackingManager::LAYER_PACKED_PANEL_IN_BAR,
      StackingManager::SHADOW_DIRECTLY_BELOW_ACTOR);
  EXPECT_LT(xconn_->stacked_xids().GetIndex(xid2),
            xconn_->stacked_xids().GetIndex(xid));
  EXPECT_LT(stage->GetStackingIndex(win2.actor()),
            stage->GetStackingIndex(win2.shadow()->group()));
  EXPECT_LT(stage->GetStackingIndex(win2.shadow()->group()),
            stage->GetStackingIndex(win.actor()));
  EXPECT_LT(stage->GetStackingIndex(win.actor()),
            stage->GetStackingIndex(win.shadow()->group()));
}

TEST_F(StackingManagerTest, StackWindowRelativeToOtherWindow) {
  // Create two windows.
  XWindow xid = CreateSimpleWindow();
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  win.SetShadowType(Shadow::TYPE_RECTANGULAR);

  XWindow xid2 = CreateSimpleWindow();
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid2, &geometry));
  Window win2(wm_.get(), xid2, false, geometry);
  win2.SetShadowType(Shadow::TYPE_RECTANGULAR);

  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  stacking_manager_->StackWindowAtTopOfLayer(
      &win,
      StackingManager::LAYER_TOPLEVEL_WINDOW,
      StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);

  // Stack the second window above the first with its shadow at the bottom of
  // the layer.
  stacking_manager_->StackWindowRelativeToOtherWindow(
      &win2,
      &win,
      StackingManager::ABOVE_SIBLING,
      StackingManager::SHADOW_AT_BOTTOM_OF_LAYER,
      StackingManager::LAYER_TOPLEVEL_WINDOW);
  EXPECT_LT(stage->GetStackingIndex(win2.actor()),
            stage->GetStackingIndex(win.actor()));
  EXPECT_LT(stage->GetStackingIndex(win.actor()),
            stage->GetStackingIndex(win.shadow()->group()));
  EXPECT_LT(stage->GetStackingIndex(win.actor()),
            stage->GetStackingIndex(win2.shadow()->group()));

  // Stack the second window below the first with its shadow at the bottom of
  // the layer.
  stacking_manager_->StackWindowRelativeToOtherWindow(
      &win2,
      &win,
      StackingManager::BELOW_SIBLING,
      StackingManager::SHADOW_AT_BOTTOM_OF_LAYER,
      StackingManager::LAYER_TOPLEVEL_WINDOW);
  EXPECT_LT(stage->GetStackingIndex(win.actor()),
            stage->GetStackingIndex(win2.actor()));
  EXPECT_LT(stage->GetStackingIndex(win2.actor()),
            stage->GetStackingIndex(win.shadow()->group()));
  EXPECT_LT(stage->GetStackingIndex(win2.actor()),
            stage->GetStackingIndex(win2.shadow()->group()));

  // Stack the second window above the first with its shadow directly under it.
  stacking_manager_->StackWindowRelativeToOtherWindow(
      &win2,
      &win,
      StackingManager::ABOVE_SIBLING,
      StackingManager::SHADOW_DIRECTLY_BELOW_ACTOR,
      StackingManager::LAYER_TOPLEVEL_WINDOW);
  EXPECT_LT(stage->GetStackingIndex(win2.actor()),
            stage->GetStackingIndex(win2.shadow()->group()));
  EXPECT_LT(stage->GetStackingIndex(win2.shadow()->group()),
            stage->GetStackingIndex(win.actor()));
  EXPECT_LT(stage->GetStackingIndex(win.actor()),
            stage->GetStackingIndex(win.shadow()->group()));
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
