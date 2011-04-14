// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_LAYOUT_LAYOUT_MANAGER_H_
#define WINDOW_MANAGER_LAYOUT_LAYOUT_MANAGER_H_

#include <deque>
#include <map>
#include <string>
#include <tr1/memory>
#include <tr1/unordered_set>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/event_consumer.h"
#include "window_manager/focus_manager.h"
#include "window_manager/key_bindings.h"
#include "window_manager/panels/panel_manager.h"
#include "window_manager/window.h"
#include "window_manager/wm_ipc.h"  // for WmIpc::Message
#include "window_manager/x11/x_types.h"

namespace window_manager {

class EventConsumerRegistrar;
class KeyBindingsActionRegistrar;
class KeyBindingsGroup;
class MotionEventCoalescer;
class PanelManager;
class Separator;
class Window;
class WindowManager;
template<class T> class Stacker;  // from util.h

// Manages the placement of regular client windows.
//
// It currently supports two modes: "active", where a single toplevel
// window is displayed at full scale and given the input focus, and
// "overview", where scaled-down copies of all toplevel windows are
// displayed across the bottom of the screen.
//
class LayoutManager : public EventConsumer,
                      public FocusChangeListener,
                      public PanelManagerAreaChangeListener {
 public:
  LayoutManager(WindowManager* wm, PanelManager* panel_manager);
  ~LayoutManager();

  int x() const { return x_; }
  int y() const { return y_; }
  int width() const { return width_; }
  int height() const { return height_; }
  int overview_panning_offset() const { return overview_panning_offset_; }
  int num_toplevels() const { return toplevels_.size(); }

  // Begin EventConsumer implementation.
  virtual bool IsInputWindow(XWindow xid);
  virtual void HandleScreenResize();
  virtual void HandleLoggedInStateChange() {}
  virtual bool HandleWindowMapRequest(Window* win);
  virtual void HandleWindowMap(Window* win);
  virtual void HandleWindowUnmap(Window* win);
  virtual void HandleWindowInitialPixmap(Window* win);
  virtual void HandleWindowConfigureRequest(Window* win,
                                            int req_x, int req_y,
                                            int req_width, int req_height);
  virtual void HandleButtonPress(XWindow xid,
                                 int x, int y,
                                 int x_root, int y_root,
                                 int button,
                                 XTime timestamp);
  virtual void HandleButtonRelease(XWindow xid,
                                   int x, int y,
                                   int x_root, int y_root,
                                   int button,
                                   XTime timestamp);
  virtual void HandlePointerEnter(XWindow xid,
                                  int x, int y,
                                  int x_root, int y_root,
                                  XTime timestamp) {}
  virtual void HandlePointerLeave(XWindow xid,
                                  int x, int y,
                                  int x_root, int y_root,
                                  XTime timestamp) {}
  virtual void HandlePointerMotion(XWindow xid,
                                   int x, int y,
                                   int x_root, int y_root,
                                   XTime timestamp);
  virtual void HandleChromeMessage(const WmIpc::Message& msg);
  virtual void HandleClientMessage(XWindow xid,
                                   XAtom message_type,
                                   const long data[5]);
  virtual void HandleWindowPropertyChange(XWindow xid, XAtom xatom);
  virtual void OwnDestroyedWindow(DestroyedWindow* destroyed_win, XWindow xid) {
    NOTREACHED();
  }
  // End EventConsumer implementation.

  // Begin FocusChangeListener implementation.
  virtual void HandleFocusChange();
  // End FocusChangeListener implementation.

  // Begin PanelManagerAreaChangeListener implementation.
  virtual void HandlePanelManagerAreaChange();
  // End PanelManagerAreaChangeListener implementation.

  // Return a pointer to an arbitrary Chrome toplevel window, if one
  // exists.  Returns NULL if there is no such window.
  Window* GetChromeWindow();

  // Take the input focus if possible.  Returns false if it doesn't make
  // sense to take the focus (currently, we take the focus if we're in
  // active mode but refuse to in overview mode).
  bool TakeFocus(XTime timestamp);

 private:
  FRIEND_TEST(LayoutManagerTest, Basic);  // uses SetMode()
  FRIEND_TEST(LayoutManagerTest, Focus);
  FRIEND_TEST(LayoutManagerTest, ConfigureTransient);
  FRIEND_TEST(LayoutManagerTest, FocusTransient);
  FRIEND_TEST(LayoutManagerTest, Resize);
  FRIEND_TEST(LayoutManagerTest, OverviewFocus);
  FRIEND_TEST(LayoutManagerTest, ChangeCurrentSnapshot);
  FRIEND_TEST(LayoutManagerTest, StackTransientsAbovePanels);
  FRIEND_TEST(LayoutManagerTest, NoDimmingInActiveMode);
  FRIEND_TEST(LayoutManagerTest, AvoidMovingCurrentWindow);
  FRIEND_TEST(LayoutManagerTest, OverviewSpacing);
  FRIEND_TEST(LayoutManagerTest, NestedTransients);
  FRIEND_TEST(LayoutManagerTest, KeyBindings);
  FRIEND_TEST(LayoutManagerTest, ChangeModeWithNoWindows);
  FRIEND_TEST(LayoutManagerTest, ChangeBackgroundsAfterInitialWindow);
  FRIEND_TEST(LayoutManagerTest, DontGrabBackAndForwardKeysInActiveMode);
  FRIEND_TEST(LayoutManagerTest, SwitchToToplevelWithModalTransient);
  FRIEND_TEST(LayoutManagerTest, TransientOwnedByChildWindow);
  FRIEND_TEST(LayoutManagerTest, CycleTabs);

  // Internal private class, declared in toplevel_window.h
  class ToplevelWindow;

  // Internal private class, declared in separator.h
  class Separator;

  // Internal private class, declared in snapshot_window.h
  class SnapshotWindow;

  // Typedefs for the containers used below.
  typedef std::deque<std::tr1::shared_ptr<ToplevelWindow> > ToplevelWindows;
  typedef std::deque<std::tr1::shared_ptr<SnapshotWindow> > SnapshotWindows;
  typedef std::deque<std::tr1::shared_ptr<Separator> > Separators;
  typedef std::map<XWindow, SnapshotWindow*> XWindowToSnapshotMap;
  typedef std::map<XWindow, ToplevelWindow*> XWindowToToplevelMap;

  // Modes used to display windows.
  enum Mode {
    // Display the current toplevel window at full size and let it
    // receive input.  Hide all other windows.
    MODE_ACTIVE,

    // Display stacked snapshots of all of the tabs instead of the
    // toplevel windows.
    MODE_OVERVIEW,

    // This is only passed in to SetMode() when the user hits Escape to
    // exit out of overview mode without selecting a window.  It's
    // immediately mapped to MODE_ACTIVE, so no other code needs to be able
    // to handle it.
    MODE_ACTIVE_CANCELLED,
  };

  // What fraction of the manager's total width should be placed between
  // groups of snapshots in overview mode?
  static const double kOverviewGroupSpacing;

  // How many pixels should be used for padding the snapshot on the
  // right side when it is selected.
  static const double kOverviewSelectedPadding;

  // What's the maximum fraction of the manager's total size that a window
  // should be scaled to in overview mode?
  static const double kOverviewWindowMaxSizeRatio;

  // What fraction of the manager's total width should be visible on the
  // sides when the snapshots are panned all the way to one end or the
  // other?
  static const double kSideMarginRatio;

  // What fraction of the manager's total width should each window use
  // for peeking out underneath the window on top of it in overview
  // mode?
  static const double kOverviewExposedWindowRatio;

  // This is the speed at which the background image moves relative to
  // how much the snapshots move when a new snapshot is selected.
  static const double kBackgroundScrollRatio;

  // Animation speed used for windows.
  static const int kWindowAnimMs;

  // This is the scale of an unselected snapshot window, relative to
  // a selected snapshot.
  static const double kOverviewNotSelectedScale;

  // This is the speed that opacity should be animated for some
  // contexts.
  static const int kWindowOpacityAnimMs;

  // This is the factor by which to stretch the background
  // horizontally so that it will scroll when the tab is changed in
  // overview mode.
  static const float kBackgroundExpansionFactor;

  // Returns a string containing the name of the given mode.
  static std::string GetModeName(Mode mode);
  Mode mode() const { return mode_; }

  // Recalculate the layout for all the managed windows, both toplevel
  // and snapshot, based on the current mode.
  void LayoutWindows(bool animate);

  // Switch the current mode.  If the mode changes, then the windows
  // will be laid out again.
  void SetMode(Mode mode);

  // Returns the current toplevel window.
  ToplevelWindow* current_toplevel() { return current_toplevel_; }
  void SetCurrentToplevel(ToplevelWindow* toplevel);

  // Returns the current snapshot window.
  SnapshotWindow* current_snapshot() { return current_snapshot_; }
  void SetCurrentSnapshot(SnapshotWindow* snapshot);

  // This sets the current snapshot window, and includes the
  // information about the mouse click that was used to select the
  // window.  The mouse coordinates should be relative to the origin
  // of the layout manager.  Supply -1 for x if the mouse coordinates
  // are not available, but the timestamp should always be supplied.
  void SetCurrentSnapshotWithClick(SnapshotWindow* snapshot,
                                   XTime timestamp,
                                   int x, int y);

  // Is the passed-in window type one that we should handle?
  static bool IsHandledWindowType(chromeos::WmIpcWindowType type);

  // Get the toplevel window represented by the passed-in input window, or
  // NULL if the input window doesn't belong to us.
  ToplevelWindow* GetToplevelWindowByInputXid(XWindow xid);

  // Get the 0-based index of the passed-in toplevel within |toplevels_|.
  // Returns -1 if it isn't present.
  int GetIndexForToplevelWindow(const ToplevelWindow& toplevel) const;

  // Get the 0-based index of the passed-in snapshot within |snapshots_|.
  // Returns -1 if it isn't present.
  int GetIndexForSnapshotWindow(const SnapshotWindow& snapshot) const;

  // Get the ToplevelWindow object representing the passed-in window.
  // Returns NULL if it isn't a toplevel window.
  ToplevelWindow* GetToplevelWindowByWindow(const Window& win);

  // Get the ToplevelWindow object representing the window with the
  // passed-in XID.  Returns NULL if the window doesn't exist or isn't a
  // toplevel window.
  ToplevelWindow* GetToplevelWindowByXid(XWindow xid);

  // Get the ToplevelWindow object that owns the passed-in
  // possibly-transient window.  Returns NULL if the window is unowned.
  ToplevelWindow* GetToplevelWindowOwningTransientWindow(const Window& win);

  // Change the area allocated to the layout manager.
  void MoveAndResizeForAvailableArea();

  // Get the snapshot window represented by the passed-in input window, or
  // NULL if the input window doesn't belong to us.
  SnapshotWindow* GetSnapshotWindowByInputXid(XWindow xid);

  // Get the SnapshotWindow object representing the passed-in window.
  // Returns NULL if it isn't a snapshot window.
  SnapshotWindow* GetSnapshotWindowByWindow(const Window& win);

  // Get the SnapshotWindow object representing the window with the
  // passed-in XID.  Returns NULL if the window doesn't exist or isn't a
  // snapshot window.
  SnapshotWindow* GetSnapshotWindowByXid(XWindow xid);

  // Get the SnapshotWindow object that is stacked "after" or "before"
  // the given one.  Returns NULL if there is no snapshot window in
  // that position.
  SnapshotWindow* GetSnapshotAfter(SnapshotWindow* window);
  SnapshotWindow* GetSnapshotBefore(SnapshotWindow* window);

  // This gets the snapshot window that corresponds to the selected
  // tab in the given toplevel window.  Returns NULL if there is no
  // selected tab, or if the selected tab doesn't have a corresponding
  // snapshot window.
  SnapshotWindow* GetSelectedSnapshotFromToplevel(const ToplevelWindow& window);

  // Get the XID of the input window created for a window.
  XWindow GetInputXidForWindow(const Window& win);

  // Helper method called by HandleWindowMap() for transient windows.
  void HandleTransientWindowMap(Window* win);

  // Activate the toplevel window at the passed-in 0-indexed position (or
  // the last window, for index -1).  Does nothing if no window exists at
  // that position or if we're not already in active mode.
  void HandleToplevelChangeRequest(int index);

  // Make the snapshot window at the passed-in 0-indexed position
  // current (or the last window, for index -1).  Does nothing if no
  // window exists at that position or if not already in overview
  // mode.
  void HandleSnapshotChangeRequest(int index);

  // This calculates a new panning offset that will center the given
  // snapshot window in the display.  LayoutWindows still needs to be
  // called after this function to have the centering take effect.  x
  // and y are the coordinates (relative to the layout manager origin)
  // of the mouse click used to select the snapshot.  If either
  // coordinate is negative, then the mouse click coordinates are
  // ignored.
  void CenterCurrentSnapshot(int x, int y);

  // Calculate the position and scaling of all snapshots for overview
  // mode and record it in |snapshots_|.  If |enforce_bounds| is true,
  // then the bounds of dragging are enforced.
  void CalculatePositionsForOverviewMode(bool enforce_bounds);

  // Cycle the current toplevel window.  Only makes sense in active mode.
  void CycleCurrentToplevelWindow(bool forward);

  // Cycle the current snapshot window.  Only makes sense in overview mode.
  void CycleCurrentSnapshotWindow(bool forward);

  // Send a message to a window describing the current state of |mode_|.
  // Does nothing if |win| isn't a toplevel Chrome window.
  void SendModeMessage(ToplevelWindow* toplevel, bool cancelled);

  // Ask the current window to delete itself.
  void SendDeleteRequestToCurrentToplevel();

  // Pan across the windows horizontally in overview mode.
  // |offset| is applied relative to the current panning offset.
  void PanOverviewMode(int offset);

  // Update the panning in overview mode based on mouse motion stored in
  // |overview_background_event_coalescer_|.  Invoked by a timer.
  void UpdateOverviewPanningForMotion();

  // Ensure that a toplevel is displayed onscreen in active mode (switching
  // out of overview mode if needed) and then tell it to take the focus.
  void DisplayAndFocusToplevel(ToplevelWindow* toplevel);

  // Enable or disable the key bindings group for the passed-in mode.
  void EnableKeyBindingsForMode(Mode mode);
  void DisableKeyBindingsForMode(Mode mode);

  // If the snapshot corresponding to the current tab in the toplevel
  // windows exists, then make that the selected snapshot.
  void UpdateCurrentSnapshot();

  // Remove a snapshot from the list of snapshots, updating all the
  // appropriate stuff.
  void RemoveSnapshot(SnapshotWindow* snapshot);

  // Remove a toplevel from the list of toplevels, updating all the
  // appropriate stuff, and also removing any snapshots that are
  // associated with this toplevel.
  void RemoveToplevel(ToplevelWindow* toplevel);

  // Sorts the snapshots_ based on the order of the toplevel windows
  // they belong to, and their tab index within those toplevel
  // windows.  Returns true if something changed when we sorted it.
  bool SortSnapshots();

  // Make sure there are n-1 separators available for placing between
  // groups of snapshots, where n is the number of real chrome
  // toplevel windows (which can be smaller than toplevels_.size()).
  void AddOrRemoveSeparatorsAsNeeded();

  // Gets the number of tabs in the toplevel windows preceeding
  // |toplevel| in the list, but not including |toplevel|'s tabs.
  int GetPreceedingTabCount(const ToplevelWindow& toplevel) const;

  // Make |toplevel| be fullscreen (this currently just means that it'll be
  // stacked above other windows, panels, etc.).  If another toplevel is
  // fullscreen already it will be restored first, and |toplevel| will be
  // made current if it isn't already.
  void MakeToplevelFullscreen(ToplevelWindow* toplevel);

  // Make |toplevel|, which should already be fullscreen, just be a regular
  // non-fullscreen window again.
  void RestoreFullscreenToplevel(ToplevelWindow* toplevel);

  // Takes ownership of the the passed-in actor and uses it as the
  // background.  This is just a separate method so that tests can use it.
  void SetBackground(Compositor::Actor* actor);

  // Resizes and positions the background to account for the image
  // aspect ratio and for scrolling in overview mode.
  void ConfigureBackground(int width, int height);

  // Set things up after we see the first toplevel Chrome window get mapped.
  void HandleFirstToplevelChromeWindowMapped(Window* win);

  // Handle a change in a transient window's modality.  If it became modal,
  // we switch to its owner, dim everything underneath the transient
  // window, and disable keyboard shortcuts.  This method is also invoked
  // when the window's _NET_WM_STATE_MODAL hint didn't change, but either
  // the window or its owner was unmapped.
  void HandleTransientWindowModalityChange(Window* transient_win,
                                           bool window_or_owner_was_unmapped);

  WindowManager* wm_;            // not owned
  PanelManager* panel_manager_;  // not owned

  // The current mode.
  Mode mode_;

  // Area available to us for placing windows.
  int x_;
  int y_;
  int width_;
  int height_;

  // Area used by the panel manager on the left and right sides of the
  // screen.
  int panel_manager_left_width_;
  int panel_manager_right_width_;

  // Information about toplevel windows, stored in the order in which
  // we'll display them in overview mode.
  ToplevelWindows toplevels_;

  // Information about snapshot windows, stored in their index order.
  SnapshotWindows snapshots_;

  // Map from input windows to the snapshot windows they represent.
  XWindowToSnapshotMap input_to_snapshot_;

  // Map from transient windows' XIDs to the toplevel windows that own
  // them.  This is based on the transient windows' WM_TRANSIENT_FOR hints
  // at the time that they were mapped; we ignore any subsequent changes to
  // this hint.  (Note that snapshot windows don't have any transients.)
  XWindowToToplevelMap transient_to_toplevel_;

  // This is the current toplevel window.  This means that in active
  // mode this one has the focus and is displayed fullscreen.  In
  // snapshot mode, this is the one that the current snapshot belongs
  // to.  Unless there are no toplevel windows, this should never be
  // NULL.
  ToplevelWindow* current_toplevel_;

  // This is the current snapshot window.  This means that in overview
  // mode, this one is displayed highlighted.  Unless there are no
  // snapshot windows, this should never be NULL.
  SnapshotWindow* current_snapshot_;

  // Fullscreen toplevel window, or NULL if no toplevel window is currently
  // fullscreen.
  ToplevelWindow* fullscreen_toplevel_;

  // Amount that snapshot windows' positions should be offset to the left
  // for overview mode.  Used to implement panning.
  int overview_panning_offset_;

  // Amount that the background position should be offset to the left
  // for overview mode, based on the currently selected snapshot.
  // This is to simulate a 3D effect: that the snapshots are "in front
  // of" the background by panning the background slightly when the
  // selection changes.
  int overview_background_offset_;

  // This is the overall width of the snapshots as they are laid out.
  // This is set in CalculatePositionsForOverviewMode, so that must be
  // called before this is current.
  int overview_width_of_snapshots_;

  // Mouse pointer motion gets stored here during a drag on the background
  // window in overview mode so that it can be applied periodically in
  // UpdateOverviewPanningForMotion().
  scoped_ptr<MotionEventCoalescer> overview_background_event_coalescer_;

  // X component of the pointer's previous position during a drag on the
  // background window.
  int overview_drag_last_x_;

  // Have we seen a MapRequest event yet?  We perform some initial setup
  // (e.g. stacking) in response to MapRequests, so we track this so we can
  // perform the same setup at the MapNotify point for windows that were
  // already mapped or were in the process of being mapped when we were
  // started.
  // TODO: Find another way to do this now that we're grabbing the server
  // at startup.
  bool saw_map_request_;

  // Have we seen a toplevel Chrome window get mapped yet?
  bool first_toplevel_chrome_window_mapped_;

  // Event registrations for the layout manager itself.
  scoped_ptr<EventConsumerRegistrar> event_consumer_registrar_;

  // Key binding actions that we've registered.
  scoped_ptr<KeyBindingsActionRegistrar> key_bindings_actions_;

  // Groups of key bindings that are relevant to different modes.
  scoped_ptr<KeyBindingsGroup> active_mode_key_bindings_group_;
  scoped_ptr<KeyBindingsGroup> overview_mode_key_bindings_group_;

  // Key bindings only enabled after |first_toplevel_chrome_window_mapped_| is
  // true.
  scoped_ptr<KeyBindingsGroup> post_toplevel_key_bindings_group_;

  // Deque of separators for placing between groups of snapshots.
  Separators separators_;

  // Input window at the layer of the background image.  This exists solely
  // for the purpose of installing button grabs -- we can't install them on
  // the root window itself since they'd get activated by clicks in any of
  // the root's subwindows (this is apparently just how button grabs work
  // -- see the list of conditions in the XGrabButton() man page).
  XWindow background_xid_;

  // This background is displayed post-login in overview mode.
  scoped_ptr<Compositor::Actor> background_;

  // Should we call LayoutWindows() after we see the initial pixmap get
  // fetched for the current toplevel window?
  bool should_layout_windows_after_initial_pixmap_;

  // Should we use animation when calling LayoutWindows() as described in
  // |should_layout_windows_after_initial_pixmap_|?  We avoid animating if
  // this is the first browser window, but leave this set to true after that.
  bool should_animate_after_initial_pixmap_;

  // Transient windows that have the _NET_WM_STATE_MODAL hint set.
  std::tr1::unordered_set<Window*> modal_transients_;

  DISALLOW_COPY_AND_ASSIGN(LayoutManager);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_LAYOUT_LAYOUT_MANAGER_H_
