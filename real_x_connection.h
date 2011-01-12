// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_REAL_X_CONNECTION_H_
#define WINDOW_MANAGER_REAL_X_CONNECTION_H_

#include <map>
#include <string>
#include <vector>

extern "C" {
#include <X11/Xlib.h>
#include <X11/Xutil.h>
}
#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro
#include <xcb/xcb.h>

#include "window_manager/geometry.h"
#include "window_manager/image_enums.h"
#include "window_manager/x_connection.h"
#include "window_manager/x_types.h"

namespace window_manager {

typedef ::Display XDisplay;

// This wraps an actual connection to an X server.
class RealXConnection : public XConnection {
 public:
  explicit RealXConnection(XDisplay* display);
  virtual ~RealXConnection();

  // Begin XConnection methods.
  virtual bool GetWindowGeometry(XWindow xid, WindowGeometry* geom_out);
  virtual bool MapWindow(XWindow xid);
  virtual bool UnmapWindow(XWindow xid);
  virtual bool MoveWindow(XWindow xid, const Point& pos);
  virtual bool ResizeWindow(XWindow xid, const Size& size);
  virtual bool ConfigureWindow(XWindow xid, const Rect& bounds);
  virtual bool RaiseWindow(XWindow xid);
  virtual bool FocusWindow(XWindow xid, XTime event_time);
  virtual bool StackWindow(XWindow xid, XWindow other, bool above);
  virtual bool ReparentWindow(XWindow xid, XWindow parent, const Point& offset);
  virtual bool SetWindowBorderWidth(XWindow xid, int width);
  virtual bool SelectInputOnWindow(XWindow xid,
                                   int event_mask,
                                   bool preserve_existing);
  virtual bool DeselectInputOnWindow(XWindow xid, int event_mask);
  virtual void FlushRequests();
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
  virtual bool RemoveInputRegionFromWindow(XWindow xid);
  virtual bool SetInputRegionForWindow(XWindow xid, const Rect& rect);
  virtual bool GetSizeHintsForWindow(XWindow xid, SizeHints* hints_out);
  virtual bool GetTransientHintForWindow(XWindow xid, XWindow* owner_out);
  virtual bool GetWindowAttributes(XWindow xid, WindowAttributes* attr_out);
  virtual bool RedirectSubwindowsForCompositing(XWindow xid);
  virtual bool RedirectWindowForCompositing(XWindow xid);
  virtual bool UnredirectWindowForCompositing(XWindow xid);
  virtual XWindow GetCompositingOverlayWindow(XWindow root);
  virtual XPixmap CreatePixmap(XDrawable drawable,
                               const Size& size,
                               int depth);
  virtual XPixmap GetCompositingPixmapForWindow(XWindow xid);
  virtual bool FreePixmap(XPixmap pixmap);
  virtual void CopyArea(XDrawable src_drawable,
                        XDrawable dest_drawable,
                        const Point& src_pos,
                        const Point& dest_pos,
                        const Size& size);
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
  virtual bool RemoveWindowBoundingRegion(XWindow xid);
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
  virtual int GetConnectionFileDescriptor();
  virtual bool IsEventPending();
  virtual void GetNextEvent(void* event);
  virtual void PeekNextEvent(void* event);
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
  virtual bool WaitForWindowToBeDestroyed(XWindow xid);
  virtual bool WaitForPropertyChange(XWindow xid, XTime* timestamp_out);
  virtual XWindow GetSelectionOwner(XAtom atom);
  virtual bool SetSelectionOwner(XAtom atom, XWindow xid, XTime timestamp);
  virtual bool GetImage(XID drawable,
                        const Rect& bounds,
                        int drawable_depth,
                        scoped_ptr_malloc<uint8_t>* data_out,
                        ImageFormat* format_out);
  virtual bool SetWindowCursor(XWindow xid, XID cursor);
  virtual XID CreateShapedCursor(uint32 shape);
  virtual XID CreateTransparentCursor();
  virtual void FreeCursor(XID cursor);
  virtual bool GetParentWindow(XWindow xid, XWindow* parent_out);
  virtual bool GetChildWindows(XWindow xid, std::vector<XWindow>* children_out);
  virtual void RefreshKeyboardMap(int request,
                                  KeyCode first_keycode,
                                  int count);
  virtual KeySym GetKeySymFromKeyCode(KeyCode keycode);
  virtual KeyCode GetKeyCodeFromKeySym(KeySym keysym);
  virtual std::string GetStringFromKeySym(KeySym keysym);
  virtual bool GrabKey(KeyCode keycode, uint32 modifiers);
  virtual bool UngrabKey(KeyCode keycode, uint32 modifiers);
  virtual XDamage CreateDamage(XDrawable drawable, DamageReportLevel level);
  virtual void DestroyDamage(XDamage damage);
  virtual void ClearDamage(XDamage damage);
  virtual void SetSyncCounter(XID counter_id, int64_t value);
  virtual XID CreateSyncCounterAlarm(XID counter_id,
                                     int64_t initial_trigger_value);
  virtual void DestroySyncCounterAlarm(XID alarm_id);
  virtual bool SetDetectableKeyboardAutoRepeat(bool detectable);
  virtual bool QueryKeyboardState(std::vector<uint8_t>* keycodes_out);
  virtual bool QueryPointerPosition(Point* absolute_pos_out);
  // End XConnection methods.

  // This convenience function is ONLY available for a real X
  // connection.  It is not part of the XConnection interface.  This
  // should not be used by anything other than GLInterface.
  XDisplay* GetDisplay() { return display_; }

  void Free(void* item);

  // Caller assumes ownership of the memory returned from this
  // function which must be freed by calling Free(), above.
  XVisualInfo* GetVisualInfo(long mask,
                             XVisualInfo* visual_template,
                             int* item_count);

  // Sync with the X server and reset our error-tracking state.  This must
  // be followed by a call to UntrapErrors().  Calls to TrapErrors() cannot
  // be nested.
  void TrapErrors();

  // Sync with the server and return the last error code that was received.
  // If no errors were received since the corresponding call to
  // TrapErrors(), returns 0.
  int UntrapErrors();

  // Get the code of the last error since TrapErrors() was called.
  int GetLastErrorCode();

  // Get a string describing an error code.
  std::string GetErrorText(int error_code);

 private:
  FRIEND_TEST(RealXConnectionTest, GetImageFormat);

  // Get an image format using information from an X image.  |lsb_first|
  // should be true if the image data is least-significant-byte first or
  // false if it's MSB-first, |image_depth| is the bits-per-pixel from the
  // image data (only 32 is supported currently), and |drawable_depth| is
  // the drawable's depth (either 32 or 24).  False is returned for
  // unsupported formats.
  static bool GetImageFormat(bool lsb_first,
                             int image_depth,
                             int drawable_depth,
                             ImageFormat* format_out);

  bool GrabServerImpl();
  bool UngrabServerImpl();

  // Ask the server for information about an extension.  Out params may be
  // NULL.  Returns false if the extension isn't present.
  bool QueryExtension(const std::string& name,
                      int* first_event_out,
                      int* first_error_out);

  // Read a property set on a window.  Returns false on error or if the
  // property isn't set.  |format_out| and |type_out| may be NULL.
  bool GetPropertyInternal(XWindow xid,
                           XAtom xatom,
                           std::string* value_out,
                           int* format_out,
                           XAtom* type_out);

  // Check for an error caused by the XCB request using the passed-in
  // cookie.  If found, logs a warning of the form "Got XCB error while
  // [format]", with additional arguments printf-ed into |format|, and
  // returns false.
  bool CheckForXcbError(xcb_void_cookie_t cookie, const char* format, ...);

  // The actual connection to the X server.
  XDisplay* display_;

  // The screen the display is on.
  int screen_;

  // XCB's representation of the connection to the X server.
  xcb_connection_t* xcb_conn_;

  // The root window.
  XWindow root_;

  // ID for the UTF8_STRING atom (we look this up ourselves so as to avoid
  // a circular dependency with AtomCache).
  XAtom utf8_string_atom_;

  DISALLOW_COPY_AND_ASSIGN(RealXConnection);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_REAL_X_CONNECTION_H_
