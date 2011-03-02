// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_CHROME_WATCHDOG_H_
#define WINDOW_MANAGER_CHROME_WATCHDOG_H_

#include <set>

#include <sys/types.h>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/scoped_ptr.h"
#include "window_manager/event_consumer.h"
#include "window_manager/x11/x_types.h"

namespace window_manager {

class EventConsumerRegistrar;
class WindowManager;
class Window;

// ChromeWatchdog sends _NET_WM_PING client messages to Chrome windows and
// kills them if they don't respond soon enough.
class ChromeWatchdog : public EventConsumer {
 public:
  explicit ChromeWatchdog(WindowManager* wm);
  virtual ~ChromeWatchdog();

  // Begin EventConsumer implementation.
  virtual bool IsInputWindow(XWindow xid) { return false; }
  virtual void HandleScreenResize() {}
  virtual void HandleLoggedInStateChange() {}
  virtual bool HandleWindowMapRequest(Window* win) { return false; }
  virtual void HandleWindowMap(Window* win);
  virtual void HandleWindowUnmap(Window* win);
  virtual void HandleWindowInitialPixmap(Window* win) {}
  virtual void HandleWindowConfigureRequest(Window* win,
                                            int req_x, int req_y,
                                            int req_width, int req_height) {}
  virtual void HandleButtonPress(XWindow xid,
                                 int x, int y,
                                 int x_root, int y_root,
                                 int button,
                                 XTime timestamp) {}
  virtual void HandleButtonRelease(XWindow xid,
                                   int x, int y,
                                   int x_root, int y_root,
                                   int button,
                                   XTime timestamp) {}
  virtual void HandlePointerEnter(XWindow xid,
                                  int x, int y,
                                  int x_root, int y_root,
                                  XTime timestamp) {}
  virtual void HandlePointerLeave(XWindow xid,
                                  int x, int y,
                                  int x_root, int y_root,
                                  XTime timestamp) {}
  virtual void HandlePointerMotion(XWindow xid,
                                   int x, int y,
                                   int x_root, int y_root,
                                   XTime timestamp) {}
  virtual void HandleChromeMessage(const WmIpc::Message& msg) {}
  virtual void HandleClientMessage(XWindow xid,
                                   XAtom message_type,
                                   const long data[5]);
  virtual void HandleWindowPropertyChange(XWindow xid, XAtom xatom) {}
  virtual void OwnDestroyedWindow(DestroyedWindow* destroyed_win, XWindow xid) {
    NOTREACHED();
  }
  // End EventConsumer implementation.

  // Send a _NET_WM_PING client message event to a Chrome window.  If
  // there's an outstanding ping, we abort it first.
  bool SendPingToChrome(XTime timestamp, int timeout_ms);

 private:
  FRIEND_TEST(ChromeWatchdogTest, Basic);

  bool is_pid_valid(pid_t pid) const { return pid > 1; }

  bool has_outstanding_ping() const { return pinged_chrome_xid_ != 0; }

  // If we have an outstanding timeout, abort it.  We cancel the timeout
  // and clear the related fields.
  void AbortTimeout();

  // Handle the timeout firing, meaning that Chrome didn't respond to our
  // ping in time.
  void HandleTimeout();

  WindowManager* wm_;  // not owned

  // Our machine's hostname.
  std::string local_hostname_;

  scoped_ptr<EventConsumerRegistrar> registrar_;

  // IDs of all currently-mapped Chrome windows that we can ping (i.e. ones
  // that support the _NET_WM_PING protocol, and were created by clients
  // that are running on the local machine and have supplied their PIDs).
  std::set<XWindow> usable_chrome_xids_;

  // The Chrome window to which an outstanding ping has been sent, or 0 if
  // we're not currently waiting for a reply.
  XWindow pinged_chrome_xid_;

  // Timestamp in the last ping that we sent.  We expect to receive this in
  // the reply.
  XTime ping_timestamp_;

  // ID of the timeout that will run HandleTimeout(), or -1 if no timeout
  // is currently registered.
  int timeout_id_;

  // PID of the last process that we killed, or -1 if we've never killed a
  // process.  Used for testing.
  pid_t last_killed_pid_;

  DISALLOW_COPY_AND_ASSIGN(ChromeWatchdog);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_CHROME_WATCHDOG_H_
