// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_MOCK_X_CONNECTION_H_
#define WINDOW_MANAGER_MOCK_X_CONNECTION_H_

extern "C" {
#include <X11/Xlib.h>
}
#include <map>
#include <queue>
#include <set>
#include <string>
#include <tr1/memory>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "window_manager/callback.h"
#include "window_manager/x_connection.h"

namespace window_manager {

// This is a fake implementation of a connection to an X server.
class MockXConnection : public XConnection {
 public:
  static const int kDisplayWidth;
  static const int kDisplayHeight;

  MockXConnection();
  ~MockXConnection();

  bool GetWindowGeometry(XWindow xid, WindowGeometry* geom_out);
  bool MapWindow(XWindow xid);
  bool UnmapWindow(XWindow xid);
  bool MoveWindow(XWindow xid, int x, int y);
  bool ResizeWindow(XWindow xid, int width, int height);
  bool ConfigureWindow(XWindow xid, int x, int y, int width, int height) {
    return (MoveWindow(xid, x, y) && ResizeWindow(xid, width, height));
  }
  bool RaiseWindow(XWindow xid);
  bool FocusWindow(XWindow xid, XTime event_time);
  bool StackWindow(XWindow xid, XWindow other, bool above);
  bool ReparentWindow(XWindow xid, XWindow parent, int x, int y) {
    return true;
  }
  bool SetWindowBorderWidth(XWindow xid, int width);
  bool SelectInputOnWindow(XWindow xid, int event_mask, bool preserve_existing);
  bool DeselectInputOnWindow(XWindow xid, int event_mask);
  bool AddButtonGrabOnWindow(
      XWindow xid, int button, int event_mask, bool synchronous);
  bool RemoveButtonGrabOnWindow(XWindow xid, int button);
  bool AddPointerGrabForWindow(XWindow xid, int event_mask, XTime timestamp);
  bool RemovePointerGrab(bool replay_events, XTime timestamp);
  bool RemoveInputRegionFromWindow(XWindow xid) { return true; }
  bool GetSizeHintsForWindow(XWindow xid, SizeHints* hints_out);
  bool GetTransientHintForWindow(XWindow xid, XWindow* owner_out);
  bool GetWindowAttributes(XWindow xid, WindowAttributes* attr_out);
  bool RedirectSubwindowsForCompositing(XWindow xid);
  bool UnredirectWindowForCompositing(XWindow xid);
  XWindow GetCompositingOverlayWindow(XWindow root) { return overlay_; }
  XPixmap GetCompositingPixmapForWindow(XWindow xid);
  bool FreePixmap(XPixmap pixmap) { return true; }
  XWindow GetRootWindow() { return root_; }
  XWindow CreateWindow(XWindow parent, int x, int y, int width, int height,
                       bool override_redirect, bool input_only, int event_mask);
  bool DestroyWindow(XWindow xid);
  bool IsWindowShaped(XWindow xid);
  bool SelectShapeEventsOnWindow(XWindow xid);
  bool GetWindowBoundingRegion(XWindow xid, ByteMap* bytemap);
  bool SelectRandREventsOnWindow(XWindow xid);
  bool GetAtoms(const std::vector<std::string>& names,
                std::vector<XAtom>* atoms_out);
  bool GetAtomName(XAtom atom, std::string* name);
  bool GetIntArrayProperty(XWindow xid, XAtom xatom, std::vector<int>* values);
  bool SetIntArrayProperty(
      XWindow xid, XAtom xatom, XAtom type, const std::vector<int>& values);
  bool GetStringProperty(XWindow xid, XAtom xatom, std::string* out);
  bool SetStringProperty(XWindow xid, XAtom xatom, const std::string& value);
  bool DeletePropertyIfExists(XWindow xid, XAtom xatom);
  int GetConnectionFileDescriptor() { return connection_pipe_fds_[0]; }
  bool IsEventPending();
  void GetNextEvent(void* event);
  bool SendClientMessageEvent(XWindow dest_xid,
                              XWindow xid,
                              XAtom message_type,
                              long data[5],
                              int event_mask);
  bool WaitForWindowToBeDestroyed(XWindow xid) { return true; }
  bool WaitForPropertyChange(XWindow xid, XTime* timestamp_out) { return true; }
  XWindow GetSelectionOwner(XAtom atom);
  bool SetSelectionOwner(XAtom atom, XWindow xid, XTime timestamp);
  bool SetWindowCursor(XWindow xid, uint32 shape);
  bool GetChildWindows(XWindow xid, std::vector<XWindow>* children_out);
  void RefreshKeyboardMap(int request, KeyCode first_keycode, int count) {
    num_keymap_refreshes_++;
  }
  KeySym GetKeySymFromKeyCode(KeyCode keycode);
  KeyCode GetKeyCodeFromKeySym(KeySym keysym);
  std::string GetStringFromKeySym(KeySym keysym) { return ""; }
  bool GrabKey(KeyCode keycode, uint32 modifiers);
  bool UngrabKey(KeyCode keycode, uint32 modifiers);
  XDamage CreateDamage(XDrawable drawable, int level) { return 1; }
  void DestroyDamage(XDamage damage) {}
  void SubtractRegionFromDamage(XDamage damage,
                                XServerRegion repair,
                                XServerRegion parts) {}
  bool SetDetectableKeyboardAutoRepeat(bool detectable) { return true; }
  bool QueryKeyboardState(std::vector<uint8_t>* keycodes_out) { return true; }
  bool QueryPointerPosition(int* x_root, int* y_root);

  // Testing-specific code.
  struct WindowInfo {
    WindowInfo(XWindow xid, XWindow parent);
    ~WindowInfo();

    // Information about a button grab installed on this window.
    struct ButtonGrabInfo {
      ButtonGrabInfo() : event_mask(0), synchronous(false) {}
      ButtonGrabInfo(int event_mask, bool synchronous)
          : event_mask(event_mask),
            synchronous(synchronous) {
      }
      int event_mask;
      bool synchronous;
    };

    // Convenience method to check whether a particular button is grabbed.
    bool button_is_grabbed(int button) {
      return button_grabs.find(button) != button_grabs.end();
    }

    XWindow xid;
    XWindow parent;
    int x, y;
    int width, height;
    int border_width;
    int depth;
    bool mapped;
    bool override_redirect;
    bool input_only;
    bool redirect_subwindows;
    bool redirected;
    int event_mask;
    std::map<XAtom, std::vector<int> > int_properties;
    std::map<XAtom, std::string> string_properties;
    XWindow transient_for;
    uint32 cursor;
    XConnection::SizeHints size_hints;

    // Window's shape, if it's been shaped using the shape extension.
    // NULL otherwise.
    scoped_ptr<ByteMap> shape;

    // Have various extension events been selected using
    // Select*EventsOnWindow()?
    bool shape_events_selected;
    bool randr_events_selected;

    // Client messages sent to the window.
    std::vector<XClientMessageEvent> client_messages;

    // Has the window has been mapped, unmapped, or configured via XConnection
    // methods?  Used to check that changes aren't made to override-redirect
    // windows.
    bool changed;

    // Information about button grabs installed on this window, keyed by
    // button.
    std::map<int, ButtonGrabInfo> button_grabs;

    // XComposite offscreen pixmap with this window's contents.
    XPixmap compositing_pixmap;

   private:
    DISALLOW_COPY_AND_ASSIGN(WindowInfo);
  };

  WindowInfo* GetWindowInfo(XWindow xid);

  WindowInfo* GetWindowInfoOrDie(XWindow xid) {
    WindowInfo* info = GetWindowInfo(xid);
    CHECK(info);
    return info;
  }

  XWindow focused_xid() const { return focused_xid_; }
  XTime last_focus_timestamp() const { return last_focus_timestamp_; }
  XWindow pointer_grab_xid() const { return pointer_grab_xid_; }
  int num_keymap_refreshes() const { return num_keymap_refreshes_; }
  int num_pointer_ungrabs_with_replayed_events() const {
    return num_pointer_ungrabs_with_replayed_events_;
  }

  bool KeyIsGrabbed(KeyCode keycode, uint32 modifiers) {
    return grabbed_keys_.count(std::make_pair(keycode, modifiers)) > 0;
  }

  // Add or remove a two-way mapping between a keycode and a keysym.
  // Keycode-to-keysym mappings are one-to-many within this class.  If a
  // keycode is mapped to multiple keysyms, GetKeySymFromKeyCode() will
  // return the first one that was registered.
  void AddKeyMapping(KeyCode keycode, KeySym keysym);
  void RemoveKeyMapping(KeyCode keycode, KeySym keysym);

  const Stacker<XWindow>& stacked_xids() const {
    return *(stacked_xids_.get());
  }

  // Set the pointer position for QueryPointerPosition().
  void SetPointerPosition(int x, int y) {
    pointer_x_ = x;
    pointer_y_ = y;
  }

  // Set a window as having an active pointer grab.  This is handy when
  // simulating a passive button grab being upgraded due to a button press.
  void set_pointer_grab_xid(XWindow xid) { pointer_grab_xid_ = xid; }

  // Append an event to the queue used by IsEventPending() and
  // GetNextEvent() and optionally write a single byte to
  // 'connection_pipe_fds_' (not writing allows us to simulate the case
  // where Xlib has read the FD itself before we had a chance to see it
  // become ready).
  void AppendEventToQueue(const XEvent& event, bool write_to_fd);

  // Register a callback to be invoked whenever a given property on a given
  // window is changed.  Takes ownership of 'cb'.
  void RegisterPropertyCallback(
      XWindow xid, XAtom xatom, Closure* cb);

  // Helper methods tests can use to initialize events.
  // 'x' and 'y' are relative to the window.
  static void InitButtonEvent(XEvent* event, const WindowInfo& info,
                              int x, int y, int button, bool press);
  static void InitButtonPressEvent(XEvent* event, const WindowInfo& info,
                                   int x, int y, int button) {
    InitButtonEvent(event, info, x, y, button, true);
  }
  static void InitButtonReleaseEvent(XEvent* event, const WindowInfo& info,
                                     int x, int y, int button) {
    InitButtonEvent(event, info, x, y, button, false);
  }
  // This just creates a message with 32-bit values.
  static void InitClientMessageEvent(
      XEvent* event, XWindow xid, XAtom type,
      long arg1, long arg2, long arg3, long arg4, long arg5);
  static void InitConfigureNotifyEvent(XEvent* event, const WindowInfo& info);
  static void InitConfigureRequestEvent(
      XEvent* event, XWindow xid, int x, int y, int width, int height);
  static void InitCreateWindowEvent(XEvent* event, const WindowInfo& info);
  static void InitDestroyWindowEvent(XEvent* event, XWindow xid);
  // 'x' and 'y' are relative to the window.
  static void InitEnterOrLeaveWindowEvent(XEvent* event, const WindowInfo& info,
                                          int x, int y, bool enter);
  static void InitEnterWindowEvent(XEvent* event, const WindowInfo& info,
                                   int x, int y) {
    InitEnterOrLeaveWindowEvent(event, info, x, y, true);
  }
  static void InitLeaveWindowEvent(XEvent* event, const WindowInfo& info,
                                   int x, int y) {
    InitEnterOrLeaveWindowEvent(event, info, x, y, false);
  }
  static void InitMapEvent(XEvent* event, XWindow xid);
  static void InitMapRequestEvent(XEvent* event, const WindowInfo& info);
  static void InitMotionNotifyEvent(XEvent* event, const WindowInfo& info,
                                    int x, int y);
  static void InitPropertyNotifyEvent(XEvent* event, XWindow xid, XAtom xatom);
  static void InitUnmapEvent(XEvent* event, XWindow xid);

 private:
  bool GrabServerImpl() { return true; }
  bool UngrabServerImpl() { return true; }

  std::map<XWindow, std::tr1::shared_ptr<WindowInfo> > windows_;

  // All windows other than the overlay and root, in top-to-bottom stacking
  // order.
  scoped_ptr<Stacker<XWindow> > stacked_xids_;

  XWindow next_window_;

  XWindow root_;
  XWindow overlay_;
  XAtom next_atom_;
  std::map<std::string, XAtom> name_to_atom_;
  std::map<XAtom, std::string> atom_to_name_;
  std::map<XAtom, XWindow> selection_owners_;
  XWindow focused_xid_;

  // Timestamp from the last FocusWindow() invocation.
  XTime last_focus_timestamp_;

  // Window that has currently grabbed the pointer, or None.
  XWindow pointer_grab_xid_;

  // Keys that have been grabbed (pairs are key codes and modifiers).
  std::set<std::pair<KeyCode, uint32> > grabbed_keys_;

  // Mappings from KeyCodes to the corresponding KeySyms and vice versa.
  std::map<KeyCode, std::vector<KeySym> > keycodes_to_keysyms_;
  std::map<KeySym, KeyCode> keysyms_to_keycodes_;

  // Number of times that RefreshKeyboardMap() has been called.
  int num_keymap_refreshes_;

  // Mappings from (window, atom) pairs to callbacks that will be invoked
  // when the corresponding properties are changed.
  std::map<std::pair<XWindow, XAtom>, std::tr1::shared_ptr<Closure> >
      property_callbacks_;

  // Current position of the mouse pointer for QueryPointerPosition().
  int pointer_x_;
  int pointer_y_;

  // Read and write ends of a pipe that we use to simulate events arriving
  // on an X connection.  We don't actually write any events here --
  // rather, we optionally write a single byte when AppendEventToQueue() is
  // called and read the byte if present when GetNextEvent() is called.  We
  // hand out the read end of the pipe in GetConnectionFileDescriptor() so
  // that EventLoop can epoll() on it.
  int connection_pipe_fds_[2];

  // Is there currently a byte written to 'connection_pipe_fds_'?
  bool connection_pipe_has_data_;

  // Event queue used by IsEventPending() and GetNextEvent().
  std::queue<XEvent> queued_events_;

  // The number of times that RemovePointerGrab() has been invoked with
  // 'replay_events' set to true.
  int num_pointer_ungrabs_with_replayed_events_;

  DISALLOW_COPY_AND_ASSIGN(MockXConnection);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_MOCK_X_CONNECTION_H_
