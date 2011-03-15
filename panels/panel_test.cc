// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/scoped_ptr.h"
#include "base/logging.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/geometry.h"
#include "window_manager/panels/panel.h"
#include "window_manager/panels/panel_manager.h"
#include "window_manager/shadow.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x11/mock_x_connection.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::vector;

namespace window_manager {

class PanelTest : public BasicWindowManagerTest {
 protected:
  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();
    panel_manager_ = wm_->panel_manager_.get();
  }

  PanelManager* panel_manager_;  // instance belonging to |wm_|
};

TEST_F(PanelTest, InputWindows) {
  XWindow titlebar_xid = CreatePanelTitlebarWindow(200, 20);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(titlebar_xid, &geometry));
  Window titlebar_win(wm_.get(), titlebar_xid, false, geometry);
  MockXConnection::WindowInfo* titlebar_info =
      xconn_->GetWindowInfoOrDie(titlebar_xid);

  XWindow content_xid = CreatePanelContentWindow(200, 400, titlebar_xid);
  ASSERT_TRUE(xconn_->GetWindowGeometry(content_xid, &geometry));
  Window content_win(wm_.get(), content_xid, false, geometry);
  MockXConnection::WindowInfo* content_info =
      xconn_->GetWindowInfoOrDie(content_xid);

  // Create a panel.
  Panel panel(panel_manager_, &content_win, &titlebar_win, true);
  panel.SetResizable(true);
  panel.Move(0, 0, 0);

  // Restack the panel and check that its titlebar is stacked above the
  // content window, and that the content window is above all of the input
  // windows used for resizing.
  panel.StackAtTopOfLayer(StackingManager::LAYER_PACKED_PANEL_IN_BAR);
  EXPECT_LT(xconn_->stacked_xids().GetIndex(titlebar_xid),
            xconn_->stacked_xids().GetIndex(content_xid));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(content_xid),
            xconn_->stacked_xids().GetIndex(panel.top_input_xid_));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(content_xid),
            xconn_->stacked_xids().GetIndex(panel.top_left_input_xid_));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(content_xid),
            xconn_->stacked_xids().GetIndex(panel.top_right_input_xid_));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(content_xid),
            xconn_->stacked_xids().GetIndex(panel.left_input_xid_));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(content_xid),
            xconn_->stacked_xids().GetIndex(panel.right_input_xid_));

  // Now move the panel to a new location and check that all of the input
  // windows are moved correctly around it.
  panel.MoveX(wm_->width() - 35, 0);

  MockXConnection::WindowInfo* top_info =
      xconn_->GetWindowInfoOrDie(panel.top_input_xid_);
  EXPECT_EQ(content_info->bounds.x - Panel::kResizeBorderWidth +
              Panel::kResizeCornerSize,
            top_info->bounds.x);
  EXPECT_EQ(titlebar_info->bounds.y - Panel::kResizeBorderWidth,
            top_info->bounds.y);
  EXPECT_EQ(titlebar_info->bounds.width + 2 * Panel::kResizeBorderWidth -
              2 * Panel::kResizeCornerSize,
            top_info->bounds.width);
  EXPECT_EQ(Panel::kResizeBorderWidth, top_info->bounds.height);

  MockXConnection::WindowInfo* top_left_info =
      xconn_->GetWindowInfoOrDie(panel.top_left_input_xid_);
  EXPECT_EQ(titlebar_info->bounds.x - Panel::kResizeBorderWidth,
            top_left_info->bounds.x);
  EXPECT_EQ(titlebar_info->bounds.y - Panel::kResizeBorderWidth,
            top_left_info->bounds.y);
  EXPECT_EQ(Panel::kResizeCornerSize, top_left_info->bounds.width);
  EXPECT_EQ(Panel::kResizeCornerSize, top_left_info->bounds.height);

  MockXConnection::WindowInfo* top_right_info =
      xconn_->GetWindowInfoOrDie(panel.top_right_input_xid_);
  EXPECT_EQ(titlebar_info->bounds.x + titlebar_info->bounds.width +
              Panel::kResizeBorderWidth - Panel::kResizeCornerSize,
            top_right_info->bounds.x);
  EXPECT_EQ(titlebar_info->bounds.y - Panel::kResizeBorderWidth,
            top_right_info->bounds.y);
  EXPECT_EQ(Panel::kResizeCornerSize, top_right_info->bounds.width);
  EXPECT_EQ(Panel::kResizeCornerSize, top_right_info->bounds.height);

  MockXConnection::WindowInfo* left_info =
      xconn_->GetWindowInfoOrDie(panel.left_input_xid_);
  EXPECT_EQ(content_info->bounds.x - Panel::kResizeBorderWidth,
            left_info->bounds.x);
  EXPECT_EQ(titlebar_info->bounds.y - Panel::kResizeBorderWidth +
              Panel::kResizeCornerSize,
            left_info->bounds.y);
  EXPECT_EQ(Panel::kResizeBorderWidth, left_info->bounds.width);
  EXPECT_EQ(content_info->bounds.height + titlebar_info->bounds.height +
              Panel::kResizeBorderWidth - Panel::kResizeCornerSize,
            left_info->bounds.height);

  MockXConnection::WindowInfo* right_info =
      xconn_->GetWindowInfoOrDie(panel.right_input_xid_);
  EXPECT_EQ(content_info->bounds.x + content_info->bounds.width,
            right_info->bounds.x);
  EXPECT_EQ(titlebar_info->bounds.y - Panel::kResizeBorderWidth +
              Panel::kResizeCornerSize,
            right_info->bounds.y);
  EXPECT_EQ(Panel::kResizeBorderWidth, right_info->bounds.width);
  EXPECT_EQ(content_info->bounds.height + titlebar_info->bounds.height +
              Panel::kResizeBorderWidth - Panel::kResizeCornerSize,
            right_info->bounds.height);

  // Input windows need to get restacked even when the panel isn't
  // resizable (so they'll be stacked correctly if it becomes resizable
  // later).
  panel.SetResizable(false);
  panel.StackAtTopOfLayer(StackingManager::LAYER_DRAGGED_PANEL);
  EXPECT_LT(xconn_->stacked_xids().GetIndex(titlebar_xid),
            xconn_->stacked_xids().GetIndex(content_xid));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(content_xid),
            xconn_->stacked_xids().GetIndex(panel.top_input_xid_));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(content_xid),
            xconn_->stacked_xids().GetIndex(panel.top_left_input_xid_));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(content_xid),
            xconn_->stacked_xids().GetIndex(panel.top_right_input_xid_));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(content_xid),
            xconn_->stacked_xids().GetIndex(panel.left_input_xid_));
  EXPECT_LT(xconn_->stacked_xids().GetIndex(content_xid),
            xconn_->stacked_xids().GetIndex(panel.right_input_xid_));
}

TEST_F(PanelTest, Resize) {
  int orig_width = 200;
  int orig_titlebar_height = 20;
  XWindow titlebar_xid =
      CreatePanelTitlebarWindow(orig_width, orig_titlebar_height);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(titlebar_xid, &geometry));
  Window titlebar_win(wm_.get(), titlebar_xid, false, geometry);
  MockXConnection::WindowInfo* titlebar_info =
      xconn_->GetWindowInfoOrDie(titlebar_xid);

  int orig_content_height = 400;
  XWindow content_xid = CreatePanelContentWindow(
      orig_width, orig_content_height, titlebar_xid);
  ASSERT_TRUE(xconn_->GetWindowGeometry(content_xid, &geometry));
  Window content_win(wm_.get(), content_xid, false, geometry);
  MockXConnection::WindowInfo* content_info =
      xconn_->GetWindowInfoOrDie(content_xid);

  // Create a panel.
  Panel panel(panel_manager_, &content_win, &titlebar_win, true);
  panel.SetResizable(true);
  panel.Move(0, 0, 0);

  // Check that one of the panel's resize handles has an asynchronous grab
  // installed on the first mouse button.
  MockXConnection::WindowInfo* handle_info =
      xconn_->GetWindowInfoOrDie(panel.top_left_input_xid_);
  EXPECT_TRUE(handle_info->button_is_grabbed(1));
  EXPECT_EQ(ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
            handle_info->button_grabs[1].event_mask);
  EXPECT_FALSE(handle_info->button_grabs[1].synchronous);

  // Pretend like the top left handle was clicked and a pointer grab was
  // automatically installed.
  xconn_->set_pointer_grab_xid(panel.top_left_input_xid_);
  panel.HandleInputWindowButtonPress(
      panel.top_left_input_xid_,
      0, 0,  // relative x, y
      1,     // button
      CurrentTime);

  // Pretend like the second button is pressed and the first button is
  // released.  We should explicitly ungrab the pointer when we see the
  // first button get released; X will only automatically remove the
  // pointer grab when *all* buttons are released.
  panel.HandleInputWindowButtonPress(
      panel.top_left_input_xid_, 0, 0, 2, CurrentTime);
  panel.HandleInputWindowButtonRelease(
      panel.top_left_input_xid_, 0, 0, 1, CurrentTime);
  EXPECT_EQ(0, xconn_->pointer_grab_xid());

  // Release the second button too, not that it really matters to us.
  panel.HandleInputWindowButtonRelease(
      panel.top_left_input_xid_, 0, 0, 2, CurrentTime);

  // Check that the panel's dimensions are unchanged.
  EXPECT_EQ(orig_width, titlebar_info->bounds.width);
  EXPECT_EQ(orig_titlebar_height, titlebar_info->bounds.height);
  EXPECT_EQ(orig_width, content_info->bounds.width);
  EXPECT_EQ(orig_content_height, content_info->bounds.height);

  int initial_x = titlebar_info->bounds.x;
  EXPECT_EQ(initial_x, content_info->bounds.x);
  int initial_titlebar_y = titlebar_info->bounds.y;
  EXPECT_EQ(initial_titlebar_y + titlebar_info->bounds.height,
            content_info->bounds.y);

  // Now start a second resize using the upper-left handle.  Drag a few
  // pixels up and to the left and then let go of the button.
  xconn_->set_pointer_grab_xid(panel.top_left_input_xid_);
  panel.HandleInputWindowButtonPress(
      panel.top_left_input_xid_, 0, 0, 1, CurrentTime);
  EXPECT_EQ(panel.top_left_input_xid_, xconn_->pointer_grab_xid());
  panel.HandleInputWindowPointerMotion(panel.top_left_input_xid_, -2, -4);
  xconn_->set_pointer_grab_xid(None);
  panel.HandleInputWindowButtonRelease(
      panel.top_left_input_xid_, -5, -6, 1, CurrentTime);

  // The titlebar should be offset by the drag and made a bit wider.
  EXPECT_EQ(initial_x - 5, titlebar_info->bounds.x);
  EXPECT_EQ(initial_titlebar_y - 6, titlebar_info->bounds.y);
  EXPECT_EQ(orig_width + 5, titlebar_info->bounds.width);
  EXPECT_EQ(orig_titlebar_height, titlebar_info->bounds.height);

  // The panel should move along with its titlebar, and it should get wider
  // and taller by the amount of the drag.
  EXPECT_EQ(initial_x - 5, content_info->bounds.x);
  EXPECT_EQ(titlebar_info->bounds.y + titlebar_info->bounds.height,
            content_info->bounds.y);
  EXPECT_EQ(orig_width + 5, content_info->bounds.width);
  EXPECT_EQ(orig_content_height + 6, content_info->bounds.height);
}

// Test that the _CHROME_STATE property is updated correctly to reflect the
// panel's expanded/collapsed state.
TEST_F(PanelTest, ChromeState) {
  XAtom state_atom = xconn_->GetAtomOrDie("_CHROME_STATE");
  XAtom collapsed_atom = xconn_->GetAtomOrDie("_CHROME_STATE_COLLAPSED_PANEL");

  // Create a collapsed panel.
  XWindow titlebar_xid = CreatePanelTitlebarWindow(200, 20);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(titlebar_xid, &geometry));
  Window titlebar_win(wm_.get(), titlebar_xid, false, geometry);
  new_panels_should_be_expanded_ = false;
  new_panels_should_take_focus_ = false;
  XWindow content_xid = CreatePanelContentWindow(200, 400, titlebar_xid);
  MockXConnection::WindowInfo* content_info =
      xconn_->GetWindowInfoOrDie(content_xid);
  ASSERT_TRUE(xconn_->GetWindowGeometry(content_xid, &geometry));
  Window content_win(wm_.get(), content_xid, false, geometry);
  Panel panel(panel_manager_, &content_win, &titlebar_win, false);
  panel.Move(0, 0, 0);

  // The panel's content window should have have a collapsed state in
  // _CHROME_STATE initially (since we told it to start collapsed).
  EXPECT_FALSE(panel.is_expanded());
  vector<int> values;
  ASSERT_TRUE(xconn_->GetIntArrayProperty(content_xid, state_atom, &values));
  ASSERT_EQ(1, values.size());
  EXPECT_EQ(collapsed_atom, values[0]);

  // We should also send a message to the panel telling it about the
  // initial state.
  EXPECT_EQ(static_cast<size_t>(1), content_info->client_messages.size());
  WmIpc::Message msg;
  ASSERT_TRUE(DecodeWmIpcMessage(content_info->client_messages[0], &msg));
  EXPECT_EQ(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_PANEL_STATE, msg.type());
  EXPECT_EQ(content_xid, msg.xid());
  EXPECT_EQ(0, msg.param(0));
  content_info->client_messages.clear();

  // After we tell the panel to notify Chrome that it's been expanded, it
  // should remove the collapsed atom (and additionally, the entire
  // property).
  panel.SetExpandedState(true);
  EXPECT_TRUE(panel.is_expanded());
  EXPECT_FALSE(xconn_->GetIntArrayProperty(content_xid, state_atom, &values));

  // We should send another message saying that it's expanded now.
  EXPECT_EQ(static_cast<size_t>(1), content_info->client_messages.size());
  ASSERT_TRUE(DecodeWmIpcMessage(content_info->client_messages[0], &msg));
  EXPECT_EQ(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_PANEL_STATE, msg.type());
  EXPECT_EQ(content_xid, msg.xid());
  EXPECT_EQ(1, msg.param(0));

  // Now tell it to notify Chrome that it's been collapsed again.
  panel.SetExpandedState(false);
  values.clear();
  ASSERT_TRUE(xconn_->GetIntArrayProperty(content_xid, state_atom, &values));
  ASSERT_EQ(1, values.size());
  EXPECT_EQ(collapsed_atom, values[0]);
}

// Test that we're able to hide panels' shadows.
TEST_F(PanelTest, Shadows) {
  // Create a collapsed panel.
  XWindow titlebar_xid = CreatePanelTitlebarWindow(200, 20);
  ASSERT_TRUE(xconn_->MapWindow(titlebar_xid));
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(titlebar_xid, &geometry));
  Window titlebar_win(wm_.get(), titlebar_xid, false, geometry);
  titlebar_win.HandleMapNotify();

  new_panels_should_be_expanded_ = false;
  new_panels_should_take_focus_ = false;
  XWindow content_xid = CreatePanelContentWindow(200, 400, titlebar_xid);
  ASSERT_TRUE(xconn_->MapWindow(content_xid));
  ASSERT_TRUE(xconn_->GetWindowGeometry(content_xid, &geometry));
  Window content_win(wm_.get(), content_xid, false, geometry);
  content_win.HandleMapNotify();

  Panel panel(panel_manager_, &content_win, &titlebar_win, true);
  panel.Move(0, 0, 0);

  // Check that Panel's constructor enabled shadows for both windows.
  ASSERT_TRUE(titlebar_win.shadow() != NULL);
  ASSERT_TRUE(content_win.shadow() != NULL);

  // Both the titlebar and content windows' shadows should be visible
  // initially.
  EXPECT_TRUE(titlebar_win.shadow()->is_shown());
  EXPECT_TRUE(content_win.shadow()->is_shown());
  EXPECT_DOUBLE_EQ(1.0, titlebar_win.shadow()->opacity());
  EXPECT_DOUBLE_EQ(1.0, content_win.shadow()->opacity());

  // Now tell the panel to hide the content shadow.
  panel.SetShadowOpacity(0, 0);
  EXPECT_TRUE(titlebar_win.shadow()->is_shown());
  EXPECT_TRUE(content_win.shadow()->is_shown());
  EXPECT_DOUBLE_EQ(0.0, titlebar_win.shadow()->opacity());
  EXPECT_DOUBLE_EQ(0.0, content_win.shadow()->opacity());
}

// Test that we don't let panels get smaller than the minimal allowed size.
TEST_F(PanelTest, SizeLimits) {
  // Create a panel with a really small (20x20) content window.
  XWindow titlebar_xid = CreatePanelTitlebarWindow(200, 20);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(titlebar_xid, &geometry));
  Window titlebar_win(wm_.get(), titlebar_xid, false, geometry);
  XWindow content_xid = CreatePanelContentWindow(20, 20, titlebar_xid);
  MockXConnection::WindowInfo* content_info =
      xconn_->GetWindowInfoOrDie(content_xid);
  content_info->size_hints.min_size.reset(150, 100);
  content_info->size_hints.max_size.reset(300, 250);
  ASSERT_TRUE(xconn_->GetWindowGeometry(content_xid, &geometry));
  Window content_win(wm_.get(), content_xid, false, geometry);

  // The content window should've been resized to the minimum size.
  Panel panel(panel_manager_, &content_win, &titlebar_win, true);
  EXPECT_EQ(content_info->size_hints.min_size.width,
            content_win.client_width());
  EXPECT_EQ(content_info->size_hints.min_size.height,
            content_win.client_height());

  // Drag the upper-left resize handle down and to the right.
  xconn_->set_pointer_grab_xid(panel.top_left_input_xid_);
  panel.HandleInputWindowButtonPress(
      panel.top_left_input_xid_, 0, 0, 1, CurrentTime);
  panel.HandleInputWindowPointerMotion(panel.top_left_input_xid_, 5, 5);
  xconn_->set_pointer_grab_xid(None);
  panel.HandleInputWindowButtonRelease(
      panel.top_left_input_xid_, 5, 5, 1, CurrentTime);

  // The content window size should be unchanged, since we tried to make it
  // smaller while it was already at the minimum.
  EXPECT_EQ(content_info->size_hints.min_size.width,
            content_win.client_width());
  EXPECT_EQ(content_info->size_hints.min_size.height,
            content_win.client_height());

  // Now drag the handle up and to the left and check that we restrict the
  // content window to the max size.
  xconn_->set_pointer_grab_xid(panel.top_left_input_xid_);
  panel.HandleInputWindowButtonPress(
      panel.top_left_input_xid_, 0, 0, 1, CurrentTime);
  panel.HandleInputWindowPointerMotion(panel.top_left_input_xid_, -300, -300);
  xconn_->set_pointer_grab_xid(None);
  panel.HandleInputWindowButtonRelease(
      panel.top_left_input_xid_, -300, -300, 1, CurrentTime);
  EXPECT_EQ(content_info->size_hints.max_size.width,
            content_win.client_width());
  EXPECT_EQ(content_info->size_hints.max_size.height,
            content_win.client_height());

  // Now tell the panel to make the content window bigger or smaller (this
  // is the path that gets taken when we get a ConfigureRequest).  These
  // requests should be capped as well.
  panel.ResizeContent(500, 500, GRAVITY_SOUTHEAST, true);
  EXPECT_EQ(content_info->size_hints.max_size.width,
            content_win.client_width());
  EXPECT_EQ(content_info->size_hints.max_size.height,
            content_win.client_height());
  panel.ResizeContent(50, 50, GRAVITY_SOUTHEAST, true);
  EXPECT_EQ(content_info->size_hints.min_size.width,
            content_win.client_width());
  EXPECT_EQ(content_info->size_hints.min_size.height,
            content_win.client_height());
}

// Check that the resize input windows get configured correctly depending
// on the the panel's user-resizable parameter.
TEST_F(PanelTest, ResizeParameter) {
  // If we create a panel that's only vertically-resizable, the top input
  // window should cover the width of the panel and all of the other
  // windows should be offscreen.
  resize_type_for_new_panels_ = chromeos::WM_IPC_PANEL_USER_RESIZE_VERTICALLY;
  Panel* panel = CreatePanel(200, 20, 300);

  MockXConnection::WindowInfo* top_info =
      xconn_->GetWindowInfoOrDie(panel->top_input_xid_);
  EXPECT_EQ(panel->content_x(), top_info->bounds.x);
  EXPECT_EQ(panel->titlebar_y() - Panel::kResizeBorderWidth,
            top_info->bounds.y);
  EXPECT_EQ(panel->width(), top_info->bounds.width);
  EXPECT_EQ(Panel::kResizeBorderWidth, top_info->bounds.height);

  EXPECT_TRUE(WindowIsOffscreen(panel->top_left_input_xid_));
  EXPECT_TRUE(WindowIsOffscreen(panel->top_right_input_xid_));
  EXPECT_TRUE(WindowIsOffscreen(panel->left_input_xid_));
  EXPECT_TRUE(WindowIsOffscreen(panel->right_input_xid_));

  // Horizontally-resizable panels should have input windows along their
  // sides, with all of the other windows offscreen.
  resize_type_for_new_panels_ = chromeos::WM_IPC_PANEL_USER_RESIZE_HORIZONTALLY;
  panel = CreatePanel(200, 20, 300);

  MockXConnection::WindowInfo* left_info =
      xconn_->GetWindowInfoOrDie(panel->left_input_xid_);
  EXPECT_EQ(panel->content_x() - Panel::kResizeBorderWidth,
            left_info->bounds.x);
  EXPECT_EQ(panel->titlebar_y(), left_info->bounds.y);
  EXPECT_EQ(Panel::kResizeBorderWidth, left_info->bounds.width);
  EXPECT_EQ(panel->total_height(), left_info->bounds.height);

  MockXConnection::WindowInfo* right_info =
      xconn_->GetWindowInfoOrDie(panel->right_input_xid_);
  EXPECT_EQ(panel->right(), right_info->bounds.x);
  EXPECT_EQ(panel->titlebar_y(), right_info->bounds.y);
  EXPECT_EQ(Panel::kResizeBorderWidth, right_info->bounds.width);
  EXPECT_EQ(panel->total_height(), right_info->bounds.height);

  EXPECT_TRUE(WindowIsOffscreen(panel->top_input_xid_));
  EXPECT_TRUE(WindowIsOffscreen(panel->top_left_input_xid_));
  EXPECT_TRUE(WindowIsOffscreen(panel->top_right_input_xid_));

  // Non-user-resizable panels should have all of their input windows offscreen.
  resize_type_for_new_panels_ = chromeos::WM_IPC_PANEL_USER_RESIZE_NONE;
  panel = CreatePanel(200, 20, 300);
  EXPECT_TRUE(WindowIsOffscreen(panel->top_input_xid_));
  EXPECT_TRUE(WindowIsOffscreen(panel->top_left_input_xid_));
  EXPECT_TRUE(WindowIsOffscreen(panel->top_right_input_xid_));
  EXPECT_TRUE(WindowIsOffscreen(panel->left_input_xid_));
  EXPECT_TRUE(WindowIsOffscreen(panel->right_input_xid_));
}

// Check how we move, scale, and stack the shadow that we draw as a
// separator between a panel's titlebar and content windows.
TEST_F(PanelTest, SeparatorShadow) {
  const int kWidth = 200;
  const int kTitlebarHeight = 20;
  const int kContentHeight = 300;
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  Panel* panel = CreatePanel(kWidth, kTitlebarHeight, kContentHeight);

  // Check that the separator shadow is scaled across the top of the
  // content window.
  panel->Move(0, 0, 0);
  EXPECT_EQ(panel->content_win_->composited_x(),
            panel->separator_shadow_->x());
  EXPECT_EQ(panel->content_win_->composited_y(),
            panel->separator_shadow_->y());
  EXPECT_EQ(panel->content_win_->client_width(),
            panel->separator_shadow_->width());
  EXPECT_EQ(0, panel->separator_shadow_->height());

  // When we move the panel, the shadow should get moved along with it.
  panel->Move(50, 100, 0);
  EXPECT_EQ(panel->content_win_->composited_x(),
            panel->separator_shadow_->x());
  EXPECT_EQ(panel->content_win_->composited_y(),
            panel->separator_shadow_->y());
  EXPECT_EQ(panel->content_win_->client_width(),
            panel->separator_shadow_->width());
  EXPECT_EQ(0, panel->separator_shadow_->height());

  // Check that the separator shadow is stacked between the titlebar and
  // the content.
  panel->StackAtTopOfLayer(StackingManager::LAYER_PACKED_PANEL_IN_BAR);
  EXPECT_LT(stage->GetStackingIndex(panel->titlebar_win_->actor()),
            stage->GetStackingIndex(panel->separator_shadow_->group()));
  EXPECT_LT(stage->GetStackingIndex(panel->separator_shadow_->group()),
            stage->GetStackingIndex(panel->content_win_->actor()));

  // The shadow should get restacked along with the panel.
  panel->StackAtTopOfLayer(StackingManager::LAYER_DRAGGED_PANEL);
  EXPECT_LT(stage->GetStackingIndex(panel->titlebar_win_->actor()),
            stage->GetStackingIndex(panel->separator_shadow_->group()));
  EXPECT_LT(stage->GetStackingIndex(panel->separator_shadow_->group()),
            stage->GetStackingIndex(panel->content_win_->actor()));

  // Check that the shadow is moved correctly in response to resizes where
  // a corner other than the top left one is fixed.
  int new_width = 100;
  panel->ResizeContent(new_width, 200, GRAVITY_SOUTHEAST, true);
  EXPECT_EQ(panel->content_win_->composited_x(),
            panel->separator_shadow_->x());
  EXPECT_EQ(panel->content_win_->composited_y(),
            panel->separator_shadow_->y());
  EXPECT_EQ(panel->content_win_->client_width(),
            panel->separator_shadow_->width());
  EXPECT_EQ(0, panel->separator_shadow_->height());

  // When we get a request to move a panel while it's fullscreen, we store
  // the requested position and apply it after the panel is unfullscreened.
  // Check that the shadow gets moved to the stored position too.
  panel->SetFullscreenState(true);
  panel->Move(20, 30, 0);
  panel->SetFullscreenState(false);

  // First double-check that the content window got moved to the requested
  // position.
  ASSERT_EQ(20 - new_width, panel->content_win_->composited_x());
  ASSERT_EQ(30 + kTitlebarHeight, panel->content_win_->composited_y());

  // Now check the shadow.
  EXPECT_EQ(panel->content_win_->composited_x(),
            panel->separator_shadow_->x());
  EXPECT_EQ(panel->content_win_->composited_y(),
            panel->separator_shadow_->y());
  EXPECT_EQ(panel->content_win_->client_width(),
            panel->separator_shadow_->width());
  EXPECT_EQ(0, panel->separator_shadow_->height());
}

// Check that we update the size limits for panel content windows when
// the window's size hints in the WM_NORMAL_HINTS property are changed.
TEST_F(PanelTest, ReloadSizeLimits) {
  // Create a panel and check that its content window gets the 200x200 size
  // that we requested.
  const int kWidth = 200;
  const int kTitlebarHeight = 20;
  const int kContentHeight = 200;
  Panel* panel = CreatePanel(kWidth, kTitlebarHeight, kContentHeight);

  const XWindow content_xid = panel->content_xid();
  MockXConnection::WindowInfo* content_info =
      xconn_->GetWindowInfoOrDie(content_xid);
  ASSERT_EQ(kWidth, content_info->bounds.width);
  ASSERT_EQ(kContentHeight, content_info->bounds.height);

  // Set a minimum size for the content window that's larger than its
  // current size.  We shouldn't resize the window immediately when we see
  // the property change...
  content_info->size_hints.min_size.reset(300, 250);
  XEvent event;
  xconn_->InitPropertyNotifyEvent(
      &event, content_xid, xconn_->GetAtomOrDie("WM_NORMAL_HINTS"));
  wm_->HandleEvent(&event);
  EXPECT_EQ(kWidth, content_info->bounds.width);
  EXPECT_EQ(kContentHeight, content_info->bounds.height);

  // ... but we should use the updated limits when we get a
  // ConfigureRequest event.
  xconn_->InitConfigureRequestEvent(&event, content_xid, Rect(0, 0, 230, 220));
  wm_->HandleEvent(&event);
  EXPECT_EQ(content_info->size_hints.min_size.width,
            content_info->bounds.width);
  EXPECT_EQ(content_info->size_hints.min_size.height,
            content_info->bounds.height);
}

TEST_F(PanelTest, TransientWindowsAreConstrainedOnscreen) {
  // Create a panel and move it off the left edge of the screen.
  static const int kPanelWidth = 200;
  static const int kTitlebarHeight = 20;
  static const int kContentHeight = 600;
  Panel* panel = CreatePanel(kPanelWidth, kTitlebarHeight, kContentHeight);
  panel->MoveX(-300 - kPanelWidth, 0);

  static const int kTransientWidth = 400;
  static const int kTransientHeight = 300;
  XWindow transient_xid =
      CreateBasicWindow(0, 0, kTransientWidth, kTransientHeight);
  xconn_->GetWindowInfoOrDie(transient_xid)->transient_for =
      panel->content_xid();
  SendInitialEventsForWindow(transient_xid);

  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  EXPECT_EQ(0, transient_info->bounds.x);
  EXPECT_EQ(wm_->height() - (kContentHeight + kTransientHeight) / 2,
            transient_info->bounds.y);
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
