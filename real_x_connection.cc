// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/real_x_connection.h"

extern "C" {
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/xfixes.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xdamage.h>
#include <X11/Xatom.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
}

#include "base/string_util.h"
#include "window_manager/geometry.h"
#include "window_manager/util.h"

using std::map;
using std::string;
using std::vector;
using window_manager::util::FindWithDefault;
using window_manager::util::XidStr;

namespace window_manager {

// Used by RealXConnection's constructor to negotiate the version of an X
// extension that we'll be using with the X server.  'name' is the
// extension's name as it appears in XCB, i.e. "damage" for
// xcb_damage_query_version(), and 'major' and 'minor' specify the minimum
// version of the extension to be accepted.
#define INIT_XCB_EXTENSION(name, major, minor)                                 \
  do {                                                                         \
    xcb_##name##_query_version_cookie_t cookie =                               \
        xcb_##name##_query_version(xcb_conn_, major, minor);                   \
    xcb_generic_error_t* error = NULL;                                         \
    scoped_ptr_malloc<xcb_##name##_query_version_reply_t> reply(               \
        xcb_##name##_query_version_reply(xcb_conn_, cookie, &error));          \
    scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);                \
    CHECK(!error) << "Unable to query " #name " extension";                    \
    LOG(INFO) << "Server has " #name " extension v"                            \
              << reply->major_version << "." << reply->minor_version;          \
    CHECK(reply->major_version >= major);                                      \
    if (reply->major_version == major)                                         \
      CHECK(reply->minor_version >= minor);                                    \
  } while (0)

// Maximum property size in bytes (both for reading and setting).
static const size_t kMaxPropertySize = 1024;

// Xlib error handler that was originally installed.
static int (*old_error_handler)(XDisplay*, XErrorEvent*) = NULL;

// Are we currently trapping errors?  Set by TrapErrors() and cleared by
// UntrapErrors().  Note that we always catch errors instead of letting
// them fall through to Xlib's default handler; this is just used to
// (sometimes) match errors with the requests that generated them.  We just
// use this variable to catch places where TrapErrors() is incorrectly
// called twice in a row.
static bool trapping_errors = false;

// Information about the last error that HandleXError() received.
static int last_error_code = 0;
static int last_error_request_major_opcode = 0;
static int last_error_request_minor_opcode = 0;

static int HandleXError(XDisplay* display, XErrorEvent* event) {
  last_error_code = event->error_code;
  last_error_request_major_opcode = event->request_code;
  last_error_request_minor_opcode = event->minor_code;
  char error_description[256] = "";
  XGetErrorText(display, event->error_code,
                error_description, sizeof(error_description));
  LOG(WARNING) << "Handled X error on display " << display << ":"
               << " error=" << last_error_code
               << " (" << error_description << ")"
               << " major=" << last_error_request_major_opcode
               << " minor=" << last_error_request_minor_opcode;
  return 0;
}

RealXConnection::RealXConnection(XDisplay* display)
    : display_(display),
      xcb_conn_(NULL),
      root_(XCB_NONE),
      utf8_string_atom_(XCB_NONE) {
  CHECK(display_);

  // Install our own Xlib error handler to avoid crashing (the default
  // behavior when Xlib sees an error in the event queue).
  old_error_handler = XSetErrorHandler(&HandleXError);

  xcb_conn_ = XGetXCBConnection(display_);
  CHECK(xcb_conn_) "Couldn't get XCB connection from Xlib display";

  // TODO: Maybe handle multiple screens later, but we just use the default
  // one for now.
  root_ = DefaultRootWindow(display_);
  CHECK(GetAtom("UTF8_STRING", &utf8_string_atom_));

  CHECK(QueryExtension("SHAPE", &shape_event_base_, NULL));
  CHECK(QueryExtension("RANDR", &randr_event_base_, NULL));
  CHECK(QueryExtension("Composite", NULL, NULL));
  CHECK(QueryExtension("DAMAGE", &damage_event_base_, NULL));
  CHECK(QueryExtension("XFIXES", NULL, NULL));

  // The shape extension's XCB interface is different; it doesn't take a
  // version number.  The extension is ancient and doesn't require that we
  // tell the server which version we support, though, so just skip it.
  INIT_XCB_EXTENSION(randr, 1, 2);
  INIT_XCB_EXTENSION(composite, 0, 4);
  INIT_XCB_EXTENSION(damage, 1, 1);
  INIT_XCB_EXTENSION(xfixes, 4, 0);
}

RealXConnection::~RealXConnection() {
  for (map<uint32, xcb_cursor_t>::const_iterator it = cursors_.begin();
       it != cursors_.end(); ++it) {
    xcb_free_cursor(xcb_conn_, it->second);
  }
  CHECK(XSetErrorHandler(old_error_handler) == &HandleXError)
      << "Our error handler was replaced with someone else's";
}

bool RealXConnection::GetWindowGeometry(XDrawable xid,
                                        WindowGeometry* geom_out) {
  CHECK(geom_out);
  xcb_get_geometry_cookie_t cookie = xcb_get_geometry(xcb_conn_, xid);
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_get_geometry_reply_t> reply(
      xcb_get_geometry_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Got X error while getting geometry for drawable "
                 << XidStr(xid);
    return false;
  }

  geom_out->x = reply->x;
  geom_out->y = reply->y;
  geom_out->width = reply->width;
  geom_out->height = reply->height;
  geom_out->border_width = reply->border_width;
  geom_out->depth = reply->depth;
  return true;
}

bool RealXConnection::MapWindow(XWindow xid) {
  xcb_map_window(xcb_conn_, xid);
  return true;
}

bool RealXConnection::UnmapWindow(XWindow xid) {
  xcb_unmap_window(xcb_conn_, xid);
  return true;
}

bool RealXConnection::MoveWindow(XWindow xid, int x, int y) {
  const uint32_t values[] = { x, y };
  xcb_configure_window(xcb_conn_, xid,
                       XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                       values);
  return true;
}

bool RealXConnection::ResizeWindow(XWindow xid, int width, int height) {
  const uint32_t values[] = { width, height };
  xcb_configure_window(xcb_conn_, xid,
                       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                       values);
  return true;
}

bool RealXConnection::ConfigureWindow(
    XWindow xid, int x, int y, int width, int height) {
  const uint32_t values[] = { x, y, width, height };
  xcb_configure_window(xcb_conn_, xid,
                       XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                       values);
  return true;
}

bool RealXConnection::RaiseWindow(XWindow xid) {
  static const uint32_t values[] = { XCB_STACK_MODE_ABOVE };
  xcb_configure_window(xcb_conn_, xid, XCB_CONFIG_WINDOW_STACK_MODE, values);
  return true;
}

bool RealXConnection::FocusWindow(XWindow xid, XTime event_time) {
  DLOG(INFO) << "Focusing window " << XidStr(xid);
  xcb_set_input_focus(xcb_conn_, XCB_INPUT_FOCUS_PARENT, xid, event_time);
  return true;
}

bool RealXConnection::StackWindow(XWindow xid, XWindow other, bool above) {
  const uint32_t values[] =
      { other, above ? XCB_STACK_MODE_ABOVE : XCB_STACK_MODE_BELOW };
  xcb_configure_window(xcb_conn_, xid,
                       XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
                       values);
  return true;
}

bool RealXConnection::ReparentWindow(
    XWindow xid, XWindow parent, int x, int y) {
  xcb_reparent_window(xcb_conn_, xid, parent, x, y);
  return true;
}

bool RealXConnection::SetWindowBorderWidth(XWindow xid, int width) {
  DCHECK_GE(width, 0);
  const uint32_t values[] = { width };
  xcb_configure_window(xcb_conn_, xid, XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
  return true;
}

// TODO: Figure out why a naive translation of this to XCB doesn't work
// (the window manager seems to behave as if the initial
// SubstructureRedirect doesn't go through).
bool RealXConnection::SelectInputOnWindow(
    XWindow xid, int event_mask, bool preserve_existing) {
  TrapErrors();
  if (preserve_existing) {
    XWindowAttributes attr;
    XGetWindowAttributes(display_, xid, &attr);
    event_mask |= attr.your_event_mask;
  }
  if (!GetLastErrorCode()) {
    // Only select the new mask if we were successful in fetching the
    // previous one to avoid blowing away the previous mask on failure.
    XSelectInput(display_, xid, event_mask);
  }
  if (int error = UntrapErrors()) {
    LOG(WARNING) << "Got X error while selecting input on window "
                 << XidStr(xid) << ": " << GetErrorText(error);
    return false;
  }
  return true;
}

// TODO: Delete this when hotkey overlay is implemented with polling.
bool RealXConnection::DeselectInputOnWindow(XWindow xid, int event_mask) {
  TrapErrors();
  XWindowAttributes attr;
  XGetWindowAttributes(display_, xid, &attr);
  attr.your_event_mask &= ~event_mask;
  if (!GetLastErrorCode()) {
    // Only select the new mask if we were successful in fetching the
    // previous one to avoid blowing away the previous mask on failure.
    XSelectInput(display_, xid, attr.your_event_mask);
  }
  if (int error = UntrapErrors()) {
    LOG(WARNING) << "Got X error while deselecting input on window "
                 << XidStr(xid) << ": " << GetErrorText(error);
    return false;
  }
  return true;
}

bool RealXConnection::AddButtonGrabOnWindow(
    XWindow xid, int button, int event_mask, bool synchronous) {
  xcb_grab_button(xcb_conn_,
                  0,                    // owner_events
                  xid,
                  event_mask,
                  synchronous ?         // pointer mode
                    XCB_GRAB_MODE_SYNC :
                    XCB_GRAB_MODE_ASYNC,
                  XCB_GRAB_MODE_ASYNC,  // keyboard_mode
                  XCB_NONE,             // confine_to
                  XCB_NONE,             // cursor
                  button,
                  XCB_NONE);            // modifiers
  return true;
}

bool RealXConnection::RemoveButtonGrabOnWindow(XWindow xid, int button) {
  xcb_ungrab_button(xcb_conn_, button, xid, XCB_NONE);
  return true;
}

bool RealXConnection::AddPointerGrabForWindow(XWindow xid,
                                              int event_mask,
                                              XTime timestamp) {
  xcb_grab_pointer_cookie_t cookie =
      xcb_grab_pointer(xcb_conn_,
                       0,                    // owner_events
                       xid,
                       event_mask,
                       XCB_GRAB_MODE_ASYNC,  // pointer_mode
                       XCB_GRAB_MODE_ASYNC,  // keyboard_mode
                       XCB_NONE,             // confine_to
                       XCB_NONE,             // cursor
                       timestamp);
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_grab_pointer_reply_t> reply(
      xcb_grab_pointer_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Pointer grab for window " << XidStr(xid) << " failed";
    return false;
  }

  if (reply->status != XCB_GRAB_STATUS_SUCCESS) {
    LOG(WARNING) << "Pointer grab for window " << XidStr(xid)
                 << " returned status " << reply->status;
    return false;
  }
  return true;
}

bool RealXConnection::RemovePointerGrab(bool replay_events, XTime timestamp) {
  if (replay_events)
    xcb_allow_events(xcb_conn_, XCB_ALLOW_REPLAY_POINTER, timestamp);
  else
    xcb_ungrab_pointer(xcb_conn_, timestamp);
  return true;
}

bool RealXConnection::RemoveInputRegionFromWindow(XWindow xid) {
  xcb_shape_rectangles(xcb_conn_,
                       XCB_SHAPE_SO_SET,
                       XCB_SHAPE_SK_INPUT,
                       0,      // ordering
                       xid,
                       0,      // x_offset
                       0,      // y_offset
                       0,      // rectangles_len
                       NULL);  // rectangles
  return true;
}

bool RealXConnection::SetInputRegionForWindow(XWindow xid,
                                              const Rect& region) {
  xcb_rectangle_t x_rectangle;
  x_rectangle.x = region.x;
  x_rectangle.y = region.y;
  x_rectangle.width = region.width;
  x_rectangle.height = region.height;
  xcb_shape_rectangles(xcb_conn_,
                       XCB_SHAPE_SO_SET,
                       XCB_SHAPE_SK_INPUT,
                       0,              // ordering
                       xid,
                       0,              // x_offset
                       0,              // y_offset
                       1,              // rectangles_len
                       &x_rectangle);  // rectangles
  return true;
}

bool RealXConnection::GetSizeHintsForWindow(XWindow xid, SizeHints* hints_out) {
  CHECK(hints_out);
  hints_out->reset();

  vector<int> values;
  if (!GetIntArrayProperty(xid, XA_WM_NORMAL_HINTS, &values))
    return false;

  // Contents of the WM_NORMAL_HINTS property (15-18 32-bit values):
  // Note that http://tronche.com/gui/x/icccm/sec-4.html#s-4.1.2.3 is
  // completely wrong. :-(
  //
  // Index  Field         Type    Comments
  // -----  -----         ----    --------
  //   0    flags         CARD32
  //   1    x             INT32   deprecated
  //   2    y             INT32   deprecated
  //   3    width         INT32   deprecated
  //   4    height        INT32   deprecated
  //   5    min_width     INT32
  //   6    min_height    INT32
  //   7    max_width     INT32
  //   8    max_height    INT32
  //   9    width_inc     INT32
  //  10    height_inc    INT32
  //  11    min_aspect_x  INT32
  //  12    min_aspect_y  INT32
  //  13    max_aspect_x  INT32
  //  14    max_aspect_y  INT32
  //  15    base_width    INT32   optional
  //  16    base_height   INT32   optional
  //  17    win_gravity   CARD32  optional

  if (values.size() < 15) {
    LOG(WARNING) << "Got WM_NORMAL_HINTS property for " << XidStr(xid)
                 << " with " << values.size() << " value"
                 << (values.size() != 1 ? "s" : "")
                 << " (expected at least 15)";
    return false;
  }

  uint32_t flags = values[0];

  if ((flags & USSize) || (flags & PSize)) {
    hints_out->width = values[3];
    hints_out->height = values[4];
  }
  if (flags & PMinSize) {
    hints_out->min_width = values[5];
    hints_out->min_height = values[6];
  }
  if (flags & PMaxSize) {
    hints_out->max_width = values[7];
    hints_out->max_height = values[8];
  }
  if (flags & PResizeInc) {
    hints_out->width_increment = values[9];
    hints_out->height_increment = values[10];
  }
  if (flags & PAspect) {
    hints_out->min_aspect_x = values[11];
    hints_out->min_aspect_y = values[12];
    hints_out->max_aspect_x = values[13];
    hints_out->max_aspect_y = values[14];
  }
  if ((flags & PBaseSize) && values.size() >= 17) {
    hints_out->base_width = values[15];
    hints_out->base_height = values[16];
  }
  if ((flags & PWinGravity) && values.size() >= 18) {
    hints_out->win_gravity = values[17];
  }

  return true;
}

bool RealXConnection::GetTransientHintForWindow(
    XWindow xid, XWindow* owner_out) {
  int owner = XCB_NONE;
  if (!GetIntProperty(xid, XA_WM_TRANSIENT_FOR, &owner))
    return false;
  *owner_out = static_cast<XWindow>(owner);
  return true;
}

bool RealXConnection::GetWindowAttributes(
    XWindow xid, WindowAttributes* attr_out) {
  CHECK(attr_out);

  xcb_get_window_attributes_cookie_t cookie =
      xcb_get_window_attributes(xcb_conn_, xid);
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_get_window_attributes_reply_t> reply(
      xcb_get_window_attributes_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Getting attributes for window " << XidStr(xid)
                 << " failed";
    return false;
  }

  switch (reply->_class) {
    case XCB_WINDOW_CLASS_INPUT_OUTPUT:
      attr_out->window_class = WindowAttributes::WINDOW_CLASS_INPUT_OUTPUT;
      break;
    case XCB_WINDOW_CLASS_INPUT_ONLY:
      attr_out->window_class = WindowAttributes::WINDOW_CLASS_INPUT_ONLY;
      break;
    default:
      NOTREACHED() << "Invalid class " << reply->_class << " for window "
                   << XidStr(xid);
  }
  switch (reply->map_state) {
    case XCB_MAP_STATE_UNMAPPED:
      attr_out->map_state = WindowAttributes::MAP_STATE_UNMAPPED;
      break;
    case XCB_MAP_STATE_UNVIEWABLE:
      attr_out->map_state = WindowAttributes::MAP_STATE_UNVIEWABLE;
      break;
    case XCB_MAP_STATE_VIEWABLE:
      attr_out->map_state = WindowAttributes::MAP_STATE_VIEWABLE;
      break;
    default:
      NOTREACHED() << "Invalid map state " << reply->map_state << " for window "
                   << XidStr(xid);
  }
  attr_out->override_redirect = (reply->override_redirect != 0);
  attr_out->visual_id = reply->visual;
  return true;
}

bool RealXConnection::RedirectSubwindowsForCompositing(XWindow xid) {
  xcb_composite_redirect_subwindows(
      xcb_conn_, xid, XCB_COMPOSITE_REDIRECT_MANUAL);
  return true;
}

bool RealXConnection::UnredirectWindowForCompositing(XWindow xid) {
  xcb_composite_unredirect_window(
      xcb_conn_, xid, XCB_COMPOSITE_REDIRECT_MANUAL);
  return true;
}

XWindow RealXConnection::GetCompositingOverlayWindow(XWindow root) {
  xcb_composite_get_overlay_window_cookie_t cookie =
      xcb_composite_get_overlay_window(xcb_conn_, root);
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_composite_get_overlay_window_reply_t> reply(
      xcb_composite_get_overlay_window_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Got X error while getting overlay window";
    return XCB_NONE;
  }
  return reply->overlay_win;
}

XPixmap RealXConnection::CreatePixmap(XDrawable drawable,
                                      int width, int height,
                                      int depth) {
  xcb_pixmap_t pixmap = xcb_generate_id(xcb_conn_);
  xcb_create_pixmap(xcb_conn_, depth, pixmap, drawable, width, height);
  return pixmap;
}

XPixmap RealXConnection::GetCompositingPixmapForWindow(XWindow xid) {
  const xcb_pixmap_t pixmap = xcb_generate_id(xcb_conn_);
  xcb_void_cookie_t cookie = xcb_composite_name_window_pixmap_checked(
      xcb_conn_, xid, pixmap);
  CheckForXcbError(cookie, "in GetCompositingPixmapForWindow "
                   "(xid=0x%08x, pixmap=0x%08x)", static_cast<int>(xid),
                   static_cast<int>(pixmap));
  return pixmap;
}

bool RealXConnection::FreePixmap(XPixmap pixmap) {
  xcb_free_pixmap(xcb_conn_, pixmap);
  return true;
}

void RealXConnection::CopyArea(XDrawable src_drawable, XDrawable dest_drawable,
                               int src_x, int src_y,
                               int dest_x, int dest_y,
                               int width, int height) {

  xcb_gcontext_t gc = xcb_generate_id(xcb_conn_);
  const static uint32_t kGcValueMask =
      XCB_GC_FUNCTION | XCB_GC_PLANE_MASK | XCB_GC_SUBWINDOW_MODE;
  const static uint32_t kGcValues[] = {
    XCB_GX_COPY,
    0xffffffff,
    // This is needed for copying e.g. the root window.
    XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS,
  };
  xcb_create_gc(xcb_conn_, gc, dest_drawable, kGcValueMask, kGcValues);
  xcb_copy_area(xcb_conn_, src_drawable, dest_drawable, gc,
                src_x, src_y, dest_x, dest_y, width, height);
  xcb_free_gc(xcb_conn_, gc);
}

XWindow RealXConnection::CreateWindow(
    XWindow parent,
    int x, int y,
    int width, int height,
    bool override_redirect,
    bool input_only,
    int event_mask,
    XVisualID visual) {
  CHECK(width > 0);
  CHECK(height > 0);
  CHECK(parent != XCB_NONE);

  uint32_t value_mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
  // The values need to be in the same order as the numerical value of the
  // enabled flags:
  // XCB_CW_BORDER_PIXEL, XCB_CW_OVERRIDE_REDIRECT, XCB_CW_EVENT_MASK and then
  // XCB_CW_COLORMAP
  std::vector<uint32_t> values;
  values.push_back(override_redirect ? 1 : 0);
  values.push_back(event_mask);

  uint32_t depth = XCB_COPY_FROM_PARENT;
  xcb_colormap_t colormap_id = 0;
  if (visual) {
    XVisualInfo template_visual_info;
    template_visual_info.visualid = visual;

    int count;
    XVisualInfo* visual_info =
        GetVisualInfo(VisualIDMask, &template_visual_info, &count);
    CHECK(count == 1);
    CHECK(visual_info);
    depth = visual_info->depth;
    XFree(visual_info);

    // X says that if the visual is different from the parent's window, we need
    // a border pixel and a colormap.
    value_mask |= XCB_CW_BORDER_PIXEL | XCB_CW_COLORMAP;
    values.insert(values.begin(), 0);  // border pixel
    colormap_id = xcb_generate_id(xcb_conn_);
    xcb_create_colormap(xcb_conn_, XCB_COLORMAP_ALLOC_NONE, colormap_id, parent,
                        visual);
    values.push_back(colormap_id);  // colormap
  }

  const xcb_window_t xid = xcb_generate_id(xcb_conn_);
  xcb_create_window(xcb_conn_,
                    depth,
                    xid,
                    parent,
                    x, y,
                    width, height,
                    0,  // border_width
                    input_only ?
                      XCB_WINDOW_CLASS_INPUT_ONLY :
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    visual,
                    value_mask,
                    &values[0]);

  if (colormap_id)
    xcb_free_colormap(xcb_conn_, colormap_id);
  return xid;
}

bool RealXConnection::DestroyWindow(XWindow xid) {
  xcb_destroy_window(xcb_conn_, xid);
  return true;
}

bool RealXConnection::IsWindowShaped(XWindow xid) {
  xcb_shape_query_extents_cookie_t cookie =
      xcb_shape_query_extents(xcb_conn_, xid);
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_shape_query_extents_reply_t> reply(
      xcb_shape_query_extents_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Got X error while checking whether window "
                 << XidStr(xid) << " is shaped";
    return false;
  }
  return (reply->bounding_shaped != 0);
}

bool RealXConnection::SelectShapeEventsOnWindow(XWindow xid) {
  // xcb_shape_select_input() appears to be broken (maybe just when used in
  // conjunction with an Xlib event loop?).
  XShapeSelectInput(display_, xid, ShapeNotifyMask);
  return true;
}

bool RealXConnection::GetWindowBoundingRegion(XWindow xid, ByteMap* bytemap) {
  TrapErrors();
  int count = 0, ordering = 0;
  XRectangle* rects =
      XShapeGetRectangles(display_, xid, ShapeBounding, &count, &ordering);
  if (int error = UntrapErrors()) {
    LOG(WARNING) << "Got X error while getting bounding rectangles for "
                 << XidStr(xid) << ": " << GetErrorText(error);
    return false;
  }
  bytemap->Clear(0x0);
  for (int i = 0; i < count; ++i) {
    const XRectangle& rect = rects[i];
    bytemap->SetRectangle(rect.x, rect.y, rect.width, rect.height, 0xff);
  }
  XFree(rects);

  // Note that xcb_shape_get_rectangles() appears to be broken up to and
  // including libxcb 1.4, the version in Ubuntu 9.10 (the rectangles that
  // it returns are full of garbage values), but works correctly in 1.5.
  // TODO: Switch to the XCB version of this code if/when we go to 1.5.
#if 0
  xcb_shape_get_rectangles_cookie_t cookie =
      xcb_shape_get_rectangles(xcb_conn_, xid, XCB_SHAPE_SK_BOUNDING);
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_shape_get_rectangles_reply_t> reply(
      xcb_shape_get_rectangles_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Got X error while getting bounding region for "
                 << XidStr(xid);
    return false;
  }

  bytemap->Clear(0x0);
  xcb_rectangle_t* rectangles =
      xcb_shape_get_rectangles_rectangles(reply.get());
  int num_rectangles = xcb_shape_get_rectangles_rectangles_length(reply.get());
  for (int i = 0; i < num_rectangles; ++i) {
    const xcb_rectangle_t& rect = rectangles[i];
    bytemap->SetRectangle(rect.x, rect.y, rect.width, rect.height, 0xff);
  }
#endif

  return true;
}

bool RealXConnection::SelectRandREventsOnWindow(XWindow xid) {
  xcb_randr_select_input(xcb_conn_, xid, 1);
  return true;
}

bool RealXConnection::GetAtoms(
    const vector<string>& names, vector<XAtom>* atoms_out) {
  CHECK(atoms_out);
  atoms_out->clear();
  atoms_out->reserve(names.size());

  vector<xcb_intern_atom_cookie_t> cookies;
  cookies.reserve(names.size());

  // Send all of our requests...
  for (vector<string>::const_iterator it = names.begin();
       it != names.end(); ++it) {
    // Create the atom if it doesn't already exist (only_if_exists=0).
    cookies.push_back(xcb_intern_atom(xcb_conn_, 0, it->size(), it->data()));
  }

  // ... and then wait for the replies.
  for (size_t i = 0; i < names.size(); ++i) {
    xcb_generic_error_t* error = NULL;
    scoped_ptr_malloc<xcb_intern_atom_reply_t> reply(
        xcb_intern_atom_reply(xcb_conn_, cookies[i], &error));
    scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
    if (error) {
      LOG(WARNING) << "Unable to look up X atom named " << names[i];
      return false;
    }
    atoms_out->push_back(reply->atom);
  }
  return true;
}

bool RealXConnection::GetAtomName(XAtom atom, string* name) {
  CHECK(name);
  name->clear();

  xcb_get_atom_name_cookie_t cookie = xcb_get_atom_name(xcb_conn_, atom);
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_get_atom_name_reply_t> reply(
      xcb_get_atom_name_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Unable to look up name for X atom " << XidStr(atom);
    return false;
  }
  name->assign(xcb_get_atom_name_name(reply.get()),
               xcb_get_atom_name_name_length(reply.get()));
  return true;
}

bool RealXConnection::GetIntArrayProperty(
    XWindow xid, XAtom xatom, vector<int>* values) {
  CHECK(values);
  values->clear();

  string str_value;
  int format = 0;
  if (!GetPropertyInternal(xid, xatom, &str_value, &format, NULL))
    return false;

  if (format != kLongFormat) {
    LOG(WARNING) << "Got value with non-" << kLongFormat << "-bit format "
                 << format << " while getting int property " << XidStr(xatom)
                 << " for window " << XidStr(xid);
    return false;
  }
  if (str_value.size() % 4 != 0) {
    LOG(WARNING) << "Got value with non-multiple-of-4 size " << str_value.size()
                 << " while getting int property " << XidStr(xatom)
                 << " for window " << XidStr(xid);
    return false;
  }

  values->reserve(str_value.size() / 4);
  for (size_t offset = 0; offset < str_value.size(); offset += 4) {
    values->push_back(
        *reinterpret_cast<const int*>(str_value.data() + offset));
  }
  return true;
}

bool RealXConnection::SetIntArrayProperty(
    XWindow xid, XAtom xatom, XAtom type, const vector<int>& values) {
  if (values.size() * kLongFormat > kMaxPropertySize) {
    LOG(WARNING) << "Setting int property " << XidStr(xatom) << " for window "
                 << XidStr(xid) << " with " << values.size()
                 << " values (max is " << (kMaxPropertySize / kLongFormat)
                 << ")";
  }

  xcb_change_property(
      xcb_conn_,
      XCB_PROP_MODE_REPLACE,
      xid,
      xatom,
      type,
      kLongFormat,  // size in bits of items in 'values'
      values.size(),
      values.data());
  return true;
}

bool RealXConnection::GetStringProperty(XWindow xid, XAtom xatom, string* out) {
  CHECK(out);
  out->clear();

  int format = 0;
  XAtom type = XCB_NONE;
  if (!GetPropertyInternal(xid, xatom, out, &format, &type))
    return false;

  if (format != kByteFormat) {
    LOG(WARNING) << "Got value with non-" << kByteFormat << "-bit format "
                 << format << " while getting string property " << XidStr(xatom)
                 << " for window " << XidStr(xid);
    return false;
  }

  if (type != XA_STRING && type != utf8_string_atom_) {
    // Just warn if the property type is unexpected.
    LOG(WARNING) << "Getting property " << XidStr(xatom)
                 << " with unsupported type " << type
                 << " as string for window " << XidStr(xid);
  }
  return true;
}

bool RealXConnection::SetStringProperty(
    XWindow xid, XAtom xatom, const string& value) {
  xcb_change_property(
      xcb_conn_,
      XCB_PROP_MODE_REPLACE,
      xid,
      xatom,
      utf8_string_atom_,
      kByteFormat,  // size in bits of items in 'value'
      value.size(),
      value.data());
  return true;
}

bool RealXConnection::DeletePropertyIfExists(XWindow xid, XAtom xatom) {
  xcb_delete_property(xcb_conn_, xid, xatom);
  return true;
}

int RealXConnection::GetConnectionFileDescriptor() {
  return XConnectionNumber(display_);
}

bool RealXConnection::IsEventPending() {
  return XPending(display_) > 0;
}

void RealXConnection::GetNextEvent(void* event) {
  DCHECK(event);
  XEvent* xevent = reinterpret_cast<XEvent*>(event);
  XNextEvent(display_, xevent);
}

void RealXConnection::PeekNextEvent(void* event) {
  DCHECK(event);
  XEvent* xevent = reinterpret_cast<XEvent*>(event);
  XPeekEvent(display_, xevent);
}

bool RealXConnection::SendClientMessageEvent(XWindow dest_xid,
                                             XWindow xid,
                                             XAtom message_type,
                                             long data[5],
                                             int event_mask) {
  XEvent event;
  XClientMessageEvent* client_event = &(event.xclient);
  client_event->type = ClientMessage;
  client_event->window = xid;
  client_event->message_type = message_type;
  client_event->format = XConnection::kLongFormat;
  memcpy(client_event->data.l, data, sizeof(client_event->data.l));

  TrapErrors();
  XSendEvent(display_,
             dest_xid,
             False,  // propagate
             event_mask,
             &event);
  if (int error = UntrapErrors()) {
    LOG(WARNING) << "Got X error while sending message to window "
                 << XidStr(dest_xid) << ": " << GetErrorText(error);
    return false;
  }
  return true;
}

bool RealXConnection::WaitForWindowToBeDestroyed(XWindow xid) {
  XEvent event;
  TrapErrors();
  do {
    XWindowEvent(display_, xid, StructureNotifyMask, &event);
  } while (event.type != DestroyNotify);
  if (int error = UntrapErrors()) {
    LOG(WARNING) << "Got X error while waiting for window " << XidStr(xid)
                 << " to be destroyed: " << GetErrorText(error);
    return false;
  }
  return true;
}

bool RealXConnection::WaitForPropertyChange(XWindow xid, XTime* timestamp_out) {
  XEvent event;
  TrapErrors();
  XWindowEvent(display_, xid, PropertyChangeMask, &event);
  if (int error = UntrapErrors()) {
    LOG(WARNING) << "Got X error while waiting for property change on window "
                 << XidStr(xid) << ": " << GetErrorText(error);
    return false;
  }
  if (timestamp_out)
    *timestamp_out = event.xproperty.time;
  return true;
}

XWindow RealXConnection::GetSelectionOwner(XAtom atom) {
  xcb_get_selection_owner_cookie_t cookie =
      xcb_get_selection_owner(xcb_conn_, atom);
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_get_selection_owner_reply_t> reply(
      xcb_get_selection_owner_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Got X error while getting selection owner for "
                 << XidStr(atom);
    return XCB_NONE;
  }
  return reply->owner;
}

bool RealXConnection::SetSelectionOwner(
    XAtom atom, XWindow xid, XTime timestamp) {
  xcb_set_selection_owner(xcb_conn_, xid, atom, timestamp);
  return true;
}

bool RealXConnection::GetImage(XID drawable, int x, int y,
                               int width, int height, int drawable_depth,
                               scoped_ptr_malloc<uint8_t>* data_out,
                               ImageFormat* format_out) {
  DCHECK(data_out);
  DCHECK(format_out);

  TrapErrors();
  XImage* image = XGetImage(
      display_, drawable, x, y, width, height, AllPlanes, ZPixmap);
  if (int error = UntrapErrors()) {
    DLOG(WARNING) << "Got X error while getting image for drawable "
                  << XidStr(drawable) << ": " << GetErrorText(error);
    return false;
  }

  if (!GetImageFormat(image->byte_order == LSBFirst,
                      image->bits_per_pixel,
                      drawable_depth,
                      format_out)) {
    DLOG(WARNING) << "Unhandled format in image:"
                  << " drawable=" << XidStr(drawable)
                  << " drawable_depth=" << drawable_depth
                  << " image_depth=" << image->bits_per_pixel
                  << " lsb_first=" << (image->byte_order == LSBFirst);
    XDestroyImage(image);
    return false;
  }

  const size_t data_size = image->bytes_per_line * image->height;
  const int format_bpp = GetBitsPerPixelInImageFormat(*format_out);
  const size_t expected_size = width * height * format_bpp / 8;
  if (data_size != expected_size) {
    DLOG(WARNING) << "Expected " << expected_size << " bytes in image from "
                  << XidStr(drawable) << " (" << width << "x" << height
                  << " at " << format_bpp << " bpp) " << " but got "
                  << data_size;
    XDestroyImage(image);
    return false;
  }

  data_out->reset(reinterpret_cast<uint8_t*>(image->data));
  image->data = NULL;  // Take ownership so Xlib doesn't free it.
  XDestroyImage(image);
  return true;
}

bool RealXConnection::SetWindowCursor(XWindow xid, uint32 shape) {
  uint32_t value_mask = XCB_CW_CURSOR;
  uint32_t values[] = { GetCursorInternal(shape) };
  xcb_change_window_attributes(xcb_conn_, xid, value_mask, values);
  return true;
}

bool RealXConnection::GetChildWindows(XWindow xid,
                                      vector<XWindow>* children_out) {
  CHECK(children_out);
  xcb_query_tree_cookie_t cookie = xcb_query_tree(xcb_conn_, xid);
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_query_tree_reply_t> reply(
      xcb_query_tree_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Got X error while querying tree for " << XidStr(xid);
    return false;
  }

  children_out->clear();
  xcb_window_t* children = xcb_query_tree_children(reply.get());
  int num_children = xcb_query_tree_children_length(reply.get());
  for (int i = 0; i < num_children; ++i)
    children_out->push_back(children[i]);
  return true;
}

void RealXConnection::RefreshKeyboardMap(int request,
                                         KeyCode first_keycode,
                                         int count) {
  // Fill an event with enough data for XRefreshKeyboardMapping() to use it
  // (technically, the 'display' and 'request' fields look like they're all
  // it actually uses).
  XMappingEvent event;
  memset(&event, 0, sizeof(event));
  event.type = MappingNotify;
  event.display = display_;
  event.request = request;
  event.first_keycode = first_keycode;
  event.count = count;
  XRefreshKeyboardMapping(&event);
}

KeySym RealXConnection::GetKeySymFromKeyCode(KeyCode keycode) {
  return XKeycodeToKeysym(display_, keycode, 0);
}

KeyCode RealXConnection::GetKeyCodeFromKeySym(KeySym keysym) {
  return XKeysymToKeycode(display_, keysym);
}

string RealXConnection::GetStringFromKeySym(KeySym keysym) {
  char* ptr = XKeysymToString(keysym);
  if (!ptr) {
    return "";
  }
  return string(ptr);
}

bool RealXConnection::GrabKey(KeyCode keycode, uint32 modifiers) {
  xcb_grab_key(xcb_conn_,
               0,                     // owner_events
               root_,                 // grab_window
               modifiers,
               keycode,
               XCB_GRAB_MODE_ASYNC,   // pointer mode
               XCB_GRAB_MODE_ASYNC);  // keyboard_mode
  return true;
}

bool RealXConnection::UngrabKey(KeyCode keycode, uint32 modifiers) {
  xcb_ungrab_key(xcb_conn_, keycode, root_, modifiers);
  return true;
}

XDamage RealXConnection::CreateDamage(XDrawable drawable,
                                      DamageReportLevel level) {
  // TODO: Argh, more functionality that doesn't seem to work (sometimes?)
  // in XCB.  Damage handles created with xcb_damage_create() don't seem to
  // generate any DamageNotify events; handles created via the
  // corresponding Xlib function work fine.  Strangely, the XCB version
  // appears to work in conjunction with GDK, so maybe something else isn't
  // being initialized correctly here.
  TrapErrors();
  XDamage damage = XDamageCreate(display_, drawable, level);
  if (int error = UntrapErrors()) {
    LOG(WARNING) << "Got X error while creating damage handle for window "
                 << XidStr(drawable) << ": " << GetErrorText(error);
    return 0;
  }
  return damage;
}

void RealXConnection::DestroyDamage(XDamage damage) {
  xcb_damage_destroy(xcb_conn_, damage);
}

void RealXConnection::ClearDamage(XDamage damage) {
  xcb_damage_subtract(xcb_conn_, damage, XCB_NONE, XCB_NONE);
}

bool RealXConnection::SetDetectableKeyboardAutoRepeat(bool detectable) {
  Bool supported = False;
  XkbSetDetectableAutoRepeat(
      display_, detectable ? True : False, &supported);
  return (supported == True ? true : false);
}

bool RealXConnection::QueryKeyboardState(vector<uint8_t>* keycodes_out) {
  CHECK(keycodes_out);
  xcb_query_keymap_cookie_t cookie = xcb_query_keymap(xcb_conn_);
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_query_keymap_reply_t> reply(
      xcb_query_keymap_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Querying keyboard state failed";
    return false;
  }
  keycodes_out->resize(arraysize(reply->keys));
  memcpy(keycodes_out->data(), reply->keys, arraysize(reply->keys));
  return true;
}

bool RealXConnection::QueryPointerPosition(int* x_root, int* y_root) {
  xcb_query_pointer_cookie_t cookie = xcb_query_pointer(xcb_conn_, root_);
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_query_pointer_reply_t> reply(
      xcb_query_pointer_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Querying pointer position failed";
    return false;
  }
  if (x_root)
    *x_root = reply->root_x;
  if (y_root)
    *y_root = reply->root_y;
  return true;
}

void RealXConnection::Free(void* item) {
  XFree(item);
}

XVisualInfo* RealXConnection::GetVisualInfo(long mask,
                                            XVisualInfo* visual_template,
                                            int* item_count) {
  return XGetVisualInfo(display_, mask, visual_template, item_count);
}

void RealXConnection::TrapErrors() {
  DCHECK(!trapping_errors) << "X errors are already being trapped";
  // Sync to process any errors in the queue from XCB requests.
  XSync(display_, False);
  trapping_errors = true;
  last_error_code = 0;
  last_error_request_major_opcode = 0;
  last_error_request_minor_opcode = 0;
}

int RealXConnection::UntrapErrors() {
  DCHECK(trapping_errors) << "X errors aren't being trapped";
  // Sync in case we sent a request that didn't generate a reply.
  XSync(display_, False);
  trapping_errors = false;
  return last_error_code;
}

int RealXConnection::GetLastErrorCode() {
  return last_error_code;
}

string RealXConnection::GetErrorText(int error_code) {
  char str[1024];
  XGetErrorText(display_, error_code, str, sizeof(str));
  return string(str);
}

// static
bool RealXConnection::GetImageFormat(bool lsb_first,
                                     int image_depth,
                                     int drawable_depth,
                                     ImageFormat* format_out) {
  // We only support 32-bit image data with or without a usable alpha
  // channel at the moment, and 16-bit RGB images.
  switch (image_depth) {
    case 32: {
      if (drawable_depth != 24 && drawable_depth != 32)
        return false;
      bool has_alpha = (drawable_depth == 32);

      // Xlib appears to not fill in the red, green, and blue masks in XImage
      // structs in some cases, such as when fetching an image from a window's
      // XComposite pixmap.  We just assume that little-endian systems store
      // data in BGR order and big-endian systems use RGB.
      if (lsb_first)
        *format_out = has_alpha ? IMAGE_FORMAT_BGRA_32 : IMAGE_FORMAT_BGRX_32;
      else
        *format_out = has_alpha ? IMAGE_FORMAT_RGBA_32 : IMAGE_FORMAT_RGBX_32;
      break;
    }
    case 16:
      // The format is packed in unsigned short, so provided the server and
      // client use the same endianness, this should work for both.
      *format_out = IMAGE_FORMAT_RGB_16;
      break;
    default:
      return false;
  }

  return true;
}

bool RealXConnection::GrabServerImpl() {
  xcb_grab_server(xcb_conn_);
  return true;
}

bool RealXConnection::UngrabServerImpl() {
  xcb_ungrab_server(xcb_conn_);
  return true;
}

bool RealXConnection::QueryExtension(const string& name,
                                     int* first_event_out,
                                     int* first_error_out) {
  xcb_query_extension_cookie_t cookie =
      xcb_query_extension(xcb_conn_, name.size(), name.data());
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_query_extension_reply_t> reply(
      xcb_query_extension_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Querying extension " << name << " failed";
    return false;
  }
  if (!reply->present) {
    LOG(WARNING) << "Extension " << name << " is not present";
    return false;
  }

  if (first_event_out)
    *first_event_out = reply->first_event;
  if (first_error_out)
    *first_error_out = reply->first_error;
  return true;
}

bool RealXConnection::GetPropertyInternal(XWindow xid,
                                          XAtom xatom,
                                          string* value_out,
                                          int* format_out,
                                          XAtom* type_out) {
  CHECK(value_out);
  value_out->clear();

  xcb_get_property_cookie_t cookie =
      xcb_get_property(xcb_conn_,
                       0,     // delete
                       xid,
                       xatom,
                       XCB_GET_PROPERTY_TYPE_ANY,
                       0,     // offset
                       kMaxPropertySize);
  xcb_generic_error_t* error = NULL;
  scoped_ptr_malloc<xcb_get_property_reply_t> reply(
      xcb_get_property_reply(xcb_conn_, cookie, &error));
  scoped_ptr_malloc<xcb_generic_error_t> scoped_error(error);
  if (error) {
    LOG(WARNING) << "Got X error while getting property " << XidStr(xatom)
                 << " for window " << XidStr(xid);
    return false;
  }
  if (reply->format == 0)
    return false;

  if (reply->bytes_after > 0) {
    LOG(WARNING) << "Didn't get " << reply->bytes_after << " extra bytes "
                 << "while getting property " << XidStr(xatom) << " for window "
                 << XidStr(xid);
  }

  const void* value = xcb_get_property_value(reply.get());
  size_t size = reply->value_len * (reply->format / 8);
  value_out->assign(static_cast<const char*>(value), size);

  if (format_out)
    *format_out = reply->format;
  if (type_out)
    *type_out = reply->type;

  return true;
}

xcb_cursor_t RealXConnection::GetCursorInternal(uint32 shape) {
  xcb_cursor_t cursor = FindWithDefault(
      cursors_, shape, static_cast<xcb_cursor_t>(XCB_NONE));
  if (cursor == XCB_NONE) {
    // XCreateFontCursor() tries to use the Xcursor library first before
    // falling back on the default cursors from the "cursor" font.  Xcursor
    // doesn't support XCB, but it lets us get nicer image-based cursors
    // from our theme instead of the cruddy default cursors.
    cursor = XCreateFontCursor(display_, shape);
    cursors_[shape] = cursor;
  }
  return cursor;
}

bool RealXConnection::CheckForXcbError(
    xcb_void_cookie_t cookie, const char* format, ...) {
  scoped_ptr_malloc<xcb_generic_error_t> error(
      xcb_request_check(xcb_conn_, cookie));
  if (!error.get())
    return true;

  va_list ap;
  va_start(ap, format);
  string message;
  StringAppendV(&message, format, ap);
  va_end(ap);

  LOG(WARNING) << "Got XCB error while " << message << ": "
               << GetErrorText(error->error_code);
  return false;
}

}  // namespace window_manager
