// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/layout_manager.h"

#include <algorithm>
#include <cmath>
#include <tr1/memory>

#include <gflags/gflags.h>

#include "base/string_util.h"
#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"
#include "window_manager/callback.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/geometry.h"
#include "window_manager/motion_event_coalescer.h"
#include "window_manager/snapshot_window.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/system_metrics.pb.h"
#include "window_manager/toplevel_window.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x_connection.h"

DEFINE_bool(lm_honor_window_size_hints, false,
            "When maximizing a client window, constrain its size according to "
            "the size hints that the client app has provided (e.g. max size, "
            "size increment, etc.) instead of automatically making it fill the "
            "screen");

using std::map;
using std::max;
using std::min;
using std::string;
using std::tr1::shared_ptr;

namespace window_manager {

// What's the maximum fraction of the manager's total size that a window
// should be scaled to in overview mode?
static const double kOverviewWindowMaxSizeRatio = 0.5;

// How many pixels should be used for padding the snapshot on the
// right side when it is selected.
static const double kOverviewSelectedPadding = 4.0;

// What fraction of the manager's total width should be placed between
// groups of snapshots in overview mode?
static const double kOverviewGroupSpacing = 0.05;

// Duration between panning updates while a drag is occurring on the
// background window in overview mode.
static const int kOverviewDragUpdateMs = 50;

// What fraction of the manager's total width should each window use for
// peeking out underneath the window on top of it in overview mode?
const double LayoutManager::kOverviewExposedWindowRatio = 0.02;

// This is the speed at which the background image moves relative to
// how much the snapshots move when a new snapshot is selected.  Note
// that if there are enough snapshots that scrolling to the last one
// would cause the background to not cover the entire screen, then
// this ratio is ignored (and a smaller ratio is calculuated).
const double LayoutManager::kBackgroundScrollRatio = 0.33;

// Animation speed used for windows.
const int LayoutManager::kWindowAnimMs = 250;

// How much should we scale a snapshot window if it is selected?
const double LayoutManager::kOverviewSelectedScale = 1.1;

// Animation speed used for opacity of windows.
const int LayoutManager::kWindowOpacityAnimMs =
    LayoutManager::kWindowAnimMs / 4;

LayoutManager::LayoutManager(WindowManager* wm, PanelManager* panel_manager)
    : wm_(wm),
      panel_manager_(panel_manager),
      mode_(MODE_NEW),
      x_(0),
      y_(0),
      width_(wm_->width()),
      height_(wm_->height()),
      panel_manager_left_width_(0),
      panel_manager_right_width_(0),
      current_toplevel_(NULL),
      current_snapshot_(NULL),
      overview_panning_offset_(0),
      overview_background_offset_(0),
      overview_background_event_coalescer_(
          new MotionEventCoalescer(
              wm_->event_loop(),
              NewPermanentCallback(
                  this, &LayoutManager::UpdateOverviewPanningForMotion),
              kOverviewDragUpdateMs)),
      overview_drag_last_x_(-1),
      saw_map_request_(false),
      event_consumer_registrar_(new EventConsumerRegistrar(wm, this)),
      key_bindings_enabled_(true),
      active_mode_key_bindings_group_(new KeyBindingsGroup(wm->key_bindings())),
      overview_mode_key_bindings_group_(
          new KeyBindingsGroup(wm->key_bindings())) {
  panel_manager_->RegisterAreaChangeListener(this);
  panel_manager_->GetArea(&panel_manager_left_width_,
                          &panel_manager_right_width_);

  // Disable the overview key bindings, since we start in active mode.
  overview_mode_key_bindings_group_->Disable();

  MoveAndResizeForAvailableArea();

  int event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
  wm_->xconn()->AddButtonGrabOnWindow(
      wm_->background_xid(), 1, event_mask, false);
  event_consumer_registrar_->RegisterForWindowEvents(wm_->background_xid());

  KeyBindings* kb = wm_->key_bindings();

  kb->AddAction(
      "switch-to-overview-mode",
      NewPermanentCallback(this, &LayoutManager::SetMode, MODE_OVERVIEW),
      NULL, NULL);
  active_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_F12, 0), "switch-to-overview-mode");

  kb->AddAction(
      "switch-to-active-mode",
      NewPermanentCallback(this, &LayoutManager::SetMode,
                           MODE_ACTIVE_CANCELLED),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Escape, 0), "switch-to-active-mode");
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_F12, 0), "switch-to-active-mode");

  kb->AddAction(
      "cycle-active-forward",
      NewPermanentCallback(
          this, &LayoutManager::CycleCurrentToplevelWindow, true),
      NULL, NULL);
  active_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Tab, KeyBindings::kAltMask),
      "cycle-active-forward");
  active_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_F2, KeyBindings::kAltMask),
      "cycle-active-forward");

  kb->AddAction(
      "cycle-active-backward",
      NewPermanentCallback(
          this, &LayoutManager::CycleCurrentToplevelWindow, false),
      NULL, NULL);
  active_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(
          XK_Tab, KeyBindings::kAltMask | KeyBindings::kShiftMask),
      "cycle-active-backward");
  active_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_F1, KeyBindings::kAltMask),
      "cycle-active-backward");

  kb->AddAction(
      "cycle-magnification-forward",
      NewPermanentCallback(
          this, &LayoutManager::CycleCurrentSnapshotWindow, true),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Right, 0), "cycle-magnification-forward");
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Tab, KeyBindings::kAltMask),
      "cycle-magnification-forward");
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_F2, KeyBindings::kAltMask),
      "cycle-magnification-forward");

  kb->AddAction(
      "cycle-magnification-backward",
      NewPermanentCallback(
          this, &LayoutManager::CycleCurrentSnapshotWindow, false),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Left, 0), "cycle-magnification-backward");
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(
          XK_Tab, KeyBindings::kAltMask | KeyBindings::kShiftMask),
      "cycle-magnification-backward");
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_F1, KeyBindings::kAltMask),
      "cycle-magnification-backward");

  kb->AddAction(
      "switch-to-active-mode-for-selected",
      NewPermanentCallback(this, &LayoutManager::SetMode, MODE_ACTIVE),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Return, 0),
      "switch-to-active-mode-for-selected");

  for (int i = 0; i < 8; ++i) {
    kb->AddAction(
        StringPrintf("activate-toplevel-with-index-%d", i),
        NewPermanentCallback(
            this, &LayoutManager::HandleToplevelChangeRequest, i),
        NULL, NULL);
    active_mode_key_bindings_group_->AddBinding(
        KeyBindings::KeyCombo(XK_1 + i, KeyBindings::kAltMask),
        StringPrintf("activate-toplevel-with-index-%d", i));

    kb->AddAction(
        StringPrintf("select-snapshot-with-index-%d", i),
        NewPermanentCallback(
            this, &LayoutManager::HandleSnapshotChangeRequest, i),
        NULL, NULL);
    overview_mode_key_bindings_group_->AddBinding(
        KeyBindings::KeyCombo(XK_1 + i, KeyBindings::kAltMask),
        StringPrintf("select-toplevel-with-index-%d", i));
  }

  kb->AddAction(
      "activate-last-toplevel",
      NewPermanentCallback(
          this, &LayoutManager::HandleToplevelChangeRequest, -1),
      NULL, NULL);
  active_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_9, KeyBindings::kAltMask),
      "activate-last-toplevel");

  kb->AddAction(
      "select-last-snapshot",
      NewPermanentCallback(
          this, &LayoutManager::HandleSnapshotChangeRequest, -1),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_9, KeyBindings::kAltMask),
      "select-last-toplevel");

  // TODO: When we support closing tabs in snapshot mode, we should
  // bind that function to ctrl-w here.
  kb->AddAction(
      "delete-active-window",
      NewPermanentCallback(
          this, &LayoutManager::SendDeleteRequestToCurrentToplevel),
      NULL, NULL);
  active_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(
          XK_w, KeyBindings::kControlMask | KeyBindings::kShiftMask),
      "delete-active-window");

  kb->AddAction(
      "pan-overview-mode-left",
      NewPermanentCallback(this, &LayoutManager::PanOverviewMode, -50),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_h, KeyBindings::kAltMask),
      "pan-overview-mode-left");

  kb->AddAction(
      "pan-overview-mode-right",
      NewPermanentCallback(this, &LayoutManager::PanOverviewMode, 50),
      NULL, NULL);
  overview_mode_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_l, KeyBindings::kAltMask),
      "pan-overview-mode-right");

  SetMode(MODE_ACTIVE);
}

LayoutManager::~LayoutManager() {
  panel_manager_->UnregisterAreaChangeListener(this);
  wm_->xconn()->RemoveButtonGrabOnWindow(wm_->background_xid(), 1);

  KeyBindings* kb = wm_->key_bindings();
  kb->RemoveAction("switch-to-overview-mode");
  kb->RemoveAction("switch-to-active-mode");
  kb->RemoveAction("cycle-active-forward");
  kb->RemoveAction("cycle-active-backward");
  kb->RemoveAction("cycle-magnification-forward");
  kb->RemoveAction("cycle-magnification-backward");
  kb->RemoveAction("switch-to-active-mode-for-selected");
  for (int i = 0; i < 8; ++i) {
    kb->RemoveAction(StringPrintf("activate-toplevel-with-index-%d", i));
    kb->RemoveAction(StringPrintf("select-snapshot-with-index-%d", i));
  }
  kb->RemoveAction("activate-last-toplevel");
  kb->RemoveAction("delete-active-window");
  kb->RemoveAction("pan-overview-mode-left");
  kb->RemoveAction("pan-overview-mode-right");

  toplevels_.clear();
  snapshots_.clear();
}

bool LayoutManager::IsInputWindow(XWindow xid) {
  return (GetSnapshotWindowByInputXid(xid) != NULL);
}

void LayoutManager::HandleScreenResize() {
  MoveAndResizeForAvailableArea();
}

bool LayoutManager::HandleWindowMapRequest(Window* win) {
  DCHECK(win);
  saw_map_request_ = true;
  if (!wm_->logged_in())
    return false;

  if (!IsHandledWindowType(win->type()))
    return false;

  if (win->type() == chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT) {
    wm_->stacking_manager()->StackWindowAtTopOfLayer(
        win, StackingManager::LAYER_SNAPSHOT_WINDOW);
  } else {
    wm_->stacking_manager()->StackWindowAtTopOfLayer(
        win, StackingManager::LAYER_TOPLEVEL_WINDOW);
  }
  win->MapClient();
  return true;
}

void LayoutManager::HandleWindowMap(Window* win) {
  DCHECK(win);
  if (!wm_->logged_in())
    return;

  // Just show override-redirect windows; they're already positioned
  // according to client apps' wishes.
  if (win->override_redirect()) {
    win->ShowComposited();
    return;
  }

  if (!IsHandledWindowType(win->type()))
    return;

  DLOG(INFO) << "Handling window map for " << win->title()
             << " (" << win->xid_str() << ") of type " << win->type();

  switch (win->type()) {
    case chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT: {
      // Register to get property changes for snapshot windows.
      event_consumer_registrar_->RegisterForPropertyChanges(
          win->xid(), wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));

      if (!saw_map_request_)
        wm_->stacking_manager()->StackWindowAtTopOfLayer(
            win, StackingManager::LAYER_SNAPSHOT_WINDOW);
      shared_ptr<SnapshotWindow> snapshot(new SnapshotWindow(win, this));
      input_to_snapshot_[snapshot->input_xid()] = snapshot.get();
      snapshots_.push_back(snapshot);
      SortSnapshots();
      DLOG(INFO) << "Adding snapshot " << win->xid_str()
                << " at tab index " << snapshot->tab_index()
                << " (total of " << snapshots_.size() << ")";
      UpdateCurrentSnapshot();
      if (mode_ == MODE_OVERVIEW) {
        if (snapshot.get() == current_snapshot_) {
          snapshot->SetState(SnapshotWindow::STATE_OVERVIEW_MODE_SELECTED);
        } else {
          snapshot->SetState(SnapshotWindow::STATE_OVERVIEW_MODE_NORMAL);
        }
      } else {
        snapshot->SetState(SnapshotWindow::STATE_ACTIVE_MODE_INVISIBLE);
      }
      break;
    }
    case chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL:
      // Register to get property changes for toplevel windows.
      event_consumer_registrar_->RegisterForPropertyChanges(
          win->xid(), wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));
      // FALL THROUGH...
    case chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE:
    case chromeos::WM_IPC_WINDOW_UNKNOWN: {
      // Perform initial setup of windows that were already mapped at startup
      // (so we never saw MapRequest events for them).
      if (!saw_map_request_)
        wm_->stacking_manager()->StackWindowAtTopOfLayer(
            win, StackingManager::LAYER_TOPLEVEL_WINDOW);

      if (win->transient_for_xid() != None) {
        ToplevelWindow* toplevel_owner =
            GetToplevelWindowByXid(win->transient_for_xid());
        if (toplevel_owner) {
          transient_to_toplevel_[win->xid()] = toplevel_owner;
          toplevel_owner->AddTransientWindow(win, mode_ == MODE_OVERVIEW);

          if (mode_ == MODE_ACTIVE &&
              current_toplevel_ != NULL &&
              current_toplevel_->IsWindowOrTransientFocused())
            current_toplevel_->TakeFocus(wm_->GetCurrentTimeFromServer());
          break;
        } else {
          LOG(WARNING) << "Ignoring " << win->xid_str()
                       << "'s WM_TRANSIENT_FOR hint of "
                       << XidStr(win->transient_for_xid())
                       << ", which isn't a toplevel window";
          // Continue on and treat the transient as a toplevel window.
        }
      }

      shared_ptr<ToplevelWindow> toplevel(
          new ToplevelWindow(win, this));

      switch (mode_) {
        case MODE_NEW:
        case MODE_ACTIVE:
        case MODE_ACTIVE_CANCELLED:
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
      }

      // Tell the newly mapped window what the mode is so it'll map
      // the snapshot windows it has if we're in overview mode.
      SendModeMessage(toplevel.get());

      SetCurrentToplevel(toplevel.get());
      break;
    }
    default:
      NOTREACHED() << "Unexpected window type " << win->type();
      break;
  }

  LayoutWindows(true);
}

void LayoutManager::HandleWindowUnmap(Window* win) {
  // Note that we sometimes get spurious double unmap notifications
  // for the same window.  We ignore these by checking to see if we
  // can find the given window in our lists of toplevel and snapshot
  // windows (and if we've removed it already, we won't find it).
  DCHECK(win);

  DLOG(INFO) << "Unmapping window " << win->xid_str()
            << " of type " << win->type();

  if (!IsHandledWindowType(win->type()))
    return;

  if (win->type() == chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT) {
    SnapshotWindow* snapshot = GetSnapshotWindowByWindow(*win);
    if (snapshot) {
      event_consumer_registrar_->UnregisterForPropertyChanges(
          win->xid(), wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));

      RemoveSnapshot(GetSnapshotWindowByWindow(*win));
      LayoutWindows(true);
    }
  } else {
    ToplevelWindow* toplevel_owner =
        GetToplevelWindowOwningTransientWindow(*win);

    if (toplevel_owner) {
      bool transient_had_focus = win->focused();
      toplevel_owner->RemoveTransientWindow(win);
      if (transient_to_toplevel_.erase(win->xid()) != 1)
        LOG(WARNING) << "No transient-to-toplevel mapping for "
                     << win->xid_str();
      if (transient_had_focus)
        toplevel_owner->TakeFocus(wm_->GetCurrentTimeFromServer());
    }

    ToplevelWindow* toplevel = GetToplevelWindowByWindow(*win);
    if (toplevel) {
      if (win->type() == chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL)
        event_consumer_registrar_->UnregisterForPropertyChanges(
            win->xid(), wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));

      RemoveToplevel(toplevel);
      LayoutWindows(true);
    }
  }
}

void LayoutManager::HandleWindowConfigureRequest(
    Window* win, int req_x, int req_y, int req_width, int req_height) {
  if (win->type() == chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT) {
    SnapshotWindow* snapshot = GetSnapshotWindowByWindow(*win);
    if (snapshot)
      if (req_width != win->client_width() ||
          req_height != win->client_height())
        win->ResizeClient(req_width, req_height, GRAVITY_NORTHWEST);
  } else {
    ToplevelWindow* toplevel_owner =
        GetToplevelWindowOwningTransientWindow(*win);
    if (toplevel_owner) {
      toplevel_owner->HandleTransientWindowConfigureRequest(
          win, req_x, req_y, req_width, req_height);
      return;
    }

    ToplevelWindow* toplevel = GetToplevelWindowByWindow(*win);
    if (toplevel) {
      if (req_width != win->client_width() ||
          req_height != win->client_height())
        win->ResizeClient(req_width, req_height, GRAVITY_NORTHWEST);
    }
  }
}

void LayoutManager::HandleButtonPress(XWindow xid,
                                      int x, int y,
                                      int x_root, int y_root,
                                      int button,
                                      XTime timestamp) {
  SnapshotWindow* snapshot = GetSnapshotWindowByInputXid(xid);
  if (snapshot) {
    if (button == 1) {  // Ignore buttons other than 1.
      LOG_IF(WARNING, mode_ != MODE_OVERVIEW)
          << "Got a click in input window " << XidStr(xid)
          << " for snapshot window " << snapshot->win()->xid_str()
          << " while not in overview mode";
      snapshot->HandleButtonPress(timestamp);
    }
    return;
  }

  if (xid == wm_->background_xid() && button == 1) {
    overview_drag_last_x_ = x;
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
                                        int x, int y,
                                        int x_root, int y_root,
                                        int button,
                                        XTime timestamp) {
  if (xid != wm_->background_xid() || button != 1)
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
                                        int x, int y,
                                        int x_root, int y_root,
                                        XTime timestamp) {
  if (xid == wm_->background_xid())
    overview_background_event_coalescer_->StorePosition(x, y);
}

void LayoutManager::HandleFocusChange(XWindow xid, bool focus_in) {
  Window* win = wm_->GetWindow(xid);
  if (!win)
    return;

  ToplevelWindow* toplevel = GetToplevelWindowOwningTransientWindow(*win);
  if (!toplevel)
    toplevel = GetToplevelWindowByWindow(*win);

  // If this is not a toplevel or transient window, we don't care
  // about the focus change.
  if (!toplevel)
    return;

  toplevel->HandleFocusChange(win, focus_in);

  // Announce that the new window is the "active" window (in the
  // _NET_ACTIVE_WINDOW sense).
  if (focus_in)
    wm_->SetActiveWindowProperty(win->xid());
}

void LayoutManager::HandleClientMessage(XWindow xid,
                                        XAtom message_type,
                                        const long data[5]) {
  Window* win = wm_->GetWindow(xid);
  if (!win)
    return;

  if (message_type == wm_->GetXAtom(ATOM_NET_WM_STATE)) {
    // Just blindly apply whatever state properties the window asked for.
    map<XAtom, bool> states;
    win->ParseWmStateMessage(data, &states);
    win->ChangeWmState(states);
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
      if (mode_ == MODE_OVERVIEW || current_toplevel_ != toplevel) {
        SetCurrentToplevel(toplevel);
        // Jump out of overview mode if a toplevel has requested focus.
        if (mode_ == MODE_OVERVIEW)
          SetMode(MODE_ACTIVE);
        else
          LayoutWindows(true);
      }
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
  if (toplevels_.empty())
    return;

  // As a last resort, if we don't have a current toplevel when we
  // layout, pick the first one.
  if (!current_toplevel_)
    current_toplevel_ = toplevels_[0].get();

  DLOG(INFO) << "Laying out windows for " << GetModeName(mode_) << " mode.";

  if (mode_ == MODE_OVERVIEW)
    CalculatePositionsForOverviewMode();

  // We iterate through the snapshot windows in descending stacking
  // order (right-to-left).  Otherwise, we'd get spurious pointer
  // enter events as a result of stacking a window underneath the
  // pointer immediately before we stack the window to its right
  // directly on top of it.  Not a huge concern now that we're not
  // listening ofr enter and leave events, but that might change again
  // in the future.
  for (SnapshotWindows::reverse_iterator it = snapshots_.rbegin();
       it != snapshots_.rend(); ++it) {
    (*it)->UpdateLayout(animate);
  }
  for (ToplevelWindows::iterator it = toplevels_.begin();
       it != toplevels_.end(); ++it) {
    (*it)->UpdateLayout(animate);
  }

  if (wm_->background())
    wm_->background()->MoveX(overview_background_offset_, kWindowAnimMs);
}

void LayoutManager::SetMode(Mode mode) {
  if (mode == mode_)
    return;

  if (key_bindings_enabled_)
    DisableKeyBindingsForModeInternal(mode_);

  mode_ = mode;
  DLOG(INFO) << "Switching to " << GetModeName(mode_) << " mode.";

  switch (mode_) {
    case MODE_NEW:
    case MODE_ACTIVE:
      if (current_snapshot_ && current_snapshot_->toplevel()) {
        // Make sure they don't get out of sync -- if the snapshot is selected,
        // then the toplevel should have changed too.
        DCHECK_EQ(current_snapshot_->toplevel(), current_toplevel_);

        // Because we were told to change snapshots, this indicates that
        // the user wants this snapshot to now be current, so we tell
        // Chrome.
        // TODO: do some handshaking here to make sure that chrome
        // switches tabs before we start our animation.
        WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_TAB_SELECT);
        msg.set_param(0, current_snapshot_->tab_index());
        wm_->wm_ipc()->SendMessage(current_snapshot_->toplevel()->win()->xid(),
                                   msg);
      }
      // Cancelling actually happens on the chrome side, since it
      // knows what tabs used to be selected.  It knows to cancel
      // because it's a different layout mode.
      // FALL THROUGH...
    case MODE_ACTIVE_CANCELLED:
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
      break;
    case MODE_OVERVIEW: {
      UpdateCurrentSnapshot();
      if (current_toplevel_->IsWindowOrTransientFocused()) {
        // We need to take the input focus away here; otherwise the
        // previously-focused window would continue to get keyboard events
        // in overview mode.  Let the WindowManager decide what to do with it.
        wm_->SetActiveWindowProperty(None);
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
      break;
    }
  }

  LayoutWindows(true);

  // Let all Chrome windows know about the new layout mode so that
  // each toplevel window will map its associated snapshot windows.
  for (ToplevelWindows::iterator it = toplevels_.begin();
       it != toplevels_.end(); ++it) {
    SendModeMessage(it->get());
  }

  // Done cancelling.
  if (mode_ == MODE_ACTIVE_CANCELLED)
    mode_ = MODE_ACTIVE;

  if (key_bindings_enabled_)
    EnableKeyBindingsForModeInternal(mode_);
}

void LayoutManager::EnableKeyBindings() {
  if (key_bindings_enabled_)
    return;
  EnableKeyBindingsForModeInternal(mode_);
  key_bindings_enabled_ = true;
}

void LayoutManager::DisableKeyBindings() {
  if (!key_bindings_enabled_)
    return;
  DisableKeyBindingsForModeInternal(mode_);
  key_bindings_enabled_ = false;
}

// static
bool LayoutManager::IsHandledWindowType(chromeos::WmIpcWindowType type) {
  return (type == chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE ||
          type == chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT ||
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

XWindow LayoutManager::GetInputXidForWindow(const Window& win) {
  SnapshotWindow* snapshot = GetSnapshotWindowByWindow(win);
  return snapshot ? snapshot->input_xid() : None;
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
    int width = width_;
    int height = height_;
    if (FLAGS_lm_honor_window_size_hints)
      (*it)->win()->GetMaxSize(width_, height_, &width, &height);
    (*it)->win()->ResizeClient(width, height, resize_gravity);
    if (mode_ == MODE_OVERVIEW) {
      // Make sure the toplevel windows are offscreen still if we're
      // in overview mode.
      (*it)->win()->MoveClientOffscreen();
    }
  }

  // Make sure the snapshot windows are offscreen still.
  for (SnapshotWindows::iterator it = snapshots_.begin();
       it != snapshots_.end(); ++it) {
    (*it)->win()->MoveClientOffscreen();
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

void LayoutManager::Metrics::Populate(chrome_os_pb::SystemMetrics *metrics_pb) {
  CHECK(metrics_pb);
  metrics_pb->Clear();
  metrics_pb->set_overview_keystroke_count(overview_by_keystroke_count);
  metrics_pb->set_overview_exit_mouse_count(overview_exit_by_mouse_count);
  metrics_pb->set_overview_exit_keystroke_count(
      overview_exit_by_keystroke_count);
  metrics_pb->set_keystroke_window_cycling_count(
      window_cycle_by_keystroke_count);
}

void LayoutManager::CalculatePositionsForOverviewMode() {
  if (toplevels_.empty() || snapshots_.empty() || mode_ != MODE_OVERVIEW)
    return;

  int selected_x = 0.5 * width_;

  const int width_limit =
      min(static_cast<double>(width_) / sqrt(snapshots_.size()),
          kOverviewWindowMaxSizeRatio * width_);
  const int height_limit =
      min(static_cast<double>(height_) / sqrt(snapshots_.size()),
          kOverviewWindowMaxSizeRatio * height_);
  ToplevelWindow* last_toplevel = snapshots_[0]->toplevel();
  int running_width = 0;
  int selected_index = 0;
  int selected_offset = 0;
  for (int i = 0; static_cast<size_t>(i) < snapshots_.size(); ++i) {
    SnapshotWindow* snapshot = snapshots_[i].get();
    bool is_selected = (snapshot == current_snapshot_);

    if (snapshot->toplevel() != last_toplevel)
      running_width += width_ * kOverviewGroupSpacing;

    if (is_selected) {
      selected_index = i;
      selected_offset = running_width;
    }

    double scale = is_selected ? kOverviewSelectedScale : 1.0;
    snapshot->SetSize(width_limit * scale, height_limit * scale);
    snapshot->SetPosition(
        running_width, 0.5 * (height_ - snapshot->overview_height()));
    running_width += is_selected ?
                     snapshot->overview_width() + kOverviewSelectedPadding  :
                     (kOverviewExposedWindowRatio * width_limit /
                      kOverviewWindowMaxSizeRatio);
    if (is_selected && selected_x >= 0) {
      // If the window will be under 'selected_x' when centered, just
      // center it.  Otherwise, move it as close to centered as possible
      // while still being under 'selected_x'.
      if (0.5 * (width_ - snapshot->overview_width()) < selected_x &&
          0.5 * (width_ + snapshot->overview_width()) >= selected_x) {
        overview_panning_offset_ =
            snapshot->overview_x() +
            0.5 * snapshot->overview_width() -
            0.5 * width_;
      } else if (0.5 * (width_ - snapshot->overview_width()) > selected_x) {
        overview_panning_offset_ = snapshot->overview_x() - selected_x + 1;
      } else {
        overview_panning_offset_ = snapshot->overview_x() - selected_x +
                                   snapshot->overview_width() - 1;
      }
    }
    last_toplevel = snapshot->toplevel();
  }

  if (wm_->background()) {
    // Now we scroll the background to the right location.  The
    // algorithm is the following: scroll the background by
    // kBackgroundScrollRatio times the offset distance to the
    // selected snapshot until there are enough snapshots that
    // selecting the last snapshot would scroll past the end of the
    // overage, and once we reach that point, start scrolling only a
    // percentage of the overage.
    int background_overage = wm_->background()->GetWidth() - wm_->width();
    int offset_limit =
        (running_width -
         (current_snapshot_ ? current_snapshot_->overview_width() : 0) -
         kOverviewSelectedPadding) * kBackgroundScrollRatio;
    if (offset_limit > background_overage) {
      overview_background_offset_ = -background_overage * selected_index /
                                    snapshots_.size();
    } else {
      overview_background_offset_ = -selected_offset * kBackgroundScrollRatio;
    }
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

  ToplevelWindow* toplevel = NULL;
  if (!current_toplevel_) {
    toplevel = forward ?
               toplevels_[0].get() :
               toplevels_[toplevels_.size()-1].get();
  } else {
    if (toplevels_.size() == 1)
      return;
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
    int new_index = (snapshots_.size() + old_index + (forward ? 1 : -1))
                    % snapshots_.size();
    SetCurrentSnapshot(snapshots_[new_index].get());
  }
  if (mode_ == MODE_OVERVIEW)
    LayoutWindows(true);
}

void LayoutManager::SetCurrentSnapshot(SnapshotWindow* snapshot) {
  CHECK(snapshot);

  if (current_snapshot_ != snapshot) {
    if (mode_ != MODE_OVERVIEW) {
      current_snapshot_ = snapshot;
      return;
    }

    // Tell the old current snapshot that it's not current anymore.
    if (current_snapshot_)
      current_snapshot_->SetState(SnapshotWindow::STATE_OVERVIEW_MODE_NORMAL);

    current_snapshot_ = snapshot;

    // Tell the snapshot that it's been selected.
    if (current_snapshot_->state() !=
        SnapshotWindow::STATE_OVERVIEW_MODE_SELECTED) {
      current_snapshot_->SetState(
          SnapshotWindow::STATE_OVERVIEW_MODE_SELECTED);
    }

    // Since we switched snapshots, we may have switched current
    // toplevel windows.
    if (current_snapshot_->toplevel())
      SetCurrentToplevel(current_snapshot_->toplevel());
  }
}

void LayoutManager::SendModeMessage(ToplevelWindow* toplevel) {
  if (!toplevel ||
      toplevel->win()->type() != chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL)
    return;

  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_LAYOUT_MODE);
  switch (mode_) {
    // Set the mode in the message using the appropriate value from wm_ipc.h.
    case MODE_NEW:
    case MODE_ACTIVE_CANCELLED:
    case MODE_ACTIVE:
      msg.set_param(0, 0);
      msg.set_param(1, mode_ == MODE_ACTIVE_CANCELLED ? 1 : 0);
      break;
    case MODE_OVERVIEW:
      msg.set_param(0, 1);
      msg.set_param(1, 0);
      break;
    default:
      CHECK(false) << "Unhandled mode " << mode_;
      break;
  }
  wm_->wm_ipc()->SendMessage(toplevel->win()->xid(), msg);
}

void LayoutManager::SendDeleteRequestToCurrentToplevel() {
  // TODO: If there's a focused transient window, the message should get
  // sent to it instead.
  if (mode_ == MODE_ACTIVE && current_toplevel_)
    current_toplevel_->win()->SendDeleteRequest(
        wm_->GetCurrentTimeFromServer());
}

void LayoutManager::PanOverviewMode(int offset) {
  overview_panning_offset_ += offset;
  if (mode_ == MODE_OVERVIEW)
    LayoutWindows(true);  // animate = true
}

void LayoutManager::UpdateOverviewPanningForMotion() {
  int dx = overview_background_event_coalescer_->x() - overview_drag_last_x_;
  overview_drag_last_x_ = overview_background_event_coalescer_->x();
  overview_panning_offset_ -= dx;
  LayoutWindows(false);  // animate = false
}

void LayoutManager::UpdateCurrentSnapshot() {
  if (snapshots_.empty()) {
    current_snapshot_ = NULL;
    return;
  }

  if (current_toplevel_) {
    int selected_index = current_toplevel_->selected_tab();
    // Go through the snapshots and find the one that corresponds to
    // the selected tab in the current toplevel window.
    for (SnapshotWindows::iterator it = snapshots_.begin();
         it != snapshots_.end(); ++it) {
      if ((*it)->tab_index() == selected_index &&
          (*it)->toplevel() == current_toplevel_) {
        SetCurrentSnapshot((*it).get());
        return;
      }
    }
  } else {
    // If we don't have an active toplevel window, then just take the
    // first snapshot.  If no snapshots, then we have to set the
    // current snapshot to NULL.
    SetCurrentSnapshot(snapshots_[0].get());
  }
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

  if (current_toplevel_ == toplevel)
    current_toplevel_ = NULL;

  // Find any transient windows associated with this toplevel window
  // and remove them.
  XWindowToToplevelMap::iterator transient_iter =
      transient_to_toplevel_.begin();
  while (transient_iter != transient_to_toplevel_.end()) {
    if (transient_iter->second == toplevel)
      transient_to_toplevel_.erase(transient_iter);
    ++transient_iter;
  }

  toplevels_.erase(toplevels_.begin() + index);

  // Find a new active toplevel window if needed.  If there's no
  // active window now, then this one was probably active previously.
  // Choose a new active window if possible; relinquish the focus
  // otherwise.
  if (!current_toplevel_) {
    if (!toplevels_.empty()) {
      const int new_index =
          (index + toplevels_.size() - 1) % toplevels_.size();
      SetCurrentToplevel(toplevels_[new_index].get());
    } else {
      if (mode_ == MODE_ACTIVE) {
        if (win->focused()) {
          wm_->SetActiveWindowProperty(None);
          wm_->TakeFocus(wm_->GetCurrentTimeFromServer());
        }
      }
    }
  }
  UpdateCurrentSnapshot();
}

bool LayoutManager::SortSnapshots() {
  SnapshotWindows old_snapshots = snapshots_;
  std::sort(snapshots_.begin(), snapshots_.end(),
            SnapshotWindow::CompareTabIndex);
  return old_snapshots != snapshots_;
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

void LayoutManager::EnableKeyBindingsForModeInternal(Mode mode) {
  switch (mode) {
    case MODE_NEW:
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

void LayoutManager::DisableKeyBindingsForModeInternal(Mode mode) {
  switch (mode) {
    case MODE_NEW:
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

}  // namespace window_manager
