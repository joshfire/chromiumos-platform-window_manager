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
#include "window_manager/compositor.h"
#include "window_manager/geometry.h"
#include "window_manager/wm_ipc.h"
#include "window_manager/x_connection.h"
#include "window_manager/x_types.h"

namespace window_manager {

struct Rect;
class Shadow;
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
  Window(WindowManager* wm, XWindow xid, bool override_redirect,
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
  bool mapped() const { return mapped_; }
  bool shaped() const { return shaped_; }

  int client_x() const { return client_x_; }
  int client_y() const { return client_y_; }
  int client_width() const { return client_width_; }
  int client_height() const { return client_height_; }

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
  void SetTitle(const std::string& title);

  const std::vector<XAtom>& wm_window_type_xatoms() const {
    return wm_window_type_xatoms_;
  }
  bool wm_state_fullscreen() const { return wm_state_fullscreen_; }
  bool wm_state_modal() const { return wm_state_modal_; }
  bool wm_hint_urgent() const { return wm_hint_urgent_; }

  // Is this window currently focused?  We don't go to the X server for
  // this; we just check with the FocusManager.
  bool IsFocused() const;

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
  // and update 'supports_wm_take_focus_'.
  void FetchAndApplyWmProtocols();

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

  // Check if the window has been shaped using the Shape extension and
  // update its compositing actor accordingly.  If the window is shaped, we
  // hide its shadow if it has one.
  void FetchAndApplyShape();

  // Query the X server to see if this window is currently mapped or not.
  // This should only be used for checking the state of an existing window
  // at startup; use mapped() after that.
  bool FetchMapState();

  // Parse a _NET_WM_STATE message about this window, storing the requested
  // state changes in 'states_out'.  The passed-in data is from the
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

  // Add or remove passive a passive grab on button presses within this
  // window.  When any button is pressed, a _synchronous_ active pointer
  // grab will be installed.  Note that this means that no pointer events
  // will be received until the pointer grab is manually removed using
  // XConnection::RemovePointerGrab() -- this can be used to ensure that
  // the client receives the initial click on its window when implementing
  // click-to-focus behavior.
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

  // Should this window have a shadow?  By default, it won't.  Note that
  // even if true is passed to this method, the shadow may not be visible
  // (shadows aren't drawn for shaped windows, for instance) -- see
  // UpdateShadowVisibility().
  void SetShouldHaveShadow(bool should_use_shadow);

  // Change the opacity of the window's shadow.  The shadow's opacity is
  // multiplied by that of the window itself.
  void SetShadowOpacity(double opacity, int anim_ms);

  // Stack the window directly above 'actor' and its shadow directly above
  // or below 'shadow_actor' if supplied or below the window otherwise.  If
  // 'actor' is NULL, the window's stacking isn't changed (but its shadow's
  // still is).  If 'shadow_actor' is supplied, 'stack_above_shadow_actor'
  // determines whether the shadow will be stacked above or below it.
  void StackCompositedAbove(Compositor::Actor* actor,
                            Compositor::Actor* shadow_actor,
                            bool stack_above_shadow_actor);

  // Stack the window directly below 'actor' and its shadow directly above
  // or below 'shadow_actor' if supplied or below the window otherwise.  If
  // 'actor' is NULL, the window's stacking isn't changed (but its shadow's
  // still is).  If 'shadow_actor' is supplied, 'stack_above_shadow_actor'
  // determines whether the shadow will be stacked above or below it.
  void StackCompositedBelow(Compositor::Actor* actor,
                            Compositor::Actor* shadow_actor,
                            bool stack_above_shadow_actor);

  // Return this window's bottom-most actor (either the window's shadow's
  // group, or its actor itself if there's no shadow).  This is useful for
  // stacking another actor underneath this window.
  Compositor::Actor* GetBottomActor();

  // Store the client window's position and size in 'rect'.
  void CopyClientBoundsToRect(Rect* rect) const;

 private:
  FRIEND_TEST(FocusManagerTest, Modality);  // sets 'wm_state_modal_'
  FRIEND_TEST(WindowManagerTest, VideoTimeProperty);

  // Minimum dimensions and rate per second for damage events at which we
  // conclude that a video is currently playing in this window.
  static const int kVideoMinWidth;
  static const int kVideoMinHeight;
  static const int kVideoMinFramerate;

  // Helper method for ParseWmStateMessage() and ChangeWmState().  Given an
  // action from a _NET_WM_STATE message (i.e. the XClientMessageEvent's
  // data.l[0] field), updates 'value' accordingly.
  void SetWmStateInternal(int action, bool* value) const;

  // Update the window's _NET_WM_STATE property based on the current values
  // of the 'wm_state_*' members.
  bool UpdateWmStateProperty();

  // Update the window's _CHROME_STATE property based on the current
  // contents of 'chrome_state_atoms_'.
  bool UpdateChromeStateProperty();

  // Free 'pixmap_', store a new offscreen pixmap containing the window's
  // contents in it, and notify 'actor_' that the pixmap has changed.
  void ResetPixmap();

  // Update the visibility of 'shadow_' if it's non-NULL.  This window's
  // shadow can be enabled or disabled via SetShouldUseShadow(), but there
  // are still other reasons that we may not draw the shadow (e.g. the
  // window is shaped or not yet mapped).  This method is called when
  // various states that can prevent us from drawing the shadow are changed.
  void UpdateShadowVisibility();

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

  // Parameters associated with 'type_'.  See chromeos::WmIpcWindowType for
  // details.
  std::vector<int> type_params_;

  // Position and size of the client window.
  int client_x_;
  int client_y_;
  int client_width_;
  int client_height_;

  // Client-requested opacity (via _NET_WM_WINDOW_OPACITY).
  double client_opacity_;

  bool composited_shown_;
  int composited_x_;
  int composited_y_;
  double composited_scale_x_;
  double composited_scale_y_;
  double composited_opacity_;

  // Current opacity requested for the window's shadow.
  double shadow_opacity_;

  std::string title_;

  // Information from the WM_NORMAL_HINTS property.
  XConnection::SizeHints size_hints_;

  // Does the window have a WM_PROTOCOLS property claiming that it supports
  // WM_TAKE_FOCUS or WM_DELETE_WINDOW messages?
  bool supports_wm_take_focus_;
  bool supports_wm_delete_window_;

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

  // Damage object used to track changes to 'xid_'.
  XID damage_;

  // Offscreen pixmap containing the window's redirected contents.
  XID pixmap_;

  // Number of "video-sized" or larger damage events that we've seen for
  // this window during the second beginning at 'video_damage_start_time_'.
  // We use this as a rough heuristic to try to detect when the user is
  // watching a video.
  int num_video_damage_events_;
  time_t video_damage_start_time_;

  DISALLOW_COPY_AND_ASSIGN(Window);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_WINDOW_H_
