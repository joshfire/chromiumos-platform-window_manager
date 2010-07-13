// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_WINDOW_MANAGER_H_
#define WINDOW_MANAGER_WINDOW_MANAGER_H_

#include <map>
#include <set>
#include <string>
#include <tr1/memory>
#include <utility>
#include <vector>

extern "C" {
// TODO: Move the event-handling methods (all private) into a separate
// header so that these includes can be removed.
#include <X11/extensions/shape.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrandr.h>
#include <X11/Xlib.h>
}

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/scoped_ptr.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"  // for Atom enum
#include "window_manager/compositor.h"
#include "window_manager/panel_manager.h"
#include "window_manager/wm_ipc.h"
#include "window_manager/x_connection.h"
#include "window_manager/x_types.h"

namespace window_manager {

class EventConsumer;
class EventLoop;
class FocusManager;
class HotkeyOverlay;
class KeyBindings;
class KeyBindingsGroup;
class LayoutManager;
class LoginController;
class ScreenLockerHandler;
class StackingManager;
class Window;
class WmIpc;
template<class T> class Stacker;

class WindowManager : public PanelManagerAreaChangeListener {
 public:
  // Visibility groups that actors can be added to.
  // See Compositor::SetActiveVisibilityGroups().
  enum VisibilityGroups {
    VISIBILITY_GROUP_SCREEN_LOCKER = 1,
  };

  WindowManager(EventLoop* event_loop,
                XConnection* xconn,
                Compositor* compositor);
  ~WindowManager();

  void set_initialize_logging(bool should_init) {
    initialize_logging_ = should_init;
  }

  EventLoop* event_loop() { return event_loop_; }
  XConnection* xconn() { return xconn_; }
  Compositor* compositor() { return compositor_; }
  StackingManager* stacking_manager() { return stacking_manager_.get(); }
  FocusManager* focus_manager() { return focus_manager_.get(); }

  XWindow root() const { return root_; }

  Compositor::StageActor* stage() { return stage_; }

  int width() const { return width_; }
  int height() const { return height_; }

  XWindow wm_xid() const { return wm_xid_; }
  XWindow active_window_xid() const { return active_window_xid_; }

  KeyBindings* key_bindings() { return key_bindings_.get(); }
  WmIpc* wm_ipc() { return wm_ipc_.get(); }
  int wm_ipc_version() const { return wm_ipc_version_; }

  bool logged_in() const { return logged_in_; }

  // Get the title for the window that we create to take ownership of management
  // selections.  This is also used to name our log files.
  static const char* GetWmName() { return "chromeos-wm"; }

  // Begin PanelManagerAreaChangeListener implementation.
  virtual void HandlePanelManagerAreaChange();
  // End PanelManagerAreaChangeListener implementation.

  // Perform initial setup.  This must be called immediately after the
  // WindowManager object is created.
  bool Init();

  // Handle notification from Chrome that the logged-in state has changed.
  // 'initial' is true when this method is invoked by Init() and false when
  // it is invoked later in response to a property change.
  void SetLoggedInState(bool logged_in, bool initial);

  // Process all pending events from 'x_conn_', invoking HandleEvent() for each.
  void ProcessPendingEvents();

  // Handle an event from the X server.
  void HandleEvent(XEvent* event);

  // Create a new X window for receiving input.
  XWindow CreateInputWindow(
      int x, int y, int width, int height, int event_mask);

  // Move and resize the passed-in window.
  // TODO: This isn't limited to input windows.
  bool ConfigureInputWindow(XWindow xid, int x, int y, int width, int height);

  // Get the X server's ID corresponding to the passed-in atom (the Atom
  // enum is defined in atom_cache.h).
  XAtom GetXAtom(Atom atom);

  // Get the name for an atom from the X server.
  const std::string& GetXAtomName(XAtom xatom);

  // Get the current time from the server.  This can be useful for e.g.
  // getting a timestamp to pass to XSetInputFocus() when triggered by an
  // event that doesn't contain a timestamp.
  XTime GetCurrentTimeFromServer();

  // Look up a window in 'client_windows_'.  The first version returns NULL
  // if the window doesn't exist, while the second crashes.
  Window* GetWindow(XWindow xid);
  Window* GetWindowOrDie(XWindow xid);

  // Focus a window.  Convenience method that just calls
  // focus_manager_->FocusWindow().
  void FocusWindow(Window* win, XTime timestamp);

  // Do something reasonable with the input focus.
  // This is intended to be called by EventConsumers when they give up the
  // focus and aren't sure what to do with it.
  void TakeFocus(XTime timestamp);

  // Set the _NET_ACTIVE_WINDOW property, which contains the ID of the
  // currently-active window (in our case, this is the toplevel window or
  // panel window that has the focus).
  bool SetActiveWindowProperty(XWindow xid);

  // Set the WM_NAME and NET_WM_NAME properties on a window.
  bool SetNamePropertiesForXid(XWindow xid, const std::string& name);

  // Update the _CHROME_VIDEO_TIME property on the root window, which
  // contains the last time that we believed a video was playing in a
  // window.  Note that updates may be rate-limited (see
  // kVideoTimePropertyUpdateSec).  Returns false if we attempted to set
  // the property but failed and true otherwise.
  bool SetVideoTimeProperty(time_t video_time);

  // Register an event consumer as being interested in non-property-change
  // events on a particular window.
  void RegisterEventConsumerForWindowEvents(
      XWindow xid, EventConsumer* event_consumer);
  void UnregisterEventConsumerForWindowEvents(
      XWindow xid, EventConsumer* event_consumer);

  // Register an event consumer as a listener for changes of a particular
  // property on a particular window.  The consumer's
  // HandleWindowPropertyChange() method will be invoked whenever we
  // receive notification that the property has been changed (after we have
  // already handled the change).
  void RegisterEventConsumerForPropertyChanges(
      XWindow xid, XAtom xatom, EventConsumer* event_consumer);
  void UnregisterEventConsumerForPropertyChanges(
      XWindow xid, XAtom xatom, EventConsumer* event_consumer);

  // Register an event consumer as being interested in a particular type of
  // WmIpc message from Chrome.  The consumer's HandleChromeMessage()
  // method will be passed all messages of this type.
  void RegisterEventConsumerForChromeMessages(
      chromeos::WmIpcMessageType message_type, EventConsumer* event_consumer);
  void UnregisterEventConsumerForChromeMessages(
      chromeos::WmIpcMessageType message_type, EventConsumer* event_consumer);

  bool client_window_debugging_enabled() const {
    return !client_window_debugging_actors_.empty();
  }

  // Callback to show or hide debugging information about client windows.
  void ToggleClientWindowDebugging();

  // Callback to toggle profiler states.
  void ToggleProfiler();

  // Function to update client window debugging info.  Called from the
  // layout manager.
  void UpdateClientWindowDebugging();

  // Get rid of the actor that we display as a background during startup.
  void DropStartupBackground();

  // Get the total number of "real" windows.  Specifically, this is the
  // number of toplevel windows plus the number of panels.
  int GetNumWindows() const;

 private:
  friend class BasicWindowManagerTest;
  friend class LayoutManagerTest;         // uses 'layout_manager_'
  friend class LoginControllerTest;       // uses 'login_controller_'
  friend class PanelTest;                 // uses 'panel_manager_'
  friend class PanelBarTest;              // uses 'panel_manager_'
  friend class PanelDockTest;             // uses 'panel_manager_'
  friend class PanelManagerTest;          // uses 'panel_manager_'
  FRIEND_TEST(LayoutManagerTest, Basic);  // uses TrackWindow()
  FRIEND_TEST(LayoutManagerTest, OverviewSpacing);
  FRIEND_TEST(LayoutManagerTest, InitialWindowStacking);
  FRIEND_TEST(LayoutManagerTest, KeyBindings);
  FRIEND_TEST(LayoutManagerTest, ChangeBackgroundsAfterInitialWindow);
  FRIEND_TEST(WindowTest, TransientFor);  // uses TrackWindow()
  FRIEND_TEST(WindowManagerTest, RegisterExistence);
  FRIEND_TEST(WindowManagerTest, EventConsumer);
  FRIEND_TEST(WindowManagerTest, ResizeScreen);
  FRIEND_TEST(WindowManagerTest, KeepPanelsAfterRestart);
  FRIEND_TEST(WindowManagerTest, LoggedIn);
  FRIEND_TEST(WindowManagerTest, ConfigureBackground);
  FRIEND_TEST(WindowManagerTest, VideoTimeProperty);

  typedef std::map<XWindow, std::set<EventConsumer*> > WindowEventConsumerMap;
  typedef std::map<std::pair<XWindow, XAtom>, std::set<EventConsumer*> >
      PropertyChangeEventConsumerMap;
  typedef std::map<chromeos::WmIpcMessageType, std::set<EventConsumer*> >
      ChromeMessageEventConsumerMap;

  // Minimum number of seconds between updates to the
  // _CHROME_VIDEO_TIME property on the root window.
  static const int kVideoTimePropertyUpdateSec;

  // Is this one of our internally-created windows?
  bool IsInternalWindow(XWindow xid) {
    return (xid == stage_xid_ || xid == overlay_xid_ || xid == wm_xid_);
  }

  // Get a manager selection as described in ICCCM section 2.8.  'atom' is
  // the selection to take, 'manager_win' is the window acquiring the
  // selection, and 'timestamp' is the current time.
  bool GetManagerSelection(
      XAtom atom, XWindow manager_win, XTime timestamp);

  // Tell the previous window and compositing managers to exit and register
  // ourselves as the new managers.
  bool RegisterExistence();

  // Set various one-time/unchanging properties on the root window as
  // specified in the Extended Window Manager Hints.
  bool SetEwmhGeneralProperties();

  // Set EWMH properties on the root window relating to the current screen
  // size (as stored in 'width_' and 'height_'): _NET_DESKTOP_GEOMETRY,
  // _NET_DESKTOP_VIEWPORT, and _NET_WORKAREA (by way of calling
  // SetEwmhWorkareaProperty()).
  bool SetEwmhSizeProperties();

  // Set the _NET_WORKAREA property on the root window to the screen area
  // minus space used by panel docks.
  bool SetEwmhWorkareaProperty();

  // Register all of our key bindings.  Called by Init().
  void RegisterKeyBindings();

  // Query the X server for all toplevel windows and start tracking (and
  // possibly managing) them.
  bool ManageExistingWindows();

  // Start tracking this window (more specifically, create a Window object
  // for it and register it in 'client_windows_').  Returns NULL for
  // windows that we specifically shouldn't track (e.g. the compositor's
  // stage or the compositing overlay window).
  Window* TrackWindow(XWindow xid, bool override_redirect,
                      XConnection::WindowGeometry& geometry);

  // Handle a window getting mapped.  This is primarily used by
  // HandleMapNotify(), but is abstracted out into a separate method so
  // that ManageExistingWindows() can also use it to handle windows that
  // were already mapped when the WM started.
  void HandleMappedWindow(Window* win);

  // Handle the screen being resized.
  void HandleScreenResize(int new_width, int new_height);

  // Set the WM_STATE property on a window.  Per ICCCM 4.1.3.1, 'state' can
  // be 0 (WithdrawnState), 1 (NormalState), or 3 (IconicState).  Per
  // 4.1.4, IconicState means that the toplevel window isn't viewable, so
  // we should use NormalState even when drawing a scaled-down version of
  // the window.
  bool SetWmStateProperty(XWindow xid, int state);

  // Update the _NET_CLIENT_LIST and _NET_CLIENT_LIST_STACKING properties
  // on the root window (as described in EWMH).
  bool UpdateClientListProperty();
  bool UpdateClientListStackingProperty();

  // Handlers for various X events.
  void HandleButtonPress(const XButtonEvent& e);
  void HandleButtonRelease(const XButtonEvent& e);
  void HandleClientMessage(const XClientMessageEvent& e);
  void HandleConfigureNotify(const XConfigureEvent& e);
  void HandleConfigureRequest(const XConfigureRequestEvent& e);
  void HandleCreateNotify(const XCreateWindowEvent& e);
  void HandleDamageNotify(const XDamageNotifyEvent& e);
  void HandleDestroyNotify(const XDestroyWindowEvent& e);
  void HandleEnterNotify(const XEnterWindowEvent& e);
  void HandleKeyPress(const XKeyEvent& e);
  void HandleKeyRelease(const XKeyEvent& e);
  void HandleLeaveNotify(const XLeaveWindowEvent& e);
  void HandleMapNotify(const XMapEvent& e);
  void HandleMapRequest(const XMapRequestEvent& e);
  void HandleMappingNotify(const XMappingEvent& e);
  void HandleMotionNotify(const XMotionEvent& e);
  void HandlePropertyNotify(const XPropertyEvent& e);
  void HandleReparentNotify(const XReparentEvent& e);
  void HandleRRScreenChangeNotify(const XRRScreenChangeNotifyEvent& e);
  void HandleShapeNotify(const XShapeEvent& e);
  void HandleUnmapNotify(const XUnmapEvent& e);

  // Run a command using system().  "&" will be appended to the command to
  // run it in the background.
  void RunCommand(std::string command);

  // Returns arbitrary chrome window. Used when sending a
  // message to Chrome, when particular window does not matter.
  // Returns 0 if there is no Chrome window currently.
  XWindow GetArbitraryChromeWindow();

  // Sends WM_IPC_MESSAGE_CHROME_NOTIFY_SYSKEY_PRESSED notification to Chrome.
  void SendNotifySyskeyMessage(chromeos::WmIpcSystemKey key);

  // Callback to show or hide the hotkey overlay images.
  void ToggleHotkeyOverlay();

  // Write a screenshot to disk.  If 'use_active_window' is true, the
  // screenshot will contain the currently-active client window's offscreen
  // pixmap.  Otherwise, the composited image from the root window will be
  // captured.
  void TakeScreenshot(bool use_active_window);

  // Helper method called repeatedly by a timeout while the hotkey overlay
  // is being displayed to query the current keyboard state from the X
  // server and pass it to the overlay.
  void QueryKeyboardState();

  // Initialize 'startup_background_' to hold a new actor that displays the
  // initial contents of the root window.  Called by Init().
  void CreateInitialBackground();

  // Callback that fades out 'unaccelerated_graphics_actor_'.
  void HideUnacceleratedGraphicsActor();

  EventLoop* event_loop_;   // not owned
  XConnection* xconn_;      // not owned
  Compositor* compositor_;  // not owned

  XWindow root_;

  // Root window dimensions and depth.
  int width_;
  int height_;
  int root_depth_;

  // Offscreen window that we just use for registering as the WM.
  XWindow wm_xid_;

  Compositor::StageActor* stage_;  // not owned

  // If we're started before the user has logged in, this displays the
  // initial contents of the root window.  We use this as a background.
  scoped_ptr<Compositor::TexturePixmapActor> startup_background_;

  // This is the pixmap that gets displayed by 'startup_background_'.
  // We copy the root window here.
  XPixmap startup_pixmap_;

  // Window containing the compositor's stage.
  XWindow stage_xid_;

  // XComposite overlay window.
  XWindow overlay_xid_;

  scoped_ptr<StackingManager> stacking_manager_;
  scoped_ptr<FocusManager> focus_manager_;

  // Windows that are being tracked.
  typedef std::map<XWindow, std::tr1::shared_ptr<Window> > WindowMap;
  WindowMap client_windows_;

  // This is a list of mapped, managed (i.e. not override-redirect) client
  // windows, in most-to-least-recently-mapped order.  Used to set EWMH's
  // _NET_CLIENT_LIST property.
  scoped_ptr<Stacker<XWindow> > mapped_xids_;

  // All immediate children of the root window (even ones that we don't
  // "track", in the sense of having Window objects for them in
  // 'client_windows_') in top-to-bottom stacking order.  EWMH's
  // _NET_CLIENT_LIST_STACKING property contains the managed (i.e. not
  // override-redirect) windows from this list.
  scoped_ptr<Stacker<XWindow> > stacked_xids_;

  // Things that consume events (e.g. LayoutManager, PanelManager, etc.).
  std::set<EventConsumer*> event_consumers_;

  // Map from windows to event consumers that will be notified if events
  // are received.
  WindowEventConsumerMap window_event_consumers_;

  // Map from (window, atom) pairs to event consumers that will be
  // notified if the corresponding property is changed.
  PropertyChangeEventConsumerMap property_change_event_consumers_;

  // Map from Chrome message types to event consumers that will receive
  // copies of the messages.
  ChromeMessageEventConsumerMap chrome_message_event_consumers_;

  // Actors that are currently being used to debug client windows.
  typedef std::vector<std::tr1::shared_ptr<Compositor::Actor> > ActorVector;
  ActorVector client_window_debugging_actors_;

  // The last window that was passed to SetActiveWindowProperty().
  XWindow active_window_xid_;

  scoped_ptr<AtomCache> atom_cache_;
  scoped_ptr<WmIpc> wm_ipc_;
  scoped_ptr<KeyBindings> key_bindings_;
  scoped_ptr<PanelManager> panel_manager_;
  scoped_ptr<LayoutManager> layout_manager_;
  scoped_ptr<LoginController> login_controller_;
  scoped_ptr<ScreenLockerHandler> screen_locker_handler_;

  // ID for the timeout that calls QueryKeyboardState().
  int query_keyboard_state_timeout_id_;

  // Is the hotkey overlay currently being shown?
  bool showing_hotkey_overlay_;

  // Shows overlayed images containing hotkeys.
  scoped_ptr<HotkeyOverlay> hotkey_overlay_;

  // Version of the IPC protocol that Chrome is currently using.  See
  // WM_IPC_MESSAGE_WM_NOTIFY_IPC_VERSION in libcros's
  // chromeos_wm_ipc_enums.h for details.
  int wm_ipc_version_;

  // Key bindings that should only be enabled when a user is logged in (e.g.
  // starting a terminal).
  scoped_ptr<KeyBindingsGroup> logged_in_key_bindings_group_;

  // Has the user logged in yet?  This affects whether some key bindings
  // are enabled or not and determines how new windows are handled.  This
  // tracks the _CHROME_LOGGED_IN property that Chrome sets on the root
  // window.
  bool logged_in_;

  // Should we initialize the logging code when we switch between logged-in
  // and logged-out mode?  This defaults to off, since we typically don't
  // want to write log files when called by tests, but main.cc sets it to
  // true.
  bool initialize_logging_;

  // Last time that we saved to the _CHROME_VIDEO_TIME property.
  time_t last_video_time_;

  // Image that we display onscreen to let the user know when we're not
  // fully hardware-accelerated, or NULL if we are accelerated.
  scoped_ptr<Compositor::ImageActor> unaccelerated_graphics_actor_;

  // ID for the timeout that calls HideUnacceleratedGraphicsActor().
  int hide_unaccelerated_graphics_actor_timeout_id_;

  DISALLOW_COPY_AND_ASSIGN(WindowManager);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_WINDOW_MANAGER_H_
