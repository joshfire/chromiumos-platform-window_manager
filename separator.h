// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_SEPARATOR_H_
#define WINDOW_MANAGER_SEPARATOR_H_

#include <string>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/layout_manager.h"
#include "window_manager/util.h"

namespace window_manager {

// This class displays a separator that can be positioned between snapshots.
class LayoutManager::Separator {
 public:
  enum State {
    // We're in active mode.
    STATE_ACTIVE_MODE_INVISIBLE,

    // We're in overview mode and the separator should be displayed in the
    // normal manner.
    STATE_OVERVIEW_MODE_NORMAL,
  };

  // The separator is hidden when first created.
  explicit Separator(LayoutManager* layout_manager);
  ~Separator() {}

  void Show();
  void Hide();
  void Move(int x, int y, int anim_ms);
  void MoveX(int x, int anim_ms);
  void MoveY(int y, int anim_ms);
  void Resize(int width, int height, int anim_ms);
  void SetOpacity(double opacity, int anim_ms);

  // These set values that will take effect when UpdateLayout is
  // called.  They are calculated in CalculatePositionsForOverviewMode
  // in the layout manager.
  void SetX(int position) { x_ = position; }
  void SetY(int position) { y_ = position; }

  int GetXPosition() const { return x_; }
  int GetYPosition() const { return y_; }
  int GetWidth() const { return actor_->GetWidth(); }
  int GetHeight() const { return actor_->GetHeight(); }

  // Sets the state of this separator.  UpdateLayout must be called after
  // this to update the layout to match.
  void SetState(State state) {
    if (state_ != state) {
      last_state_ = state;
      state_ = state;
    }
  }

  // Updates the layout of this separator based on its current state.  If
  // animate is true, then animate the window into its new state,
  // otherwise just jump to the new state.
  void UpdateLayout(bool animate);

 private:
  // Initialize static members.  Called the first time that the constructor
  // is invoked.
  void Init();

  // Static texture Actor that we clone for each separator.
  static Compositor::Actor* texture_;

  LayoutManager* layout_manager_;  // not owned
  Compositor* compositor_;  // not owned

  // Per-instance clone of |texture_| actor.
  scoped_ptr<Compositor::Actor> actor_;

  // The current display state.
  State state_;

  // The previous display state.
  State last_state_;

  // Values that are stored until we're ready to apply them (and
  // possibly animate) in UpdateLayout.
  int x_;
  int y_;
  int width_;
  int height_;

  DISALLOW_COPY_AND_ASSIGN(Separator);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_SEPARATOR_H_
