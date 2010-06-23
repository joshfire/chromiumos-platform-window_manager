// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/login_controller.h"

#include <map>
#include <string>

#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/callback.h"
#include "window_manager/event_loop.h"
#include "window_manager/focus_manager.h"
#include "window_manager/geometry.h"
#include "window_manager/key_bindings.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

using std::map;
using std::set;
using std::string;
using std::vector;
using window_manager::util::XidStr;

namespace window_manager {

// Time for the animations.
static const int kAnimationTimeInMs = 200;

// Time for the initial show animation.
static const int kInitialShowAnimationTimeInMs = 400;

// Amount of time we delay between when all the windows have been mapped and the
// animation is started.
static const int kInitialShowDelayMs = 50;

// Amount of time to take for animations when transitioning from the
// logged-out state to the logged-in state.
static const int kLoggedInTransitionAnimMs = 100;

// Used when nothing is selected.
static const size_t kNoSelection = -1;

static const int kNoTimer = -1;

// Action names for navigating across user windows.
static const char kSelectLeftAction[] = "login-select-left";
static const char kSelectRightAction[] = "login-select-right";

// Returns the index of the user the window belongs to or -1 if the window does
// not have a parameter specifying the index.
static int GetUserIndex(Window* win) {
  return win->type_params().empty() ? -1 : win->type_params()[0];
}

// Macro that returns from current function if the specified window doesn't
// have a parameter identifying the index.
#define FAIL_IF_INDEX_MISSING(win, type) \
  if (GetUserIndex(win) == -1) {                                 \
    LOG(WARNING) << "index missing for window " << win->xid_str() << \
        " of type " << type; \
    return; \
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
      inited_sizes_(false),
      has_all_windows_(false),
      padding_(0),
      border_width_(0),
      border_height_(0),
      unselected_border_width_(0),
      unselected_border_height_(0),
      border_to_controls_gap_(0),
      controls_height_(0),
      label_height_(0),
      unselected_label_height_(0),
      unselected_border_scale_x_(0),
      unselected_border_scale_y_(0),
      unselected_image_scale_x_(0),
      unselected_image_scale_y_(0),
      unselected_label_scale_x_(0),
      unselected_label_scale_y_(0),
      waiting_for_guest_(false),
      selected_entry_index_(kNoSelection),
      selection_changed_manager_(this),
      guest_window_(NULL),
      background_window_(NULL),
      initial_show_timeout_id_(kNoTimer),
      login_window_to_focus_(NULL),
      waiting_to_hide_windows_(false),
      entry_key_bindings_group_(new KeyBindingsGroup(wm_->key_bindings())) {
  registrar_.RegisterForChromeMessages(
      chromeos::WM_IPC_MESSAGE_WM_HIDE_LOGIN);
  registrar_.RegisterForChromeMessages(
      chromeos::WM_IPC_MESSAGE_WM_SET_LOGIN_STATE);

  entry_key_bindings_group_->Disable();
  RegisterNavigationKeyBindings();
}

LoginController::~LoginController() {
  if (initial_show_timeout_id_ != kNoTimer) {
    wm_->event_loop()->RemoveTimeout(initial_show_timeout_id_);
    initial_show_timeout_id_ = kNoTimer;
  }
}

bool LoginController::IsInputWindow(XWindow xid) {
  for (Entries::const_iterator it = entries_.begin();
       it != entries_.end(); ++it) {
    if (xid == it->input_window_xid)
      return true;
  }
  return false;
}

void LoginController::HandleScreenResize() {
  if (wm_->logged_in()) {
    // Make sure that all of our client windows stay offscreen.
    set<XWindow> xids;
    get_all_xids(&xids);
    for (set<XWindow>::iterator it = xids.begin(); it != xids.end(); ++it) {
      Window* win = wm_->GetWindow(*it);
      if (win) {
        win->MoveClientOffscreen();
      } else {
        DCHECK(IsInputWindow(*it)) << "Window " << XidStr(*it);
        wm_->xconn()->ConfigureWindowOffscreen(*it);
      }
    }
  } else {
    NOTIMPLEMENTED();
  }
}

void LoginController::HandleLoggedInStateChange() {
  if (wm_->logged_in()) {
    waiting_to_hide_windows_ = true;
    entry_key_bindings_group_->Disable();
  }
}

bool LoginController::HandleWindowMapRequest(Window* win) {
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
      win->MapClient();
      return true;
    default:
      if (wm_->logged_in())
        return false;

      // If we're not logged in yet, just map everything we see --
      // LayoutManager's not going to do it for us.
      wm_->stacking_manager()->StackWindowAtTopOfLayer(
          win, StackingManager::LAYER_LOGIN_OTHER_WINDOW);
      win->MapClient();
      return true;
  }
}

void LoginController::HandleWindowMap(Window* win) {
  switch (win->type()) {
    case chromeos::WM_IPC_WINDOW_LOGIN_GUEST: {
      if (guest_window_)
        LOG(WARNING) << "two guest windows encountered.";
      guest_window_ = win;
      wm_->focus_manager()->UseClickToFocusForWindow(guest_window_);
      registrar_.RegisterForWindowEvents(guest_window_->xid());
      break;
    }
    case chromeos::WM_IPC_WINDOW_LOGIN_BORDER: {
      FAIL_IF_INDEX_MISSING(win, "border");
      Entry* entry = GetEntryForWindow(win);
      if (entry->border_window)
        LOG(WARNING) << "two borders at index " << GetUserIndex(win);
      entry->border_window = win;
      wm_->xconn()->RemoveInputRegionFromWindow(entry->border_window->xid());
      break;
    }
    case chromeos::WM_IPC_WINDOW_LOGIN_IMAGE: {
      FAIL_IF_INDEX_MISSING(win, "image");
      Entry* entry = GetEntryForWindow(win);
      if (entry->image_window)
        LOG(WARNING) << "two images at index " << GetUserIndex(win);
      entry->image_window = win;
      break;
    }
    case chromeos::WM_IPC_WINDOW_LOGIN_CONTROLS: {
      FAIL_IF_INDEX_MISSING(win, "controls");
      Entry* entry = GetEntryForWindow(win);
      if (entry->controls_window)
        LOG(WARNING) << "two controls at index " << GetUserIndex(win);
      entry->controls_window = win;
      wm_->focus_manager()->UseClickToFocusForWindow(entry->controls_window);
      registrar_.RegisterForWindowEvents(entry->controls_window->xid());
      break;
    }
    case chromeos::WM_IPC_WINDOW_LOGIN_LABEL: {
      FAIL_IF_INDEX_MISSING(win, "label");
      Entry* entry = GetEntryForWindow(win);
      if (entry->label_window)
        LOG(WARNING) << "two labels at index " << GetUserIndex(win);
      entry->label_window = win;
      break;
    }
    case chromeos::WM_IPC_WINDOW_LOGIN_UNSELECTED_LABEL: {
      FAIL_IF_INDEX_MISSING(win, "unselectedlabel");
      Entry* entry = GetEntryForWindow(win);
      if (entry->unselected_label_window)
        LOG(WARNING) << "two unselected labels at index " << GetUserIndex(win);
      entry->unselected_label_window = win;
      wm_->xconn()->RemoveInputRegionFromWindow(
          entry->unselected_label_window->xid());
      break;
    }
    case chromeos::WM_IPC_WINDOW_LOGIN_BACKGROUND: {
      if (win->type_params().empty()) {
        LOG(WARNING) << " background window missing expected param";
        return;
      }
      if (background_window_)
        LOG(WARNING) << "two background windows encountered.";
      background_window_ = win;
      wm_->focus_manager()->UseClickToFocusForWindow(background_window_);
      registrar_.RegisterForWindowEvents(background_window_->xid());
      registrar_.RegisterForPropertyChanges(
          background_window_->xid(),
          wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));
      break;
    }
    default:
      if (wm_->logged_in()) {
        if (waiting_to_hide_windows_ &&
            win->type() == chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL) {
          waiting_to_hide_windows_ = false;
          HideWindowsAfterLogin();
        }
      } else {
        // If we're not logged in yet, just show other windows at the spot
        // where they asked to be mapped.
        if (!non_login_xids_.insert(win->xid()).second) {
          LOG(ERROR) << "Already managing window " << win->xid_str();
          return;
        }
        registrar_.RegisterForWindowEvents(win->xid());
        if (!win->override_redirect()) {
          // Restack the window again in case it was mapped before the
          // window manager started.
          wm_->stacking_manager()->StackWindowAtTopOfLayer(
              win, StackingManager::LAYER_LOGIN_OTHER_WINDOW);

          // If this is a transient window, center it over its owner
          // (unless it's an infobubble, which we just let Chrome position
          // wherever it wants).
          if (win->transient_for_xid() &&
              win->type() != chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE) {
            Window* owner_win = wm_->GetWindow(win->transient_for_xid());
            if (owner_win)
              win->CenterClientOverWindow(owner_win);
          }
          wm_->focus_manager()->UseClickToFocusForWindow(win);
          wm_->FocusWindow(win, wm_->GetCurrentTimeFromServer());
        }
        win->MoveCompositedToClient();
        win->ShowComposited();
      }

      return;
  }

  login_xids_.insert(win->xid());
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      win, StackingManager::LAYER_LOGIN_WINDOW);

  OnGotNewWindowOrPropertyChange();

  // TODO(sky): there is a race condition here. If we die and restart with the
  // login already running we don't really know what state it was in. We need
  // Chrome to keep the current state as a parameter on one of the windows so
  // that we know what state it was in.

  if (win == guest_window_ && waiting_for_guest_)
    SelectGuest();
}

void LoginController::HandleWindowUnmap(Window* win) {
  set<XWindow>::iterator non_login_it = non_login_xids_.find(win->xid());
  if (non_login_it != non_login_xids_.end()) {
    non_login_xids_.erase(*non_login_it);
    registrar_.UnregisterForWindowEvents(win->xid());
    if (win->IsFocused()) {
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

  has_all_windows_ = false;

  if (win == background_window_) {
    registrar_.UnregisterForPropertyChanges(
        background_window_->xid(),
        wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));
    registrar_.UnregisterForWindowEvents(background_window_->xid());
    background_window_ = NULL;
  } else if (win == guest_window_) {
    registrar_.UnregisterForWindowEvents(guest_window_->xid());
    guest_window_ = NULL;
    waiting_for_guest_ = false;
  } else {
    for (size_t i = 0; i < entries_.size(); ++i) {
      Entry& entry = entries_[i];
      if (entry.border_window == win) {
        entry.border_window = NULL;
      } else if (entry.image_window == win) {
        entry.image_window = NULL;
      } else if (entry.controls_window == win) {
        registrar_.UnregisterForWindowEvents(entry.controls_window->xid());
        entry.controls_window = NULL;
      } else if (entry.label_window == win) {
        entry.label_window = NULL;
      } else if (entry.unselected_label_window == win) {
        entry.unselected_label_window = NULL;
      } else {
        continue;
      }

      if (entry.has_no_windows()) {
        UnregisterInputWindow(&entry);
        entries_.erase(entries_.begin() + i);
        if (!guest_window_ && !entries_.empty() && selected_entry_index_ == i) {
          // Selected entry was unmapped, switch active entry to next one.
          // If next is a guest entry, select previous one.
          selected_entry_index_ = -1;
          size_t active_entry = i;
          if (active_entry == (entries_.size() - 1) && entries_.size() > 1)
            active_entry--;
          SelectEntryAt(active_entry);
        }
      }

      // Only one entry can possibly contain a window, no need to continue
      // through other entries.
      break;
    }
  }

  login_xids_.erase(win->xid());

  if (login_window_to_focus_ == win)
    login_window_to_focus_ = NULL;
}

void LoginController::HandleWindowConfigureRequest(Window* win,
                                                   int req_x,
                                                   int req_y,
                                                   int req_width,
                                                   int req_height) {
  if (!IsLoginWindow(win)) {
    if (!wm_->logged_in()) {
      // If Chrome isn't logged in, just make whatever changes the window
      // asked for.
      win->MoveClient(req_x, req_y);
      win->MoveCompositedToClient();
      win->ResizeClient(req_width, req_height, GRAVITY_NORTHWEST);
    }
    return;
  }

  // We manage the x/y, but let Chrome manage the width/height.
  win->ResizeClient(req_width, req_height, GRAVITY_NORTHWEST);
}

void LoginController::HandleButtonPress(XWindow xid,
                                        int x, int y,
                                        int x_root, int y_root,
                                        int button,
                                        XTime timestamp) {
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

  for (Entries::const_iterator it = entries_.begin(); it != entries_.end();
       ++it) {
    if (it->input_window_xid == xid) {
      SelectEntryAt(it - entries_.begin());
      return;
    }
  }

  // Otherwise, this was probably just some window that had a button grab
  // as a result of us calling FocusManager::UseClickToFocusForWindow().
  if (login_window_to_focus_)
    wm_->FocusWindow(login_window_to_focus_, wm_->GetCurrentTimeFromServer());
}

void LoginController::HandleChromeMessage(const WmIpc::Message& msg) {
  switch (msg.type()) {
    case chromeos::WM_IPC_MESSAGE_WM_HIDE_LOGIN: {
      Hide();
      break;
    }

    case chromeos::WM_IPC_MESSAGE_WM_SET_LOGIN_STATE: {
      SetEntrySelectionEnabled(msg.param(0) == 1);
      break;
    }

    default:
      break;
  }
}

void LoginController::HandleClientMessage(XWindow xid,
                                          XAtom message_type,
                                          const long data[5]) {
  if (wm_->logged_in())
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

void LoginController::HandleWindowPropertyChange(XWindow xid, XAtom xatom) {
  // Currently only listen for property changes on the background window.
  DCHECK(background_window_ && background_window_->xid() == xid);

  OnGotNewWindowOrPropertyChange();
}

void LoginController::InitSizes(int unselected_image_size, int padding) {
  if (inited_sizes_)
    return;

  DCHECK(!entries_.empty());

  inited_sizes_ = true;

  padding_ = padding;

  const Entry& entry = entries_[0];
  controls_height_ = entry.controls_window->client_height();
  label_height_ = entry.label_window->client_height();
  unselected_label_height_ = entry.unselected_label_window->client_height();
  border_width_ = entry.border_window->client_width();
  border_to_controls_gap_ = (border_width_ -
                             entry.image_window->client_width()) / 2;
  border_height_ = entry.border_window->client_height();

  unselected_border_width_ = border_width_ -
      (entry.image_window->client_width() - unselected_image_size);
  unselected_border_height_ = border_height_ -
      (entry.image_window->client_height() - unselected_image_size) -
      entry.controls_window->client_height() - border_to_controls_gap_;

  unselected_border_scale_x_ = static_cast<double>(unselected_border_width_) /
      static_cast<double>(border_width_);
  unselected_border_scale_y_ = static_cast<double>(unselected_border_height_) /
      static_cast<double>(border_height_);

  unselected_image_scale_x_ =
      static_cast<double>(unselected_border_width_ - border_to_controls_gap_ -
                          border_to_controls_gap_) /
      static_cast<double>(border_width_ - border_to_controls_gap_ -
                          border_to_controls_gap_);
  unselected_image_scale_y_ =
      static_cast<double>(unselected_border_height_ - border_to_controls_gap_ -
                          border_to_controls_gap_) /
      static_cast<double>(border_height_ - border_to_controls_gap_ -
                          border_to_controls_gap_ - controls_height_ -
                          border_to_controls_gap_);

  unselected_label_scale_x_ =
      static_cast<double>(entry.unselected_label_window->client_width()) /
      static_cast<double>(entry.label_window->client_width());
  unselected_label_scale_y_ =
      static_cast<double>(entry.unselected_label_window->client_height()) /
      static_cast<double>(entry.label_window->client_height());
}


void LoginController::InitialShow() {
  wm_->event_loop()->RemoveTimeout(initial_show_timeout_id_);
  initial_show_timeout_id_ = kNoTimer;

  DCHECK(!entries_.empty());

  selected_entry_index_ = 0;

  vector<Point> origins;
  CalculateIdealOrigins(entries_.size(), selected_entry_index_, &origins);

  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];
    const bool selected = (i == selected_entry_index_);

    Rect border_bounds, image_bounds, controls_bounds, label_bounds;
    CalculateEntryBounds(origins[i], selected,
                         &border_bounds, &image_bounds,
                         &controls_bounds, &label_bounds);

    if (selected) {
      // Move the input window off screen.
      wm_->ConfigureInputWindow(entry.input_window_xid, -1, -1, 1, 1);
      FocusLoginWindow(entry.controls_window, wm_->GetCurrentTimeFromServer());
      entry.unselected_label_window->HideComposited();
      entry.label_window->ShowComposited();
      entry.label_window->MoveClient(label_bounds.x, label_bounds.y);
      entry.controls_window->MoveClient(controls_bounds.x, controls_bounds.y);
      entry.image_window->MoveClient(image_bounds.x, image_bounds.y);
    } else {
      ScaleUnselectedEntry(entry, border_bounds, label_bounds, true);
      entry.label_window->HideComposited();
      entry.label_window->MoveClientOffscreen();
      entry.unselected_label_window->ShowComposited();
    }

    entry.border_window->SetCompositedOpacity(0, 0);
    entry.border_window->MoveComposited(border_bounds.x, border_bounds.y, 0);
    entry.border_window->ShowComposited();
    entry.border_window->SetCompositedOpacity(1, kInitialShowAnimationTimeInMs);

    entry.image_window->SetCompositedOpacity(0, 0);
    entry.image_window->MoveComposited(image_bounds.x, image_bounds.y, 0);
    entry.image_window->ShowComposited();
    entry.image_window->SetCompositedOpacity(1, kInitialShowAnimationTimeInMs);

    entry.controls_window->SetCompositedOpacity(0, 0);
    entry.controls_window->MoveComposited(
        controls_bounds.x, controls_bounds.y, 0);
    entry.controls_window->ShowComposited();
    entry.controls_window->SetCompositedOpacity(
        1, kInitialShowAnimationTimeInMs);

    Window* label_window =
        selected ? entry.label_window : entry.unselected_label_window;
    label_window->SetCompositedOpacity(0, 0);
    label_window->MoveComposited(label_bounds.x, label_bounds.y, 0);
    label_window->ShowComposited();
    label_window->SetCompositedOpacity(1, kInitialShowAnimationTimeInMs);
  }
}

void LoginController::ConfigureBackgroundWindow() {
  DCHECK(background_window_);
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      background_window_, StackingManager::LAYER_LOGIN_WINDOW);
  background_window_->MoveClient(0, 0);
  background_window_->MoveCompositedToClient();
  background_window_->SetCompositedOpacity(0, 0);
  background_window_->ShowComposited();
  background_window_->SetCompositedOpacity(1, kInitialShowAnimationTimeInMs);
}

void LoginController::StackWindows() {
  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];
    wm_->stacking_manager()->StackWindowAtTopOfLayer(
        entry.unselected_label_window, StackingManager::LAYER_LOGIN_WINDOW);
    wm_->stacking_manager()->StackWindowAtTopOfLayer(
        entry.label_window, StackingManager::LAYER_LOGIN_WINDOW);
    wm_->stacking_manager()->StackWindowAtTopOfLayer(
        entry.border_window, StackingManager::LAYER_LOGIN_WINDOW);
    wm_->stacking_manager()->StackWindowAtTopOfLayer(
        entry.image_window, StackingManager::LAYER_LOGIN_WINDOW);
    wm_->stacking_manager()->StackWindowAtTopOfLayer(
        entry.controls_window, StackingManager::LAYER_LOGIN_WINDOW);
  }

  // Move the input windows to the top of the stack.
  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];
    wm_->stacking_manager()->StackXidAtTopOfLayer(
        entry.input_window_xid, StackingManager::LAYER_LOGIN_WINDOW);
  }
}

void LoginController::SelectEntryAt(size_t index) {
  if (index == selected_entry_index_)
    return;

  // Process any pending selection change.
  if (selection_changed_manager_.is_scheduled()) {
    ProcessSelectionChangeCompleted(
        selection_changed_manager_.selected_index());
    selection_changed_manager_.Stop();
  }

  const bool selecting_guest = (index + 1 == entries_.size());
  if (IsOldChrome() && selecting_guest) {
    if (!guest_window_) {
      waiting_for_guest_ = true;
      // We haven't got the guest window yet, tell chrome to create it.
      entry_key_bindings_group_->Disable();
      wm_->wm_ipc()->SendMessage(
          entries_[0].border_window->xid(),  // Doesn't matter which window we
                                             // use.
          WmIpc::Message(chromeos::WM_IPC_MESSAGE_CHROME_CREATE_GUEST_WINDOW));
      return;
    }
    SelectGuest();
    return;
  }

  // For guest entry navigation bindings should be disabled to allow normal
  // keyboard navigation among controls on guest login dialog.
  const bool guest_was_selected =
      (selected_entry_index_ == entries_.size() - 1);
  if (selecting_guest)
    entry_key_bindings_group_->Disable();
  else if (guest_was_selected)
    entry_key_bindings_group_->Enable();

  waiting_for_guest_ = selecting_guest;

  const size_t last_selected_index = selected_entry_index_;

  selected_entry_index_ = index;

  vector<Point> origins;
  CalculateIdealOrigins(entries_.size(), selected_entry_index_, &origins);

  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];
    const bool selected = (i == selected_entry_index_);
    const bool was_selected = (i == last_selected_index);
    const bool is_guest = (i + 1 == entries_.size());

    Rect border_bounds, image_bounds, controls_bounds, label_bounds;
    CalculateEntryBounds(origins[i], selected,
                         &border_bounds, &image_bounds,
                         &controls_bounds, &label_bounds);

    if (selected) {
      entry.border_window->ScaleComposited(1, 1, kAnimationTimeInMs);
      entry.controls_window->ScaleComposited(1, 1, kAnimationTimeInMs);
      if (selecting_guest) {
        // Hide image window and place controls window on its positions.
        entry.image_window->HideComposited();
        entry.controls_window->MoveClient(image_bounds.x, image_bounds.y);
      } else {
        entry.image_window->MoveClient(image_bounds.x, image_bounds.y);
        entry.image_window->ScaleComposited(1, 1, kAnimationTimeInMs);
        entry.controls_window->MoveClient(controls_bounds.x, controls_bounds.y);
      }
      wm_->ConfigureInputWindow(entry.input_window_xid, -1, -1, 1, 1);
      FocusLoginWindow(entry.controls_window, wm_->GetCurrentTimeFromServer());

      // This item became selected. Move the label window to match the bounds
      // of the unselected label and scale it up.
      entry.label_window->ScaleComposited(unselected_label_scale_x_,
                                          unselected_label_scale_y_,
                                          0);
      entry.label_window->MoveComposited(
          entry.unselected_label_window->composited_x(),
          entry.unselected_label_window->composited_y(),
          0);
      entry.label_window->ShowComposited();
      entry.label_window->ScaleComposited(1, 1, kAnimationTimeInMs);
      entry.label_window->MoveComposited(label_bounds.x,
                                         label_bounds.y,
                                         kAnimationTimeInMs);
      entry.label_window->MoveClient(label_bounds.x, label_bounds.y);
      entry.unselected_label_window->HideComposited();
    } else {
      ScaleUnselectedEntry(entry, border_bounds, label_bounds, false);
    }

    if (was_selected) {
      entry.label_window->ScaleComposited(unselected_label_scale_x_,
                                          unselected_label_scale_y_,
                                          kAnimationTimeInMs);
      entry.label_window->MoveComposited(label_bounds.x, label_bounds.y,
                                         kAnimationTimeInMs);
      entry.controls_window->MoveClientOffscreen();
      entry.image_window->MoveClientOffscreen();
      entry.label_window->MoveClientOffscreen();
      // Show image window if it was hidden for guest entry.
      if (guest_was_selected)
        entry.image_window->ShowComposited();
    }

    entry.border_window->MoveComposited(border_bounds.x, border_bounds.y,
                                        kAnimationTimeInMs);
    entry.image_window->MoveComposited(image_bounds.x, image_bounds.y,
                                       kAnimationTimeInMs);
    if (selecting_guest && is_guest) {
      entry.controls_window->MoveComposited(image_bounds.x, image_bounds.y,
                                            kAnimationTimeInMs);
    } else {
      entry.controls_window->MoveComposited(controls_bounds.x,
                                            controls_bounds.y,
                                            kAnimationTimeInMs);
    }

    if (!selected && !was_selected) {
      entry.unselected_label_window->MoveComposited(label_bounds.x,
                                                    label_bounds.y,
                                                    kAnimationTimeInMs);
    }
  }

  if (last_selected_index != static_cast<size_t>(-1))
    selection_changed_manager_.Schedule(last_selected_index);
}

void LoginController::Hide() {
  selection_changed_manager_.Stop();

  if (IsOldChrome() && selected_entry_index_ + 1 == entries_.size())
    return;  // Guest entry is selected and all the windows are already
             // offscreen.

  vector<Point> origins;
  CalculateIdealOrigins(entries_.size(), selected_entry_index_, &origins);

  const int max_y = wm_->height();

  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];
    const bool selected = (i == selected_entry_index_);

    Rect border_bounds, image_bounds, controls_bounds, label_bounds;
    CalculateEntryBounds(origins[i], selected,
                         &border_bounds, &image_bounds,
                         &controls_bounds, &label_bounds);
    entry.border_window->MoveComposited(border_bounds.x,
                                        max_y, kAnimationTimeInMs);
    entry.border_window->SetCompositedOpacity(0, kAnimationTimeInMs);
    entry.image_window->MoveComposited(
        image_bounds.x,
        max_y + (image_bounds.y - border_bounds.y),
        kAnimationTimeInMs);
    entry.image_window->SetCompositedOpacity(0, kAnimationTimeInMs);

    if (selected) {
      entry.controls_window->MoveComposited(
          controls_bounds.x,
          max_y + (controls_bounds.y - border_bounds.y),
          kAnimationTimeInMs);
      entry.controls_window->SetCompositedOpacity(0, kAnimationTimeInMs);
      entry.controls_window->MoveClientOffscreen();
      entry.label_window->MoveComposited(
          label_bounds.x,
          max_y + (label_bounds.y - border_bounds.y),
          kAnimationTimeInMs);
      entry.label_window->SetCompositedOpacity(0, kAnimationTimeInMs);
      entry.label_window->MoveClientOffscreen();
      entry.image_window->MoveClientOffscreen();
    } else {
      entry.unselected_label_window->MoveComposited(
          label_bounds.x,
          max_y + (label_bounds.y - border_bounds.y),
          kAnimationTimeInMs);
      entry.unselected_label_window->SetCompositedOpacity(0,
                                                          kAnimationTimeInMs);
    }
  }
}

void LoginController::SetEntrySelectionEnabled(bool enable) {
  if (enable) {
    entry_key_bindings_group_->Enable();
    for (size_t i = 0; i < entries_.size(); ++i) {
      if (i != selected_entry_index_) {
        wm_->ConfigureInputWindow(entries_[i].input_window_xid,
                                  entries_[i].border_window->composited_x(),
                                  entries_[i].border_window->composited_y(),
                                  unselected_border_width_,
                                  unselected_border_height_ +
                                      border_to_controls_gap_ +
                                      unselected_label_height_);
      }
    }
  } else {
    entry_key_bindings_group_->Disable();
    for (size_t i = 0; i < entries_.size(); ++i)
      wm_->ConfigureInputWindow(entries_[i].input_window_xid, -1, -1, 1, 1);
  }
}

void LoginController::SelectGuest() {
  DCHECK(guest_window_);

  entry_key_bindings_group_->Disable();

  waiting_for_guest_ = false;

  const Entry& guest_entry = entries_.back();

  // Move the guest window to its original location of guest border.
  const int guest_width = guest_window_->client_width();
  const int guest_height = guest_window_->client_height();
  const float x_scale = (static_cast<float>(unselected_border_width_) /
                         static_cast<float>(guest_width));
  const float y_scale = (static_cast<float>(unselected_border_height_) /
                         static_cast<float>(guest_height));
  guest_window_->ScaleComposited(x_scale, y_scale, 0);
  guest_window_->SetCompositedOpacity(0, 0);
  guest_window_->MoveComposited(guest_entry.border_window->composited_x(),
                                guest_entry.border_window->composited_y(),
                                0);
  guest_window_->StackCompositedBelow(guest_entry.border_window->actor(), NULL,
                                      true);
  guest_window_->StackClientBelow(guest_entry.border_window->xid());
  guest_window_->ShowComposited();

  // Move the guest window to its target location and focus it.
  guest_window_->ScaleComposited(1, 1, kAnimationTimeInMs);
  guest_window_->SetCompositedOpacity(1, kAnimationTimeInMs);
  guest_window_->MoveComposited((wm_->width() - guest_width) / 2,
                                (wm_->height() - guest_height) / 2,
                                kAnimationTimeInMs);
  guest_window_->MoveClientToComposited();
  FocusLoginWindow(guest_window_, wm_->GetCurrentTimeFromServer());

  // Move the guest entry to the center of the window, expanding it to normal
  // size and fading out.
  Rect guest_border_target_bounds;
  Rect guest_image_target_bounds;
  Rect guest_label_target_bounds;
  Rect guest_controls_target_bounds;
  Point guest_origin((wm_->width() - border_width_) / 2,
                     (wm_->height() - border_height_ - label_height_) / 2);
  CalculateEntryBounds(guest_origin, true, &guest_border_target_bounds,
                       &guest_image_target_bounds,
                       &guest_controls_target_bounds,
                       &guest_label_target_bounds);
  guest_entry.border_window->MoveComposited(guest_border_target_bounds.x,
                                            guest_border_target_bounds.y,
                                            kAnimationTimeInMs);
  guest_entry.border_window->ScaleComposited(1, 1, kAnimationTimeInMs);
  guest_entry.border_window->SetCompositedOpacity(0, kAnimationTimeInMs);
  guest_entry.controls_window->HideComposited();
  guest_entry.image_window->ShowComposited();
  guest_entry.image_window->MoveComposited(guest_image_target_bounds.x,
                                           guest_image_target_bounds.y,
                                           kAnimationTimeInMs);
  guest_entry.image_window->ScaleComposited(1, 1, kAnimationTimeInMs);
  guest_entry.image_window->SetCompositedOpacity(0, kAnimationTimeInMs);

  // Fade out the rest of the entries.
  for (size_t i = 0; i < entries_.size() - 1; ++i) {
    entries_[i].border_window->SetCompositedOpacity(0, kAnimationTimeInMs);
    entries_[i].image_window->SetCompositedOpacity(0, kAnimationTimeInMs);
    entries_[i].controls_window->SetCompositedOpacity(0, kAnimationTimeInMs);
    entries_[i].label_window->SetCompositedOpacity(0, kAnimationTimeInMs);
    entries_[i].unselected_label_window->SetCompositedOpacity(
        0, kAnimationTimeInMs);
    wm_->ConfigureInputWindow(entries_[i].input_window_xid, -1, -1, -1, -1);
  }

  guest_entry.unselected_label_window->SetCompositedOpacity(0,
                                                            kAnimationTimeInMs);
  guest_entry.label_window->SetCompositedOpacity(0, kAnimationTimeInMs);
  wm_->ConfigureInputWindow(guest_entry.input_window_xid, -1, -1, -1, -1);
}

void LoginController::CalculateIdealOrigins(
    size_t entry_count,
    size_t selected_index,
    vector<Point>* origins) {
  const int selected_width = border_width_;
  const int selected_height = border_height_ + label_height_;

  const int unselected_width = unselected_border_width_;
  const int unselected_height = unselected_border_height_;

  const int selected_y = (wm_->height() - selected_height) / 2;
  const int unselected_y = (wm_->height() - unselected_height) / 2;

  int width = entry_count * unselected_width + (entry_count - 1) * padding_;
  if (selected_index != string::npos)
    width += selected_width - unselected_width;
  int x = (wm_->width() - width) / 2;
  for (size_t i = 0; i < entry_count; ++i) {
    int y;
    int w;
    if (selected_index == i) {
      y = selected_y;
      w = selected_width;
    } else {
      y = unselected_y;
      w = unselected_width;
    }
    origins->push_back(Point(x, y));
    x += w + padding_;
  }
}

void LoginController::CalculateEntryBounds(const Point& origin,
                                           bool selected,
                                           Rect* border_bounds,
                                           Rect* image_bounds,
                                           Rect* controls_bounds,
                                           Rect* label_bounds) {
  DCHECK(inited_sizes_);
  if (selected) {
    *border_bounds = Rect(origin.x, origin.y, border_width_,
                          border_height_);
    *image_bounds = Rect(origin.x + border_to_controls_gap_,
                         origin.y + border_to_controls_gap_,
                         border_bounds->width - border_to_controls_gap_ -
                         border_to_controls_gap_,
                         border_bounds->height - border_to_controls_gap_ -
                         border_to_controls_gap_ - controls_height_ -
                         border_to_controls_gap_);
    *controls_bounds = Rect(image_bounds->x,
                            image_bounds->y + image_bounds->height +
                            border_to_controls_gap_,
                            image_bounds->width,
                            controls_height_);
    *label_bounds = Rect(image_bounds->x,
                         border_bounds->y + border_bounds->height +
                         border_to_controls_gap_,
                         image_bounds->width,
                         label_height_);
  } else {
    *border_bounds = Rect(origin.x, origin.y, unselected_border_width_,
                          unselected_border_height_);
    *image_bounds = Rect(origin.x + border_to_controls_gap_,
                         origin.y + border_to_controls_gap_,
                         border_bounds->width - border_to_controls_gap_ -
                         border_to_controls_gap_,
                         border_bounds->height - border_to_controls_gap_ -
                         border_to_controls_gap_);
    *controls_bounds = Rect(image_bounds->x,
                            image_bounds->y + image_bounds->height +
                            border_to_controls_gap_,
                            0, 0);
    *label_bounds = Rect(image_bounds->x,
                         border_bounds->y + border_bounds->height +
                         border_to_controls_gap_,
                         image_bounds->width,
                         unselected_label_height_);
  }
}

void LoginController::ScaleUnselectedEntry(const Entry& entry,
                                           const Rect& border_bounds,
                                           const Rect& label_bounds,
                                           bool initial) {
  const int animation_time = initial ? 0 : kAnimationTimeInMs;
  entry.border_window->ScaleComposited(unselected_border_scale_x_,
                                       unselected_border_scale_y_,
                                       animation_time);
  entry.image_window->ScaleComposited(unselected_image_scale_x_,
                                      unselected_image_scale_y_,
                                      animation_time);
  entry.controls_window->ScaleComposited(0, 0, 0);
  if (initial)
    entry.label_window->HideComposited();

  wm_->ConfigureInputWindow(entry.input_window_xid,
                            border_bounds.x, border_bounds.y,
                            border_bounds.width, label_bounds.y +
                            label_bounds.height - border_bounds.y);
}

bool LoginController::IsLoginWindow(Window* window) const {
  return login_xids_.count(window->xid()) > 0;
}

LoginController::Entry* LoginController::GetEntryForWindow(Window* win) {
  return GetEntryAt(GetUserIndex(win));
}

LoginController::Entry* LoginController::GetEntryAt(int index) {
  while (entries_.size() <= static_cast<size_t>(index)) {
    entries_.push_back(Entry());
    CreateAndRegisterInputWindow(&entries_.back());
  }
  return &(entries_[index]);
}

void LoginController::CreateAndRegisterInputWindow(Entry* entry) {
  // Real bounds is set later on.
  entry->input_window_xid =
      wm_->CreateInputWindow(-1, -1, 1, 1, ButtonPressMask);
  wm_->SetNamePropertiesForXid(
      entry->input_window_xid, "input window for login entry");

  login_xids_.insert(entry->input_window_xid);

  registrar_.RegisterForWindowEvents(entry->input_window_xid);
}

void LoginController::UnregisterInputWindow(Entry* entry) {
  if (entry->input_window_xid == 0)
    return;

  registrar_.UnregisterForWindowEvents(entry->input_window_xid);

  wm_->xconn()->DestroyWindow(entry->input_window_xid);
  login_xids_.erase(entry->input_window_xid);
  entry->input_window_xid = 0;
}

void LoginController::ProcessSelectionChangeCompleted(
    size_t last_selected_index) {
  if (last_selected_index >= entries_.size())
    return;

  Entry& entry = entries_[last_selected_index];
  entry.unselected_label_window->MoveComposited(
      entry.label_window->composited_x(),
      entry.label_window->composited_y(),
      0);
  entry.unselected_label_window->ShowComposited();
  entry.label_window->HideComposited();
}

bool LoginController::HasAllWindows() {
  if (!IsBackgroundWindowReady())
    return false;

  if (entries_.empty())
    return false;

  if (!entries_[0].border_window)
    return false;

  int user_count = entries_[0].border_window->type_params()[1];
  if (entries_.size() != static_cast<size_t>(user_count))
    return false;

  for (size_t i = 0; i < entries_.size(); ++i) {
    if (!entries_[i].has_all_windows())
      return false;
  }
  return true;
}

void LoginController::OnGotNewWindowOrPropertyChange() {
  if (!has_all_windows_ && HasAllWindows()) {
    if (!inited_sizes_) {
      if (entries_[0].border_window->type_params().size() != 4) {
        LOG(WARNING) << "first border window must have 4 parameters";
        return;
      }

      InitSizes(entries_[0].border_window->type_params()[2],
                entries_[0].border_window->type_params()[3]);
    }

    DCHECK(!entries_.empty() && entries_[0].border_window);
    has_all_windows_ = true;
    entry_key_bindings_group_->Enable();

    ConfigureBackgroundWindow();
    StackWindows();

    // Don't show initial animation for guest only case.
    if (initial_show_timeout_id_ == kNoTimer && entries_.size() > 1) {
      initial_show_timeout_id_ = wm_->event_loop()->AddTimeout(
          NewPermanentCallback(this, &LoginController::InitialShow),
          kInitialShowDelayMs,
          0);
    }
  }

  if (entries_.empty() && guest_window_ && IsBackgroundWindowReady()) {
    ConfigureBackgroundWindow();

    guest_window_->MoveClient(
        (wm_->width() - guest_window_->client_width()) / 2,
        (wm_->height() - guest_window_->client_height()) / 2);
    guest_window_->MoveCompositedToClient();
    wm_->stacking_manager()->StackWindowAtTopOfLayer(
        guest_window_, StackingManager::LAYER_LOGIN_WINDOW);
    guest_window_->SetCompositedOpacity(0, 0);
    guest_window_->ShowComposited();
    guest_window_->SetCompositedOpacity(1, kInitialShowAnimationTimeInMs);
    FocusLoginWindow(guest_window_, wm_->GetCurrentTimeFromServer());
  }
}

bool LoginController::IsBackgroundWindowReady() {
  // Wait until chrome painted the background window, otherwise we get an ugly
  // gray flash.
  return background_window_ && background_window_->type_params()[0] == 1;
}

void LoginController::FocusLoginWindow(Window* win, XTime timestamp) {
  DCHECK(win);
  wm_->FocusWindow(win, timestamp);
  login_window_to_focus_ = win;
}

void LoginController::HideWindowsAfterLogin() {
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

  // Give up the focus if we have it.
  Window* focused_win = wm_->focus_manager()->focused_win();
  if (focused_win && xids.count(focused_win->xid()))
    wm_->FocusWindow(NULL, wm_->GetCurrentTimeFromServer());
}

void LoginController::RegisterNavigationKeyBindings() {
  KeyBindings* kb = wm_->key_bindings();
  kb->AddAction(
      kSelectLeftAction,
      NewPermanentCallback(this, &LoginController::CycleSelectedEntry, false),
      NULL, NULL);
  entry_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Left), kSelectLeftAction);
  entry_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Tab, KeyBindings::kShiftMask),
      kSelectLeftAction);
  kb->AddAction(
      kSelectRightAction,
      NewPermanentCallback(this, &LoginController::CycleSelectedEntry, true),
      NULL, NULL);
  entry_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Right), kSelectRightAction);
  entry_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Tab), kSelectRightAction);
}

void LoginController::CycleSelectedEntry(bool to_right) {
  int index = static_cast<int>(selected_entry_index_) + (to_right ? 1 : -1);
  if (index >= 0 && index < static_cast<int>(entries_.size()))
    SelectEntryAt(static_cast<size_t>(index));
}

bool LoginController::IsOldChrome() {
  // HACK(dpolukhin): detect old chrome version to preserve old WM behaviour.
  // Will be removed in couple weeks. It is required for backward compatibility.
  return (controls_height_ == entries_.back().controls_window->client_height());
}

}  // namespace window_manager
