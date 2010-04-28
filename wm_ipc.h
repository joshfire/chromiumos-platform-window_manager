// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_WM_IPC_H_
#define WINDOW_MANAGER_WM_IPC_H_

#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/logging.h"
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

  enum WindowType {
    WINDOW_TYPE_UNKNOWN = 0,

    // A top-level Chrome window.
    //   param[0]: The number of tabs currently in this Chrome window.
    //   param[1]: The index of the currently selected tab in this
    //             Chrome window.
    WINDOW_TYPE_CHROME_TOPLEVEL,

    // Vestiges of the old windows-across-the-bottom overview mode.
    DEPRECATED_WINDOW_TYPE_CHROME_TAB_SUMMARY,
    DEPRECATED_WINDOW_TYPE_CHROME_FLOATING_TAB,

    // The contents of a popup window.
    //   param[0]: X ID of associated titlebar, which must be mapped before
    //             its content
    //   param[1]: Initial state for panel (0 is collapsed, 1 is expanded)
    WINDOW_TYPE_CHROME_PANEL_CONTENT,

    // A small window representing a collapsed panel in the panel bar and
    // drawn above the panel when it's expanded.
    WINDOW_TYPE_CHROME_PANEL_TITLEBAR,

    // Vestiges of an earlier UI design.
    DEPRECATED_WINDOW_TYPE_CREATE_BROWSER_WINDOW,

    // A Chrome info bubble (e.g. the bookmark bubble).  These are
    // transient RGBA windows; we skip the usual transient behavior of
    // centering them over their owner and omit drawing a drop shadow.
    WINDOW_TYPE_CHROME_INFO_BUBBLE,

    // A window showing a view of a tab within a Chrome window.
    //   param[0]: X ID of toplevel window that owns it.
    //   param[1]: index of this tab in the toplevel window that owns it.
    WINDOW_TYPE_CHROME_TAB_SNAPSHOT,

    // The following types are used for the windows that represent a user that
    // has already logged into the system.
    //
    // Visually the BORDER contains the IMAGE and CONTROLS windows, the LABEL
    // and UNSELECTED_LABEL are placed beneath the BORDER. The LABEL window is
    // onscreen when the user is selected, otherwise the UNSELECTED_LABEL is
    // on screen. The GUEST window is used when the user clicks on the entry
    // that represents the 'guest' user.
    //
    // The following parameters are set for these windows (except GUEST and
    // BACKGROUND):
    //   param[0]: the visual index of the user the window corresponds to.
    //             For example, all windows with an index of 0 occur first,
    //             followed by windows with an index of 1...
    //
    // The following additional params are set on the first BORDER window
    // (BORDER window whose param[0] == 0).
    //   param[1]: the total number of users.
    //   param[2]: size of the unselected image.
    //   param[3]: gap between image and controls.
    WINDOW_TYPE_LOGIN_BORDER,
    WINDOW_TYPE_LOGIN_IMAGE,
    WINDOW_TYPE_LOGIN_CONTROLS,
    WINDOW_TYPE_LOGIN_LABEL,
    WINDOW_TYPE_LOGIN_UNSELECTED_LABEL,
    WINDOW_TYPE_LOGIN_GUEST,
    WINDOW_TYPE_LOGIN_BACKGROUND,

    kNumWindowTypes,
  };
  // Get or set a property describing a window's type.  The window type
  // property must be set before mapping a window (for GTK+ apps, this
  // means it must happen between gtk_widget_realize() and
  // gtk_widget_show()).  Type-specific parameters may also be supplied
  // ('params' is mandatory for GetWindowType() but optional for
  // SetWindowType()).  false is returned if an error occurs.
  bool GetWindowType(XWindow xid, WindowType* type, std::vector<int>* params);
  bool SetWindowType(XWindow xid,
                     WindowType type,
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
    // NOTE: Don't remove values from this enum; it is shared between
    // Chrome and the window manager.
    enum Type {
      UNKNOWN = 0,

      // Vestiges of the old windows-across-the-bottom overview mode.
      DEPRECATED_CHROME_NOTIFY_FLOATING_TAB_OVER_TAB_SUMMARY,
      DEPRECATED_CHROME_NOTIFY_FLOATING_TAB_OVER_TOPLEVEL,
      DEPRECATED_CHROME_SET_TAB_SUMMARY_VISIBILITY,

      // Tell the WM to collapse or expand a panel.
      //   param[0]: X ID of the panel window
      //   param[1]: desired state (0 means collapsed, 1 means expanded)
      WM_SET_PANEL_STATE,

      // Notify Chrome that the panel state has changed.  Sent to the panel
      // window.
      //   param[0]: new state (0 means collapsed, 1 means expanded)
      // TODO: Deprecate this; Chrome can just watch for changes to the
      // _CHROME_STATE property to get the same information.
      CHROME_NOTIFY_PANEL_STATE,

      // From the old windows-across-the-bottom overview mode.
      DEPRECATED_WM_MOVE_FLOATING_TAB,

      // Notify the WM that a panel has been dragged.
      //   param[0]: X ID of the panel's content window
      //   param[1]: X coordinate to which the upper-right corner of the
      //             panel's titlebar window was dragged
      //   param[2]: Y coordinate to which the upper-right corner of the
      //             panel's titlebar window was dragged
      // Note: The point given is actually that of one pixel to the right
      // of the upper-right corner of the titlebar window.  For example, a
      // no-op move message for a 10-pixel wide titlebar whose upper-left
      // point is at (0, 0) would contain the X and Y paremeters (10, 0):
      // in other words, the position of the titlebar's upper-left point
      // plus its width.  This is intended to make both the Chrome and WM
      // side of things simpler and to avoid some easy-to-make off-by-one
      // errors.
      WM_NOTIFY_PANEL_DRAGGED,

      // Notify the WM that the panel drag is complete (that is, the mouse
      // button has been released).
      //   param[0]: X ID of the panel's content window
      WM_NOTIFY_PANEL_DRAG_COMPLETE,

      // Deprecated.  Send a _NET_ACTIVE_WINDOW client message to focus a
      // window instead (e.g. using gtk_window_present()).
      DEPRECATED_WM_FOCUS_WINDOW,

      // Notify Chrome that the layout mode (for example, overview or
      // active) has changed.  Since overview mode can be "cancelled"
      // (user hits escape to revert), we have an extra parameter to
      // indicate this.
      //   param[0]: new mode (0 means active mode, 1 means overview mode)
      //   param[1]: was mode cancelled? (0 = no, 1 = yes)
      CHROME_NOTIFY_LAYOUT_MODE,

      // Deprecated. Instruct the WM to enter overview mode.
      //   param[0]: X ID of the window to show the tab overview for.
      DEPRECATED_WM_SWITCH_TO_OVERVIEW_MODE,

      // Let the WM know which version of this file Chrome is using.  It's
      // difficult to make changes synchronously to Chrome and the WM (our
      // build scripts can use a locally-built Chromium, the latest one
      // from the buildbot, or an older hardcoded version), so it's useful
      // to be able to maintain compatibility in the WM with versions of
      // Chrome that exhibit older behavior.
      //
      // Chrome should send a message to the WM at startup containing the
      // latest version from the list below.  For backwards compatibility,
      // the WM assumes version 0 if it doesn't receive a message.  Here
      // are the changes that have been made in successive versions of the
      // protocol:
      //
      // 1: WM_NOTIFY_PANEL_DRAGGED contains the position of the
      //    upper-right, rather than upper-left, corner of of the titlebar
      //    window
      //
      // TODO: The latest version should be hardcoded in this file once the
      // file is being shared between Chrome and the WM so Chrome can just
      // pull it from there.  Better yet, the message could be sent
      // automatically in WmIpc's c'tor.
      //
      //   param[0]: version of this protocol currently supported
      WM_NOTIFY_IPC_VERSION,

      // Notify Chrome when a tab has been selected in the overview.
      // Sent to the toplevel window associated with the magnified
      // tab.
      //   param[0]: tab index of newly selected tab.
      CHROME_NOTIFY_TAB_SELECT,

      // Forces the window manager to hide the login windows.
      WM_HIDE_LOGIN,

      // Sets whether login is enabled. If true the user can click on any of the
      // login windows to select one, if false clicks on unselected windows are
      // ignored. This is used when the user attempts a login to make sure the
      // user doesn't select another user.
      //
      //   param[0]: true to enable, false to disable.
      WM_SET_LOGIN_STATE,

      // Notify chrome when the guest entry is selected and the guest window
      // hasn't been created yet.
      CHROME_CREATE_GUEST_WINDOW,

      kNumTypes,
    };

    Message() {
      Init(UNKNOWN);
    }
    explicit Message(Type type) {
      Init(type);
    }

    Type type() const { return type_; }
    void set_type(Type type) { type_ = type; }

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
    void Init(Type type) {
      set_type(type);
      xid_ = 0;
      for (int i = 0; i < max_params(); ++i) {
        set_param(i, 0);
      }
    }

    // Type of message that was sent.
    Type type_;

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
