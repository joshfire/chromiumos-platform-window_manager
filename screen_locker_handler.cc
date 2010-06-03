// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/screen_locker_handler.h"

#include <tr1/unordered_set>

#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

using std::set;
using std::tr1::unordered_set;

namespace window_manager {

ScreenLockerHandler::ScreenLockerHandler(WindowManager* wm)
    : wm_(wm) {
}

ScreenLockerHandler::~ScreenLockerHandler() {
  if (is_locked()) {
    unordered_set<int> visibility_groups;
    wm_->compositor()->SetActiveVisibilityGroups(visibility_groups);
  }
}

void ScreenLockerHandler::HandleWindowMap(Window* win) {
  DCHECK(win);
  if (win->type() != chromeos::WM_IPC_WINDOW_CHROME_SCREEN_LOCKER)
    return;

  if (!win->override_redirect()) {
    LOG(WARNING) << "Got non-override-redirect screen locker window "
                 << win->xid_str();
  }

  win->ShowComposited();
  win->actor()->AddToVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);

  const bool was_locked = is_locked();
  screen_locker_xids_.insert(win->xid());

  if (!was_locked) {
    DLOG(INFO) << "First screen locker window " << win->xid_str()
               << " mapped; hiding other windows";
    unordered_set<int> visibility_groups;
    visibility_groups.insert(WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);
    wm_->compositor()->SetActiveVisibilityGroups(visibility_groups);
  }
}

void ScreenLockerHandler::HandleWindowUnmap(Window* win) {
  DCHECK(win);
  if (!screen_locker_xids_.count(win->xid()))
    return;

  win->actor()->RemoveFromVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);
  screen_locker_xids_.erase(win->xid());

  if (!is_locked()) {
    DLOG(INFO) << "Last screen locker window " << win->xid_str()
               << " unmapped; unhiding other windows";
    unordered_set<int> visibility_groups;
    wm_->compositor()->SetActiveVisibilityGroups(visibility_groups);
  }
}

}  // namespace window_manager
