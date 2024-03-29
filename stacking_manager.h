// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_STACKING_MANAGER_H_
#define WINDOW_MANAGER_STACKING_MANAGER_H_

#include <map>
#include <tr1/memory>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "window_manager/compositor/compositor.h"
#include "window_manager/x11/x_types.h"

namespace window_manager {

class AtomCache;
class Window;
class XConnection;

// Used to stack X11 client windows and compositor actors.  StackingManager
// creates a window and an actor to use as reference points for each
// logical stacking layer and provides methods to move windows and actors
// between layers.
class StackingManager {
 public:
  // Layers into which windows can be stacked, in top-to-bottom order.
  // Layers above LAYER_TOP_CLIENT_WINDOW don't have X windows, since we want
  // them to always appear above client windows.
  enum Layer {
    // Debugging objects that should be positioned above everything else.
    LAYER_DEBUGGING = 0,

    // Snapshots of the screen animated while locking or shutting down.
    LAYER_SCREEN_LOCKER_SNAPSHOT,

    // Actors belonging to client windows are initially stacked at this layer.
    // They shouldn't be raised above it (but note that an override-redirect
    // window can stack itself above this layer's X window -- the layers above
    // this one have no X windows, so their actors should always be stacked
    // above client windows' actors).
    LAYER_TOP_CLIENT_WINDOW,

    // Chrome screen locker window.
    LAYER_SCREEN_LOCKER,

    // A fullscreen window (maybe a regular Chrome window; maybe a panel
    // content window).
    LAYER_FULLSCREEN_WINDOW,

    // A panel as it's being dragged.  This is a separate layer so that the
    // panel's shadow will be cast over stationary panels.
    LAYER_DRAGGED_PANEL,

    // A transient window belonging to the currently-active toplevel
    // window.  Transients are stacked here when in active mode so that
    // they'll obscure panels.  (In overview mode, they're stacked directly
    // above their owners.)
    LAYER_ACTIVE_TRANSIENT_WINDOW,

    // Panel bar's input windows.
    LAYER_PANEL_BAR_INPUT_WINDOW,

    // A stationary, packed (that is, in the main group on the right) panel
    // in the panel bar.
    LAYER_PACKED_PANEL_IN_BAR,

    // A stationary, floating ("independently positioned") panel in the
    // panel bar.
    LAYER_FLOATING_PANEL_IN_BAR,

    // A stationary panel in a panel dock.
    LAYER_PACKED_PANEL_IN_DOCK,

    // Panel docks along the sides of the screen (specifically, their
    // backgrounds).
    LAYER_PANEL_DOCK,

    // Toplevel windows, along with their transient windows.
    LAYER_TOPLEVEL_WINDOW,

    // Snapshot windows, along with their input windows.
    LAYER_SNAPSHOT_WINDOW,

    // "Other" non-login windows (e.g. transient dialogs) managed by
    // LoginController.
    LAYER_LOGIN_OTHER_WINDOW,

    // Chrome login windows used by LoginController.
    LAYER_LOGIN_WINDOW,

    // The background image.
    LAYER_BACKGROUND,

    kNumLayers,
  };

  // Policies for stacking something relative to a supplied sibling.
  enum SiblingPolicy {
    // Stack the object directly above the sibling.
    ABOVE_SIBLING = 0,

    // Stack the object directly below the sibling.
    BELOW_SIBLING,
  };

  // Policies for stacking a window's shadow.
  enum ShadowPolicy {
    // Stack the shadow at the bottom of the window's layer.
    // This can be useful for preventing a window's shadow from falling on its
    // siblings -- imagine the case of two stationary panels located a pixel
    // apart.
    SHADOW_AT_BOTTOM_OF_LAYER = 0,

    // Stack the shadow directly below the window.
    SHADOW_DIRECTLY_BELOW_ACTOR,
  };

  // The layer reference points will be created at the top of the current stack
  // of X windows (for LAYER_TOP_CLIENT_WINDOW and below) and children of the
  // default compositor stage.
  StackingManager(XConnection* xconn,
                  Compositor* compositor,
                  AtomCache* atom_cache);
  ~StackingManager();

  // Is the passed-in X window one of our internal windows?
  bool IsInternalWindow(XWindow xid) {
    return (xid_to_layer_.find(xid) != xid_to_layer_.end());
  }

  // Stack a window (both its X window and its compositor actor) at the top
  // of the passed-in layer, which must be LAYER_TOP_CLIENT_WINDOW or below.
  void StackWindowAtTopOfLayer(
      Window* win, Layer layer, ShadowPolicy shadow_policy);

  // Stack an X window at the top of the passed-in layer, which must be
  // LAYER_TOP_CLIENT_WINDOW or below.  This is useful for X windows that don't
  // have Window objects associated with them (e.g. input windows).
  void StackXidAtTopOfLayer(XWindow xid, Layer layer);

  // Stack a compositor actor at the top of the passed-in layer.
  void StackActorAtTopOfLayer(Compositor::Actor* actor, Layer layer);

  // Stack a window's client and composited windows directly above or below
  // another window.  Make sure that |sibling| is in |layer| if using
  // SHADOW_AT_BOTTOM_OF_LAYER -- things will get confusing otherwise.
  void StackWindowRelativeToOtherWindow(
      Window* win,
      Window* sibling,
      SiblingPolicy sibling_policy,
      ShadowPolicy shadow_policy,
      Layer shadow_layer);

  // Stack a compositor actor above or below another actor.
  void StackActorRelativeToOtherActor(
      Compositor::Actor* actor,
      Compositor::Actor* sibling,
      SiblingPolicy sibling_policy);

  // If |xid| is being used as a layer's stacking reference point, return
  // the actor corresponding to the layer.  Returns NULL otherwise.
  Compositor::Actor* GetActorIfLayerXid(XWindow xid);

 private:
  friend class BasicWindowManagerTest;  // uses Get*ForLayer()
  FRIEND_TEST(LayoutManagerTest, InitialWindowStacking);  // uses |layer_to_*|
  FRIEND_TEST(WindowManagerTest, StackOverrideRedirectWindowsAboveLayers);

  // Get a layer's name.
  static const char* LayerToName(Layer layer);

  // Get the actor or XID for a particular layer.  These crash if the layer
  // is invalid.
  Compositor::Actor* GetActorForLayer(Layer layer);
  XWindow GetXidForLayer(Layer layer);

  XConnection* xconn_;  // not owned

  // Maps from layers to the corresponding X or Compositor reference points.
  // The reference points are stacked at the top of their corresponding
  // layer (in other words, the Stack*AtTopOfLayer() methods will stack
  // windows and actors directly beneath the corresponding reference
  // points).
  std::map<Layer, XWindow> layer_to_xid_;
  std::map<Layer, std::tr1::shared_ptr<Compositor::Actor> >
      layer_to_actor_;

  // Map we can use for quick lookup of whether an X window belongs to us,
  // and to find the layer corresponding to an X window.
  std::map<XWindow, Layer> xid_to_layer_;
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_STACKING_MANAGER_H_
