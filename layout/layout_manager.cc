// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/layout/layout_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tr1/memory>

#include <gflags/gflags.h>

#include "base/logging.h"
#include "base/string_util.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"
#include "window_manager/callback.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/focus_manager.h"
#include "window_manager/geometry.h"
#include "window_manager/layout/separator.h"
#include "window_manager/layout/snapshot_window.h"
#include "window_manager/layout/toplevel_window.h"
#include "window_manager/motion_event_coalescer.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x11/x_connection.h"

DEFINE_string(background_image, "", "Background image to display");

DEFINE_string(initial_chrome_window_mapped_file,
              "", "When we first see a toplevel Chrome window get mapped, "
              "we write its ID as an ASCII decimal number to this file.  "
              "Tests can watch for the file to know when the user is fully "
              "logged in.  Leave empty to disable.");

// TODO: Switch to true or remove the flag once http://crosbug.com/7263 and
// http://crosbug.com/7333 are fixed.
DEFINE_bool(enable_overview_mode, false,
            "Should the user be able to switch to overview mode to see all "
            "of their open tabs at once?");

DEFINE_string(xterm_command, "xterm", "Command to launch a terminal");

using std::map;
using std::max;
using std::min;
using std::string;
using std::tr1::shared_ptr;
using window_manager::util::FindWithDefault;
using window_manager::util::RunCommandInBackground;
using window_manager::util::XidStr;

namespace window_manager {

// Duration between panning updates while a drag is occurring on the
// background window in overview mode.
static const int kOverviewDragUpdateMs = 50;

// What fraction of the layout manager's total height should be used
// for the height of the separator.
static const double kSeparatorHeightRatio = 0.8;

// The width of the separator in pixels.
static const int kSeparatorWidth = 2;

// Various keybinding action names (finally made into static globals since
// they keep getting typoed).
static const char* kSwitchToOverviewModeAction = "switch-to-overview-mode";
static const char* kSwitchToActiveModeAction = "switch-to-active-mode";
static const char* kCycleToplevelForwardAction = "cycle-toplevel-forward";
static const char* kCycleToplevelBackwardAction = "cycle-toplevel-backward";
static const char* kCycleSnapshotForwardAction = "cycle-snapshot-forward";
static const char* kCycleSnapshotBackwardAction = "cycle-snapshot-backward";
static const char* kSwitchToActiveModeForSelectedAction =
    "switch-to-active-mode-for-selected";
static const char* kSelectToplevelWithIndexActionFormat =
    "select-toplevel-with-index-%d";
static const char* kSelectSnapshotWithIndexActionFormat =
    "select-snapshot-with-index-%d";
static const char* kSelectLastToplevelAction = "select-last-toplevel";
static const char* kSelectLastSnapshotAction = "select-last-snapshot";
static const char* kPanOverviewModeLeftAction = "pan-overview-mode-left";
static const char* kPanOverviewModeRightAction = "pan-overview-mode-right";
static const char* kLaunchTerminalAction = "launch-terminal";

const double LayoutManager::kOverviewGroupSpacing = 0.06;
const double LayoutManager::kOverviewSelectedPadding = 4.0;
const double LayoutManager::kOverviewWindowMaxSizeRatio = 0.7;
const double LayoutManager::kSideMarginRatio = 0.7;
const double LayoutManager::kOverviewExposedWindowRatio = 0.09;
const int LayoutManager::kWindowAnimMs = 200;
const double LayoutManager::kOverviewNotSelectedScale = 0.95;
const int LayoutManager::kWindowOpacityAnimMs =
    LayoutManager::kWindowAnimMs / 2;
const float LayoutManager::kBackgroundExpansionFactor = 1.5;

LayoutManager::LayoutManager(WindowManager* wm, PanelManager* panel_manager)
    : wm_(wm),
      panel_manager_(panel_manager),
      mode_(MODE_ACTIVE),
      x_(0),
      y_(0),
      width_(wm_->width()),
      height_(wm_->height()),
      panel_manager_left_width_(0),
      panel_manager_right_width_(0),
      current_toplevel_(NULL),
      current_snapshot_(NULL),
      fullscreen_toplevel_(NULL),
      overview_panning_offset_(INT_MAX),
      overview_background_offset_(0),
      overview_width_of_snapshots_(0),
      overview_background_event_coalescer_(
          new MotionEventCoalescer(
              wm_->event_loop(),
              NewPermanentCallback(
                  this, &LayoutManager::UpdateOverviewPanningForMotion),
              kOverviewDragUpdateMs)),
      overview_drag_last_x_(-1),
      saw_map_request_(false),
      first_toplevel_chrome_window_mapped_(false),
      event_consumer_registrar_(new EventConsumerRegistrar(wm, this)),
      key_bindings_actions_(new KeyBindingsActionRegistrar(wm->key_bindings())),
      active_mode_key_bindings_group_(new KeyBindingsGroup(wm->key_bindings())),
      overview_mode_key_bindings_group_(
          new KeyBindingsGroup(wm->key_bindings())),
      post_toplevel_key_bindings_group_(
          new KeyBindingsGroup(wm->key_bindings())),
      background_xid_(wm_->CreateInputWindow(wm_->root_bounds(), 0)),
      should_layout_windows_after_initial_pixmap_(false),
      should_animate_after_initial_pixmap_(false) {
  wm_->focus_manager()->RegisterFocusChangeListener(this);
  panel_manager_->RegisterAreaChangeListener(this);
  panel_manager_->GetArea(&panel_manager_left_width_,
                          &panel_manager_right_width_);

  // Disable the overview key bindings, since we start in active mode.
  overview_mode_key_bindings_group_->Disable();
  post_toplevel_key_bindings_group_->Disable();

  MoveAndResizeForAvailableArea();

  wm_->stacking_manager()->StackXidAtTopOfLayer(
      background_xid_, StackingManager::LAYER_BACKGROUND);
  wm_->SetNamePropertiesForXid(background_xid_, "background input window");

  if (!FLAGS_background_image.empty()) {
    if (FLAGS_enable_overview_mode) {
      SetBackground(
          wm_->compositor()->CreateImageFromFile(FLAGS_background_image));
    } else {
      LOG(INFO) << "Overview mode is disabled; ignoring --background_image";
    }
  }

  event_consumer_registrar_->RegisterForChromeMessages(
      chromeos::WM_IPC_MESSAGE_WM_CYCLE_WINDOWS);

  int event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
  wm_->xconn()->AddButtonGrabOnWindow(
      background_xid_, 1, event_mask, false);
  event_consumer_registrar_->RegisterForWindowEvents(background_xid_);

  key_bindings_actions_->AddAction(
      kCycleToplevelForwardAction,
      NewPermanentCallback(
          this, &LayoutManager::CycleCurrentToplevelWindow, true),
      NULL, NULL);
  active_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Tab, KeyBindings::kAltMask),
      kCycleToplevelForwardAction);

  key_bindings_actions_->AddAction(
      kCycleToplevelBackwardAction,
      NewPermanentCallback(
          this, &LayoutManager::CycleCurrentToplevelWindow, false),
      NULL, NULL);
  active_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(
          XK_Tab, KeyBindings::kAltMask | KeyBindings::kShiftMask),
      kCycleToplevelBackwardAction);

  key_bindings_actions_->AddAction(
      kCycleSnapshotForwardAction,
      NewPermanentCallback(
          this, &LayoutManager::CycleCurrentSnapshotWindow, true),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Right, 0), kCycleSnapshotForwardAction);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Tab, KeyBindings::kAltMask),
      kCycleSnapshotForwardAction);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Tab, KeyBindings::kControlMask),
      kCycleSnapshotForwardAction);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_F2, 0), kCycleSnapshotForwardAction);

  key_bindings_actions_->AddAction(
      kCycleSnapshotBackwardAction,
      NewPermanentCallback(
          this, &LayoutManager::CycleCurrentSnapshotWindow, false),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Left, 0), kCycleSnapshotBackwardAction);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(
          XK_Tab, KeyBindings::kAltMask | KeyBindings::kShiftMask),
      kCycleSnapshotBackwardAction);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(
          XK_Tab, KeyBindings::kControlMask | KeyBindings::kShiftMask),
      kCycleSnapshotBackwardAction);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_F1, 0), kCycleSnapshotBackwardAction);

  if (FLAGS_enable_overview_mode) {
    key_bindings_actions_->AddAction(
        kSwitchToOverviewModeAction,
        NewPermanentCallback(this, &LayoutManager::SetMode, MODE_OVERVIEW),
        NULL, NULL);
    active_mode_key_bindings_group_->AddBinding(
        KeyBindings::KeyCombo(XK_F5, 0), kSwitchToOverviewModeAction);
  } else {
    active_mode_key_bindings_group_->AddBinding(
        KeyBindings::KeyCombo(XK_F5, 0),
        kCycleToplevelForwardAction);
    active_mode_key_bindings_group_->AddBinding(
        KeyBindings::KeyCombo(XK_F5, KeyBindings::kShiftMask),
        kCycleToplevelBackwardAction);
  }

  key_bindings_actions_->AddAction(
      kSwitchToActiveModeAction,
      NewPermanentCallback(
          this, &LayoutManager::SetMode, MODE_ACTIVE_CANCELLED),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Escape, 0), kSwitchToActiveModeAction);

  key_bindings_actions_->AddAction(
      kSwitchToActiveModeForSelectedAction,
      NewPermanentCallback(this, &LayoutManager::SetMode, MODE_ACTIVE),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Return, 0),
      kSwitchToActiveModeForSelectedAction);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_F5, 0), kSwitchToActiveModeForSelectedAction);

  for (int i = 0; i < 8; ++i) {
    key_bindings_actions_->AddAction(
        StringPrintf(kSelectToplevelWithIndexActionFormat, i),
        NewPermanentCallback(
            this, &LayoutManager::HandleToplevelChangeRequest, i),
        NULL, NULL);
    active_mode_key_bindings_group_->AddBinding(
        KeyBindings::KeyCombo(XK_1 + i, KeyBindings::kAltMask),
        StringPrintf(kSelectToplevelWithIndexActionFormat, i));

    key_bindings_actions_->AddAction(
        StringPrintf(kSelectSnapshotWithIndexActionFormat, i),
        NewPermanentCallback(
            this, &LayoutManager::HandleSnapshotChangeRequest, i),
        NULL, NULL);
    overview_mode_key_bindings_group_->AddBinding(
        KeyBindings::KeyCombo(XK_1 + i, KeyBindings::kAltMask),
        StringPrintf(kSelectSnapshotWithIndexActionFormat, i));
  }

  key_bindings_actions_->AddAction(
      kSelectLastToplevelAction,
      NewPermanentCallback(
          this, &LayoutManager::HandleToplevelChangeRequest, -1),
      NULL, NULL);
  active_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_9, KeyBindings::kAltMask),
      kSelectLastToplevelAction);

  key_bindings_actions_->AddAction(
      kSelectLastSnapshotAction,
      NewPermanentCallback(
          this, &LayoutManager::HandleSnapshotChangeRequest, -1),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_9, KeyBindings::kAltMask),
      kSelectLastToplevelAction);

  // TODO: Choose better key bindings for panning in overview mode; these
  // were just stupid placeholders that were used for testing.
  key_bindings_actions_->AddAction(
      kPanOverviewModeLeftAction,
      NewPermanentCallback(this, &LayoutManager::PanOverviewMode, -50),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_h, KeyBindings::kAltMask),
      kPanOverviewModeLeftAction);

  key_bindings_actions_->AddAction(
      kPanOverviewModeRightAction,
      NewPermanentCallback(this, &LayoutManager::PanOverviewMode, 50),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_l, KeyBindings::kAltMask),
      kPanOverviewModeRightAction);

  key_bindings_actions_->AddAction(
      kLaunchTerminalAction,
      NewPermanentCallback(&RunCommandInBackground, FLAGS_xterm_command),
      NULL, NULL);
  post_toplevel_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(
          XK_t, KeyBindings::kControlMask | KeyBindings::kAltMask),
      kLaunchTerminalAction);
}

LayoutManager::~LayoutManager() {
  wm_->focus_manager()->UnregisterFocusChangeListener(this);
  panel_manager_->UnregisterAreaChangeListener(this);

  toplevels_.clear();
  snapshots_.clear();

  current_toplevel_ = NULL;
  current_snapshot_ = NULL;
  fullscreen_toplevel_ = NULL;

  wm_->xconn()->RemoveButtonGrabOnWindow(background_xid_, 1);
  wm_->xconn()->DestroyWindow(background_xid_);
  background_xid_ = 0;
}

bool LayoutManager::IsInputWindow(XWindow xid) {
  return (GetSnapshotWindowByInputXid(xid) != NULL);
}

void LayoutManager::HandleScreenResize() {
  MoveAndResizeForAvailableArea();
  ConfigureBackground(wm_->width(), wm_->height());
  if (background_xid_)
    wm_->xconn()->ResizeWindow(background_xid_, wm_->root_size());
}

bool LayoutManager::HandleWindowMapRequest(Window* win) {
  DCHECK(win);
  saw_map_request_ = true;

  if (!IsHandledWindowType(win->type()) &&
      (!win->transient_for_xid() ||
       !GetToplevelWindowOwningTransientWindow(*win)))
    return false;

  if (win->type() == chromeos::WM_IPC_WINDOW_CHROME_TAB_FAV_ICON ||
      win->type() == chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT ||
      win->type() == chromeos::WM_IPC_WINDOW_CHROME_TAB_TITLE) {
    wm_->stacking_manager()->StackWindowAtTopOfLayer(
        win,
        StackingManager::LAYER_SNAPSHOT_WINDOW,
        StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
  } else {
    wm_->stacking_manager()->StackWindowAtTopOfLayer(
        win,
        StackingManager::LAYER_TOPLEVEL_WINDOW,
        StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);

    // Resize windows to their final size before mapping them to give them
    // more time to draw their contents.
    if ((win->type() == chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL ||
         win->type() == chromeos::WM_IPC_WINDOW_UNKNOWN) &&
        !win->transient_for_xid()) {
      win->Resize(Size(width_, height_), GRAVITY_NORTHWEST);
    }
  }
  return true;
}

void LayoutManager::HandleWindowMap(Window* win) {
  DCHECK(win);
  if (win->override_redirect() || !IsHandledWindowType(win->type()))
    return;

  const size_t initial_num_toplevels = toplevels_.size();
  bool defer_layout = false;

  switch (win->type()) {
    case chromeos::WM_IPC_WINDOW_CHROME_TAB_FAV_ICON:
    case chromeos::WM_IPC_WINDOW_CHROME_TAB_TITLE: {
      if (!saw_map_request_)
        wm_->stacking_manager()->StackWindowAtTopOfLayer(
            win,
            StackingManager::LAYER_SNAPSHOT_WINDOW,
            StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
      LOG_IF(WARNING, win->type_params().empty()) << "Missing type parameters.";
      if (!win->type_params().empty()) {
        SnapshotWindow* snapshot = GetSnapshotWindowByXid(
            static_cast<XWindow>(win->type_params()[0]));
        LOG_IF(WARNING, !snapshot)
            << "Attempting to add decoration to nonexistent snapshot";
        if (!snapshot)
          return;
        snapshot->AddDecoration(win);
      }
      break;
    }
    case chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT: {
      // Register to get property changes for snapshot windows.
      event_consumer_registrar_->RegisterForPropertyChanges(
          win->xid(), wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));

      if (!saw_map_request_)
        wm_->stacking_manager()->StackWindowAtTopOfLayer(
            win,
            StackingManager::LAYER_SNAPSHOT_WINDOW,
            StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
      shared_ptr<SnapshotWindow> snapshot(new SnapshotWindow(win, this));
      input_to_snapshot_[snapshot->input_xid()] = snapshot.get();
      snapshots_.push_back(snapshot);
      if (mode_ == MODE_OVERVIEW) {
        if (snapshot.get() == current_snapshot_) {
          snapshot->SetState(SnapshotWindow::STATE_OVERVIEW_MODE_SELECTED);
        } else {
          snapshot->SetState(SnapshotWindow::STATE_OVERVIEW_MODE_NORMAL);
        }
      } else {
        snapshot->SetState(SnapshotWindow::STATE_ACTIVE_MODE_INVISIBLE);
      }
      SortSnapshots();
      DLOG(INFO) << "Adding snapshot " << win->xid_str()
                 << " at tab index " << snapshot->tab_index()
                 << " (total of " << snapshots_.size() << ")";
      UpdateCurrentSnapshot();
      break;
    }
    case chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL:
      // Register to get property changes for toplevel windows.
      event_consumer_registrar_->RegisterForPropertyChanges(
          win->xid(), wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));
      if (!first_toplevel_chrome_window_mapped_) {
        first_toplevel_chrome_window_mapped_ = true;
        HandleFirstToplevelChromeWindowMapped(win);
      }
      // FALL THROUGH...
    case chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE:
    case chromeos::WM_IPC_WINDOW_UNKNOWN: {
      if (win->transient_for_xid()) {
        HandleTransientWindowMap(win);
        return;
      }

      // Perform initial setup of windows that were already mapped at startup
      // (so we never saw MapRequest events for them).
      if (!saw_map_request_)
        wm_->stacking_manager()->StackWindowAtTopOfLayer(
            win,
            StackingManager::LAYER_TOPLEVEL_WINDOW,
            StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);

      if (GetToplevelWindowByWindow(*win)) {
        // WindowManager should already weed out duplicate notifications.
        // See http://crosbug.com/4176.
        LOG(DFATAL) << "Got notification about already-handled window "
                    << win->xid_str() << " getting mapped";
        return;
      }

      shared_ptr<ToplevelWindow> toplevel(new ToplevelWindow(win, this));

      switch (mode_) {
        case MODE_ACTIVE:
          // Activate the new window, adding it to the right of the
          // currently-active window.
          if (current_toplevel_) {
            int old_index = GetIndexForToplevelWindow(*current_toplevel_);
            CHECK(old_index >= 0);
            ToplevelWindows::iterator it = toplevels_.begin() + old_index + 1;
            toplevels_.insert(it, toplevel);
          } else {
            toplevels_.push_back(toplevel);
          }
          break;
        case MODE_OVERVIEW:
          // In overview mode, just put new windows on the right.
          toplevels_.push_back(toplevel);
          break;
        default:
          NOTREACHED() << "Unhandled mode " << mode_;
      }

      // Only switch to the new toplevel window if there aren't any modal
      // dialogs open; the user wouldn't be able to switch back otherwise.
      if (modal_transients_.empty())
        SetCurrentToplevel(toplevel.get());
      else
        toplevel->SetState(ToplevelWindow::STATE_ACTIVE_MODE_OFFSCREEN);
      AddOrRemoveSeparatorsAsNeeded();

      // Tell the newly mapped window what the mode is so it'll map
      // the snapshot windows it has if we're in overview mode.
      SendModeMessage(toplevel.get(), false);  // cancelled=false

      // Clients can set the fullscreen hint on a window before mapping it.
      if (win->wm_state_fullscreen())
        MakeToplevelFullscreen(toplevel.get());

      if (!win->has_initial_pixmap())
        defer_layout = true;

      break;
    }
    default:
      NOTREACHED() << "Unexpected window type " << win->type();
      break;
  }

  // Don't animate the first window that gets shown.
  bool should_animate = !(initial_num_toplevels == 0 && toplevels_.size() == 1);

  if (defer_layout) {
    should_layout_windows_after_initial_pixmap_ = true;
    should_animate_after_initial_pixmap_ = should_animate;
  } else {
    LayoutWindows(should_animate);
  }
}

void LayoutManager::HandleWindowUnmap(Window* win) {
  DCHECK(win);

  if (win->override_redirect() || !IsHandledWindowType(win->type()))
    return;

  switch (win->type()) {
    case chromeos::WM_IPC_WINDOW_CHROME_TAB_FAV_ICON:
    case chromeos::WM_IPC_WINDOW_CHROME_TAB_TITLE: {
      for (size_t i = 0; i < snapshots_.size(); ++i) {
        if (snapshots_[i]->title() == win)
          snapshots_[i]->clear_title();
        if (snapshots_[i]->fav_icon() == win)
          snapshots_[i]->clear_fav_icon();
      }
      break;
    }
    case chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT: {
      SnapshotWindow* snapshot = GetSnapshotWindowByWindow(*win);
      if (snapshot) {
        event_consumer_registrar_->UnregisterForPropertyChanges(
            win->xid(), wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));

        RemoveSnapshot(GetSnapshotWindowByWindow(*win));
        LayoutWindows(true);
      }
      break;
    }
    default: {
      ToplevelWindow* toplevel_owner =
          GetToplevelWindowOwningTransientWindow(*win);

      if (toplevel_owner) {
        if (win->wm_state_modal())
          HandleTransientWindowModalityChange(win, true);  // unmapped=true
        bool transient_had_focus = win->IsFocused();
        toplevel_owner->HandleTransientWindowUnmap(win);
        if (transient_to_toplevel_.erase(win->xid()) != 1)
          LOG(WARNING) << "No transient-to-toplevel mapping for "
                       << win->xid_str();
        if (transient_had_focus)
          toplevel_owner->TakeFocus(wm_->GetCurrentTimeFromServer());
        break;
      }

      ToplevelWindow* toplevel = GetToplevelWindowByWindow(*win);
      if (toplevel) {
        if (win->type() == chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL)
          event_consumer_registrar_->UnregisterForPropertyChanges(
              win->xid(), wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));

        RemoveToplevel(toplevel);
        if (background_.get() && wm_->GetNumWindows() == 0)
          background_->Hide();
        AddOrRemoveSeparatorsAsNeeded();
        LayoutWindows(true);
      }
      break;
    }
  }
}

void LayoutManager::HandleWindowPixmapFetch(Window* win) {
  DCHECK(win);

  if (should_layout_windows_after_initial_pixmap_ &&
      current_toplevel_ && current_toplevel_->win() == win) {
    should_layout_windows_after_initial_pixmap_ = false;
    LayoutWindows(should_animate_after_initial_pixmap_);
  }
}

void LayoutManager::HandleWindowConfigureRequest(
    Window* win, const Rect& requested_bounds) {
  if (win->type() == chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT) {
    SnapshotWindow* snapshot = GetSnapshotWindowByWindow(*win);
    if (snapshot) {
      if (requested_bounds.size() != win->client_size()) {
        win->Resize(requested_bounds.size(), GRAVITY_NORTHWEST);
        LayoutWindows(false);
      } else {
        win->SendSyntheticConfigureNotify();
      }
    }
    return;
  }

  ToplevelWindow* toplevel_owner = GetToplevelWindowOwningTransientWindow(*win);
  if (toplevel_owner) {
    toplevel_owner->HandleTransientWindowConfigureRequest(
        win, requested_bounds);
    return;
  }

  // Ignore requests to resize toplevel windows, but send them fake
  // ConfigureNotify events to let them know that we saw the requests.
  ToplevelWindow* toplevel = GetToplevelWindowByWindow(*win);
  if (toplevel) {
    win->SendSyntheticConfigureNotify();
    return;
  }
}

void LayoutManager::HandleButtonPress(XWindow xid,
                                      const Point& relative_pos,
                                      const Point& absolute_pos,
                                      int button,
                                      XTime timestamp) {
  if (xid == background_xid_ && button == 1) {
    overview_drag_last_x_ = relative_pos.x;
    overview_background_event_coalescer_->Start();
    return;
  }

  Window* win = wm_->GetWindow(xid);
  if (!win)
    return;

  // Otherwise, it probably means that the user previously focused a panel
  // and then clicked back on a toplevel or transient window.
  ToplevelWindow* toplevel = GetToplevelWindowOwningTransientWindow(*win);
  if (!toplevel)
    toplevel = GetToplevelWindowByWindow(*win);
  if (toplevel)
    toplevel->HandleButtonPress(win, timestamp);
}

void LayoutManager::HandleButtonRelease(XWindow xid,
                                        const Point& relative_pos,
                                        const Point& absolute_pos,
                                        int button,
                                        XTime timestamp) {
  SnapshotWindow* snapshot = GetSnapshotWindowByInputXid(xid);
  if (snapshot) {
    if (button == 1) {  // Ignore buttons other than 1.
      LOG_IF(WARNING, mode_ != MODE_OVERVIEW)
          << "Got a click in input window " << XidStr(xid)
          << " for snapshot window " << snapshot->win()->xid_str()
          << " while not in overview mode";
      snapshot->HandleButtonRelease(
          timestamp, absolute_pos.x - x_, absolute_pos.y - y_);
    }
    return;
  }

  if (xid != background_xid_ || button != 1)
    return;

  // The X server automatically removes our asynchronous pointer grab when
  // the mouse buttons are released.
  overview_background_event_coalescer_->Stop();

  // We need to do one last configure to update the input windows'
  // positions, which we didn't bother doing while panning.
  LayoutWindows(true);

  return;
}

void LayoutManager::HandlePointerMotion(XWindow xid,
                                        const Point& relative_pos,
                                        const Point& absolute_pos,
                                        XTime timestamp) {
  if (xid == background_xid_)
    overview_background_event_coalescer_->StorePosition(relative_pos);
}

void LayoutManager::HandleChromeMessage(const WmIpc::Message& message) {
  if (message.type() == chromeos::WM_IPC_MESSAGE_WM_CYCLE_WINDOWS)
    CycleCurrentToplevelWindow(message.param(0) != 0);
}

void LayoutManager::HandleClientMessage(XWindow xid,
                                        XAtom message_type,
                                        const long data[5]) {
  Window* win = wm_->GetWindow(xid);
  if (!win)
    return;

  if (message_type == wm_->GetXAtom(ATOM_NET_WM_STATE)) {
    map<XAtom, bool> states;
    win->ParseWmStateMessage(data, &states);
    map<XAtom, bool>::const_iterator it =
        states.find(wm_->GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN));
    if (it != states.end()) {
      ToplevelWindow* toplevel = GetToplevelWindowByWindow(*win);
      if (toplevel) {
        if (it->second)
          MakeToplevelFullscreen(toplevel);
        else
          RestoreFullscreenToplevel(toplevel);
      }
    }
    it = states.find(wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL));
    if (it != states.end()) {
      ToplevelWindow* owner = GetToplevelWindowOwningTransientWindow(*win);
      if (owner) {
        map<XAtom, bool> new_state;
        new_state[it->first] = it->second;
        win->ChangeWmState(new_state);
        HandleTransientWindowModalityChange(win, false);  // unmapped=false
      }
    }
  } else if (message_type == wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW)) {
    DLOG(INFO) << "Got _NET_ACTIVE_WINDOW request to focus " << XidStr(xid)
               << " (requestor says its currently-active window is "
               << XidStr(data[2]) << "; real active window is "
               << XidStr(wm_->active_window_xid()) << ")";

    // If we got a _NET_ACTIVE_WINDOW request for a transient, switch to
    // its owner instead.
    ToplevelWindow* toplevel = GetToplevelWindowOwningTransientWindow(*win);
    if (toplevel)
      toplevel->SetPreferredTransientWindowToFocus(win);
    else
      toplevel = GetToplevelWindowByWindow(*win);

    if (toplevel) {
      DisplayAndFocusToplevel(toplevel);
    } else {
      // If it wasn't a toplevel window, then look and see if it was a
      // snapshot window.  If it was, and we're in active mode, switch
      // to overview mode, otherwise, just switch to that snapshot
      // window.
      SnapshotWindow* snapshot = GetSnapshotWindowByWindow(*win);
      if (snapshot) {
        SetCurrentSnapshot(snapshot);
        if (mode_ == MODE_ACTIVE) {
          SetMode(MODE_OVERVIEW);
        } else {
          LayoutWindows(true);
        }
      }
    }
  }
}

void LayoutManager::HandleWindowPropertyChange(XWindow xid, XAtom xatom) {
  Window* win = wm_->GetWindow(xid);
  if (!win)
    return;

  ToplevelWindow* toplevel = GetToplevelWindowByXid(xid);
  bool changed = false;
  if (toplevel) {
    changed = toplevel->PropertiesChanged();
  } else {
    SnapshotWindow* snapshot = GetSnapshotWindowByXid(xid);
    if (snapshot) {
      changed = snapshot->PropertiesChanged();
    } else {
      LOG(WARNING) << "Received a property change message from a "
                   << "window (" << win->xid_str()
                   << ") that we weren't expecting one from.";
      return;
    }
  }

  if (changed) {
    SortSnapshots();
    UpdateCurrentSnapshot();
    if (mode_ == MODE_OVERVIEW)
      LayoutWindows(true);
  }
}

void LayoutManager::HandleFocusChange() {
  if (fullscreen_toplevel_ &&
      !fullscreen_toplevel_->IsWindowOrTransientFocused()) {
    RestoreFullscreenToplevel(fullscreen_toplevel_);
  }
}

void LayoutManager::HandlePanelManagerAreaChange() {
  panel_manager_->GetArea(&panel_manager_left_width_,
                          &panel_manager_right_width_);
  MoveAndResizeForAvailableArea();
}

Window* LayoutManager::GetChromeWindow() {
  for (size_t i = 0; i < toplevels_.size(); ++i) {
    if (toplevels_[i]->win()->type() == chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL)
      return toplevels_[i]->win();
  }
  return NULL;
}

bool LayoutManager::TakeFocus(XTime timestamp) {
  if (mode_ != MODE_ACTIVE || !current_toplevel_)
    return false;

  current_toplevel_->TakeFocus(timestamp);
  return true;
}

string LayoutManager::GetModeName(Mode mode) {
  switch(mode) {
    case MODE_ACTIVE:
      return string("Active");
    case MODE_ACTIVE_CANCELLED:
      return string("Active Cancelled");
    case MODE_OVERVIEW:
      return string("Overview");
    default:
      return string("<unknown mode>");
  }
}

void LayoutManager::LayoutWindows(bool animate) {
  should_layout_windows_after_initial_pixmap_ = false;

  if (toplevels_.empty())
    return;

  // As a last resort, if we don't have a current toplevel when we
  // layout, pick the first one.
  if (!current_toplevel_)
    current_toplevel_ = toplevels_[0].get();

  DLOG(INFO) << "Laying out windows for " << GetModeName(mode_) << " mode.";

  if (mode_ == MODE_OVERVIEW) {
    // Unless we're doing a layout in "immediate" mode (i.e. no
    // animation, which usually means we're dragging), we want to
    // enforce the bounds of scrolling.
    CalculatePositionsForOverviewMode(animate);
  }

  // We iterate through the snapshot windows in descending stacking
  // order (right-to-left).  Otherwise, we'd get spurious pointer
  // enter events as a result of stacking a window underneath the
  // pointer immediately before we stack the window to its right
  // directly on top of it.  Not a huge concern now that we're not
  // listening for enter and leave events, but that might change again
  // in the future.
  for (SnapshotWindows::reverse_iterator it = snapshots_.rbegin();
       it != snapshots_.rend(); ++it) {
    (*it)->UpdateLayout(animate);
  }
  for (ToplevelWindows::iterator it = toplevels_.begin();
       it != toplevels_.end(); ++it) {
    // Don't mess with fullscreen windows.
    if (it->get() == fullscreen_toplevel_)
      continue;
    (*it)->UpdateLayout(animate);
  }
  for (Separators::iterator it = separators_.begin();
       it != separators_.end(); ++it) {
    (*it)->UpdateLayout(animate);
  }

  if (background_.get())
    background_->MoveX(overview_background_offset_,
                       animate ? kWindowAnimMs : 0);

  if (wm_->client_window_debugging_enabled())
    wm_->UpdateClientWindowDebugging();
}

void LayoutManager::SetMode(Mode mode) {
  // Just treat the active-cancelled state as regular active mode;
  // we're really just using it to pass an extra bit of information
  // into this method so we can notify Chrome that overview mode was
  // cancelled.  Cancelling actually happens on the Chrome side, since
  // it knows what tabs used to be selected.
  bool was_cancelled = false;
  if (mode == MODE_ACTIVE_CANCELLED) {
    was_cancelled = true;
    mode = MODE_ACTIVE;
  }

  if (mode == mode_)
    return;

  DisableKeyBindingsForMode(mode_);
  mode_ = mode;
  DLOG(INFO) << "Switching to " << GetModeName(mode_) << " mode";

  switch (mode_) {
    case MODE_ACTIVE:
      if (current_toplevel_)
        current_toplevel_->TakeFocus(wm_->GetCurrentTimeFromServer());
      for (ToplevelWindows::iterator it = toplevels_.begin();
           it != toplevels_.end(); ++it) {
        if (it->get() == current_toplevel_) {
          (*it)->SetState(ToplevelWindow::STATE_ACTIVE_MODE_IN_FADE);
        } else {
          (*it)->SetState(ToplevelWindow::STATE_ACTIVE_MODE_OFFSCREEN);
        }
      }
      for (SnapshotWindows::iterator it = snapshots_.begin();
           it != snapshots_.end(); ++it) {
        (*it)->SetState(SnapshotWindow::STATE_ACTIVE_MODE_INVISIBLE);
      }
      for (Separators::iterator it = separators_.begin();
           it != separators_.end(); ++it) {
        (*it)->SetState(Separator::STATE_ACTIVE_MODE_INVISIBLE);
      }
      break;
    case MODE_OVERVIEW: {
      UpdateCurrentSnapshot();

      if (current_toplevel_ &&
          current_toplevel_->IsWindowOrTransientFocused()) {
        // We need to give the input focus away here; otherwise the
        // previously-focused window would continue to get keyboard events
        // in overview mode.  Let the WindowManager decide what to do with it.
        wm_->TakeFocus(wm_->GetCurrentTimeFromServer());
      }

      for (ToplevelWindows::iterator it = toplevels_.begin();
           it != toplevels_.end(); ++it) {
        (*it)->SetState(ToplevelWindow::STATE_OVERVIEW_MODE);
      }
      for (SnapshotWindows::reverse_iterator it = snapshots_.rbegin();
           it != snapshots_.rend(); ++it) {
        if (it->get() == current_snapshot_) {
          (*it)->SetState(SnapshotWindow::STATE_OVERVIEW_MODE_SELECTED);
        } else {
          (*it)->SetState(SnapshotWindow::STATE_OVERVIEW_MODE_NORMAL);
        }
      }
      for (Separators::iterator it = separators_.begin();
           it != separators_.end(); ++it) {
        (*it)->SetState(Separator::STATE_OVERVIEW_MODE_NORMAL);
      }
      break;
    }
    default:
      NOTREACHED() << "Unhandled mode " << mode_;
  }

  LayoutWindows(true);

  // Let all Chrome windows know about the new layout mode so that
  // each toplevel window will map its associated snapshot windows.
  for (ToplevelWindows::iterator it = toplevels_.begin();
       it != toplevels_.end(); ++it) {
    SendModeMessage(it->get(), was_cancelled);
  }

  EnableKeyBindingsForMode(mode_);
}

// static
bool LayoutManager::IsHandledWindowType(chromeos::WmIpcWindowType type) {
  return (type == chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE ||
          type == chromeos::WM_IPC_WINDOW_CHROME_TAB_FAV_ICON ||
          type == chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT ||
          type == chromeos::WM_IPC_WINDOW_CHROME_TAB_TITLE ||
          type == chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL ||
          type == chromeos::WM_IPC_WINDOW_UNKNOWN);
}

int LayoutManager::GetIndexForToplevelWindow(
    const ToplevelWindow& toplevel) const {
  for (size_t i = 0; i < toplevels_.size(); ++i)
    if (toplevels_[i].get() == &toplevel)
      return static_cast<int>(i);
  return -1;
}

int LayoutManager::GetIndexForSnapshotWindow(
    const SnapshotWindow& snapshot) const {
  for (size_t i = 0; i < snapshots_.size(); ++i)
    if (snapshots_[i].get() == &snapshot)
      return static_cast<int>(i);
  return -1;
}

LayoutManager::ToplevelWindow* LayoutManager::GetToplevelWindowByWindow(
    const Window& win) {
  for (size_t i = 0; i < toplevels_.size(); ++i)
    if (toplevels_[i]->win() == &win)
      return toplevels_[i].get();
  return NULL;
}

LayoutManager::ToplevelWindow* LayoutManager::GetToplevelWindowByXid(
    XWindow xid) {
  const Window* win = wm_->GetWindow(xid);
  if (!win)
    return NULL;
  return GetToplevelWindowByWindow(*win);
}

LayoutManager::ToplevelWindow*
LayoutManager::GetToplevelWindowOwningTransientWindow(const Window& win) {
  return FindWithDefault(
      transient_to_toplevel_, win.xid(), static_cast<ToplevelWindow*>(NULL));
}

LayoutManager::SnapshotWindow* LayoutManager::GetSnapshotWindowByInputXid(
    XWindow xid) {
  return FindWithDefault(
      input_to_snapshot_, xid, static_cast<SnapshotWindow*>(NULL));
}

LayoutManager::SnapshotWindow* LayoutManager::GetSnapshotWindowByWindow(
    const Window& win) {
  for (size_t i = 0; i < snapshots_.size(); ++i)
    if (snapshots_[i]->win() == &win)
      return snapshots_[i].get();
  return NULL;
}

LayoutManager::SnapshotWindow* LayoutManager::GetSnapshotWindowByXid(
    XWindow xid) {
  const Window* win = wm_->GetWindow(xid);
  if (!win)
    return NULL;
  return GetSnapshotWindowByWindow(*win);
}

LayoutManager::SnapshotWindow* LayoutManager::GetSnapshotAfter(
    SnapshotWindow* window) {
  int index = GetIndexForSnapshotWindow(*window);
  if (index >= 0 && static_cast<int>(snapshots_.size()) > (index + 1)) {
    return snapshots_[index + 1].get();
  }
  return NULL;
}

LayoutManager::SnapshotWindow* LayoutManager::GetSnapshotBefore(
    SnapshotWindow* window) {
  int index = GetIndexForSnapshotWindow(*window);
  if (index > 0)
    return snapshots_[index - 1].get();
  return NULL;
}

LayoutManager::SnapshotWindow* LayoutManager::GetSelectedSnapshotFromToplevel(
    const ToplevelWindow& toplevel) {
  if (toplevel.selected_tab() >= 0) {
    int index = GetPreceedingTabCount(toplevel) + toplevel.selected_tab();
    if (index >= 0 && index < static_cast<int>(snapshots_.size()))
      return snapshots_[index].get();
  }
  return NULL;
}

XWindow LayoutManager::GetInputXidForWindow(const Window& win) {
  SnapshotWindow* snapshot = GetSnapshotWindowByWindow(win);
  return snapshot ? snapshot->input_xid() : None;
}

void LayoutManager::HandleTransientWindowMap(Window* win) {
  DCHECK(win);
  DCHECK(win->transient_for_xid());

  XWindow owner_xid = win->transient_for_xid();
  if (!wm_->GetWindow(owner_xid)) {
    // Flash can create a transient window that claims to belong to the
    // window deep in the tree that's embedding it, so if we see an owner
    // that's not a direct child of the root, walk up the tree.
    XConnection::ScopedServerGrab server_grab(wm_->xconn());
    while (true) {
      XWindow parent_xid = 0;
      if (!wm_->xconn()->GetParentWindow(owner_xid, &parent_xid)) {
        LOG(WARNING) << "Got error while querying parent of "
                     << XidStr(owner_xid) << " while tracing lineage of "
                     << "transient window " << win->xid_str() << " with "
                     << "non-toplevel owner "
                     << XidStr(win->transient_for_xid());
        return;
      }
      if (parent_xid == wm_->root())
        break;
      owner_xid = parent_xid;
    }
  }

  ToplevelWindow* toplevel_owner = NULL;
  Window* owner_win = wm_->GetWindow(owner_xid);
  if (owner_win) {
    // Try to find the toplevel window representing the owner.  If
    // the owner is itself a transient window, just give the new
    // window to the owner's owner (this has the effect of us also
    // later being able to handle transients for *this* transient in
    // the same way).
    toplevel_owner = GetToplevelWindowByWindow(*owner_win);
    if (!toplevel_owner)
      toplevel_owner = GetToplevelWindowOwningTransientWindow(*owner_win);
  }

  // If we didn't find an owner for the transient, don't do anything
  // with it.  Maybe it belongs to to a panel instead.
  if (!toplevel_owner)
    return;

  if (win->type() != chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE &&
      !win->is_rgba()) {
    win->SetShadowType(Shadow::TYPE_RECTANGULAR);
  }

  transient_to_toplevel_[win->xid()] = toplevel_owner;
  toplevel_owner->HandleTransientWindowMap(win, mode_ == MODE_OVERVIEW);

  if (win->wm_state_modal()) {
    // If the transient is modal, make sure that it gets the focus and that
    // we're showing its toplevel window.
    HandleTransientWindowModalityChange(win, false);  // unmapped=false
  } else if (toplevel_owner->IsWindowOrTransientFocused()) {
    // The transient is non-modal, but we tell its toplevel to take the
    // focus if it's shown so it can pass the focus to the transient if it
    // wants to.
    toplevel_owner->TakeFocus(wm_->GetCurrentTimeFromServer());
  }
}

void LayoutManager::MoveAndResizeForAvailableArea() {
  const int old_x = x_;
  const int old_width = width_;

  x_ = panel_manager_left_width_;
  y_ = 0;
  width_ = wm_->width() -
           (panel_manager_left_width_ + panel_manager_right_width_);
  height_ = wm_->height();

  // If there's a larger difference between our new and old left edge than
  // between the new and old right edge, then we keep the right sides of the
  // windows fixed while resizing.
  Gravity resize_gravity =
      abs(x_ - old_x) > abs(x_ + width_ - (old_x + old_width)) ?
      GRAVITY_NORTHEAST :
      GRAVITY_NORTHWEST;

  for (ToplevelWindows::iterator it = toplevels_.begin();
       it != toplevels_.end(); ++it) {
    if (it->get() == fullscreen_toplevel_) {
      (*it)->win()->Resize(wm_->root_size(), GRAVITY_NORTHWEST);
    } else {
      (*it)->win()->Resize(Size(width_, height_), resize_gravity);
    }
  }

  LayoutWindows(true);
}

void LayoutManager::SetCurrentToplevel(ToplevelWindow* toplevel) {
  CHECK(toplevel);

  // If we're not in active mode, nothing changes in the layout.
  if (mode_ != MODE_ACTIVE) {
    current_toplevel_ = toplevel;
    return;
  }

  DLOG(INFO) << "Setting current toplevel to " << toplevel->win()->xid_str();

  // Determine which way we should slide.
  ToplevelWindow::State state_for_new_win;
  ToplevelWindow::State state_for_old_win;
  int this_index = GetIndexForToplevelWindow(*toplevel);
  int current_index = -1;
  if (current_toplevel_)
    current_index = GetIndexForToplevelWindow(*(current_toplevel_));

  if (current_index < 0 || this_index > current_index) {
    state_for_new_win = ToplevelWindow::STATE_ACTIVE_MODE_IN_FROM_RIGHT;
    state_for_old_win = ToplevelWindow::STATE_ACTIVE_MODE_OUT_TO_LEFT;
  } else {
    state_for_new_win = ToplevelWindow::STATE_ACTIVE_MODE_IN_FROM_LEFT;
    state_for_old_win = ToplevelWindow::STATE_ACTIVE_MODE_OUT_TO_RIGHT;
  }

  if (current_toplevel_)
    current_toplevel_->SetState(state_for_old_win);

  toplevel->SetState(state_for_new_win);
  current_toplevel_ = toplevel;
  current_toplevel_->TakeFocus(wm_->GetCurrentTimeFromServer());
}

void LayoutManager::HandleToplevelChangeRequest(int index) {
  if (toplevels_.empty())
    return;

  if (index < 0)
    index = static_cast<int>(toplevels_.size()) + index;
  if (index < 0 || index >= static_cast<int>(toplevels_.size()))
    return;
  if (toplevels_[index].get() == current_toplevel_)
    return;

  SetCurrentToplevel(toplevels_[index].get());
  LayoutWindows(true);
}

void LayoutManager::HandleSnapshotChangeRequest(int index) {
  if (snapshots_.empty())
    return;

  if (index < 0)
    index = static_cast<int>(snapshots_.size()) + index;
  if (index < 0 || index >= static_cast<int>(snapshots_.size()))
    return;
  if (snapshots_[index].get() == current_snapshot_)
    return;

  SetCurrentSnapshot(snapshots_[index].get());
  LayoutWindows(true);
}

void LayoutManager::CenterCurrentSnapshot(int x, int y) {
  int center_x = (x >= 0 && y >= 0) ? x : width_ / 2;
  if (current_snapshot_) {
    // If part of the window will be under |center_x| when centered
    // (and not tilted), just center it.  Otherwise, leave it where it
    // is so we can select it on a double click.
    if ((width_ - current_snapshot_->overview_width()) / 2 < center_x &&
        (width_ + current_snapshot_->overview_width()) / 2 >= center_x) {
      overview_panning_offset_ = -(current_snapshot_->overview_x() +
                                   (current_snapshot_->overview_width() -
                                    width_) / 2);
    }
  } else {
    overview_panning_offset_  = center_x;
  }
}

void LayoutManager::CalculatePositionsForOverviewMode(bool enforce_bounds) {
  if (toplevels_.empty() || snapshots_.empty() || mode_ != MODE_OVERVIEW)
    return;

  ToplevelWindow* last_toplevel = snapshots_[0]->toplevel();
  int running_width = 0;
  int selected_index = 0;
  int selected_offset = 0;
  const int snapshot_width = snapshots_[0]->win()->client_width();
  const int snapshot_height = snapshots_[0]->win()->client_height();
  for (int i = 0; static_cast<size_t>(i) < snapshots_.size(); ++i) {
    SnapshotWindow* snapshot = snapshots_[i].get();
    bool is_selected = (snapshot == current_snapshot_);

    if (is_selected) {
      selected_index = i;
      selected_offset = running_width;
    }

    double scale = is_selected ? 1.0 : kOverviewNotSelectedScale;
    snapshot->SetSize(snapshot_width * scale, snapshot_height * scale);
    int vertical_position = (height_ - snapshot->overview_height()) / 2 +
                            (snapshot_height * scale) * ((1.0 - scale) / 2.0);
    snapshot->SetPosition(running_width, vertical_position);

    // Here we see if we need a separator.
    if (snapshot->toplevel() != last_toplevel) {
      Separators::size_type separator_index = 0;
      for (size_t j = 0; j < toplevels_.size(); ++j) {
        if (toplevels_[j].get() == last_toplevel)
          break;
        // Only count the real toplevel windows in the toplevels_ list
        // to find out which separator to use.
        if (toplevels_[j]->win()->type() ==
            chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL)
          ++separator_index;
      }

      DCHECK(separators_.size() > separator_index)
          << "Not enough separators: (size " << separators_.size()
          << " <= index " << separator_index << "), when there are "
          << toplevels_.size() << " toplevels.";
      DCHECK(i > 0);

      // Now figure out where the separator goes.
      if (separators_.size() > separator_index && i > 0) {
        int previous_position = snapshots_[i - 1]->overview_x() +
                                snapshots_[i - 1]->overview_tilted_width();
        Separator* separator = separators_[separator_index].get();
        separator->SetX((running_width + previous_position) / 2);
        int new_height = kSeparatorHeightRatio * height_;
        separator->Resize(kSeparatorWidth, new_height, 0);
        separator->SetY((height_ - new_height) / 2);
      }
    }

    if (static_cast<size_t>(i + 1) < snapshots_.size()) {
      if (is_selected) {
        running_width += snapshot->overview_width() + kOverviewSelectedPadding;
        if (snapshots_[i + 1]->toplevel() != snapshot->toplevel())
          running_width += width_ * kOverviewGroupSpacing + 0.5f;
      } else {
        // If the next snapshot is in a different toplevel, then we
        // want to add the whole width of the window and some space.
        if (snapshots_[i + 1]->toplevel() != snapshot->toplevel()) {
          running_width += snapshot->overview_tilted_width() +
                           width_ * kOverviewGroupSpacing + 0.5f;
        } else {
          running_width += (kOverviewExposedWindowRatio * snapshot_width /
                            kOverviewWindowMaxSizeRatio);
        }
      }
    } else {
      // Still need to add this on the last one to get the
      // overview_width_of_snapshots_ correct.
      running_width += is_selected ?
                       snapshot->overview_width() + kOverviewSelectedPadding :
                       (kOverviewExposedWindowRatio * snapshot_width /
                        kOverviewWindowMaxSizeRatio);
    }
    last_toplevel = snapshot->toplevel();
  }

  // Calculate the overall size of all the snapshots.
  if (snapshots_.back().get() != current_snapshot_) {
    overview_width_of_snapshots_ =
        running_width - (kOverviewExposedWindowRatio *
                         snapshot_width / kOverviewWindowMaxSizeRatio) +
        snapshots_.back()->overview_tilted_width() + 0.5;
  } else {
    overview_width_of_snapshots_ = running_width - kOverviewSelectedPadding;
  }

  if (enforce_bounds) {
    const float kMargin = width_ * kSideMarginRatio;
    int min_x = kMargin;
    int max_x = width_ - overview_width_of_snapshots_ - kMargin;
    if (max_x < min_x)
      std::swap(max_x, min_x);

    // If we haven't set the panning offset before, center the current
    // snapshot.
    if (overview_panning_offset_ == INT_MAX)
      CenterCurrentSnapshot(-1, -1);

    // There's two modes here: one where the snapshots are too wide
    // to fit, and one where they aren't.  Just so happens that we
    // want to do similar things in both cases.
    if (overview_panning_offset_ < min_x) {
      overview_panning_offset_ = min_x;
    } else {
      if (snapshots_.size() > 0) {
        if (overview_panning_offset_ > max_x)
          overview_panning_offset_ = max_x;
      } else {
        // If there are no snapshots, might as well center it all.
        overview_panning_offset_ = width_ / 2;
      }
    }
  }

  if (background_.get()) {
    // Now we scroll the background to the right location.
    const float kMargin = width_;
    int panning_min_x = -overview_width_of_snapshots_;
    int panning_max_x = kMargin;
    int background_overage = background_->GetWidth() - wm_->width();
    float scroll_percent = 1.0f -
        static_cast<float>(overview_panning_offset_ - panning_min_x) /
        (panning_max_x - panning_min_x);
    scroll_percent = max(0.f, scroll_percent);
    scroll_percent = min(scroll_percent, 1.f);
    overview_background_offset_ = -background_overage * scroll_percent;
  }
}

void LayoutManager::CycleCurrentToplevelWindow(bool forward) {
  if (mode_ != MODE_ACTIVE) {
    LOG(WARNING) << "Ignoring request to cycle active toplevel outside of "
                 << "active mode (current mode is " << mode_ << ")";
    return;
  }
  if (toplevels_.empty())
    return;

  if (wm_->key_bindings()->current_event_time()) {
    const KeyBindings::KeyCombo& combo =
        wm_->key_bindings()->current_key_combo();
    if (forward) {
      if (combo.keysym == XK_Tab)
        wm_->ReportUserAction("Accel_NextWindow_Tab");
      else if (combo.keysym == XK_F5)
        wm_->ReportUserAction("Accel_NextWindow_F5");
    } else {
      if (combo.keysym == XK_Tab)
        wm_->ReportUserAction("Accel_PrevWindow_Tab");
      else if (combo.keysym == XK_F5)
        wm_->ReportUserAction("Accel_PrevWindow_F5");
    }
  }

  ToplevelWindow* toplevel = NULL;
  if (!current_toplevel_) {
    toplevel = forward ?
               toplevels_[0].get() :
               toplevels_[toplevels_.size()-1].get();
  } else {
    if (toplevels_.size() == 1) {
      current_toplevel_->DoNudgeAnimation(forward);
      return;
    }

    int old_index = GetIndexForToplevelWindow(*current_toplevel_);
    int new_index = (toplevels_.size() + old_index + (forward ? 1 : -1)) %
                    toplevels_.size();
    toplevel = toplevels_[new_index].get();
  }
  CHECK(toplevel);

  SetCurrentToplevel(toplevel);
  if (mode_ == MODE_ACTIVE)
    LayoutWindows(true);
}

void LayoutManager::CycleCurrentSnapshotWindow(bool forward) {
  if (mode_ != MODE_OVERVIEW) {
    LOG(WARNING) << "Ignoring request to cycle current snapshot outside of "
                 << "overview mode (current mode is " << GetModeName(mode_)
                 << ")";
    return;
  }
  if (snapshots_.empty())
    return;
  if (current_snapshot_ && snapshots_.size() == 1)
    return;

  if (!current_snapshot_) {
    UpdateCurrentSnapshot();
  } else {
    int old_index = GetIndexForSnapshotWindow(*current_snapshot_);
    int new_index = old_index + (forward ? 1 : -1);

    // This clamps the snapshot index to the ends.
    new_index = min(new_index, static_cast<int>(snapshots_.size()) - 1);
    new_index = max(new_index, 0);

    XTime event_time = wm_->key_bindings()->current_event_time();
    // If this is the result of a key press, then we want to
    // use the event time from that key press.
    if (event_time) {
      SetCurrentSnapshotWithClick(snapshots_[new_index].get(),
                                  event_time, -1, -1);
    } else {
      SetCurrentSnapshot(snapshots_[new_index].get());
    }
  }
  if (mode_ == MODE_OVERVIEW)
    LayoutWindows(true);
}

void LayoutManager::SetCurrentSnapshot(SnapshotWindow* snapshot) {
  SetCurrentSnapshotWithClick(snapshot,
                              wm_->GetCurrentTimeFromServer(),
                              -1, -1);
}

void LayoutManager::SetCurrentSnapshotWithClick(SnapshotWindow* snapshot,
                                                XTime timestamp,
                                                int x, int y) {
  CHECK(snapshot);

  if (current_snapshot_ != snapshot) {
    if (mode_ != MODE_OVERVIEW) {
      current_snapshot_ = snapshot;
      current_snapshot_->SetState(SnapshotWindow::STATE_ACTIVE_MODE_INVISIBLE);
      return;
    }

    // Tell the old current snapshot that it's not current anymore.
    if (current_snapshot_)
      current_snapshot_->SetState(SnapshotWindow::STATE_OVERVIEW_MODE_NORMAL);

    current_snapshot_ = snapshot;
    DLOG(INFO) << "Set current snapshot to "
               << current_snapshot_->win()->xid_str();

    // Tell the snapshot that it's been selected.
    current_snapshot_->SetState(SnapshotWindow::STATE_OVERVIEW_MODE_SELECTED);

    // Since we switched snapshots, we may have switched current
    // toplevel windows.
    if (current_snapshot_->toplevel())
      SetCurrentToplevel(current_snapshot_->toplevel());

    // Detect a change in the current snapshot and report it to
    // Chrome, but only in overview mode.  We keep a timestamp of when
    // we sent this to Chrome, and then we ignore any events that
    // happened earlier than this timestamp.
    if (current_snapshot_ && current_toplevel_ &&
        current_snapshot_->toplevel() == current_toplevel_ &&
        current_toplevel_->selected_tab() != current_snapshot_->tab_index()) {
      current_toplevel_->SendTabSelectedMessage(current_snapshot_->tab_index(),
                                                timestamp);
    }

    CalculatePositionsForOverviewMode(false);
    CenterCurrentSnapshot(x, y);
  }
}

void LayoutManager::SendModeMessage(ToplevelWindow* toplevel, bool cancelled) {
  if (!toplevel ||
      toplevel->win()->type() != chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL)
    return;

  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_LAYOUT_MODE);
  switch (mode_) {
    // Set the mode in the message using the appropriate value from wm_ipc.h.
    case MODE_ACTIVE:
      msg.set_param(0, 0);
      break;
    case MODE_OVERVIEW:
      msg.set_param(0, 1);
      break;
    default:
      CHECK(false) << "Unhandled mode " << mode_;
      break;
  }
  msg.set_param(1, cancelled);
  wm_->wm_ipc()->SendMessage(toplevel->win()->xid(), msg);
}

void LayoutManager::PanOverviewMode(int offset) {
  overview_panning_offset_ += offset;
  if (mode_ == MODE_OVERVIEW)
    LayoutWindows(true);  // animate = true
}

void LayoutManager::UpdateOverviewPanningForMotion() {
  int dx = overview_background_event_coalescer_->x() - overview_drag_last_x_;
  overview_drag_last_x_ = overview_background_event_coalescer_->x();
  overview_panning_offset_ += dx;
  LayoutWindows(false);  // animate = false
}

void LayoutManager::DisplayAndFocusToplevel(ToplevelWindow* toplevel) {
  DCHECK(toplevel);

  bool switched_toplevel = false;
  if (current_toplevel_ != toplevel) {
    SetCurrentToplevel(toplevel);
    switched_toplevel = true;
  }

  if (mode_ == MODE_ACTIVE) {
    if (switched_toplevel)
      LayoutWindows(true);
    else
      toplevel->TakeFocus(wm_->GetCurrentTimeFromServer());
  } else {
    SetMode(MODE_ACTIVE);
  }
}

void LayoutManager::EnableKeyBindingsForMode(Mode mode) {
  switch (mode) {
    case MODE_ACTIVE:
      active_mode_key_bindings_group_->Enable();
      break;
    case MODE_OVERVIEW:
      overview_mode_key_bindings_group_->Enable();
      break;
    default:
      NOTREACHED() << "Unhandled mode " << mode;
  }
}

void LayoutManager::DisableKeyBindingsForMode(Mode mode) {
  switch (mode) {
    case MODE_ACTIVE:
      active_mode_key_bindings_group_->Disable();
      break;
    case MODE_OVERVIEW:
      overview_mode_key_bindings_group_->Disable();
      break;
    default:
      NOTREACHED() << "Unhandled mode " << mode;
  }
}

void LayoutManager::UpdateCurrentSnapshot() {
  if (snapshots_.empty()) {
    current_snapshot_ = NULL;
    return;
  }

  if (current_toplevel_) {
    int selected_tab = current_toplevel_->selected_tab();
    // Go through the snapshots and find the one that corresponds to
    // the selected tab in the current toplevel window.
    for (SnapshotWindows::iterator it = snapshots_.begin();
         it != snapshots_.end(); ++it) {
      if ((*it)->tab_index() == selected_tab &&
          (*it)->toplevel() == current_toplevel_) {
        SetCurrentSnapshot((*it).get());
        return;
      }
    }
    LOG(WARNING) << "Unable to find snapshot in current toplevel "
                 << "for selected tab " << selected_tab;
  }

  // If we don't have an active toplevel window, then just take the
  // first snapshot.
  SetCurrentSnapshot(snapshots_[0].get());
}

void LayoutManager::RemoveSnapshot(SnapshotWindow* snapshot) {
  DCHECK(snapshot);
  if (!snapshot)
    return;

  const int index = GetIndexForSnapshotWindow(*snapshot);
  if (index < 0) {
    LOG(WARNING) << "Snapshot " << snapshot->win()->xid_str()
                 << " index not found.";
    return;
  }

  DLOG(INFO) << "Removing snapshot " << snapshot->win()->xid_str()
             << " at index " << index;

  if (current_snapshot_ == snapshot)
    current_snapshot_ = NULL;

  // Find any input windows associated with this snapshot and remove
  // them.
  XWindowToSnapshotMap::iterator input_iter = input_to_snapshot_.begin();
  while (input_iter != input_to_snapshot_.end()) {
    if (input_iter->second == snapshot)
      input_to_snapshot_.erase(input_iter);
    ++input_iter;
  }

  snapshots_.erase(snapshots_.begin() + index);

  // Find a new current snapshot if we were in overview mode.
  if (mode_ == MODE_OVERVIEW) {
    if (!current_snapshot_) {
      if (!snapshots_.empty()) {
        const int new_index =
            (index + snapshots_.size() - 1) % snapshots_.size();
        SetCurrentSnapshot(snapshots_[new_index].get());
      }
    }
  }
}

void LayoutManager::RemoveToplevel(ToplevelWindow* toplevel) {
  DCHECK(toplevel);
  if (!toplevel)
    return;

  const int index = GetIndexForToplevelWindow(*toplevel);
  if (index < 0) {
    LOG(WARNING) << "Toplevel " << toplevel->win()->xid_str()
                 << " index not found.";
    return;
  }

  Window* win = toplevel->win();
  DLOG(INFO) << "Removing toplevel " << toplevel->win()->xid_str()
             << " at index " << index;

  // Find any transient windows associated with this toplevel window
  // and remove them.
  XWindowToToplevelMap::iterator transient_iter =
      transient_to_toplevel_.begin();
  while (transient_iter != transient_to_toplevel_.end()) {
    if (transient_iter->second == toplevel) {
      HandleTransientWindowModalityChange(
          wm_->GetWindowOrDie(transient_iter->first), true);  // unmapped=true
      transient_to_toplevel_.erase(transient_iter);
    }
    ++transient_iter;
  }

  // Find any snapshots that reference this toplevel window, and
  // remove them.
  {
    SnapshotWindows remaining;
    for (SnapshotWindows::iterator it = snapshots_.begin();
         it != snapshots_.end(); ++it) {
      if ((*it)->toplevel() != toplevel)
        remaining.push_back(*it);
    }
    snapshots_.swap(remaining);
  }

  // Find a new active toplevel window if needed.  If there's no
  // active window now, then this one was probably active previously.
  // Choose a new active window if possible; relinquish the focus
  // otherwise.
  if (current_toplevel_ == toplevel) {
    if (toplevels_.size() > 1) {
      // If we close the first window in the cycle, we will activate the second
      // window, otherwise we activate the previous window in the cycle.  Make
      // sure the current_toplevel is removed after calling SetCurrentToplevel
      // to get the proper animation.
      const int new_index = index == 0 ? 1 : index - 1;
      SetCurrentToplevel(toplevels_[new_index].get());
    } else {
      current_toplevel_ = NULL;
      if (mode_ == MODE_ACTIVE && win->IsFocused())
        wm_->TakeFocus(wm_->GetCurrentTimeFromServer());
    }
  }
  if (fullscreen_toplevel_ == toplevel)
    fullscreen_toplevel_ = NULL;
  toplevels_.erase(toplevels_.begin() + index);
  UpdateCurrentSnapshot();
}

bool LayoutManager::SortSnapshots() {
  SnapshotWindows old_snapshots = snapshots_;
  std::sort(snapshots_.begin(), snapshots_.end(),
            SnapshotWindow::CompareTabIndex);
  return old_snapshots != snapshots_;
}

void LayoutManager::AddOrRemoveSeparatorsAsNeeded() {
  // If there aren't at least two toplevels, then we don't need any
  // separators.
  if (toplevels_.size() < 2) {
    separators_.clear();
    return;
  }

  // Make sure there are n-1 separators available for placing
  // between groups of snapshots.

  // Count only "real" chrome toplevel windows, because other toplevel
  // types don't produce snapshot groups.
  ToplevelWindows::iterator iter = toplevels_.begin();
  Separators::size_type num_separators_desired = 0;
  while (iter != toplevels_.end()) {
    if ((*iter)->win()->type() == chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL)
      num_separators_desired++;
    ++iter;
  }

  if (num_separators_desired > 1) {
    // We want n-1 separators, so decrement by one, but make sure it's
    // not negative.
    --num_separators_desired;

    // Add any that are needed.
    while (separators_.size() < num_separators_desired) {
      separators_.push_back(shared_ptr<Separator>(new Separator(this)));
    }

    // And also make sure there aren't too many.
    while (separators_.size() > num_separators_desired) {
      separators_.erase(separators_.begin());
    }
  } else {
    separators_.clear();
  }
}

int LayoutManager::GetPreceedingTabCount(const ToplevelWindow& toplevel) const {
  int count = 0;
  for (ToplevelWindows::const_iterator it = toplevels_.begin();
       it != toplevels_.end(); ++it) {
    if (it->get() == &toplevel)
      return count;
    count += (*it)->tab_count();
  }
  return count;
}

void LayoutManager::MakeToplevelFullscreen(ToplevelWindow* toplevel) {
  DCHECK(toplevel);
  if (toplevel->is_fullscreen()) {
    LOG(WARNING) << "Ignoring request to fullscreen already-fullscreen "
                 << "toplevel window " << toplevel->win()->xid_str();
    return;
  }

  if (fullscreen_toplevel_)
    RestoreFullscreenToplevel(fullscreen_toplevel_);

  if (toplevel != current_toplevel_) {
    SetCurrentToplevel(toplevel);
    LayoutWindows(true);
  }
  if (!toplevel->IsWindowOrTransientFocused())
    toplevel->TakeFocus(wm_->GetCurrentTimeFromServer());
  toplevel->SetFullscreenState(true);
  fullscreen_toplevel_ = toplevel;
}

void LayoutManager::RestoreFullscreenToplevel(ToplevelWindow* toplevel) {
  DCHECK(toplevel);
  if (!toplevel->is_fullscreen()) {
    LOG(WARNING) << "Ignoring request to restore non-fullscreen "
                 << "toplevel window " << toplevel->win()->xid_str();
    return;
  }
  toplevel->SetFullscreenState(false);
  if (fullscreen_toplevel_ == toplevel)
    fullscreen_toplevel_ = NULL;
}

void LayoutManager::SetBackground(Compositor::Actor* actor) {
  DCHECK(actor);
  background_.reset(actor);
  background_->SetName("overview mode background");
  if (first_toplevel_chrome_window_mapped_)
    background_->Show();
  else
    background_->Hide();
  ConfigureBackground(wm_->width(), wm_->height());
  wm_->stage()->AddActor(background_.get());
  wm_->stacking_manager()->StackActorAtTopOfLayer(
      background_.get(), StackingManager::LAYER_BACKGROUND);
}

void LayoutManager::ConfigureBackground(int width, int height) {
  if (!background_.get())
    return;

  // Calculate the expansion of the background image.  It should be
  // zoomed to preserve aspect ratio and fill the screen, and then
  // scaled up by kBackgroundExpansionFactor so that it is wider
  // than the physical display so that we can scroll it horizontally
  // when the user switches tabs in overview mode.
  double image_aspect = static_cast<double>(background_->GetWidth()) /
                        static_cast<double>(background_->GetHeight());
  double display_aspect = static_cast<double>(width) /
                          static_cast<double>(height);
  int background_height, background_width;
  if (image_aspect > display_aspect) {
    // Image is wider than the display, scale image height to match
    // the height of the display, and the image width to preserve
    // the image ratio, and then expand them both to make it wide
    // enough for scrolling.  The "+.5"'s are for proper rounding.
    background_height = height;
    background_width = height * image_aspect + 0.5f;

    if (background_width < width * kBackgroundExpansionFactor) {
      // Even with the tall aspect ratio we have, the width still
      // isn't wide enough, so we scale up the image some more so it
      // is wide enough, preserving the aspect.
      float extra_expansion =
          width * kBackgroundExpansionFactor / background_width;
      background_width = background_width * extra_expansion + 0.5f;
      background_height = background_height * extra_expansion + 0.5f;
    }
  } else {
    // Image is narrower than the display, scale image width to
    // match the width of the display, and the image height to
    // preserve the image ratio, and then expand them both to make
    // it wide enough for scrolling.
    background_width = 0.5f + kBackgroundExpansionFactor * width;
    background_height = 0.5f + kBackgroundExpansionFactor * width /
                        image_aspect;
  }

  DLOG(INFO) << "Configuring background image of size "
             << background_->GetWidth() << "x" << background_->GetHeight()
             << " as " << background_width << "x" << background_height
             << " for " << width << "x" << height << " display";

  background_->Scale(
      static_cast<float>(background_width) / background_->GetWidth(),
      static_cast<float>(background_height) / background_->GetHeight(),
      0);  // anim_ms

  // Center the image vertically.
  background_->Move(0, (height - background_height) / 2, 0);
}

void LayoutManager::HandleFirstToplevelChromeWindowMapped(Window* win) {
  DCHECK(win);

  // Start drawing our background when we see the first Chrome window.
  if (background_.get())
    background_->Show();

  post_toplevel_key_bindings_group_->Enable();

  if (!FLAGS_initial_chrome_window_mapped_file.empty()) {
    DLOG(INFO) << "Writing initial Chrome window's ID to file "
               << FLAGS_initial_chrome_window_mapped_file;
    FILE* file = fopen(FLAGS_initial_chrome_window_mapped_file.c_str(), "w+");
    if (!file) {
      PLOG(ERROR) << "Unable to open file "
                  << FLAGS_initial_chrome_window_mapped_file;
    } else {
      fprintf(file, "%lu", win->xid());
      fclose(file);
    }
  }
}

void LayoutManager::HandleTransientWindowModalityChange(
    Window* transient_win, bool window_or_owner_was_unmapped) {
  DCHECK(transient_win);

  const bool was_modal = modal_transients_.count(transient_win);
  const bool is_modal = !window_or_owner_was_unmapped &&
                        transient_win->wm_state_modal();
  if (was_modal == is_modal)
    return;

  const bool previously_had_modal_transients = !modal_transients_.empty();

  if (is_modal) {
    modal_transients_.insert(transient_win);
    ToplevelWindow* owner =
        GetToplevelWindowOwningTransientWindow(*transient_win);
    DCHECK(owner);
    if (owner)
      DisplayAndFocusToplevel(owner);
  } else {
    modal_transients_.erase(transient_win);

    // If there are still other modal windows, focus one of them.
    if (!modal_transients_.empty()) {
      Window* new_win_to_focus = *(modal_transients_.begin());
      ToplevelWindow* owner =
          GetToplevelWindowOwningTransientWindow(*new_win_to_focus);
      DCHECK(owner);
      if (owner)
        DisplayAndFocusToplevel(owner);
    }
  }

  if (previously_had_modal_transients && modal_transients_.empty())
    EnableKeyBindingsForMode(mode_);
  else if (!previously_had_modal_transients && !modal_transients_.empty())
    DisableKeyBindingsForMode(mode_);
}

}  // namespace window_manager
