// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/stacking_manager.h"

#include <string>

#include "base/string_util.h"
#include "window_manager/atom_cache.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/x11/x_connection.h"

using std::map;
using std::string;
using std::tr1::shared_ptr;
using window_manager::util::FindWithDefault;
using window_manager::util::XidStr;

namespace window_manager {

StackingManager::StackingManager(XConnection* xconn,
                                 Compositor* compositor,
                                 AtomCache* atom_cache)
    : xconn_(xconn) {
  XWindow root = xconn_->GetRootWindow();

  for (int i = kNumLayers - 1; i >= 0; --i) {
    Layer layer = static_cast<Layer>(i);
    string name = StringPrintf("%s layer", LayerToName(layer));

    XWindow xid = 0;
    if (layer >= LAYER_TOP_CLIENT_WINDOW) {
      xid = xconn_->CreateWindow(root, Rect(-1, -1, 1, 1), true, true, 0, 0);
      xconn_->SetStringProperty(xid, atom_cache->GetXAtom(ATOM_WM_NAME), name);
      xconn_->SetStringProperty(
          xid, atom_cache->GetXAtom(ATOM_NET_WM_NAME), name);
      layer_to_xid_[layer] = xid;
      xid_to_layer_[xid] = layer;
    }

    shared_ptr<Compositor::Actor> actor(compositor->CreateGroup());
    actor->SetName(
        xid ? StringPrintf("%s %s", name.c_str(), XidStr(xid).c_str()) : name);
    actor->Hide();
    compositor->GetDefaultStage()->AddActor(actor.get());
    actor->RaiseToTop();
    layer_to_actor_[layer] = actor;
  }
}

StackingManager::~StackingManager() {
  for (map<XWindow, Layer>::const_iterator it = xid_to_layer_.begin();
       it != xid_to_layer_.end(); ++it)
    xconn_->DestroyWindow(it->first);
}

void StackingManager::StackWindowAtTopOfLayer(
    Window* win, Layer layer, ShadowPolicy shadow_policy) {
  DCHECK(win);

  DCHECK_GE(layer, LAYER_TOP_CLIENT_WINDOW)
      << "Window " << win->xid_str() << " being stacked above "
      << "top-client-window layer";
  Compositor::Actor* layer_actor = GetActorForLayer(layer);

  Compositor::Actor* lower_layer_actor = NULL;
  // Find the next-lowest layer so we can stack the window's shadow
  // directly above it.
  // TODO: This won't work for the bottom layer; write additional code to
  // handle it if it ever becomes necessary.
  if (shadow_policy == SHADOW_AT_BOTTOM_OF_LAYER)
    lower_layer_actor = GetActorForLayer(static_cast<Layer>(layer + 1));
  win->StackCompositedBelow(layer_actor, lower_layer_actor, true);

  XWindow layer_xid = GetXidForLayer(layer);
  win->StackClientBelow(layer_xid);
}

void StackingManager::StackXidAtTopOfLayer(XWindow xid, Layer layer) {
  DCHECK_GE(layer, LAYER_TOP_CLIENT_WINDOW)
      << "Window " << XidStr(xid) << " being stacked above "
      << "top-client-window layer";
  XWindow layer_xid = GetXidForLayer(layer);
  xconn_->StackWindow(xid, layer_xid, false);  // above=false
}

void StackingManager::StackActorAtTopOfLayer(Compositor::Actor* actor,
                                             Layer layer) {
  DCHECK(actor);
  Compositor::Actor* layer_actor = GetActorForLayer(layer);
  actor->Lower(layer_actor);
}

void StackingManager::StackWindowRelativeToOtherWindow(
    Window* win,
    Window* sibling,
    SiblingPolicy sibling_policy,
    ShadowPolicy shadow_policy,
    Layer shadow_layer) {
  DCHECK(win);
  DCHECK(sibling);

  Compositor::Actor* lower_layer_actor = NULL;
  if (shadow_policy == SHADOW_AT_BOTTOM_OF_LAYER)
    lower_layer_actor = GetActorForLayer(static_cast<Layer>(shadow_layer + 1));

  switch (sibling_policy) {
    case ABOVE_SIBLING:
      win->StackCompositedAbove(
          sibling->GetTopActor(), lower_layer_actor, true);
      win->StackClientAbove(sibling->xid());
      break;
    case BELOW_SIBLING: {
      // If we're stacking |win|'s shadow at the bottom of the layer, assume
      // that |sibling|'s shadow was also stacked there and stack |win| directly
      // under |sibling| instead of under its shadow.
      Compositor::Actor* sibling_actor =
          shadow_policy == SHADOW_AT_BOTTOM_OF_LAYER ?
          sibling->actor() :
          sibling->GetBottomActor();
      win->StackCompositedBelow(sibling_actor, lower_layer_actor, true);
      win->StackClientBelow(sibling->xid());
      break;
    }
    default:
      NOTREACHED() << "Unknown sibling policy " << sibling_policy;
  }
}

void StackingManager::StackActorRelativeToOtherActor(
    Compositor::Actor* actor,
    Compositor::Actor* sibling,
    SiblingPolicy sibling_policy) {
  DCHECK(actor);
  DCHECK(sibling);

  switch (sibling_policy) {
    case ABOVE_SIBLING:
      actor->Raise(sibling);
      break;
    case BELOW_SIBLING:
      actor->Lower(sibling);
      break;
    default:
      NOTREACHED() << "Unknown sibling policy " << sibling_policy;
  }
}

Compositor::Actor* StackingManager::GetActorIfLayerXid(XWindow xid) {
  map<XWindow, Layer>::const_iterator it = xid_to_layer_.find(xid);
  if (it == xid_to_layer_.end())
    return NULL;
  return GetActorForLayer(it->second);
}

// static
const char* StackingManager::LayerToName(Layer layer) {
  switch (layer) {
    case LAYER_DEBUGGING:                return "debugging";
    case LAYER_SCREEN_LOCKER_SNAPSHOT:   return "screen locker snapshot";
    case LAYER_TOP_CLIENT_WINDOW:        return "top client window";
    case LAYER_SCREEN_LOCKER:            return "screen locker";
    case LAYER_FULLSCREEN_WINDOW:        return "fullscreen window";
    case LAYER_DRAGGED_PANEL:            return "dragged panel";
    case LAYER_ACTIVE_TRANSIENT_WINDOW:  return "active transient window";
    case LAYER_PANEL_BAR_INPUT_WINDOW:   return "panel bar input window";
    case LAYER_PACKED_PANEL_IN_BAR:      return "packed panel in bar";
    case LAYER_FLOATING_PANEL_IN_BAR:    return "floating panel in bar";
    case LAYER_PACKED_PANEL_IN_DOCK:     return "packed panel in dock";
    case LAYER_PANEL_DOCK:               return "panel dock";
    case LAYER_TOPLEVEL_WINDOW:          return "toplevel window";
    case LAYER_SNAPSHOT_WINDOW:          return "snapshot window";
    case LAYER_LOGIN_OTHER_WINDOW:       return "login other window";
    case LAYER_LOGIN_WINDOW:             return "login window";
    case LAYER_BACKGROUND:               return "background";
    default:                             return "unknown";
  }
}

Compositor::Actor* StackingManager::GetActorForLayer(Layer layer) {
  shared_ptr<Compositor::Actor> layer_actor =
      FindWithDefault(layer_to_actor_, layer, shared_ptr<Compositor::Actor>());
  CHECK(layer_actor.get()) << "Invalid layer " << layer;
  return layer_actor.get();
}

XWindow StackingManager::GetXidForLayer(Layer layer) {
  XWindow xid = FindWithDefault(layer_to_xid_, layer, static_cast<XWindow>(0));
  CHECK(xid) << "Invalid layer " << layer;
  return xid;
}

}  // namespace window_manager
