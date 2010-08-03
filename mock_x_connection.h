// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_MOCK_X_CONNECTION_H_
#define WINDOW_MANAGER_MOCK_X_CONNECTION_H_

#include <map>
#include <queue>
#include <set>
#include <string>
#include <tr1/memory>
#include <utility>
#include <vector>

extern "C" {
#include <X11/extensions/Xdamage.h>
#include <X11/Xlib.h>
}

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

  // Begin XConnection methods.
  virtual bool GetWindowGeometry(XWindow xid, WindowGeometry* geom_out);
  virtual bool MapWindow(XWindow xid);
  virtual bool UnmapWindow(XWindow xid);
  virtual bool MoveWindow(XWindow xid, int x, int y);
  virtual bool ResizeWindow(XWindow xid, int width, int height);
  virtual bool ConfigureWindow(
      XWindow xid, int x, int y, int width, int height) {
    return (MoveWindow(xid, x, y) && ResizeWindow(xid, width, height));
  }
  virtual bool RaiseWindow(XWindow xid);
  virtual bool FocusWindow(XWindow xid, XTime event_time);
  virtual bool StackWindow(XWindow xid, XWindow other, bool above);
  virtual bool ReparentWindow(XWindow xid, XWindow parent, int x, int y) {
    return true;
  }
  virtual bool SetWindowBorderWidth(XWindow xid, int width);
  virtual bool SelectInputOnWindow(
      XWindow xid, int event_mask, bool preserve_existing);
  virtual bool DeselectInputOnWindow(XWindow xid, int event_mask);
  virtual bool AddButtonGrabOnWindow(
      XWindow xid, int button, int event_mask, bool synchronous);
  virtual bool RemoveButtonGrabOnWindow(XWindow xid, int button);
  virtual bool AddPointerGrabForWindow(
      XWindow xid, int event_mask, XTime timestamp);
  virtual bool RemovePointerGrab(bool replay_events, XTime timestamp);
  virtual bool RemoveInputRegionFromWindow(XWindow xid) { return true; }
  virtual bool SetInputRegionForWindow(XWindow xid, const Rect& rect) {
    return true;
  }
  virtual bool GetSizeHintsForWindow(XWindow xid, SizeHints* hints_out);
  virtual bool GetTransientHintForWindow(XWindow xid, XWindow* owner_out);
  virtual bool GetWindowAttributes(XWindow xid, WindowAttributes* attr_out);
  virtual bool RedirectSubwindowsForCompositing(XWindow xid);
  virtual bool RedirectWindowForCompositing(XWindow xid);
  virtual bool UnredirectWindowForCompositing(XWindow xid);
  virtual XWindow GetCompositingOverlayWindow(XWindow root) { return overlay_; }
  virtual XPixmap CreatePixmap(XDrawable drawable,
                               int width, int height,
                               int depth);
  virtual XPixmap GetCompositingPixmapForWindow(XWindow xid);
  virtual bool FreePixmap(XPixmap pixmap);
  virtual void CopyArea(XDrawable src_drawable, XDrawable dest_drawable,
                        int src_x, int src_y,
                        int dest_x, int dest_y,
                        int width, int height) {}
  virtual XWindow GetRootWindow() { return root_; }
  virtual XWindow CreateWindow(
      XWindow parent, int x, int y, int width, int height,
      bool override_redirect, bool input_only, int event_mask,
      XVisualID visual);
  virtual bool DestroyWindow(XWindow xid);
  virtual bool IsWindowShaped(XWindow xid);
  virtual bool SelectShapeEventsOnWindow(XWindow xid);
  virtual bool GetWindowBoundingRegion(XWindow xid, ByteMap* bytemap);
  virtual bool SetWindowBoundingRegionToRect(XWindow xid, const Rect& region);
  virtual bool RemoveWindowBoundingRegion(XWindow xid);
  virtual bool SelectRandREventsOnWindow(XWindow xid);
  virtual bool GetAtoms(const std::vector<std::string>& names,
                        std::vector<XAtom>* atoms_out);
  virtual bool GetAtomName(XAtom atom, std::string* name);
  virtual bool GetIntArrayProperty(
      XWindow xid, XAtom xatom, std::vector<int>* values);
  virtual bool SetIntArrayProperty(
      XWindow xid, XAtom xatom, XAtom type, const std::vector<int>& values);
  virtual bool GetStringProperty(XWindow xid, XAtom xatom, std::string* out);
  virtual bool SetStringProperty(
      XWindow xid, XAtom xatom, const std::string& value);
  virtual bool DeletePropertyIfExists(XWindow xid, XAtom xatom);
  virtual int GetConnectionFileDescriptor() { return connection_pipe_fds_[0]; }
  virtual bool IsEventPending();
  virtual void GetNextEvent(void* event) {
    GetEventInternal(reinterpret_cast<XEvent*>(event), true);
  }
  virtual void PeekNextEvent(void* event) {
    GetEventInternal(reinterpret_cast<XEvent*>(event), false);
  }
  virtual bool SendClientMessageEvent(XWindow dest_xid,
                                      XWindow xid,
                                      XAtom message_type,
                                      long data[5],
                                      int event_mask);
  virtual bool WaitForWindowToBeDestroyed(XWindow xid) { return true; }
  virtual bool WaitForPropertyChange(XWindow xid, XTime* timestamp_out);
  virtual XWindow GetSelectionOwner(XAtom atom);
  virtual bool SetSelectionOwner(XAtom atom, XWindow xid, XTime timestamp);
  virtual bool GetImage(XID drawable, int x, int y,
                        int width, int height, int drawable_depth,
                        scoped_ptr_malloc<uint8_t>* data_out,
                        ImageFormat* format_out);
  virtual bool SetWindowCursor(XWindow xid, uint32 shape);
  virtual bool GetChildWindows(XWindow xid, std::vector<XWindow>* children_out);
  virtual void RefreshKeyboardMap(
      int request, KeyCode first_keycode, int count) {
    num_keymap_refreshes_++;
  }
  virtual KeySym GetKeySymFromKeyCode(KeyCode keycode);
  virtual KeyCode GetKeyCodeFromKeySym(KeySym keysym);
  virtual std::string GetStringFromKeySym(KeySym keysym) { return ""; }
  virtual bool GrabKey(KeyCode keycode, uint32 modifiers);
  virtual bool UngrabKey(KeyCode keycode, uint32 modifiers);
  virtual XDamage CreateDamage(XDrawable drawable, DamageReportLevel level) {
    return 1;
  }
  virtual void DestroyDamage(XDamage damage) {}
  virtual void ClearDamage(XDamage damage) {}
  virtual bool SetDetectableKeyboardAutoRepeat(bool detectable) {
    using_detectable_keyboard_auto_repeat_ = detectable;
    return true;
  }
  virtual bool QueryKeyboardState(std::vector<uint8_t>* keycodes_out) {
    return true;
  }
  virtual bool QueryPointerPosition(int* x_root, int* y_root);
  // End XConnection methods.

  // Testing-specific code.
  struct WindowInfo {
    WindowInfo(XWindow xid, XWindow parent);

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
    XVisualID visual;
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

    // Number of times that the window has been modified using a
    // ConfigureWindow request (that is: move, resize, restack, or border
    // width change).
    int num_configures;

    // Information about button grabs installed on this window, keyed by
    // button.
    std::map<int, ButtonGrabInfo> button_grabs;

   private:
    DISALLOW_COPY_AND_ASSIGN(WindowInfo);
  };

  struct PixmapInfo {
    PixmapInfo(XWindow xid, int width, int height, int depth);

    XID xid;
    int width, height;
    int depth;

   private:
    DISALLOW_COPY_AND_ASSIGN(PixmapInfo);
  };

  WindowInfo* GetWindowInfo(XWindow xid) const;
  WindowInfo* GetWindowInfoOrDie(XWindow xid) const {
    WindowInfo* info = GetWindowInfo(xid);
    CHECK(info);
    return info;
  }

  PixmapInfo* GetPixmapInfo(XID xid) const;
  PixmapInfo* GetPixmapInfoOrDie(XPixmap xid) const {
    PixmapInfo* info = GetPixmapInfo(xid);
    CHECK(info);
    return info;
  }

  XWindow focused_xid() const { return focused_xid_; }
  XTime last_focus_timestamp() const { return last_focus_timestamp_; }
  XWindow pointer_grab_xid() const { return pointer_grab_xid_; }
  int num_keymap_refreshes() const { return num_keymap_refreshes_; }
  bool using_detectable_keyboard_auto_repeat() const {
    return using_detectable_keyboard_auto_repeat_;
  }
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

  // Get the window beneath 'xid', or 0 if 'xid' is at the bottom.
  XWindow GetWindowBelowWindow(XWindow xid) const;

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
  void InitButtonEvent(XEvent* event, XWindow xid,
                       int x, int y, int button, bool press) const;
  void InitButtonPressEvent(XEvent* event, XWindow xid,
                            int x, int y, int button) const {
    InitButtonEvent(event, xid, x, y, button, true);
  }
  void InitButtonReleaseEvent(XEvent* event, XWindow xid,
                              int x, int y, int button) const {
    InitButtonEvent(event, xid, x, y, button, false);
  }
  // |press| is true if this is a key press instead of a key release.
  // |key_mask| can be any combination of: ShiftMask, LockMask,
  // ControlMask, Mod1Mask, Mod2Mask, Mod3Mask, Mod4Mask, and Mod5Mask
  // (where Mod1Mask is the Alt key mask).
  void InitKeyEvent(XEvent* event, XWindow xid,
                    unsigned int keycode,
                    unsigned int key_mask,
                    XTime time,
                    bool press) const;
  void InitKeyPressEvent(XEvent* event, XWindow xid,
                         unsigned int keycode,
                         unsigned int key_mask,
                         XTime time) const {
    InitKeyEvent(event, xid, keycode, key_mask, time, true);
  }
  void InitKeyReleaseEvent(XEvent* event, XWindow xid,
                           unsigned int keycode,
                           unsigned int key_mask,
                           XTime time) const {
    InitKeyEvent(event, xid, keycode, key_mask, time, false);
  }
  // This just creates a message with 32-bit values.
  void InitClientMessageEvent(
      XEvent* event, XWindow xid, XAtom type,
      long arg1, long arg2, long arg3, long arg4, long arg5) const;
  void InitConfigureNotifyEvent(XEvent* event, XWindow xid) const;
  void InitConfigureRequestEvent(
      XEvent* event, XWindow xid, int x, int y, int width, int height) const;
  void InitCreateWindowEvent(XEvent* event, XWindow xid) const;
  void InitDamageNotifyEvent(XEvent* event, XWindow drawable,
                             int x, int y, int width, int height) const;
  void InitDestroyWindowEvent(XEvent* event, XWindow xid) const;
  // 'x' and 'y' are relative to the window.
  void InitEnterOrLeaveWindowEvent(XEvent* event, XWindow xid,
                                   int x, int y, bool enter) const;
  void InitEnterWindowEvent(XEvent* event, XWindow xid,
                            int x, int y) const {
    InitEnterOrLeaveWindowEvent(event, xid, x, y, true);
  }
  void InitLeaveWindowEvent(XEvent* event, XWindow xid,
                            int x, int y) const {
    InitEnterOrLeaveWindowEvent(event, xid, x, y, false);
  }
  void InitMapEvent(XEvent* event, XWindow xid) const;
  void InitMapRequestEvent(XEvent* event, XWindow xid) const;
  void InitMotionNotifyEvent(XEvent* event, XWindow xid, int x, int y) const;
  void InitPropertyNotifyEvent(XEvent* event, XWindow xid, XAtom xatom) const;
  void InitUnmapEvent(XEvent* event, XWindow xid) const;

 private:
  bool GrabServerImpl() { return true; }
  bool UngrabServerImpl() { return true; }

  // Helper method used by GetNextEvent() and PeekNextEvent().
  // Copies the first event in 'queued_events_' to 'event', reads from
  // 'connection_pipe_fds_' if possible to simulate draining the connection
  // to the X server, and removes the event from 'queued_events_' if
  // 'remove_from_queue' is true.
  void GetEventInternal(XEvent* event, bool remove_from_queue);

  // Map from IDs to info about the corresponding windows or pixmaps.
  std::map<XWindow, std::tr1::shared_ptr<WindowInfo> > windows_;
  std::map<XID, std::tr1::shared_ptr<PixmapInfo> > pixmaps_;

  // All windows other than the overlay and root, in top-to-bottom stacking
  // order.
  scoped_ptr<Stacker<XWindow> > stacked_xids_;

  // Next ID that should be used by CreateWindow() or CreatePixmap().
  XWindow next_xid_;

  XWindow root_;
  XWindow overlay_;
  XAtom next_atom_;
  std::map<std::string, XAtom> name_to_atom_;
  std::map<XAtom, std::string> atom_to_name_;
  std::map<XAtom, XWindow> selection_owners_;
  XWindow focused_xid_;

  // Timestamp from the last FocusWindow() invocation.
  XTime last_focus_timestamp_;

  // The "current time" according to this mock server.  This is just
  // incremented by 10 each time WaitForPropertyChange() is called.
  XTime current_time_;

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

  // Value set by SetDetectableKeyboardAutoRepeat().
  bool using_detectable_keyboard_auto_repeat_;

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
