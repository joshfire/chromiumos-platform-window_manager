// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/transient_window_collection.h"

#include <list>

#include "window_manager/stacking_manager.h"
#include "window_manager/window_manager.h"

using std::list;
using std::map;
using std::tr1::shared_ptr;

namespace window_manager {

TransientWindowCollection::TransientWindowCollection(
    Window* owner_win,
    Window* win_to_stack_above,
    CenterPolicy center_policy,
    KeepOnscreenPolicy keep_onscreen_policy,
    EventConsumer* event_consumer)
    : owner_win_(owner_win),
      win_to_stack_above_(win_to_stack_above ? win_to_stack_above : owner_win),
      event_consumer_(event_consumer),
      stacked_transients_(new Stacker<TransientWindow*>),
      transient_to_focus_(NULL),
      shown_(true),
      center_policy_(center_policy),
      keep_onscreen_policy_(keep_onscreen_policy) {
  DCHECK(owner_win_);
  DCHECK(event_consumer);
}

TransientWindowCollection::~TransientWindowCollection() {
  while (!transients_.empty())
    RemoveWindow(transients_.begin()->second->win);
  transient_to_focus_ = NULL;
}

bool TransientWindowCollection::ContainsWindow(const Window& win) const {
  return transients_.find(win.xid()) != transients_.end();
}

bool TransientWindowCollection::HasFocusedWindow() const {
  for (TransientWindowMap::const_iterator it = transients_.begin();
       it != transients_.end(); ++it) {
    if (it->second->win->IsFocused())
      return true;
  }
  return false;
}

bool TransientWindowCollection::TakeFocus(XTime timestamp) {
  if (!transient_to_focus_)
    return false;

  RestackTransientWindowOnTop(transient_to_focus_);
  wm()->FocusWindow(transient_to_focus_->win, timestamp);
  return true;
}

void TransientWindowCollection::SetPreferredWindowToFocus(
    Window* transient_win) {
  if (!transient_win) {
    if (transient_to_focus_ && !transient_to_focus_->win->wm_state_modal())
      transient_to_focus_ = NULL;
    return;
  }

  TransientWindow* transient = GetTransientWindow(*transient_win);
  if (!transient) {
    LOG(ERROR) << "Got request to prefer focusing " << transient_win->xid_str()
               << ", which isn't transient for " << owner_win_->xid_str();
    return;
  }

  if (transient == transient_to_focus_)
    return;

  if (!transient_to_focus_ ||
      !transient_to_focus_->win->wm_state_modal() ||
      transient_win->wm_state_modal())
    transient_to_focus_ = transient;
}

void TransientWindowCollection::AddWindow(
    Window* transient_win, bool stack_directly_above_owner) {
  CHECK(transient_win);
  if (ContainsWindow(*transient_win)) {
    LOG(ERROR) << "Got request to add already-present transient window "
               << transient_win->xid_str() << " to " << owner_win_->xid_str();
    return;
  }

  wm()->RegisterEventConsumerForWindowEvents(transient_win->xid(),
                                             event_consumer_);
  shared_ptr<TransientWindow> transient(new TransientWindow(transient_win));
  transients_[transient_win->xid()] = transient;

  // Info bubbles always keep their initial positions.
  if (transient_win->type() == chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE) {
    transient->SaveOffsetsRelativeToRect(
        owner_win_->client_bounds(),
        Point(transient_win->composited_x(), transient_win->composited_y()));
    transient->centered = false;
  } else {
    transient->UpdateOffsetsToCenterOverRect(
        owner_win_->client_bounds(),
        wm()->root_bounds(),
        keep_onscreen_policy_ == KEEP_ONSCREEN_ALWAYS);
    transient->centered = true;
  }

  // If the new transient is non-modal, stack it above the top non-modal
  // transient that we have.  If it's modal, just put it on top of all
  // other transients.
  TransientWindow* transient_to_stack_above = NULL;
  for (list<TransientWindow*>::const_iterator it =
           stacked_transients_->items().begin();
       it != stacked_transients_->items().end(); ++it) {
    if (transient_win->wm_state_modal() || !(*it)->win->wm_state_modal()) {
      transient_to_stack_above = (*it);
      break;
    }
  }
  if (transient_to_stack_above)
    stacked_transients_->AddAbove(transient.get(), transient_to_stack_above);
  else
    stacked_transients_->AddOnBottom(transient.get());

  SetPreferredWindowToFocus(transient_win);
  wm()->focus_manager()->UseClickToFocusForWindow(
      transient_win, FocusManager::PASS_CLICKS_THROUGH);

  transient_win->SetVisibility(shown_ ?
                               Window::VISIBILITY_SHOWN :
                               Window::VISIBILITY_HIDDEN);
  ConfigureTransientWindow(transient.get(), 0);
  ApplyStackingForTransientWindow(
      transient.get(),
      transient_to_stack_above ?
      transient_to_stack_above->win :
      (stack_directly_above_owner ? win_to_stack_above_ : NULL));
}

void TransientWindowCollection::RemoveWindow(Window* transient_win) {
  CHECK(transient_win);
  TransientWindow* transient = GetTransientWindow(*transient_win);
  if (!transient) {
    LOG(ERROR) << "Got request to remove not-present transient window "
               << transient_win->xid_str() << " from " << owner_win_->xid_str();
    return;
  }

  transient_win->SetVisibility(Window::VISIBILITY_HIDDEN);
  wm()->UnregisterEventConsumerForWindowEvents(transient_win->xid(),
                                               event_consumer_);
  stacked_transients_->Remove(transient);
  CHECK(transients_.erase(transient_win->xid()) == 1);

  if (transient_to_focus_ == transient) {
    transient_to_focus_ = NULL;
    TransientWindow* new_transient = FindTransientWindowToFocus();
    SetPreferredWindowToFocus(
        new_transient ? new_transient->win : NULL);
  }
}

void TransientWindowCollection::ConfigureAllWindowsRelativeToOwner(
    int anim_ms) {
  for (TransientWindowMap::iterator it = transients_.begin();
       it != transients_.end(); ++it) {
    ConfigureTransientWindow(it->second.get(), anim_ms);
  }
}

void TransientWindowCollection::ApplyStackingForAllWindows(
    bool stack_directly_above_owner) {
  Window* prev_win = stack_directly_above_owner ? win_to_stack_above_ : NULL;
  for (list<TransientWindow*>::const_reverse_iterator it =
           stacked_transients_->items().rbegin();
       it != stacked_transients_->items().rend();
       ++it) {
    TransientWindow* transient = *it;
    ApplyStackingForTransientWindow(transient, prev_win);
    prev_win = transient->win;
  }
}

void TransientWindowCollection::HandleConfigureRequest(
    Window* transient_win, const Rect& requested_bounds) {
  CHECK(transient_win);
  TransientWindow* transient = GetTransientWindow(*transient_win);
  CHECK(transient);

  const Rect orig_client_bounds = transient_win->client_bounds();

  // Move and resize the transient window as requested (only let info bubbles
  // move themselves).
  if (transient_win->type() == chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE) {
    transient->SaveOffsetsRelativeToRect(
        owner_win_->client_bounds(), requested_bounds.position());
    transient->centered = false;
  }

  if (requested_bounds.size() != transient_win->client_size()) {
    transient_win->Resize(requested_bounds.size(), GRAVITY_NORTHWEST);
    if (transient->centered) {
      transient->UpdateOffsetsToCenterOverRect(
          owner_win_->client_bounds(),
          wm()->root_bounds(),
          keep_onscreen_policy_ == KEEP_ONSCREEN_ALWAYS);
    }
  }

  ConfigureTransientWindow(transient, 0);

  // If the window didn't change, send a fake ConfigureNotify to the
  // client to let it know that we at least considered its request.
  if (transient_win->client_bounds() == orig_client_bounds)
    transient_win->SendSyntheticConfigureNotify();
}

void TransientWindowCollection::CloseAllWindows() {
  if (transients_.empty())
    return;

  XTime timestamp = wm()->GetCurrentTimeFromServer();
  for (TransientWindowMap::const_iterator it = transients_.begin();
       it != transients_.end(); ++it) {
    Window* win = it->second->win;
    if (!win->SendDeleteRequest(timestamp))
      LOG(WARNING) << "Unable to close transient window " << win->xid_str();
  }
}

void TransientWindowCollection::Show() {
  shown_ = true;
  for (TransientWindowMap::const_iterator it = transients_.begin();
       it != transients_.end(); ++it) {
    TransientWindow* transient = it->second.get();
    transient->win->SetVisibility(Window::VISIBILITY_SHOWN);
  }
}

void TransientWindowCollection::Hide() {
  shown_ = false;
  for (TransientWindowMap::const_iterator it = transients_.begin();
       it != transients_.end(); ++it) {
    TransientWindow* transient = it->second.get();
    transient->win->SetVisibility(Window::VISIBILITY_HIDDEN);
  }
}

void
TransientWindowCollection::TransientWindow::UpdateOffsetsToCenterOverRect(
    const Rect& center_rect, const Rect& bounding_rect, bool force_constrain) {
  x_offset = (center_rect.width - win->client_width()) / 2;
  y_offset = (center_rect.height - win->client_height()) / 2;

  // Only honor the bounding rectangle if the center rect already falls
  // completely inside of it or if we've been told to do so.
  if (bounding_rect.contains_rect(center_rect) || force_constrain) {
    if (center_rect.x + x_offset + win->client_width() >
        bounding_rect.x + bounding_rect.width) {
      x_offset = bounding_rect.x + bounding_rect.width -
                 win->client_width() - center_rect.x;
    }
    if (center_rect.x + x_offset < bounding_rect.x) {
      x_offset = bounding_rect.x - center_rect.x;
    }

    if (center_rect.y + y_offset + win->client_height() >
        bounding_rect.y + bounding_rect.height) {
      y_offset = bounding_rect.y + bounding_rect.height -
                 win->client_height() - center_rect.y;
    }
    if (center_rect.y + y_offset < bounding_rect.y) {
      y_offset = bounding_rect.y - center_rect.y;
    }
  }
}

TransientWindowCollection::TransientWindow*
TransientWindowCollection::GetTransientWindow(const Window& win) {
  TransientWindowMap::iterator it = transients_.find(win.xid());
  if (it == transients_.end())
    return NULL;
  return it->second.get();
}

void TransientWindowCollection::ConfigureTransientWindow(
    TransientWindow* transient, int anim_ms) {
  if (center_policy_ == CENTER_OVER_OWNER ||
      transient->win->type() == chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE) {
    transient->win->Move(
        Point(owner_win_->composited_x() +
                  owner_win_->composited_scale_x() * transient->x_offset,
              owner_win_->composited_y() +
                  owner_win_->composited_scale_y() * transient->y_offset),
        anim_ms);
    transient->win->ScaleComposited(
        owner_win_->composited_scale_x(),
        owner_win_->composited_scale_y(),
        anim_ms);
    transient->win->SetCompositedOpacity(
        owner_win_->composited_opacity(), anim_ms);
  } else if (center_policy_ == CENTER_ONSCREEN) {
    transient->win->Move(
        Point((wm()->width() - transient->win->client_width()) / 2,
              (wm()->height() - transient->win->client_height()) / 2),
        anim_ms);
  } else {
    NOTREACHED() << "Unknown center policy " << center_policy_;
  }
}

void TransientWindowCollection::ApplyStackingForTransientWindow(
    TransientWindow* transient, Window* other_win) {
  DCHECK(transient);
  if (other_win) {
    wm()->stacking_manager()->StackWindowRelativeToOtherWindow(
        transient->win,
        other_win,
        StackingManager::ABOVE_SIBLING,
        StackingManager::SHADOW_DIRECTLY_BELOW_ACTOR,
        StackingManager::LAYER_ACTIVE_TRANSIENT_WINDOW);
  } else {
    wm()->stacking_manager()->StackWindowAtTopOfLayer(
        transient->win,
        StackingManager::LAYER_ACTIVE_TRANSIENT_WINDOW,
        StackingManager::SHADOW_DIRECTLY_BELOW_ACTOR);
  }
}

TransientWindowCollection::TransientWindow*
TransientWindowCollection::FindTransientWindowToFocus() const {
  if (stacked_transients_->items().empty())
    return NULL;

  for (list<TransientWindow*>::const_iterator it =
           stacked_transients_->items().begin();
       it != stacked_transients_->items().end();
       ++it) {
    if ((*it)->win->wm_state_modal())
      return *it;
  }
  return stacked_transients_->items().front();
}

void TransientWindowCollection::RestackTransientWindowOnTop(
    TransientWindow* transient) {
  if (transient == stacked_transients_->items().front())
    return;

  DCHECK(stacked_transients_->Contains(transient));
  DCHECK_GT(stacked_transients_->items().size(), 1U);
  TransientWindow* transient_to_stack_above =
      stacked_transients_->items().front();
  stacked_transients_->Remove(transient);
  stacked_transients_->AddOnTop(transient);
  ApplyStackingForTransientWindow(transient, transient_to_stack_above->win);
}

}  // namespace window_manager
