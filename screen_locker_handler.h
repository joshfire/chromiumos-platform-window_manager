// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_SCREEN_LOCKER_HANDLER_H_
#define WINDOW_MANAGER_SCREEN_LOCKER_HANDLER_H_

#include <set>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/event_consumer.h"
#include "window_manager/x_types.h"

namespace window_manager {

class EventConsumerRegistrar;
class WindowManager;
class Window;

// ScreenLockerHandler is an event consumer that hides all other actors
// when a screen locker window gets mapped and unhides them when the locker
// window is unmapped.  It also handles messages sent by the power manager when
// the power button is pressed or unpressed or the system is shutting down, and
// messages sent by Chrome when the user is signing out.
class ScreenLockerHandler : public EventConsumer {
 public:
  explicit ScreenLockerHandler(WindowManager* wm);
  ~ScreenLockerHandler();

  bool session_ending() const { return session_ending_; }

  // Begin EventConsumer implementation.
  virtual bool IsInputWindow(XWindow xid) { return false; }
  virtual void HandleScreenResize();
  virtual void HandleLoggedInStateChange() {}
  virtual bool HandleWindowMapRequest(Window* win);
  virtual void HandleWindowMap(Window* win);
  virtual void HandleWindowUnmap(Window* win);
  virtual void HandleWindowInitialPixmap(Window* win);
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
  virtual void HandleChromeMessage(const WmIpc::Message& msg);
  virtual void HandleClientMessage(XWindow xid,
                                   XAtom message_type,
                                   const long data[5]) {}
  virtual void HandleWindowPropertyChange(XWindow xid, XAtom xatom) {}
  virtual void OwnDestroyedWindow(DestroyedWindow* destroyed_win, XWindow xid) {
    NOTREACHED();
  }
  // End EventConsumer implementation.

 private:
  friend class ScreenLockerHandlerTest;
  FRIEND_TEST(ScreenLockerHandlerTest, BasicLock);
  FRIEND_TEST(ScreenLockerHandlerTest, AbortedLock);
  FRIEND_TEST(ScreenLockerHandlerTest, SuccessfulLock);
  FRIEND_TEST(ScreenLockerHandlerTest, AbortedShutdown);
  FRIEND_TEST(ScreenLockerHandlerTest, HandleShutdown);
  FRIEND_TEST(ScreenLockerHandlerTest, InputsAlreadyGrabbed);
  FRIEND_TEST(ScreenLockerHandlerTest, DeferLockUntilWindowIsVisible);

  // Final size that we scale the snapshot of the screen down to in the
  // pre-lock and pre-shutdown states.
  static const float kSlowCloseSizeRatio;

  // Is there a window in |screen_locker_xids_| whose initial pixmap has
  // been loaded?
  bool HasWindowWithInitialPixmap() const;

  // Handle the power button having just been pressed while we're in an
  // unlocked state.  We take a snapshot of the screen, display only it,
  // and make it slowly zoom away from the user.
  void HandlePreLock();

  // Handle the power button having been released while in the pre-lock
  // state.  We animate the snapshot scaling back to its normal size and
  // set a timer to destroy it and switch back to displaying all actors.
  void HandleAbortedLock();

  // Handle the screen getting locked (that is, the first screen locker
  // window just got mapped).  We make the snapshot from the pre-lock state
  // zoom quickly down to the center of the screen and display only the
  // screen locker window.
  void HandleLocked();

  // Handle the screen getting unlocked (that is, the last screen locker
  // window was unmapped).  We display all actors.
  void HandleUnlocked();

  // Handle the power button having just been pressed while we're in the
  // locked state, or while not logged in.  We take a snapshot of the
  // screen, display only it, and make it slowly zoom away from the user.
  void HandlePreShutdown();

  // Handle the power button having been released while in the pre-shutdown
  // state.  We animate the snapshot scaling back to its normal size and
  // set a timer to destroy it and switch back to displaying the screen
  // locker window (if locked) or all actors (if unlocked).
  void HandleAbortedShutdown();

  // Handle notification that the current session is ending (either due to
  // shutdown if |shutting_down| is true or due to signout otherwise).  We set
  // the pointer to use a transparent cursor, grab the keyboard and pointer, and
  // display an animation.
  void HandleSessionEnding(bool shutting_down);

  // Try to grab the pointer and keyboard if they aren't grabbed already.
  // Once they're both grabbed, unregisters |grab_inputs_timeout_id_|.
  void TryToGrabInputs();

  // If |snapshot_actor_| is unset, grab and display a snapshot of the current
  // contents of the screen.
  void SetUpSnapshot();

  // Animate a snapshot of the screen slowly scaling down to
  // kSlowCloseSizeRatio.
  void StartSlowCloseAnimation();

  // Start an animation undoing the scaling from StartSlowCloseAnimation()
  // and register a timeout to call
  // DestroySnapshotAndUpdateVisibilityGroup() when it's done.
  void StartUndoSlowCloseAnimation();

  // Animate a snapshot of the screen quickly getting scaled down to the center
  // of the screen.  If |destroy_snapshot_when_done| is true, also register a
  // timeout to call DestroySnapshotAndUpdateVisibilityGroup() when it's done.
  // If there's an existing snapshot (from an in-progress slow-close animation),
  // we use it.
  void StartFastCloseAnimation(bool destroy_snapshot_when_done);

  // Animate a snapshot of the screen quickly fading out to black.
  void StartFadeoutAnimation();

  // Destroy |snapshot_actor_| and |snapshot_pixmap_|.
  void DestroySnapshot();

  // Call DestroySnapshot() and also reset the active visibility groups to
  // show all actors (if the screen is unlocked) or just the screen locker
  // window (if the screen is locked).
  void DestroySnapshotAndUpdateVisibilityGroup();

  // Reset |destroy_snapshot_timeout_id_| to -1 and call
  // DestroySnapshotAndUpdateVisibilityGroup().
  void HandleDestroySnapshotTimeout();

  WindowManager* wm_;  // not owned

  // Mapped screen locker windows.
  std::set<XWindow> screen_locker_xids_;

  // Non-screen locker windows that we should nevertheless show while the
  // screen is locked.
  std::set<XWindow> other_xids_to_show_while_locked_;

  scoped_ptr<EventConsumerRegistrar> registrar_;

  // Snapshot of the screen that we use for animations.
  XPixmap snapshot_pixmap_;
  scoped_ptr<Compositor::TexturePixmapActor> snapshot_actor_;

  // Timeout for calling DestroySnapshotAndUpdateVisibilityGroup(), or -1
  // if unset.
  int destroy_snapshot_timeout_id_;

  // Is the screen currently locked?  We only consider the screen to be
  // locked if a screen locker window has been mapped and we've loaded a
  // pixmap for it.
  bool is_locked_;

  // Is the current X session ending?  Set to true in response to a
  // WM_IPC_MESSAGE_WM_NOTIFY_SIGNING_OUT or
  // WM_IPC_MESSAGE_WM_NOTIFY_SHUTTING_DOWN message and never unset.
  bool session_ending_;

  // Recurring timeout that we use to try to grab the pointer and the keyboard
  // when the session is ending.
  int grab_inputs_timeout_id_;

  // Are the pointer and keyboard grabbed?
  bool pointer_grabbed_;
  bool keyboard_grabbed_;

  // Transparent cursor that we use to hide the pointer while the session is
  // ending.
  XID transparent_cursor_;

  DISALLOW_COPY_AND_ASSIGN(ScreenLockerHandler);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_SCREEN_LOCKER_HANDLER_H_
