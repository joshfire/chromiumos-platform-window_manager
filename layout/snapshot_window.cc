// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/layout/snapshot_window.h"

#include <algorithm>
#include <cmath>
#include <tr1/memory>

#include <gflags/gflags.h>

#include "base/logging.h"
#include "base/string_util.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"
#include "window_manager/callback.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/layout/toplevel_window.h"
#include "window_manager/motion_event_coalescer.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x11/x_connection.h"

// Define this to get copius output from this file.
#if !defined(EXTRA_LOGGING)
#undef EXTRA_LOGGING
#endif

namespace window_manager {

using std::string;
using window_manager::util::XidStr;

const float LayoutManager::SnapshotWindow::kUnselectedTilt = 0.8;
const int LayoutManager::SnapshotWindow::kFavIconPadding = 5;
const int LayoutManager::SnapshotWindow::kTitlePadding = 8;

// If the difference between the scale of the snapshot and 1.0 is
// below this threshold, then it will be considered to be 1.0.
static const float kMinScaleThreshold = 0.01;


LayoutManager::SnapshotWindow::SnapshotWindow(Window* win,
                                              LayoutManager* layout_manager)
    : win_(win),
      layout_manager_(layout_manager),
      tab_index_(-1),
      toplevel_(NULL),
      toplevel_xid_(0),
      title_(NULL),
      fav_icon_(NULL),
      input_xid_(wm()->CreateInputWindow(Rect(-1, -1, 1, 1),
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
  win_->MoveComposited(layout_manager_->width(), layout_manager_->height(), 0);
  win_->SetCompositedOpacity(1.0, 0);

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
  win_->HideComposited();
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

void LayoutManager::SnapshotWindow::AddDecoration(Window* decoration) {
  if (!decoration)
    return;

  DLOG(INFO) << "Adding decoration " << decoration->xid_str() << " of type "
             << decoration->type_str() << " on snapshot " << win_->xid_str();

  decoration->SetCompositedOpacity(0.0, 0);
  decoration->ShowComposited();

  // Move the client offscreen -- it doesn't need to receive any
  // input.
  decoration->MoveClientOffscreen();

  switch (decoration->type()) {
    case chromeos::WM_IPC_WINDOW_CHROME_TAB_FAV_ICON:
      fav_icon_ = decoration;
      break;
    case chromeos::WM_IPC_WINDOW_CHROME_TAB_TITLE:
      title_ = decoration;
      break;
    default:
      NOTREACHED();
      break;
  }
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
#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Configuring snapshot " << win_->xid_str()
            << " for " << GetStateName(state_);
#endif

  if (last_state_ == STATE_OVERVIEW_MODE_SELECTED) {
    // The selected snapshot should be stretched to cover the layout
    // manager region.
    float snapshot_scale_x = static_cast<float>(layout_manager_->width()) /
                             win_->client_width();
    float snapshot_scale_y = static_cast<float>(layout_manager_->height()) /
                             win_->client_height();
    win_->ScaleComposited(snapshot_scale_x, snapshot_scale_y, anim_ms);
    win_->actor()->ShowDimmed(false, anim_ms);
    win_->actor()->SetTilt(0.0, anim_ms);
    win_->MoveComposited(layout_manager_->x(), layout_manager_->y(), anim_ms);
    if (fav_icon_) {
      fav_icon_->SetCompositedOpacity(1.0f, 0);
      fav_icon_->MoveComposited(layout_manager_->x(),
                                kTitlePadding + layout_manager_->y() +
                                win_->client_height() * snapshot_scale_y,
                                anim_ms);
    }
    if (title_) {
      title_->SetCompositedOpacity(1.0f, 0);
      int x_position = layout_manager_->x();
      if (fav_icon_) {
        x_position += fav_icon_->composited_width() +
                      kFavIconPadding;
      }
      title_->MoveComposited(x_position,
                             kTitlePadding + layout_manager_->y() +
                             win_->client_height() * snapshot_scale_y, anim_ms);

    }
  }


  // TODO: Maybe just unmap input windows.
  wm()->xconn()->ConfigureWindowOffscreen(input_xid_);
}

void LayoutManager::SnapshotWindow::ConfigureForOverviewMode(bool animate) {
  if (state_ == STATE_ACTIVE_MODE_INVISIBLE)
    return;

  bool switched_to_overview = last_state_ != STATE_OVERVIEW_MODE_SELECTED &&
                              last_state_ != STATE_OVERVIEW_MODE_NORMAL;

  // Don't animate anything when this isn't the selected snapshot.
  if (switched_to_overview && state_ != STATE_OVERVIEW_MODE_SELECTED)
    animate = false;

  const int anim_ms = animate ? LayoutManager::kWindowAnimMs : 0;
  const int opacity_anim_ms = animate ? LayoutManager::kWindowOpacityAnimMs : 0;

  if (switched_to_overview) {
#if defined(EXTRA_LOGGING)
    DLOG(INFO) << "Performing overview start animation because "
               << "we were in mode " << GetStateName(last_state_);
#endif
    // Configure the windows immediately to be over top of the active
    // window so that the scaling animation can take place.

    // The snapshot should cover the screen.
    float snapshot_scale_x = static_cast<float>(layout_manager_->width()) /
                             win_->client_width();
    float snapshot_scale_y = static_cast<float>(layout_manager_->height()) /
                             win_->client_height();

    win_->ScaleComposited(snapshot_scale_x, snapshot_scale_y, 0);
    win_->MoveComposited(layout_manager_->x(), layout_manager_->y(), 0);
    if (state_ == STATE_OVERVIEW_MODE_SELECTED) {
      if (fav_icon_) {
        fav_icon_->SetCompositedOpacity(1.0f, 0);
        fav_icon_->MoveComposited(layout_manager_->x(),
                                  kTitlePadding + layout_manager_->y() +
                                  win_->client_height() * snapshot_scale_y, 0);
      }

      if (title_) {
        title_->SetCompositedOpacity(1.0f, 0);
        int x_position = layout_manager_->x();
        if (fav_icon_) {
          x_position += fav_icon_->composited_width() +
                        kFavIconPadding;
        }
        title_->MoveComposited(x_position,
                               kTitlePadding + layout_manager_->y() +
                               win_->client_height() * snapshot_scale_y, 0);
      }
    }
  }

  SnapshotWindow* snapshot_to_stack_under =
      layout_manager_->GetSnapshotAfter(this);

#if defined(EXTRA_LOGGING)
  DLOG(INFO) << "Configuring snapshot " << win_->xid_str()
            << " for " << GetStateName(state_);
#endif
  if (!snapshot_to_stack_under ||
      (state_ == STATE_OVERVIEW_MODE_SELECTED && switched_to_overview)) {
    // We want to make sure that the currently selected window is
    // stacked on top during the mode-switching animation, but stacked
    // regularly otherwise.
    wm()->stacking_manager()->StackWindowAtTopOfLayer(
        win_,
        StackingManager::LAYER_SNAPSHOT_WINDOW,
        StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
    wm()->stacking_manager()->StackXidAtTopOfLayer(
        input_xid_, StackingManager::LAYER_SNAPSHOT_WINDOW);
  } else {
    wm()->stacking_manager()->StackWindowRelativeToOtherWindow(
        win_,
        snapshot_to_stack_under->win(),
        StackingManager::BELOW_SIBLING,
        StackingManager::SHADOW_AT_BOTTOM_OF_LAYER,
        StackingManager::LAYER_SNAPSHOT_WINDOW);
    wm()->xconn()->StackWindow(
        input_xid_, snapshot_to_stack_under->input_xid(), false);
  }

  int absolute_overview_x = layout_manager_->x() +
                            layout_manager_->overview_panning_offset() +
                            overview_x_;
  int absolute_overview_y = layout_manager_->y() + overview_y_;

  double new_tilt =
      state_ == STATE_OVERVIEW_MODE_NORMAL ? kUnselectedTilt : 0.0;

  int input_width = Compositor::Actor::GetTiltedWidth(overview_width_,
                                                      new_tilt);

  win_->actor()->ShowDimmed(state_ == STATE_OVERVIEW_MODE_NORMAL, anim_ms);
  win_->actor()->SetTilt(new_tilt, anim_ms);
  win_->ScaleComposited(overview_scale_, overview_scale_, anim_ms);
  win_->MoveComposited(absolute_overview_x, absolute_overview_y, anim_ms);

  int title_y = kTitlePadding + absolute_overview_y +
                win_->client_height() * overview_scale_;
  if (fav_icon_) {
    fav_icon_->SetCompositedOpacity(1.0f, opacity_anim_ms);
    fav_icon_->MoveComposited(absolute_overview_x, title_y, anim_ms);
  }

  int overview_height_with_title = overview_height_;
  if (title_) {
    if (state_ == STATE_OVERVIEW_MODE_SELECTED)
      title_->SetCompositedOpacity(1.0f, opacity_anim_ms);
    else
      title_->SetCompositedOpacity(0.0f, opacity_anim_ms);

    int x_position = absolute_overview_x;
    if (fav_icon_)
      x_position += fav_icon_->composited_width() + kFavIconPadding;

    title_->MoveComposited(x_position, title_y, anim_ms);
    overview_height_with_title += kTitlePadding +
                                  title_->client_height() * overview_scale_;
  }

  wm()->ConfigureInputWindow(input_xid_,
                             Rect(absolute_overview_x,
                                  absolute_overview_y,
                                  input_width,
                                  overview_height_with_title));
}

void LayoutManager::SnapshotWindow::SetSize(int max_width, int max_height) {
  const int client_width = win_->client_width();
  const int client_height = win_->client_height();
  if (static_cast<double>(client_width) / client_height >
      static_cast<double>(max_width) / max_height) {
    overview_scale_ = static_cast<double>(max_width) / client_width;
    // If we're really close to 1.0, then just snap to it to keep the
    // image from blurring.
    if (fabs(1.0 - overview_scale_) < kMinScaleThreshold)
      overview_scale_ = 1.0;
    overview_width_ = max_width;
    overview_height_ = client_height * overview_scale_ + 0.5;
  } else {
    overview_scale_ = static_cast<double>(max_height) / client_height;
    // If we're really close to 1.0, then just snap to it to keep the
    // image from blurring.
    if (fabs(1.0 - overview_scale_) < kMinScaleThreshold)
      overview_scale_ = 1.0;
    overview_width_ = client_width * overview_scale_ + 0.5;
    overview_height_ = max_height;
  }
#ifdef EXTRA_LOGGING
  DLOG(INFO) << "Setting snapshot scale to " << overview_scale_
             << " max: " << max_width << "x" << max_height
             << " client: " << client_width << "x" << client_height;
#endif
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
