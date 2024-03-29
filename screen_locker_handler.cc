// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/screen_locker_handler.h"

#include <algorithm>
#include <cmath>
#include <tr1/unordered_set>

#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"
#include "window_manager/callback.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/event_loop.h"
#include "window_manager/geometry.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"

using std::find;
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

// How long should we take to fade the screen to black when the user signs out?
static const int kSignoutAnimMs = 100;

// How long should we wait between repeated attempts to grab the pointer and
// keyboard while the session is ending?
static const int kGrabInputsTimeoutMs = 100;

const float ScreenLockerHandler::kSlowCloseSizeRatio = 0.95;

ScreenLockerHandler::ScreenLockerHandler(WindowManager* wm)
    : wm_(wm),
      registrar_(new EventConsumerRegistrar(wm_, this)),
      snapshot_pixmap_(0),
      destroy_snapshot_timeout_id_(-1),
      is_locked_(false),
      session_ending_(false),
      grab_inputs_timeout_id_(-1),
      pointer_grabbed_(false),
      keyboard_grabbed_(false),
      transparent_cursor_(0) {
  registrar_->RegisterForChromeMessages(
      chromeos::WM_IPC_MESSAGE_WM_NOTIFY_POWER_BUTTON_STATE);
  registrar_->RegisterForChromeMessages(
      chromeos::WM_IPC_MESSAGE_WM_NOTIFY_SHUTTING_DOWN);
  registrar_->RegisterForChromeMessages(
      chromeos::WM_IPC_MESSAGE_WM_NOTIFY_SIGNING_OUT);
}

ScreenLockerHandler::~ScreenLockerHandler() {
  if (is_locked_)
    wm_->compositor()->ResetActiveVisibilityGroups();

  wm_->event_loop()->RemoveTimeoutIfSet(&destroy_snapshot_timeout_id_);
  if (snapshot_pixmap_)
    wm_->xconn()->FreePixmap(snapshot_pixmap_);

  wm_->event_loop()->RemoveTimeoutIfSet(&grab_inputs_timeout_id_);
  if (transparent_cursor_) {
    wm_->xconn()->FreeCursor(transparent_cursor_);
    transparent_cursor_ = 0;
  }
}

void ScreenLockerHandler::HandleScreenResize() {
  for (set<XWindow>::const_iterator it = screen_locker_xids_.begin();
       it != screen_locker_xids_.end(); ++it) {
    Window* win = wm_->GetWindowOrDie(*it);
    // TODO: The override-redirect check can be removed once Chrome is
    // using regular windows for the screen locker.
    if (!win->override_redirect())
      win->Resize(wm_->root_size(), GRAVITY_NORTHWEST);
  }
}

bool ScreenLockerHandler::HandleWindowMapRequest(Window* win) {
  DCHECK(win);
  if (win->type() != chromeos::WM_IPC_WINDOW_CHROME_SCREEN_LOCKER)
    return false;

  win->SetVisibility(Window::VISIBILITY_SHOWN);
  win->Move(Point(0, 0), 0);
  win->Resize(wm_->root_size(), GRAVITY_NORTHWEST);
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      win,
      StackingManager::LAYER_SCREEN_LOCKER,
      StackingManager::SHADOW_DIRECTLY_BELOW_ACTOR);
  return true;
}

void ScreenLockerHandler::HandleWindowMap(Window* win) {
  DCHECK(win);
  if (win->override_redirect()) {
    // If we see an override-redirect info bubble that's asking to be displayed
    // while the screen is locked or a tooltip, add it to the screen locker
    // visibility group.
    const bool is_shown_info_bubble =
        win->type() == chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE &&
        !win->type_params().empty() &&
        win->type_params()[0];
    const bool is_tooltip =
        find(win->wm_window_type_xatoms().begin(),
             win->wm_window_type_xatoms().end(),
             wm_->GetXAtom(ATOM_NET_WM_WINDOW_TYPE_TOOLTIP)) !=
        win->wm_window_type_xatoms().end();
    if (is_tooltip || is_shown_info_bubble) {
      other_xids_to_show_while_locked_.insert(win->xid());
      win->actor()->AddToVisibilityGroup(
          WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);
      return;
    }
  }

  if (win->type() != chromeos::WM_IPC_WINDOW_CHROME_SCREEN_LOCKER)
    return;

  registrar_->RegisterForWindowEvents(win->xid());

  if (!is_locked_)
    win->SetCompositedOpacity(0, 0);
  win->actor()->AddToVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);

  screen_locker_xids_.insert(win->xid());
  if (!is_locked_ && HasWindowWithInitialPixmap())
    HandleLocked();
}

void ScreenLockerHandler::HandleWindowUnmap(Window* win) {
  DCHECK(win);
  if (other_xids_to_show_while_locked_.count(win->xid())) {
    win->actor()->RemoveFromVisibilityGroup(
        WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);
    other_xids_to_show_while_locked_.erase(win->xid());
    return;
  }

  if (!screen_locker_xids_.count(win->xid()))
    return;

  registrar_->UnregisterForWindowEvents(win->xid());

  win->actor()->RemoveFromVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);
  screen_locker_xids_.erase(win->xid());

  if (is_locked_ && !HasWindowWithInitialPixmap())
    HandleUnlocked();
}

void ScreenLockerHandler::HandleWindowPixmapFetch(Window* win) {
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
    HandleSessionEnding(true);   // shutting_down=true
  } else if (msg.type() == chromeos::WM_IPC_MESSAGE_WM_NOTIFY_SIGNING_OUT) {
    HandleSessionEnding(false);  // shutting_down=false
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

  // Only show the fast-close animation if we were already showing the
  // slow-close animation (in response to the power button being held).
  // Otherwise, the screen has probably been locked in response to the lid
  // being closed, so we want to make sure we've gotten rid of the unlocked
  // contents of the screen before we draw and tell Chrome to go ahead with
  // suspend.
  const bool do_animation = (snapshot_actor_.get() != NULL);

  DLOG(INFO) << "First screen locker window visible; hiding other windows";
  if (do_animation)
    StartFastCloseAnimation(true);
  wm_->compositor()->SetActiveVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SCREEN_LOCKER);

  // An arbitrary screen locker window.
  Window* chrome_win = NULL;

  // Make any screen locker windows quickly fade in.
  for (set<XWindow>::const_iterator it = screen_locker_xids_.begin();
       it != screen_locker_xids_.end(); ++it) {
    Window* win = wm_->GetWindowOrDie(*it);
    win->SetCompositedOpacity(1, do_animation ? kScreenLockerFadeInMs : 0);
    if (!chrome_win)
      chrome_win = win;
  }

  // Redraw (only if we hid the screen contents immediately) and then let
  // Chrome know that we're ready for the system to be suspended now.
  if (!do_animation)
    wm_->compositor()->ForceDraw();
  DCHECK(chrome_win);
  WmIpc::Message msg(
      chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_SCREEN_REDRAWN_FOR_LOCK);
  wm_->wm_ipc()->SendMessage(chrome_win->xid(), msg);

  // This shouldn't be necessary since Chrome grabs the pointer and keyboard on
  // behalf of the screen locker window, but some GTK+ widgets won't accept
  // input if they think that their toplevel window is inactive due to
  // _NET_WM_ACTIVE_WINDOW not being updated.
  wm_->FocusWindow(chrome_win, wm_->GetCurrentTimeFromServer());
}

void ScreenLockerHandler::HandleUnlocked() {
  DCHECK(is_locked_);
  DCHECK(!HasWindowWithInitialPixmap());
  is_locked_ = false;

  if (session_ending_)
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
    wm_->compositor()->ForceDraw();
  }
  StartSlowCloseAnimation();
  wm_->compositor()->SetActiveVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SESSION_ENDING);
}

void ScreenLockerHandler::HandleAbortedShutdown() {
  DLOG(INFO) << "Shutdown aborted";
  StartUndoSlowCloseAnimation();
}

void ScreenLockerHandler::HandleSessionEnding(bool shutting_down) {
  if (shutting_down)
    LOG(INFO) << "System is shutting down";
  else
    LOG(INFO) << "User is signing out";

  if (session_ending_)
    return;
  session_ending_ = true;

  transparent_cursor_ = wm_->xconn()->CreateTransparentCursor();
  wm_->xconn()->SetWindowCursor(wm_->root(), transparent_cursor_);

  TryToGrabInputs();
  if (!pointer_grabbed_ || !keyboard_grabbed_) {
    grab_inputs_timeout_id_ =
        wm_->event_loop()->AddTimeout(
            NewPermanentCallback(this, &ScreenLockerHandler::TryToGrabInputs),
            kGrabInputsTimeoutMs,   // initial timeout
            kGrabInputsTimeoutMs);  // recurring timeout
  }

  if (shutting_down)
    StartFastCloseAnimation(false);
  else
    StartFadeoutAnimation();
  wm_->compositor()->SetActiveVisibilityGroup(
      WindowManager::VISIBILITY_GROUP_SESSION_ENDING);
}

void ScreenLockerHandler::TryToGrabInputs() {
  DCHECK_NE(transparent_cursor_, static_cast<XID>(0));

  if (!pointer_grabbed_ || !keyboard_grabbed_) {
    XTime now = wm_->GetCurrentTimeFromServer();
    if (!pointer_grabbed_) {
      if (wm_->xconn()->GrabPointer(wm_->root(), 0, now, transparent_cursor_))
        pointer_grabbed_ = true;
    }
    if (!keyboard_grabbed_) {
      if (wm_->xconn()->GrabKeyboard(wm_->root(), now))
        keyboard_grabbed_ = true;
    }
  }

  // If both are grabbed, we don't need to be called again.
  if (pointer_grabbed_ && keyboard_grabbed_)
    wm_->event_loop()->RemoveTimeoutIfSet(&grab_inputs_timeout_id_);
}

void ScreenLockerHandler::SetUpSnapshot() {
  if (snapshot_actor_.get())
    return;

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
      WindowManager::VISIBILITY_GROUP_SESSION_ENDING);
  snapshot_actor_->Move(0, 0, 0);
  snapshot_actor_->Scale(1.0, 1.0, 0);
}

void ScreenLockerHandler::StartSlowCloseAnimation() {
  if (!snapshot_actor_.get()) {
    SetUpSnapshot();
  } else {
    snapshot_actor_->Move(0, 0, 0);
    snapshot_actor_->Scale(1.0, 1.0, 0);
  }

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
    SetUpSnapshot();

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

void ScreenLockerHandler::StartFadeoutAnimation() {
  if (!snapshot_actor_.get()) {
    SetUpSnapshot();
  } else {
    snapshot_actor_->Move(0, 0, 0);
    snapshot_actor_->Scale(1.0, 1.0, 0);
  }
  snapshot_actor_->SetOpacity(0, kSignoutAnimMs);
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
