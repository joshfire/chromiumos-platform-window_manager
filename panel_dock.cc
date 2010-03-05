// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/panel_dock.h"

#include <algorithm>
#include <utility>

#include <gflags/gflags.h>

#include "window_manager/event_consumer_registrar.h"
#include "window_manager/panel.h"
#include "window_manager/panel_manager.h"
#include "window_manager/shadow.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

DEFINE_string(panel_dock_background_image,
              "../assets/images/panel_dock_bg.png",
              "Image to use for panel dock backgrounds");

namespace window_manager {

using std::find;
using std::make_pair;
using std::tr1::shared_ptr;
using std::vector;


// Amount of time to take for sliding the dock background in or out when
// the dock is shown or hidden.
// TODO: This animation looks janky (there's a brief flash where the WM
// background image is visible), so we disable it for now.
static const int kBackgroundAnimMs = 0;

// Amount of time to take when fading a panel's shadow in or out as it's
// detached or attached.
static const int kPanelShadowAnimMs = 150;

// Amount of time to take when packing panels into the dock.
static const int kPackPanelsAnimMs = 150;

const int PanelDock::kDetachThresholdPixels = 50;
const int PanelDock::kAttachThresholdPixels = 20;

PanelDock::PanelDock(PanelManager* panel_manager, DockType type, int width)
    : panel_manager_(panel_manager),
      type_(type),
      x_(type == DOCK_TYPE_LEFT ? 0 : wm()->width() - width),
      y_(0),
      width_(width),
      height_(wm()->height()),
      total_panel_height_(0),
      dragged_panel_(NULL),
      bg_actor_(wm()->clutter()->CreateImage(
                    FLAGS_panel_dock_background_image)),
      bg_shadow_(new Shadow(wm()->clutter())),
      bg_input_xid_(wm()->CreateInputWindow(
                        -1, -1, 1, 1, ButtonPressMask|ButtonReleaseMask)),
      event_consumer_registrar_(
          new EventConsumerRegistrar(wm(), panel_manager_)) {
  event_consumer_registrar_->RegisterForWindowEvents(bg_input_xid_);

  wm()->stacking_manager()->StackXidAtTopOfLayer(
      bg_input_xid_, StackingManager::LAYER_PANEL_DOCK);

  const int bg_x = (type == DOCK_TYPE_LEFT) ? x_ - width_ : x_ + width_;
  bg_shadow_->group()->SetName("panel dock background shadow");
  wm()->stage()->AddActor(bg_shadow_->group());
  bg_shadow_->Resize(width_, height_, 0);
  bg_shadow_->Move(bg_x, y_, 0);
  bg_shadow_->SetOpacity(0, 0);
  bg_shadow_->Show();
  wm()->stacking_manager()->StackActorAtTopOfLayer(
      bg_shadow_->group(), StackingManager::LAYER_PANEL_DOCK);

  bg_actor_->SetName("panel dock background");
  wm()->stage()->AddActor(bg_actor_.get());
  bg_actor_->SetSize(width_, height_);
  bg_actor_->Move(bg_x, y_, 0);
  bg_actor_->SetVisibility(true);
  wm()->stacking_manager()->StackActorAtTopOfLayer(
      bg_actor_.get(), StackingManager::LAYER_PANEL_DOCK);
}

PanelDock::~PanelDock() {
  wm()->xconn()->DestroyWindow(bg_input_xid_);
  dragged_panel_ = NULL;
}

void PanelDock::GetInputWindows(std::vector<XWindow>* windows_out) {
  DCHECK(windows_out);
  windows_out->clear();
  windows_out->push_back(bg_input_xid_);
}

void PanelDock::AddPanel(Panel* panel, PanelSource source) {
  DCHECK(panel);
  DCHECK(find(panels_.begin(), panels_.end(), panel) == panels_.end());

  shared_ptr<PanelInfo> info(new PanelInfo);
  info->snapped_y = total_panel_height_;
  CHECK(panel_infos_.insert(make_pair(panel, info)).second);

  panels_.push_back(panel);
  total_panel_height_ += panel->total_height();
  if (source == PANEL_SOURCE_DRAGGED)
    ReorderPanel(panel);

  if (panels_.size() == static_cast<size_t>(1)) {
    wm()->ConfigureInputWindow(bg_input_xid_, x_, y_, width_, height_);
    bg_actor_->MoveX(x_, kBackgroundAnimMs);
    bg_shadow_->MoveX(x_, kBackgroundAnimMs);
    bg_shadow_->SetOpacity(1, kBackgroundAnimMs);
    panel_manager_->HandleDockVisibilityChange(this);
  }

  panel->StackAtTopOfLayer(
      source == PANEL_SOURCE_DRAGGED ?
        StackingManager::LAYER_DRAGGED_PANEL :
        StackingManager::LAYER_STATIONARY_PANEL_IN_DOCK);

  // Try to make the panel fit vertically within our dimensions.
  int panel_y = panel->titlebar_y();
  if (panel_y + panel->total_height() > y_ + height_)
    panel_y = y_ + height_ - panel->total_height();
  if (panel_y < y_)
    panel_y = y_;
  panel->Move(type_ == DOCK_TYPE_RIGHT ?  x_ + width_ : x_ + panel->width(),
              panel_y, true, 0);
  // TODO: Ideally, we would resize the panel here to match our width, but
  // that messes up the subsequent notification messages about the panel
  // being dragged -- some of them will be with regard to the panel's old
  // dimensions and others will be with regard to the new dimensions.
  // Instead, we defer resizing the panel until the drag is complete.

  if (panel->content_win()->focused()) {
    FocusPanel(panel, false, wm()->GetCurrentTimeFromServer());
  } else {
    panel->AddButtonGrab();
  }
}

void PanelDock::RemovePanel(Panel* panel) {
  if (dragged_panel_ == panel)
    dragged_panel_ = NULL;

  vector<Panel*>::iterator it = find(panels_.begin(), panels_.end(), panel);
  DCHECK(it != panels_.end());
  panels_.erase(it);
  CHECK_EQ(static_cast<int>(panel_infos_.erase(panel)), 1);

  if (panels_.empty()) {
    const int bg_x = type_ == DOCK_TYPE_LEFT ? x_ - width_ : x_ + width_;
    wm()->xconn()->ConfigureWindowOffscreen(bg_input_xid_);
    bg_actor_->MoveX(bg_x, kBackgroundAnimMs);
    bg_shadow_->MoveX(bg_x, kBackgroundAnimMs);
    bg_shadow_->SetOpacity(0, kBackgroundAnimMs);
    panel_manager_->HandleDockVisibilityChange(this);
  } else {
    PackPanels(dragged_panel_);
  }
}

bool PanelDock::ShouldAddDraggedPanel(const Panel* panel,
                                      int drag_x,
                                      int drag_y) {
  return (type_ == DOCK_TYPE_RIGHT) ?
         drag_x >= x_ + width_ - kAttachThresholdPixels :
         drag_x - panel->content_width() <= x_ + kAttachThresholdPixels;
}

void PanelDock::HandlePanelButtonPress(Panel* panel,
                                       int button,
                                       XTime timestamp) {
  FocusPanel(panel, true, timestamp);
}

void PanelDock::HandlePanelFocusChange(Panel* panel, bool focus_in) {
  if (!focus_in)
    panel->AddButtonGrab();
}

void PanelDock::HandleSetPanelStateMessage(Panel* panel, bool expand) {
  LOG(WARNING) << "Ignoring request to " << (expand ? "expand" : "collapse")
               << " docked panel " << panel->xid_str();
}

bool PanelDock::HandleNotifyPanelDraggedMessage(Panel* panel,
                                                int drag_x,
                                                int drag_y) {
  if (type_ == DOCK_TYPE_RIGHT) {
    if (drag_x <= x_ + width_ - kDetachThresholdPixels)
      return false;
  } else {
    if (drag_x - panel->content_width() >= x_ + kDetachThresholdPixels)
      return false;
  }

  if (dragged_panel_ != panel) {
    dragged_panel_ = panel;
    panel->StackAtTopOfLayer(StackingManager::LAYER_DRAGGED_PANEL);
    panel->SetShadowOpacity(1, kPanelShadowAnimMs);
  }

  // Cap the drag position within the Y bounds of the dock.
  if (drag_y + panel->total_height() > y_ + height_)
    drag_y = y_ + height_ - panel->total_height();
  if (drag_y < y_)
    drag_y = y_;

  panel->MoveY(drag_y, false, 0);
  ReorderPanel(panel);
  return true;
}

void PanelDock::HandleNotifyPanelDragCompleteMessage(Panel* panel) {
  if (dragged_panel_ != panel)
    return;
  // Move client windows.
  panel->Move(panel->right(), panel->titlebar_y(), true, 0);
  if (panel->width() != width_) {
    panel->ResizeContent(
        width_, panel->content_height(),
        type_ == DOCK_TYPE_RIGHT ?
          Window::GRAVITY_NORTHEAST :
          Window::GRAVITY_NORTHWEST);
  }
  panel->SetShadowOpacity(0, kPanelShadowAnimMs);
  panel->StackAtTopOfLayer(StackingManager::LAYER_STATIONARY_PANEL_IN_DOCK);
  dragged_panel_ = NULL;
  PackPanels(NULL);
}

void PanelDock::HandleFocusPanelMessage(Panel* panel) {
  FocusPanel(panel, false, wm()->GetCurrentTimeFromServer());
}

void PanelDock::HandlePanelResize(Panel* panel) {
  // TODO: We should probably prevent a panel's width from being changed at
  // all while it's docked, and repack all the panels in the dock if the
  // panel's height is changed.
}

void PanelDock::HandleScreenResize() {
  height_ = wm()->height();
  if (type_ == DOCK_TYPE_RIGHT)
    x_ = wm()->width() - width_;

  bool hidden = panels_.empty();

  // Move the background.
  int bg_x = x_;
  if (hidden)
    bg_x = (type_ == DOCK_TYPE_LEFT) ? x_ - width_ : x_ + width_;
  bg_actor_->SetSize(width_, height_);
  bg_actor_->Move(bg_x, y_, 0);
  bg_shadow_->Resize(width_, height_, 0);
  bg_shadow_->Move(bg_x, y_, 0);
  if (!hidden)
    wm()->ConfigureInputWindow(bg_input_xid_, x_, y_, width_, height_);

  // If we're on the right side of the screen, we need to move the panels.
  if (type_ == DOCK_TYPE_RIGHT) {
    for (vector<Panel*>::iterator it = panels_.begin();
         it != panels_.end(); ++it) {
      (*it)->MoveX(x_ + width_, true, 0);
    }
  }
}

WindowManager* PanelDock::wm() { return panel_manager_->wm(); }

PanelDock::PanelInfo* PanelDock::GetPanelInfoOrDie(Panel* panel) {
  shared_ptr<PanelInfo> info =
      FindWithDefault(panel_infos_, panel, shared_ptr<PanelInfo>());
  CHECK(info.get());
  return info.get();
}

void PanelDock::ReorderPanel(Panel* fixed_panel) {
  DCHECK(fixed_panel);

  Panels::iterator src_it = find(panels_.begin(), panels_.end(), fixed_panel);
  DCHECK(src_it != panels_.end());
  const int src_position = src_it - panels_.begin();

  int dest_position = src_position;
  if (fixed_panel->titlebar_y() < GetPanelInfoOrDie(fixed_panel)->snapped_y) {
    // If we're above our snapped position, look for the furthest panel
    // whose midpoint has been passed by our top edge.
    for (int i = src_position - 1; i >= 0; --i) {
      Panel* panel = panels_[i];
      if (fixed_panel->titlebar_y() <=
          panel->titlebar_y() + 0.5 * panel->total_height()) {
        dest_position = i;
      } else {
        break;
      }
    }
  } else {
    // Otherwise, do the same check with our bottom edge below us.
    for (int i = src_position + 1; i < static_cast<int>(panels_.size()); ++i) {
      Panel* panel = panels_[i];
      if (fixed_panel->titlebar_y() + fixed_panel->total_height() >
          panel->titlebar_y() + 0.5 * panel->total_height()) {
        dest_position = i;
      } else {
        break;
      }
    }
  }

  if (dest_position != src_position) {
    Panels::iterator dest_it = panels_.begin() + dest_position;
    if (dest_it > src_it)
      rotate(src_it, src_it + 1, dest_it + 1);
    else
      rotate(dest_it, src_it, src_it + 1);
    PackPanels(fixed_panel);
  }
}

void PanelDock::PackPanels(Panel* fixed_panel) {
  int total_panel_height_ = 0;
  for (vector<Panel*>::iterator it = panels_.begin();
       it != panels_.end(); ++it) {
    Panel* panel = *it;
    PanelInfo* info = GetPanelInfoOrDie(panel);
    info->snapped_y = total_panel_height_;
    if (panel != fixed_panel && panel->titlebar_y() != info->snapped_y)
      panel->MoveY(info->snapped_y, true, kPackPanelsAnimMs);
    total_panel_height_ += panel->total_height();
  }
}

void PanelDock::FocusPanel(Panel* panel,
                           bool remove_pointer_grab,
                           XTime timestamp) {
  DCHECK(panel);
  panel->RemoveButtonGrab(remove_pointer_grab);
  wm()->SetActiveWindowProperty(panel->content_win()->xid());
  panel->content_win()->TakeFocus(timestamp);
}

};  // namespace window_manager
