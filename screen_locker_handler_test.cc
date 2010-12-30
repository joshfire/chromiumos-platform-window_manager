// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cmath>
#include <vector>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/screen_locker_handler.h"
#include "window_manager/test_lib.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::vector;

namespace window_manager {

class ScreenLockerHandlerTest : public BasicWindowManagerTest {
 protected:
  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();
    handler_ = wm_->screen_locker_handler_.get();
  }

  MockCompositor::TexturePixmapActor* GetSnapshotActor() {
    return dynamic_cast<MockCompositor::TexturePixmapActor*>(
        handler_->snapshot_actor_.get());
  }

  void TestActorConfiguredForSlowClose(
      MockCompositor::TexturePixmapActor* actor) {
    static const float kSizeRatio = ScreenLockerHandler::kSlowCloseSizeRatio;
    CHECK(actor);
    EXPECT_TRUE(actor->is_shown());
    EXPECT_FLOAT_EQ(round(0.5 * (1.0 - kSizeRatio) * wm_->width()),
                    actor->x());
    EXPECT_FLOAT_EQ(round(0.5 * (1.0 - kSizeRatio) * wm_->height()),
                    actor->y());
    EXPECT_FLOAT_EQ(kSizeRatio, actor->scale_x());
    EXPECT_FLOAT_EQ(kSizeRatio, actor->scale_y());
    EXPECT_FLOAT_EQ(1.0, actor->opacity());
  }

  void TestActorConfiguredForUndoSlowClose(
      MockCompositor::TexturePixmapActor* actor) {
    CHECK(actor);
    EXPECT_TRUE(actor->is_shown());
    EXPECT_EQ(0, actor->x());
    EXPECT_EQ(0, actor->y());
    EXPECT_FLOAT_EQ(1.0, actor->scale_x());
    EXPECT_FLOAT_EQ(1.0, actor->scale_y());
    EXPECT_FLOAT_EQ(1.0, actor->opacity());
  }

  void TestActorConfiguredForFastClose(
      MockCompositor::TexturePixmapActor* actor) {
    CHECK(actor);
    EXPECT_TRUE(actor->is_shown());
    EXPECT_EQ(static_cast<int>(round(0.5 * wm_->width())), actor->x());
    EXPECT_EQ(static_cast<int>(round(0.5 * wm_->height())), actor->y());
    EXPECT_FLOAT_EQ(0.0, actor->scale_x());
    EXPECT_FLOAT_EQ(0.0, actor->scale_y());
    EXPECT_FLOAT_EQ(0.0, actor->opacity());
  }

  void TestActorConfiguredForFadeout(
      MockCompositor::TexturePixmapActor* actor) {
    CHECK(actor);
    EXPECT_TRUE(actor->is_shown());
    EXPECT_EQ(0, actor->x());
    EXPECT_EQ(0, actor->y());
    EXPECT_FLOAT_EQ(1.0, actor->scale_x());
    EXPECT_FLOAT_EQ(1.0, actor->scale_y());
    EXPECT_FLOAT_EQ(0.0, actor->opacity());
  }

  bool IsOnlyActiveVisibilityGroup(int group) {
    if (compositor_->active_visibility_groups().size() !=
        static_cast<size_t>(1)) {
      return false;
    }
    return (*(compositor_->active_visibility_groups().begin()) == group);
  }

  ScreenLockerHandler* handler_;
};

TEST_F(ScreenLockerHandlerTest, BasicLock) {
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
  EXPECT_EQ(0, screen_locker_info->bounds.x);
  EXPECT_EQ(0, screen_locker_info->bounds.y);
  EXPECT_EQ(wm_->width(), screen_locker_info->bounds.width);
  EXPECT_EQ(wm_->height(), screen_locker_info->bounds.height);
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
  EXPECT_TRUE(IsOnlyActiveVisibilityGroup(
                  WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER));

  // We should've redrawn the screen and sent the screen locker window a
  // message letting it know that we did so.
  EXPECT_GT(compositor_->num_draws(), initial_num_draws);
  EXPECT_TRUE(
      GetFirstWmIpcMessageOfType(
          screen_locker_xid,
          chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_SCREEN_REDRAWN_FOR_LOCK,
          &msg));

  // We shouldn't animate a snapshot of the screen when we go directly from
  // the unlocked to locked states (without seeing pre-lock) -- this
  // probably means that the screen's getting locked because the system is
  // about to be suspended, so we want to make sure that we're not showing
  // the unlocked contents onscreen (http;//crosbug.com/8867).  (It could
  // also mean that we're running on a system that doesn't report power
  // button releases correctly.)
  EXPECT_TRUE(handler_->snapshot_actor_.get() == NULL);

  // Now resize the root window and check that the screen locker window is
  // also resized.
  const XWindow root_xid = xconn_->GetRootWindow();
  MockXConnection::WindowInfo* root_info = xconn_->GetWindowInfoOrDie(root_xid);
  const int new_width = root_info->bounds.width + 20;
  const int new_height = root_info->bounds.height + 20;
  xconn_->ResizeWindow(root_xid, Size(new_width, new_height));
  XEvent resize_event;
  xconn_->InitConfigureNotifyEvent(&resize_event, root_xid);
  wm_->HandleEvent(&resize_event);
  EXPECT_EQ(new_width, screen_locker_info->bounds.width);
  EXPECT_EQ(new_height, screen_locker_info->bounds.height);

  // Unmap the screen locker window and check that the original toplevel
  // window would be drawn again.
  ASSERT_TRUE(xconn_->UnmapWindow(screen_locker_xid));
  XEvent event;
  xconn_->InitUnmapEvent(&event, screen_locker_xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(compositor_->active_visibility_groups().empty());
}

TEST_F(ScreenLockerHandlerTest, AbortedLock) {
  // Tell the window manager that the user started holding the power button
  // to lock the screen.
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_POWER_BUTTON_STATE);
  msg.set_param(0, chromeos::WM_IPC_POWER_BUTTON_PRE_LOCK);
  SendWmIpcMessage(msg);

  // We should have taken a snapshot of the screen.
  MockCompositor::TexturePixmapActor* actor = GetSnapshotActor();
  ASSERT_TRUE(actor != NULL);
  TestActorConfiguredForSlowClose(actor);
  EXPECT_NE(-1, handler_->destroy_snapshot_timeout_id_);

  // The snapshot should be the only actor currently visible.
  EXPECT_EQ(static_cast<size_t>(2), actor->visibility_groups().size());
  EXPECT_TRUE(actor->visibility_groups().count(
                  WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER) != 0);
  EXPECT_TRUE(actor->visibility_groups().count(
                  WindowManager::VISIBILITY_GROUP_SESSION_ENDING) != 0);
  EXPECT_TRUE(IsOnlyActiveVisibilityGroup(
                  WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER));

  // Now tell the WM that the button was released before being held long
  // enough to lock.
  msg.set_param(0, chromeos::WM_IPC_POWER_BUTTON_ABORTED_LOCK);
  SendWmIpcMessage(msg);

  // Check that we're still showing the same actor, and that it's being
  // scaled back to its natural size.
  ASSERT_EQ(actor, GetSnapshotActor());
  TestActorConfiguredForUndoSlowClose(actor);

  // Check that a timeout was registered to destroy the snapshot, and then
  // invoke the callback and check that the actor was destroyed and we're
  // displaying all actors again.
  ASSERT_NE(-1, handler_->destroy_snapshot_timeout_id_);
  if (!EventLoop::IsTimerFdSupported()) {
    LOG(ERROR) << "Aborting test because of missing timerfd support";
    return;
  }
  wm_->event_loop()->RunTimeoutForTesting(
      handler_->destroy_snapshot_timeout_id_);
  EXPECT_EQ(-1, handler_->destroy_snapshot_timeout_id_);
  EXPECT_EQ(static_cast<size_t>(0),
            compositor_->active_visibility_groups().size());
}

TEST_F(ScreenLockerHandlerTest, SuccessfulLock) {
  // Tell the window manager that we're in the pre-lock state.
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_POWER_BUTTON_STATE);
  msg.set_param(0, chromeos::WM_IPC_POWER_BUTTON_PRE_LOCK);
  SendWmIpcMessage(msg);
  MockCompositor::TexturePixmapActor* actor = GetSnapshotActor();
  ASSERT_TRUE(actor != NULL);
  TestActorConfiguredForSlowClose(actor);

  // Map a screen locker window.
  XWindow screen_locker_xid = CreateSimpleWindow();
  wm_->wm_ipc()->SetWindowType(
      screen_locker_xid, chromeos::WM_IPC_WINDOW_CHROME_SCREEN_LOCKER, NULL);
  SendInitialEventsForWindow(screen_locker_xid);

  // We should still be showing the snapshot actor, but it should be
  // getting scaled down to the center of the screen.
  ASSERT_EQ(actor, GetSnapshotActor());
  TestActorConfiguredForFastClose(actor);

  // Invoke the timeout to destroy it and check that we're showing only the
  // screen locker window.
  ASSERT_NE(-1, handler_->destroy_snapshot_timeout_id_);
  if (!EventLoop::IsTimerFdSupported()) {
    LOG(ERROR) << "Aborting test because of missing timerfd support";
    return;
  }
  wm_->event_loop()->RunTimeoutForTesting(
      handler_->destroy_snapshot_timeout_id_);
  EXPECT_EQ(-1, handler_->destroy_snapshot_timeout_id_);
  EXPECT_TRUE(GetSnapshotActor() == NULL);
  EXPECT_TRUE(IsOnlyActiveVisibilityGroup(
                  WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER));
}

TEST_F(ScreenLockerHandlerTest, AbortedShutdown) {
  // Tell the window manager that the user started holding the power button
  // to shut down the system.
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_POWER_BUTTON_STATE);
  msg.set_param(0, chromeos::WM_IPC_POWER_BUTTON_PRE_SHUTDOWN);
  SendWmIpcMessage(msg);

  // We should have taken a snapshot of the screen.
  MockCompositor::TexturePixmapActor* actor = GetSnapshotActor();
  ASSERT_TRUE(actor != NULL);
  TestActorConfiguredForSlowClose(actor);
  EXPECT_NE(-1, handler_->destroy_snapshot_timeout_id_);

  // The snapshot should be the only actor currently visible.
  EXPECT_TRUE(actor->visibility_groups().count(
                  WindowManager::VISIBILITY_GROUP_SESSION_ENDING) != 0);
  EXPECT_TRUE(IsOnlyActiveVisibilityGroup(
                  WindowManager::VISIBILITY_GROUP_SESSION_ENDING));

  // Now tell the WM that the button was released before being held long
  // enough to shut down.
  msg.set_param(0, chromeos::WM_IPC_POWER_BUTTON_ABORTED_SHUTDOWN);
  SendWmIpcMessage(msg);

  // Check that we're still showing the same actor, and that it's being
  // scaled back to its natural size.
  ASSERT_EQ(actor, GetSnapshotActor());
  TestActorConfiguredForUndoSlowClose(actor);

  // Check that a timeout was registered to destroy the snapshot, and then
  // invoke the callback and check that the actor was destroyed and we're
  // displaying all actors again.
  ASSERT_NE(-1, handler_->destroy_snapshot_timeout_id_);
  if (!EventLoop::IsTimerFdSupported()) {
    LOG(ERROR) << "Aborting test because of missing timerfd support";
    return;
  }
  wm_->event_loop()->RunTimeoutForTesting(
      handler_->destroy_snapshot_timeout_id_);
  EXPECT_EQ(-1, handler_->destroy_snapshot_timeout_id_);
  EXPECT_EQ(static_cast<size_t>(0),
            compositor_->active_visibility_groups().size());

  // Now map a screen locker window so we can try the same thing from the
  // locked state.
  XWindow screen_locker_xid = CreateSimpleWindow();
  wm_->wm_ipc()->SetWindowType(
      screen_locker_xid, chromeos::WM_IPC_WINDOW_CHROME_SCREEN_LOCKER, NULL);
  SendInitialEventsForWindow(screen_locker_xid);
  ASSERT_TRUE(IsOnlyActiveVisibilityGroup(
                  WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER));

  // Enter the pre-shutdown state as before.
  msg.set_param(0, chromeos::WM_IPC_POWER_BUTTON_PRE_SHUTDOWN);
  SendWmIpcMessage(msg);
  actor = GetSnapshotActor();
  ASSERT_TRUE(actor != NULL);
  TestActorConfiguredForSlowClose(actor);
  EXPECT_TRUE(IsOnlyActiveVisibilityGroup(
                  WindowManager::VISIBILITY_GROUP_SESSION_ENDING));

  // After aborting, we should be showing just the screen locker window.
  msg.set_param(0, chromeos::WM_IPC_POWER_BUTTON_ABORTED_SHUTDOWN);
  SendWmIpcMessage(msg);
  ASSERT_NE(-1, handler_->destroy_snapshot_timeout_id_);
  wm_->event_loop()->RunTimeoutForTesting(
      handler_->destroy_snapshot_timeout_id_);
  EXPECT_EQ(-1, handler_->destroy_snapshot_timeout_id_);
  EXPECT_TRUE(IsOnlyActiveVisibilityGroup(
                  WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER));
}

// Test that we do stuff in response to notification that the system is
// shutting down.
TEST_F(ScreenLockerHandlerTest, HandleShutdown) {
  // Go into the pre-shutdown state first.
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_POWER_BUTTON_STATE);
  msg.set_param(0, chromeos::WM_IPC_POWER_BUTTON_PRE_SHUTDOWN);
  SendWmIpcMessage(msg);

  // Check that we've started the slow-close animation.
  MockCompositor::TexturePixmapActor* actor = GetSnapshotActor();
  ASSERT_TRUE(actor != NULL);
  TestActorConfiguredForSlowClose(actor);
  EXPECT_NE(-1, handler_->destroy_snapshot_timeout_id_);
  EXPECT_TRUE(IsOnlyActiveVisibilityGroup(
                  WindowManager::VISIBILITY_GROUP_SESSION_ENDING));

  // Notify the window manager that the system is being shut down.
  msg.set_type(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_SHUTTING_DOWN);
  msg.set_param(0, 0);
  SendWmIpcMessage(msg);

  // Check that we grabbed the pointer and keyboard and assigned a transparent
  // cursor to the root window.
  XWindow root = xconn_->GetRootWindow();
  EXPECT_EQ(root, xconn_->pointer_grab_xid());
  EXPECT_EQ(root, xconn_->keyboard_grab_xid());
  EXPECT_EQ(MockXConnection::kTransparentCursor,
            xconn_->GetWindowInfoOrDie(root)->cursor);

  // We should be using the same snapshot that we already grabbed, and we
  // should be displaying the fast-close animation.
  ASSERT_EQ(actor, GetSnapshotActor());
  TestActorConfiguredForFastClose(actor);
  EXPECT_TRUE(IsOnlyActiveVisibilityGroup(
                  WindowManager::VISIBILITY_GROUP_SESSION_ENDING));

  // There's no need to destroy the snapshot after we're done with the
  // animation; we're not going to be showing anything else onscreen.
  EXPECT_EQ(-1, handler_->destroy_snapshot_timeout_id_);
}

// Test that we don't consider the screen to be locked until the screen
// locker window is actually visible.
TEST_F(ScreenLockerHandlerTest, DeferLockUntilWindowIsVisible) {
  // Enable the sync request protocol on a screen locker window before
  // mapping it so that we'll hold off on fetching its pixmap until it
  // tells us that it's ready.
  XWindow screen_locker_xid = CreateSimpleWindow();
  wm_->wm_ipc()->SetWindowType(
      screen_locker_xid, chromeos::WM_IPC_WINDOW_CHROME_SCREEN_LOCKER, NULL);
  ConfigureWindowForSyncRequestProtocol(screen_locker_xid);

  // We should continue showing all actors after the locker window is mapped.
  SendInitialEventsForWindow(screen_locker_xid);
  Window* screen_locker_win = wm_->GetWindowOrDie(screen_locker_xid);
  ASSERT_FALSE(screen_locker_win->has_initial_pixmap());
  EXPECT_FALSE(handler_->is_locked_);
  EXPECT_EQ(static_cast<size_t>(0),
            compositor_->active_visibility_groups().size());

  // When we're notified that the pixmap is painted, we should switch to
  // showing only the screen locker actor.
  SendSyncRequestProtocolAlarm(screen_locker_xid);
  ASSERT_TRUE(screen_locker_win->has_initial_pixmap());
  EXPECT_TRUE(handler_->is_locked_);
  EXPECT_TRUE(IsOnlyActiveVisibilityGroup(
                  WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER));
}

// Check that when we see an override-redirect info bubble window that asks to
// remain visible while the screen is locked, we add it to the screen locker
// visibility group.
TEST_F(ScreenLockerHandlerTest, ShowSomeOtherWindowsWhileLocked) {
  XWindow info_bubble_xid = CreateSimpleWindow();
  xconn_->GetWindowInfoOrDie(info_bubble_xid)->override_redirect = true;
  vector<int> params;
  params.push_back(1);  // show while locked
  wm_->wm_ipc()->SetWindowType(
      info_bubble_xid, chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE, &params);
  xconn_->MapWindow(info_bubble_xid);
  SendInitialEventsForWindow(info_bubble_xid);
  MockCompositor::TexturePixmapActor* info_bubble_actor =
      GetMockActorForWindow(wm_->GetWindowOrDie(info_bubble_xid));
  EXPECT_TRUE(info_bubble_actor->visibility_groups().count(
                WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER));

  // The actor should be removed from the visibility group when the window is
  // unmapped.
  XEvent event;
  xconn_->InitUnmapEvent(&event, info_bubble_xid);
  wm_->HandleEvent(&event);
  EXPECT_FALSE(info_bubble_actor->visibility_groups().count(
                 WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER));
}

// Test that we handle messages from Chrome notifying us that the user is
// signing out.
TEST_F(ScreenLockerHandlerTest, SigningOut) {
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_SIGNING_OUT);
  SendWmIpcMessage(msg);

  // We should grab the pointer and keyboard, assign a transparent cursor to the
  // root window, and fade out a snapshot of the screen.
  XWindow root = xconn_->GetRootWindow();
  EXPECT_EQ(root, xconn_->pointer_grab_xid());
  EXPECT_EQ(root, xconn_->keyboard_grab_xid());
  EXPECT_EQ(MockXConnection::kTransparentCursor,
            xconn_->GetWindowInfoOrDie(root)->cursor);
  TestActorConfiguredForFadeout(GetSnapshotActor());
  EXPECT_TRUE(IsOnlyActiveVisibilityGroup(
                  WindowManager::VISIBILITY_GROUP_SESSION_ENDING));
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
