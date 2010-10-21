// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/screen_locker_handler.h"

#include <cmath>
#include <tr1/unordered_set>

#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/callback.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/event_loop.h"
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

// How long should we take to scale the snapshot of the screen down to a
// slightly-smaller size once the user starts holding the power button?
static const int kSlowCloseAnimMs = 400;

// How long should we take to scale the snapshot of the screen back to its
// original size when the button is released?
static const int kUndoSlowCloseAnimMs = 100;

// How long should we take to scale the snapshot down to a point in the
// center of the screen once the screen has been locked or we've been
// notified that the system is shutting down?
static const int kFastCloseAnimMs = 150;

// How long should we take to fade the screen locker window in in the
// background once the screen has been locked?
static const int kScreenLockerFadeInMs = 50;

// How long we'll wait for another message after we enter the pre-lock or
// pre-shutdown state before giving up and reverting back to the previous
// state.  This is just here as backup so we don't get stuck showing the
// snapshot onscreen forever if the power manager dies or something.
static const int kAbortAnimationMs = 2000;

const float ScreenLockerHandler::kSlowCloseSizeRatio = 0.95;

ScreenLockerHandler::ScreenLockerHandler(WindowManager* wm)
    : wm_(wm),
      registrar_(new EventConsumerRegistrar(wm_, this)),
      snapshot_pixmap_(0),
      destroy_snapshot_timeout_id_(-1),
      is_locked_(false),
      shutting_down_(false) {
  registrar_->RegisterForChromeMessages(
      chromeos::WM_IPC_MESSAGE_WM_NOTIFY_POWER_BUTTON_STATE);
  registrar_->RegisterForChromeMessages(
      chromeos::WM_IPC_MESSAGE_WM_NOTIFY_SHUTTING_DOWN);
}

ScreenLockerHandler::~ScreenLockerHandler() {
  if (is_locked_)
    wm_->compositor()->ResetActiveVisibilityGroups();

  wm_->event_loop()->RemoveTimeoutIfSet(&destroy_snapshot_timeout_id_);
  if (snapshot_pixmap_)
    wm_->xconn()->FreePixmap(snapshot_pixmap_);
}

void ScreenLockerHandler::HandleScreenResize() {
  for (set<XWindow>::const_iterator it = screen_locker_xids_.begin();
       it != screen_locker_xids_.end(); ++it) {
    Window* win = wm_->GetWindowOrDie(*it);
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

  registrar_->RegisterForWindowEvents(win->xid());

  if (!is_locked_)
    win->SetCompositedOpacity(0, 0);
  win->ShowComposited();
  win->actor()->AddToVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);

  screen_locker_xids_.insert(win->xid());
  if (!is_locked_ && HasWindowWithInitialPixmap())
    HandleLocked();
}

void ScreenLockerHandler::HandleWindowUnmap(Window* win) {
  DCHECK(win);
  if (!screen_locker_xids_.count(win->xid()))
    return;

  registrar_->UnregisterForWindowEvents(win->xid());

  win->actor()->RemoveFromVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);
  screen_locker_xids_.erase(win->xid());

  if (is_locked_ && !HasWindowWithInitialPixmap())
    HandleUnlocked();
}

void ScreenLockerHandler::HandleWindowInitialPixmap(Window* win) {
  if (!is_locked_ && HasWindowWithInitialPixmap())
    HandleLocked();
}

void ScreenLockerHandler::HandleChromeMessage(const WmIpc::Message& msg) {
  if (msg.type() == chromeos::WM_IPC_MESSAGE_WM_NOTIFY_POWER_BUTTON_STATE) {
    chromeos::WmIpcPowerButtonState state =
        static_cast<chromeos::WmIpcPowerButtonState>(msg.param(0));
    switch (state) {
      case chromeos::WM_IPC_POWER_BUTTON_PRE_LOCK:
        HandlePreLock();
        break;
      case chromeos::WM_IPC_POWER_BUTTON_ABORTED_LOCK:
        HandleAbortedLock();
        break;
      case chromeos::WM_IPC_POWER_BUTTON_PRE_SHUTDOWN:
        HandlePreShutdown();
        break;
      case chromeos::WM_IPC_POWER_BUTTON_ABORTED_SHUTDOWN:
        HandleAbortedShutdown();
        break;
      default:
        LOG(ERROR) << "Unexpected state in power button state message: "
                   << state;
    }
  } else if (msg.type() == chromeos::WM_IPC_MESSAGE_WM_NOTIFY_SHUTTING_DOWN) {
    HandleShuttingDown();
  } else {
    NOTREACHED() << "Received unwanted Chrome message "
                 << chromeos::WmIpcMessageTypeToString(msg.type());
  }
}

bool ScreenLockerHandler::HasWindowWithInitialPixmap() const {
  for (set<XWindow>::const_iterator it = screen_locker_xids_.begin();
       it != screen_locker_xids_.end(); ++it) {
    if (wm_->GetWindowOrDie(*it)->has_initial_pixmap())
      return true;
  }
  return false;
}

void ScreenLockerHandler::HandlePreLock() {
  DLOG(INFO) << "Starting pre-lock animation";
  StartSlowCloseAnimation();
  wm_->compositor()->SetActiveVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);
}

void ScreenLockerHandler::HandleAbortedLock() {
  DLOG(INFO) << "Lock aborted";
  StartUndoSlowCloseAnimation();
}

void ScreenLockerHandler::HandleLocked() {
  // We should be called when the first screen locker window becomes visible.
  DCHECK(!is_locked_);
  DCHECK(HasWindowWithInitialPixmap());
  is_locked_ = true;

  DLOG(INFO) << "First screen locker window visible; hiding other windows";
  StartFastCloseAnimation(true);
  wm_->compositor()->SetActiveVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);

  // An arbitrary screen locker window.
  Window* chrome_win = NULL;

  // Make any screen locker windows quickly fade in.
  for (set<XWindow>::const_iterator it = screen_locker_xids_.begin();
       it != screen_locker_xids_.end(); ++it) {
    Window* win = wm_->GetWindowOrDie(*it);
    win->SetCompositedOpacity(1, kScreenLockerFadeInMs);
    if (!chrome_win)
      chrome_win = win;
  }

  // Redraw and then let Chrome know that we're ready for the system to
  // be suspended now.
  wm_->compositor()->Draw();
  DCHECK(chrome_win);
  WmIpc::Message msg(
      chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_SCREEN_REDRAWN_FOR_LOCK);
  wm_->wm_ipc()->SendMessage(chrome_win->xid(), msg);
}

void ScreenLockerHandler::HandleUnlocked() {
  DCHECK(is_locked_);
  DCHECK(!HasWindowWithInitialPixmap());
  is_locked_ = false;

  if (shutting_down_)
    return;

  DLOG(INFO) << "Last screen locker window unmapped; unhiding other windows";
  wm_->event_loop()->RemoveTimeoutIfSet(&destroy_snapshot_timeout_id_);

  // This is arguably incorrect if the user types their password on the lock
  // screen, starts holding the power button, and then hits Enter to unlock the
  // screen: we'll abort the pre-shutdown animation here.  It's not an issue in
  // practice, though: if they release the power button before we'd shut down,
  // the snapshot is already gone and the aborted-shutdown message is a no-op;
  // if they hold the power button and we start shutting down, we'll grab a new
  // snapshot for the fast-close animation.
  DestroySnapshotAndUpdateVisibilityGroup();
}

void ScreenLockerHandler::HandlePreShutdown() {
  DLOG(INFO) << "Starting pre-shutdown animation";
  if (snapshot_actor_.get()) {
    // Make sure that we'll use a new snapshot.  If the power button was
    // held since before the screen was locked, we don't want to reuse the
    // snapshot taken while the screen was unlocked.
    DestroySnapshotAndUpdateVisibilityGroup();
    wm_->compositor()->Draw();
  }
  StartSlowCloseAnimation();
  wm_->compositor()->SetActiveVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SHUTDOWN);
}

void ScreenLockerHandler::HandleAbortedShutdown() {
  DLOG(INFO) << "Shutdown aborted";
  StartUndoSlowCloseAnimation();
}

void ScreenLockerHandler::HandleShuttingDown() {
  LOG(INFO) << "System is shutting down";
  if (shutting_down_)
    return;
  shutting_down_ = true;

  XID cursor = wm_->xconn()->CreateTransparentCursor();
  wm_->xconn()->SetWindowCursor(wm_->root(), cursor);
  wm_->xconn()->GrabPointer(wm_->root(), 0, 0, cursor);
  if (cursor)
    wm_->xconn()->FreeCursor(cursor);
  wm_->xconn()->GrabKeyboard(wm_->root(), 0);

  StartFastCloseAnimation(false);
  wm_->compositor()->SetActiveVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SHUTDOWN);
}

void ScreenLockerHandler::StartSlowCloseAnimation() {
  if (!snapshot_actor_.get()) {
    DCHECK_EQ(snapshot_pixmap_, static_cast<XPixmap>(0));
    snapshot_pixmap_ = wm_->xconn()->CreatePixmap(
        wm_->root(), Size(wm_->width(), wm_->height()), wm_->root_depth());
    wm_->xconn()->CopyArea(wm_->root(),       // src
                           snapshot_pixmap_,  // dest
                           Point(0, 0),       // src_pos
                           Point(0, 0),       // dest_pos
                           Size(wm_->width(), wm_->height()));
    snapshot_actor_.reset(wm_->compositor()->CreateTexturePixmap());
    snapshot_actor_->SetPixmap(snapshot_pixmap_);
    wm_->stage()->AddActor(snapshot_actor_.get());
    wm_->stacking_manager()->StackActorAtTopOfLayer(
        snapshot_actor_.get(), StackingManager::LAYER_SCREEN_LOCKER_SNAPSHOT);
    snapshot_actor_->AddToVisibilityGroup(
        WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);
    snapshot_actor_->AddToVisibilityGroup(
        WindowManager::VISIBILITY_GROUP_SHUTDOWN);
  }

  snapshot_actor_->Move(0, 0, 0);
  snapshot_actor_->Scale(1.0, 1.0, 0);

  snapshot_actor_->Move(
      round(0.5 * (1.0 - kSlowCloseSizeRatio) * wm_->width()),
      round(0.5 * (1.0 - kSlowCloseSizeRatio) * wm_->height()),
      kSlowCloseAnimMs);
  snapshot_actor_->Scale(
      kSlowCloseSizeRatio, kSlowCloseSizeRatio, kSlowCloseAnimMs);

  wm_->event_loop()->RemoveTimeoutIfSet(&destroy_snapshot_timeout_id_);
  destroy_snapshot_timeout_id_ =
      wm_->event_loop()->AddTimeout(
          NewPermanentCallback(
              this,
              &ScreenLockerHandler::HandleDestroySnapshotTimeout),
          kAbortAnimationMs,
          0);  // recurring timeout
}

void ScreenLockerHandler::StartUndoSlowCloseAnimation() {
  if (!snapshot_actor_.get()) {
    LOG(WARNING) << "Ignoring request to undo slow-close animation when it's "
                 << "not in-progress";
    return;
  }

  snapshot_actor_->Move(0, 0, kUndoSlowCloseAnimMs);
  snapshot_actor_->Scale(1.0, 1.0, kUndoSlowCloseAnimMs);

  wm_->event_loop()->RemoveTimeoutIfSet(&destroy_snapshot_timeout_id_);
  destroy_snapshot_timeout_id_ =
      wm_->event_loop()->AddTimeout(
          NewPermanentCallback(
              this,
              &ScreenLockerHandler::HandleDestroySnapshotTimeout),
          kUndoSlowCloseAnimMs,
          0);  // recurring timeout
}

void ScreenLockerHandler::StartFastCloseAnimation(
    bool destroy_snapshot_when_done) {
  if (!snapshot_actor_.get())
    StartSlowCloseAnimation();

  DCHECK(snapshot_actor_.get());
  snapshot_actor_->Move(
      round(0.5 * wm_->width()), round(0.5 * wm_->height()), kFastCloseAnimMs);
  snapshot_actor_->Scale(0, 0, kFastCloseAnimMs);
  snapshot_actor_->SetOpacity(0, kFastCloseAnimMs);

  wm_->event_loop()->RemoveTimeoutIfSet(&destroy_snapshot_timeout_id_);
  if (destroy_snapshot_when_done) {
    destroy_snapshot_timeout_id_ =
        wm_->event_loop()->AddTimeout(
            NewPermanentCallback(
                this,
                &ScreenLockerHandler::HandleDestroySnapshotTimeout),
            kFastCloseAnimMs,
            0);  // recurring timeout
  }
}

void ScreenLockerHandler::DestroySnapshot() {
  snapshot_actor_.reset();
  wm_->xconn()->FreePixmap(snapshot_pixmap_);
  snapshot_pixmap_ = 0;
}

void ScreenLockerHandler::DestroySnapshotAndUpdateVisibilityGroup() {
  DestroySnapshot();

  // Let the real windows be visible again.
  if (is_locked_) {
    wm_->compositor()->SetActiveVisibilityGroup(
        WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);
  } else {
    wm_->compositor()->ResetActiveVisibilityGroups();
  }
}

void ScreenLockerHandler::HandleDestroySnapshotTimeout() {
  destroy_snapshot_timeout_id_ = -1;
  DestroySnapshotAndUpdateVisibilityGroup();
}

}  // namespace window_manager
