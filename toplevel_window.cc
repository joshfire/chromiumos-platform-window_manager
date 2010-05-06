// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/toplevel_window.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <string>
#include <tr1/memory>

#include <gflags/gflags.h>

#include "base/logging.h"
#include "base/string_util.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"
#include "window_manager/callback.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/motion_event_coalescer.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/system_metrics.pb.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x_connection.h"

// Define this if you want extra logging in this file.
#if !defined(EXTRA_LOGGING)
#undef EXTRA_LOGGING
#endif

// Defined in layout_manager.cc
DECLARE_bool(lm_honor_window_size_hints);

using std::list;
using std::make_pair;
using std::map;
using std::max;
using std::min;
using std::pair;
using std::string;
using std::tr1::shared_ptr;
using std::vector;

namespace window_manager {

// When animating a window zooming out while switching windows, what size
// should it scale to?
static const double kWindowFadeSizeFraction = 0.5;

LayoutManager::ToplevelWindow::ToplevelWindow(Window* win,
                                              LayoutManager* layout_manager)
    : win_(win),
      layout_manager_(layout_manager),
      state_(STATE_NEW),
      last_state_(STATE_NEW),
      stacked_transients_(new Stacker<TransientWindow*>),
      transient_to_focus_(NULL),
      selected_tab_(-1),
      tab_count_(0),
      event_consumer_registrar_(
          new EventConsumerRegistrar(wm(), layout_manager_)) {
#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Creating ToplevelWindow for window " << XidStr(win_->xid());
#endif

  event_consumer_registrar_->RegisterForWindowEvents(win_->xid());

  int width = layout_manager_->width();
  int height = layout_manager_->height();
  if (FLAGS_lm_honor_window_size_hints)
    win->GetMaxSize(width, height, &width, &height);
  win->ResizeClient(width, height, GRAVITY_NORTHWEST);

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
  win->AddButtonGrab();
}

LayoutManager::ToplevelWindow::~ToplevelWindow() {
#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Deleting toplevel window " << win_->xid_str();
#endif
  while (!transients_.empty())
    RemoveTransientWindow(transients_.begin()->second->win);
  win_->RemoveButtonGrab();
  win_ = NULL;
  layout_manager_ = NULL;
  transient_to_focus_ = NULL;
}

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
  DLOG(INFO) << "Updating Layout for toplevel "
            << win_->xid_str() << " in state "
            << GetStateName(state_);
#endif
  if (state_ == STATE_OVERVIEW_MODE) {
    ConfigureForOverviewMode(animate);
  } else {
    ConfigureForActiveMode(animate);
  }
  last_state_ = state_;
}

bool LayoutManager::ToplevelWindow::PropertiesChanged() {
  if (win_->type() == chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL) {
    if (win_->type_params().size() > 1) {
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
                  << selected_tab_;
      }
#endif
      return changed;
    }
  }
  return false;
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
      // These start off invisible, and fade in, scaling up from
      // something smaller than full size.
      float center_scale = 0.5 * kWindowFadeSizeFraction;
      win_->SetCompositedOpacity(0, 0);
      win_->ScaleComposited(0.5, 0.5, 0);
      win_->MoveComposited(layout_x + center_scale * win_->client_width(),
                           layout_y + center_scale * win_->client_height(), 0);
      break;
    }
    case STATE_OVERVIEW_MODE:
      NOTREACHED() << "Tried to layout overview mode in ConfigureForActiveMode";
      break;
  }

  ApplyStackingForAllTransientWindows(false);  // stack in upper layer

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
      float center_scale = 0.5 * kWindowFadeSizeFraction;
      win_->SetCompositedOpacity(0.f, anim_ms);
      win_->MoveComposited(layout_x + center_scale * win_->client_width(),
                           layout_y + center_scale * win_->client_height(),
                           anim_ms);
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
      win_->MoveClientToComposited();
      win_->SetCompositedOpacity(1.f, anim_ms / 4);
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
    ConfigureAllTransientWindows(anim_ms);
    win_->SetShadowOpacity(1.0, anim_ms);
  } else {
    win_->MoveClientOffscreen();
    win_->SetShadowOpacity(0, anim_ms);
    ConfigureAllTransientWindows(
         last_state_ == STATE_ACTIVE_MODE_ONSCREEN ? anim_ms : 0);
  }
}

void LayoutManager::ToplevelWindow::ConfigureForOverviewMode(bool animate) {
  const int anim_ms = animate ? LayoutManager::kWindowAnimMs : 0;
  // If this is the current toplevel window, we fade it out while
  // scaling it down.
  if (layout_manager_->current_toplevel() == this) {
    float center_scale = 0.5 * kWindowFadeSizeFraction;
    win_->ScaleComposited(kWindowFadeSizeFraction, kWindowFadeSizeFraction,
                          anim_ms);
    win_->MoveComposited(center_scale * win_->client_width(),
                         center_scale * win_->client_height(),
                         anim_ms);
    win_->SetCompositedOpacity(0.0, anim_ms / 4);
  } else {
    win_->SetCompositedOpacity(0.0, 0);
  }
  ApplyStackingForAllTransientWindows(true);  // stack above toplevel
  win_->MoveClientOffscreen();
}

void LayoutManager::ToplevelWindow::TakeFocus(XTime timestamp) {
  // We'll get an inactive/active flicker in the toplevel window if we let
  // the window manager set the active window property to None when the
  // transient is unmapped and we then set it back to another transient or
  // to the toplevel window after we see the FocusIn notification, so we
  // proactively set it to the window that we're focusing here.
  if (transient_to_focus_) {
    RestackTransientWindowOnTop(transient_to_focus_);
    transient_to_focus_->win->TakeFocus(timestamp);
    wm()->SetActiveWindowProperty(transient_to_focus_->win->xid());
  } else {
    win_->TakeFocus(timestamp);
    wm()->SetActiveWindowProperty(win_->xid());
  }
}

void LayoutManager::ToplevelWindow::SetPreferredTransientWindowToFocus(
    Window* transient_win) {
  if (!transient_win) {
    if (transient_to_focus_ && !transient_to_focus_->win->wm_state_modal())
      transient_to_focus_ = NULL;
    return;
  }

  TransientWindow* transient = GetTransientWindow(*transient_win);
  if (!transient) {
    LOG(ERROR) << "Got request to prefer focusing " << transient_win->xid_str()
               << ", which isn't transient for " << win_->xid_str();
    return;
  }

  if (transient == transient_to_focus_)
    return;

  if (!transient_to_focus_ ||
      !transient_to_focus_->win->wm_state_modal() ||
      transient_win->wm_state_modal())
    transient_to_focus_ = transient;
}

bool LayoutManager::ToplevelWindow::IsWindowOrTransientFocused() const {
  if (win_->focused())
    return true;

  for (map<XWindow, shared_ptr<TransientWindow> >::const_iterator it =
           transients_.begin();
       it != transients_.end(); ++it) {
    if (it->second->win->focused())
      return true;
  }
  return false;
}

void LayoutManager::ToplevelWindow::AddTransientWindow(
    Window* transient_win, bool stack_directly_above_toplevel) {
  CHECK(transient_win);
  if (transients_.find(transient_win->xid()) != transients_.end()) {
    LOG(ERROR) << "Got request to add already-present transient window "
               << transient_win->xid_str() << " to " << win_->xid_str();
    return;
  }

  wm()->RegisterEventConsumerForWindowEvents(transient_win->xid(),
                                             layout_manager_);
  shared_ptr<TransientWindow> transient(new TransientWindow(transient_win));
  transients_[transient_win->xid()] = transient;

  // All transient windows other than info bubbles get centered over their
  // owner.
  if (transient_win->type() == chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE) {
    transient->SaveOffsetsRelativeToOwnerWindow(win_);
    transient->centered = false;
  } else {
    transient->UpdateOffsetsToCenterOverOwnerWindow(win_);
    transient->centered = true;
  }

  // If the new transient is non-modal, stack it above the top non-modal
  // transient that we have.  If it's modal, just put it on top of all
  // other transients.
  TransientWindow* transient_to_stack_above = NULL;
  for (list<TransientWindow*>::const_iterator it =
           stacked_transients_->items().begin();
       it != stacked_transients_->items().end(); ++it) {
    if (transient_win->wm_state_modal() || !(*it)->win->wm_state_modal()) {
      transient_to_stack_above = (*it);
      break;
    }
  }
  if (transient_to_stack_above)
    stacked_transients_->AddAbove(transient.get(), transient_to_stack_above);
  else
    stacked_transients_->AddOnBottom(transient.get());

  SetPreferredTransientWindowToFocus(transient_win);

  ConfigureTransientWindow(transient.get(), 0);
  ApplyStackingForTransientWindow(
      transient.get(),
      transient_to_stack_above ?
      transient_to_stack_above->win :
      (stack_directly_above_toplevel ? win_ : NULL));

  transient_win->ShowComposited();
  transient_win->AddButtonGrab();
}

void LayoutManager::ToplevelWindow::RemoveTransientWindow(
    Window* transient_win) {
  CHECK(transient_win);
  TransientWindow* transient = GetTransientWindow(*transient_win);
  if (!transient) {
    LOG(ERROR) << "Got request to remove not-present transient window "
               << transient_win->xid_str() << " from " << win_->xid_str();
    return;
  }

  wm()->UnregisterEventConsumerForWindowEvents(transient_win->xid(),
                                               layout_manager_);
  stacked_transients_->Remove(transient);
  CHECK(transients_.erase(transient_win->xid()) == 1);
  transient_win->RemoveButtonGrab();

  if (transient_to_focus_ == transient) {
    transient_to_focus_ = NULL;
    TransientWindow* new_transient = FindTransientWindowToFocus();
    SetPreferredTransientWindowToFocus(
        new_transient ? new_transient->win : NULL);
  }
}

void LayoutManager::ToplevelWindow::HandleTransientWindowConfigureRequest(
    Window* transient_win,
    int req_x, int req_y, int req_width, int req_height) {
  CHECK(transient_win);
  TransientWindow* transient = GetTransientWindow(*transient_win);
  CHECK(transient);

  // Move and resize the transient window as requested.
  bool moved = false;
  if (req_x != transient_win->client_x() ||
      req_y != transient_win->client_y()) {
    transient_win->MoveClient(req_x, req_y);
    transient->SaveOffsetsRelativeToOwnerWindow(win_);
    transient->centered = false;
    moved = true;
  }

  if (req_width != transient_win->client_width() ||
      req_height != transient_win->client_height()) {
    transient_win->ResizeClient(req_width, req_height, GRAVITY_NORTHWEST);
    if (transient->centered) {
      transient->UpdateOffsetsToCenterOverOwnerWindow(win_);
      moved = true;
    }
  }

  if (moved)
    ConfigureTransientWindow(transient, 0);
}

void LayoutManager::ToplevelWindow::HandleFocusChange(
    Window* focus_win, bool focus_in) {
  DCHECK(focus_win == win_ || GetTransientWindow(*focus_win));

  if (focus_in) {
#if defined(EXTRA_LOGGING)
    DLOG(INFO) << "Got focus-in for " << focus_win->xid_str()
               << "; removing passive button grab";
#endif
    focus_win->RemoveButtonGrab();
  } else {
    // Listen for button presses on this window so we'll know when it
    // should be focused again.
#if defined(EXTRA_LOGGING)
    DLOG(INFO) << "Got focus-out for " << focus_win->xid_str()
               << "; re-adding passive button grab";
#endif
    focus_win->AddButtonGrab();
  }
}

void LayoutManager::ToplevelWindow::HandleButtonPress(
    Window* button_win, XTime timestamp) {
  SetPreferredTransientWindowToFocus(
      GetTransientWindow(*button_win) ? button_win : NULL);
  TakeFocus(timestamp);
  wm()->xconn()->RemovePointerGrab(true, timestamp);  // replay events
}

LayoutManager::ToplevelWindow::TransientWindow*
LayoutManager::ToplevelWindow::GetTransientWindow(const Window& win) {
  map<XWindow, shared_ptr<TransientWindow> >::iterator it =
      transients_.find(win.xid());
  if (it == transients_.end())
    return NULL;
  return it->second.get();
}

void LayoutManager::ToplevelWindow::ConfigureTransientWindow(
    TransientWindow* transient, int anim_ms) {
  // TODO: Check if 'win_' is offscreen, and make sure that the transient
  // window is offscreen as well if so.
  transient->win->MoveClient(
      win_->client_x() + transient->x_offset,
      win_->client_y() + transient->y_offset);

  transient->win->MoveComposited(
      win_->composited_x() + win_->composited_scale_x() * transient->x_offset,
      win_->composited_y() + win_->composited_scale_y() * transient->y_offset,
      anim_ms);
  transient->win->ScaleComposited(
      win_->composited_scale_x(), win_->composited_scale_y(), anim_ms);
  transient->win->SetCompositedOpacity(win_->composited_opacity(), anim_ms);
}

void LayoutManager::ToplevelWindow::ConfigureAllTransientWindows(
    int anim_ms) {
  for (map<XWindow, shared_ptr<TransientWindow> >::iterator it =
           transients_.begin();
       it != transients_.end(); ++it) {
    ConfigureTransientWindow(it->second.get(), anim_ms);
  }
}

void LayoutManager::ToplevelWindow::ApplyStackingForTransientWindow(
    TransientWindow* transient, Window* other_win) {
  DCHECK(transient);
  if (other_win) {
    transient->win->StackClientAbove(other_win->xid());
    transient->win->StackCompositedAbove(other_win->actor(), NULL, false);
  } else {
    wm()->stacking_manager()->StackWindowAtTopOfLayer(
        transient->win, StackingManager::LAYER_ACTIVE_TRANSIENT_WINDOW);
  }
}

void LayoutManager::ToplevelWindow::ApplyStackingForAllTransientWindows(
    bool stack_directly_above_toplevel) {
  Window* prev_win = stack_directly_above_toplevel ? win_ : NULL;
  for (list<TransientWindow*>::const_reverse_iterator it =
           stacked_transients_->items().rbegin();
       it != stacked_transients_->items().rend();
       ++it) {
    TransientWindow* transient = *it;
    ApplyStackingForTransientWindow(transient, prev_win);
    prev_win = transient->win;
  }
}

LayoutManager::ToplevelWindow::TransientWindow*
LayoutManager::ToplevelWindow::FindTransientWindowToFocus() const {
  if (stacked_transients_->items().empty())
    return NULL;

  for (list<TransientWindow*>::const_iterator it =
           stacked_transients_->items().begin();
       it != stacked_transients_->items().end();
       ++it) {
    if ((*it)->win->wm_state_modal())
      return *it;
  }
  return stacked_transients_->items().front();
}

void LayoutManager::ToplevelWindow::RestackTransientWindowOnTop(
    TransientWindow* transient) {
  if (transient == stacked_transients_->items().front())
    return;

  DCHECK(stacked_transients_->Contains(transient));
  DCHECK_GT(stacked_transients_->items().size(), 1U);
  TransientWindow* transient_to_stack_above =
      stacked_transients_->items().front();
  stacked_transients_->Remove(transient);
  stacked_transients_->AddOnTop(transient);
  ApplyStackingForTransientWindow(transient, transient_to_stack_above->win);
}

}  // namespace window_manager
