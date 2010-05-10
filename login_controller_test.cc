// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/scoped_ptr.h"
#include "base/logging.h"
#include "window_manager/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/login_controller.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

namespace window_manager {

class LoginControllerTest : public BasicWindowManagerTest {
 protected:
  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();
    // Use a WindowManager object that thinks that Chrome isn't logged in
    // yet so that LoginController will manage non-login windows as well.
    wm_.reset(new WindowManager(event_loop_.get(),
                                xconn_.get(),
                                compositor_.get(),
                                false));  // logged_in=false
    CHECK(wm_->Init());
  }
};

// Check that LoginController does some half-baked handling of any other
// windows that get mapped before Chrome is in a logged-in state.
TEST_F(LoginControllerTest, OtherWindows) {
  const int initial_x = 20;
  const int initial_y = 30;
  const int initial_width = 300;
  const int initial_height = 200;
  const XWindow xid =
      CreateBasicWindow(initial_x, initial_y, initial_width, initial_height);
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  ASSERT_FALSE(info->mapped);

  XEvent event;
  MockXConnection::InitCreateWindowEvent(&event, *info);
  wm_->HandleEvent(&event);
  Window* win = wm_->GetWindowOrDie(xid);
  MockCompositor::Actor* actor =
      dynamic_cast<MockCompositor::Actor*>(win->actor());
  CHECK(actor);

  // If LoginManager sees a MapRequest event before Chrome is logged in,
  // check that it maps the window in the requested location.
  MockXConnection::InitMapRequestEvent(&event, *info);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(info->mapped);
  EXPECT_EQ(initial_x, info->x);
  EXPECT_EQ(initial_y, info->y);
  EXPECT_EQ(initial_width, info->width);
  EXPECT_EQ(initial_height, info->height);

  // The window should still be in the same spot after it's mapped, and it
  // should be visible too.
  MockXConnection::InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(initial_x, info->x);
  EXPECT_EQ(initial_y, info->y);
  EXPECT_EQ(initial_width, info->width);
  EXPECT_EQ(initial_height, info->height);
  EXPECT_EQ(initial_x, actor->x());
  EXPECT_EQ(initial_y, actor->y());
  EXPECT_EQ(initial_width, actor->GetWidth());
  EXPECT_EQ(initial_height, actor->GetHeight());
  EXPECT_TRUE(actor->visible());
  EXPECT_DOUBLE_EQ(1, actor->opacity());

  // Check that the client is able to move and resize itself.
  const int new_x = 40;
  const int new_y = 50;
  const int new_width = 500;
  const int new_height = 400;
  MockXConnection::InitConfigureRequestEvent(
      &event, xid, new_x, new_y, new_width, new_height);
  wm_->HandleEvent(&event);
  EXPECT_EQ(new_x, info->x);
  EXPECT_EQ(new_y, info->y);
  EXPECT_EQ(new_width, info->width);
  EXPECT_EQ(new_height, info->height);

  MockXConnection::InitConfigureNotifyEvent(&event, *info);
  wm_->HandleEvent(&event);
  EXPECT_EQ(new_x, actor->x());
  EXPECT_EQ(new_y, actor->y());
  EXPECT_EQ(new_width, actor->GetWidth());
  EXPECT_EQ(new_height, actor->GetHeight());

  MockXConnection::InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
