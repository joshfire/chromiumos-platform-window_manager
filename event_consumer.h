// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_EVENT_CONSUMER_H_
#define WINDOW_MANAGER_EVENT_CONSUMER_H_

#include "window_manager/geometry.h"
#include "window_manager/wm_ipc.h"  // for WmIpc::Message
#include "window_manager/x11/x_types.h"

namespace window_manager {

class DestroyedWindow;
class Window;

// This is an interface for things that want to receive X events from the
// WindowManager class.
//
// Except where noted otherwise, consumers express interest in a particular
// window's events by calling
// WindowManager::RegisterEventConsumerForWindowEvents().  When the window
// manager receives an event concerning the window, all interested
// consumers' handlers will be invoked in an arbitrary order.  Note that
// consumers may also need to select some event types on windows using
// XConnection::SelectInputOnWindow() in order for the X server to report
// those events to the window manager.
//
// The common case is:
// - A client creates a window and WindowManager begins tracking it.
// - The client tries to map the window.  WindowManager starts invoking
//   consumers' HandleWindowMapRequest() methods until one of them returns true.
// - After the map request has been sent (and typically before the map
//   notify has actually been received -- override-redirect windows are an
//   exception), WindowManager invokes all consumers' HandleWindowMap()
//   methods.  The consumer that will be handling the window (typically the
//   one that handled the map request) registers interest in the window's
//   events by calling RegisterEventConsumerForWindowEvents() with the
//   window's ID.
// - Stuff happens and the interested consumer is notified about the window's
//   events.
// - The client unmaps the window.  WindowManager invokes all consumers'
//   HandleWindowUnmap() methods.  The consumer that's handling the window
//   deletes any internal state about it and unregisters interest in the
//   window's events.
// - The client destroys the window and WindowManager stops tracking it.
class EventConsumer {
 public:
  EventConsumer() {}
  virtual ~EventConsumer() {}

  // Is the passed-in window an input window owned by this consumer?
  virtual bool IsInputWindow(XWindow xid) = 0;

  // Handle the screen being resized.
  // This method is invoked for all consumers.
  virtual void HandleScreenResize() = 0;

  // Handle Chrome notifying us that the user is either logged in or
  // logged out.  This method is invoked for all consumers.
  virtual void HandleLoggedInStateChange() = 0;

  // Handle a window's request to be mapped.  This is invoked to give
  // consumers a chance to change a window's position, size, or stacking
  // before it gets mapped.  If a consumer wants the window to be mapped, it
  // should do any desired setup of the window (but *not* map the window itself)
  // and then return true.
  //
  // WindowManager attempts to invoke this method for all consumers.  Once a
  // consumer returns true, the window will be mapped and the event won't be
  // passed to any other consumers.
  virtual bool HandleWindowMapRequest(Window* win) = 0;

  // Handle a window being mapped.  Invoked for all consumers.
  virtual void HandleWindowMap(Window* win) = 0;

  // Handle a window being unmapped.  Invoked for all consumers.
  virtual void HandleWindowUnmap(Window* win) = 0;

  // Handle a pixmap being fetched for a mapped window.  A newly-mapped window
  // cannot be composited until its pixmap has been fetched for the first time
  // (Window::has_initial_pixmap() can be used to track this).  When a window is
  // resized, a new pixmap must be fetched before it can be composited at the
  // new size.  Pixmaps are fetched automatically when needed; EventConsumers
  // don't need to do anything to trigger it.
  virtual void HandleWindowPixmapFetch(Window* win) = 0;

  // Handle a mapped window's request to be configured (unmapped windows'
  // requests are applied automatically).  If the consumer wants to configure
  // the window (possibly with different parameters than the requested ones), it
  // should call Window::Move() and Resize().  Otherwise, if the consumer is
  // managing the window but chooses not to make any changes to it in response
  // to the request, it should call Window::SendSyntheticConfigureNotify().
  virtual void HandleWindowConfigureRequest(Window* win,
                                            const Rect& requested_bounds) = 0;

  // Handle a button press or release on a window.  The first position is
  // relative to the upper-left corner of the window, while the second is
  // absolute.
  virtual void HandleButtonPress(XWindow xid,
                                 const Point& relative_pos,
                                 const Point& absolute_pos,
                                 int button,
                                 XTime timestamp) = 0;
  virtual void HandleButtonRelease(XWindow xid,
                                   const Point& relative_pos,
                                   const Point& absolute_pos,
                                   int button,
                                   XTime timestamp) = 0;

  // Handle the pointer entering, leaving, or moving within an input window.
  virtual void HandlePointerEnter(XWindow xid,
                                  const Point& relative_pos,
                                  const Point& absolute_pos,
                                  XTime timestamp) = 0;
  virtual void HandlePointerLeave(XWindow xid,
                                  const Point& relative_pos,
                                  const Point& absolute_pos,
                                  XTime timestamp) = 0;
  virtual void HandlePointerMotion(XWindow xid,
                                   const Point& relative_pos,
                                   const Point& absolute_pos,
                                   XTime timestamp) = 0;

  // Handle a Chrome-specific message sent by a client app.  Messages are
  // sent to consumers that have expressed interest in the messages' types
  // with WindowManager::RegisterEventConsumerForChromeMessages().
  virtual void HandleChromeMessage(const WmIpc::Message& msg) = 0;

  // Handle a regular X ClientMessage event from a client app.
  // These events are sent to consumers that have expressed interest in
  // events on the window referenced in the event's |window| field.
  virtual void HandleClientMessage(XWindow xid,
                                   XAtom message_type,
                                   const long data[5]) = 0;

  // Handle a property change.  These changes are sent to consumers that
  // have expressed interest in the (xid, xatom) pair with
  // WindowManager::RegisterEventConsumerForPropertyChanges().
  virtual void HandleWindowPropertyChange(XWindow xid, XAtom xatom) = 0;

  // Take ownership of a DestroyedWindow object after the underlying X
  // window has been destroyed.  Use
  // WindowManager::RegisterEventConsumerForDestroyedWindow() to register
  // interest in owning a not-yet-destroyed window (but see also
  // EventConsumerRegistrar::RegisterForDestroyedWindow()).
  //
  // |xid| shouldn't be used for anything other than passing to
  // EventConsumerRegistrar::HandleDestroyedWindow(), since it refers to a
  // window that no longer exists (and the ID may soon be recycled for a
  // new window).
  virtual void OwnDestroyedWindow(DestroyedWindow* destroyed_win,
                                  XWindow xid) = 0;
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_EVENT_CONSUMER_H_
