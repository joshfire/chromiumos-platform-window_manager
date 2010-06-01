// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/separator.h"

#include <cmath>
#include <string>

#include <gflags/gflags.h>

#include "window_manager/stacking_manager.h"
#include "window_manager/window_manager.h"

DEFINE_string(separator_image, "../assets/images/separator.png",
              "Path to the image file containing the separator image.");

using std::string;

namespace window_manager {

static double kSeparatorOpacity = 0.7;

// Static member.
Compositor::Actor* LayoutManager::Separator::texture_ = NULL;

LayoutManager::Separator::Separator(LayoutManager* layout_manager)
    : layout_manager_(layout_manager),
      state_(STATE_ACTIVE_MODE_INVISIBLE),
      last_state_(STATE_ACTIVE_MODE_INVISIBLE) {
  CHECK(layout_manager_);
  compositor_ = layout_manager_->wm_->compositor();

  if (!texture_)
    Init();

  actor_.reset(compositor_->CloneActor(texture_));
  actor_->SetName("separator");
  Hide();
  compositor_->GetDefaultStage()->AddActor(actor_.get());
  layout_manager_->wm_->stacking_manager()->StackActorAtTopOfLayer(
      actor_.get(), StackingManager::LAYER_SNAPSHOT_WINDOW);
}

void LayoutManager::Separator::Show() {
  actor_->SetVisibility(true);
}

void LayoutManager::Separator::Hide() {
  actor_->SetVisibility(false);
}

void LayoutManager::Separator::Move(int x, int y, int anim_ms) {
  actor_->Move(x, y, anim_ms);
}

void LayoutManager::Separator::MoveX(int x, int anim_ms) {
  actor_->MoveX(x, anim_ms);
}

void LayoutManager::Separator::MoveY(int y, int anim_ms) {
  actor_->MoveY(y, anim_ms);
}

void LayoutManager::Separator::Resize(int width, int height, int anim_ms) {
  if (actor_->GetWidth() > 0 && actor_->GetHeight() > 0)
    actor_->Scale(static_cast<double>(width) / actor_->GetWidth(),
                  static_cast<double>(height) / actor_->GetHeight(),
                  anim_ms);
}

void LayoutManager::Separator::SetOpacity(double opacity, int anim_ms) {
  actor_->SetOpacity(opacity, anim_ms);
}

void LayoutManager::Separator::UpdateLayout(bool animate) {
  int anim_ms = animate ? LayoutManager::kWindowAnimMs : 0;
  int overview_x = layout_manager_->x() +
                   layout_manager_->overview_panning_offset() + x_;
  int overview_y = layout_manager_->y() + y_;
  if (state_ == STATE_ACTIVE_MODE_INVISIBLE) {
    SetOpacity(0.0, anim_ms);
    Move(overview_x, layout_manager_->y() + layout_manager_->height(), anim_ms);
    // Would like to hide here, but can't because then the above
    // animations wouldn't be seen.
  } else {
    // Start below the layout manager and slide in while fading in.
    if (last_state_ != state_) {
      Move(overview_x, layout_manager_->y() + layout_manager_->height(), 0);
      SetOpacity(0.0, 0);
      Resize(width_, height_, 0);  // don't animate the resize.
      last_state_ = state_;
    }

    Show();
    SetOpacity(kSeparatorOpacity, anim_ms);
    Move(overview_x, overview_y, anim_ms);
  }
}

void LayoutManager::Separator::Init() {
  CHECK(!texture_) << "Was Init() already called?";
  string filename = FLAGS_separator_image;
  texture_ = compositor_->CreateImage(filename);
  texture_->SetName(filename);

  // Even though we don't actually want to display it, we need to add
  // the texture_ to the default stage; otherwise the compositor
  // complains that actors that are cloned from it are unmappable.
  // TODO: This used to be the case with Clutter; is it still true?
  texture_->SetVisibility(false);
  compositor_->GetDefaultStage()->AddActor(texture_);
}

}  // namespace window_manager
