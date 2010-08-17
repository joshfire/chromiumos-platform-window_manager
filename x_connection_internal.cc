// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/x_connection_internal.h"

#include <cstring>

#include "window_manager/x_connection.h"

namespace window_manager {

namespace x_connection_internal {

void InitXClientMessageEvent(XEvent* event_out,
                             XWindow xid,
                             XAtom message_type,
                             long data[5]) {
  XClientMessageEvent* client_event = &(event_out->xclient);
  memset(client_event, 0, sizeof(*client_event));
  client_event->type = ClientMessage;
  client_event->window = xid;
  client_event->message_type = message_type;
  client_event->format = XConnection::kLongFormat;
  memcpy(client_event->data.l, data, sizeof(client_event->data.l));
}

void InitXConfigureEvent(XEvent* event_out,
                         XWindow xid,
                         int x, int y,
                         int width, int height,
                         int border_width,
                         XWindow above_xid,
                         bool override_redirect) {
  XConfigureEvent* configure_event = &(event_out->xconfigure);
  memset(configure_event, 0, sizeof(*configure_event));
  configure_event->type = ConfigureNotify;
  configure_event->event = xid;
  configure_event->window = xid;
  configure_event->x = x;
  configure_event->y = y;
  configure_event->width = width;
  configure_event->height = height;
  configure_event->border_width = 0;
  configure_event->above = above_xid;
  configure_event->override_redirect = override_redirect ? 1 : 0;
}

void StoreInt64InXSyncValue(int64_t src, XSyncValue* dest) {
  XSyncIntsToValue(dest, src & 0xffffffff, (src >> 32) & 0x7fffffff);
}

}  // namespace x_connection_internal

}  // namespace window_manager
