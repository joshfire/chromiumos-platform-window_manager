// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

extern "C" {
#include <X11/Xlib.h>
#include <X11/extensions/sync.h>
}

#include "base/basictypes.h"
#include "window_manager/x_types.h"

#ifndef WINDOW_MANAGER_X_CONNECTION_INTERNAL_H_
#define WINDOW_MANAGER_X_CONNECTION_INTERNAL_H_

// This file defines functions that are useful for implementations of
// XConnection.  A separate file is used so that Xlib headers won't need to
// be pulled into x_connection.h.

namespace window_manager {

namespace x_connection_internal {

// Initialize an Xlib event to hold a ClientMessage event.
void InitXClientMessageEvent(XEvent* event_out,
                             XWindow xid,
                             XAtom message_type,
                             long data[5]);

// Initialize an Xlib event to hold a synthetic ConfigureNotify event.
void InitXConfigureEvent(XEvent* event_out,
                         XWindow xid,
                         int x, int y,
                         int width, int height,
                         int border_width,
                         XWindow above_xid,
                         bool override_redirect);

// Store a signed 64-bit integer in an XSyncValue (used by the Xlib
// implementation of the Sync extension).
void StoreInt64InXSyncValue(int64_t src, XSyncValue* dest);

}  // namespace x_connection_internal

}  // namespace window_manager

#endif  // WINDOW_MANAGER_X_CONNECTION_INTERNAL_H_
