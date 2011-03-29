// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/login/login_controller.h"

#include <map>
#include <string>
#include <tr1/memory>

#include <gflags/gflags.h>

#include "chromeos/dbus/service_constants.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/callback.h"
#include "window_manager/dbus_interface.h"
#include "window_manager/event_loop.h"
#include "window_manager/focus_manager.h"
#include "window_manager/geometry.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

DEFINE_string(calibrate_display_command,
              "", "Command to run to calibrate the display while we're "
              "transitioning to the login background");

using std::map;
using std::set;
using std::string;
using std::tr1::shared_ptr;
using std::vector;
using window_manager::util::XidStr;

namespace window_manager {

// Time for the animations.
static const int kAnimationTimeInMs = 200;

// Time for the initial show animation.
static const int kInitialShowAnimationTimeInMs = 200;

// Amount of time to take for animations when transitioning from the
// logged-out state to the logged-in state.
static const int kLoggedInTransitionAnimMs = 100;

// Used when nothing is selected.  The underflow is intentional; we just
// want a big number that we'll never see for a valid selection.
static const size_t kNoSelection = static_cast<size_t>(-1);

static const int kNoTimer = -1;

// Action names for navigating across user windows.
static const char kSelectLeftAction[] = "login-select-left";
static const char kSelectRightAction[] = "login-select-right";

// Macro that returns from current function if the specified window doesn't
// have a parameter identifying the index.
#define FAIL_IF_INDEX_MISSING(win, type)                                       \
  if (LoginEntry::GetUserIndex(win) == kNoSelection) {                         \
    LOG(WARNING) << "index missing for window " << win->xid_str() <<           \
        " of type " << type;                                                   \
    return;                                                                    \
  }

LoginController::SelectionChangedManager::SelectionChangedManager(
    LoginController* layout)
    : layout_(layout),
      timeout_id_(kNoTimer),
      selected_index_(kNoSelection) {
}

LoginController::SelectionChangedManager::~SelectionChangedManager() {
  Stop();
}

void LoginController::SelectionChangedManager::Schedule(size_t selected_index) {
  if (timeout_id_ != kNoTimer)
    Run();

  selected_index_ = selected_index;

  // TODO: this is really the wrong place for this. Instead we need a way to
  // know when the animation completes.
  timeout_id_ = layout_->wm_->event_loop()->AddTimeout(
      NewPermanentCallback(this, &SelectionChangedManager::Run),
      kAnimationTimeInMs,
      0);
}

void LoginController::SelectionChangedManager::Stop() {
  if (timeout_id_ != kNoTimer) {
    layout_->wm_->event_loop()->RemoveTimeout(timeout_id_);
    timeout_id_ = kNoTimer;
  }
}

void LoginController::SelectionChangedManager::Run() {
  Stop();
  layout_->ProcessSelectionChangeCompleted(selected_index_);
}

LoginController::LoginController(WindowManager* wm)
    : wm_(wm),
      registrar_(wm, this),
      all_windows_are_ready_(false),
      selected_entry_index_(kNoSelection),
      selection_changed_manager_(this),
      wizard_window_(NULL),
      background_window_(NULL),
      login_window_to_focus_(NULL),
      waiting_for_browser_window_(false),
      requested_destruction_(false),
      is_entry_selection_enabled_(true),
      last_inserted_entry_(kNoSelection),
      hide_mouse_cursor_xid_(0) {
  registrar_.RegisterForChromeMessages(
      chromeos::WM_IPC_MESSAGE_WM_SET_LOGIN_STATE);
  registrar_.RegisterForChromeMessages(
      chromeos::WM_IPC_MESSAGE_WM_SELECT_LOGIN_USER);

#ifndef TOUCH_UI
  // Hide the mouse cursor until the user moves the pointer.  We map a
  // fullscreen input window so we can tell when they've moved it (at which
  // point we'll destroy the window immediately so other windows can receive
  // input).
  DCHECK(!wm_->logged_in());
  wm_->xconn()->HideCursor();
  hide_mouse_cursor_xid_ =
      wm_->CreateInputWindow(
          wm_->bounds(), PointerMotionMask | LeaveWindowMask | ButtonPressMask);
  registrar_.RegisterForWindowEvents(hide_mouse_cursor_xid_);
  wm_->xconn()->RaiseWindow(hide_mouse_cursor_xid_);
#endif
}

LoginController::~LoginController() {
  if (hide_mouse_cursor_xid_)
    ShowMouseCursor();
}

bool LoginController::IsInputWindow(XWindow xid) {
  return hide_mouse_cursor_xid_ && xid == hide_mouse_cursor_xid_;
}

void LoginController::HandleScreenResize() {
  if (requested_destruction_)
    return;

  if (background_window_)
    background_window_->ResizeClient(wm_->width(), wm_->height(),
                                     GRAVITY_NORTHWEST);

  if (all_windows_are_ready_ &&
      is_entry_selection_enabled_ &&
      !waiting_for_browser_window_) {
    vector<Point> origins;
    CalculateIdealOrigins(&origins);
    for (size_t i = 0; i < entries_.size(); ++i) {
      if (!entries_[i]->has_all_windows())
        continue;
      entries_[i]->UpdatePositionAndScale(
          origins[i], i == selected_entry_index_, 0);
    }
  }
}

void LoginController::HandleLoggedInStateChange() {
  if (wm_->logged_in())
    waiting_for_browser_window_ = true;
}

bool LoginController::HandleWindowMapRequest(Window* win) {
  if (requested_destruction_)
    return false;

  switch (win->type()) {
    case chromeos::WM_IPC_WINDOW_LOGIN_BACKGROUND:
    case chromeos::WM_IPC_WINDOW_LOGIN_GUEST:
    case chromeos::WM_IPC_WINDOW_LOGIN_BORDER:
    case chromeos::WM_IPC_WINDOW_LOGIN_IMAGE:
    case chromeos::WM_IPC_WINDOW_LOGIN_CONTROLS:
    case chromeos::WM_IPC_WINDOW_LOGIN_LABEL:
    case chromeos::WM_IPC_WINDOW_LOGIN_UNSELECTED_LABEL:
      // Move all client windows offscreen.  We'll move the windows that
      // need to be onscreen (just the background and controls windows) later.
      win->MoveClientOffscreen();
      return true;
    case chromeos::WM_IPC_WINDOW_UNKNOWN:
    case chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE: {
      // Only map other windows that are transient for our windows.
      if (!login_xids_.count(win->transient_for_xid()) &&
          !non_login_xids_.count(win->transient_for_xid())) {
        return false;
      }
      wm_->stacking_manager()->StackWindowAtTopOfLayer(
          win, StackingManager::LAYER_LOGIN_OTHER_WINDOW);
      return true;
    }
    default:
      return false;
  }
}

void LoginController::HandleWindowMap(Window* win) {
  if (requested_destruction_ || win->override_redirect())
    return;

  // Focus the first entry's controls window as soon as it's mapped instead
  // of waiting until all of the windows are painted.  We want to make sure
  // that we don't drop any keystrokes as the user is typing their password.
  // See http://crosbug.com/10456.
  if (win->type() == chromeos::WM_IPC_WINDOW_LOGIN_CONTROLS &&
      wm_->focus_manager()->focused_win() == NULL &&
      LoginEntry::GetUserIndex(win) == 0) {
    FocusLoginWindow(win);
    wm_->xconn()->FlushRequests();
  }

  if (waiting_for_browser_window_ &&
      win->type() == chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL) {
    if (win->has_initial_pixmap()) {
      HandleInitialBrowserWindowVisible();
      // Return immediately since the previous call will destroy us.
      return;
    } else {
      // Otherwise, we'll just wait for the window to get drawn.
      browser_xids_.insert(win->xid());
      registrar_.RegisterForWindowEvents(win->xid());
    }
    return;
  }

  switch (win->type()) {
    case chromeos::WM_IPC_WINDOW_LOGIN_GUEST: {
      if (wizard_window_)
        LOG(WARNING) << "Two wizard windows encountered";
      wizard_window_ = win;
      wm_->focus_manager()->UseClickToFocusForWindow(wizard_window_);
      registrar_.RegisterForWindowEvents(wizard_window_->xid());
      break;
    }
    case chromeos::WM_IPC_WINDOW_LOGIN_BORDER: {
      FAIL_IF_INDEX_MISSING(win, "border");
      GetEntryForWindow(win, true)->SetBorderWindow(win);
      break;
    }
    case chromeos::WM_IPC_WINDOW_LOGIN_IMAGE: {
      FAIL_IF_INDEX_MISSING(win, "image");
      GetEntryForWindow(win, true)->SetImageWindow(win);
      break;
    }
    case chromeos::WM_IPC_WINDOW_LOGIN_CONTROLS: {
      FAIL_IF_INDEX_MISSING(win, "controls");
      GetEntryForWindow(win, true)->SetControlsWindow(win);
      break;
    }
    case chromeos::WM_IPC_WINDOW_LOGIN_LABEL: {
      FAIL_IF_INDEX_MISSING(win, "label");
      GetEntryForWindow(win, true)->SetLabelWindow(win);
      break;
    }
    case chromeos::WM_IPC_WINDOW_LOGIN_UNSELECTED_LABEL: {
      FAIL_IF_INDEX_MISSING(win, "unselected label");
      GetEntryForWindow(win, true)->SetUnselectedLabelWindow(win);
      break;
    }
    case chromeos::WM_IPC_WINDOW_LOGIN_BACKGROUND: {
      if (background_window_)
        LOG(WARNING) << "Two background windows encountered";
      background_window_ = win;
      wm_->focus_manager()->UseClickToFocusForWindow(background_window_);
      registrar_.RegisterForWindowEvents(background_window_->xid());
      break;
    }
    default:
      const XWindow owner_xid = win->transient_for_xid();
      if (!login_xids_.count(owner_xid) && !non_login_xids_.count(owner_xid))
        return;
      Window* owner_win = wm_->GetWindow(owner_xid);
      DCHECK(owner_win);

      if (!non_login_xids_.insert(win->xid()).second) {
        LOG(ERROR) << "Already managing window " << win->xid_str();
        return;
      }
      registrar_.RegisterForWindowEvents(win->xid());

      // Restack the window again in case it was mapped before the
      // window manager started.
      wm_->stacking_manager()->StackWindowAtTopOfLayer(
          win, StackingManager::LAYER_LOGIN_OTHER_WINDOW);

      // Center the window over its owner (unless it's an infobubble, which
      // we just let Chrome position wherever it wants).
      if (win->type() != chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE) {
        win->CenterClientOverWindow(owner_win);
        if (!win->is_rgba())
          win->SetShadowType(Shadow::TYPE_RECTANGULAR);
      }

      wm_->focus_manager()->UseClickToFocusForWindow(win);
      wm_->FocusWindow(win, wm_->GetCurrentTimeFromServer());
      win->MoveCompositedToClient();
      win->ShowComposited();
      return;
  }

  login_xids_.insert(win->xid());
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      win, StackingManager::LAYER_LOGIN_WINDOW);

  // Register our interest in taking ownership of this window after the
  // underlying X window gets destroyed.
  if (!registered_destroyed_xids_.count(win->xid())) {
    registrar_.RegisterForDestroyedWindow(win->xid());
    registered_destroyed_xids_.insert(win->xid());
  }

  DoInitialSetupIfWindowsAreReady();

  // TODO(sky): there is a race condition here. If we die and restart with the
  // login already running we don't really know what state it was in. We need
  // Chrome to keep the current state as a parameter on one of the windows so
  // that we know what state it was in.

  // If guest entry is present and selected and wizard window is created, do
  // the animation for switching between entry and screen windows.
  if (win == wizard_window_ &&
      !entries_.empty() &&
      IsGuestEntryIndex(selected_entry_index_))
    SelectWizardWindow();
}

void LoginController::HandleWindowUnmap(Window* win) {
  if (win->override_redirect())
    return;

  // If one of the browser windows that we were waiting to get painted is
  // getting closed (maybe Chrome crashed), stop watching it.
  set<XWindow>::iterator browser_it = browser_xids_.find(win->xid());
  if (browser_it != browser_xids_.end()) {
    registrar_.UnregisterForWindowEvents(win->xid());
    browser_xids_.erase(browser_it);
    return;
  }

  set<XWindow>::iterator non_login_it = non_login_xids_.find(win->xid());
  if (non_login_it != non_login_xids_.end()) {
    win->HideComposited();
    non_login_xids_.erase(*non_login_it);
    registrar_.UnregisterForWindowEvents(win->xid());

    if (win->IsFocused() && !wm_->logged_in()) {
      // If the window was transient, pass the focus to its owner (as long
      // as it's not the background window, which we never want to receive
      // the focus); otherwise just focus the previously-focused login
      // window.
      Window* owner_win = win->transient_for_xid() ?
          wm_->GetWindow(win->transient_for_xid()) : NULL;
      if (owner_win && owner_win->mapped() && owner_win != background_window_) {
        wm_->FocusWindow(owner_win, wm_->GetCurrentTimeFromServer());
      } else if (login_window_to_focus_) {
        wm_->FocusWindow(login_window_to_focus_,
                         wm_->GetCurrentTimeFromServer());
      }
    }
    return;
  }

  if (!IsLoginWindow(win))
    return;

  if (win == background_window_) {
    registrar_.UnregisterForWindowEvents(background_window_->xid());
    background_window_ = NULL;
  } else if (win == wizard_window_) {
    registrar_.UnregisterForWindowEvents(wizard_window_->xid());
    wizard_window_ = NULL;
  } else {
    for (Entries::iterator it = entries_.begin(); it < entries_.end(); ++it) {
      if ((*it)->HandleWindowUnmap(win)) {
        all_windows_are_ready_ = false;
        if ((*it)->has_no_windows()) {
          size_t deleted_index = it - entries_.begin();
          size_t active_index = selected_entry_index_;
          selected_entry_index_ = kNoSelection;
          last_inserted_entry_ = kNoSelection;
          entries_.erase(it);
          if (!wizard_window_ && !entries_.empty()) {
            // In case only one user pod was removed we should reset our state
            // to ready.
            all_windows_are_ready_ = AllWindowsAreReady();

            // Need to activate next entry only if all remaining entries are
            // ready. If Chrome crashes or destroys all windows one-by-one
            // we don't need to switch active entries.
            if (!all_windows_are_ready_)
              break;

            // Update other entries positions on screen.
            if (deleted_index < active_index ||
                active_index == entries_.size() ||
                (deleted_index == active_index &&
                 IsGuestEntryIndex(active_index) &&
                 entries_.size() > 1)) {
              // We need to decrement active_index in 3 cases:
              // 1. removed entry was located prior to active entry,
              //    decrement is needed to preserve the same selected entry
              // 2. removed entry was last entry so new active entry will be
              //    previous one
              // 3. if selected entry was unmapped and next entry is a guest,
              //    select previous one to avoid undesired guest activation
              --active_index;
            }
            DCHECK_LT(active_index, entries_.size());
            SelectEntryAt(active_index);
          }
        }
        // Only one entry can possibly contain a window, no need to continue
        // through other entries.
        break;
      }
    }
  }

  login_xids_.erase(win->xid());

  if (login_window_to_focus_ == win)
    login_window_to_focus_ = NULL;
}

void LoginController::HandleWindowInitialPixmap(Window* win) {
  if (waiting_for_browser_window_ && browser_xids_.count(win->xid())) {
    HandleInitialBrowserWindowVisible();
    return;
  }

  if (!all_windows_are_ready_) {
    if (win == background_window_) {
      DoInitialSetupIfWindowsAreReady();
    } else {
      LoginEntry* entry = GetEntryForWindow(win, false);
      if (entry && entry->HasAllPixmaps())
        DoInitialSetupIfWindowsAreReady();
    }
  }
}

void LoginController::HandleWindowConfigureRequest(Window* win,
                                                   int req_x,
                                                   int req_y,
                                                   int req_width,
                                                   int req_height) {
  if (requested_destruction_)
    return;

  if (IsLoginWindow(win)) {
    // We manage the x/y, but let Chrome manage the width/height.
    win->ResizeClient(req_width, req_height, GRAVITY_NORTHWEST);
  } else if (non_login_xids_.count(win->xid())) {
    // If this is a non-login window that we're managing, just make
    // whatever changes the client asked for.
    win->MoveClient(req_x, req_y);
    win->MoveCompositedToClient();
    win->ResizeClient(req_width, req_height, GRAVITY_NORTHWEST);
  }
}

void LoginController::HandleButtonPress(XWindow xid,
                                        int x, int y,
                                        int x_root, int y_root,
                                        int button,
                                        XTime timestamp) {
  if (requested_destruction_)
    return;

  if (hide_mouse_cursor_xid_)
    ShowMouseCursor();

  // Ignore clicks if a modal window has the focus.
  if (wm_->focus_manager()->focused_win() &&
      wm_->focus_manager()->focused_win()->wm_state_modal()) {
    return;
  }

  // If we saw a click in one of the other windows, focus and raise it.
  if (non_login_xids_.count(xid)) {
    Window* win = wm_->GetWindowOrDie(xid);
    wm_->FocusWindow(win, timestamp);
    wm_->stacking_manager()->StackWindowAtTopOfLayer(
        win, StackingManager::LAYER_LOGIN_OTHER_WINDOW);
    return;
  }

  if (login_xids_.count(xid) == 0)
    return;

  // Otherwise, this was probably just some window that had a button grab
  // as a result of us calling FocusManager::UseClickToFocusForWindow().
  if (login_window_to_focus_)
    wm_->FocusWindow(login_window_to_focus_, wm_->GetCurrentTimeFromServer());
}

void LoginController::HandlePointerLeave(XWindow xid,
                                         int x, int y,
                                         int x_root, int y_root,
                                         XTime timestamp) {
  // We'll get a LeaveNotify event when the pointer is grabbed.  We need to show
  // the mouse cursor in response to this since we won't get any MotionNotify
  // events while it's grabbed.  If the user opens a menu with the keyboard
  // (causing a grab) and then tries to move the mouse, we want to make sure
  // that they can see the cursor.
  if (hide_mouse_cursor_xid_)
    ShowMouseCursor();
}

void LoginController::HandlePointerMotion(XWindow xid,
                                          int x, int y,
                                          int x_root, int y_root,
                                          XTime timestamp) {
  if (hide_mouse_cursor_xid_)
    ShowMouseCursor();
}

void LoginController::HandleChromeMessage(const WmIpc::Message& msg) {
  if (requested_destruction_)
    return;

  switch (msg.type()) {
    case chromeos::WM_IPC_MESSAGE_WM_SET_LOGIN_STATE: {
      SetEntrySelectionEnabled(msg.param(0) == 1);
      break;
    }

    case chromeos::WM_IPC_MESSAGE_WM_SELECT_LOGIN_USER: {
      if (is_entry_selection_enabled_) {
        size_t select_entry = static_cast<size_t>(msg.param(0));
        if (select_entry >= entries_.size()) {
          // Invalid index, just use some valid value instead.
          select_entry = 0;
        }
        SelectEntryAt(select_entry);
      }
      break;
    }

    default:
      break;
  }
}

void LoginController::HandleClientMessage(XWindow xid,
                                          XAtom message_type,
                                          const long data[5]) {
  if (requested_destruction_)
    return;

  Window* win = wm_->GetWindow(xid);
  if (!win)
    return;

  if (message_type == wm_->GetXAtom(ATOM_NET_WM_STATE)) {
    map<XAtom, bool> states;
    win->ParseWmStateMessage(data, &states);
    win->ChangeWmState(states);
  } else if (message_type == wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW)) {
    if (non_login_xids_.count(xid)) {
      wm_->FocusWindow(win, data[1]);
      wm_->stacking_manager()->StackWindowAtTopOfLayer(
          win, StackingManager::LAYER_LOGIN_OTHER_WINDOW);
    } else if (login_xids_.count(xid)) {
      wm_->FocusWindow(win, data[1]);
    }
  }
}

void LoginController::OwnDestroyedWindow(DestroyedWindow* destroyed_win,
                                         XWindow xid) {
  DCHECK(destroyed_win);
  // If the user has already logged in, then hang on to this destroyed
  // window so we can keep displaying it a bit longer.
  if (wm_->logged_in())
    destroyed_windows_.insert(shared_ptr<DestroyedWindow>(destroyed_win));
  else
    delete destroyed_win;

  // Let the registrar know that it no longer needs to unregister our
  // interest in this window.
  registrar_.HandleDestroyedWindow(xid);
  registered_destroyed_xids_.erase(xid);
}

void LoginController::InitialShow() {
  DCHECK(!entries_.empty());

  selected_entry_index_ = 0;

  vector<Point> origins;
  CalculateIdealOrigins(&origins);
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (!entries_[i]->has_all_windows()) {
      // Something bad has happened, for example Chrome crashed and windows are
      // being destroyed in random order, just skip this invalid entry.
      continue;
    }
    const bool is_selected = (i == selected_entry_index_);
    entries_[i]->UpdatePositionAndScale(origins[i], is_selected, 0);
    entries_[i]->FadeOut(0);
    entries_[i]->FadeIn(origins[i], is_selected, kInitialShowAnimationTimeInMs);
    if (is_selected)
      FocusLoginWindow(entries_[i]->controls_window());
  }
}

void LoginController::ConfigureBackgroundWindow() {
  // TODO: This is very much not the right place to be loading gamma settings;
  // ideally session_manager_setup.sh would load them immediately after X is
  // started.  That leads to a noticeable shift in colors, though, so we instead
  // sneak it in here, while we're also fading from the boot splash image to the
  // login background window.
  if (!FLAGS_calibrate_display_command.empty()) {
    string cmd = FLAGS_calibrate_display_command + " &";
    LOG(INFO) << "Running \"" << cmd << "\" to calibrate display";
    if (system(cmd.c_str()) != 0)
      LOG(ERROR) << "Display calibration failed";
  }

  DCHECK(background_window_);
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      background_window_, StackingManager::LAYER_LOGIN_WINDOW);
  background_window_->MoveClient(0, 0);
  background_window_->MoveCompositedToClient();
  background_window_->SetCompositedOpacity(0, 0);
  background_window_->ShowComposited();
  background_window_->SetCompositedOpacity(1, kInitialShowAnimationTimeInMs);

  // Make the window manager drop its actor containing the initial contents
  // of the root window once the login background window has faded all the
  // way in -- we don't want the initial contents to be visible again if
  // Chrome goes away because the user initiated shutdown from the login
  // screen.
  wm_->event_loop()->AddTimeout(
      NewPermanentCallback(wm_, &WindowManager::DropStartupBackground),
      kInitialShowAnimationTimeInMs, 0);
}

void LoginController::StackWindows() {
  for (Entries::iterator it = entries_.begin(); it < entries_.end(); ++it) {
    if (!(*it)->has_all_windows())
      continue;
    (*it)->StackWindows();
  }
}

void LoginController::SelectEntryAt(size_t index) {
  DLOG(INFO) << "Selecting entry with index " << index
             << ". Current selection is " << selected_entry_index_;

  if (index == selected_entry_index_)
    return;

  // Process any pending selection change.
  if (selection_changed_manager_.is_scheduled()) {
    ProcessSelectionChangeCompleted(
        selection_changed_manager_.selected_index());
    selection_changed_manager_.Stop();
  }

  const size_t last_selected_index = selected_entry_index_;

  DCHECK_LT(index, entries_.size());
  selected_entry_index_ = index;

  // Bail out before moving any entries around if we're waiting to go away.
  if (wm_->logged_in())
    return;

  vector<Point> origins;
  CalculateIdealOrigins(&origins);
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (!entries_[i]->has_all_windows())
      continue;

    if (i == selected_entry_index_) {
      DLOG(INFO) << "Calling Select for entry with index " << i;
      entries_[i]->Select(origins[i], kAnimationTimeInMs);
      FocusLoginWindow(entries_[i]->controls_window());
    } else if (i == last_selected_index) {
      DLOG(INFO) << "Calling Deselect for entry with index " << i;
      entries_[i]->Deselect(origins[i], kAnimationTimeInMs);
    } else {
      entries_[i]->UpdatePositionAndScale(origins[i], false,
                                          kAnimationTimeInMs);
    }
  }

  if (last_selected_index != kNoSelection)
    selection_changed_manager_.Schedule(last_selected_index);
}

void LoginController::SetEntrySelectionEnabled(bool enable) {
  is_entry_selection_enabled_ = enable;
}

void LoginController::SelectWizardWindow() {
  DLOG(INFO) << "Switching to wizard screen window.";
  DCHECK(wizard_window_);

  DCHECK(!entries_.empty());
  LoginEntry* guest_entry = entries_.back().get();
  DCHECK(guest_entry);
  DCHECK(guest_entry->has_all_windows());
  if (!guest_entry->has_all_windows())
    return;

  // Move the wizard window to its original location of guest border.
  // TODO(dpolukhin): create GuestEntry class to encapsulate guest animation.
  const int guest_width = wizard_window_->client_width();
  const int guest_height = wizard_window_->client_height();
  const float x_scale = (static_cast<float>(guest_entry->selected_width()) /
                         static_cast<float>(guest_width));
  const float y_scale = (static_cast<float>(guest_entry->selected_height()) /
                         static_cast<float>(guest_height));
  wizard_window_->ScaleComposited(x_scale, y_scale, 0);
  wizard_window_->SetCompositedOpacity(0, 0);
  wizard_window_->MoveComposited(guest_entry->border_window()->composited_x(),
                                guest_entry->border_window()->composited_y(),
                                0);
  wizard_window_->StackCompositedBelow(guest_entry->border_window()->actor(),
                                      NULL, true);
  wizard_window_->StackClientBelow(guest_entry->border_window()->xid());
  wizard_window_->ShowComposited();

  // Move the wizard window to its target location and focus it.
  wizard_window_->ScaleComposited(1, 1, kAnimationTimeInMs);
  wizard_window_->SetCompositedOpacity(1, kAnimationTimeInMs);
  wizard_window_->MoveComposited((wm_->width() - guest_width) / 2,
                                 (wm_->height() - guest_height) / 2,
                                 kAnimationTimeInMs);
  wizard_window_->MoveClientToComposited();
  FocusLoginWindow(wizard_window_);

  for (Entries::iterator it = entries_.begin(); it < entries_.end(); ++it) {
    if (!(*it)->has_all_windows())
      continue;

    (*it)->FadeOut(kAnimationTimeInMs);
  }
}

void LoginController::CalculateIdealOrigins(vector<Point>* origins) {
  const LoginEntry* entry = entries_[0].get();
  const int screen_height = wm_->height();
  const int padding = entry->padding();

  int width = 0;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (selected_entry_index_ == i)
      width += entries_[i]->selected_width();
    else
      width += entries_[i]->unselected_width();
  }
  // Take padding from first entry, later we perhaps may want to take average
  // or max padding of two adjacent entries. Now all entries have the same
  // padding so it doesn't matter.
  width += (entries_.size() - 1) * padding;

  int x = (wm_->width() - width) / 2;
  for (size_t i = 0; i < entries_.size(); ++i) {
    int y;
    int w;
    if (selected_entry_index_ == i) {
      y = (screen_height - entries_[i]->selected_height()) / 2;
      w = entries_[i]->selected_width();
    } else {
      y = (screen_height - entries_[i]->unselected_height()) / 2;
      w = entries_[i]->unselected_width();
    }
    origins->push_back(Point(x, y));
    x += w + padding;
  }
}

bool LoginController::IsLoginWindow(Window* window) const {
  return login_xids_.count(window->xid()) > 0;
}

bool LoginController::IsGuestEntryIndex(size_t index) const {
  return index + 1 == entries_.size();
}

LoginEntry* LoginController::GetEntryForWindow(Window* win,
                                               bool possibly_insert) {
  size_t entry_index = LoginEntry::GetUserIndex(win);
  if (entry_index == kNoSelection)
    return NULL;

  LoginEntry* entry = GetEntryAt(entry_index);
  if (possibly_insert && entry && entry->has_all_windows() &&
      entry->GetUserIndex(entry->border_window()) != entry_index) {
    if (entry->GetUserIndex(entry->border_window()) != entry_index + 1) {
      LOG(WARNING) << "Invalid login entry index " << entry_index
                   << " in window " << win->xid_str();
    }
    entry = new LoginEntry(wm_, &registrar_);
    entries_.insert(entries_.begin() + entry_index, Entries::value_type(entry));
    last_inserted_entry_ = entry_index;
    all_windows_are_ready_ = false;
    if (selected_entry_index_ != kNoSelection &&
        entry_index <= selected_entry_index_)
      selected_entry_index_++;
  }

  return entry;
}

LoginEntry* LoginController::GetEntryAt(size_t index) {
  while (entries_.size() <= index) {
    entries_.push_back(Entries::value_type(new LoginEntry(wm_, &registrar_)));
    all_windows_are_ready_ = false;
  }
  return entries_[index].get();
}

void LoginController::ProcessSelectionChangeCompleted(
    size_t last_selected_index) {
  DLOG(INFO) << "Selection change completed. Last selected entry: "
             << last_selected_index << ". New selected entry: "
             << selected_entry_index_;
  if (last_selected_index >= entries_.size())
    return;

  if (last_selected_index != selected_entry_index_ &&
      entries_[last_selected_index]->has_all_windows())
    entries_[last_selected_index]->ProcessSelectionChangeCompleted(false);

  if (selected_entry_index_ != kNoSelection &&
      entries_[selected_entry_index_]->has_all_windows()) {
    entries_[selected_entry_index_]->ProcessSelectionChangeCompleted(true);
  }
}

bool LoginController::AllWindowsAreReady() {
  if (!IsBackgroundWindowReady())
    return false;

  if (entries_.empty() || entries_[0]->GetUserCount() != entries_.size())
    return false;

  for (Entries::const_iterator it = entries_.begin(); it != entries_.end();
       ++it) {
    if (!(*it)->HasAllPixmaps())
      return false;
  }

  return true;
}

void LoginController::DoInitialSetupIfWindowsAreReady() {
  // Bail if we already handled this.
  if (all_windows_are_ready_)
    return;

  if (AllWindowsAreReady()) {
    all_windows_are_ready_ = true;
    if (last_inserted_entry_ == kNoSelection) {
      ConfigureBackgroundWindow();
      StackWindows();
      InitialShow();
      NotifySessionManager();
    } else {
      // Not an initial show, all windows except for last_inserted_entry_ are
      // visible already so just show the entry and update all positions.
      entries_[last_inserted_entry_]->StackWindows();

      vector<Point> origins;
      CalculateIdealOrigins(&origins);
      origins[last_inserted_entry_].y = wm_->height();
      entries_[last_inserted_entry_]->UpdatePositionAndScale(
          origins[last_inserted_entry_], false, 0);
      entries_[last_inserted_entry_]->FadeOut(0);
      entries_[last_inserted_entry_]->FadeIn(origins[last_inserted_entry_],
          false, kInitialShowAnimationTimeInMs);

      size_t prev_selected_index = selected_entry_index_;
      selected_entry_index_ = kNoSelection;
      last_inserted_entry_ = kNoSelection;
      SelectEntryAt(prev_selected_index);
    }
  } else if (entries_.empty() && wizard_window_ && IsBackgroundWindowReady()) {
    // If we're running an older version of Chrome (param[0] is missing) or
    // this is the first time that the wizard window has been mapped
    // (param[0] is non-zero), make the background window fade in.
    // Otherwise, the background should have already been configured previously.
    if (wizard_window_->type_params().empty() ||
        wizard_window_->type_params()[0]) {
      ConfigureBackgroundWindow();
    }

    wizard_window_->MoveClient(
        (wm_->width() - wizard_window_->client_width()) / 2,
        (wm_->height() - wizard_window_->client_height()) / 2);
    wizard_window_->MoveCompositedToClient();
    wm_->stacking_manager()->StackWindowAtTopOfLayer(
        wizard_window_, StackingManager::LAYER_LOGIN_WINDOW);
    wizard_window_->SetCompositedOpacity(0, 0);
    wizard_window_->ShowComposited();
    wizard_window_->SetCompositedOpacity(1, kInitialShowAnimationTimeInMs);
    FocusLoginWindow(wizard_window_);
    NotifySessionManager();
  }
}

bool LoginController::IsBackgroundWindowReady() {
  return background_window_ && background_window_->has_initial_pixmap();
}

void LoginController::FocusLoginWindow(Window* win) {
  DCHECK(win);
  wm_->FocusWindow(win, wm_->GetCurrentTimeFromServer());
  login_window_to_focus_ = win;
}

void LoginController::HandleInitialBrowserWindowVisible() {
  DCHECK(waiting_for_browser_window_);
  waiting_for_browser_window_ = false;
  if (hide_mouse_cursor_xid_)
    ShowMouseCursor();
  HideWindowsAndRequestDestruction();
}

void LoginController::ShowMouseCursor() {
  DCHECK(hide_mouse_cursor_xid_);
  wm_->xconn()->ShowCursor();
  registrar_.UnregisterForWindowEvents(hide_mouse_cursor_xid_);
  wm_->xconn()->DestroyWindow(hide_mouse_cursor_xid_);
  hide_mouse_cursor_xid_ = 0;
}

void LoginController::HideWindowsAndRequestDestruction() {
  // Move all of our client windows offscreen and make the composited
  // representations invisible.
  set<XWindow> xids;
  get_all_xids(&xids);
  for (set<XWindow>::iterator it = xids.begin(); it != xids.end(); ++it) {
    Window* win = wm_->GetWindow(*it);
    if (win) {
      win->MoveClientOffscreen();
      win->HideComposited();
    } else {
      DCHECK(IsInputWindow(*it)) << "Window " << XidStr(*it);
      wm_->xconn()->ConfigureWindowOffscreen(*it);
    }
  }

  // Also ditch any already-destroyed windows that we were hanging on to.
  destroyed_windows_.clear();

  // Give up the focus if we have it.
  Window* focused_win = wm_->focus_manager()->focused_win();
  if (focused_win && xids.count(focused_win->xid()))
    wm_->FocusWindow(NULL, wm_->GetCurrentTimeFromServer());

  requested_destruction_ = true;
  wm_->DestroyLoginController();
}

void LoginController::NotifySessionManager() {
  wm_->dbus()->CallMethod(login_manager::kSessionManagerServiceName,
                          login_manager::kSessionManagerServicePath,
                          login_manager::kSessionManagerInterface,
                          login_manager::kSessionManagerEmitLoginPromptVisible);
}

}  // namespace window_manager
