// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/chrome_watchdog.h"

#include <csignal>

#include <gflags/gflags.h>

#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/callback.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/event_loop.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

DEFINE_bool(kill_chrome_if_hanging, false,
            "Kill Chrome if it doesn't respond to pings sent via X");

using chromeos::WmIpcMessageType;
using window_manager::util::GetHostname;

namespace window_manager {

static const int kSignalToSend = SIGABRT;

ChromeWatchdog::ChromeWatchdog(WindowManager* wm)
    : wm_(wm),
      local_hostname_(GetHostname()),
      registrar_(new EventConsumerRegistrar(wm, this)),
      pinged_chrome_xid_(0),
      ping_timestamp_(0),
      timeout_id_(-1),
      last_killed_pid_(-1) {
}

ChromeWatchdog::~ChromeWatchdog() {
  if (has_outstanding_ping())
    AbortTimeout();
}

void ChromeWatchdog::HandleWindowMap(Window* win) {
  DCHECK(win);
  if (WmIpcWindowTypeIsChrome(win->type()) &&
      win->supports_wm_ping() &&
      win->client_hostname() == local_hostname_ &&
      is_pid_valid(win->client_pid())) {
    usable_chrome_xids_.insert(win->xid());
  }
}

void ChromeWatchdog::HandleWindowUnmap(Window* win) {
  DCHECK(win);
  usable_chrome_xids_.erase(win->xid());

  if (has_outstanding_ping() && pinged_chrome_xid_ == win->xid())
    AbortTimeout();
}

void ChromeWatchdog::HandleClientMessage(XWindow xid,
                                         XAtom message_type,
                                         const long data[5]) {
  DCHECK_EQ(xid, wm_->root());
  if (message_type != wm_->GetXAtom(ATOM_WM_PROTOCOLS) ||
      static_cast<XAtom>(data[0]) != wm_->GetXAtom(ATOM_NET_WM_PING) ||
      static_cast<XTime>(data[1]) != ping_timestamp_ ||
      static_cast<XWindow>(data[2]) != pinged_chrome_xid_) {
    return;
  }

  AbortTimeout();
}

bool ChromeWatchdog::SendPingToChrome(XTime timestamp, int timeout_ms) {
  if (has_outstanding_ping()) {
    LOG(ERROR) << "Got request to send ping while previous ping is still "
               << "outstanding; abandoning previous ping";
    AbortTimeout();
  }

  if (usable_chrome_xids_.empty())
    return false;

  XWindow xid = *(usable_chrome_xids_.begin());
  if (!wm_->GetWindowOrDie(xid)->SendPing(timestamp))
    return false;

  DCHECK_LT(timeout_id_, 0);
  timeout_id_ = wm_->event_loop()->AddTimeout(
      NewPermanentCallback(this, &ChromeWatchdog::HandleTimeout),
      timeout_ms, 0);

  pinged_chrome_xid_ = xid;
  ping_timestamp_ = timestamp;
  registrar_->RegisterForWindowEvents(wm_->root());
  return true;
}

void ChromeWatchdog::AbortTimeout() {
  if (!has_outstanding_ping())
    return;

  registrar_->UnregisterForWindowEvents(wm_->root());
  pinged_chrome_xid_ = 0;
  ping_timestamp_ = 0;
  wm_->event_loop()->RemoveTimeout(timeout_id_);
  timeout_id_ = -1;
}

void ChromeWatchdog::HandleTimeout() {
  DCHECK(has_outstanding_ping());
  registrar_->UnregisterForWindowEvents(wm_->root());
  ping_timestamp_ = 0;
  timeout_id_ = -1;

  Window* win = wm_->GetWindowOrDie(pinged_chrome_xid_);
  pid_t chrome_pid = static_cast<pid_t>(win->client_pid());

  LOG(INFO) << "Chrome window " << win->xid_str() << " didn't respond to ping; "
            << (!FLAGS_kill_chrome_if_hanging ? "(not really) " : "")
            << "sending signal " << kSignalToSend << " to PID " << chrome_pid;
  if (FLAGS_kill_chrome_if_hanging && is_pid_valid(chrome_pid)) {
    if (kill(chrome_pid, kSignalToSend) != 0)
      PLOG(ERROR) << "Unable to kill Chrome PID " << chrome_pid;
  }

  last_killed_pid_ = chrome_pid;
  pinged_chrome_xid_ = 0;
}

}  // namespace window_manager
