// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/chrome_watchdog.h"
#include "window_manager/event_loop.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window_manager.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

DECLARE_bool(kill_chrome_if_hanging);  // from chrome_watchdog.cc

using window_manager::util::GetHostname;

namespace window_manager {

class ChromeWatchdogTest : public BasicWindowManagerTest {};

TEST_F(ChromeWatchdogTest, Basic) {
  ChromeWatchdog* watchdog = wm_->chrome_watchdog_.get();

  const XWindow kRoot = xconn_->GetRootWindow();
  const XAtom kProtocolsAtom = xconn_->GetAtomOrDie("WM_PROTOCOLS");
  const XAtom kPingAtom = xconn_->GetAtomOrDie("_NET_WM_PING");
  const XAtom kPidAtom = xconn_->GetAtomOrDie("_NET_WM_PID");
  const XAtom kClientMachineAtom = xconn_->GetAtomOrDie("WM_CLIENT_MACHINE");
  const XAtom kCardinalAtom = xconn_->GetAtomOrDie("CARDINAL");

  int kPid = 456;
  int kTimestamp = 123;
  int kTimeoutMs = 5000;

  // We should fail if no windows have been mapped.
  EXPECT_FALSE(watchdog->SendPingToChrome(kTimestamp, kTimeoutMs));

  // Map a single non-Chrome window and check that we don't send a ping
  // message to it.
  XWindow non_chrome_xid = CreateSimpleWindow();
  AppendAtomToProperty(non_chrome_xid, kProtocolsAtom, kPingAtom);
  xconn_->SetIntProperty(non_chrome_xid, kPidAtom, kCardinalAtom, kPid);
  xconn_->SetStringProperty(non_chrome_xid, kClientMachineAtom, GetHostname());
  SendInitialEventsForWindow(non_chrome_xid);
  xconn_->GetWindowInfoOrDie(non_chrome_xid)->client_messages.clear();
  EXPECT_FALSE(watchdog->SendPingToChrome(kTimestamp, kTimeoutMs));
  EXPECT_TRUE(
      xconn_->GetWindowInfoOrDie(non_chrome_xid)->client_messages.empty());

  // We should also fail if we only have a Chrome window that doesn't
  // support _NET_WM_PING...
  XWindow non_ping_xid = CreateToplevelWindow(2, 0, 0, 0, 640, 480);
  xconn_->SetIntProperty(non_ping_xid, kPidAtom, kCardinalAtom, kPid);
  xconn_->SetStringProperty(non_ping_xid, kClientMachineAtom, GetHostname());
  SendInitialEventsForWindow(non_ping_xid);
  EXPECT_FALSE(watchdog->SendPingToChrome(kTimestamp, kTimeoutMs));

  // ... or that's running on a different host...
  XWindow other_host_xid = CreateToplevelWindow(2, 0, 0, 0, 640, 480);
  AppendAtomToProperty(other_host_xid, kProtocolsAtom, kPingAtom);
  xconn_->SetIntProperty(other_host_xid, kPidAtom, kCardinalAtom, kPid);
  xconn_->SetStringProperty(other_host_xid, kClientMachineAtom, "bogus123");
  SendInitialEventsForWindow(other_host_xid);
  EXPECT_FALSE(watchdog->SendPingToChrome(kTimestamp, kTimeoutMs));

  // ... or that didn't supply its PID.
  XWindow no_pid_xid = CreateToplevelWindow(2, 0, 0, 0, 640, 480);
  AppendAtomToProperty(no_pid_xid, kProtocolsAtom, kPingAtom);
  xconn_->SetStringProperty(no_pid_xid, kClientMachineAtom, GetHostname());
  SendInitialEventsForWindow(no_pid_xid);
  EXPECT_FALSE(watchdog->SendPingToChrome(kTimestamp, kTimeoutMs));

  // Now create a Chrome window that supports _NET_WM_PING and has supplied
  // a PID and is running on the local host, and check that it receives a
  // message.
  XWindow toplevel_xid = CreateToplevelWindow(2, 0, 0, 0, 640, 480);
  AppendAtomToProperty(toplevel_xid, kProtocolsAtom, kPingAtom);
  xconn_->SetIntProperty(toplevel_xid, kPidAtom, kCardinalAtom, kPid);
  xconn_->SetStringProperty(toplevel_xid, kClientMachineAtom, GetHostname());
  SendInitialEventsForWindow(toplevel_xid);
  MockXConnection::WindowInfo* toplevel_info =
      xconn_->GetWindowInfoOrDie(toplevel_xid);
  toplevel_info->client_messages.clear();
  EXPECT_TRUE(watchdog->SendPingToChrome(kTimestamp, kTimeoutMs));

  // Also check the contents of the message, per EWMH's specification of
  // _NET_WM_PING.
  ASSERT_EQ(1, toplevel_info->client_messages.size());
  const XClientMessageEvent& msg = toplevel_info->client_messages[0];
  EXPECT_EQ(kProtocolsAtom, msg.message_type);
  EXPECT_EQ(XConnection::kLongFormat, msg.format);
  EXPECT_EQ(kPingAtom, msg.data.l[0]);
  EXPECT_EQ(kTimestamp, msg.data.l[1]);
  EXPECT_EQ(toplevel_xid, msg.data.l[2]);

  // Send a response, but with an older timestamp.  It should be ignored.
  XEvent event;
  xconn_->InitClientMessageEvent(&event, kRoot, kProtocolsAtom,
                                 kPingAtom, kTimestamp - 1, toplevel_xid, 0, 0);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(watchdog->has_outstanding_ping());

  // A response for a different window should also be ignored.
  xconn_->InitClientMessageEvent(&event, kRoot, kProtocolsAtom,
                                 kPingAtom, kTimestamp, toplevel_xid - 1, 0, 0);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(watchdog->has_outstanding_ping());

  // We should cancel the ping if we get the proper response.
  xconn_->InitClientMessageEvent(&event, kRoot, kProtocolsAtom,
                                 kPingAtom, kTimestamp, toplevel_xid, 0, 0);
  wm_->HandleEvent(&event);
  EXPECT_FALSE(watchdog->has_outstanding_ping());
  EXPECT_EQ(-1, watchdog->timeout_id_);
  EXPECT_EQ(-1, watchdog->last_killed_pid_);

  // If the timeout is invoked after we send the ping, we should kill
  // Chrome (well, not really, since this is a test, but 'last_killed_pid_'
  // should be set, at least).
  EXPECT_TRUE(watchdog->SendPingToChrome(kTimestamp, kTimeoutMs));
  watchdog->HandleTimeout();
  EXPECT_FALSE(watchdog->has_outstanding_ping());
  EXPECT_EQ(-1, watchdog->timeout_id_);
  EXPECT_EQ(kPid, watchdog->last_killed_pid_);
}

}  // namespace window_manager

int main(int argc, char** argv) {
  // Ensure that we don't really send signals to processes.
  window_manager::AutoReset<bool> flag_resetter(
      &FLAGS_kill_chrome_if_hanging, false);
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
