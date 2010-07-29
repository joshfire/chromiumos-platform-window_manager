// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/stacking_manager.h"

#include <string>

#include "base/string_util.h"
#include "window_manager/atom_cache.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/x_connection.h"

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

    XWindow xid = xconn_->CreateWindow(root, -1, -1, 1, 1, true, true, 0, 0);
    xconn_->SetStringProperty(xid, atom_cache->GetXAtom(ATOM_WM_NAME), name);
    xconn_->SetStringProperty(
        xid, atom_cache->GetXAtom(ATOM_NET_WM_NAME), name);
    layer_to_xid_[layer] = xid;
    xid_to_layer_[xid] = layer;

    shared_ptr<Compositor::Actor> actor(compositor->CreateGroup());
    actor->SetName(StringPrintf("%s %s", name.c_str(), XidStr(xid).c_str()));
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

bool StackingManager::StackWindowAtTopOfLayer(Window* win, Layer layer) {
  DCHECK(win);

  Compositor::Actor* layer_actor = GetActorForLayer(layer);

  // Find the next-lowest layer so we can stack the window's shadow
  // directly above it.
  // TODO: This won't work for the bottom layer; write additional code to
  // handle it if it ever becomes necessary.
  Compositor::Actor* lower_layer_actor =
      GetActorForLayer(static_cast<Layer>(layer + 1));
  win->StackCompositedBelow(layer_actor, lower_layer_actor, true);

  XWindow layer_xid = GetXidForLayer(layer);
  return win->StackClientBelow(layer_xid);
}

bool StackingManager::StackXidAtTopOfLayer(XWindow xid, Layer layer) {
  XWindow layer_xid = GetXidForLayer(layer);
  return xconn_->StackWindow(xid, layer_xid, false);  // above=false
}

void StackingManager::StackActorAtTopOfLayer(Compositor::Actor* actor,
                                             Layer layer) {
  DCHECK(actor);
  Compositor::Actor* layer_actor = GetActorForLayer(layer);
  actor->Lower(layer_actor);
}

bool StackingManager::StackWindowRelativeToOtherWindow(
    Window* win, Window* sibling, bool above, Layer layer) {
  DCHECK(win);
  DCHECK(sibling);

  Compositor::Actor* lower_layer_actor =
      GetActorForLayer(static_cast<Layer>(layer + 1));
  if (above)
    win->StackCompositedAbove(sibling->actor(), lower_layer_actor, true);
  else
    win->StackCompositedBelow(sibling->actor(), lower_layer_actor, true);

  return above ?
         win->StackClientAbove(sibling->xid()) :
         win->StackClientBelow(sibling->xid());
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
    case LAYER_SCREEN_LOCKER:            return "screen locker";
    case LAYER_HOTKEY_OVERLAY:           return "hotkey overlay";
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
