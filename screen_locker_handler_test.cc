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
  XWindow screen_locker_xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      0, 0,   // x, y
      wm_->width(), wm_->height(),
      true,   // override redirect
      false,  // input only
      0);     // event mask
  wm_->wm_ipc()->SetWindowType(
      screen_locker_xid, chromeos::WM_IPC_WINDOW_CHROME_SCREEN_LOCKER, NULL);
  ASSERT_TRUE(xconn_->MapWindow(screen_locker_xid));
  SendInitialEventsForWindow(screen_locker_xid);

  // This window's actor *should* be added to a group, and this should now
  // be the only group that we're drawing.
  MockCompositor::TexturePixmapActor* screen_locker_actor =
      GetMockActorForWindow(wm_->GetWindowOrDie(screen_locker_xid));
  EXPECT_EQ(static_cast<size_t>(1),
            screen_locker_actor->visibility_groups().size());
  EXPECT_TRUE(screen_locker_actor->visibility_groups().count(
                WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER));
  EXPECT_EQ(static_cast<size_t>(1),
            compositor_->active_visibility_groups().size());
  EXPECT_TRUE(compositor_->active_visibility_groups().count(
                WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER));

  // Now unmap the screen locker window and check that the original
  // toplevel window would be drawn again.
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
