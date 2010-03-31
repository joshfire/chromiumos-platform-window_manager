// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_LAYOUT_MANAGER_H_
#define WINDOW_MANAGER_LAYOUT_MANAGER_H_

#include <deque>
#include <map>
#include <string>
#include <tr1/memory>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "window_manager/clutter_interface.h"
#include "window_manager/event_consumer.h"
#include "window_manager/key_bindings.h"
#include "window_manager/window.h"
#include "window_manager/wm_ipc.h"  // for WmIpc::Message
#include "window_manager/x_types.h"

namespace chrome_os_pb {
class SystemMetrics;
}

namespace window_manager {

class EventConsumerRegistrar;
class KeyBindingsGroup;
class MotionEventCoalescer;
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
class LayoutManager : public EventConsumer {
 public:
  // 'x', 'y', 'width', and 'height' specify the area available for
  // displaying client windows.  Because of the way that overview mode is
  // currently implemented, this should ideally be flush with the bottom of
  // the screen.
  LayoutManager(WindowManager* wm, int x, int y, int width, int height);
  ~LayoutManager();

  // Struct for keeping track of user metrics relevant to the LayoutManager.
  struct Metrics {
    Metrics()
        : overview_by_keystroke_count(0),
          overview_exit_by_mouse_count(0),
          overview_exit_by_keystroke_count(0),
          window_cycle_by_keystroke_count(0) {
    }

    // Given a metrics protobuffer, populates the applicable fields.
    void Populate(chrome_os_pb::SystemMetrics* metrics_pb);

    void Reset() {
      overview_by_keystroke_count = 0;
      overview_exit_by_mouse_count = 0;
      overview_exit_by_keystroke_count = 0;
      window_cycle_by_keystroke_count = 0;
    }

    int overview_by_keystroke_count;
    int overview_exit_by_mouse_count;
    int overview_exit_by_keystroke_count;
    int window_cycle_by_keystroke_count;
  };

  int x() const { return x_; }
  int y() const { return y_; }
  int width() const { return width_; }
  int height() const { return height_; }
  int overview_panning_offset() const { return overview_panning_offset_; }
  bool key_bindings_enabled() const { return key_bindings_enabled_; }

  // Returns a pointer to the struct in which LayoutManager tracks
  // relevant user metrics.
  Metrics* GetMetrics() { return &metrics_; }

  // Note: Begin EventConsumer implementation.
  bool IsInputWindow(XWindow xid);

  // Handle a window's map request.  In most cases, we just restack the
  // window, move it offscreen, and map it (info bubbles don't get moved,
  // though).
  bool HandleWindowMapRequest(Window* win);

  // Handle a new window.  This method takes care of rearranging windows
  // for the current layout if necessary.
  void HandleWindowMap(Window* win);

  void HandleWindowUnmap(Window* win);
  void HandleWindowConfigureRequest(Window* win,
                                    int req_x, int req_y,
                                    int req_width, int req_height);
  void HandleButtonPress(XWindow xid,
                         int x, int y,
                         int x_root, int y_root,
                         int button,
                         XTime timestamp);
  void HandleButtonRelease(XWindow xid,
                           int x, int y,
                           int x_root, int y_root,
                           int button,
                           XTime timestamp);
  void HandlePointerEnter(XWindow xid,
                          int x, int y,
                          int x_root, int y_root,
                          XTime timestamp);
  void HandlePointerLeave(XWindow xid,
                          int x, int y,
                          int x_root, int y_root,
                          XTime timestamp) {}
  void HandlePointerMotion(XWindow xid,
                           int x, int y,
                           int x_root, int y_root,
                           XTime timestamp);
  void HandleFocusChange(XWindow xid, bool focus_in);
  void HandleChromeMessage(const WmIpc::Message& msg);
  void HandleClientMessage(XWindow xid, XAtom message_type, const long data[5]);
  void HandleWindowPropertyChange(XWindow xid, XAtom xatom) {}
  // Note: End EventConsumer implementation.

  // Return a pointer to an arbitrary Chrome toplevel window, if one
  // exists.  Returns NULL if there is no such window.
  Window* GetChromeWindow();

  // Take the input focus if possible.  Returns 'false' if it doesn't make
  // sense to take the focus (currently, we take the focus if we're in
  // active mode but refuse to in overview mode).
  bool TakeFocus(XTime timestamp);

  // Change the area allocated to the layout manager.
  void MoveAndResize(int x, int y, int width, int height);

  // Enable or disable key bindings.
  void EnableKeyBindings();
  void DisableKeyBindings();

 private:
  FRIEND_TEST(LayoutManagerTest, Basic);  // uses SetMode()
  FRIEND_TEST(LayoutManagerTest, Focus);
  FRIEND_TEST(LayoutManagerTest, FocusTransient);
  FRIEND_TEST(LayoutManagerTest, OverviewFocus);
  FRIEND_TEST(LayoutManagerTest, StackTransientsAbovePanels);
  FRIEND_TEST(LayoutManagerTest, NoDimmingInActiveMode);

  // Animation speed used for windows.
  static const int kWindowAnimMs;

  // Internal private class, declared in toplevel_window.h
  class ToplevelWindow;

  // Is the passed-in window type one that we should handle?
  static bool IsHandledWindowType(WmIpc::WindowType type);

  // Get the toplevel window represented by the passed-in input window, or
  // NULL if the input window doesn't belong to us.
  ToplevelWindow* GetToplevelWindowByInputXid(XWindow xid);

  // Get the 0-based index of the passed-in toplevel within 'toplevels_'.
  // Returns -1 if it isn't present.
  int GetIndexForToplevelWindow(const ToplevelWindow& toplevel) const;

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

  // Get the XID of the input window created for a toplevel window.  This
  // is just used by testing code.
  XWindow GetInputXidForWindow(const Window& win);

  // Do some initial setup for windows that we're going to manage.
  // This includes stacking them and moving them offscreen.
  void DoInitialSetupForWindow(Window* win);

  // Modes used to display windows.
  enum Mode {
    // Display 'active_window_' at full size and let it receive input.
    // Hide all other windows.
    MODE_ACTIVE = 0,

    // Display thumbnails of all of the windows across the bottom of the
    // screen.
    MODE_OVERVIEW,
  };

  // Helper method that activates 'toplevel', using the passed-in
  // states for it and for the previously-active toplevel window.
  // Only has an effect if we're already in active mode.  The two ints
  // must be valid entries in the LayoutManager::ToplevelWindow::State
  // enum.
  void SetActiveToplevelWindow(ToplevelWindow* toplevel,
                               int state_for_new_win,
                               int state_for_old_win);

  // Switch to active mode.  If 'activate_magnified_win' is true and
  // there's a currently-magnified toplevel window, we focus it; otherwise
  // we refocus the previously-focused window).
  void SwitchToActiveMode(bool activate_magnified_win);

  // Activate the toplevel window at the passed-in 0-indexed position (or
  // the last window, for index -1).  Does nothing if no window exists at
  // that position or if we're not already in active mode.
  void ActivateToplevelWindowByIndex(int index);

  // Magnify the toplevel window at the passed-in 0-indexed position (or
  // the last window, for index -1).  Does nothing if no window exists at
  // that position or if not already in overview mode.
  void MagnifyToplevelWindowByIndex(int index);

  // Switch the current mode.
  void SetMode(Mode mode);

  // Calculate toplevel windows' positions and move them there.
  void LayoutToplevelWindowsForActiveMode(bool update_focus);
  // 'magnified_x' is passed to CalculatePositionsForOverviewMode().
  void LayoutToplevelWindowsForOverviewMode(int magnified_x);

  // Calculate the position and scaling of all windows for overview mode
  // and record it in 'toplevels_'.  If 'magnified_x' (given relative to
  // the layout manager's origin) is non-negative,
  // 'overview_panning_offset_' is set such that the magnified window is as
  // close to centered as possible while still being positioned underneath
  // 'magnified_x'.  This is useful for ensuring that the magnified window
  // remains underneath the pointer.
  void CalculatePositionsForOverviewMode(int magnified_x);

  // Configure all toplevel windows for overview mode based on their
  // previously-calculated positions.  'incremental' is passed to
  // ToplevelWindow::ConfigureForOverviewMode().
  void ConfigureWindowsForOverviewMode(bool incremental);

  // Get the toplevel window whose image in overview mode covers the
  // passed-in position, or NULL if no such window exists.
  ToplevelWindow* GetOverviewToplevelWindowAtPoint(int x, int y) const;

  // Add or remove the relevant key bindings for the passed-in mode.
  void AddKeyBindingsForMode(Mode mode);
  void RemoveKeyBindingsForMode(Mode mode);

  // Cycle the active toplevel window.  Only makes sense in active mode.
  void CycleActiveToplevelWindow(bool forward);

  // Cycle the magnified toplevel window.  Only makes sense in overview mode.
  void CycleMagnifiedToplevelWindow(bool forward);

  // Set 'magnified_toplevel_' to the passed-in toplevel window (which can
  // be NULL to disable magnification).
  void SetMagnifiedToplevelWindow(ToplevelWindow* toplevel);

  // Send a message to a window describing the current state of 'mode_'.
  // Does nothing if 'win' isn't a toplevel Chrome window.
  void SendModeMessage(ToplevelWindow* toplevel);

  // Ask the active window to delete itself.
  void SendDeleteRequestToActiveWindow();

  // Pan across the windows horizontally in overview mode.
  // 'offset' is applied relative to the current panning offset.
  void PanOverviewMode(int offset);

  // Update the panning in overview mode based on mouse motion stored in
  // 'overview_background_event_coalescer_'.  Invoked by a timer.
  void UpdateOverviewPanningForMotion();

  // Helper method that enables or disables the key bindings group for the
  // passed-in mode (irrespective of 'key_bindings_enabled_').
  void EnableKeyBindingsForModeInternal(Mode mode);
  void DisableKeyBindingsForModeInternal(Mode mode);

  WindowManager* wm_;  // not owned

  // The current mode.
  Mode mode_;

  // Area available to us for placing windows.
  int x_;
  int y_;
  int width_;
  int height_;

  // Information about toplevel windows, stored in the order in which
  // we'll display them in overview mode.
  typedef std::deque<std::tr1::shared_ptr<ToplevelWindow> > ToplevelWindows;
  ToplevelWindows toplevels_;

  // Map from input windows to the toplevel windows they represent.
  std::map<XWindow, ToplevelWindow*> input_to_toplevel_;

  // Map from transient windows' XIDs to the toplevel windows that own
  // them.  This is based on the transient windows' WM_TRANSIENT_FOR hints
  // at the time that they were mapped; we ignore any subsequent changes to
  // this hint.
  std::map<XWindow, ToplevelWindow*> transient_to_toplevel_;

  // Currently-magnified toplevel window in overview mode, or NULL if no
  // window is magnified.
  ToplevelWindow* magnified_toplevel_;

  // Currently-active toplevel window in active mode.
  ToplevelWindow* active_toplevel_;

  // Window that when clicked creates a new browser.  Only shown in
  // overview mode, and may be NULL.
  Window* create_browser_window_;

  // Amount that toplevel windows' positions should be offset to the left
  // for overview mode.  Used to implement panning.
  int overview_panning_offset_;

  // Mouse pointer motion gets stored here during a drag on the background
  // window in overview mode so that it can be applied periodically in
  // UpdateOverviewPanningForMotion().
  scoped_ptr<MotionEventCoalescer> overview_background_event_coalescer_;

  // X component of the pointer's previous position during a drag on the
  // background window.
  int overview_drag_last_x_;

  Metrics metrics_;

  // Have we seen a MapRequest event yet?  We perform some initial setup
  // (e.g. stacking) in response to MapRequests, so we track this so we can
  // perform the same setup at the MapNotify point for windows that were
  // already mapped or were in the process of being mapped when we were
  // started.
  // TODO: This is yet another hack that could probably removed in favor of
  // something more elegant if/when we're sharing an X connection with
  // Clutter and can safely grab the server at startup.
  bool saw_map_request_;

  // Event registrations for the layout manager itself.
  scoped_ptr<EventConsumerRegistrar> event_consumer_registrar_;

  // Are key bindings currently enabled?
  bool key_bindings_enabled_;

  // Groups of key bindings that are relevant to different modes.
  scoped_ptr<KeyBindingsGroup> active_mode_key_bindings_group_;
  scoped_ptr<KeyBindingsGroup> overview_mode_key_bindings_group_;

  DISALLOW_COPY_AND_ASSIGN(LayoutManager);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_LAYOUT_MANAGER_H_
