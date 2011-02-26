// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_PANEL_BAR_H_
#define WINDOW_MANAGER_PANEL_BAR_H_

#include <map>
#include <set>
#include <tr1/memory>
#include <vector>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/panel_container.h"
#include "window_manager/x_types.h"

namespace window_manager {

class EventConsumerRegistrar;
class Panel;
class PanelManager;
class PointerPositionWatcher;
class Shadow;
class Window;
class WindowManager;

// The panel bar handles panels that are pinned to the bottom of the
// screen.
class PanelBar : public PanelContainer {
 public:
  explicit PanelBar(PanelManager* panel_manager);
  ~PanelBar();

  WindowManager* wm();

  // Begin PanelContainer implementation.
  virtual void GetInputWindows(std::vector<XWindow>* windows_out);
  virtual void AddPanel(Panel* panel, PanelSource source);
  virtual void RemovePanel(Panel* panel);
  virtual bool ShouldAddDraggedPanel(const Panel* panel,
                                     int drag_x, int drag_y);
  virtual void HandleInputWindowButtonPress(XWindow xid,
                                            int x, int y,
                                            int x_root, int y_root,
                                            int button,
                                            XTime timestamp);
  virtual void HandleInputWindowButtonRelease(XWindow xid,
                                              int x, int y,
                                              int x_root, int y_root,
                                              int button,
                                              XTime timestamp) {}
  virtual void HandleInputWindowPointerEnter(XWindow xid,
                                             int x, int y,
                                             int x_root, int y_root,
                                             XTime timestamp);
  virtual void HandleInputWindowPointerLeave(XWindow xid,
                                             int x, int y,
                                             int x_root, int y_root,
                                             XTime timestamp);
  virtual void HandlePanelButtonPress(Panel* panel,
                                      int button,
                                      XTime timestamp);
  virtual void HandlePanelTitlebarPointerEnter(Panel* panel, XTime timestamp);
  virtual void HandleSetPanelStateMessage(Panel* panel, bool expand);
  virtual bool HandleNotifyPanelDraggedMessage(Panel* panel,
                                               int drag_x, int drag_y);
  virtual void HandleNotifyPanelDragCompleteMessage(Panel* panel);
  virtual void HandleFocusPanelMessage(Panel* panel, XTime timestamp);
  virtual void HandlePanelResizeRequest(Panel* panel,
                                        int req_width, int req_height);
  virtual void HandlePanelResizeByUser(Panel* panel);
  virtual void HandleScreenResize();
  virtual void HandlePanelUrgencyChange(Panel* panel);
  virtual bool TakeFocus(XTime timestamp);
  // End PanelContainer implementation.

  // Number of pixels between the rightmost panel and the right edge of the
  // screen, in pixels.
  static const int kRightPaddingPixels;

  // Amount of horizontal padding to place between panels, in pixels.
  static const int kPixelsBetweenPanels;

  // How close does the pointer need to get to the bottom of the screen
  // before we show hidden collapsed panels?
  static const int kShowCollapsedPanelsDistancePixels;

  // How far away from the bottom of the screen can the pointer get before
  // we hide collapsed panels?
  static const int kHideCollapsedPanelsDistancePixels;

  // How much of the top of a collapsed panel's titlebar should peek up
  // from the bottom of the screen when it is hidden?
  static const int kHiddenCollapsedPanelHeightPixels;

  // How far to the left of the main block of packed panels does a panel
  // need to be dragged before it becomes a floating, "independently
  // positioned" panel?
  static const int kFloatingPanelThresholdPixels;

 private:
  friend class BasicWindowManagerTest;
  FRIEND_TEST(PanelBarTest, FocusNewPanel);
  FRIEND_TEST(PanelBarTest, HideCollapsedPanels);
  FRIEND_TEST(PanelBarTest, DeferHidingDraggedCollapsedPanel);
  FRIEND_TEST(WindowManagerTest, KeepPanelsAfterRestart);

  // PanelBar-specific information about a panel.
  struct PanelInfo {
    PanelInfo() : desired_right(0), is_floating(false) {}

    // X position of the right edge of where the panel wants to be.
    //
    // For panels in |packed_panels_|, this is the panel's snapped
    // position.  While the panel is being dragged, this may be different
    // from its actual composited position -- we only snap the panels to
    // this position when the drag is complete.
    //
    // For panels in |floating_panels_|, this is the position where the
    // user last dropped the panel.  The panel may be displaced to either
    // side if another panel is dropped on top of it, or may be pushed to
    // the left by the main group of packed panels.
    int desired_right;

    // Is this panel in |floating_panels_| (as opposed to
    // |packed_panels_|)?
    bool is_floating;
  };

  typedef std::set<Panel*> PanelSet;
  typedef std::vector<Panel*> PanelVector;

  // Is |collapsed_panel_state_| such that collapsed panels are currently
  // hidden offscreen?
  bool CollapsedPanelsAreHidden() const {
    return collapsed_panel_state_ == COLLAPSED_PANEL_STATE_HIDDEN ||
           collapsed_panel_state_ == COLLAPSED_PANEL_STATE_WAITING_TO_SHOW;
  }

  // Get the PanelInfo object for a panel, crashing if it's not present.
  PanelInfo* GetPanelInfoOrDie(Panel* panel);

  // Get the current number of collapsed panels.
  int GetNumCollapsedPanels();

  // Compute the Y-position where the top of the passed-in panel
  // should be placed (depending on whether it's expanded or collapsed,
  // whether collapsed panels are currently hidden, whether the panel's
  // urgent flag is set, etc.).
  int ComputePanelY(const Panel& panel);

  // Ensure that a panel is in either |packed_panels_| or
  // |floating_panels_|, removing it from the other array if we need to
  // move it.  Also updates |info->is_floating| and makes sure that the
  // panel is inserted at the correct position in the vector.  Returns true
  // if the panel was moved and false otherwise.
  bool MovePanelToPackedVector(Panel* panel, PanelInfo* info);
  bool MovePanelToFloatingVector(Panel* panel, PanelInfo* info);

  // Expand a panel.  If |create_anchor| is true, we additionally create an
  // anchor for it.
  void ExpandPanel(Panel* panel, bool create_anchor, int anim_ms);

  // Collapse a panel.
  void CollapsePanel(Panel* panel, int anim_ms);

  // Focus the passed-in panel's content window.
  // Also updates |desired_panel_to_focus_|.
  void FocusPanel(Panel* panel, XTime timestamp);

  // Get the panel with the passed-in content or titlebar window.
  // Returns NULL for unknown windows.
  Panel* GetPanelByWindow(const Window& win);

  // Get an iterator to the panel containing |win| (either a content or
  // titlebar window) from the passed-in vector.  Returns panels.end() if
  // the panel isn't present.
  static PanelVector::iterator FindPanelInVectorByWindow(
      PanelVector& panels, const Window& win);

  // Handle the end of a panel drag.
  void HandlePanelDragComplete(Panel* panel);

  // Update the position of |panel_to_reorder| within |panels| based on its
  // current position.  Note that the panel doesn't actually get moved to a
  // new position onscreen; we just rotate it to the spot where it should
  // be in the vector -- ArrangePanels() must be called afterwards to pack
  // the panels.  Returns true if the panel was reordered and false
  // otherwise.
  static bool ReorderPanelInVector(Panel* panel_to_reorder,
                                   PanelVector* panels);

  // Pack all panels in |packed_panels_| with the exception of
  // |dragged_panel_| (if non-NULL) towards the right.  We reserve space
  // for |dragged_panel_| and update its desired/snapped position, but we
  // don't update its actual position.
  //
  // If |arrange_floating| is true, floating ("independently positioned")
  // panels are also arranged.  All floating panels are shifted to the left
  // as needed so as to not overlap the main group of packed panels, and
  // they're also arranged so as to not overlap each other.  If
  // |fixed_floating_panel| is non-NULL, its current position takes
  // precedence over that of other panels -- see
  // UpdateFloatingPanelDesiredPositions() for details.
  void ArrangePanels(bool arrange_floating, Panel* fixed_floating_panel);

  // When a floating panel was just dropped (let's call it |fixed_panel|
  // here), we sometimes need to move some of the floating panels that are
  // to its right to make room for it.  If there's not enough room for
  // them, we may need to move some of them to the fixed panel's left.
  // This method is used by ArrangePanels() to do that.  |right_boundary|
  // is the rightmost position that a floating panel's right edge can take
  // (typically the left edge of the main group of packed panels plus some
  // padding).
  void ShiftFloatingPanelsAroundFixedPanel(Panel* fixed_panel,
                                           int right_boundary);

  // Create an anchor for a panel.  If there's a previous anchor, we
  // destroy it.
  void CreateAnchor(Panel* panel);

  // Destroy the anchor.
  void DestroyAnchor();

  // Get the expanded panel closest to |panel|, or NULL if there are no
  // other expanded panels (or if |panel| isn't expanded).
  Panel* GetNearestExpandedPanel(Panel* panel);

  // Move |show_collapsed_panels_input_xid_| onscreen or offscreen.
  void ConfigureShowCollapsedPanelsInputWindow(bool move_onscreen);

  // Initialize |hide_collapsed_panels_pointer_watcher_| to call
  // HideCollapsedPanels() as soon as it sees the pointer move too far away
  // from the bottom of the screen.
  void StartHideCollapsedPanelsWatcher();

  // Show collapsed panels' full titlebars at the bottom of the screen.
  void ShowCollapsedPanels();

  // Hide everything but the very top of collapsed panels' titlebars.  If a
  // collapsed panel is being dragged, defers hiding them and sets
  // |collapsed_panel_state_| to COLLAPSED_PANEL_STATE_WAITING_TO_HIDE
  // instead.
  void HideCollapsedPanels();

  // If |show_collapsed_panels_timeout_id_| is set, disable the timeout and
  // clear the variable.
  void DisableShowCollapsedPanelsTimeout();

  // Invoke ShowCollapsedPanels() and clear
  // |show_collapsed_panels_timeout_id_|.
  void HandleShowCollapsedPanelsTimeout();

  PanelManager* panel_manager_;  // not owned

  // All of our panels, in no particular order.
  PanelSet all_panels_;

  // Total width of all packed panels (including padding between them).
  int packed_panel_width_;

  // Panels that are packed against the right edge of the screen, in
  // left-to-right order.
  PanelVector packed_panels_;

  // Panels that have been dragged off to the left and are now
  // independently positioned, in left-to-right order.
  PanelVector floating_panels_;

  // Information about our panels that doesn't belong in the Panel class
  // itself.
  std::map<Panel*, std::tr1::shared_ptr<PanelInfo> > panel_infos_;

  // The panel that's currently being dragged, or NULL if none is.
  Panel* dragged_panel_;

  // Is |dragged_panel_| being dragged horizontally (as opposed to
  // vertically)?
  bool dragging_panel_horizontally_;

  // Input window used to receive events for the anchor displayed under
  // panels after they're expanded.
  XWindow anchor_input_xid_;

  // Panel for which the anchor is currently being displayed.
  Panel* anchor_panel_;

  // Textured actor used to draw the anchor.
  scoped_ptr<Compositor::Actor> anchor_actor_;

  // Watches the pointer's position so we know when to destroy the anchor.
  scoped_ptr<PointerPositionWatcher> anchor_pointer_watcher_;

  // If we need to give the focus to a panel, we choose this one.
  Panel* desired_panel_to_focus_;

  // Different states that we can be in with regard to showing collapsed
  // panels at the bottom of the screen.
  enum CollapsedPanelState {
    // Showing the panels' full titlebars.
    COLLAPSED_PANEL_STATE_SHOWN = 0,

    // Just showing the tops of the titlebars.
    COLLAPSED_PANEL_STATE_HIDDEN,

    // Hiding the titlebars, but we'll show them after
    // |show_collapsed_panels_timeout_id_| fires.
    COLLAPSED_PANEL_STATE_WAITING_TO_SHOW,

    // Showing the titlebars, but the pointer has moved up from the bottom
    // of the screen while dragging a collapsed panel and we'll hide the
    // collapsed panels as soon as the drag finishes.
    COLLAPSED_PANEL_STATE_WAITING_TO_HIDE,
  };
  CollapsedPanelState collapsed_panel_state_;

  // Input window used to detect when the mouse is at the bottom of the
  // screen so that we can show collapsed panels.
  XWindow show_collapsed_panels_input_xid_;

  // ID of a timeout that we use to delay calling ShowCollapsedPanels()
  // after the pointer enters |show_collapsed_panels_input_xid_|, or -1 if
  // unset.
  int show_collapsed_panels_timeout_id_;

  // Used to monitor the pointer position when we're showing collapsed
  // panels so that we'll know to hide them when the pointer far enough
  // away.
  scoped_ptr<PointerPositionWatcher> hide_collapsed_panels_pointer_watcher_;

  // PanelManager event registrations related to the panel bar's input
  // windows.
  scoped_ptr<EventConsumerRegistrar> event_consumer_registrar_;

  DISALLOW_COPY_AND_ASSIGN(PanelBar);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_PANEL_BAR_H_
