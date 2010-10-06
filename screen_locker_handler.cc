// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/screen_locker_handler.h"

#include <tr1/unordered_set>

#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/geometry.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"

using std::set;
using std::tr1::unordered_set;
using window_manager::util::XidStr;

namespace window_manager {

ScreenLockerHandler::ScreenLockerHandler(WindowManager* wm)
    : wm_(wm) {
}

ScreenLockerHandler::~ScreenLockerHandler() {
  if (is_locked())
    wm_->compositor()->ResetActiveVisibilityGroups();
}

void ScreenLockerHandler::HandleScreenResize() {
  for (set<XWindow>::const_iterator it = screen_locker_xids_.begin();
       it != screen_locker_xids_.end(); ++it) {
    Window* win = wm_->GetWindow(*it);
    DCHECK(win) << "Window " << XidStr(*it) << " is missing";
    // TODO: The override-redirect check can be removed once Chrome is
    // using regular windows for the screen locker.
    if (!win->override_redirect())
      win->ResizeClient(wm_->width(), wm_->height(), GRAVITY_NORTHWEST);
  }
}

bool ScreenLockerHandler::HandleWindowMapRequest(Window* win) {
  DCHECK(win);
  if (win->type() != chromeos::WM_IPC_WINDOW_CHROME_SCREEN_LOCKER)
    return false;

  win->MoveClient(0, 0);
  win->MoveCompositedToClient();
  win->ResizeClient(wm_->width(), wm_->height(), GRAVITY_NORTHWEST);
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      win, StackingManager::LAYER_SCREEN_LOCKER);
  win->MapClient();
  return true;
}

void ScreenLockerHandler::HandleWindowMap(Window* win) {
  DCHECK(win);
  if (win->type() != chromeos::WM_IPC_WINDOW_CHROME_SCREEN_LOCKER)
    return;

  win->ShowComposited();
  win->actor()->AddToVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);

  const bool was_locked = is_locked();
  screen_locker_xids_.insert(win->xid());

  if (!was_locked) {
    DLOG(INFO) << "First screen locker window " << win->xid_str()
               << " mapped; hiding other windows";
    wm_->compositor()->SetActiveVisibilityGroup(
        WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);

    // Redraw and then let Chrome know that we're ready for the system to
    // be suspended now.
    wm_->compositor()->Draw();
    WmIpc::Message msg(
        chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_SCREEN_REDRAWN_FOR_LOCK);
    wm_->wm_ipc()->SendMessage(win->xid(), msg);
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
    wm_->compositor()->ResetActiveVisibilityGroups();
  }
}

}  // namespace window_manager
