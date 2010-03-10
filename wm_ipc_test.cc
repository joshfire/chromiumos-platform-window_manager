// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "window_manager/clutter_interface.h"
#include "window_manager/event_loop.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/test_lib.h"
#include "window_manager/window_manager.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

namespace window_manager {

class WmIpcTest : public BasicWindowManagerTest {};

TEST_F(WmIpcTest, XidIncludedInMessage) {
  // Create a window and send a message to it.
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  WmIpc::Message sent_msg(WmIpc::Message::CHROME_NOTIFY_PANEL_STATE);
  sent_msg.set_param(0, 1);
  EXPECT_TRUE(wm_->wm_ipc()->SendMessage(xid, sent_msg));

  // Now check that the message was really sent, and that we end up with
  // the same data that we sent after asking WmIpc to parse it for us.
  ASSERT_EQ(1, static_cast<int>(info->client_messages.size()));
  const XClientMessageEvent& event = info->client_messages[0];
  WmIpc::Message received_msg;
  EXPECT_TRUE(wm_->wm_ipc()->GetMessage(event.window,
                                        event.message_type,
                                        event.format,
                                        event.data.l,
                                        &received_msg));
  EXPECT_EQ(WmIpc::Message::CHROME_NOTIFY_PANEL_STATE, received_msg.type());
  EXPECT_EQ(xid, received_msg.xid());
  EXPECT_EQ(1, received_msg.param(0));
  EXPECT_EQ(0, received_msg.param(1));
  EXPECT_EQ(0, received_msg.param(2));
  EXPECT_EQ(0, received_msg.param(3));
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}