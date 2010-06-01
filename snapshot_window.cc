// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/snapshot_window.h"

#include <algorithm>
#include <cmath>
#include <tr1/memory>

#include <gflags/gflags.h>

#include "base/logging.h"
#include "base/string_util.h"
#include "window_manager/atom_cache.h"
#include "window_manager/callback.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/motion_event_coalescer.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/system_metrics.pb.h"
#include "window_manager/toplevel_window.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x_connection.h"

// Define this to get copius output from this file.
#if !defined(EXTRA_LOGGING)
#undef EXTRA_LOGGING
#endif

namespace window_manager {

using std::string;

const float LayoutManager::SnapshotWindow::kUnselectedTilt = 0.9;

LayoutManager::SnapshotWindow::SnapshotWindow(Window* win,
                                              LayoutManager* layout_manager)
    : win_(win),
      layout_manager_(layout_manager),
      tab_index_(-1),
      toplevel_(NULL),
      toplevel_xid_(0),
      input_xid_(wm()->CreateInputWindow(-1, -1, 1, 1,
                                         ButtonPressMask | ButtonReleaseMask)),
      state_(STATE_NEW),
      last_state_(STATE_NEW),
      overview_x_(0),
      overview_y_(0),
      overview_width_(0),
      overview_height_(0),
      overview_scale_(1.f),
      event_consumer_registrar_(
          new EventConsumerRegistrar(wm(), layout_manager_)) {
#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Creating SnapshotWindow for window " << win_->xid_str();
#endif

  event_consumer_registrar_->RegisterForWindowEvents(win_->xid());
  event_consumer_registrar_->RegisterForWindowEvents(input_xid_);

  if (win_->type_params().size() > 0) {
    toplevel_xid_ = win_->type_params()[0];
  } else {
    LOG(ERROR) << "Window " << win_->xid_str()
               << " has incorrect type parameters.";
  }

  PropertiesChanged();

  wm()->stacking_manager()->StackXidAtTopOfLayer(
      input_xid_, StackingManager::LAYER_SNAPSHOT_WINDOW);
  wm()->SetNamePropertiesForXid(
      input_xid_, string("input window for snapshot ") + win_->xid_str());

  // Move the composited window offscreen before showing it.
  win_->MoveComposited(wm()->width(), wm()->height(), 0);

  // Show the composited window.
  win_->ShowComposited();

  // Move the client offscreen -- it doesn't need to receive any
  // input.
  win_->MoveClientOffscreen();
}

LayoutManager::SnapshotWindow::~SnapshotWindow() {
#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Deleting snapshot window " << win_->xid_str();
#endif
  wm()->xconn()->DestroyWindow(input_xid_);
  win_ = NULL;
  layout_manager_ = NULL;
  input_xid_ = 0;
}

string LayoutManager::SnapshotWindow::GetStateName(State state) {
  switch (state) {
    case STATE_NEW:
      return string("New");
    case STATE_ACTIVE_MODE_INVISIBLE:
      return string("Active Mode Invisible");
    case STATE_OVERVIEW_MODE_NORMAL:
      return string("Overview Mode Normal");
    case STATE_OVERVIEW_MODE_SELECTED:
      return string("Overview Mode Selected");
    default:
      LOG(WARNING) << "Unknown state " << state << " encountered.";
      return string("Unknown State");
  }
}

void LayoutManager::SnapshotWindow::SetState(State state) {
#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Switching snapshot " << win_->xid_str()
            << " state from " << GetStateName(state_) << " to "
            << GetStateName(state);
#endif
  state_ = state;
}

void LayoutManager::SnapshotWindow::UpdateLayout(bool animate) {
#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Updating layout for snapshot "
            << win_->xid_str() << " in state "
            << GetStateName(last_state_);
#endif
  if (state_ == STATE_ACTIVE_MODE_INVISIBLE) {
    ConfigureForActiveMode(animate);
  } else {
    ConfigureForOverviewMode(animate);
  }
  last_state_ = state_;
}

bool LayoutManager::SnapshotWindow::PropertiesChanged() {
  // TODO: Handle changes in the toplevel window here too.
  int old_tab_index = tab_index_;

  // Notice if the tab_index changed.
  if (win_->type_params().size() > 1) {
    tab_index_ = win_->type_params()[1];
  } else {
    LOG(ERROR) << "Chrome snapshot window " << win_->xid_str()
               << " has missing parameters.";
    tab_index_ = -1;
  }

  bool changed = tab_index_ != old_tab_index;
#if defined(EXTRA_LOGGING)
  if (changed) {
    DLOG(INFO) << "Properties of snapshot " << win_->xid_str()
              << " changed index from " << old_tab_index
              << " to " << tab_index_;
  }
#endif
  return changed;
}

int LayoutManager::SnapshotWindow::CalculateOverallIndex() const {
  if (toplevel()) {
    return layout_manager_->GetPreceedingTabCount(*(toplevel())) +
        tab_index();
  }
  return -1;
}

void LayoutManager::SnapshotWindow::ConfigureForActiveMode(bool animate) {
  const int anim_ms = animate ? LayoutManager::kWindowAnimMs : 0;
  const int opacity_anim_ms = animate ? LayoutManager::kWindowOpacityAnimMs : 0;
#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Configuring snapshot " << win_->xid_str()
            << " for " << GetStateName(state_);
#endif
  win_->SetCompositedOpacity(0, opacity_anim_ms);

  int start_x = layout_manager_->x() +
                layout_manager_->width() -
                (win_->composited_scale_x() * win_->client_width());
  int start_y = layout_manager_->y() +
                layout_manager_->height() -
                (win_->composited_scale_y() * win_->client_height());

  // If this window is to the right of the selected snapshot window, then
  // we want to start offscreen to the right.
  if (layout_manager_->current_snapshot() &&
      CalculateOverallIndex() >
      layout_manager_->current_snapshot()->CalculateOverallIndex()) {
    start_x += layout_manager_->width();
  }

  win_->MoveComposited(start_x, start_y, anim_ms);

  // The snapshot should cover the screen in its largest dimension.
  float snapshot_scale = std::min(
      layout_manager_->width() / win_->client_width(),
      layout_manager_->height() / win_->client_height());
  win_->ScaleComposited(snapshot_scale, snapshot_scale, anim_ms);
  win_->actor()->ShowDimmed(false, anim_ms);
  win_->actor()->SetTilt(0.0, anim_ms);

  // TODO: Maybe just unmap input windows.
  wm()->xconn()->ConfigureWindowOffscreen(input_xid_);
}

void LayoutManager::SnapshotWindow::ConfigureForOverviewMode(bool animate) {
  if (state_ == STATE_ACTIVE_MODE_INVISIBLE)
    return;

  const int anim_ms = animate ? LayoutManager::kWindowAnimMs : 0;
  const int opacity_anim_ms = animate ? LayoutManager::kWindowOpacityAnimMs : 0;

  if (last_state_ != STATE_OVERVIEW_MODE_SELECTED &&
      last_state_ != STATE_OVERVIEW_MODE_NORMAL) {
#if defined(EXTRA_LOGGING)
    DLOG(INFO) << "Performing overview start animation because "
              << "we were in mode " << GetStateName(last_state_);
#endif
    // Configure the windows immediately to be over top of the active
    // window so that the scaling animation can take place.

    // The snapshot should cover the screen in its largest dimension.
    float snapshot_scale = std::min(
        layout_manager_->width() / win_->client_width(),
        layout_manager_->height() / win_->client_height());

    win_->ScaleComposited(snapshot_scale, snapshot_scale, 0);
    win_->SetCompositedOpacity(0.f, 0);

    // Start with the window at the bottom right, to match up with
    // content area of the corresponding toplevel window's web page,
    // since all of our UI chrome is at the top of a Chrome window.
    int start_x = layout_manager_->x() +
                  layout_manager_->width() -
                  (win_->composited_scale_x() * win_->client_width());
    int start_y = layout_manager_->y() +
                  layout_manager_->height() -
                  (win_->composited_scale_y() * win_->client_height());

    // If this window is to the right of the selected snapshot window, then
    // we want to start offscreen to the right.
    if (state_ == STATE_OVERVIEW_MODE_NORMAL &&
        layout_manager_->current_snapshot() &&
        CalculateOverallIndex() >
        layout_manager_->current_snapshot()->CalculateOverallIndex()) {
      start_x += layout_manager_->width();
    }

    win_->MoveComposited(start_x, start_y, 0);

    // Setup the animation of the scale and opacity.
    win_->SetCompositedOpacity(1.f, opacity_anim_ms);
  }

  SnapshotWindow* snapshot_to_stack_under =
      layout_manager_->GetSnapshotAfter(this);

#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Configuring snapshot " << win_->xid_str()
            << " for " << GetStateName(state_);
#endif
  if (snapshot_to_stack_under) {
    win_->StackCompositedBelow(
        snapshot_to_stack_under->win()->GetBottomActor(), NULL, false);
    wm()->xconn()->StackWindow(
        input_xid_, snapshot_to_stack_under->input_xid(), false);
  } else {
    // Even though this method stacks the shadow at the bottom of the
    // layer, it should be safe to do since we use GetBottomActor()
    // above to make sure that the other windows are stacked beneath
    // this window's shadow.
    wm()->stacking_manager()->StackWindowAtTopOfLayer(
        win_, StackingManager::LAYER_SNAPSHOT_WINDOW);
    wm()->stacking_manager()->StackXidAtTopOfLayer(
        input_xid_, StackingManager::LAYER_SNAPSHOT_WINDOW);
  }

  int absolute_overview_x = layout_manager_->x() +
                            layout_manager_->overview_panning_offset() +
                            overview_x_;
  int absolute_overview_y = layout_manager_->y() + overview_y_;

  double new_tilt =
      state_ == STATE_OVERVIEW_MODE_NORMAL ? kUnselectedTilt : 0.0;

  int input_width = Compositor::Actor::GetTiltedWidth(overview_width_,
                                                      new_tilt);
  wm()->ConfigureInputWindow(input_xid_,
                             absolute_overview_x,
                             absolute_overview_y,
                             input_width,
                             overview_height_);

  win_->actor()->ShowDimmed(state_ == STATE_OVERVIEW_MODE_NORMAL, anim_ms);
  win_->actor()->SetTilt(new_tilt, anim_ms);
  win_->ScaleComposited(overview_scale_, overview_scale_, anim_ms);
  win_->MoveComposited(absolute_overview_x, absolute_overview_y, anim_ms);
}

void LayoutManager::SnapshotWindow::SetSize(int max_width, int max_height) {
  const int client_width = win_->client_width();
  const int client_height = win_->client_height();
  if (static_cast<double>(client_width) / client_height >
      static_cast<double>(max_width) / max_height) {
    overview_scale_ = static_cast<double>(max_width) / client_width;
    overview_width_ = max_width;
    overview_height_ = client_height * overview_scale_ + 0.5;
  } else {
    overview_scale_ = static_cast<double>(max_height) / client_height;
    overview_width_ = client_width * overview_scale_ + 0.5;
    overview_height_ = max_height;
  }
}

void LayoutManager::SnapshotWindow::HandleButtonRelease(XTime timestamp,
                                                        int x, int y) {
  if (layout_manager_->current_snapshot() == this) {
    // If we're already the current snapshot, then switch modes to ACTIVE mode.
    layout_manager_->SetMode(LayoutManager::MODE_ACTIVE);
  } else {
    layout_manager_->SetCurrentSnapshotWithClick(this, timestamp, x, y);
    layout_manager_->LayoutWindows(true);
  }
}

}  // namespace window_manager
