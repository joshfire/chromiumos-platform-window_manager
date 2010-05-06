// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_TOPLEVEL_WINDOW_H_
#define WINDOW_MANAGER_TOPLEVEL_WINDOW_H_

#include <map>

#include "base/scoped_ptr.h"
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

    // We're in overview mode and the window should shrink and fade
    // out and disappear into the appropriate snapshot.
    STATE_OVERVIEW_MODE,

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
  };

  Window* win() { return win_; }

  // Returns the current state of this window.
  State state() const { return state_; }
  static const char* GetStateName(State state);

  // Sets the state of this window, updating the layout to match if
  // the state has changed.
  void SetState(State state);

  // Updates the layout of this window based on its current state.  If
  // animate is true, then animate the window into its new state,
  // otherwise just jump to the new state.
  void UpdateLayout(bool animate);

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

  // Handles changes in the window type properties: the selected tab,
  // or the number of tabs.  Called by the layout manager when it sees
  // that properties have changed.  Returns true if any of the
  // properties that this class is interested in have changed.
  bool PropertiesChanged();

  // The index of the currently selected tab from the window type
  // parameters of the toplevel window.
  int selected_tab() const { return selected_tab_; }

  // The total number of tabs in this toplevel window, from the window
  // type parameters of the toplevel window.
  int tab_count() const { return tab_count_; }

 private:
  // A transient window belonging to a toplevel window.
  // TODO: Make this into a class that uses EventConsumerRegistrar.
  struct TransientWindow {
   public:
    explicit TransientWindow(Window* win)
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

  // Configure the window for active mode.  This involves either
  // moving the client window on- or offscreen (depending on
  // 'window_is_active') and animating the composited window according
  // to 'state_'.
  void ConfigureForActiveMode(bool animate);

  // Configure the window for overview mode.  This involves moving its
  // client window offscreen, and hiding the composite window entirely.
  void ConfigureForOverviewMode(bool animate);

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

  // The state the window is in.  Used to determine how it should be
  // animated by ArrangeFor*() methods.
  State state_;

  // Keeps the previous state.
  State last_state_;

  // Transient windows belonging to this toplevel window, keyed by XID.
  std::map<XWindow, std::tr1::shared_ptr<TransientWindow> > transients_;

  // Transient windows in top-to-bottom stacking order.
  scoped_ptr<Stacker<TransientWindow*> > stacked_transients_;

  // Transient window that should be focused when TakeFocus() is called,
  // or NULL if the toplevel window should be focused.
  TransientWindow* transient_to_focus_;

  // This is the tab index of the currently selected tab in this
  // toplevel window.  It is updated through changes in the window
  // properties.
  int selected_tab_;

  // This is the number of tabs in this toplevel window.
  int tab_count_;

  // LayoutManager event registrations for this toplevel window.
  scoped_ptr<EventConsumerRegistrar> event_consumer_registrar_;

  DISALLOW_COPY_AND_ASSIGN(ToplevelWindow);
};

}  // end namespace window_manager

#endif  // WINDOW_MANAGER_TOPLEVEL_WINDOW_H_
