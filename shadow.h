// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_SHADOW_H_
#define WINDOW_MANAGER_SHADOW_H_

#include <string>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "window_manager/compositor.h"
#include "window_manager/util.h"

namespace window_manager {

// This class displays a drop shadow that can be positioned under a window.
//
// This is a bit trickier than just scaling a single textured actor.  We
// want shadows to have the same weight regardless of their dimensions, so
// we arrange eight actors (corners and top/bottom/sides) around the
// window, scaling the top/bottom/sides as needed.  A group containing all
// of the shadow's actors is exposed for adding to containers or
// restacking.
class Shadow {
 public:
  // The shadow is hidden when first created.
  explicit Shadow(Compositor* compositor);
  ~Shadow() {}

  bool is_shown() const { return is_shown_; }
  double opacity() const { return opacity_; }

  // Get the group containing all of the actors.
  Compositor::Actor* group() const { return group_.get(); }

  void Show();
  void Hide();
  void Move(int x, int y, int anim_ms);
  void MoveX(int x, int anim_ms);
  void MoveY(int y, int anim_ms);
  void Resize(int width, int height, int anim_ms);
  void SetOpacity(double opacity, int anim_ms);

 private:
  FRIEND_TEST(ShadowTest, Basic);

  // Initialize static members.  Called the first time that the constructor
  // is invoked.
  void Init();

  // Helper method for Init().  Given the base name of an image file,
  // creates and returns a new texture actor.
  Compositor::Actor* InitTexture(const std::string& filename);

  // Static Compositor::TextureActors that we clone for each shadow.
  static Compositor::Actor* top_texture_;
  static Compositor::Actor* bottom_texture_;
  static Compositor::Actor* left_texture_;
  static Compositor::Actor* right_texture_;
  static Compositor::Actor* tl_texture_;
  static Compositor::Actor* tr_texture_;
  static Compositor::Actor* bl_texture_;
  static Compositor::Actor* br_texture_;

  // Size in pixels of one side of the transparent inset area in corner
  // images.
  //
  //   +---------+
  //   |   ...xxx|  For example, in the top-left corner image depicted
  //   | .xxXXXXX|  to the left, the inset would be the size of the
  //   | .xXX    |  transparent area in the lower right that should be
  //   | .xXX    |  overlayed over the client window.  This area must
  //   +---------+  be square.
  static int kInset;

  // Width in pixels of the shadow along various edges.
  static int kTopHeight;
  static int kBottomHeight;
  static int kLeftWidth;
  static int kRightWidth;

  Compositor* compositor_;  // not owned

  // These are just used by tests.
  bool is_shown_;
  double opacity_;

  // Group containing corner and top/bottom/side actors.
  scoped_ptr<Compositor::ContainerActor> group_;

  // Per-instance clones of '*_texture_' actors.
  scoped_ptr<Compositor::Actor> top_actor_;
  scoped_ptr<Compositor::Actor> bottom_actor_;
  scoped_ptr<Compositor::Actor> left_actor_;
  scoped_ptr<Compositor::Actor> right_actor_;
  scoped_ptr<Compositor::Actor> tl_actor_;
  scoped_ptr<Compositor::Actor> tr_actor_;
  scoped_ptr<Compositor::Actor> bl_actor_;
  scoped_ptr<Compositor::Actor> br_actor_;

  DISALLOW_COPY_AND_ASSIGN(Shadow);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_SHADOW_H_
