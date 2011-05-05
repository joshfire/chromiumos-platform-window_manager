// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/layout/toplevel_window.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <string>
#include <tr1/memory>

#include <gflags/gflags.h>

#include "base/logging.h"
#include "base/string_util.h"
#include "base/time.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"
#include "window_manager/callback.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/focus_manager.h"
#include "window_manager/geometry.h"
#include "window_manager/layout/snapshot_window.h"
#include "window_manager/motion_event_coalescer.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/transient_window_collection.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x11/x_connection.h"

// Define this if you want extra logging in this file.
#if !defined(EXTRA_LOGGING)
#undef EXTRA_LOGGING
#endif

using base::TimeDelta;
using std::list;
using std::make_pair;
using std::map;
using std::max;
using std::min;
using std::pair;
using std::string;
using std::tr1::shared_ptr;
using std::vector;
using window_manager::util::XidStr;

namespace window_manager {

// When animating a window zooming out while switching windows, what size
// should it scale to?
static const double kWindowFadeSizeFraction = 0.7;

// Distance over which we move the window for the no-op window-switching
// animation.
static const int kNudgeAnimPixels = 25;

// Amount of time used for the no-op window-switching animation.
static const int kNudgeAnimMs = 180;

LayoutManager::ToplevelWindow::ToplevelWindow(Window* win,
                                              LayoutManager* layout_manager)
    : win_(win),
      layout_manager_(layout_manager),
      state_(STATE_NEW),
      last_state_(STATE_NEW),
      transients_(
          new TransientWindowCollection(
              win,   // owner_win
              NULL,  // win_to_stack_above
              TransientWindowCollection::CENTER_OVER_OWNER,
              TransientWindowCollection::KEEP_ONSCREEN_IF_OWNER_IS_ONSCREEN,
              layout_manager)),
      selected_tab_(-1),
      tab_count_(0),
      last_tab_selected_time_(0),
      event_consumer_registrar_(
          new EventConsumerRegistrar(wm(), layout_manager_)),
      is_fullscreen_(false) {
#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Creating ToplevelWindow for window " << XidStr(win_->xid());
#endif

  event_consumer_registrar_->RegisterForWindowEvents(win_->xid());

  int width = layout_manager_->width();
  int height = layout_manager_->height();
  win->Resize(Size(width, height), GRAVITY_NORTHWEST);

  // Let the window know that it's maximized.
  map<XAtom, bool> wm_state;
  wm_state[wm()->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_HORZ)] = true;
  wm_state[wm()->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_VERT)] = true;
  win->ChangeWmState(wm_state);

  // Initialize local properties from the window properties.
  PropertiesChanged();

  // Start with client offscreen, and composited window invisible.
  win->MoveClientOffscreen();
  win->SetCompositedOpacity(0, 0);
  win->ShowComposited();

  // Make sure that we hear about button presses on this window.
  wm()->focus_manager()->UseClickToFocusForWindow(
      win, FocusManager::PASS_CLICKS_THROUGH);
}

LayoutManager::ToplevelWindow::~ToplevelWindow() {
#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Deleting toplevel window " << win_->xid_str();
#endif
  transients_->CloseAllWindows();
  win_->HideComposited();
  transients_.reset(NULL);
  win_ = NULL;
  layout_manager_ = NULL;
}

// static
const char* LayoutManager::ToplevelWindow::GetStateName(State state) {
  switch(state) {
    case STATE_NEW:
      return "New";
    case STATE_OVERVIEW_MODE:
      return "Overview Mode";
    case STATE_ACTIVE_MODE_ONSCREEN:
      return "Active Mode Onscreen";
    case STATE_ACTIVE_MODE_OFFSCREEN:
      return "Active Mode Offscreen";
    case STATE_ACTIVE_MODE_IN_FROM_RIGHT:
      return "Active Mode In From Right";
    case STATE_ACTIVE_MODE_IN_FROM_LEFT:
      return "Active Mode In From Left";
    case STATE_ACTIVE_MODE_OUT_TO_LEFT:
      return "Active Mode Out To Left";
    case STATE_ACTIVE_MODE_OUT_TO_RIGHT:
      return "Active Mode Out To Right";
    case STATE_ACTIVE_MODE_IN_FADE:
      return "Active Mode In Fade";
    case STATE_ACTIVE_MODE_OUT_FADE:
      return "Active Mode Out Fade";
    default:
      return "UNKNOWN STATE";
  }
}

void LayoutManager::ToplevelWindow::SetState(State state) {
#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Switching toplevel " << win_->xid_str()
            << " state from " << GetStateName(state_) << " to "
            << GetStateName(state);
#endif
  state_ = state;
}

void LayoutManager::ToplevelWindow::UpdateLayout(bool animate) {
#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Updating layout for toplevel " << win_->xid_str()
             << " in state " << GetStateName(state_);
#endif
  if (state_ == STATE_OVERVIEW_MODE) {
    if (last_state_ != STATE_OVERVIEW_MODE)
      ConfigureForOverviewMode(animate);
  } else {
    ConfigureForActiveMode(animate);
  }
  last_state_ = state_;
}

bool LayoutManager::ToplevelWindow::PropertiesChanged() {
  if (win_->type() != chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL)
    return false;

  if (win_->type_params().size() < 1) {
    LOG(ERROR) << "Chrome isn't sending enough type parameters for "
               << "TOPLEVEL windows";
    return false;
  }

  // Try and be a little backward compatible here.  If Chrome isn't
  // sending the timestamp, then just assume they're all current
  // (the old behavior).  This will ease the transition while the
  // ChromeOS version of Chrome lags the TOT version.
  // TODO(gspencer): Remove this once Chrome rolls to the new
  // version.
  XTime event_time = last_tab_selected_time_;
  if (win_->type_params().size() > 2) {
    event_time = static_cast<XTime>(win_->type_params()[2]);
  }

  // Only do something if the timestamp on the change was newer or
  // equal than our last request, to avoid a race with Chrome
  // where we get reset back to something that occurred earlier in
  // time.
  if (event_time < last_tab_selected_time_)
    return false;

  last_tab_selected_time_ = event_time;
  // Notice if the number of tabs or the selected tab changed.
  int old_tab_count = tab_count_;
  int old_selected_tab = selected_tab_;
  selected_tab_ = win_->type_params()[1];
  tab_count_ = win_->type_params()[0];

  bool changed = tab_count_ != old_tab_count ||
                 selected_tab_ != old_selected_tab;
#if defined(EXTRA_LOGGING)
  if (changed) {
    DLOG(INFO) << "Properties of toplevel " << win_->xid_str()
               << " changed count from " << old_tab_count << " to "
               << tab_count_
               << " and selected from " << old_selected_tab << " to "
               << selected_tab_
               << " at time " << last_tab_selected_time_;
  }
#endif

  return changed;
}

void LayoutManager::ToplevelWindow::SendTabSelectedMessage(int tab_index,
                                                           XTime timestamp) {
  last_tab_selected_time_ = timestamp;
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_TAB_SELECT);
  msg.set_param(0, tab_index);
  msg.set_param(1, timestamp);
  wm()->wm_ipc()->SendMessage(win_->xid(), msg);
}

void LayoutManager::ToplevelWindow::SetFullscreenState(bool fullscreen) {
  if (fullscreen == is_fullscreen_)
    return;

  is_fullscreen_ = fullscreen;
  if (win_->wm_state_fullscreen() != is_fullscreen_) {
    map<XAtom, bool> wm_state;
    wm_state[wm()->GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN)] = is_fullscreen_;
    win_->ChangeWmState(wm_state);
  }

  if (is_fullscreen_) {
    wm()->stacking_manager()->StackWindowAtTopOfLayer(
        win_,
        StackingManager::LAYER_FULLSCREEN_WINDOW,
        StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
    win_->Resize(wm()->root_size(), GRAVITY_NORTHWEST);
    win_->MoveClient(0, 0);
    win_->MoveCompositedToClient();
    // If a window has its fullscreen hint set when it's first mapped,
    // LayoutManager will avoid calling ConfigureForActiveMode(), so we
    // need to manually make sure that the window is visible here.
    win_->SetCompositedOpacity(1, 0);
  } else {
    wm()->stacking_manager()->StackWindowAtTopOfLayer(
        win_,
        StackingManager::LAYER_TOPLEVEL_WINDOW,
        StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
    win_->Resize(Size(layout_manager_->width(), layout_manager_->height()),
                 GRAVITY_NORTHWEST);
    win_->MoveClient(layout_manager_->x(), layout_manager_->y());
    win_->MoveCompositedToClient();
  }

  const bool stack_transient_directly_above_win =
      is_fullscreen_ || state_ == STATE_OVERVIEW_MODE;
  transients_->ApplyStackingForAllWindows(stack_transient_directly_above_win);
}

void LayoutManager::ToplevelWindow::DoNudgeAnimation(bool move_to_left) {
  if (state_ != STATE_ACTIVE_MODE_ONSCREEN)
    return;

  AnimationPair* animations = win_->CreateMoveCompositedAnimation();
  animations->AppendKeyframe(
      win_->composited_x() + (move_to_left ? -1 : 1) * kNudgeAnimPixels,
      win_->composited_y(),
      TimeDelta::FromMilliseconds(kNudgeAnimMs / 2));
  animations->AppendKeyframe(
      win_->composited_x(), win_->composited_y(),
      TimeDelta::FromMilliseconds(kNudgeAnimMs / 2));
  win_->SetMoveCompositedAnimation(animations);
}

void LayoutManager::ToplevelWindow::ConfigureForActiveMode(bool animate) {
  const int layout_x = layout_manager_->x();
  const int layout_y = layout_manager_->y();
  const int layout_width = layout_manager_->width();
  const int layout_height = layout_manager_->height();
  const int this_index = layout_manager_->GetIndexForToplevelWindow(*this);
  const int current_index = layout_manager_->GetIndexForToplevelWindow(
      *(layout_manager_->current_toplevel()));
  const bool to_left_of_active = this_index < current_index;
  const int anim_ms = animate ? LayoutManager::kWindowAnimMs : 0;
  const int opacity_anim_ms = animate ? LayoutManager::kWindowOpacityAnimMs : 0;
  const int animation_time =
      (last_state_ == STATE_ACTIVE_MODE_ONSCREEN) ? anim_ms : 0;

  // Center window vertically and horizontally.
  const int win_y =
      layout_y + std::max(0, (layout_height - win_->client_height())) / 2;
  const int win_x =
      layout_x + std::max(0, (layout_width - win_->client_width())) / 2;

  // This switch is to setup the starting conditions for each kind of
  // transition.
  switch (state_) {
    case STATE_ACTIVE_MODE_OFFSCREEN:
    case STATE_ACTIVE_MODE_ONSCREEN:
    case STATE_ACTIVE_MODE_OUT_FADE:
    case STATE_ACTIVE_MODE_OUT_TO_LEFT:
    case STATE_ACTIVE_MODE_OUT_TO_RIGHT:
      // Nothing to do for initial setup on these -- they start
      // animating from wherever they are.
      break;
    case STATE_NEW: {
      // Start new windows at the bottom of the layout area.
      win_->MoveComposited(win_x, layout_y + layout_height, 0);
      win_->ScaleComposited(1.0, 1.0, 0);
      win_->SetCompositedOpacity(1.0, 0);
      break;
    }
    case STATE_ACTIVE_MODE_IN_FROM_RIGHT: {
      // These start off to the right.
      win_->MoveComposited(layout_x + layout_width, win_y, 0);
      win_->SetCompositedOpacity(1.0, 0);
      win_->ScaleComposited(1.0, 1.0, 0);
      break;
    }
    case STATE_ACTIVE_MODE_IN_FROM_LEFT: {
      // These start off to the left.
      win_->MoveComposited(layout_x - win_->client_width(), win_y, 0);
      win_->SetCompositedOpacity(1.0, 0);
      win_->ScaleComposited(1.0, 1.0, 0);
      break;
    }
    case STATE_ACTIVE_MODE_IN_FADE: {
      win_->SetCompositedOpacity(0.0, 0);
      SnapshotWindow* selected_snapshot =
          layout_manager_->GetSelectedSnapshotFromToplevel(*this);
      if (selected_snapshot) {
        // Since we have a current snapshot, we start off with the
        // location and dimensions of that snapshot.
        const int snapshot_x = selected_snapshot->overview_x() +
                               layout_manager_->overview_panning_offset();
        const int snapshot_y = selected_snapshot->overview_y();
        const int snapshot_width = selected_snapshot->overview_width();
        const int snapshot_height = selected_snapshot->overview_height();
        win_->MoveComposited(snapshot_x, snapshot_y, 0);
        win_->ScaleComposited(static_cast<float>(snapshot_width) /
                              win_->client_width(),
                              static_cast<float>(snapshot_height) /
                              win_->client_height(), 0);
      } else {
        // These start off invisible, and fade in, scaling up from
        // something smaller than full size.
        win_->ScaleComposited(kWindowFadeSizeFraction,
                              kWindowFadeSizeFraction, 0);
        win_->MoveComposited(
            layout_x + (layout_width -
                        (kWindowFadeSizeFraction * win_->client_width())) / 2,
            layout_y + (layout_height -
                        (kWindowFadeSizeFraction * win_->client_height())) / 2,
            0);
      }
      break;
    }
    case STATE_OVERVIEW_MODE:
      NOTREACHED() << "Tried to layout overview mode in ConfigureForActiveMode";
      break;
  }

  transients_->ApplyStackingForAllWindows(is_fullscreen_);

  // Now set in motion the animations by targeting their destination.
  switch (state_) {
    case STATE_ACTIVE_MODE_OUT_TO_LEFT: {
      win_->MoveComposited(layout_x - layout_width, win_y, animation_time);
      SetState(STATE_ACTIVE_MODE_OFFSCREEN);
      break;
    }
    case STATE_ACTIVE_MODE_OUT_TO_RIGHT: {
      win_->MoveComposited(layout_x + layout_width, win_y, animation_time);
      SetState(STATE_ACTIVE_MODE_OFFSCREEN);
      break;
    }
    case STATE_ACTIVE_MODE_OUT_FADE: {
      win_->SetCompositedOpacity(0.f, opacity_anim_ms);
      win_->MoveComposited(
          layout_x +
          (layout_width - (kWindowFadeSizeFraction * win_->client_width())) /
          (2 * kWindowFadeSizeFraction),
          layout_y +
          (layout_height - (kWindowFadeSizeFraction * win_->client_height())) /
          (2 * kWindowFadeSizeFraction), anim_ms);
      win_->ScaleComposited(
          kWindowFadeSizeFraction, kWindowFadeSizeFraction, anim_ms);
      SetState(STATE_ACTIVE_MODE_OFFSCREEN);
      break;
    }
    case STATE_ACTIVE_MODE_OFFSCREEN: {
      win_->SetCompositedOpacity(1.f, 0);
      win_->ScaleComposited(1.f, 1.f, animation_time);
      win_->MoveComposited(layout_x +
                           (to_left_of_active ? -layout_width : layout_width),
                           win_y, animation_time);
      break;
    }
    case STATE_ACTIVE_MODE_IN_FADE:
    case STATE_ACTIVE_MODE_IN_FROM_LEFT:
    case STATE_ACTIVE_MODE_IN_FROM_RIGHT:
    case STATE_ACTIVE_MODE_ONSCREEN:
    case STATE_NEW:  {
      win_->MoveComposited(win_x, win_y, anim_ms);
      win_->SetCompositedOpacity(1.f, opacity_anim_ms);
      win_->ScaleComposited(1.f, 1.f, anim_ms);
      SetState(STATE_ACTIVE_MODE_ONSCREEN);
      break;
    }
    case STATE_OVERVIEW_MODE:
      NOTREACHED() << "Tried to layout overview mode in ConfigureForActiveMode";
      break;
  }

  if (state_ == STATE_ACTIVE_MODE_ONSCREEN) {
    win_->MoveClient(win_x, win_y);
    transients_->ConfigureAllWindowsRelativeToOwner(anim_ms);
  } else {
    win_->MoveClientOffscreen();
    transients_->ConfigureAllWindowsRelativeToOwner(
        last_state_ == STATE_ACTIVE_MODE_ONSCREEN ? anim_ms : 0);
  }

  // If we previously hid our transient windows because we were in overview
  // mode, show them again.
  if (!transients_->shown())
    transients_->Show();
}

void LayoutManager::ToplevelWindow::ConfigureForOverviewMode(bool animate) {
  const int anim_ms = animate ? LayoutManager::kWindowAnimMs : 0;
  const int opacity_anim_ms = animate ? LayoutManager::kWindowOpacityAnimMs : 0;
  // If this is the current toplevel window, we fade it out while
  // scaling it down.
  if (layout_manager_->current_toplevel() == this) {
    SnapshotWindow* selected_snapshot =
        layout_manager_->GetSelectedSnapshotFromToplevel(*this);
    if (selected_snapshot) {
      const int snapshot_x =
          selected_snapshot->overview_x() +
          layout_manager_->overview_panning_offset();
      const int snapshot_y = selected_snapshot->overview_y();
      const int snapshot_width = selected_snapshot->overview_width();
      const int snapshot_height = selected_snapshot->overview_height();

      win_->MoveComposited(snapshot_x, snapshot_y, anim_ms);
      win_->ScaleComposited(static_cast<float>(snapshot_width) /
                            win_->client_width(),
                            static_cast<float>(snapshot_height) /
                            win_->client_height(), anim_ms);
    } else {
      float center_scale = 0.5 * kWindowFadeSizeFraction;
      win_->ScaleComposited(kWindowFadeSizeFraction, kWindowFadeSizeFraction,
                            anim_ms);
      win_->MoveComposited(center_scale * win_->client_width(),
                           center_scale * win_->client_height(),
                           anim_ms);
    }
    win_->SetCompositedOpacity(0.0, opacity_anim_ms);
  } else {
    win_->SetCompositedOpacity(0.0, 0);
  }
  if (transients_->shown())
    transients_->Hide();
  win_->MoveClientOffscreen();
}

void LayoutManager::ToplevelWindow::TakeFocus(XTime timestamp) {
  if (!transients_->TakeFocus(timestamp))
    wm()->FocusWindow(win_, timestamp);
}

void LayoutManager::ToplevelWindow::SetPreferredTransientWindowToFocus(
    Window* transient_win) {
  transients_->SetPreferredWindowToFocus(transient_win);
}

bool LayoutManager::ToplevelWindow::IsWindowOrTransientFocused() const {
  return win_->IsFocused() || transients_->HasFocusedWindow();
}

void LayoutManager::ToplevelWindow::HandleTransientWindowMap(
    Window* transient_win, bool in_overview_mode) {
  const bool stack_directly_above_toplevel = in_overview_mode || is_fullscreen_;
  transients_->AddWindow(transient_win, stack_directly_above_toplevel);
}

void LayoutManager::ToplevelWindow::HandleTransientWindowUnmap(
    Window* transient_win) {
  transients_->RemoveWindow(transient_win);
}

void LayoutManager::ToplevelWindow::HandleTransientWindowConfigureRequest(
    Window* transient_win, const Rect& requested_bounds) {
  transients_->HandleConfigureRequest(transient_win, requested_bounds);
}

void LayoutManager::ToplevelWindow::HandleButtonPress(
    Window* button_win, XTime timestamp) {
  // Don't reassign the focus if it's already held by a modal window.
  if (wm()->IsModalWindowFocused())
    return;

  transients_->SetPreferredWindowToFocus(
      transients_->ContainsWindow(*button_win) ? button_win : NULL);
  TakeFocus(timestamp);
}

}  // namespace window_manager
