// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_LAYOUT_SNAPSHOT_WINDOW_H_
#define WINDOW_MANAGER_LAYOUT_SNAPSHOT_WINDOW_H_

#include "window_manager/layout/layout_manager.h"

namespace window_manager {

class LayoutManager;

// A snapshot window that the layout manager is managing.
// Note that this is a private internal class to LayoutManager.

class LayoutManager::SnapshotWindow {
 public:
  // This is the amount of tilt to give to an unselected
  // snapshot.
  static const float kUnselectedTilt;

  // Padding between the fav icon and the title in pixels.
  static const int kFavIconPadding;

  // Padding between the bottom of the snapshot and the title in pixels.
  static const int kTitlePadding;

  SnapshotWindow(Window* win, LayoutManager* layout_manager);
  ~SnapshotWindow();

  enum State {
    // The window has just been added.
    STATE_NEW = 0,

    // We're in active mode.
    STATE_ACTIVE_MODE_INVISIBLE,

    // We're in overview mode and the window should be displayed in the
    // normal manner.
    STATE_OVERVIEW_MODE_NORMAL,

    // We're in overview mode and the window should be selected.
    STATE_OVERVIEW_MODE_SELECTED,
  };

  Window* win() const { return win_; }
  XWindow input_xid() const { return input_xid_; }
  // Returns the tab index for sorting.
  int tab_index() const { return tab_index_; }
  LayoutManager::ToplevelWindow* toplevel() const {
    if (!toplevel_) {
      const_cast<LayoutManager::SnapshotWindow*>(this)->toplevel_ =
          layout_manager_->GetToplevelWindowByXid(toplevel_xid_);
      LOG_IF(ERROR, !toplevel_) << "Snapshot " << win_->xid_str()
                                << " can't find its toplevel window";
    }
    return toplevel_;
  }
  Window* title() const { return title_; }
  Window* fav_icon() const { return fav_icon_; }
  void clear_title() { title_ = NULL; }
  void clear_fav_icon() { fav_icon_ = NULL; }
  int overview_x() const { return overview_x_; }
  int overview_y() const { return overview_y_; }
  int overview_width() const { return overview_width_; }
  int overview_height() const { return overview_height_; }
  int overview_tilted_width() const {
    return Compositor::Actor::GetTiltedWidth(
        overview_width_,
        (this == layout_manager_->current_snapshot()) ? 0 : kUnselectedTilt);
  }
  State state() const { return state_; }

  // Used for debugging to get the readable name of a state.
  static std::string GetStateName(State state);

  // Sets the state of this window.  UpdateLayout must be called after
  // this to update the layout to match.
  void SetState(State state);

  // Adds a decoration to this snapshot.  A decoration is a
  // Chrome-rendered window that contains the title or fav icon of the
  // snapshot.  This sets title_ or fav_icon_, depending the window
  // type.
  void AddDecoration(Window* decoration);

  // Updates the layout of this window based on its current state.  If
  // animate is true, then animate the window into its new state,
  // otherwise just jump to the new state.
  void UpdateLayout(bool animate);

  // Handles changes in the window type properties: e.g. the tab index
  // changes.  Called by the layout manager when it sees that
  // properties have changed.  If internal state changed, returns
  // true.
  bool PropertiesChanged();

  // Handles a resize of the screen by making sure that client windows are
  // still offscreen.
  void HandleScreenResize();

  // Comparison function used when sorting snapshot windows by tab index.
  static bool CompareTabIndex(std::tr1::shared_ptr<SnapshotWindow> first,
                              std::tr1::shared_ptr<SnapshotWindow> second) {
    // Returns true if first goes before second in the sort.
    return first->CalculateOverallIndex() < second->CalculateOverallIndex();
  }

  // Get the absolute X-position of the window's center.
  int GetAbsoluteOverviewCenterX() const {
    return layout_manager_->x() + overview_x_ + 0.5 * overview_width_;
  }

  // Get the absolute Y-position to place a window directly below the
  // layout manager's region.
  int GetAbsoluteOverviewOffscreenY() const {
    return layout_manager_->y() + layout_manager_->height();
  }

  // Set |overview_x_| and |overview_y_| to the passed-in values.
  void SetPosition(int x, int y) {
    overview_x_ = x;
    overview_y_ = y;
  }

  // Update the stored size of the composited window such that it fits
  // in the dimensions |max_width| and |max_height|.
  void SetSize(int max_width, int max_height);

  // Handle this snapshot window's input window getting a button
  // release event by selecting it as the current snapshot, or
  // switching back to active mode if this snapshot is already the
  // current one.  |x| and |y| are mouse coordinates that are relative
  // to the layout manager's origin.
  void HandleButtonRelease(XTime timestamp, int x, int y);

 private:
  WindowManager* wm() { return layout_manager_->wm_; }

  // Returns the index of this snapshot in the overall list of snapshots.
  int CalculateOverallIndex() const;

  // Configure the window for active mode.  This animating the
  // composited window according to |state_|, and fading out to reveal
  // the active toplevel window (which should be in place after
  // animating its own configuration to active mode).
  void ConfigureForActiveMode(bool animate);

  // Configure the window for overview mode.  This involves animating its
  // composited position and scale as specified by |overview_*| and
  // |state_|, and moving its input window onscreen.
  void ConfigureForOverviewMode(bool animate);

  // Window object for the snapshot client window.
  Window* win_;  // not owned

  LayoutManager* layout_manager_;  // not owned

  // This is the tab index of this snapshot from the last time the
  // properties changed (or we were created).
  int tab_index_;

  // This is the toplevel window that this snapshot belongs to.  We
  // have to keep both because sometimes the toplevel window hasn't
  // been mapped by the time the snapshot is mapped.  This is because
  // Chrome doesn't offer a notification when the toplevel window is
  // mapped, only when the browser window is "ready", so consequently,
  // snapshots can be mapped before the toplevel window is, even
  // though they are only mapped in response to the notification that
  // the browser window is "ready" (the problem being that "ready"
  // doesn't have to mean "mapped").
  ToplevelWindow* toplevel_;
  XWindow toplevel_xid_;

  // This is the window associated with the snapshot title rendered by
  // Chrome.
  Window* title_;

  // This is the window associated with the snapshot fav icon rendered by
  // Chrome.
  Window* fav_icon_;

  // The invisible input window that represents the client window in
  // overview mode.
  XWindow input_xid_;

  // The state the window is in.  Used to determine how it should be
  // animated by ArrangeFor*() methods.
  State state_;

  // This keeps the state that this window was in at the end of the
  // last update, so we can use that to determine what the transition
  // animation should be between states.
  State last_state_;

  // Position and dimensions that should be used for drawing the
  // window in overview mode.  The X and Y coordinates are relative to
  // the layout manager's origin.
  int overview_x_;
  int overview_y_;
  int overview_width_;
  int overview_height_;
  float overview_scale_;

  // LayoutManager event registrations for this snapshot window and its
  // input window.
  scoped_ptr<EventConsumerRegistrar> event_consumer_registrar_;

  DISALLOW_COPY_AND_ASSIGN(SnapshotWindow);
};

}  // end namespace window_manager

#endif  // WINDOW_MANAGER_LAYOUT_SNAPSHOT_WINDOW_H_
