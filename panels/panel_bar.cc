// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/panels/panel_bar.h"

#include <algorithm>

#include <gflags/gflags.h>

#include "base/logging.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/event_loop.h"
#include "window_manager/panels/panel.h"
#include "window_manager/panels/panel_manager.h"
#include "window_manager/pointer_position_watcher.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x11/x_connection.h"

DEFINE_string(panel_anchor_image, "../assets/images/panel_anchor.png",
              "Image to use for anchors on the panel bar");

DEFINE_bool(allow_panels_to_be_detached, false,
            "Should panels be detachable from the panel bar?");

using std::find;
using std::make_pair;
using std::max;
using std::min;
using std::tr1::shared_ptr;
using std::vector;
using window_manager::util::FindWithDefault;
using window_manager::util::ReorderIterator;
using window_manager::util::XidStr;

namespace window_manager {

const int PanelBar::kRightPaddingPixels = 24;
const int PanelBar::kPixelsBetweenPanels = 6;
const int PanelBar::kShowCollapsedPanelsDistancePixels = 1;
const int PanelBar::kHideCollapsedPanelsDistancePixels = 30;
const int PanelBar::kHiddenCollapsedPanelHeightPixels = 3;
const int PanelBar::kFloatingPanelThresholdPixels = 30;

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

// Amount of time to take when hiding or unhiding collapsed panels.
static const int kHideCollapsedPanelsAnimMs = 100;

// How long should we wait before showing collapsed panels when the user
// moves the pointer down to the bottom row of pixels?
static const int kShowCollapsedPanelsDelayMs = 200;

PanelBar::PanelBar(PanelManager* panel_manager)
    : panel_manager_(panel_manager),
      packed_panel_width_(0),
      dragged_panel_(NULL),
      dragging_panel_horizontally_(false),
      anchor_input_xid_(wm()->CreateInputWindow(-1, -1, 1, 1, ButtonPressMask)),
      anchor_panel_(NULL),
      anchor_actor_(
          wm()->compositor()->CreateImageFromFile(FLAGS_panel_anchor_image)),
      desired_panel_to_focus_(NULL),
      collapsed_panel_state_(COLLAPSED_PANEL_STATE_HIDDEN),
      show_collapsed_panels_input_xid_(
          wm()->CreateInputWindow(-1, -1, 1, 1,
                                  EnterWindowMask | LeaveWindowMask)),
      show_collapsed_panels_timeout_id_(-1),
      event_consumer_registrar_(
          new EventConsumerRegistrar(wm(), panel_manager)) {
  event_consumer_registrar_->RegisterForWindowEvents(anchor_input_xid_);
  event_consumer_registrar_->RegisterForWindowEvents(
      show_collapsed_panels_input_xid_);

  anchor_actor_->SetName("panel anchor");
  anchor_actor_->SetOpacity(0, 0);
  wm()->stage()->AddActor(anchor_actor_.get());
  wm()->stacking_manager()->StackActorAtTopOfLayer(
      anchor_actor_.get(), StackingManager::LAYER_PANEL_BAR_INPUT_WINDOW);

  // Stack the anchor input window above the show-collapsed-panels one so
  // we won't get spurious leave events in the former.
  wm()->stacking_manager()->StackXidAtTopOfLayer(
      show_collapsed_panels_input_xid_,
      StackingManager::LAYER_PANEL_BAR_INPUT_WINDOW);
  wm()->stacking_manager()->StackXidAtTopOfLayer(
      anchor_input_xid_, StackingManager::LAYER_PANEL_BAR_INPUT_WINDOW);

  wm()->SetNamePropertiesForXid(anchor_input_xid_, "panel anchor input window");
  wm()->SetNamePropertiesForXid(
      show_collapsed_panels_input_xid_, "show-collapsed-panels input window");
}

PanelBar::~PanelBar() {
  DisableShowCollapsedPanelsTimeout();
  wm()->xconn()->DestroyWindow(anchor_input_xid_);
  anchor_input_xid_ = None;
  wm()->xconn()->DestroyWindow(show_collapsed_panels_input_xid_);
  show_collapsed_panels_input_xid_ = None;
}

WindowManager* PanelBar::wm() {
  return panel_manager_->wm();
}

void PanelBar::GetInputWindows(vector<XWindow>* windows_out) {
  CHECK(windows_out);
  windows_out->clear();
  windows_out->push_back(anchor_input_xid_);
  windows_out->push_back(show_collapsed_panels_input_xid_);
}

void PanelBar::AddPanel(Panel* panel, PanelSource source) {
  DCHECK(panel);
  CHECK(all_panels_.insert(panel).second)
      << "Tried to add already-present panel " << panel->xid_str();

  shared_ptr<PanelInfo> info(new PanelInfo);
  int padding =
      packed_panels_.empty() ? kRightPaddingPixels : kPixelsBetweenPanels;
  info->desired_right = wm()->width() - packed_panel_width_ - padding;
  info->is_floating = false;
  CHECK(panel_infos_.insert(make_pair(panel, info)).second);

  // Decide where we want to insert the panel.  If Chrome requested that
  // the panel be opened to the left of its creator, we insert it in the
  // correct spot in |packed_panels_| and place it to the left of its
  // creator's fixed position.
  PanelVector::iterator insert_it = packed_panels_.begin();
  if (source == PANEL_SOURCE_NEW &&
      panel->content_win()->type_params().size() >= 4 &&
      panel->content_win()->type_params()[3]) {
    XWindow creator_xid = panel->content_win()->type_params()[3];
    Window* creator_win = wm()->GetWindow(creator_xid);
    if (creator_win) {
      PanelVector::iterator it =
          FindPanelInVectorByWindow(packed_panels_, *creator_win);
      if (it == packed_panels_.end()) {
        LOG(WARNING) << "Unable to find creator panel " << XidStr(creator_xid)
                     << " for new panel " << panel->xid_str();
      } else {
        padding = kPixelsBetweenPanels;
        info->desired_right = GetPanelInfoOrDie(*it)->desired_right -
                              (*it)->width() - padding;
        insert_it = it;
      }
    }
  }

  packed_panels_.insert(insert_it, panel);
  packed_panel_width_ += panel->width() + padding;

  // If the panel is being dragged, move it to the correct position within
  // |packed_panels_|.
  if (source == PANEL_SOURCE_DRAGGED) {
    DCHECK(!dragged_panel_);
    dragged_panel_ = panel;
    dragging_panel_horizontally_ = true;
    ReorderPanelInVector(panel, &packed_panels_);
  }

  panel->StackAtTopOfLayer(source == PANEL_SOURCE_DRAGGED ?
                           StackingManager::LAYER_DRAGGED_PANEL :
                           StackingManager::LAYER_PACKED_PANEL_IN_BAR);

  const int final_y = ComputePanelY(*panel);

  // Now move the panel to its final position.
  switch (source) {
    case PANEL_SOURCE_NEW:
      // Make newly-created panels slide in from the bottom of the screen.
      panel->Move(info->desired_right, wm()->height(), false, 0);
      panel->MoveY(final_y, true, kPanelStateAnimMs);
      break;
    case PANEL_SOURCE_DRAGGED:
      panel->MoveY(final_y, true, 0);
      break;
    case PANEL_SOURCE_DROPPED:
      panel->Move(info->desired_right, final_y, true, kDroppedPanelAnimMs);
      break;
    default:
      NOTREACHED() << "Unknown panel source " << source;
  }

  ArrangePanels(true, NULL);
  panel->SetResizable(panel->is_expanded());

  // If this is a new panel and it requested the focus, or it was already
  // focused (e.g. it was focused when it got detached, and now it's being
  // reattached), or there's just no other focused window, call
  // FocusPanel() to focus it if needed and update
  // |desired_panel_to_focus_|.
  const bool focus_requested =
      source == PANEL_SOURCE_NEW &&
      (panel->content_win()->type_params().size() < 3 ||
       panel->content_win()->type_params()[2]);
  if (!wm()->IsModalWindowFocused() &&
      panel->is_expanded() &&
      (focus_requested ||
       panel->IsFocused() ||
       !wm()->focus_manager()->focused_win())) {
    FocusPanel(panel, wm()->GetCurrentTimeFromServer());
  }

  // If this is the only collapsed panel, we need to configure the input
  // window to watch for the pointer moving to the bottom of the screen.
  if (!panel->is_expanded() && GetNumCollapsedPanels() == 1)
    ConfigureShowCollapsedPanelsInputWindow(true);
}

void PanelBar::RemovePanel(Panel* panel) {
  DCHECK(panel);
  CHECK(all_panels_.erase(panel) == static_cast<size_t>(1))
      << "Tried to remove nonexistent panel " << panel->xid_str();

  if (anchor_panel_ == panel)
    DestroyAnchor();
  if (dragged_panel_ == panel)
    dragged_panel_ = NULL;
  // If this was a focused content window, then let's try to find a nearby
  // panel to focus if we get asked to do so later.
  if (desired_panel_to_focus_ == panel)
    desired_panel_to_focus_ = GetNearestExpandedPanel(panel);

  bool was_collapsed = !panel->is_expanded();
  CHECK(panel_infos_.erase(panel) == 1);
  PanelVector::iterator it =
      FindPanelInVectorByWindow(packed_panels_, *(panel->content_win()));
  if (it != packed_panels_.end()) {
    packed_panels_.erase(it);
  } else {
    it = FindPanelInVectorByWindow(floating_panels_, *(panel->content_win()));
    if (it != floating_panels_.end()) {
      floating_panels_.erase(it);
    } else {
      LOG(WARNING) << "Got request to remove panel " << panel->xid_str()
                   << " but didn't find it";
      return;
    }
  }

  // This also recomputes the total width.
  ArrangePanels(true, NULL);

  if (dragged_panel_ && !(GetPanelInfoOrDie(dragged_panel_)->is_floating))
    if (ReorderPanelInVector(dragged_panel_, &packed_panels_))
      ArrangePanels(false, NULL);

  // If this was the last collapsed panel, move the input window offscreen.
  if (was_collapsed && GetNumCollapsedPanels() == 0)
    ConfigureShowCollapsedPanelsInputWindow(false);
}

bool PanelBar::ShouldAddDraggedPanel(const Panel* panel,
                                     int drag_x,
                                     int drag_y) {
  return drag_y + panel->total_height() >
         wm()->height() - kPanelAttachThresholdPixels;
}

void PanelBar::HandleInputWindowButtonPress(XWindow xid,
                                            int x, int y,
                                            int x_root, int y_root,
                                            int button,
                                            XTime timestamp) {
  if (wm()->IsModalWindowFocused())
    return;

  CHECK(xid == anchor_input_xid_);
  if (button != 1)
    return;

  // Destroy the anchor and collapse the corresponding panel.
  DLOG(INFO) << "Got button press in anchor window";
  Panel* panel = anchor_panel_;
  DestroyAnchor();
  if (panel)
    CollapsePanel(panel, kPanelStateAnimMs);
  else
    LOG(WARNING) << "Anchor panel no longer exists";
}

void PanelBar::HandleInputWindowPointerEnter(XWindow xid,
                                             int x, int y,
                                             int x_root, int y_root,
                                             XTime timestamp) {
  if (xid == show_collapsed_panels_input_xid_) {
    DLOG(INFO) << "Got mouse enter in show-collapsed-panels window";
    if (x_root >= wm()->width() - packed_panel_width_) {
      // If the user moves the pointer down quickly to the bottom of the
      // screen, it's possible that it could end up below a collapsed panel
      // without us having received an enter event in the panel's titlebar.
      // Show the panels immediately in this case.
      ShowCollapsedPanels();
    } else {
      // Otherwise, set up a timeout to show the panels if we're not already
      // doing so.
      if (collapsed_panel_state_ != COLLAPSED_PANEL_STATE_SHOWN &&
          collapsed_panel_state_ != COLLAPSED_PANEL_STATE_WAITING_TO_SHOW) {
        collapsed_panel_state_ = COLLAPSED_PANEL_STATE_WAITING_TO_SHOW;
        DCHECK_EQ(show_collapsed_panels_timeout_id_, -1);
        show_collapsed_panels_timeout_id_ =
            wm()->event_loop()->AddTimeout(
                NewPermanentCallback(
                    this, &PanelBar::HandleShowCollapsedPanelsTimeout),
                kShowCollapsedPanelsDelayMs, 0);
      }
    }
  }
}

void PanelBar::HandleInputWindowPointerLeave(XWindow xid,
                                             int x, int y,
                                             int x_root, int y_root,
                                             XTime timestamp) {
  if (xid == show_collapsed_panels_input_xid_) {
    DLOG(INFO) << "Got mouse leave in show-collapsed-panels window";
    if (collapsed_panel_state_ == COLLAPSED_PANEL_STATE_WAITING_TO_SHOW) {
      collapsed_panel_state_ = COLLAPSED_PANEL_STATE_HIDDEN;
      DisableShowCollapsedPanelsTimeout();
    }
  }
}

void PanelBar::HandlePanelButtonPress(
    Panel* panel, int button, XTime timestamp) {
  if (wm()->IsModalWindowFocused())
    return;
  DCHECK(panel);
  DLOG(INFO) << "Got button press in panel " << panel->xid_str()
             << "; giving it the focus";
  // Get rid of the passive button grab, and then ungrab the pointer
  // and replay events so the panel will get a copy of the click.
  FocusPanel(panel, timestamp);
}

void PanelBar::HandlePanelTitlebarPointerEnter(Panel* panel, XTime timestamp) {
  DCHECK(panel);
  DLOG(INFO) << "Got pointer enter in panel " << panel->xid_str()
             << "'s titlebar";
  if (collapsed_panel_state_ != COLLAPSED_PANEL_STATE_SHOWN &&
      !panel->is_expanded()) {
    ShowCollapsedPanels();
  }
}

void PanelBar::HandleSetPanelStateMessage(Panel* panel, bool expand) {
  DCHECK(panel);
  if (expand)
    ExpandPanel(panel, true, kPanelStateAnimMs);
  else
    CollapsePanel(panel, kPanelStateAnimMs);
}

bool PanelBar::HandleNotifyPanelDraggedMessage(Panel* panel,
                                               int drag_x,
                                               int drag_y) {
  DCHECK(panel);
  DLOG(INFO) << "Notified about drag of panel " << panel->xid_str()
             << " to (" << drag_x << ", " << drag_y << ")";

  if (FLAGS_allow_panels_to_be_detached) {
    const int y_threshold =
        wm()->height() - panel->total_height() - kPanelDetachThresholdPixels;
    if (drag_y <= y_threshold)
      return false;
  }

  if (dragged_panel_ != panel) {
    if (dragged_panel_) {
      LOG(WARNING) << "Abandoning dragged panel " << dragged_panel_->xid_str()
                   << " in favor of " << panel->xid_str();
      HandlePanelDragComplete(dragged_panel_);
    }

    DLOG(INFO) << "Starting drag of panel " << panel->xid_str();
    dragged_panel_ = panel;
    dragging_panel_horizontally_ =
        (abs(drag_x - panel->right()) > abs(drag_y - panel->titlebar_y()));
    panel->StackAtTopOfLayer(StackingManager::LAYER_DRAGGED_PANEL);
  }

  if (dragging_panel_horizontally_) {
    panel->MoveX(drag_x, false, 0);
    PanelInfo* info = GetPanelInfoOrDie(panel);

    // Make sure that the panel is in the correct vector (floating vs.
    // packed) for its current position.

    // We want to find the total width of all packed panels (except the
    // dragged panel, if it's packed), plus the padding that would go to
    // the right of the dragged panel (which differs depending on whether
    // there are other packed panels or not).
    int packed_width_with_padding = packed_panel_width_;
    if (!info->is_floating) {
      packed_width_with_padding -= panel->width();
    } else {
      packed_width_with_padding +=
          packed_panels_.empty() ? kRightPaddingPixels : kPixelsBetweenPanels;
    }

    const int floating_threshold =
        wm()->width() - packed_width_with_padding -
        kFloatingPanelThresholdPixels;

    bool moved_to_other_vector = false;
    if (drag_x < floating_threshold) {
      moved_to_other_vector = MovePanelToFloatingVector(panel, info);
      info->desired_right = drag_x;
      ArrangePanels(false, NULL);
    } else {
      moved_to_other_vector = MovePanelToPackedVector(panel, info);
      ArrangePanels(false, NULL);
    }

    if (!moved_to_other_vector) {
      // If we didn't move the panel to the other vector, then just make
      // sure that it's in the correct position within its current vector.
      PanelVector* panel_vector =
          info->is_floating ? &floating_panels_ : &packed_panels_;
      if (ReorderPanelInVector(panel, panel_vector) && !info->is_floating)
        ArrangePanels(false, NULL);
    }

  } else {
    // If we're dragging vertically, cap the Y value between the lowest and
    // highest positions that the panel can take while in the bar.
    const int capped_y =
        max(min(drag_y, wm()->height() - panel->titlebar_height()),
            wm()->height() - panel->total_height());
    panel->MoveY(capped_y, false, 0);
  }
  return true;
}

void PanelBar::HandleNotifyPanelDragCompleteMessage(Panel* panel) {
  DCHECK(panel);
  HandlePanelDragComplete(panel);
}

void PanelBar::HandleFocusPanelMessage(Panel* panel, XTime timestamp) {
  DCHECK(panel);
  if (!panel->is_expanded())
    ExpandPanel(panel, false, kPanelStateAnimMs);
  FocusPanel(panel, timestamp);
}

void PanelBar::HandlePanelResizeRequest(Panel* panel,
                                        int req_width, int req_height) {
  DCHECK(panel);
  panel->ResizeContent(req_width, req_height, GRAVITY_SOUTHEAST, true);
  ArrangePanels(true, NULL);
}

void PanelBar::HandlePanelResizeByUser(Panel* panel) {
  DCHECK(panel);
  Panel* fixed_floating_panel = NULL;
  PanelInfo* info = GetPanelInfoOrDie(panel);
  if (info->is_floating) {
    info->desired_right = panel->right();
    fixed_floating_panel = panel;
  }
  ArrangePanels(true, fixed_floating_panel);
}

void PanelBar::HandleScreenResize() {
  // Make all of the panels jump to their new Y positions first and then
  // repack them to animate them sliding to their new X positions.
  for (PanelSet::iterator it = all_panels_.begin();
       it != all_panels_.end(); ++it) {
    (*it)->MoveY(ComputePanelY(**it), true, 0);
  }
  if (dragged_panel_ && !(GetPanelInfoOrDie(dragged_panel_)->is_floating))
    ReorderPanelInVector(dragged_panel_, &packed_panels_);
  ArrangePanels(true, NULL);
}

void PanelBar::HandlePanelUrgencyChange(Panel* panel) {
  DCHECK(panel);
  if (!panel->is_expanded()) {
    const int computed_y = ComputePanelY(*panel);
    if (panel->titlebar_y() != computed_y)
      panel->MoveY(computed_y, true, kHideCollapsedPanelsAnimMs);
  }
}

bool PanelBar::TakeFocus(XTime timestamp) {
  // If we already decided on a panel to focus, use it.
  if (desired_panel_to_focus_) {
    FocusPanel(desired_panel_to_focus_, timestamp);
    return true;
  }

  // Just focus the first onscreen, expanded panel.
  for (PanelVector::iterator it = floating_panels_.begin();
       it != floating_panels_.end(); ++it) {
    if ((*it)->is_expanded() && (*it)->right() > 0) {
      FocusPanel(*it, timestamp);
      return true;
    }
  }
  for (PanelVector::iterator it = packed_panels_.begin();
       it != packed_panels_.end(); ++it) {
    if ((*it)->is_expanded() && (*it)->right() > 0) {
      FocusPanel(*it, timestamp);
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

int PanelBar::GetNumCollapsedPanels() {
  int count = 0;
  for (PanelSet::const_iterator it = all_panels_.begin();
       it != all_panels_.end(); ++it) {
    if (!(*it)->is_expanded())
      count++;
  }
  return count;
}

int PanelBar::ComputePanelY(const Panel& panel) {
  if (panel.is_expanded()) {
    return wm()->height() - panel.total_height();
  } else {
    if (CollapsedPanelsAreHidden() && !panel.is_urgent())
      return wm()->height() - kHiddenCollapsedPanelHeightPixels;
    else
      return wm()->height() - panel.titlebar_height();
  }
}

bool PanelBar::MovePanelToPackedVector(Panel* panel, PanelInfo* info) {
  DCHECK(panel);
  DCHECK(info);
  if (!info->is_floating)
    return false;

  DLOG(INFO) << "Moving panel " << panel->xid_str() << " to packed vector";
  PanelVector::iterator it =
      FindPanelInVectorByWindow(floating_panels_, *(panel->content_win()));
  DCHECK(it != floating_panels_.end());
  floating_panels_.erase(it);
  // Add the panel to the beginning of the vector.  If it's getting dragged
  // from the floating vector at the left edge of the screen, it's likely
  // to end up at the left edge of the packed vector at the right edge of
  // the screen.
  packed_panels_.insert(packed_panels_.begin(), panel);
  info->is_floating = false;
  ReorderPanelInVector(panel, &packed_panels_);
  return true;
}

bool PanelBar::MovePanelToFloatingVector(Panel* panel, PanelInfo* info) {
  DCHECK(panel);
  DCHECK(info);
  if (info->is_floating)
    return false;

  DLOG(INFO) << "Moving panel " << panel->xid_str() << " to floating vector";
  PanelVector::iterator it =
      FindPanelInVectorByWindow(packed_panels_, *(panel->content_win()));
  DCHECK(it != packed_panels_.end());
  packed_panels_.erase(it);
  // See MovePanelToPackedVector()'s comment.
  floating_panels_.push_back(panel);
  info->is_floating = true;
  ReorderPanelInVector(panel, &floating_panels_);
  return true;
}

void PanelBar::ExpandPanel(Panel* panel, bool create_anchor, int anim_ms) {
  CHECK(panel);
  if (panel->is_expanded()) {
    LOG(WARNING) << "Ignoring request to expand already-expanded panel "
                 << panel->xid_str();
    return;
  }

  panel->SetExpandedState(true);
  panel->MoveY(ComputePanelY(*panel), true, anim_ms);
  panel->SetResizable(true);
  if (create_anchor)
    CreateAnchor(panel);

  if (GetNumCollapsedPanels() == 0)
    ConfigureShowCollapsedPanelsInputWindow(false);
}

void PanelBar::CollapsePanel(Panel* panel, int anim_ms) {
  CHECK(panel);
  if (!panel->is_expanded()) {
    LOG(WARNING) << "Ignoring request to collapse already-collapsed panel "
                 << panel->xid_str();
    return;
  }

  // In case we need to focus another panel, find the nearest one before we
  // collapse this one.
  Panel* panel_to_focus = GetNearestExpandedPanel(panel);

  if (anchor_panel_ == panel)
    DestroyAnchor();

  panel->SetExpandedState(false);
  panel->MoveY(ComputePanelY(*panel), true, anim_ms);
  panel->SetResizable(false);

  // Give up the focus if this panel had it.
  if (panel->IsFocused()) {
    desired_panel_to_focus_ = panel_to_focus;
    XTime timestamp = wm()->GetCurrentTimeFromServer();
    if (!TakeFocus(timestamp))
      wm()->TakeFocus(timestamp);
  }

  if (GetNumCollapsedPanels() == 1)
    ConfigureShowCollapsedPanelsInputWindow(true);
}

void PanelBar::FocusPanel(Panel* panel, XTime timestamp) {
  CHECK(panel);
  panel->TakeFocus(timestamp);
  desired_panel_to_focus_ = panel;
}

Panel* PanelBar::GetPanelByWindow(const Window& win) {
  for (PanelSet::const_iterator it = all_panels_.begin();
       it != all_panels_.end(); ++it) {
    if ((*it)->titlebar_win() == &win || (*it)->content_win() == &win)
      return (*it);
  }
  return NULL;
}

// static
PanelBar::PanelVector::iterator PanelBar::FindPanelInVectorByWindow(
    PanelVector& panels, const Window& win) {
  for (PanelVector::iterator it = panels.begin(); it != panels.end(); ++it)
    if ((*it)->titlebar_win() == &win || (*it)->content_win() == &win)
      return it;
  return panels.end();
}

void PanelBar::HandlePanelDragComplete(Panel* panel) {
  CHECK(panel);
  DLOG(INFO) << "Got notification that panel drag is complete for "
             << panel->xid_str();
  if (dragged_panel_ != panel)
    return;

  PanelInfo* info = GetPanelInfoOrDie(panel);
  dragged_panel_ = NULL;

  if (dragging_panel_horizontally_) {
    ArrangePanels(true, info->is_floating ? panel : NULL);
  } else {
    // Move the panel back to the correct Y position, expanding or collapsing
    // it if needed.
    const bool mostly_visible =
        panel->titlebar_y() < wm()->height() - 0.5 * panel->total_height();
    // Cut the regular expanding/collapsing animation time in half; we're
    // already at least halfway to the final position.
    const int anim_ms = 0.5 * kPanelStateAnimMs;
    if (mostly_visible && !panel->is_expanded()) {
      ExpandPanel(panel, false, anim_ms);
      FocusPanel(panel, wm()->GetCurrentTimeFromServer());
    } else if (!mostly_visible && panel->is_expanded()) {
      CollapsePanel(panel, anim_ms);
    } else {
      panel->MoveY(ComputePanelY(*panel), true, anim_ms);
    }
  }

  panel->StackAtTopOfLayer(
      info->is_floating ?
      StackingManager::LAYER_FLOATING_PANEL_IN_BAR :
      StackingManager::LAYER_PACKED_PANEL_IN_BAR);

  if (collapsed_panel_state_ == COLLAPSED_PANEL_STATE_WAITING_TO_HIDE) {
    // If the user moved the pointer up from the bottom of the screen while
    // they were dragging the panel...
    Point pointer_pos;
    wm()->xconn()->QueryPointerPosition(&pointer_pos);
    if (pointer_pos.y < wm()->height() - kHideCollapsedPanelsDistancePixels) {
      // Hide the panels if they didn't move the pointer back down again
      // before releasing the button.
      HideCollapsedPanels();
    } else {
      // Otherwise, keep showing the panels and start watching the pointer
      // position again.
      collapsed_panel_state_ = COLLAPSED_PANEL_STATE_SHOWN;
      StartHideCollapsedPanelsWatcher();
    }
  }
}

// static
bool PanelBar::ReorderPanelInVector(Panel* panel_to_reorder,
                                    PanelVector* panels) {
  DCHECK(panel_to_reorder);
  DCHECK(panels);

  PanelVector::iterator src_it =
      find(panels->begin(), panels->end(), panel_to_reorder);
  DCHECK(src_it != panels->end());

  // Find the leftmost panel whose midpoint our left edge is to the left
  // of, and the rightmost panel whose midpoint our right edge is to the
  // right of.
  PanelVector::iterator min_it = panels->end() - 1;
  PanelVector::iterator max_it = panels->begin();
  for (PanelVector::iterator it = panels->begin(); it != panels->end(); ++it) {
    Panel* panel = *it;
    if (panel_to_reorder == panel)
      continue;
    if (panel_to_reorder->content_x() <= panel->content_center())
      min_it = min(min_it, it);
    if (panel_to_reorder->right() > panel->content_center())
      max_it = max(max_it, it);
  }

  // If we found a range where it seems reasonable to stick the panel, put
  // it as far right as we can.
  if (max_it >= min_it && max_it != src_it) {
    ReorderIterator(src_it, max_it);
    return true;
  }
  return false;
}

void PanelBar::ArrangePanels(bool arrange_floating,
                             Panel* fixed_floating_panel) {
  // Pack all of the packed panels to the right.
  packed_panel_width_ = 0;
  for (PanelVector::reverse_iterator it = packed_panels_.rbegin();
       it != packed_panels_.rend(); ++it) {
    // Calculate the padding needed to this panel's right.
    const int padding = (it == packed_panels_.rbegin()) ?
                        kRightPaddingPixels : kPixelsBetweenPanels;
    Panel* panel = *it;
    PanelInfo* info = GetPanelInfoOrDie(panel);

    info->desired_right = wm()->width() - packed_panel_width_ - padding;
    if (panel != dragged_panel_ &&
        (panel->right() != info->desired_right ||
         !panel->client_windows_have_correct_position())) {
      panel->MoveX(info->desired_right, true, kPanelArrangeAnimMs);
    }

    packed_panel_width_ += panel->width() + padding;
  }

  // Now make the floating panels not overlap using the space to the left
  // of the group of packed panels.
  if (arrange_floating) {
    int right_boundary = wm()->width() - packed_panel_width_ -
        (packed_panel_width_ == 0 ? kRightPaddingPixels : kPixelsBetweenPanels);

    if (fixed_floating_panel)
      ShiftFloatingPanelsAroundFixedPanel(fixed_floating_panel, right_boundary);

    for (PanelVector::reverse_iterator it = floating_panels_.rbegin();
         it != floating_panels_.rend(); ++it) {
      Panel* panel = *it;
      PanelInfo* info = GetPanelInfoOrDie(panel);

      if (panel != dragged_panel_) {
        const int panel_right = min(info->desired_right, right_boundary);
        if (panel->right() != panel_right ||
            !panel->client_windows_have_correct_position())
          panel->MoveX(panel_right, true, kPanelArrangeAnimMs);
      }
      right_boundary = panel->content_x() - kPixelsBetweenPanels;
    }
  }
}

void PanelBar::ShiftFloatingPanelsAroundFixedPanel(Panel* fixed_panel,
                                                   int right_boundary) {
  DCHECK(fixed_panel);

  // Make sure that the fixed panel is in the allowable area.
  if (fixed_panel->right() > right_boundary)
    fixed_panel->MoveX(right_boundary, true, kPanelArrangeAnimMs);

  PanelVector::iterator fixed_it = FindPanelInVectorByWindow(
      floating_panels_, *(fixed_panel->content_win()));
  DCHECK(fixed_it != floating_panels_.end());

  // Figure out the total amount of space that's available between the
  // right edge of the floating panel and the right boundary, and the
  // amount of space needed by the panels that are currently there.
  int space_to_right_of_fixed = right_boundary - fixed_panel->right();
  int panel_width_to_right_of_fixed = 0;
  for (PanelVector::iterator it = fixed_it + 1;
       it != floating_panels_.end(); ++it)
    panel_width_to_right_of_fixed += (*it)->width() + kPixelsBetweenPanels;

  // See how many panels we'll need to shift to the left of the fixed panel
  // to make them fit in the space, and then shift them (by reordering the
  // fixed panel in the vector).
  PanelVector::iterator new_fixed_it = fixed_it;
  for (PanelVector::iterator it = fixed_it + 1;
       it != floating_panels_.end(); ++it) {
    if (panel_width_to_right_of_fixed <= space_to_right_of_fixed)
      break;
    new_fixed_it = it;
    panel_width_to_right_of_fixed -=
        ((*it)->width() + kPixelsBetweenPanels);
  }

  // If we didn't need to shift any of the panels that were to our right,
  // and there are panels to our left that want to be to the right, move
  // them if we have space.
  if (new_fixed_it == fixed_it && fixed_it != floating_panels_.begin()) {
    for (PanelVector::iterator it = fixed_it - 1; ; it--) {
      Panel* panel = *it;
      PanelInfo* info = GetPanelInfoOrDie(panel);
      if (info->desired_right - 0.5 * panel->width() < fixed_panel->content_x())
        break;
      int new_width_to_right =
          panel_width_to_right_of_fixed + panel->width() + kPixelsBetweenPanels;
      if (new_width_to_right > space_to_right_of_fixed)
        break;
      new_fixed_it = it;
      panel_width_to_right_of_fixed = new_width_to_right;

      if (it == floating_panels_.begin())
        break;
    }
  }
  DCHECK_LE(panel_width_to_right_of_fixed, space_to_right_of_fixed);

  if (new_fixed_it != fixed_it)
    ReorderIterator(fixed_it, new_fixed_it);

  // Now make one more pass through all of the panels to the right, and
  // shift their desired positions to the right as needed so they won't
  // overlap.  (Note that it's possible that they'll extend beyond the
  // right boundary now if they weren't packed efficiently; ArrangePanels()
  // will take care of shifting them back to the left when it makes its
  // final pass.)
  int left_edge = fixed_panel->right() + kPixelsBetweenPanels;
  for (PanelVector::iterator it = new_fixed_it + 1;
       it != floating_panels_.end(); ++it) {
    Panel* panel = *it;
    PanelInfo* info = GetPanelInfoOrDie(panel);
    if (info->desired_right - panel->width() < left_edge)
      info->desired_right = left_edge + panel->width();
    left_edge = info->desired_right + kPixelsBetweenPanels;
  }
}

void PanelBar::CreateAnchor(Panel* panel) {
  Point pointer_pos;
  wm()->xconn()->QueryPointerPosition(&pointer_pos);

  const int width = anchor_actor_->GetWidth();
  const int height = anchor_actor_->GetHeight();
  const int x = min(max(static_cast<int>(pointer_pos.x - 0.5 * width), 0),
                    wm()->width() - width);
  const int y = wm()->height() - height;

  wm()->ConfigureInputWindow(anchor_input_xid_, x, y, width, height);
  anchor_panel_ = panel;
  anchor_actor_->Move(x, y, 0);
  anchor_actor_->SetOpacity(1, kAnchorFadeAnimMs);

  // We might not get a LeaveNotify event*, so we also poll the pointer
  // position.

  // * If the mouse cursor has already been moved away before the anchor
  // input window gets created, the anchor never gets a mouse leave event.
  // Additionally, Chrome appears to be stacking its status bubble window
  // above all other windows, so we sometimes get a leave event as soon as
  // we slide a panel up.
  anchor_pointer_watcher_.reset(
      new PointerPositionWatcher(
          wm()->event_loop(),
          wm()->xconn(),
          NewPermanentCallback(this, &PanelBar::DestroyAnchor),
          false,  // watch_for_entering_target=false
          x, y, width, height));
}

void PanelBar::DestroyAnchor() {
  wm()->xconn()->ConfigureWindowOffscreen(anchor_input_xid_);
  anchor_actor_->SetOpacity(0, kAnchorFadeAnimMs);
  anchor_panel_ = NULL;
  anchor_pointer_watcher_.reset();
}

Panel* PanelBar::GetNearestExpandedPanel(Panel* panel) {
  if (!panel || !panel->is_expanded())
    return NULL;

  Panel* nearest_panel = NULL;
  int best_distance = kint32max;
  for (PanelSet::iterator it = all_panels_.begin();
       it != all_panels_.end(); ++it) {
    if (*it == panel || !(*it)->is_expanded())
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

void PanelBar::ConfigureShowCollapsedPanelsInputWindow(bool move_onscreen) {
  DLOG(INFO) << (move_onscreen ? "Showing" : "Hiding") << " input window "
             << XidStr(show_collapsed_panels_input_xid_)
             << " for showing collapsed panels";
  if (move_onscreen) {
    wm()->ConfigureInputWindow(
        show_collapsed_panels_input_xid_,
        0, wm()->height() - kShowCollapsedPanelsDistancePixels,
        wm()->width(), kShowCollapsedPanelsDistancePixels);
  } else {
    wm()->xconn()->ConfigureWindowOffscreen(show_collapsed_panels_input_xid_);
  }
}

void PanelBar::StartHideCollapsedPanelsWatcher() {
  hide_collapsed_panels_pointer_watcher_.reset(
      new PointerPositionWatcher(
          wm()->event_loop(),
          wm()->xconn(),
          NewPermanentCallback(this, &PanelBar::HideCollapsedPanels),
          false,  // watch_for_entering_target=false
          0, wm()->height() - kHideCollapsedPanelsDistancePixels,
          wm()->width(), kHideCollapsedPanelsDistancePixels));
}

void PanelBar::ShowCollapsedPanels() {
  DLOG(INFO) << "Showing collapsed panels";
  DisableShowCollapsedPanelsTimeout();
  collapsed_panel_state_ = COLLAPSED_PANEL_STATE_SHOWN;

  for (PanelSet::iterator it = all_panels_.begin();
       it != all_panels_.end(); ++it) {
    Panel* panel = *it;
    if (panel->is_expanded())
      continue;
    const int computed_y = ComputePanelY(*panel);
    if (panel->titlebar_y() != computed_y)
      panel->MoveY(computed_y, true, kHideCollapsedPanelsAnimMs);
  }

  ConfigureShowCollapsedPanelsInputWindow(false);
  StartHideCollapsedPanelsWatcher();
}

void PanelBar::HideCollapsedPanels() {
  DLOG(INFO) << "Hiding collapsed panels";
  DisableShowCollapsedPanelsTimeout();

  if (dragged_panel_ && !dragged_panel_->is_expanded()) {
    // Don't hide the panels in the middle of the drag -- we'll do it in
    // HandlePanelDragComplete() instead.
    DLOG(INFO) << "Deferring hiding collapsed panels since collapsed panel "
               << dragged_panel_->xid_str() << " is currently being dragged";
    collapsed_panel_state_ = COLLAPSED_PANEL_STATE_WAITING_TO_HIDE;
    return;
  }

  collapsed_panel_state_ = COLLAPSED_PANEL_STATE_HIDDEN;
  for (PanelSet::iterator it = all_panels_.begin();
       it != all_panels_.end(); ++it) {
    Panel* panel = *it;
    if (panel->is_expanded())
      continue;
    const int computed_y = ComputePanelY(*panel);
    if (panel->titlebar_y() != computed_y)
      panel->MoveY(computed_y, true, kHideCollapsedPanelsAnimMs);
  }

  if (GetNumCollapsedPanels() > 0)
    ConfigureShowCollapsedPanelsInputWindow(true);
  hide_collapsed_panels_pointer_watcher_.reset();
}

void PanelBar::DisableShowCollapsedPanelsTimeout() {
  if (show_collapsed_panels_timeout_id_ >= 0) {
    wm()->event_loop()->RemoveTimeout(show_collapsed_panels_timeout_id_);
    show_collapsed_panels_timeout_id_ = -1;
  }
}

void PanelBar::HandleShowCollapsedPanelsTimeout() {
  DCHECK(collapsed_panel_state_ == COLLAPSED_PANEL_STATE_WAITING_TO_SHOW);
  DisableShowCollapsedPanelsTimeout();
  ShowCollapsedPanels();
}

}  // namespace window_manager
