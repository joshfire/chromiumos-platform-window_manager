// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/login/login_entry.h"

#include "window_manager/event_consumer_registrar.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

namespace window_manager {

LoginEntry::LoginEntry(WindowManager* wm, EventConsumerRegistrar* registrar)
  : wm_(wm),
    registrar_(registrar),
    border_window_(NULL),
    image_window_(NULL),
    controls_window_(NULL),
    label_window_(NULL),
    unselected_label_window_(NULL),
    sizes_initialized_(false) {
}

LoginEntry::~LoginEntry() {
  HandleWindowUnmap(controls_window_);
}

size_t LoginEntry::GetUserIndex(Window* win) {
  switch (win->type()) {
    case chromeos::WM_IPC_WINDOW_LOGIN_BORDER:
    case chromeos::WM_IPC_WINDOW_LOGIN_IMAGE:
    case chromeos::WM_IPC_WINDOW_LOGIN_CONTROLS:
    case chromeos::WM_IPC_WINDOW_LOGIN_LABEL:
    case chromeos::WM_IPC_WINDOW_LOGIN_UNSELECTED_LABEL:
      return win->type_params().empty() ? -1 : win->type_params()[0];
    default:
      return -1;
  }
}

bool LoginEntry::HasAllPixmaps() const {
  return has_all_windows() && border_window_->has_initial_pixmap() &&
         image_window_->has_initial_pixmap() &&
         controls_window_->has_initial_pixmap() &&
         label_window_->has_initial_pixmap() &&
         unselected_label_window_->has_initial_pixmap();
}

void LoginEntry::SetBorderWindow(Window* win) {
  if (border_window_)
    LOG(WARNING) << "two borders at index " << GetUserIndex(win);
  HandleWindowUnmap(border_window_);

  if (win->type_params().size() != 4) {
    LOG(ERROR) << "border window must have 4 parameters";
    border_window_ = NULL;
    return;
  }

  border_window_ = win;
  border_window_->SetShadowType(Shadow::TYPE_RECTANGULAR);
  registrar_->RegisterForWindowEvents(win->xid());
  if (has_all_windows())
    InitSizes();
}

void LoginEntry::SetImageWindow(Window* win) {
  if (image_window_)
    LOG(WARNING) << "two images at index " << GetUserIndex(win);
  HandleWindowUnmap(image_window_);
  image_window_ = win;
  registrar_->RegisterForWindowEvents(win->xid());
  if (has_all_windows())
    InitSizes();
}

void LoginEntry::SetControlsWindow(Window* win) {
  if (controls_window_)
    LOG(WARNING) << "two controls at index " << GetUserIndex(win);
  HandleWindowUnmap(controls_window_);
  controls_window_ = win;
  wm_->focus_manager()->UseClickToFocusForWindow(
      win, FocusManager::PASS_CLICKS_THROUGH);
  registrar_->RegisterForWindowEvents(win->xid());
  if (has_all_windows())
    InitSizes();
}

void LoginEntry::SetLabelWindow(Window* win) {
  if (label_window_)
    LOG(WARNING) << "two labels at index " << GetUserIndex(win);
  HandleWindowUnmap(label_window_);
  label_window_ = win;
  registrar_->RegisterForWindowEvents(win->xid());
  if (has_all_windows())
    InitSizes();
}

void LoginEntry::SetUnselectedLabelWindow(Window* win) {
  if (unselected_label_window_)
    LOG(WARNING) << "two unselected labels at index " << GetUserIndex(win);
  HandleWindowUnmap(unselected_label_window_);
  unselected_label_window_ = win;
  registrar_->RegisterForWindowEvents(win->xid());
  if (has_all_windows())
    InitSizes();
}

bool LoginEntry::HandleWindowUnmap(Window* win) {
  if (win == NULL) {
    return false;
  } else if (border_window_ == win) {
    border_window_ = NULL;
  } else if (image_window_ == win) {
    image_window_ = NULL;
  } else if (controls_window_ == win) {
    controls_window_ = NULL;
  } else if (label_window_ == win) {
    label_window_ = NULL;
  } else if (unselected_label_window_ == win) {
    unselected_label_window_ = NULL;
  } else {
    return false;
  }
  registrar_->UnregisterForWindowEvents(win->xid());
  sizes_initialized_ = false;
  return true;
}

size_t LoginEntry::GetUserCount() const {
  if (!border_window_)
    return -1;
  return border_window_->type_params()[1];
}

bool LoginEntry::IsNewUser() const {
  return GetUserIndex(border_window_) == GetUserCount() - 1;
}

void LoginEntry::InitSizes() {
  DCHECK(has_all_windows());
  sizes_initialized_ = true;

  int unselected_image_size = border_window_->type_params()[2];
  padding_ = border_window_->type_params()[3];

  border_width_ = border_window_->client_width();
  border_height_ = border_window_->client_height();

  controls_height_ = controls_window_->client_height();

  unselected_image_scale_x_ =
      static_cast<double>(unselected_image_size) /
      static_cast<double>(image_window_->client_width());
  unselected_image_scale_y_ =
      static_cast<double>(unselected_image_size) /
      static_cast<double>(image_window_->client_height());

  border_to_image_gap_ =
      (border_width_ - controls_window_->client_width()) / 2;
  border_to_unselected_image_gap_ =
      border_to_image_gap_ * unselected_image_scale_x_ + 0.5;
  unselected_border_width_ = unselected_image_size +
      2 * border_to_unselected_image_gap_;
  unselected_border_height_ = unselected_image_size +
      2 * border_to_unselected_image_gap_;

  unselected_border_scale_x_ = static_cast<double>(unselected_border_width_) /
      static_cast<double>(border_width_);
  unselected_border_scale_y_ = static_cast<double>(unselected_border_height_) /
      static_cast<double>(border_height_);

  unselected_label_scale_x_ =
      static_cast<double>(unselected_label_window_->client_width()) /
      static_cast<double>(label_window_->client_width());
  unselected_label_scale_y_ =
      static_cast<double>(unselected_label_window_->client_height()) /
      static_cast<double>(label_window_->client_height());
}

void LoginEntry::ScaleCompositeWindows(bool is_selected, int anim_ms) {
  DCHECK(sizes_initialized_);

  if (is_selected) {
    border_window_->ScaleComposited(1, 1, anim_ms);
    if (IsNewUser()) {
      // Image window for New User pod is 256x256 as usual but control window
      // is bigger so we need to upscale image to match controls window to
      // make animation nicer.
      double selected_image_scale_x =
          static_cast<double>(border_window_->client_width()) /
          static_cast<double>(image_window_->client_width());
      double selected_image_scale_y =
          static_cast<double>(border_window_->client_height()) /
          static_cast<double>(image_window_->client_height());
      image_window_->ScaleComposited(
          selected_image_scale_x,
          selected_image_scale_y,
          anim_ms);
    } else {
      image_window_->ScaleComposited(1, 1, anim_ms);
    }
    controls_window_->ScaleComposited(1, 1, anim_ms);
    label_window_->ScaleComposited(1, 1, anim_ms);
    unselected_label_window_->ScaleComposited(1 / unselected_label_scale_x_,
                                              1 / unselected_label_scale_y_,
                                              anim_ms);
  } else {
    border_window_->ScaleComposited(unselected_border_scale_x_,
                                    unselected_border_scale_y_, anim_ms);
    image_window_->ScaleComposited(unselected_image_scale_x_,
                                   unselected_image_scale_y_, anim_ms);
    if (IsNewUser()) {
      int unselected_image_size = border_window_->type_params()[2];
      double unselected_guest_scale_x =
          static_cast<double>(unselected_image_size) /
          static_cast<double>(controls_window_->client_width());
      double unselected_guest_scale_y =
          static_cast<double>(unselected_image_size) /
          static_cast<double>(controls_window_->client_height());
      controls_window_->ScaleComposited(unselected_guest_scale_x,
                                        unselected_guest_scale_y, anim_ms);
    } else {
      controls_window_->ScaleComposited(unselected_image_scale_x_, 0, anim_ms);
    }
    label_window_->ScaleComposited(unselected_label_scale_x_,
                                   unselected_label_scale_y_, anim_ms);
    unselected_label_window_->ScaleComposited(1, 1, anim_ms);
  }
}

void LoginEntry::UpdateClientWindows(const Point& origin, bool is_selected) {
  DCHECK(sizes_initialized_);

  int width = image_window_->client_width();
  int height = image_window_->client_height();
  if (is_selected) {
    if (!IsNewUser())
      image_window_->MoveClientToComposited();
  } else {
    // Move client to cover whole border plus gap between border and label.
    width = unselected_border_width_;
    height = unselected_border_height_ + border_to_image_gap_;
    DCHECK(height > 0) << "Label is above the image.";
    if (width > image_window_->client_width() ||
        height > image_window_->client_height()) {
      LOG(WARNING) << "Image window is not big enough to hold"
                      " the border and the label.";
    }
    image_window_->MoveClient(origin.x, origin.y);
  }
  wm_->xconn()->SetInputRegionForWindow(image_window_->xid(),
                                        Rect(0, 0, width, height));

  if (is_selected) {
    controls_window_->MoveClientToComposited();
    label_window_->MoveClientToComposited();
    unselected_label_window_->MoveClientOffscreen();
  } else {
    controls_window_->MoveClientOffscreen();
    label_window_->MoveClientOffscreen();
    unselected_label_window_->MoveClientToComposited();
  }
}

void LoginEntry::UpdatePositionAndScale(const Point& origin, bool is_selected,
                                        int anim_ms) {
  DCHECK(sizes_initialized_);

  // Border window is always aligned with entry's origin.
  border_window_->MoveComposited(origin.x, origin.y, anim_ms);

  // Image window is always aligned with border, save the gap.
  int border_gap = is_selected ?
                   border_to_image_gap_ :
                   border_to_unselected_image_gap_;
  int image_x = origin.x + border_gap;
  int image_y = origin.y + border_gap;
  int controls_x = image_x;
  int controls_y = image_y;
  int label_x = image_x;
  int label_y = image_y;

  if (IsNewUser()) {
    // For New User entry controls window is always aligned with image,
    // overlapping it if selected and hidden if not.
    // Label is slightly below the border window.
    int actual_border_height = unselected_border_height_;
    if (is_selected)
      actual_border_height = border_height_;
    label_y = origin.y + actual_border_height + border_to_image_gap_;
  } else {
    // For normal entries, label is aligned with the bottom of image,
    // Controls window is below the image, mind the gap.
    int label_height = unselected_label_window_->client_height();
    if (is_selected)
      label_height = label_window_->client_height();
    int image_height = unselected_border_height_ - 2 * border_gap;
    if (is_selected)
      image_height = image_window_->client_height();
    label_y = image_y + image_height - label_height;
    controls_y =
        origin.y + border_height_ - border_gap - controls_height_;
    if (!is_selected)
      controls_y = image_y + image_height + border_gap;
  }

  image_window_->MoveComposited(image_x, image_y, anim_ms);
  controls_window_->MoveComposited(controls_x, controls_y, anim_ms);
  label_window_->MoveComposited(label_x, label_y, anim_ms);
  unselected_label_window_->MoveComposited(label_x, label_y, anim_ms);

  ScaleCompositeWindows(is_selected, anim_ms);
  UpdateClientWindows(origin, is_selected);
}

void LoginEntry::FadeIn(const Point& origin, bool is_selected, int anim_ms) {
  DCHECK(sizes_initialized_);

  border_window_->ShowComposited();
  border_window_->SetCompositedOpacity(1, anim_ms);

  if (is_selected) {
    if (!IsNewUser()) {
      image_window_->ShowComposited();
      image_window_->SetCompositedOpacity(1, anim_ms);
    }

    controls_window_->ShowComposited();
    controls_window_->SetCompositedOpacity(1, anim_ms);

    label_window_->ShowComposited();
    label_window_->SetCompositedOpacity(1, anim_ms);
  } else {
    image_window_->ShowComposited();
    image_window_->SetCompositedOpacity(1, anim_ms);

    unselected_label_window_->ShowComposited();
    unselected_label_window_->SetCompositedOpacity(1, anim_ms);
  }

  UpdateClientWindows(origin, is_selected);
}

void LoginEntry::FadeOut(int anim_ms) {
  DCHECK(sizes_initialized_);

  border_window_->SetCompositedOpacity(0, anim_ms);
  border_window_->MoveClientOffscreen();

  image_window_->SetCompositedOpacity(0, anim_ms);
  image_window_->MoveClientOffscreen();

  controls_window_->SetCompositedOpacity(0, anim_ms);
  controls_window_->MoveClientOffscreen();

  label_window_->SetCompositedOpacity(0, anim_ms);
  label_window_->MoveClientOffscreen();

  unselected_label_window_->SetCompositedOpacity(0, anim_ms);
  unselected_label_window_->MoveClientOffscreen();
}

void LoginEntry::Select(const Point& origin, int anim_ms) {
  DCHECK(sizes_initialized_);

  UpdatePositionAndScale(origin, true, anim_ms);

  controls_window_->ShowComposited();
  if (IsNewUser()) {
    controls_window_->SetCompositedOpacity(1, anim_ms);
    image_window_->SetCompositedOpacity(0, anim_ms);
  } else {
    controls_window_->SetCompositedOpacity(1, 0);
  }

  label_window_->ShowComposited();
  label_window_->SetCompositedOpacity(1, anim_ms);

  unselected_label_window_->SetCompositedOpacity(0, anim_ms);
}

void LoginEntry::Deselect(const Point& origin, int anim_ms) {
  DCHECK(sizes_initialized_);

  UpdatePositionAndScale(origin, false, anim_ms);

  if (IsNewUser()) {
    image_window_->ShowComposited();
    controls_window_->SetCompositedOpacity(0, anim_ms);
    image_window_->SetCompositedOpacity(1, anim_ms);
  }

  label_window_->SetCompositedOpacity(0, anim_ms);

  unselected_label_window_->ShowComposited();
  unselected_label_window_->SetCompositedOpacity(1, anim_ms);
}

void LoginEntry::ProcessSelectionChangeCompleted(bool is_selected) {
  DCHECK(sizes_initialized_);

  if (is_selected) {
    if (IsNewUser())
      image_window_->HideComposited();
    unselected_label_window_->HideComposited();
  } else {
    controls_window_->HideComposited();
    label_window_->HideComposited();
    controls_window_->SetCompositedOpacity(0, 0);
  }
}

void LoginEntry::StackWindows() {
  DCHECK(sizes_initialized_);

  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      border_window_,
      StackingManager::LAYER_LOGIN_WINDOW,
      StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      image_window_,
      StackingManager::LAYER_LOGIN_WINDOW,
      StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      unselected_label_window_,
      StackingManager::LAYER_LOGIN_WINDOW,
      StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      label_window_,
      StackingManager::LAYER_LOGIN_WINDOW,
      StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      controls_window_,
      StackingManager::LAYER_LOGIN_WINDOW,
      StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
}

}  // namespace window_manager
