// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_TOPLEVEL_WINDOW_H_
#define WINDOW_MANAGER_TOPLEVEL_WINDOW_H_

#include "window_manager/layout_manager.h"

namespace window_manager {

class LayoutManager;

// A toplevel window that the layout manager is managing.
// Note that this is a private internal class to LayoutManager.

class LayoutManager::ToplevelWindow {
 public:
  ToplevelWindow(Window* win, LayoutManager* layout_manager);
  ~ToplevelWindow();

  enum State {
    // The window has just been added.
    STATE_NEW = 0,

    // We're in active mode and the window is onscreen.
    STATE_ACTIVE_MODE_ONSCREEN,

    // We're in active mode and the window is offscreen.
    STATE_ACTIVE_MODE_OFFSCREEN,

    // We're in active mode and the window should be animated sliding in
    // or out from a specific direction.
    STATE_ACTIVE_MODE_IN_FROM_RIGHT,
    STATE_ACTIVE_MODE_IN_FROM_LEFT,
    STATE_ACTIVE_MODE_OUT_TO_LEFT,
    STATE_ACTIVE_MODE_OUT_TO_RIGHT,
    STATE_ACTIVE_MODE_IN_FADE,
    STATE_ACTIVE_MODE_OUT_FADE,

    // We're in overview mode and the window should be displayed in the
    // normal manner on the bottom of the screen.
    STATE_OVERVIEW_MODE_NORMAL,

    // We're in overview mode and the window should be magnified.
    STATE_OVERVIEW_MODE_MAGNIFIED,
  };

  Window* win() { return win_; }
  XWindow input_xid() { return input_xid_; }

  State state() const { return state_; }
  void set_state(State state) { state_ = state; }

  int overview_x() const { return overview_x_; }
  int overview_y() const { return overview_y_; }
  int overview_width() const { return overview_width_; }
  int overview_height() const { return overview_height_; }
  int overview_scale() const { return overview_scale_; }

  // Get the absolute X-position of the window's center.
  int GetAbsoluteOverviewCenterX() const {
    return layout_manager_->x() + overview_x_ + 0.5 * overview_width_;
  }

  // Get the absolute Y-position to place a window directly below the
  // layout manager's region.
  int GetAbsoluteOverviewOffscreenY() const {
    return layout_manager_->y() + layout_manager_->height();
  }

  // Does the passed-in point fall within the bounds of our window in
  // overview mode?
  bool OverviewWindowContainsPoint(int x, int y) const {
    return x >= overview_x_ &&
        x < overview_x_ + overview_width_ &&
        y >= overview_y_ &&
        y < overview_y_ + overview_height_;
  }

  // Get the absolute position of the window for overview mode.
  int GetAbsoluteOverviewX() const;
  int GetAbsoluteOverviewY() const;

  // Configure the window for active mode.  This involves either moving
  // the client window on- or offscreen (depending on
  // 'window_is_active'), animating the composited window according to
  // 'state_', and possibly focusing the window or one of its transients.
  // 'to_left_of_active' describes whether the window is to the left of
  // the active window or not.  If 'update_focus' is true, the window
  // will take the focus if it's active.
  void ConfigureForActiveMode(bool window_is_active,
                              bool to_left_of_active,
                              bool update_focus);

  // Configure the window for overview mode.  This involves animating its
  // composited position and scale as specified by 'overview_*' and
  // 'state_', moving its client window offscreen (so it won't receive
  // mouse events), and moving its input window onscreen.  If
  // 'incremental' is true, this is assumed to be an incremental change
  // being done in response to e.g. a mouse drag, so we will skip doing
  // things like animating changes and restacking windows.
  void ConfigureForOverviewMode(bool window_is_magnified,
                                bool dim_if_unmagnified,
                                ToplevelWindow* toplevel_to_stack_under,
                                bool incremental);

  // Set 'overview_x_' and 'overview_y_' to the passed-in values.
  void UpdateOverviewPosition(int x, int y) {
    overview_x_ = x;
    overview_y_ = y;
  }

  // Update 'overview_width_', 'overview_height_', and 'overview_scale_'
  // for our composited window such that it fits in the dimensions
  // 'max_width' and 'max_height'.
  void UpdateOverviewScaling(int max_width, int max_height);

  // Focus 'transient_to_focus_' if non-NULL or 'win_' otherwise.  Also
  // raises the transient window to the top of the stacking order.
  void TakeFocus(XTime timestamp);

  // Set the window to be focused the next time that TakeFocus() is
  // called.  NULL can be passed to indicate that the toplevel window
  // should get the focus.  Note that this request may be ignored if a
  // modal transient window already has the focus.
  void SetPreferredTransientWindowToFocus(Window* transient_win);

  // Does the toplevel window or one of its transients have the input focus?
  bool IsWindowOrTransientFocused() const;

  // Add a transient window.  Called in response to the window being
  // mapped.  The transient will typically be stacked above any other
  // existing transients (unless an existing transient is modal), but if
  // this is the only transient, it will be stacked above the toplevel if
  // 'stack_directly_above_toplevel' is true and in
  // StackingManager::LAYER_ACTIVE_TRANSIENT_WINDOW otherwise.
  void AddTransientWindow(Window* transient_win,
                          bool stack_directly_above_toplevel);

  // Remove a transient window.  Called in response to the window being
  // unmapped.
  void RemoveTransientWindow(Window* transient_win);

  // Handle a ConfigureRequest event about one of our transient windows.
  void HandleTransientWindowConfigureRequest(
      Window* transient_win,
      int req_x, int req_y, int req_width, int req_height);

  // Handle one of this toplevel's windows (either the toplevel window
  // itself or one of its transients) gaining or losing the input focus.
  void HandleFocusChange(Window* focus_win, bool focus_in);

  // Handle one of this toplevel's windows (either the toplevel window
  // itself or one of its transients) getting a button press.  We remove
  // the active pointer grab and try to assign the focus to the
  // clicked-on window.
  void HandleButtonPress(Window* button_win, XTime timestamp);

 private:
  // A transient window belonging to a toplevel window.
  // TODO: Make this into a class that uses EventConsumerRegistrar.
  struct TransientWindow {
   public:
    TransientWindow(Window* win)
        : win(win),
          x_offset(0),
          y_offset(0),
          centered(false) {
    }
    ~TransientWindow() {
      win = NULL;
    }

    // Save the transient window's current offset from its owner.
    void SaveOffsetsRelativeToOwnerWindow(Window* owner_win) {
      x_offset = win->client_x() - owner_win->client_x();
      y_offset = win->client_y() - owner_win->client_y();
    }

    // Update offsets so the transient will be centered over the
    // passed-in owner window.
    void UpdateOffsetsToCenterOverOwnerWindow(Window* owner_win) {
      x_offset = 0.5 * (owner_win->client_width() - win->client_width());
      y_offset = 0.5 * (owner_win->client_height() - win->client_height());
    }

    // The transient window itself.
    Window* win;

    // Transient window's position's offset from its owner's origin.
    int x_offset;
    int y_offset;

    // Is the transient window centered over its owner?  We set this when
    // we first center a transient window but remove it if the client
    // ever moves the transient itself.
    bool centered;
  };

  WindowManager* wm() { return layout_manager_->wm_; }

  // Get the TransientWindow struct representing the passed-in window.
  TransientWindow* GetTransientWindow(const Window& win);

  // Update the passed-in transient window's client and composited
  // windows appropriately for the toplevel window's current
  // configuration.
  void MoveAndScaleTransientWindow(TransientWindow* transient, int anim_ms);

  // Call UpdateTransientWindowPositionAndScale() for all transient
  // windows.
  void MoveAndScaleAllTransientWindows(int anim_ms);

  // Stack a transient window's composited and client windows.  If
  // 'other_win' is non-NULL, we stack 'transient' above it; otherwise,
  // we stack 'transient' at the top of
  // StackingManager::LAYER_ACTIVE_TRANSIENT_WINDOW.
  void ApplyStackingForTransientWindow(
      TransientWindow* transient, Window* other_win);

  // Restack all transient windows' composited and client windows in the
  // order dictated by 'stacked_transients_'.  If
  // 'stack_directly_above_toplevel' is false, then we stack the
  // transients at StackingManager::LAYER_ACTIVE_TRANSIENT_WINDOW instead
  // of directly above 'win_'.
  void ApplyStackingForAllTransientWindows(
      bool stack_directly_above_toplevel);

  // Choose a new transient window to focus.  We choose the topmost modal
  // window if there is one; otherwise we just return the topmost
  // transient, or NULL if there aren't any transients.
  TransientWindow* FindTransientWindowToFocus() const;

  // Move a transient window to the top of this toplevel's stacking
  // order, if it's not already there.  Updates the transient's position
  // in 'stacked_transients_' and also restacks its composited and client
  // windows.
  void RestackTransientWindowOnTop(TransientWindow* transient);

  // Window object for the toplevel client window.
  Window* win_;  // not owned

  LayoutManager* layout_manager_;  // not owned

  // The invisible input window that represents the client window in
  // overview mode.
  XWindow input_xid_;

  // The state the window is in.  Used to determine how it should be
  // animated by ArrangeFor*() methods.
  State state_;

  // Position, dimensions, and scale that should be used for drawing the
  // window in overview mode.  The X and Y coordinates are relative to
  // the layout manager's origin.
  int overview_x_;
  int overview_y_;
  int overview_width_;
  int overview_height_;
  double overview_scale_;

  // Transient windows belonging to this toplevel window, keyed by XID.
  std::map<XWindow, std::tr1::shared_ptr<TransientWindow> > transients_;

  // Transient windows in top-to-bottom stacking order.
  scoped_ptr<Stacker<TransientWindow*> > stacked_transients_;

  // Transient window that should be focused when TakeFocus() is called,
  // or NULL if the toplevel window should be focused.
  TransientWindow* transient_to_focus_;

  // LayoutManager event registrations for this toplevel window and its
  // input window.
  scoped_ptr<EventConsumerRegistrar> event_consumer_registrar_;

  DISALLOW_COPY_AND_ASSIGN(ToplevelWindow);
};

}  // end namespace window_manager

#endif  // WINDOW_MANAGER_TOPLEVEL_WINDOW_H_
