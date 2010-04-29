// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_WM_IPC_H_
#define WINDOW_MANAGER_WM_IPC_H_

#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/x_types.h"

namespace window_manager {

class AtomCache;
class XConnection;

// This class simplifies window-manager-to-client-app communication.  It
// consists primarily of utility methods to set and read properties on
// client windows and to pass messages back and forth between the WM and
// apps.
class WmIpc {
 public:
  WmIpc(XConnection* xconn, AtomCache* cache);

  // Get a window suitable for sending messages to the window manager.
  XWindow wm_window() const { return wm_window_; }

  // Get or set a property describing a window's type.  The window type
  // property must be set before mapping a window (for GTK+ apps, this
  // means it must happen between gtk_widget_realize() and
  // gtk_widget_show()).  Type-specific parameters may also be supplied
  // ('params' is mandatory for GetWindowType() but optional for
  // SetWindowType()).  false is returned if an error occurs.
  bool GetWindowType(XWindow xid,
                     chromeos::WmIpcWindowType* type,
                     std::vector<int>* params);
  bool SetWindowType(XWindow xid,
                     chromeos::WmIpcWindowType type,
                     const std::vector<int>* params);

  // Messages are sent via ClientMessageEvents that have 'message_type' set
  // to _CHROME_WM_MESSAGE, 'format' set to 32 (that is, 32-bit values),
  // and l[0] set to a value from the MessageType enum.  The remaining four
  // values in the 'l' array contain data specific to the type of message
  // being sent.
  // TODO: It'll require a protocol change, but it'd be good to change the
  // implementation so that messages that need to pass a window ID (that
  // is, most of them) do so in the 'window' field of the ClientMessage
  // event.  This will free up another data field for the payload and is
  // more consistent with many ICCCM and EWMH messages.
  struct Message {
   public:
    Message() {
      Init(chromeos::WM_IPC_MESSAGE_UNKNOWN);
    }
    explicit Message(chromeos::WmIpcMessageType type) {
      Init(type);
    }

    chromeos::WmIpcMessageType type() const { return type_; }
    void set_type(chromeos::WmIpcMessageType type) { type_ = type; }

    XWindow xid() const { return xid_; }
    void set_xid(XWindow xid) { xid_ = xid; }

    inline int max_params() const {
      return arraysize(params_);
    }
    long param(int index) const {
      DCHECK(index >= 0);
      DCHECK(index < max_params());
      return params_[index];
    }
    void set_param(int index, long value) {
      DCHECK(index >= 0);
      DCHECK(index < max_params());
      params_[index] = value;
    }

   private:
    // Common initialization code shared between constructors.
    void Init(chromeos::WmIpcMessageType type) {
      set_type(type);
      xid_ = 0;
      for (int i = 0; i < max_params(); ++i)
        set_param(i, 0);
    }

    // Type of message that was sent.
    chromeos::WmIpcMessageType type_;

    // Window associated with the event (more specifically, the 'window'
    // field of the ClientMessage event).
    XWindow xid_;

    // Type-specific data.  This is bounded by the number of 32-bit values
    // that we can pack into a ClientMessage event -- it holds five, but we
    // use the first one to store the Chrome OS message type.
    long params_[4];
  };

  // Check whether the contents of a ClientMessage event from the X server
  // belong to us.  If they do, the message is copied to 'msg' and true is
  // returned; otherwise, false is returned and the caller should continue
  // processing the event.  'xid' should be the 'window' field of the
  // ClientMessage event.
  bool GetMessage(XWindow xid,
                  XAtom message_type,
                  int format,
                  const long data[5],
                  Message* msg_out);

  // Send a message to a window.  false is returned if an error occurs.
  // Note that msg.xid() is ignored; the recipient's copy of the message
  // will contain the destination window specified in this method's 'xid'
  // parameter.
  bool SendMessage(XWindow xid, const Message& msg);

  // Set a property on the chosen window that contains system metrics
  // information.  False returned on error.
  bool SetSystemMetricsProperty(XWindow xid, const std::string& metrics);

 private:
  XConnection* xconn_;     // not owned
  AtomCache* atom_cache_;  // not owned

  // Window used for sending messages to the window manager.
  XWindow wm_window_;

  DISALLOW_COPY_AND_ASSIGN(WmIpc);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_WM_IPC_H_
