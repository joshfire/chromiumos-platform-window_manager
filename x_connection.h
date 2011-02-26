// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_X_CONNECTION_H_
#define WINDOW_MANAGER_X_CONNECTION_H_

#include <map>
#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/time.h"
#include "window_manager/geometry.h"
#include "window_manager/image_enums.h"
#include "window_manager/x_types.h"

namespace window_manager {

class ByteMap;  // from util.h
struct Rect;
template<class T> class Stacker;  // from util.h

// This is an abstract base class representing a connection to the X
// server.
class XConnection {
 public:
  XConnection()
      : damage_event_base_(0),
        shape_event_base_(0),
        randr_event_base_(0),
        sync_event_base_(0),
        server_grabbed_(false) {
  }
  virtual ~XConnection() {}

  // Data returned by GetWindowGeometry().
  struct WindowGeometry {
    WindowGeometry()
        : bounds(0, 0, 1, 1),
          border_width(0),
          depth(0) {
    }

    Rect bounds;
    int border_width;
    int depth;
  };

  // Data returned by GetSizeHintsForWindow().
  struct SizeHints {
    SizeHints() {
      reset();
    }

    // Reset all of the hints to -1.
    void reset() {
      size.reset(-1, -1);
      min_size.reset(-1, -1);
      max_size.reset(-1, -1);
      size_increment.reset(-1, -1);
      min_aspect_ratio.reset(-1, -1);
      max_aspect_ratio.reset(-1, -1);
      base_size.reset(-1, -1);
      win_gravity = -1;
    }

    // Hints are set to -1 if not defined.
    Size size;
    Size min_size;
    Size max_size;
    Size size_increment;
    Size min_aspect_ratio;
    Size max_aspect_ratio;
    Size base_size;
    int win_gravity;
  };

  // Data returned by GetWindowAttributes().
  struct WindowAttributes {
    enum WindowClass {
      WINDOW_CLASS_INPUT_OUTPUT = 0,
      WINDOW_CLASS_INPUT_ONLY,
    };

    enum MapState {
      MAP_STATE_UNMAPPED = 0,
      MAP_STATE_UNVIEWABLE,
      MAP_STATE_VIEWABLE,
    };

    WindowAttributes()
        : window_class(WINDOW_CLASS_INPUT_OUTPUT),
          map_state(MAP_STATE_UNMAPPED),
          override_redirect(false),
          visual_id(0) {
    }

    WindowClass window_class;
    MapState map_state;
    bool override_redirect;
    XVisualID visual_id;
  };

  // RAII-type object returned by CreateScopedServerGrab() that grabs the
  // X server in its constructor and releases the grab in its destructor.
  class ScopedServerGrab {
   public:
    explicit ScopedServerGrab(XConnection* xconn) : xconn_(xconn) {
      xconn_->GrabServer();
    }

    ~ScopedServerGrab() {
      xconn_->UngrabServer();
    }

   private:
    XConnection* xconn_;

    DISALLOW_COPY_AND_ASSIGN(ScopedServerGrab);
  };

  // RAII-type object that destroys a window in its destructor.
  class WindowDestroyer {
   public:
    WindowDestroyer(XConnection* xconn, XWindow xid)
        : xconn_(xconn),
          xid_(xid) {}
    ~WindowDestroyer() {
      if (xid_) {
        xconn_->DestroyWindow(xid_);
        xid_ = 0;
      }
    }

   private:
    XConnection* xconn_;  // not owned
    XWindow xid_;

    DISALLOW_COPY_AND_ASSIGN(WindowDestroyer);
  };

  // Different ways that damage to a drawable can be reported.
  // The values for these symbols are taken from the Damage wire format
  // (e.g. see damagewire.h in the Xlib Damage implementation).
  enum DamageReportLevel {
    DAMAGE_REPORT_LEVEL_RAW_RECTANGLES = 0,
    DAMAGE_REPORT_LEVEL_DELTA_RECTANGLES = 1,
    DAMAGE_REPORT_LEVEL_BOUNDING_BOX = 2,
    DAMAGE_REPORT_LEVEL_NON_EMPTY = 3,
  };

  // Get the base event ID for extension events.
  int damage_event_base() const { return damage_event_base_; }
  int shape_event_base() const { return shape_event_base_; }
  int randr_event_base() const { return randr_event_base_; }
  int sync_event_base() const { return sync_event_base_; }

  // NOTE: In most cases, the RealXConnection implementations of methods that
  // don't pass any data back via out-params return true without waiting to
  // check for success.  If you depend on knowing whether the request succeeded
  // or failed (failures are common, since clients can destroy windows at any
  // time without any involvement from the window manager), check that the code
  // that you're calling waits for a reply from the X server.

  // Get a window's geometry.
  virtual bool GetWindowGeometry(XWindow xid, WindowGeometry* geom_out) = 0;

  // Map or unmap a window.  MapWindow() returns false if the request fails.
  virtual bool MapWindow(XWindow xid) = 0;
  virtual bool UnmapWindow(XWindow xid) = 0;

  // Move or resize a window.  |width| and |height| must be positive.
  virtual bool MoveWindow(XWindow xid, const Point& pos) = 0;
  virtual bool ResizeWindow(XWindow xid, const Size& size) = 0;
  virtual bool ConfigureWindow(XWindow xid, const Rect& bounds) = 0;

  // Configure a window to be 1x1 and offscreen.
  virtual bool ConfigureWindowOffscreen(XWindow xid) {
    return ConfigureWindow(xid, Rect(-1, -1, 1, 1));
  }

  // Raise a window on top of all other windows.
  virtual bool RaiseWindow(XWindow xid) = 0;

  // Stack a window directly above or below another window.
  virtual bool StackWindow(XWindow xid, XWindow other, bool above) = 0;

  // Give keyboard focus to a window.  |event_time| should be the
  // server-supplied time of the event that caused the window to be
  // focused.
  virtual bool FocusWindow(XWindow xid, XTime event_time) = 0;

  // Reparent a window in another window.
  virtual bool ReparentWindow(XWindow xid,
                              XWindow parent,
                              const Point& offset) = 0;

  // Set the width of a window's border.
  virtual bool SetWindowBorderWidth(XWindow xid, int width) = 0;

  // Select input events on a window.  If |preserve_existing| is true, the
  // existing input selection for the window will be preserved.
  virtual bool SelectInputOnWindow(XWindow xid,
                                   int event_mask,
                                   bool preserve_existing) = 0;

  // Deselect certain input events on a window.
  virtual bool DeselectInputOnWindow(XWindow xid, int event_mask) = 0;

  // Grab the server, preventing other clients from communicating with it.
  // These methods invoke GrabServerImpl() and UngrabServerImpl().
  bool GrabServer();
  bool UngrabServer();

  // Flush any queued requests to the X server.  Note that events are flushed
  // automatically when GetNextEvent() is called.
  virtual void FlushRequests() = 0;

  // Grab the server, returning an object (ownership of which is
  // transferred to the caller) that will ungrab the server when destroyed.
  ScopedServerGrab* CreateScopedServerGrab();

  // Install a passive button grab on a window.  When the specified button
  // is pressed, an active pointer grab will be installed.  Only events
  // matched by |event_mask| will be reported.  If |synchronous| is false,
  // when all of the buttons are released, the pointer grab will be
  // automatically removed.  If |synchronous| is true, no further pointer
  // events will be reported until the the pointer grab is manually removed
  // using UngrabPointer() -- this is useful in conjunction with
  // UngrabPointer()'s |replay_events| parameter to send initial clicks to
  // client apps when implementing click-to-focus behavior.
  virtual bool AddButtonGrabOnWindow(XWindow xid,
                                     int button,
                                     int event_mask,
                                     bool synchronous) = 0;

  // Uninstall a passive button grab.
  virtual bool RemoveButtonGrabOnWindow(XWindow xid, int button) = 0;

  // Grab the pointer asynchronously, such that all subsequent events matching
  // |event_mask| will be reported to the calling client.  If |cursor| is
  // non-zero, it will be displayed for the duration of the grab.  Returns false
  // if an error occurs or if the grab fails (e.g. because it's already grabbed
  // by another client).
  virtual bool GrabPointer(XWindow xid,
                           int event_mask,
                           XTime timestamp,
                           XID cursor) = 0;

  // Remove a pointer grab, possibly also replaying the pointer events that
  // occurred during it if it was synchronous and |replay_events| is true
  // (sending them to the original window instead of just to the grabbing
  // client).
  virtual bool UngrabPointer(bool replay_events, XTime timestamp) = 0;

  // Grab the keyboard asynchronously, such that all subsequent key events will
  // be reported to the calling client.
  virtual bool GrabKeyboard(XWindow xid, XTime timestamp) = 0;

  // Remove the input region from a window, so that events fall through it.
  virtual bool RemoveInputRegionFromWindow(XWindow xid) = 0;

  // Sets the input region for a window so that events outside the region
  // fall through the window.
  virtual bool SetInputRegionForWindow(XWindow xid, const Rect& region) = 0;

  // Get the size hints for a window.
  virtual bool GetSizeHintsForWindow(XWindow xid, SizeHints* hints_out) = 0;

  // Get the transient-for hint for a window.
  virtual bool GetTransientHintForWindow(XWindow xid, XWindow* owner_out) = 0;

  // Get a window's attributes.
  virtual bool GetWindowAttributes(XWindow xid, WindowAttributes* attr_out) = 0;

  // Redirect all of a window's present and future child windows to
  // offscreen pixmaps so they can be composited.
  virtual bool RedirectSubwindowsForCompositing(XWindow xid) = 0;

  // Redirect one window for compositing.
  virtual bool RedirectWindowForCompositing(XWindow xid) = 0;

  // Un-redirect a previously-redirected window.  This is useful when a
  // plugin window gets reparented away from the root and we realize that
  // we won't need to composite it after all.
  virtual bool UnredirectWindowForCompositing(XWindow xid) = 0;

  // Get the overlay window.  (XComposite provides a window that is stacked
  // below the screensaver window but above all other windows).
  virtual XWindow GetCompositingOverlayWindow(XWindow root) = 0;

  // Create a pixmap on the same screen as |drawable|.
  virtual XPixmap CreatePixmap(XDrawable drawable,
                               const Size& size,
                               int depth) = 0;

  // Get a pixmap referring to a redirected window's offscreen storage.
  virtual XPixmap GetCompositingPixmapForWindow(XWindow xid) = 0;

  // Free a pixmap.
  virtual bool FreePixmap(XPixmap pixmap) = 0;

  // Copy an area of one drawable to another drawable.
  virtual void CopyArea(XDrawable src_drawable,
                        XDrawable dest_drawable,
                        const Point& src_pos,
                        const Point& dest_pos,
                        const Size& size) = 0;

  // Get the root window.
  virtual XWindow GetRootWindow() = 0;

  // Create a new window.  The width and height must be positive.
  // |event_mask| determines which events the window receives; it takes
  // values from the "Input Event Masks" section of X.h.  The window is a
  // child of |parent|.  |visual| can be either the ID of the desired
  // visual, or 0 to mean copy-from-parent.
  virtual XWindow CreateWindow(XWindow parent,
                               const Rect& bounds,
                               bool override_redirect,
                               bool input_only,
                               int event_mask,
                               XVisualID visual) = 0;

  // Destroy a window.
  virtual bool DestroyWindow(XWindow xid) = 0;

  // Has a window's bounding region been shaped using the Shape extension?
  virtual bool IsWindowShaped(XWindow xid) = 0;

  // Select ShapeNotify events on a window.
  virtual bool SelectShapeEventsOnWindow(XWindow xid) = 0;

  // Get the rectangles defining a window's bounding region.
  virtual bool GetWindowBoundingRegion(XWindow xid, ByteMap* bytemap) = 0;

  // Set/remove bounding region for a window.
  virtual bool SetWindowBoundingRegionToRect(XWindow xid,
                                             const Rect& region) = 0;
  virtual bool RemoveWindowBoundingRegion(XWindow xid) = 0;

  // Select RandR events on a window.
  virtual bool SelectRandREventsOnWindow(XWindow xid) = 0;

  // Look up the X ID for a single atom, creating it if necessary.
  bool GetAtom(const std::string& name, XAtom* atom_out);

  // Wrapper around GetAtom() that dies if the lookup fails.
  XAtom GetAtomOrDie(const std::string& name);

  // Look up all of the atoms in |names| in the X server, creating them if
  // necessary, and return the corresponding atom X IDs.
  virtual bool GetAtoms(const std::vector<std::string>& names,
                        std::vector<XAtom>* atoms_out) = 0;

  // Get the name of the passed-in atom, saving it to |name|.  Returns
  // false if the atom isn't present in the server.
  virtual bool GetAtomName(XAtom atom, std::string* name) = 0;

  // Get or set a property consisting of a single 32-bit integer.
  // Calls the corresponding abstract {Get,Set}IntArrayProperty() method.
  bool GetIntProperty(XWindow xid, XAtom xatom, int* value);
  bool SetIntProperty(XWindow xid, XAtom xatom, XAtom type, int value);

  // Get or set a property consisting of one or more 32-bit integers.
  virtual bool GetIntArrayProperty(XWindow xid,
                                   XAtom xatom,
                                   std::vector<int>* values) = 0;
  virtual bool SetIntArrayProperty(XWindow xid,
                                   XAtom xatom,
                                   XAtom type,
                                   const std::vector<int>& values) = 0;

  // Get or set a string property (of type STRING or UTF8_STRING when
  // getting and UTF8_STRING when setting).
  virtual bool GetStringProperty(XWindow xid,
                                 XAtom xatom,
                                 std::string* out) = 0;
  virtual bool SetStringProperty(XWindow xid,
                                 XAtom xatom,
                                 const std::string& value) = 0;

  // Delete a property on a window if it exists.
  virtual bool DeletePropertyIfExists(XWindow xid, XAtom xatom) = 0;

  // Get the X connection's file descriptor.
  virtual int GetConnectionFileDescriptor() = 0;

  // Is there an unprocessed event available?
  virtual bool IsEventPending() = 0;

  // Get the next event and remove it from the queue, blocking if one isn't
  // available.  |event| is actually a pointer to an XEvent; we just don't
  // want to include Xlib.h here. :-(
  virtual void GetNextEvent(void* event) = 0;

  // Gets the next event without removing it from the queue, blocking if
  // one isn't available.  |event| is a pointer to an XEvent.
  virtual void PeekNextEvent(void* event) = 0;

  // Send a ClientMessage event with 32-bit data to a window.  If
  // |event_mask| is 0, the event is sent to the client that created
  // |dest_xid|; otherwise the event is sent to all clients selecting any
  // of the event types included in the mask.
  virtual bool SendClientMessageEvent(XWindow dest_xid,
                                      XWindow xid,
                                      XAtom message_type,
                                      long data[5],
                                      int event_mask) = 0;

  // Send a ConfigureNotify event to all clients listening for
  // StructureNotify on a window.  Note that these events will get sent
  // automatically if the window is resized; this method is just useful if
  // a synthetic ConfigureNotify event needs to be sent for some reason
  // (e.g. _NET_WM_SYNC_REQUEST).
  virtual bool SendConfigureNotifyEvent(XWindow xid,
                                        const Rect& bounds,
                                        int border_width,
                                        XWindow above_xid,
                                        bool override_redirect) = 0;

  // Block until |xid| is gone.  (You must select StructureNotify on the
  // window first.)
  virtual bool WaitForWindowToBeDestroyed(XWindow xid) = 0;

  // Wait for the next PropertyNotify event on the passed-in window.  If
  // |timestamp_out| is non-NULL, the timestamp from the event is copied
  // there.
  virtual bool WaitForPropertyChange(XWindow xid, XTime* timestamp_out) = 0;

  // Get the window owning the passed-in selection, or set the owner for a
  // selection.
  virtual XWindow GetSelectionOwner(XAtom atom) = 0;
  virtual bool SetSelectionOwner(XAtom atom, XWindow xid, XTime timestamp) = 0;

  // Get the contents of a drawable.
  // Returns false for unsupported formats or X errors.
  virtual bool GetImage(XID drawable,
                        const Rect& bounds,
                        int drawable_depth,
                        scoped_ptr_malloc<uint8_t>* data_out,
                        ImageFormat* format_out) = 0;

  // Change the cursor for a window.  |shape| is a definition from
  // Xlib's cursorfont.h header.
  virtual bool SetWindowCursor(XWindow xid, XID cursor) = 0;

  // Create a cursor based in a given standard style.  |shape| is a definition
  // from Xlib's cursorfont.h header.
  virtual XID CreateShapedCursor(uint32 shape) = 0;

  // Create a transparent cursor.  Returns 0 on failure.
  virtual XID CreateTransparentCursor() = 0;

  // Free a cursor previously allocated using CreateShapedCursor() or
  // CreateTransparentCursor().
  virtual void FreeCursor(XID cursor) = 0;

  // Get the parent window of |xid|.  Sets |parent_out| to 0 if passed the
  // root window.
  virtual bool GetParentWindow(XWindow xid, XWindow* parent_out) = 0;

  // Get all subwindows of a window in bottom-to-top stacking order.
  virtual bool GetChildWindows(XWindow xid,
                               std::vector<XWindow>* children_out) = 0;

  // Refresh the mapping between keysyms and keycodes.
  // The parameters correspond to the matching fields in the MappingNotify
  // event.
  virtual void RefreshKeyboardMap(int request,
                                  KeyCode first_keycode,
                                  int count) = 0;

  // Convert between keysyms and keycodes.
  virtual KeySym GetKeySymFromKeyCode(KeyCode keycode) = 0;
  virtual KeyCode GetKeyCodeFromKeySym(KeySym keysym) = 0;

  // Get the string representation of a keysym.  Returns the empty string
  // for unknown keysyms.
  virtual std::string GetStringFromKeySym(KeySym keysym) = 0;

  // Grab or ungrab a key combination.
  virtual bool GrabKey(KeyCode keycode, uint32 modifiers) = 0;
  virtual bool UngrabKey(KeyCode keycode, uint32 modifiers) = 0;

  // Manage damage regions.
  virtual XDamage CreateDamage(XDrawable drawable, DamageReportLevel level) = 0;
  virtual void DestroyDamage(XDamage damage) = 0;
  virtual void ClearDamage(XDamage damage) = 0;

  // Set a Sync extension counter to a particular value.
  virtual void SetSyncCounter(XID counter_id, int64_t value) = 0;

  // Create an alarm for a Sync extension counter, such that we'll be
  // notified when the counter reaches |initial_trigger_value|.  Returns
  // the ID of the alarm.
  virtual XID CreateSyncCounterAlarm(XID counter_id,
                                     int64_t initial_trigger_value) = 0;

  // Destroy an alarm for a Sync extension counter.
  virtual void DestroySyncCounterAlarm(XID alarm_id) = 0;

  // When auto-repeating a key combo, the X Server may send:
  //   KeyPress   @ time_0    <-- Key pressed down
  //   KeyRelease @ time_1    <-- First auto-repeat
  //   KeyPress   @ time_1    <-- First auto-repeat, cont.
  //   KeyRelease @ time_2    <-- Key released
  //
  // Calling XkbSetDetectableAutorepeat() changes this behavior for this
  // client only to:
  //   KeyPress   @ time_0    <-- Key pressed down
  //   KeyPress   @ time_1    <-- First auto-repeat
  //   KeyRelease @ time_2    <-- Key released
  //
  // This clears up the problem with mis-reporting an auto-repeat key
  // release as an actual key release (but note also that this was broken
  // for a while in the X.org server but has since been fixed; see
  // http://bugs.freedesktop.org/show_bug.cgi?id=22515).
  virtual bool SetDetectableKeyboardAutoRepeat(bool detectable) = 0;

  // Get the pressed-vs.-not-pressed state of all keys.  |keycodes_out| is
  // a 256-bit vector representing the logical state of the keyboard (read:
  // keycodes, not keysyms), with bits set to 1 for depressed keys.
  virtual bool QueryKeyboardState(std::vector<uint8_t>* keycodes_out) = 0;

  // Helper method to check the state of a given key in
  // QueryKeyboardState()'s output.  Returns true if the key is depressed.
  inline static bool GetKeyCodeState(const std::vector<uint8_t>& states,
                                     KeyCode keycode) {
    return (states[keycode / 8] & (0x1 << (keycode % 8)));
  }

  // Query the pointer's current position relative to the root window.
  virtual bool QueryPointerPosition(Point* absolute_pos_out) = 0;

  // Set the background pixmap of a window.  This is tiled across the window
  // automatically by the server when the window is exposed.  Set to 'None'
  // to disable automatic window-clearing by the server.
  virtual bool SetWindowBackgroundPixmap(XWindow xid, XPixmap pixmap) = 0;

  // Value that should be used in event and property |format| fields for
  // byte and long arguments.
  static const int kByteFormat;
  static const int kLongFormat;

 protected:
  // Base IDs for extension events.  Implementations should initialize
  // these in their constructors.
  int damage_event_base_;
  int shape_event_base_;
  int randr_event_base_;
  int sync_event_base_;

 private:
  virtual bool GrabServerImpl() = 0;
  virtual bool UngrabServerImpl() = 0;

  // Is the server currently grabbed?
  bool server_grabbed_;

  // Time at which the server grab started.
  base::TimeTicks server_grab_time_;

  DISALLOW_COPY_AND_ASSIGN(XConnection);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_X_CONNECTION_H_
