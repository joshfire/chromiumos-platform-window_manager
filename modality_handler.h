// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_MODALITY_HANDLER_H_
#define WINDOW_MANAGER_MODALITY_HANDLER_H_

#include <set>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/event_consumer.h"
#include "window_manager/focus_manager.h"

namespace window_manager {

class EventConsumerRegistrar;
class Window;
class WindowManager;

class ModalityChangeListener {
 public:
  // Invoked on a transition from not having a modal window focused to having
  // one focused, or vice versa.
  virtual void HandleModalityChange() = 0;

 protected:
  virtual ~ModalityChangeListener() {}
};

class ModalityHandler : public EventConsumer,
                        public FocusChangeListener {
 public:
  ModalityHandler(WindowManager* wm);
  virtual ~ModalityHandler();

  bool modal_window_is_focused() const { return modal_window_is_focused_; }

  // EventConsumer implementation.
  virtual bool IsInputWindow(XWindow xid) { return false; }
  virtual void HandleScreenResize();
  virtual void HandleLoggedInStateChange() {}
  virtual bool HandleWindowMapRequest(Window* win) { return false; }
  virtual void HandleWindowMap(Window* win);
  virtual void HandleWindowUnmap(Window* win);
  virtual void HandleWindowPixmapFetch(Window* win) {}
  virtual void HandleWindowConfigureRequest(Window* win,
                                            const Rect& requested_bounds) {}
  virtual void HandleButtonPress(XWindow xid,
                                 const Point& relative_pos,
                                 const Point& absolute_pos,
                                 int button,
                                 XTime timestamp) {}
  virtual void HandleButtonRelease(XWindow xid,
                                   const Point& relative_pos,
                                   const Point& absolute_pos,
                                   int button,
                                   XTime timestamp) {}
  virtual void HandlePointerEnter(XWindow xid,
                                  const Point& relative_pos,
                                  const Point& absolute_pos,
                                  XTime timestamp) {}
  virtual void HandlePointerLeave(XWindow xid,
                                  const Point& relative_pos,
                                  const Point& absolute_pos,
                                  XTime timestamp) {}
  virtual void HandlePointerMotion(XWindow xid,
                                   const Point& relative_pos,
                                   const Point& absolute_pos,
                                   XTime timestamp) {}
  virtual void HandleChromeMessage(const WmIpc::Message& msg) {}
  virtual void HandleClientMessage(XWindow xid,
                                   XAtom message_type,
                                   const long data[5]) {}
  virtual void HandleWindowPropertyChange(XWindow xid, XAtom xatom);
  virtual void OwnDestroyedWindow(DestroyedWindow* destroyed_win,
                                  XWindow xid) {}

  // FocusChangeListener implementation.
  virtual void HandleFocusChange();

  // Register or unregister a listener that will be notified after a change in
  // modality.
  void RegisterModalityChangeListener(ModalityChangeListener* listener);
  void UnregisterModalityChangeListener(ModalityChangeListener* listener);

 private:
  FRIEND_TEST(ModalityHandlerTest, Basic);
  FRIEND_TEST(PanelBarTest, ModalDimming);

  // Invoked when it's possible that a modal dialog has gained or lost the
  // focus.
  void HandlePossibleModalityChange();

  WindowManager* wm_;  // not owned

  scoped_ptr<EventConsumerRegistrar> event_consumer_registrar_;

  // Does a modal window currently have the focus?
  bool modal_window_is_focused_;

  // Partially-transparent black rectangle that we display beneath a modal
  // transient window to emphasize it.
  scoped_ptr<Compositor::ColoredBoxActor> dimming_actor_;

  // Listeners that will be notified when modality changes.
  std::set<ModalityChangeListener*> modality_change_listeners_;  // not owned

  DISALLOW_COPY_AND_ASSIGN(ModalityHandler);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_MODALITY_HANDLER_H_
