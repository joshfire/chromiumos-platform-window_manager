// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/modality_handler.h"

#include "window_manager/event_consumer_registrar.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

using window_manager::util::XidStr;

namespace window_manager {

// Opacity of the black rectangle that we use to dim everything in the
// background when a modal dialog is being displayed.
static const double kDimmingOpacity = 0.5;

// Duration in milliseconds over which we dim and undim the background when a
// modal dialog is mapped and unmapped.
static const int kDimmingFadeMs = 100;

ModalityHandler::ModalityHandler(WindowManager* wm)
    : wm_(wm),
      event_consumer_registrar_(new EventConsumerRegistrar(wm_, this)),
      modal_window_is_focused_(false),
      dimming_actor_(
          wm_->compositor()->CreateColoredBox(
              wm_->width(), wm_->height(), Compositor::Color(0, 0, 0))) {
  wm_->focus_manager()->RegisterFocusChangeListener(this);
  dimming_actor_->SetName("modal window dimming");
  dimming_actor_->SetOpacity(0, 0);
  dimming_actor_->Show();
  wm_->stage()->AddActor(dimming_actor_.get());
}

ModalityHandler::~ModalityHandler() {
  wm_->focus_manager()->UnregisterFocusChangeListener(this);
}

void ModalityHandler::HandleScreenResize() {
  dimming_actor_->SetSize(wm_->width(), wm_->height());
}

void ModalityHandler::HandleWindowMap(Window* win) {
  event_consumer_registrar_->RegisterForPropertyChanges(
      win->xid(), wm_->GetXAtom(ATOM_NET_WM_STATE));
}

void ModalityHandler::HandleWindowUnmap(Window* win) {
  event_consumer_registrar_->UnregisterForPropertyChanges(
      win->xid(), wm_->GetXAtom(ATOM_NET_WM_STATE));
}

void ModalityHandler::HandleWindowPropertyChange(XWindow xid, XAtom xatom) {
  Window* focused_win = wm_->focus_manager()->focused_win();
  if (!focused_win || focused_win->xid() != xid)
    return;

  HandlePossibleModalityChange();
}

void ModalityHandler::HandleFocusChange() {
  HandlePossibleModalityChange();
}

void ModalityHandler::HandlePossibleModalityChange() {
  Window* focused_win = wm_->focus_manager()->focused_win();
  bool focused_win_is_modal = focused_win && focused_win->wm_state_modal();

  if (focused_win_is_modal) {
    wm_->stacking_manager()->StackActorRelativeToOtherActor(
        dimming_actor_.get(),
        focused_win->GetBottomActor(),
        StackingManager::BELOW_SIBLING);
    if (!modal_window_is_focused_) {
      modal_window_is_focused_ = true;
      dimming_actor_->SetOpacity(kDimmingOpacity, kDimmingFadeMs);
    }
  } else {
    if (modal_window_is_focused_) {
      modal_window_is_focused_ = false;
      dimming_actor_->SetOpacity(0, kDimmingFadeMs);
    }
  }
}

}  // namespace window_manager
