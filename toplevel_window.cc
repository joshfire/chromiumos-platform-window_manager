// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/toplevel_window.h"

#include <algorithm>
#include <cmath>
#include <tr1/memory>
extern "C" {
#include <X11/Xatom.h>
}

#include <gflags/gflags.h>

#include "base/string_util.h"
#include "base/logging.h"
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

// Defined in layout_manager.cc
DECLARE_bool(lm_honor_window_size_hints);
DECLARE_string(lm_overview_gradient_image);

namespace window_manager {

using std::map;
using std::vector;
using std::pair;
using std::make_pair;
using std::list;
using std::tr1::shared_ptr;

// Animation speed for windows in new overview mode.
static const int kOverviewAnimMs = 100;

// When animating a window zooming out while switching windows, what size
// should it scale to?
static const double kWindowFadeSizeFraction = 0.5;

ClutterInterface::Actor*
LayoutManager::ToplevelWindow::static_gradient_texture_ = NULL;

LayoutManager::ToplevelWindow::ToplevelWindow(Window* win,
                                              LayoutManager* layout_manager)
    : win_(win),
      layout_manager_(layout_manager),
      input_xid_(
          wm()->CreateInputWindow(
              -1, -1, 1, 1,
              ButtonPressMask | EnterWindowMask | LeaveWindowMask)),
      state_(STATE_NEW),
      overview_x_(0),
      overview_y_(0),
      overview_width_(0),
      overview_height_(0),
      overview_scale_(1.0),
      stacked_transients_(new Stacker<TransientWindow*>),
      transient_to_focus_(NULL),
      event_consumer_registrar_(
          new EventConsumerRegistrar(wm(), layout_manager_)) {
  event_consumer_registrar_->RegisterForWindowEvents(win_->xid());
  event_consumer_registrar_->RegisterForWindowEvents(input_xid_);

  if (!static_gradient_texture_) {
    static_gradient_texture_ = wm()->clutter()->CreateImage(
        FLAGS_lm_overview_gradient_image);
    static_gradient_texture_->SetVisibility(false);
    static_gradient_texture_->SetName("static gradient screen");
    wm()->stage()->AddActor(static_gradient_texture_);
  }

  int width = layout_manager_->width();
  int height = layout_manager_->height();
  if (FLAGS_lm_honor_window_size_hints)
    win->GetMaxSize(width, height, &width, &height);
  win->ResizeClient(width, height, Window::GRAVITY_NORTHWEST);

  wm()->stacking_manager()->StackXidAtTopOfLayer(
      input_xid_, StackingManager::LAYER_TOPLEVEL_WINDOW);

  // Let the window know that it's maximized.
  vector<pair<XAtom, bool> > wm_state;
  wm_state.push_back(
      make_pair(wm()->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_HORZ), true));
  wm_state.push_back(
      make_pair(wm()->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_VERT), true));
  win->ChangeWmState(wm_state);

  win->MoveClientOffscreen();
  win->SetCompositedOpacity(0, 0);
  win->ShowComposited();
  // Make sure that we hear about button presses on this window.
  win->AddButtonGrab();

  gradient_actor_.reset(
      wm()->clutter()->CloneActor(static_gradient_texture_));
  gradient_actor_->SetOpacity(0, 0);
  gradient_actor_->SetVisibility(true);
  gradient_actor_->SetName("gradient screen");
  wm()->stage()->AddActor(gradient_actor_.get());
}

LayoutManager::ToplevelWindow::~ToplevelWindow() {
  while (!transients_.empty())
    RemoveTransientWindow(transients_.begin()->second->win);
  wm()->xconn()->DestroyWindow(input_xid_);
  win_->RemoveButtonGrab();
  win_ = NULL;
  layout_manager_ = NULL;
  input_xid_ = None;
  transient_to_focus_ = NULL;
}

int LayoutManager::ToplevelWindow::GetAbsoluteOverviewX() const {
  return layout_manager_->x() - layout_manager_->overview_panning_offset() +
      overview_x_;
}

int LayoutManager::ToplevelWindow::GetAbsoluteOverviewY() const {
  return layout_manager_->y() + overview_y_;
}

void LayoutManager::ToplevelWindow::ConfigureForActiveMode(
    bool window_is_active,
    bool to_left_of_active,
    bool update_focus) {
  const int layout_x = layout_manager_->x();
  const int layout_y = layout_manager_->y();
  const int layout_width = layout_manager_->width();
  const int layout_height = layout_manager_->height();

  // Center window vertically.
  const int win_y =
      layout_y + std::max(0, (layout_height - win_->client_height())) / 2;

  // TODO: This is a pretty huge mess.  Replace it with a saner way of
  // tracking animation state for windows.
  if (window_is_active) {
    // Center window horizontally.
    const int win_x =
        layout_x + std::max(0, (layout_width - win_->client_width())) / 2;
    if (state_ == STATE_NEW ||
        state_ == STATE_ACTIVE_MODE_OFFSCREEN ||
        state_ == STATE_ACTIVE_MODE_IN_FROM_RIGHT ||
        state_ == STATE_ACTIVE_MODE_IN_FROM_LEFT ||
        state_ == STATE_ACTIVE_MODE_IN_FADE) {
      // If the active window is in a state that requires that it be
      // animated in from a particular location or opacity, move it there
      // immediately.
      if (state_ == STATE_ACTIVE_MODE_IN_FROM_RIGHT) {
        win_->MoveComposited(layout_x + layout_width, win_y, 0);
        win_->SetCompositedOpacity(1.0, 0);
        win_->ScaleComposited(1.0, 1.0, 0);
      } else if (state_ == STATE_ACTIVE_MODE_IN_FROM_LEFT) {
        win_->MoveComposited(layout_x - win_->client_width(), win_y, 0);
        win_->SetCompositedOpacity(1.0, 0);
        win_->ScaleComposited(1.0, 1.0, 0);
      } else if (state_ == STATE_ACTIVE_MODE_IN_FADE) {
        win_->SetCompositedOpacity(0, 0);
        win_->MoveComposited(
            layout_x - 0.5 * kWindowFadeSizeFraction * win_->client_width(),
            layout_y - 0.5 * kWindowFadeSizeFraction * win_->client_height(),
            0);
        win_->ScaleComposited(
            1 + kWindowFadeSizeFraction, 1 + kWindowFadeSizeFraction, 0);
      } else {
        // Animate new or offscreen windows as moving up from the bottom
        // of the layout area.
        win_->MoveComposited(win_x, GetAbsoluteOverviewOffscreenY(), 0);
        win_->ScaleComposited(1.0, 1.0, 0);
      }
    }
    MoveAndScaleAllTransientWindows(0);

    // In any case, give the window input focus and animate it moving to
    // its final location.
    win_->MoveClient(win_x, win_y);
    win_->MoveComposited(win_x, win_y, LayoutManager::kWindowAnimMs);
    win_->ScaleComposited(1.0, 1.0, LayoutManager::kWindowAnimMs);
    win_->SetCompositedOpacity(1.0, LayoutManager::kWindowAnimMs);
    gradient_actor_->SetOpacity(0, 0);
    if (update_focus)
      TakeFocus(wm()->GetCurrentTimeFromServer());
    state_ = STATE_ACTIVE_MODE_ONSCREEN;
  } else {
    if (state_ == STATE_ACTIVE_MODE_OUT_TO_LEFT) {
      win_->MoveComposited(
          layout_x - win_->client_width(), win_y, LayoutManager::kWindowAnimMs);
    } else if (state_ == STATE_ACTIVE_MODE_OUT_TO_RIGHT) {
      win_->MoveComposited(layout_x + layout_width, win_y,
                           LayoutManager::kWindowAnimMs);
    } else if (state_ == STATE_ACTIVE_MODE_OUT_FADE) {
      win_->SetCompositedOpacity(0, LayoutManager::kWindowAnimMs);
      win_->MoveComposited(
          layout_x + 0.5 * kWindowFadeSizeFraction * win_->client_width(),
          layout_y + 0.5 * kWindowFadeSizeFraction * win_->client_height(),
          LayoutManager::kWindowAnimMs);
      win_->ScaleComposited(
          kWindowFadeSizeFraction, kWindowFadeSizeFraction,
          LayoutManager::kWindowAnimMs);
    } else {
      // Move non-active windows offscreen instead of just outside of the
      // layout manager area -- we don't want them to be briefly visible
      // if space opens up on the side due to a panel dock being hidden.
      //
      // We even move windows in STATE_ACTIVE_MODE_OFFSCREEN; the layout
      // manager size might've just changed due to a panel being undocked,
      // and we don't want the edges of these windows to be peeking
      // onscreen.
      int x = to_left_of_active ?  0 - overview_width_ : wm()->width();
      win_->MoveComposited(x, GetAbsoluteOverviewY(),
                           LayoutManager::kWindowAnimMs);
      gradient_actor_->Move(x, GetAbsoluteOverviewY(),
                            LayoutManager::kWindowAnimMs);
      win_->ScaleComposited(overview_scale_, overview_scale_,
                            LayoutManager::kWindowAnimMs);
      win_->SetCompositedOpacity(0.5, LayoutManager::kWindowAnimMs);
    }
    // Fade out the window's shadow entirely so it won't be visible if
    // the window is just slightly offscreen.
    win_->SetShadowOpacity(0, LayoutManager::kWindowAnimMs);
    state_ = STATE_ACTIVE_MODE_OFFSCREEN;
    win_->MoveClientOffscreen();
  }

  ApplyStackingForAllTransientWindows(false);  // stack in upper layer
  MoveAndScaleAllTransientWindows(LayoutManager::kWindowAnimMs);

  // TODO: Maybe just unmap input windows.
  wm()->xconn()->ConfigureWindowOffscreen(input_xid_);
}

void LayoutManager::ToplevelWindow::ConfigureForOverviewMode(
    bool window_is_magnified,
    bool dim_if_unmagnified,
    ToplevelWindow* toplevel_to_stack_under,
    bool incremental) {
  if (!incremental) {
    if (toplevel_to_stack_under) {
      win_->StackCompositedBelow(
          toplevel_to_stack_under->win()->GetBottomActor(), NULL, false);
      win_->StackClientBelow(toplevel_to_stack_under->win()->xid());
      wm()->xconn()->StackWindow(
          input_xid_, toplevel_to_stack_under->input_xid(), false);
    } else {
      wm()->stacking_manager()->StackWindowAtTopOfLayer(
          win_, StackingManager::LAYER_TOPLEVEL_WINDOW);
      wm()->stacking_manager()->StackXidAtTopOfLayer(
          input_xid_, StackingManager::LAYER_TOPLEVEL_WINDOW);
    }

    // We want to get new windows into their starting state immediately;
    // we animate other windows smoothly.
    const int anim_ms = (state_ == STATE_NEW) ? 0 : kOverviewAnimMs;

    win_->ScaleComposited(overview_scale_, overview_scale_, anim_ms);
    win_->SetCompositedOpacity(1.0, anim_ms);
    win_->MoveClientOffscreen();
    wm()->ConfigureInputWindow(input_xid_,
                               GetAbsoluteOverviewX(), GetAbsoluteOverviewY(),
                               overview_width_, overview_height_);
    ApplyStackingForAllTransientWindows(true);  // stack above toplevel

    gradient_actor_->Raise(
        !stacked_transients_->items().empty() ?
        stacked_transients_->items().front()->win->actor() :
        win_->actor());
    gradient_actor_->SetOpacity(window_is_magnified ? 0 : 1, anim_ms);

    // Make new windows slide in from the right.
    if (state_ == STATE_NEW) {
      const int initial_x = layout_manager_->x() + layout_manager_->width();
      const int initial_y = GetAbsoluteOverviewY();
      win_->MoveComposited(initial_x, initial_y, 0);
      gradient_actor_->Move(initial_x, initial_y, 0);
    }
    state_ = window_is_magnified ?
             STATE_OVERVIEW_MODE_MAGNIFIED :
             STATE_OVERVIEW_MODE_NORMAL;
  }

  win_->MoveComposited(GetAbsoluteOverviewX(), GetAbsoluteOverviewY(),
                       incremental ? 0 : kOverviewAnimMs);
  MoveAndScaleAllTransientWindows(incremental ? 0 : kOverviewAnimMs);
  gradient_actor_->Move(GetAbsoluteOverviewX(), GetAbsoluteOverviewY(),
                        incremental ? 0 : kOverviewAnimMs);
  gradient_actor_->Scale(
      overview_scale_ * win_->client_width() / gradient_actor_->GetWidth(),
      overview_scale_ * win_->client_height() / gradient_actor_->GetHeight(),
      incremental ? 0 : kOverviewAnimMs);
}

void LayoutManager::ToplevelWindow::UpdateOverviewScaling(int max_width,
                                                          int max_height) {
  double scale_x = max_width / static_cast<double>(win_->client_width());
  double scale_y = max_height / static_cast<double>(win_->client_height());
  double tmp_scale = std::min(scale_x, scale_y);

  overview_width_  = tmp_scale * win_->client_width();
  overview_height_ = tmp_scale * win_->client_height();
  overview_scale_  = tmp_scale;
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
  if (transient_win->type() == WmIpc::WINDOW_TYPE_CHROME_INFO_BUBBLE) {
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

  MoveAndScaleTransientWindow(transient.get(), 0);
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
    transient_win->ResizeClient(
        req_width, req_height, Window::GRAVITY_NORTHWEST);
    if (transient->centered) {
      transient->UpdateOffsetsToCenterOverOwnerWindow(win_);
      moved = true;
    }
  }

  if (moved)
    MoveAndScaleTransientWindow(transient, 0);
}

void LayoutManager::ToplevelWindow::HandleFocusChange(
    Window* focus_win, bool focus_in) {
  DCHECK(focus_win == win_ || GetTransientWindow(*focus_win));

  if (focus_in) {
    DLOG(INFO) << "Got focus-in for " << focus_win->xid_str()
               << "; removing passive button grab";
    focus_win->RemoveButtonGrab();
  } else {
    // Listen for button presses on this window so we'll know when it
    // should be focused again.
    DLOG(INFO) << "Got focus-out for " << focus_win->xid_str()
               << "; re-adding passive button grab";
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

void LayoutManager::ToplevelWindow::MoveAndScaleTransientWindow(
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
}

void LayoutManager::ToplevelWindow::MoveAndScaleAllTransientWindows(
    int anim_ms) {
  for (map<XWindow, shared_ptr<TransientWindow> >::iterator it =
           transients_.begin();
       it != transients_.end(); ++it) {
    MoveAndScaleTransientWindow(it->second.get(), anim_ms);
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
  DCHECK(stacked_transients_->items().size() > 1U);
  TransientWindow* transient_to_stack_above =
      stacked_transients_->items().front();
  stacked_transients_->Remove(transient);
  stacked_transients_->AddOnTop(transient);
  ApplyStackingForTransientWindow(transient, transient_to_stack_above->win);
}

}  // namespace window_manager
