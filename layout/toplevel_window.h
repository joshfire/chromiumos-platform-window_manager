// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_LAYOUT_TOPLEVEL_WINDOW_H_
#define WINDOW_MANAGER_LAYOUT_TOPLEVEL_WINDOW_H_

#include <map>

#include "base/scoped_ptr.h"
#include "window_manager/layout/layout_manager.h"

namespace window_manager {

class LayoutManager;
class TransientWindowCollection;

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

  bool is_fullscreen() const { return is_fullscreen_; }

  // Returns the current state of this window.
  State state() const { return state_; }
  static const char* GetStateName(State state);

  // Sets the state of this window, updating the layout to match if
  // the state has changed.
  void SetState(State state);

  // Updates the layout of this window based on its current state.  If
  // animate is true, then animate the window from |last_state_| to
  // |state_|; otherwise just jump to the new state.
  void UpdateLayout(bool animate);

  // Focus this window (or maybe one of its transients).
  void TakeFocus(XTime timestamp);

  // Handle the screen being resized, in a limited capacity.  Use
  // UpdateLayout() instead to move and resize the toplevel window if
  // needed; this method just ensures that hidden transient client windows
  // remain offscreen.
  void HandleScreenResize();

  // Try to set the window to be focused the next time that TakeFocus() is
  // called.  NULL can be passed to indicate that the toplevel window
  // should get the focus.
  void SetPreferredTransientWindowToFocus(Window* transient_win);

  // Does the toplevel window or one of its transients have the input focus?
  bool IsWindowOrTransientFocused() const;

  // Handle a transient window that belongs to this toplevel being mapped.
  void HandleTransientWindowMap(Window* transient_win,
                                bool stack_directly_above_toplevel);

  // Handle a transient window that belong to this toplevel being unmapped.
  void HandleTransientWindowUnmap(Window* transient_win);

  // Handle a ConfigureRequest event about one of our transient windows.
  void HandleTransientWindowConfigureRequest(
      Window* transient_win,
      int req_x, int req_y, int req_width, int req_height);

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

  // This tells Chrome via IPC to change the currently selected tab to
  // the given tab.  Keeps a timestamp to avoid a race when handling
  // property changes.  Note that |selected_tab_| will not change to
  // match until we receive the associated property change message
  // from Chrome.
  void SendTabSelectedMessage(int tab_index, XTime timestamp);

  // Fullscreen or unfullscreen this toplevel window.
  void SetFullscreenState(bool fullscreen);

  // Display an animation where the window tries to slide offscreen (to the
  // left if |move_to_left| is true or to the right otherwise) but then
  // bounces back.  Used when the user tries to cycle toplevels while only
  // one toplevel is present.  Only makes sense when in
  // STATE_ACTIVE_MODE_ONSCREEN.
  void DoNudgeAnimation(bool move_to_left);

 private:
  WindowManager* wm() { return layout_manager_->wm_; }

  // Configure the window for active mode.  This involves either
  // moving the client window on- or offscreen (depending on
  // |window_is_active|) and animating the composited window according
  // to |state_|.
  void ConfigureForActiveMode(bool animate);

  // Configure the window for overview mode.  This involves moving its
  // client window offscreen, and hiding the composite window entirely.
  void ConfigureForOverviewMode(bool animate);

  // Window object for the toplevel client window.
  Window* win_;  // not owned

  LayoutManager* layout_manager_;  // not owned

  // The state the window is in.  Used to determine how it should be
  // animated by ArrangeFor*() methods.
  State state_;

  // State in which we were most recently laid out.
  State last_state_;

  // Transient windows belonging to this toplevel window.
  scoped_ptr<TransientWindowCollection> transients_;

  // This is the tab index of the currently selected tab in this
  // toplevel window.  It is updated through changes in the window
  // properties.
  int selected_tab_;

  // This is the number of tabs in this toplevel window.
  int tab_count_;

  // The last time a tab was selected.  Only increases, and is set
  // when we either send a message to Chrome, or when we receive a
  // newer property change message than this from Chrome.  Used to
  // prevent race conditions between Chrome and the window manager
  // when changing tabs.
  XTime last_tab_selected_time_;

  // LayoutManager event registrations for this toplevel window.
  scoped_ptr<EventConsumerRegistrar> event_consumer_registrar_;

  // Is this toplevel window currently fullscreen?
  bool is_fullscreen_;

  DISALLOW_COPY_AND_ASSIGN(ToplevelWindow);
};

}  // end namespace window_manager

#endif  // WINDOW_MANAGER_LAYOUT_TOPLEVEL_WINDOW_H_
