// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_WINDOW_H_
#define WINDOW_MANAGER_WINDOW_H_

#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"  // for Atom enum
#include "window_manager/compositor/compositor.h"
#include "window_manager/geometry.h"
#include "window_manager/shadow.h"
#include "window_manager/wm_ipc.h"
#include "window_manager/x11/x_connection.h"
#include "window_manager/x11/x_types.h"

namespace window_manager {

class AnimationPair;
class DestroyedWindow;
struct Rect;
template<class T> class Stacker;  // from util.h
class WindowManager;

// A client window.
//
// Because we use Xcomposite, there are (at least) two locations for a
// given window that we need to keep track of:
//
// - Where the client window is actually located on the X server.  This is
//   relevant for input -- we shape the compositing overlay window so that
//   events fall through it to the client windows underneath.
// - Where the window gets drawn on the compositing overlay window.  It'll
//   typically just be drawn in the same location as the actual X window,
//   but we could also e.g. draw a scaled-down version of it in a different
//   location.
//
// These two locations are not necessarily the same.  When animating a
// window move, it may be desirable to just move the X window once to the
// final location and then animate the move on the overlay.  As a result,
// there are different sets of methods to manipulate the client window and
// the composited window.
class Window {
 public:
  Window(WindowManager* wm,
         XWindow xid,
         bool override_redirect,
         const XConnection::WindowGeometry& geometry);
  ~Window();

  XWindow xid() const { return xid_; }
  const std::string& xid_str() const { return xid_str_; }
  WindowManager* wm() { return wm_; }
  Compositor::TexturePixmapActor* actor() { return actor_.get(); }
  const Shadow* shadow() const { return shadow_.get(); }
  XWindow transient_for_xid() const { return transient_for_xid_; }
  bool override_redirect() const { return override_redirect_; }
  chromeos::WmIpcWindowType type() const { return type_; }
  const std::vector<int>& type_params() const { return type_params_; }
  const char* type_str() const {
    return chromeos::WmIpcWindowTypeToString(type_);
  }
  bool mapped() const { return mapped_; }
  bool shaped() const { return shaped_; }
  bool is_rgba() const { return client_depth_ == 32; }

  int client_x() const { return client_x_; }
  int client_y() const { return client_y_; }
  int client_width() const { return client_width_; }
  int client_height() const { return client_height_; }
  int client_depth() const { return client_depth_; }

  bool composited_shown() const { return composited_shown_; }
  int composited_x() const { return composited_x_; }
  int composited_y() const { return composited_y_; }
  int composited_width() const { return client_width_ * composited_scale_x_; }
  int composited_height() const { return client_height_ * composited_scale_y_; }
  double composited_scale_x() const { return composited_scale_x_; }
  double composited_scale_y() const { return composited_scale_y_; }
  double composited_opacity() const { return composited_opacity_; }

  // The client might've already requested that the window be translucent,
  // in addition to whatever level has been set on the composited window.
  double combined_opacity() const {
    return composited_opacity_ * client_opacity_;
  }

  const std::string& title() const { return title_; }
  const XConnection::SizeHints& size_hints() const { return size_hints_; }
  bool supports_wm_ping() const { return supports_wm_ping_; }
  const std::vector<XAtom>& wm_window_type_xatoms() const {
    return wm_window_type_xatoms_;
  }
  bool wm_state_fullscreen() const { return wm_state_fullscreen_; }
  bool wm_state_modal() const { return wm_state_modal_; }
  bool wm_hint_urgent() const { return wm_hint_urgent_; }
  const std::string& client_hostname() const { return client_hostname_; }
  int client_pid() const { return client_pid_; }

  // Have we received a pixmap for this window yet?  If not, it won't be
  // drawn onscreen.
  bool has_initial_pixmap() const { return pixmap_ != 0; }

  // Is this window currently focused?  We don't go to the X server for
  // this; we just check with the FocusManager.
  bool IsFocused() const;

  // Update |title_| based on _NET_WM_NAME.
  void FetchAndApplyTitle();

  // Get and apply hints that have been set for the client window.
  bool FetchAndApplySizeHints();
  bool FetchAndApplyTransientHint();

  // Update the window based on its Chrome OS window type property.
  bool FetchAndApplyWindowType();

  // Update the window's opacity in response to the current value of its
  // _NET_WM_WINDOW_OPACITY property.
  void FetchAndApplyWindowOpacity();

  // Fetch the window's WM_HINTS property (ICCCM 4.1.2.4) if it exists and
  // apply any changes that we see.
  void FetchAndApplyWmHints();

  // Fetch the window's WM_PROTOCOLS property (ICCCM 4.1.2.7) if it exists
  // and update the various |supports_wm_*| members.  Also calls
  // FetchAndApplyWmSyncRequestCounter(), if the window claims to support
  // _NET_WM_SYNC_REQUEST.
  void FetchAndApplyWmProtocols();

  // Fetch the window's _NET_WM_SYNC_REQUEST_COUNTER property (as described
  // in EWMH) and ask the Sync extension to notify us whenever it changes.
  bool FetchAndApplyWmSyncRequestCounterProperty();

  // Fetch the window's _NET_WM_STATE property and update our internal copy
  // of it.  ClientMessage events should be used to update the states of mapped
  // windows, so this is primarily useful for getting the initial state of the
  // window before it's been mapped.
  void FetchAndApplyWmState();

  // Fetch the window's _NET_WM_WINDOW_TYPE property and update
  // wm_window_type_atoms_.
  void FetchAndApplyWmWindowType();

  // Fetch the window's _CHROME_STATE property and update our internal copy
  // of it.
  void FetchAndApplyChromeState();

  // Fetch the window's WM_CLIENT_MACHINE property and update
  // |client_hostname_|.
  void FetchAndApplyWmClientMachine();

  // Fetch the window's _NET_WM_PID property and update |client_pid_|.
  void FetchAndApplyWmPid();

  // Check if the window has been shaped using the Shape extension and
  // update its compositing actor accordingly.  If the window is shaped, we
  // hide its shadow if it has one.
  void FetchAndApplyShape();

  // Query the X server to see if this window is currently mapped or not.
  // This should only be used for checking the state of an existing window
  // at startup; use mapped() after that.
  bool FetchMapState();

  // Parse a _NET_WM_STATE message about this window, storing the requested
  // state changes in |states_out|.  The passed-in data is from the
  // ClientMessage event.
  void ParseWmStateMessage(
      const long data[5], std::map<XAtom, bool>* states_out) const;

  // Set or unset _NET_WM_STATE values for this window.  Updates our
  // internal copy of the state and the window's _NET_WM_STATE property.
  bool ChangeWmState(const std::map<XAtom, bool>& states);

  // Set or unset particular _CHROME_STATE values for this window (each
  // atom's bool value states whether it should be added or removed).
  // Other existing values in the property remain unchanged.
  bool ChangeChromeState(const std::map<XAtom, bool>& states);

  // Give keyboard focus to the client window, using a WM_TAKE_FOCUS
  // message if the client supports it or a SetInputFocus request
  // otherwise.  (Note that the client doesn't necessarily need to accept
  // the focus if WM_TAKE_FOCUS is used; see ICCCM 4.1.7.)
  //
  // Most callers should call FocusManager::FocusWindow() instead of
  // invoking this directly; FocusManager handles adding or removing button
  // grabs for click-to-focus and updating the _NET_ACTIVE_WINDOW property.
  bool TakeFocus(XTime timestamp);

  // If the window supports WM_DELETE_WINDOW messages, ask it to delete
  // itself.  Just does nothing and returns false otherwise.
  bool SendDeleteRequest(XTime timestamp);

  // Send a _NET_WM_PING client message to the window so we can check that
  // it's not frozen.
  bool SendPing(XTime timestamp);

  // Add or remove passive a passive grab on button presses within this
  // window.  When any button is pressed, a _synchronous_ active pointer
  // grab will be installed.  Note that this means that no pointer events
  // will be received until the pointer grab is manually removed using
  // XConnection::UngrabPointer() -- this can be used to ensure that the client
  // receives the initial click on its window when implementing click-to-focus
  // behavior.
  //
  // Most callers should use FocusManager::UseClickToFocusForWindow(),
  // which will handle all of this for them.
  bool AddButtonGrab();
  bool RemoveButtonGrab();

  // Get the largest possible size for this window smaller than or equal to
  // the passed-in desired dimensions (while respecting any sizing hints it
  // supplied via the WM_NORMAL_HINTS property).
  void GetMaxSize(int desired_width, int desired_height,
                  int* width_out, int* height_out) const;

  // Tell the X server to map or unmap this window.
  bool MapClient();
  bool UnmapClient();

  // Update our internal copy of the client window's position or size.
  // External callers should only use these methods to record position and
  // size changes that they hear about for override-redirect windows;
  // non-override-redirect windows can be moved or resized using
  // MoveClient() and ResizeClient().
  void SaveClientPosition(int x, int y) {
    client_x_ = x;
    client_y_ = y;
  }
  void SaveClientSize(int width, int height) {
    client_width_ = width;
    client_height_ = height;
  }

  // Ask the X server to move or resize the client window.  Also calls the
  // corresponding SetClient*() method on success.  Returns false on
  // failure.
  bool MoveClient(int x, int y);

  bool MoveClientOffscreen();
  bool MoveClientToComposited();

  // Center the client window over the passed-in window.
  bool CenterClientOverWindow(Window* owner);

  // Resize the client window.  A southeast gravity means that the
  // bottom-right corner of the window will remain fixed while the
  // upper-left corner will move to accomodate the new size.
  bool ResizeClient(int width, int height, Gravity gravity);

  // Stack the client window directly above or below another window.
  bool StackClientAbove(XWindow sibling_xid);
  bool StackClientBelow(XWindow sibling_xid);

  // Make various changes to the composited window (and its shadow).
  void MoveComposited(int x, int y, int anim_ms);
  void MoveCompositedX(int x, int anim_ms);
  void MoveCompositedY(int y, int anim_ms);
  void MoveCompositedToClient();  // no animation
  void ShowComposited();
  void HideComposited();
  void SetCompositedOpacity(double opacity, int anim_ms);
  void ScaleComposited(double scale_x, double scale_y, int anim_ms);

  // Create and return a pair of Animation objects that can be used to animate
  // the window's X and Y positions.  Ownership of the object is passed to the
  // caller, who should pass it back via SetMoveCompositedAnimation() after
  // adding additional keyframes.
  //
  // Windows with shadows cannot currently be animated (this is DCHECK()-ed).
  AnimationPair* CreateMoveCompositedAnimation();

  // Use a pair of animations previously allocated with
  // CreateMoveCompositedAnimation() to animate this window's position.
  // Takes ownership of |animations|.
  void SetMoveCompositedAnimation(AnimationPair* animations);

  // Handle us having sent a request to the X server to map this
  // (non-override-redirect) window.  We send a _NET_WM_SYNC_REQUEST
  // message to the window and send a synthetic ConfigureNotify event, so
  // that we'll be notified by the client when it's finished painting the
  // window.
  void HandleMapRequested();

  // Handle a MapNotify event about this window.
  // We throw away the old pixmap for the window and get the new one.
  void HandleMapNotify();

  void HandleUnmapNotify();

  // This method is called when this window is redirected for compositing
  // after it has been unredirected.  The previously stored pixmap is no longer
  // valid, so it updates the pixmap by calling ResetPixmap.  The content of
  // the root window is copied to the new pixmap, so that the new pixmap's
  // uninitialized contents are not briefly visible.  This method can only be
  // called if this window's contents are currently painted on the entire root
  // window at (0, 0).
  void HandleRedirect();

  // Handle a ConfigureNotify event about this window.
  // Currently, we just pass the window's width and height so we can resize
  // the actor if needed.
  void HandleConfigureNotify(int width, int height);

  // Handle the window's contents being changed.
  void HandleDamageNotify(const Rect& bounding_box);

  // Handle the underlying X window being destroyed.  If this method is
  // invoked before destroying this Window object, a few
  // compositing-related resources (actor, shadow, X pixmap) will be
  // jettisoned via the returned DestroyedWindow object, ownership of which
  // passes to the caller.
  //
  // Attempts to continue using the Window object after invoking this
  // method will end in heartbreak -- almost every method will crash.  Only
  // call this when you're about to destroy the Window object.  Just
  // destroying the Window without calling this method first is fine if you
  // have no desire to continue displaying the window's contents onscreen.
  DestroyedWindow* HandleDestroyNotify();

  // Enable drawing a drop shadow of a given type beneath this window.
  // Note that even if this method is called, the shadow may not be visible
  // (shadows aren't drawn for shaped windows, for instance) -- see
  // UpdateShadowVisibility().  By default, we don't draw a shadow until
  // this method is called.
  void SetShadowType(Shadow::Type type);

  // Disable drawing a drop shadow beneath this window.
  void DisableShadow();

  // Change the opacity of the window's shadow.  The shadow's opacity is
  // multiplied by that of the window itself.
  void SetShadowOpacity(double opacity, int anim_ms);

  // Stack the window directly above |actor| and its shadow directly above
  // or below |shadow_actor| if supplied or below the window otherwise.  If
  // |actor| is NULL, the window's stacking isn't changed (but its shadow's
  // still is).  If |shadow_actor| is supplied, |stack_above_shadow_actor|
  // determines whether the shadow will be stacked above or below it.
  void StackCompositedAbove(Compositor::Actor* actor,
                            Compositor::Actor* shadow_actor,
                            bool stack_above_shadow_actor);

  // Stack the window directly below |actor| and its shadow directly above
  // or below |shadow_actor| if supplied or below the window otherwise.  If
  // |actor| is NULL, the window's stacking isn't changed (but its shadow's
  // still is).  If |shadow_actor| is supplied, |stack_above_shadow_actor|
  // determines whether the shadow will be stacked above or below it.
  void StackCompositedBelow(Compositor::Actor* actor,
                            Compositor::Actor* shadow_actor,
                            bool stack_above_shadow_actor);

  // Return this window's bottom-most actor (either the window's shadow's
  // group, or its actor itself if there's no shadow).  This is useful for
  // stacking another actor underneath this window.
  Compositor::Actor* GetBottomActor();

  // Store the client window's position and size in |rect|.
  void CopyClientBoundsToRect(Rect* rect) const;

  // Handle notification that a Sync extension alarm (presumably
  // |wm_sync_request_alarm_|) has triggered.  |value| contains the
  // triggering value of the counter being watched.
  void HandleSyncAlarmNotify(XID alarm_id, int64_t value);

  // Send a synthetic ConfigureNotify event to the client containing the
  // window's current position, size, etc.
  void SendSyntheticConfigureNotify();

 private:
  friend class BasicWindowManagerTest;

  FRIEND_TEST(FocusManagerTest, Modality);  // sets |wm_state_modal_|
  FRIEND_TEST(WindowTest, SyncRequest);
  FRIEND_TEST(WindowTest, DeferFetchingPixmapUntilPainted);
  FRIEND_TEST(WindowManagerTest, VideoTimeProperty);
  FRIEND_TEST(WindowManagerTest, HandleLateSyncRequestCounter);

  // Minimum dimensions and rate per second for damage events at which we
  // conclude that a video is currently playing in this window.
  static const int kVideoMinWidth;
  static const int kVideoMinHeight;
  static const int kVideoMinFramerate;

  // Dimensions in which the actor should be moved by
  // MoveActorToAdjustedPosition().
  enum MoveDimensions {
    MOVE_DIMENSIONS_X_AND_Y = 0,
    MOVE_DIMENSIONS_X_ONLY,
    MOVE_DIMENSIONS_Y_ONLY,
  };

  // Is the entirety of the client window currently offscreen?
  bool IsClientWindowOffscreen() const;

  // Helper method for ParseWmStateMessage() and ChangeWmState().  Given an
  // action from a _NET_WM_STATE message (i.e. the XClientMessageEvent's
  // data.l[0] field), updates |value| accordingly.
  void SetWmStateInternal(int action, bool* value) const;

  // Update the window's _NET_WM_STATE property based on the current values
  // of the |wm_state_*| members.
  bool UpdateWmStateProperty();

  // Update the window's _CHROME_STATE property based on the current
  // contents of |chrome_state_atoms_|.
  bool UpdateChromeStateProperty();

  // Destroys |wm_sync_request_alarm_| if non-NULL, unregisters our
  // interest in it in the WindowManager, and resets
  // |client_has_redrawn_after_last_resize_| to true.
  void DestroyWmSyncRequestAlarm();

  // Move the actor to its correct position over |anim_ms|, given
  // |composited_x_| and |composited_y_|, the composited scale, and the actor's
  // current size versus the client window's size.  |dimensions| can be used to
  // limit the dimension over which the actor is moved to just X or Y (when
  // invoked by MoveCompositedX() or MoveCompositedY()).
  //
  // Way more background than you want to know: resizing a client window can be
  // tricky for compositing window managers.  Suppose that we have a 20x20
  // window located at (10, 10) and we want to make it bigger so that its
  // upper-left corner goes to (5, 10) while the right edge remains fixed,
  // resulting in a 25x20 window.  ResizeClient() asks the X server to
  // atomically move and resize the window to the new bounds, but the window
  // can't be drawn at the new size until the client has received the
  // ConfigureNotify event and finished painting the new pixmap.  If we move the
  // actor to (5, 10) immediately and then update its pixmap later, the window
  // will initially appear to jump to the left by 5 pixels; once we get the new
  // pixmap, the right edge will expand by 5 pixels.
  //
  // To avoid this jank, we update |composited_x_| and |composited_y_|
  // immediately in ResizeClient() if the window's origin moved due to the
  // resize gravity but hold off on actually moving the actor until its size
  // changes.  Similarly, methods like MoveComposited() may not actually move
  // the actor to the requested position immediately -- if we're waiting for the
  // pixmap to be resized, we take the difference between its current size and
  // the newly-requested size into account.
  void MoveActorToAdjustedPosition(MoveDimensions dimensions, int anim_ms);

  // Free |pixmap_|, store a new offscreen pixmap containing the window's
  // contents in it, and notify |actor_| that the pixmap has changed.
  void ResetPixmap();

  // Update the visibility of |shadow_| if it's non-NULL.  This window's
  // shadow can be enabled or disabled via SetShouldUseShadow(), but there
  // are still other reasons that we may not draw the shadow (e.g. the
  // window is shaped or not yet mapped).  This method is called when
  // various states that can prevent us from drawing the shadow are changed.
  void UpdateShadowVisibility();

  // If the client supports _NET_WM_SYNC_REQUEST, increment
  // |current_wm_sync_num_| and send the client a message telling it to
  // update the counter after it's seen the ConfigureNotify and redrawn its
  // contents.
  void SendWmSyncRequestMessage();

  XWindow xid_;
  std::string xid_str_;  // hex for debugging
  WindowManager* wm_;
  scoped_ptr<Compositor::TexturePixmapActor> actor_;

  // This contains a shadow if SetShouldUseShadow(true) has been called and
  // is NULL otherwise.
  scoped_ptr<Shadow> shadow_;

  // The XID that this window says it's transient for.  Note that the
  // client can arbitrarily supply an ID here; the window doesn't
  // necessarily exist.  A good general practice may be to examine this
  // value when the window is mapped and ignore any changes after that.
  XWindow transient_for_xid_;

  // Was override-redirect set when the window was originally created?
  bool override_redirect_;

  // Is the client window currently mapped?  This is only updated when the
  // Window object is first created and when a MapNotify or UnmapNotify
  // event is received (dependent on the receiver calling set_mapped()
  // appropriately), so e.g. a call to MapClient() will not be immediately
  // reflected in this variable.
  bool mapped_;

  // Is the window shaped (using the Shape extension)?
  bool shaped_;

  // Client-supplied window type.
  chromeos::WmIpcWindowType type_;

  // Parameters associated with |type_|.  See chromeos::WmIpcWindowType for
  // details.
  std::vector<int> type_params_;

  // Position and size of the client window.
  int client_x_;
  int client_y_;
  int client_width_;
  int client_height_;

  // Bit depth of the client window.
  int client_depth_;

  // Client-requested opacity (via _NET_WM_WINDOW_OPACITY).
  double client_opacity_;

  bool composited_shown_;
  int composited_x_;
  int composited_y_;
  double composited_scale_x_;
  double composited_scale_y_;
  double composited_opacity_;

  // Gravity used to position the actor in the case where the actor's size
  // differs from that of the client window.  See MoveActorToAdjustedPosition()
  // for details.
  Gravity actor_gravity_;

  // Current opacity requested for the window's shadow.
  double shadow_opacity_;

  std::string title_;

  // Information from the WM_NORMAL_HINTS property.
  XConnection::SizeHints size_hints_;

  // Does the window have a WM_PROTOCOLS property claiming that it supports
  // WM_TAKE_FOCUS or WM_DELETE_WINDOW messages?
  bool supports_wm_take_focus_;
  bool supports_wm_delete_window_;
  bool supports_wm_ping_;

  // EWMH window state, as set by _NET_WM_STATE client messages and exposed
  // in the window's _NET_WM_STATE property.
  // TODO: Just store these in a set like we do for _CHROME_STATE below.
  bool wm_state_fullscreen_;
  bool wm_state_maximized_horz_;
  bool wm_state_maximized_vert_;
  bool wm_state_modal_;

  // Is this window marked urgent, per the ICCCM UrgencyHint flag in its
  // WM_HINTS property?
  bool wm_hint_urgent_;

  // EWMH window types from the window's _NET_WM_WINDOW_TYPE property, in
  // the order in which they appear (which is the window's preference for
  // which type should be used).
  std::vector<XAtom> wm_window_type_xatoms_;

  // Chrome window state, as exposed in the window's _CHROME_STATE
  // property.
  std::set<XAtom> chrome_state_xatoms_;

  // Damage object used to track changes to |xid_|.
  XID damage_;

  // Offscreen pixmap containing the window's redirected contents.
  XID pixmap_;

  // Number of "video-sized" or larger damage events that we've seen for
  // this window during the second beginning at |video_damage_start_time_|.
  // We use this as a rough heuristic to try to detect when the user is
  // watching a video.
  int num_video_damage_events_;
  time_t video_damage_start_time_;

  // XSync counter ID from the window's _NET_WM_SYNC_REQUEST_COUNTER
  // property, or 0 if the window doesn't support _NET_WM_SYNC_REQUEST.
  XID wm_sync_request_alarm_;

  // Most-recent update request number that we've sent to the window before
  // resizing it as part of _NET_WM_SYNC_REQUEST.
  int64_t current_wm_sync_num_;

  // Has the client indicated that it's redrawn the window after the last
  // time that we resized it?  (If not, we're currently waiting for the
  // window to update |wm_sync_request_counter_| to match
  // |current_wm_sync_num_|.)  We also leave this at true if the client
  // doesn't support _NET_WM_SYNC_REQUEST.
  bool client_has_redrawn_after_last_resize_;

  // Hostname of the system on which the client is running, as specified in
  // the WM_CLIENT_MACHINE property.
  std::string client_hostname_;

  // The client's PID as specified in the _NET_WM_PID property, or -1 if
  // unknown.
  int client_pid_;

  DISALLOW_COPY_AND_ASSIGN(Window);
};

// We sometimes want to continue displaying a window's contents onscreen
// even after receiving a DestroyNotify event indicating that the
// underlying X window was closed.  DestroyedWindow contains a subset of
// compositing-related resources that have been released from an
// about-to-be-deleted Window object.
class DestroyedWindow {
 public:
  DestroyedWindow(WindowManager* wm,
                  XWindow xid,
                  Compositor::TexturePixmapActor* actor,
                  Shadow* shadow,
                  XID pixmap);
  ~DestroyedWindow();

  WindowManager* wm() { return wm_; }
  Compositor::TexturePixmapActor* actor() { return actor_.get(); }
  Shadow* shadow() { return shadow_.get(); }

 private:
  WindowManager* wm_;  // not owned

  // Compositing actor being used to display |pixmap_|.  This object
  // initially uses the same position, scaling, stacking, opacity, etc.
  // that it had when owned by the Window.
  scoped_ptr<Compositor::TexturePixmapActor> actor_;

  // Drop shadow that was set for the window, or NULL if no shadow was set.
  // Note that changes made to |actor_| will need to be manually applied to
  // |shadow_| as well.
  scoped_ptr<Shadow> shadow_;

  // X pixmap displayed by |actor_|; freed in our destructor.
  // TODO: Can this be freed when the Window object is destroyed, or even
  // earlier?  The actor is displaying a GL texture bound to a GLX pixmap
  // that was created from this X pixmap.
  XID pixmap_;

  DISALLOW_COPY_AND_ASSIGN(DestroyedWindow);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_WINDOW_H_
