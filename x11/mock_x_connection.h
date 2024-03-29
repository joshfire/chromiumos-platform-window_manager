// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_X11_MOCK_X_CONNECTION_H_
#define WINDOW_MANAGER_X11_MOCK_X_CONNECTION_H_

#include <map>
#include <queue>
#include <set>
#include <string>
#include <tr1/memory>
#include <utility>
#include <vector>

extern "C" {
#include <X11/Xlib.h>
}

#include "base/logging.h"
#include "window_manager/callback.h"
#include "window_manager/geometry.h"
#include "window_manager/math_types.h"
#include "window_manager/x11/x_connection.h"

namespace window_manager {

// This is a fake implementation of a connection to an X server.
class MockXConnection : public XConnection {
 public:
  static const int kDisplayWidth;
  static const int kDisplayHeight;
  static const XID kTransparentCursor;

  MockXConnection();
  ~MockXConnection();

  // Begin XConnection methods.
  virtual bool GetWindowGeometry(XWindow xid, WindowGeometry* geom_out);
  virtual bool MapWindow(XWindow xid);
  virtual bool UnmapWindow(XWindow xid);
  virtual bool MoveWindow(XWindow xid, const Point& pos);
  virtual bool ResizeWindow(XWindow xid, const Size& size);
  virtual bool ConfigureWindow(XWindow xid, const Rect& bounds) {
    return (MoveWindow(xid, Point(bounds.x, bounds.y)) &&
            ResizeWindow(xid, Size(bounds.width, bounds.height)));
  }
  virtual bool RaiseWindow(XWindow xid);
  virtual bool FocusWindow(XWindow xid, XTime event_time);
  virtual bool StackWindow(XWindow xid, XWindow other, bool above);
  virtual bool ReparentWindow(XWindow xid,
                              XWindow parent,
                              const Point& offset) {
    return true;
  }
  virtual bool SetWindowBorderWidth(XWindow xid, int width);
  virtual bool SelectInputOnWindow(XWindow xid,
                                   int event_mask,
                                   bool preserve_existing);
  virtual bool DeselectInputOnWindow(XWindow xid, int event_mask);
  virtual void FlushRequests() {}
  virtual bool AddButtonGrabOnWindow(XWindow xid,
                                     int button,
                                     int event_mask,
                                     bool synchronous);
  virtual bool RemoveButtonGrabOnWindow(XWindow xid, int button);
  virtual bool GrabPointer(XWindow xid,
                           int event_mask,
                           XTime timestamp,
                           XID cursor);
  virtual bool UngrabPointer(bool replay_events, XTime timestamp);
  virtual bool GrabKeyboard(XWindow xid, XTime timestamp);
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
  virtual XPixmap CreatePixmap(XDrawable drawable, const Size& size, int depth);
  virtual XPixmap GetCompositingPixmapForWindow(XWindow xid);
  virtual bool FreePixmap(XPixmap pixmap);
  virtual void CopyArea(XDrawable src_drawable, XDrawable dest_drawable,
                        const Point& src_pos,
                        const Point& dest_pos,
                        const Size& size) {}
  virtual XWindow GetRootWindow() { return root_; }
  virtual XWindow CreateWindow(XWindow parent,
                               const Rect& bounds,
                               bool override_redirect,
                               bool input_only,
                               int event_mask,
                               XVisualID visual);
  virtual bool DestroyWindow(XWindow xid);
  virtual bool IsWindowShaped(XWindow xid);
  virtual bool SelectShapeEventsOnWindow(XWindow xid);
  virtual bool GetWindowBoundingRegion(XWindow xid, ByteMap* bytemap);
  virtual bool SetWindowBoundingRegionToRect(XWindow xid, const Rect& region);
  virtual bool ResetWindowBoundingRegionToDefault(XWindow xid);
  virtual bool SelectRandREventsOnWindow(XWindow xid);
  virtual bool GetAtoms(const std::vector<std::string>& names,
                        std::vector<XAtom>* atoms_out);
  virtual bool GetAtomName(XAtom atom, std::string* name);
  virtual bool GetIntArrayProperty(XWindow xid,
                                   XAtom xatom,
                                   std::vector<int>* values);
  virtual bool SetIntArrayProperty(XWindow xid,
                                   XAtom xatom,
                                   XAtom type,
                                   const std::vector<int>& values);
  virtual bool GetStringProperty(XWindow xid, XAtom xatom, std::string* out);
  virtual bool SetStringProperty(XWindow xid,
                                 XAtom xatom,
                                 const std::string& value);
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
  virtual bool SendConfigureNotifyEvent(XWindow xid,
                                        const Rect& bounds,
                                        int border_width,
                                        XWindow above_xid,
                                        bool override_redirect);
  virtual bool WaitForWindowToBeDestroyed(XWindow xid) { return true; }
  virtual bool WaitForPropertyChange(XWindow xid, XTime* timestamp_out);
  virtual XWindow GetSelectionOwner(XAtom atom);
  virtual bool SetSelectionOwner(XAtom atom, XWindow xid, XTime timestamp);
  virtual bool GetImage(XID drawable,
                        const Rect& bounds,
                        int drawable_depth,
                        scoped_ptr_malloc<uint8_t>* data_out,
                        ImageFormat* format_out);
  virtual bool SetWindowCursor(XWindow xid, XID cursor);
  virtual XID CreateShapedCursor(uint32 shape) {
    return static_cast<XID>(shape);
  }
  virtual XID CreateTransparentCursor() { return kTransparentCursor; }
  virtual void FreeCursor(XID cursor) {}
  virtual void HideCursor() { cursor_shown_ = false; }
  virtual void ShowCursor() { cursor_shown_ = true; }
  virtual bool GetParentWindow(XWindow xid, XWindow* parent_out);
  virtual bool GetChildWindows(XWindow xid, std::vector<XWindow>* children_out);
  virtual void RefreshKeyboardMap(int request,
                                  KeyCode first_keycode,
                                  int count) {
    num_keymap_refreshes_++;
  }
  virtual KeySym GetKeySymFromKeyCode(KeyCode keycode);
  virtual KeyCode GetKeyCodeFromKeySym(KeySym keysym);
  virtual std::string GetStringFromKeySym(KeySym keysym) { return ""; }
  virtual bool GrabKey(KeyCode keycode, uint32 modifiers);
  virtual bool UngrabKey(KeyCode keycode, uint32 modifiers);
  virtual XDamage CreateDamage(XDrawable drawable, DamageReportLevel level) {
    return next_xid_++;
  }
  virtual void DestroyDamage(XDamage damage) {}
  virtual void ClearDamage(XDamage damage) {}
  virtual void SetSyncCounter(XID counter_id, int64_t value);
  virtual XID CreateSyncCounterAlarm(XID counter_id,
                                     int64_t initial_trigger_value);
  virtual void DestroySyncCounterAlarm(XID alarm_id);
  virtual bool SetDetectableKeyboardAutoRepeat(bool detectable) {
    using_detectable_keyboard_auto_repeat_ = detectable;
    return true;
  }
  virtual bool QueryKeyboardState(std::vector<uint8_t>* keycodes_out) {
    return true;
  }
  virtual bool QueryPointerPosition(Point* absolute_pos_out);
  virtual bool SetWindowBackgroundPixmap(XWindow xid, XPixmap pixmap);
  virtual bool RenderQueryExtension() {
    return true;
  }
  virtual XPixmap CreatePixmapFromContainer(const ImageContainer& container) {
    return 0;
  }
  virtual XPicture RenderCreatePicture(Drawable drawable, int depth) {
    return 0;
  }
  virtual void RenderComposite(bool blend,
                               XPicture src,
                               XPicture mask,
                               XPicture dst,
                               const Point& srcpos,
                               const Point& maskpos,
                               const Matrix4& transform,
                               const Size& size) {}
  virtual bool RenderFreePicture(XPicture pict) {return true;}
  virtual void RenderFillRectangle(XPicture dst,
                                   float red,
                                   float green,
                                   float blue,
                                   const Point& pos,
                                   const Size& size) {}

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
    Rect bounds;
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

    // Cursor assigned to this window via SetWindowCursor().  Note that our
    // implementation of CreateShapedCursor() just casts the shape into an XID,
    // so this will contain the shape that was used in the common case.
    XID cursor;

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

    // Synthetic ConfigureNotify events sent to the window.
    std::vector<XConfigureEvent> configure_notify_events;

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

    // Window background fill pixmap, set by SetWindowBackgroundPixmap()
    XPixmap background_pixmap;

   private:
    DISALLOW_COPY_AND_ASSIGN(WindowInfo);
  };

  struct PixmapInfo {
    PixmapInfo(XWindow xid, const Size& size, int depth);

    XID xid;
    Size size;
    int depth;

   private:
    DISALLOW_COPY_AND_ASSIGN(PixmapInfo);
  };

  struct SyncCounterAlarmInfo {
    SyncCounterAlarmInfo(XID counter_id, int64_t initial_trigger_value)
        : counter_id(counter_id),
          initial_trigger_value(initial_trigger_value) {
    }

    XID counter_id;
    int64_t initial_trigger_value;

   private:
    DISALLOW_COPY_AND_ASSIGN(SyncCounterAlarmInfo);
  };

  WindowInfo* GetWindowInfo(XWindow xid) const;
  WindowInfo* GetWindowInfoOrDie(XWindow xid) const {
    WindowInfo* info = GetWindowInfo(xid);
    CHECK(info);
    return info;
  }

  PixmapInfo* GetPixmapInfo(XPixmap xid) const;
  PixmapInfo* GetPixmapInfoOrDie(XPixmap xid) const {
    PixmapInfo* info = GetPixmapInfo(xid);
    CHECK(info);
    return info;
  }

  SyncCounterAlarmInfo* GetSyncCounterAlarmInfo(XID xid) const;
  SyncCounterAlarmInfo* GetSyncCounterAlarmInfoOrDie(XID xid) const {
    SyncCounterAlarmInfo* info = GetSyncCounterAlarmInfo(xid);
    CHECK(info);
    return info;
  }

  // Get the value currently stored in a Sync extension counter, dying if
  // the counter wasn't created.
  int64_t GetSyncCounterValueOrDie(XID counter_id) const;

  Rect root_bounds() const { return GetWindowInfoOrDie(root_)->bounds; }
  XWindow focused_xid() const { return focused_xid_; }
  XTime last_focus_timestamp() const { return last_focus_timestamp_; }
  XWindow pointer_grab_xid() const { return pointer_grab_xid_; }
  XWindow keyboard_grab_xid() const { return keyboard_grab_xid_; }
  bool cursor_shown() const { return cursor_shown_; }
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
  void SetPointerPosition(const Point& pos) { pointer_pos_ = pos; }

  // Get the window beneath |xid|, or 0 if |xid| is at the bottom.
  XWindow GetWindowBelowWindow(XWindow xid) const;

  // Set a window as having an active pointer grab.  This is handy when
  // simulating a passive button grab being upgraded due to a button press.
  void set_pointer_grab_xid(XWindow xid) { pointer_grab_xid_ = xid; }

  // Set a window as having the keyboard grabbed.
  void set_keyboard_grab_xid(XWindow xid) { keyboard_grab_xid_ = xid; }

  // Append an event to the queue used by IsEventPending() and
  // GetNextEvent() and optionally write a single byte to
  // |connection_pipe_fds_| (not writing allows us to simulate the case
  // where Xlib has read the FD itself before we had a chance to see it
  // become ready).
  void AppendEventToQueue(const XEvent& event, bool write_to_fd);

  // Register a callback to be invoked whenever a given property on a given
  // window is changed.  Takes ownership of |cb|.
  void RegisterPropertyCallback(
      XWindow xid, XAtom xatom, Closure* cb);

  // Helper methods tests can use to initialize events.
  // |x| and |y| are relative to the window.
  void InitButtonEvent(XEvent* event,
                       XWindow xid,
                       const Point& pos,
                       int button,
                       bool press) const;
  void InitButtonPressEvent(XEvent* event,
                            XWindow xid,
                            const Point& pos,
                            int button) const {
    InitButtonEvent(event, xid, pos, button, true);
  }
  void InitButtonReleaseEvent(XEvent* event,
                              XWindow xid,
                              const Point& pos,
                              int button) const {
    InitButtonEvent(event, xid, pos, button, false);
  }
  // |press| is true if this is a key press instead of a key release.
  // |key_mask| can be any combination of: ShiftMask, LockMask,
  // ControlMask, Mod1Mask, Mod2Mask, Mod3Mask, Mod4Mask, and Mod5Mask
  // (where Mod1Mask is the Alt key mask).
  void InitKeyEvent(XEvent* event,
                    XWindow xid,
                    KeyCode key_code,
                    uint32_t modifiers,
                    XTime time,
                    bool press) const;
  void InitKeyPressEvent(XEvent* event,
                         XWindow xid,
                         KeyCode key_code,
                         uint32_t modifiers,
                         XTime time) const {
    InitKeyEvent(event, xid, key_code, modifiers, time, true);
  }
  void InitKeyReleaseEvent(XEvent* event,
                           XWindow xid,
                           KeyCode key_code,
                           uint32_t modifiers,
                           XTime time) const {
    InitKeyEvent(event, xid, key_code, modifiers, time, false);
  }
  // This just creates a message with 32-bit values.
  void InitClientMessageEvent(
      XEvent* event, XWindow xid, XAtom type,
      long arg1, long arg2, long arg3, long arg4, long arg5) const;
  void InitConfigureNotifyEvent(XEvent* event, XWindow xid) const;
  void InitConfigureRequestEvent(
      XEvent* event, XWindow xid, const Rect& bounds) const;
  void InitCreateWindowEvent(XEvent* event, XWindow xid) const;
  void InitDamageNotifyEvent(XEvent* event,
                             XWindow drawable,
                             const Rect& bounds) const;
  void InitDestroyWindowEvent(XEvent* event, XWindow xid) const;
  // |x| and |y| are relative to the window.
  void InitEnterOrLeaveWindowEvent(XEvent* event,
                                   XWindow xid,
                                   const Point& pos,
                                   bool enter) const;
  void InitEnterWindowEvent(XEvent* event,
                            XWindow xid,
                            const Point& pos) const {
    InitEnterOrLeaveWindowEvent(event, xid, pos, true);
  }
  void InitLeaveWindowEvent(XEvent* event,
                            XWindow xid,
                            const Point& pos) const {
    InitEnterOrLeaveWindowEvent(event, xid, pos, false);
  }
  void InitMapEvent(XEvent* event, XWindow xid) const;
  void InitMapRequestEvent(XEvent* event, XWindow xid) const;
  void InitMotionNotifyEvent(XEvent* event,
                             XWindow xid,
                             const Point& pos) const;
  void InitPropertyNotifyEvent(XEvent* event, XWindow xid, XAtom xatom) const;
  void InitSyncAlarmNotifyEvent(
      XEvent* event, XID alarm_xid, int64_t value) const;
  void InitUnmapEvent(XEvent* event, XWindow xid) const;

 private:
  bool GrabServerImpl() { return true; }
  bool UngrabServerImpl() { return true; }

  // Helper method used by GetNextEvent() and PeekNextEvent().
  // Copies the first event in |queued_events_| to |event|, reads from
  // |connection_pipe_fds_| if possible to simulate draining the connection
  // to the X server, and removes the event from |queued_events_| if
  // |remove_from_queue| is true.
  void GetEventInternal(XEvent* event, bool remove_from_queue);

  // Map from IDs to info about the corresponding windows or pixmaps.
  std::map<XWindow, std::tr1::shared_ptr<WindowInfo> > windows_;
  std::map<XPixmap, std::tr1::shared_ptr<PixmapInfo> > pixmaps_;

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

  // Window that has currently grabbed the pointer or keyboard, or 0.
  XWindow pointer_grab_xid_;
  XWindow keyboard_grab_xid_;

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
  Point pointer_pos_;

  // Is the mouse cursor currently shown?
  // true unless HideCursor() has been called.
  bool cursor_shown_;

  // Value set by SetDetectableKeyboardAutoRepeat().
  bool using_detectable_keyboard_auto_repeat_;

  // Read and write ends of a pipe that we use to simulate events arriving
  // on an X connection.  We don't actually write any events here --
  // rather, we optionally write a single byte when AppendEventToQueue() is
  // called and read the byte if present when GetNextEvent() is called.  We
  // hand out the read end of the pipe in GetConnectionFileDescriptor() so
  // that EventLoop can epoll() on it.
  int connection_pipe_fds_[2];

  // Is there currently a byte written to |connection_pipe_fds_|?
  bool connection_pipe_has_data_;

  // Event queue used by IsEventPending() and GetNextEvent().
  std::queue<XEvent> queued_events_;

  // The number of times that UngrabPointer() has been invoked with
  // |replay_events| set to true.
  int num_pointer_ungrabs_with_replayed_events_;

  // IDs and values of Sync extension counters.
  std::map<XID, int64_t> sync_counters_;

  // Alarms that have been registered to watch Sync extension counters.
  std::map<XID, std::tr1::shared_ptr<SyncCounterAlarmInfo> >
      sync_counter_alarms_;

  DISALLOW_COPY_AND_ASSIGN(MockXConnection);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_X11_MOCK_X_CONNECTION_H_
