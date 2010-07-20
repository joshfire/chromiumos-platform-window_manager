// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/test_lib.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

namespace window_manager {

class ScreenLockerHandlerTest : public BasicWindowManagerTest {};

TEST_F(ScreenLockerHandlerTest, Basic) {
  // Create a regular toplevel window.
  XWindow toplevel_xid = CreateSimpleWindow();
  SendInitialEventsForWindow(toplevel_xid);

  // The window's actor shouldn't be in any visibility groups, and the
  // compositor shouldn't be restricting its drawing to a particular group.
  MockCompositor::TexturePixmapActor* toplevel_actor =
      GetMockActorForWindow(wm_->GetWindowOrDie(toplevel_xid));
  EXPECT_TRUE(toplevel_actor->visibility_groups().empty());
  EXPECT_TRUE(compositor_->active_visibility_groups().empty());

  // Now create a screen locker window.
  XWindow screen_locker_xid =
      CreateBasicWindow(5, 5, wm_->width() - 5, wm_->height() - 5);
  MockXConnection::WindowInfo* screen_locker_info =
      xconn_->GetWindowInfoOrDie(screen_locker_xid);
  wm_->wm_ipc()->SetWindowType(
      screen_locker_xid, chromeos::WM_IPC_WINDOW_CHROME_SCREEN_LOCKER, NULL);
  WmIpc::Message msg;
  EXPECT_FALSE(
      GetFirstWmIpcMessageOfType(
          screen_locker_xid,
          chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_SCREEN_REDRAWN_FOR_LOCK,
          &msg));
  const int initial_num_draws = compositor_->num_draws();
  SendInitialEventsForWindow(screen_locker_xid);
  Window* screen_locker_win = wm_->GetWindowOrDie(screen_locker_xid);
  MockCompositor::TexturePixmapActor* screen_locker_actor =
      GetMockActorForWindow(screen_locker_win);

  // Check that the window was moved to (0, 0), resized to cover the whole
  // screen, stacked correctly, and shown.
  EXPECT_EQ(0, screen_locker_info->x);
  EXPECT_EQ(0, screen_locker_info->y);
  EXPECT_EQ(wm_->width(), screen_locker_info->width);
  EXPECT_EQ(wm_->height(), screen_locker_info->height);
  EXPECT_EQ(0, screen_locker_actor->GetX());
  EXPECT_EQ(0, screen_locker_actor->GetY());
  EXPECT_TRUE(WindowIsInLayer(screen_locker_win,
                              StackingManager::LAYER_SCREEN_LOCKER));
  EXPECT_TRUE(screen_locker_actor->is_shown());

  // This window's actor *should* be added to a group, and this should now
  // be the only group that we're drawing.
  EXPECT_EQ(static_cast<size_t>(1),
            screen_locker_actor->visibility_groups().size());
  EXPECT_TRUE(screen_locker_actor->visibility_groups().count(
                WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER));
  EXPECT_EQ(static_cast<size_t>(1),
            compositor_->active_visibility_groups().size());
  EXPECT_TRUE(compositor_->active_visibility_groups().count(
                WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER));

  // We should've redrawn the screen and sent the screen locker window a
  // message letting it know that we did so.
  EXPECT_GT(compositor_->num_draws(), initial_num_draws);
  EXPECT_TRUE(
      GetFirstWmIpcMessageOfType(
          screen_locker_xid,
          chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_SCREEN_REDRAWN_FOR_LOCK,
          &msg));

  // Now resize the root window and check that the screen locker window is
  // also resized.
  const XWindow root_xid = xconn_->GetRootWindow();
  MockXConnection::WindowInfo* root_info = xconn_->GetWindowInfoOrDie(root_xid);
  const int new_width = root_info->width + 20;
  const int new_height = root_info->height + 20;
  xconn_->ResizeWindow(root_xid, new_width, new_height);
  XEvent resize_event;
  xconn_->InitConfigureNotifyEvent(&resize_event, root_xid);
  wm_->HandleEvent(&resize_event);
  EXPECT_EQ(new_width, screen_locker_info->width);
  EXPECT_EQ(new_height, screen_locker_info->height);

  // Unmap the screen locker window and check that the original toplevel
  // window would be drawn again.
  ASSERT_TRUE(xconn_->UnmapWindow(screen_locker_xid));
  XEvent event;
  xconn_->InitUnmapEvent(&event, screen_locker_xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(compositor_->active_visibility_groups().empty());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
