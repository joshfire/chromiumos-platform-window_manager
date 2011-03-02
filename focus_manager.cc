// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/focus_manager.h"

#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x11/x_connection.h"

using std::set;

namespace window_manager {

FocusManager::FocusManager(WindowManager* wm)
    : wm_(wm),
      focused_win_(NULL),
      last_focus_timestamp_(0) {
  DCHECK(wm_);
}

FocusManager::~FocusManager() {
}

void FocusManager::FocusWindow(Window* win, XTime timestamp) {
  if (win == focused_win_)
    return;

  if (timestamp < last_focus_timestamp_) {
    DLOG(INFO) << "Timestamp for focusing " << (win ? win->xid_str() : "root")
               << " (" << timestamp << ") precedes the last timestamp "
               << "used for focusing (" << last_focus_timestamp_ << "); "
               << "reusing the last timestamp instead";
    timestamp = last_focus_timestamp_;
  } else {
    last_focus_timestamp_ = timestamp;
  }

  if (focused_win_ && click_to_focus_windows_.count(focused_win_))
    focused_win_->AddButtonGrab();

  focused_win_ = win;
  if (focused_win_) {
    focused_win_->TakeFocus(timestamp);
    if (click_to_focus_windows_.count(focused_win_))
      focused_win_->RemoveButtonGrab();
  } else {
    wm_->xconn()->FocusWindow(wm_->xconn()->GetRootWindow(), timestamp);
  }

  wm_->SetActiveWindowProperty(focused_win_ ? focused_win_->xid() : 0);

  for (set<FocusChangeListener*>::const_iterator it =
         focus_change_listeners_.begin();
       it != focus_change_listeners_.end(); ++it) {
    (*it)->HandleFocusChange();
  }
}

void FocusManager::UseClickToFocusForWindow(Window* win) {
  DCHECK(win);
  if (!click_to_focus_windows_.insert(win).second)
    return;

  if (focused_win_ != win)
    win->AddButtonGrab();
}

void FocusManager::HandleWindowUnmap(Window* win) {
  DCHECK(win);
  set<Window*>::iterator it = click_to_focus_windows_.find(win);
  if (it != click_to_focus_windows_.end()) {
    win->RemoveButtonGrab();
    click_to_focus_windows_.erase(it);
  }

  if (focused_win_ == win)
    FocusWindow(NULL, wm_->GetCurrentTimeFromServer());
}

void FocusManager::HandleButtonPressInWindow(Window* win, XTime timestamp) {
  DCHECK(win);
  if (click_to_focus_windows_.count(win)) {
    bool replay_events = !focused_win_ || !focused_win_->wm_state_modal();
    wm_->xconn()->UngrabPointer(replay_events, timestamp);
  }
}

void FocusManager::RegisterFocusChangeListener(
    FocusChangeListener* listener) {
  DCHECK(listener);
  bool added = focus_change_listeners_.insert(listener).second;
  DCHECK(added) << "Listener " << listener << " was already registered";
}

void FocusManager::UnregisterFocusChangeListener(
    FocusChangeListener* listener) {
  int num_removed = focus_change_listeners_.erase(listener);
  DCHECK_EQ(num_removed, 1) << "Listener " << listener << " wasn't registered";
}

}  // namespace window_manager
