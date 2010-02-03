// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/panel_bar.h"

#include <algorithm>

#include <gflags/gflags.h>

#include "base/logging.h"
#include "window_manager/clutter_interface.h"
#include "window_manager/panel.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x_connection.h"

DEFINE_string(panel_bar_image, "", "deprecated");
DEFINE_string(panel_anchor_image, "../assets/images/panel_anchor.png",
              "Image to use for anchors on the panel bar");

namespace window_manager {

using chromeos::NewPermanentCallback;
using std::make_pair;
using std::map;
using std::max;
using std::min;
using std::tr1::shared_ptr;
using std::vector;

const int PanelBar::kPixelsBetweenPanels = 3;

// Amount of time to take when arranging panels.
static const int kPanelArrangeAnimMs = 150;

// Amount of time to take when fading the panel anchor in or out.
static const int kAnchorFadeAnimMs = 150;

// Amount of time to take for expanding and collapsing panels.
static const int kPanelStateAnimMs = 150;

// Amount of time to take when animating a dropped panel sliding into the
// panel bar.
static const int kDroppedPanelAnimMs = 50;

// How many pixels away from the panel bar should a panel be dragged before
// it gets detached?
static const int kPanelDetachThresholdPixels = 50;

// How close does a panel need to get to the panel bar before it's attached?
static const int kPanelAttachThresholdPixels = 20;

PanelBar::PanelBar(WindowManager* wm)
    : wm_(wm),
      total_panel_width_(0),
      dragged_panel_(NULL),
      anchor_input_xid_(
          wm_->CreateInputWindow(-1, -1, 1, 1,
                                 ButtonPressMask | LeaveWindowMask)),
      anchor_panel_(NULL),
      anchor_actor_(wm_->clutter()->CreateImage(FLAGS_panel_anchor_image)),
      desired_panel_to_focus_(NULL) {
  anchor_actor_->SetName("panel anchor");
  anchor_actor_->SetOpacity(0, 0);
  wm_->stage()->AddActor(anchor_actor_.get());
  wm_->stacking_manager()->StackActorAtTopOfLayer(
      anchor_actor_.get(), StackingManager::LAYER_PANEL_BAR);
}

PanelBar::~PanelBar() {
  wm_->xconn()->DestroyWindow(anchor_input_xid_);
}

void PanelBar::GetInputWindows(vector<XWindow>* windows_out) {
  CHECK(windows_out);
  windows_out->clear();
  windows_out->push_back(anchor_input_xid_);
}

void PanelBar::AddPanel(Panel* panel, PanelSource source, bool expanded) {
  DCHECK(panel);

  shared_ptr<PanelInfo> info(new PanelInfo);
  info->is_expanded = expanded;
  info->snapped_right =
      wm_->width() - total_panel_width_ - kPixelsBetweenPanels;
  panel_infos_.insert(make_pair(panel, info));

  panels_.insert(panels_.begin(), panel);
  total_panel_width_ += panel->width() + kPixelsBetweenPanels;

  // If the panel is being dragged, move it to the correct position within
  // 'panels_' and repack all other panels.
  if (source == PANEL_SOURCE_DRAGGED)
    ReorderPanel(panel);

  panel->StackAtTopOfLayer(source == PANEL_SOURCE_DRAGGED ?
                           StackingManager::LAYER_DRAGGED_PANEL :
                           StackingManager::LAYER_STATIONARY_PANEL);

  const int final_y = wm_->height() -
      (expanded ? panel->total_height() : panel->titlebar_height());

  // Now move the panel to its final position.
  switch (source) {
    case PANEL_SOURCE_NEW:
      // Make newly-created panels animate in from offscreen.
      panel->Move(info->snapped_right, wm_->height(), false, 0);
      panel->MoveY(final_y, true, kPanelStateAnimMs);
      break;
    case PANEL_SOURCE_DRAGGED:
      panel->MoveY(final_y, true, 0);
      break;
    case PANEL_SOURCE_DROPPED:
      panel->Move(info->snapped_right, final_y, true, kDroppedPanelAnimMs);
      break;
    default:
      NOTREACHED() << "Unknown panel source " << source;
  }

  panel->SetResizable(expanded);
  panel->NotifyChromeAboutState(expanded);

  // If this is a new panel or it was already focused (e.g. it was
  // focused when it got detached, and now it's being reattached),
  // call FocusPanel() to focus it if needed and update
  // 'desired_panel_to_focus_'.
  if (expanded &&
      (source == PANEL_SOURCE_NEW || panel->content_win()->focused())) {
    FocusPanel(panel, false, wm_->GetCurrentTimeFromServer());
  } else {
    panel->AddButtonGrab();
  }
}

void PanelBar::RemovePanel(Panel* panel) {
  DCHECK(panel);

  if (anchor_panel_ == panel)
    DestroyAnchor();
  if (dragged_panel_ == panel)
    dragged_panel_ = NULL;
  // If this was a focused content window, then let's try to find a nearby
  // panel to focus if we get asked to do so later.
  if (desired_panel_to_focus_ == panel)
    desired_panel_to_focus_ = GetNearestExpandedPanel(panel);

  CHECK(panel_infos_.erase(panel) == 1);
  Panels::iterator it =
      FindPanelInVectorByWindow(panels_, *(panel->content_win()));
  if (it == panels_.end()) {
    LOG(WARNING) << "Got request to remove panel " << panel->xid_str()
                 << " but didn't find it in panels_";
    return;
  }

  total_panel_width_ -= ((*it)->width() + kPixelsBetweenPanels);
  panels_.erase(it);

  PackPanels(dragged_panel_);
  if (dragged_panel_)
    ReorderPanel(dragged_panel_);
}

bool PanelBar::ShouldAddDraggedPanel(const Panel* panel,
                                     int drag_x,
                                     int drag_y) {
  return drag_y + panel->total_height() >
         wm_->height() - kPanelAttachThresholdPixels;
}

void PanelBar::HandleInputWindowButtonPress(XWindow xid,
                                            int x, int y,
                                            int x_root, int y_root,
                                            int button,
                                            Time timestamp) {
  CHECK_EQ(xid, anchor_input_xid_);
  if (button != 1)
    return;

  // Destroy the anchor and collapse the corresponding panel.
  VLOG(1) << "Got button press in anchor window";
  Panel* panel = anchor_panel_;
  DestroyAnchor();
  if (panel)
    CollapsePanel(panel);
  else
    LOG(WARNING) << "Anchor panel no longer exists";
}

void PanelBar::HandleInputWindowPointerLeave(XWindow xid, Time timestamp) {
  CHECK_EQ(xid, anchor_input_xid_);

  // TODO: There appears to be a bit of a race condition here.  If the
  // mouse cursor has already been moved away before the anchor input
  // window gets created, the anchor never gets a mouse leave event.  Find
  // some way to work around this.
  VLOG(1) << "Got mouse leave in anchor window";
  DestroyAnchor();
}

void PanelBar::HandlePanelButtonPress(
    Panel* panel, int button, Time timestamp) {
  DCHECK(panel);
  VLOG(1) << "Got button press in panel " << panel->xid_str()
          << "; giving it the focus";
  // Get rid of the passive button grab, and then ungrab the pointer
  // and replay events so the panel will get a copy of the click.
  FocusPanel(panel, true, timestamp);  // remove_pointer_grab=true
}

void PanelBar::HandlePanelFocusChange(Panel* panel, bool focus_in) {
  DCHECK(panel);
  if (!focus_in)
    panel->AddButtonGrab();
}

void PanelBar::HandleSetPanelStateMessage(Panel* panel, bool expand) {
  DCHECK(panel);
  if (expand)
    ExpandPanel(panel, true, kPanelStateAnimMs);
  else
    CollapsePanel(panel);
}

bool PanelBar::HandleNotifyPanelDraggedMessage(Panel* panel,
                                               int drag_x,
                                               int drag_y) {
  DCHECK(panel);

  VLOG(2) << "Notified about drag of panel " << panel->xid_str()
          << " to (" << drag_x << ", " << drag_y << ")";

  PanelInfo* info = GetPanelInfoOrDie(panel);
  const int y_threshold =
      wm_->height() - panel->total_height() - kPanelDetachThresholdPixels;
  if (info->is_expanded && drag_y <= y_threshold)
    return false;

  if (dragged_panel_ != panel) {
    if (dragged_panel_) {
      LOG(WARNING) << "Abandoning dragged panel " << dragged_panel_->xid_str()
                   << " in favor of " << panel->xid_str();
      HandlePanelDragComplete(dragged_panel_);
    }

    VLOG(2) << "Starting drag of panel " << panel->xid_str();
    dragged_panel_ = panel;
    panel->StackAtTopOfLayer(StackingManager::LAYER_DRAGGED_PANEL);
  }

  dragged_panel_->MoveX((wm_->wm_ipc_version() >= 1) ?
                          drag_x :
                          drag_x + dragged_panel_->titlebar_width(),
                        false, 0);

  ReorderPanel(dragged_panel_);
  return true;
}

void PanelBar::HandleNotifyPanelDragCompleteMessage(Panel* panel) {
  DCHECK(panel);
  HandlePanelDragComplete(panel);
}

void PanelBar::HandleFocusPanelMessage(Panel* panel) {
  DCHECK(panel);
  if (!GetPanelInfoOrDie(panel)->is_expanded)
    ExpandPanel(panel, false, kPanelStateAnimMs);
  FocusPanel(panel, false, wm_->GetCurrentTimeFromServer());
}

void PanelBar::HandleScreenResize() {
  // Make all of the panels jump to their new Y positions first and then
  // repack them to animate them sliding to their new X positions.
  for (Panels::iterator it = panels_.begin(); it != panels_.end(); ++it) {
    Panel* panel = *it;
    int new_y = GetPanelInfoOrDie(panel)->is_expanded ?
        wm_->height() - panel->total_height() :
        wm_->height() - panel->titlebar_height();
    (*it)->MoveY(new_y, true, 0);
  }
  PackPanels(dragged_panel_);
  if (dragged_panel_)
    ReorderPanel(dragged_panel_);
}

bool PanelBar::TakeFocus() {
  Time timestamp = wm_->GetCurrentTimeFromServer();

  // If we already decided on a panel to focus, use it.
  if (desired_panel_to_focus_) {
    FocusPanel(desired_panel_to_focus_, false, timestamp);
    return true;
  }

  // Just focus the first expanded panel.
  for (Panels::iterator it = panels_.begin(); it != panels_.end(); ++it) {
    if (GetPanelInfoOrDie(*it)->is_expanded) {
      FocusPanel(*it, false, timestamp);  // remove_pointer_grab=false
      return true;
    }
  }
  return false;
}


PanelBar::PanelInfo* PanelBar::GetPanelInfoOrDie(Panel* panel) {
  shared_ptr<PanelInfo> info =
      FindWithDefault(panel_infos_, panel, shared_ptr<PanelInfo>());
  CHECK(info.get());
  return info.get();
}

void PanelBar::ExpandPanel(Panel* panel, bool create_anchor, int anim_ms) {
  CHECK(panel);
  PanelInfo* info = GetPanelInfoOrDie(panel);
  if (info->is_expanded) {
    LOG(WARNING) << "Ignoring request to expand already-expanded panel "
                 << panel->xid_str();
    return;
  }

  panel->MoveY(wm_->height() - panel->total_height(), true, anim_ms);
  panel->SetResizable(true);
  panel->NotifyChromeAboutState(true);
  info->is_expanded = true;
  if (create_anchor)
    CreateAnchor(panel);
}

void PanelBar::CollapsePanel(Panel* panel) {
  CHECK(panel);
  PanelInfo* info = GetPanelInfoOrDie(panel);
  if (!info->is_expanded) {
    LOG(WARNING) << "Ignoring request to collapse already-collapsed panel "
                 << panel->xid_str();
    return;
  }

  // In case we need to focus another panel, find the nearest one before we
  // collapse this one.
  Panel* panel_to_focus = GetNearestExpandedPanel(panel);

  if (anchor_panel_ == panel)
    DestroyAnchor();

  panel->MoveY(wm_->height() - panel->titlebar_height(),
               true, kPanelStateAnimMs);
  panel->SetResizable(false);
  panel->NotifyChromeAboutState(false);
  info->is_expanded = false;

  // Give up the focus if this panel had it.
  if (panel->content_win()->focused()) {
    desired_panel_to_focus_ = panel_to_focus;
    if (!TakeFocus()) {
      wm_->SetActiveWindowProperty(None);
      wm_->TakeFocus();
    }
  }
}

void PanelBar::FocusPanel(Panel* panel,
                          bool remove_pointer_grab,
                          Time timestamp) {
  CHECK(panel);
  panel->RemoveButtonGrab(true);  // remove_pointer_grab
  wm_->SetActiveWindowProperty(panel->content_win()->xid());
  panel->content_win()->TakeFocus(timestamp);
  desired_panel_to_focus_ = panel;
}

Panel* PanelBar::GetPanelByWindow(const Window& win) {
  Panels::iterator it = FindPanelInVectorByWindow(panels_, win);
  return (it != panels_.end()) ? *it : NULL;
}

// static
PanelBar::Panels::iterator PanelBar::FindPanelInVectorByWindow(
    Panels& panels, const Window& win) {
  for (Panels::iterator it = panels.begin(); it != panels.end(); ++it)
    if ((*it)->titlebar_win() == &win || (*it)->content_win() == &win)
      return it;
  return panels.end();
}

void PanelBar::HandlePanelDragComplete(Panel* panel) {
  CHECK(panel);
  VLOG(2) << "Got notification that panel drag is complete for "
          << panel->xid_str();
  if (dragged_panel_ != panel)
    return;

  panel->StackAtTopOfLayer(StackingManager::LAYER_STATIONARY_PANEL);
  panel->MoveX(
      GetPanelInfoOrDie(panel)->snapped_right, true, kPanelArrangeAnimMs);
  dragged_panel_ = NULL;
}

void PanelBar::ReorderPanel(Panel* fixed_panel) {
  CHECK(fixed_panel);

  // First, find the position of the dragged panel.
  Panels::iterator fixed_it = FindPanelInVectorByWindow(
      panels_, *(fixed_panel->content_win()));
  CHECK(fixed_it != panels_.end());

  // Next, check if the center of the panel has moved over another panel.
  const int center_x = (wm_->wm_ipc_version() >= 1) ?
                       fixed_panel->right() - 0.5 * fixed_panel->width() :
                       fixed_panel->right() + 0.5 * fixed_panel->width();
  Panels::iterator it = panels_.begin();
  for (; it != panels_.end(); ++it) {
    int snapped_left = 0, snapped_right = 0;
    if (*it == fixed_panel) {
      // If we're comparing against ourselves, use our original position
      // rather than wherever we've currently been dragged by the user.
      PanelInfo* info = GetPanelInfoOrDie(fixed_panel);
      snapped_left = info->snapped_right - fixed_panel->width();
      snapped_right = info->snapped_right;
    } else {
      snapped_left = (*it)->content_x();
      snapped_right = (*it)->right();
    }
    if (center_x >= snapped_left && center_x < snapped_right)
      break;
  }

  // If it has, then we reorder the panels.
  if (it != panels_.end() && *it != fixed_panel) {
    if (it > fixed_it)
      rotate(fixed_it, fixed_it + 1, it + 1);
    else
      rotate(it, fixed_it, fixed_it + 1);
    PackPanels(fixed_panel);
  }
}

void PanelBar::PackPanels(Panel* fixed_panel) {
  total_panel_width_ = 0;

  for (Panels::reverse_iterator it = panels_.rbegin();
       it != panels_.rend(); ++it) {
    Panel* panel = *it;
    // TODO: PackPanels() gets called in response to every move message
    // that we receive about a dragged panel.  Check that it's not too
    // inefficient to do all of these lookups vs. storing the panel info
    // alongside the Panel pointer in 'panels_'.
    PanelInfo* info = GetPanelInfoOrDie(panel);

    info->snapped_right =
        wm_->width() - total_panel_width_ - kPixelsBetweenPanels;
    if (panel != fixed_panel && panel->right() != info->snapped_right)
      panel->MoveX(info->snapped_right, true, kPanelArrangeAnimMs);

    total_panel_width_ += panel->width() + kPixelsBetweenPanels;
  }
}

void PanelBar::CreateAnchor(Panel* panel) {
  int pointer_x = 0;
  wm_->xconn()->QueryPointerPosition(&pointer_x, NULL);

  const int width = anchor_actor_->GetWidth();
  const int height = anchor_actor_->GetHeight();
  const int x = min(max(static_cast<int>(pointer_x - 0.5 * width), 0),
                    wm_->width() - width);
  const int y = wm_->height() - height;

  wm_->ConfigureInputWindow(anchor_input_xid_, x, y, width, height);
  anchor_panel_ = panel;
  anchor_actor_->Move(x, y, 0);
  anchor_actor_->SetOpacity(1, kAnchorFadeAnimMs);
}

void PanelBar::DestroyAnchor() {
  wm_->xconn()->ConfigureWindowOffscreen(anchor_input_xid_);
  anchor_actor_->SetOpacity(0, kAnchorFadeAnimMs);
  anchor_panel_ = NULL;
}

Panel* PanelBar::GetNearestExpandedPanel(Panel* panel) {
  if (!panel || !GetPanelInfoOrDie(panel)->is_expanded)
    return NULL;

  Panel* nearest_panel = NULL;
  int best_distance = kint32max;
  for (Panels::iterator it = panels_.begin(); it != panels_.end(); ++it) {
    if (*it == panel || !GetPanelInfoOrDie(*it)->is_expanded)
      continue;

    int distance = kint32max;
    if ((*it)->right() <= panel->content_x())
      distance = panel->content_x() - (*it)->right();
    else if ((*it)->content_x() >= panel->right())
      distance = (*it)->content_x() - panel->right();
    else
      distance = abs((*it)->content_center() - panel->content_center());

    if (distance < best_distance) {
      best_distance = distance;
      nearest_panel = *it;
    }
  }
  return nearest_panel;
}

}  // namespace window_manager
