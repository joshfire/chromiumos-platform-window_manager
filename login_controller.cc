// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/login_controller.h"

#include <string>

#include "window_manager/callback.h"
#include "window_manager/event_loop.h"
#include "window_manager/geometry.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

using std::string;
using std::vector;

namespace window_manager {

// Time for the animations.
static const int kAnimationTimeInMs = 200;

// Time for the initial show animation.
static const int kInitialShowAnimationTimeInMs = 400;

// Amount of time we delay between when all the windows have been mapped and the
// animation is started.
static const int kInitialShowDelayMs = 50;

// Used when nothing is selected.
static const size_t kNoSelection = -1;

static const int kNoTimer = -1;

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
      initial_show_timeout_id_(kNoTimer) {
  registrar_.RegisterForChromeMessages(WmIpc::Message::WM_HIDE_LOGIN);
  registrar_.RegisterForChromeMessages(WmIpc::Message::WM_SET_LOGIN_STATE);
}

LoginController::~LoginController() {
  if (!known_windows_.empty())
    LOG(WARNING) << "leaking windows; Chrome didn't close all login windows";

  if (initial_show_timeout_id_ != kNoTimer) {
    wm_->event_loop()->RemoveTimeout(initial_show_timeout_id_);
    initial_show_timeout_id_ = kNoTimer;
  }
}

bool LoginController::IsInputWindow(XWindow xid) {
  return known_windows_.count(xid) > 0;
}

bool LoginController::HandleWindowMapRequest(Window* win) {
  switch (win->type()) {
    case WmIpc::WINDOW_TYPE_LOGIN_BACKGROUND:
    case WmIpc::WINDOW_TYPE_LOGIN_GUEST:
    case WmIpc::WINDOW_TYPE_LOGIN_BORDER:
    case WmIpc::WINDOW_TYPE_LOGIN_IMAGE:
    case WmIpc::WINDOW_TYPE_LOGIN_CONTROLS:
    case WmIpc::WINDOW_TYPE_LOGIN_LABEL:
    case WmIpc::WINDOW_TYPE_LOGIN_UNSELECTED_LABEL:
      // Move the client offscreen. We'll move it back on screen when ready.
      win->MoveClientOffscreen();
      win->MapClient();
      return true;
    default:
      return false;
  }
}

void LoginController::HandleWindowMap(Window* win) {
  switch (win->type()) {
    case WmIpc::WINDOW_TYPE_LOGIN_GUEST: {
      if (guest_window_)
        LOG(WARNING) << "two guest windows encountered.";

      guest_window_ = win;
      break;
    }
    case WmIpc::WINDOW_TYPE_LOGIN_BORDER: {
      FAIL_IF_INDEX_MISSING(win, "border");

      Entry* entry = GetEntryForWindow(win);

      if (entry->border_window)
        LOG(WARNING) << "two borders at index " << GetUserIndex(win);

      entry->border_window = win;
      break;
    }
    case WmIpc::WINDOW_TYPE_LOGIN_IMAGE: {
      FAIL_IF_INDEX_MISSING(win, "image");

      Entry* entry = GetEntryForWindow(win);

      if (entry->image_window)
        LOG(WARNING) << "two images at index " << GetUserIndex(win);

      entry->image_window = win;
      break;
    }
    case WmIpc::WINDOW_TYPE_LOGIN_CONTROLS: {
      FAIL_IF_INDEX_MISSING(win, "controls");

      Entry* entry = GetEntryForWindow(win);

      if (entry->controls_window)
        LOG(WARNING) << "two controls at index " << GetUserIndex(win);

      entry->controls_window = win;
      break;
    }
    case WmIpc::WINDOW_TYPE_LOGIN_LABEL: {
      FAIL_IF_INDEX_MISSING(win, "label");

      Entry* entry = GetEntryForWindow(win);

      if (entry->label_window)
        LOG(WARNING) << "two labels at index " << GetUserIndex(win);

      entry->label_window = win;
      break;
    }
    case WmIpc::WINDOW_TYPE_LOGIN_UNSELECTED_LABEL: {
      FAIL_IF_INDEX_MISSING(win, "unselectedlabel");

      Entry* entry = GetEntryForWindow(win);

      if (entry->unselected_label_window)
        LOG(WARNING) << "two unselected labels at index " << GetUserIndex(win);

      entry->unselected_label_window = win;
      break;
    }
    case WmIpc::WINDOW_TYPE_LOGIN_BACKGROUND: {
      if (win->type_params().empty()) {
        LOG(WARNING) << " background window missing expected param";
        return;
      }

      if (background_window_)
        LOG(WARNING) << "two background windows encountered.";

      LOG(INFO) << "background mapped " << win->xid_str();

      background_window_ = win;
      registrar_.RegisterForPropertyChanges(
          background_window_->xid(),
          wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));
      break;
    }
    default:
      return;
  }

  known_windows_.insert(win->xid());

  OnGotNewWindowOrPropertyChange();

  // TODO(sky): there is a race condition here. If we die and restart with the
  // login already running we don't really know what state it was in. We need
  // Chrome to keep the current state as a parameter on one of the windows so
  // that we know what state it was in.

  if (win == guest_window_ && waiting_for_guest_)
    SelectGuest();
}

void LoginController::HandleWindowUnmap(Window* win) {
  if (!IsManaging(win))
    return;

  has_all_windows_ = false;

  if (win == background_window_) {
    registrar_.UnregisterForPropertyChanges(
        background_window_->xid(),
        wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));
    background_window_ = NULL;
  } else if (win == guest_window_) {
    guest_window_ = NULL;
    waiting_for_guest_ = false;
  } else {
    for (size_t i = 0; i < entries_.size(); ++i) {
      Entry& entry = entries_[i];
      if (entry.border_window == win)
        entry.border_window = NULL;
      else if (entry.image_window == win)
        entry.image_window = NULL;
      else if (entry.controls_window == win)
        entry.controls_window = NULL;
      else if (entry.label_window == win)
        entry.label_window = NULL;
      else if (entry.unselected_label_window == win)
        entry.unselected_label_window = NULL;
      else
        continue;

      if (entry.has_no_windows()) {
        UnregisterInputWindow(&entry);
        entries_.erase(entries_.begin() + i);
      }

      // Only one entry can possibly contain a window, no need to continue
      // through other entries.
      break;
    }
  }

  known_windows_.erase(win->xid());
}

void LoginController::HandleWindowConfigureRequest(Window* win,
                                                   int req_x,
                                                   int req_y,
                                                   int req_width,
                                                   int req_height) {
  if (!IsManaging(win))
    return;

  // We manage the x/y, but let Chrome manage the width/height.
  win->ResizeClient(req_width, req_height, GRAVITY_NORTHWEST);
}

void LoginController::HandleButtonPress(XWindow xid,
                                        int x, int y,
                                        int x_root, int y_root,
                                        int button,
                                        XTime timestamp) {
  if (known_windows_.count(xid) == 0)
    return;

  for (Entries::const_iterator it = entries_.begin(); it != entries_.end();
       ++it) {
    if (it->input_window_xid == xid) {
      SelectEntryAt(it - entries_.begin());
      return;
    }
  }
}

void LoginController::HandleChromeMessage(const WmIpc::Message& msg) {
  switch (msg.type()) {
    case WmIpc::Message::WM_HIDE_LOGIN: {
      Hide();
      break;
    }

    case WmIpc::Message::WM_SET_LOGIN_STATE: {
      ToggleLoginEnabled(msg.param(0) == 1);
      break;
    }

    default:
      break;
  }
}

void LoginController::HandleClientMessage(XWindow xid,
                                          XAtom message_type,
                                          const long data[5]) {
}

void LoginController::HandleFocusChange(XWindow xid, bool focus_in) {
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

  const int max_y = wm_->height();

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
      entry.controls_window->TakeFocus(wm_->GetCurrentTimeFromServer());
      entry.unselected_label_window->HideComposited();
      entry.label_window->ShowComposited();
    } else {
      ScaleUnselectedEntry(entry, border_bounds, label_bounds, true);
      entry.label_window->HideComposited();
      entry.unselected_label_window->ShowComposited();
    }

    entry.border_window->MoveComposited(border_bounds.x, max_y, 0);
    entry.border_window->ShowComposited();
    entry.border_window->MoveComposited(border_bounds.x, border_bounds.y,
                                        kInitialShowAnimationTimeInMs);

    entry.image_window->MoveComposited(image_bounds.x, max_y +
                                       (image_bounds.y - border_bounds.y), 0);
    entry.image_window->ShowComposited();
    entry.image_window->MoveComposited(image_bounds.x, image_bounds.y,
                                       kInitialShowAnimationTimeInMs);

    entry.controls_window->MoveComposited(
        controls_bounds.x, max_y + (controls_bounds.y - border_bounds.y),
        0);
    entry.controls_window->ShowComposited();
    entry.controls_window->MoveComposited(controls_bounds.x, controls_bounds.y,
                                          kInitialShowAnimationTimeInMs);

    Window* label_window =
        selected ? entry.label_window : entry.unselected_label_window;
    label_window->MoveComposited(
        image_bounds.x, max_y + (label_bounds.y - border_bounds.y),
        0);
    label_window->MoveComposited(label_bounds.x, label_bounds.y,
                                 kInitialShowAnimationTimeInMs);
  }
}

void LoginController::StackWindows() {
  background_window_->MoveComposited(0, 0, 0);
  background_window_->ShowComposited();
  background_window_->MoveClientToComposited();

  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];
    entry.border_window->StackCompositedAbove(
        background_window_->actor(), NULL, true);
    entry.label_window->StackCompositedAbove(
        background_window_->actor(), NULL, true);
    entry.unselected_label_window->StackCompositedAbove(
        background_window_->actor(), NULL, true);
    entry.image_window->StackCompositedAbove(
        entry.border_window->actor(), NULL, true);
    entry.controls_window->StackCompositedAbove(
        entry.border_window->actor(), NULL, true);
    // Move the input window to the top of the stack. We really only need it
    // above all of entries windows.
    wm_->stacking_manager()->StackXidAtTopOfLayer(
        entry.input_window_xid, StackingManager::LAYER_TOPLEVEL_WINDOW);
  }

  background_window_->StackClientBelow(entries_[0].input_window_xid);
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

  if (index + 1 == entries_.size()) {
    if (!guest_window_) {
      waiting_for_guest_ = true;
      // We haven't got the guest window yet, tell chrome to create it.
      wm_->wm_ipc()->SendMessage(
          entries_[0].border_window->xid(),  // Doesn't matter which window we
                                             // use.
          WmIpc::Message(WmIpc::Message::CHROME_CREATE_GUEST_WINDOW));
      return;
    }
    SelectGuest();
    return;
  }

  waiting_for_guest_ = false;

  const size_t last_selected_index = selected_entry_index_;

  selected_entry_index_ = index;

  vector<Point> origins;
  CalculateIdealOrigins(entries_.size(), selected_entry_index_, &origins);

  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];
    const bool selected = (i == selected_entry_index_);
    const bool was_selected = (i == last_selected_index);

    Rect border_bounds, image_bounds, controls_bounds, label_bounds;
    CalculateEntryBounds(origins[i], selected,
                         &border_bounds, &image_bounds,
                         &controls_bounds, &label_bounds);

    if (selected) {
      entry.border_window->ScaleComposited(1, 1, kAnimationTimeInMs);
      entry.image_window->ScaleComposited(1, 1, kAnimationTimeInMs);
      entry.controls_window->ScaleComposited(1, 1, kAnimationTimeInMs);
      entry.controls_window->MoveClient(controls_bounds.x, controls_bounds.y);
      wm_->ConfigureInputWindow(entry.input_window_xid, -1, -1, 1, 1);
      entry.controls_window->TakeFocus(wm_->GetCurrentTimeFromServer());

      // This item became selected. Move the label window to match the bounds
      // of the unnselected and scale it up.
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
    }

    entry.border_window->MoveComposited(border_bounds.x, border_bounds.y,
                                        kAnimationTimeInMs);

    entry.image_window->MoveComposited(image_bounds.x, image_bounds.y,
                                       kAnimationTimeInMs);

    entry.controls_window->MoveComposited(controls_bounds.x, controls_bounds.y,
                                          kAnimationTimeInMs);

    if (!selected && !was_selected) {
      entry.unselected_label_window->MoveComposited(label_bounds.x,
                                                    label_bounds.y,
                                                    kAnimationTimeInMs);
    }
  }

  selection_changed_manager_.Schedule(last_selected_index);
}

void LoginController::Hide() {
  selection_changed_manager_.Stop();

  if (selected_entry_index_ + 1 == entries_.size())
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
      entry.label_window->MoveComposited(
          label_bounds.x,
          max_y + (label_bounds.y - border_bounds.y),
          kAnimationTimeInMs);
      entry.label_window->SetCompositedOpacity(0, kAnimationTimeInMs);
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

void LoginController::ToggleLoginEnabled(bool enable) {
  if (enable) {
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
    for (size_t i = 0; i < entries_.size(); ++i)
      wm_->ConfigureInputWindow(entries_[i].input_window_xid, -1, -1, -1, -1);
  }
}

void LoginController::SelectGuest() {
  DCHECK(guest_window_);

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
  guest_window_->ShowComposited();

  // Move the guest window to its target location.
  guest_window_->ScaleComposited(1, 1, kAnimationTimeInMs);
  guest_window_->SetCompositedOpacity(1, kAnimationTimeInMs);
  guest_window_->MoveComposited((wm_->width() - guest_width) / 2,
                                (wm_->height() - guest_height) / 2,
                                kAnimationTimeInMs);
  guest_window_->MoveClient((wm_->width() - guest_width) / 2,
                            (wm_->height() - guest_height) / 2);
  guest_window_->RaiseClient();

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
  wm_->ConfigureInputWindow(guest_entry.input_window_xid, -1, -1, -1, -1);
}

void LoginController::CalculateIdealOrigins(
    size_t entry_count,
    size_t selected_index,
    vector<Point>* origins) {
  // We should at least have a guest and non-guest user.
  DCHECK_GT(entry_count, static_cast<size_t>(1));

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

bool LoginController::IsManaging(Window* window) const {
  return known_windows_.count(window->xid()) > 0;
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

  known_windows_.insert(entry->input_window_xid);

  registrar_.RegisterForWindowEvents(entry->input_window_xid);
}

void LoginController::UnregisterInputWindow(Entry* entry) {
  if (entry->input_window_xid == 0)
    return;

  registrar_.UnregisterForWindowEvents(entry->input_window_xid);

  wm_->xconn()->DestroyWindow(entry->input_window_xid);
  known_windows_.erase(entry->input_window_xid);
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
  DCHECK_GT(user_count, 1);
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

    StackWindows();

    if (initial_show_timeout_id_ == kNoTimer) {
      initial_show_timeout_id_ = wm_->event_loop()->AddTimeout(
          NewPermanentCallback(this, &LoginController::InitialShow),
          kInitialShowDelayMs,
          0);
    }
  }

  if (entries_.empty() && guest_window_ && IsBackgroundWindowReady()) {
    background_window_->MoveComposited(0, 0, 0);
    background_window_->ShowComposited();
    background_window_->MoveClientToComposited();
    background_window_->RaiseClient();

    guest_window_->MoveComposited(
        (wm_->width() - guest_window_->client_width()) / 2,
        (wm_->height() - guest_window_->client_height()) / 2,
        0);
    guest_window_->StackCompositedAbove(
        background_window_->actor(), NULL, true);
    guest_window_->ShowComposited();
    guest_window_->MoveClientToComposited();

    // Make sure the guest window is above the background.
    guest_window_->RaiseClient();
  }
}

bool LoginController::IsBackgroundWindowReady() {
  // Wait until chrome painted the background window, otherwise we get an ugly
  // gray flash.
  return background_window_ && background_window_->type_params()[0] == 1;
}

}  // namespace window_manager
