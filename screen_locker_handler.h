// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_SCREEN_LOCKER_HANDLER_H_
#define WINDOW_MANAGER_SCREEN_LOCKER_HANDLER_H_

#include <set>

#include "window_manager/event_consumer.h"
#include "window_manager/x_types.h"

namespace window_manager {

class WindowManager;
class Window;

// ScreenLockerHandler is a simple event consumer that hides all other
// actors when a screen locker window gets mapped and unhides them when the
// locker window is unmapped.
class ScreenLockerHandler : public EventConsumer {
 public:
  explicit ScreenLockerHandler(WindowManager* wm);
  ~ScreenLockerHandler();

  // Begin EventConsumer implementation.
  virtual bool IsInputWindow(XWindow xid) { return false; }
  virtual void HandleScreenResize();
  virtual void HandleLoggedInStateChange() {}
  virtual bool HandleWindowMapRequest(Window* win);
  virtual void HandleWindowMap(Window* win);
  virtual void HandleWindowUnmap(Window* win);
  virtual void HandleWindowConfigureRequest(Window* win,
                                            int req_x, int req_y,
                                            int req_width, int req_height) {}
  virtual void HandleButtonPress(XWindow xid,
                                 int x, int y,
                                 int x_root, int y_root,
                                 int button,
                                 XTime timestamp) {}
  virtual void HandleButtonRelease(XWindow xid,
                                   int x, int y,
                                   int x_root, int y_root,
                                   int button,
                                   XTime timestamp) {}
  virtual void HandlePointerEnter(XWindow xid,
                                  int x, int y,
                                  int x_root, int y_root,
                                  XTime timestamp) {}
  virtual void HandlePointerLeave(XWindow xid,
                                  int x, int y,
                                  int x_root, int y_root,
                                  XTime timestamp) {}
  virtual void HandlePointerMotion(XWindow xid,
                                   int x, int y,
                                   int x_root, int y_root,
                                   XTime timestamp) {}
  virtual void HandleChromeMessage(const WmIpc::Message& msg) {}
  virtual void HandleClientMessage(XWindow xid,
                                   XAtom message_type,
                                   const long data[5]) {}
  virtual void HandleWindowPropertyChange(XWindow xid, XAtom xatom) {}
  virtual void OwnDestroyedWindow(DestroyedWindow* destroyed_win) {
    NOTREACHED();
  }
  // End EventConsumer implementation.

 private:
  bool is_locked() const { return !screen_locker_xids_.empty(); }

  WindowManager* wm_;  // not owned

  // Mapped screen locker windows.
  std::set<XWindow> screen_locker_xids_;

  DISALLOW_COPY_AND_ASSIGN(ScreenLockerHandler);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_SCREEN_LOCKER_HANDLER_H_
