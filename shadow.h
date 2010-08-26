// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_SHADOW_H_
#define WINDOW_MANAGER_SHADOW_H_

#include <map>
#include <string>
#include <tr1/memory>

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
  // Different types of shadows that can be created.
  enum Type {
    // Shadow surrounding all edges of a rectangular window.
    TYPE_RECTANGULAR = 0,

    // Shadow surrounding the top and sides of a panel titlebar window
    // (with rounded corners on the top).
    TYPE_PANEL_TITLEBAR,

    // Shadow beneath the the left and right sides of a panel content window.
    TYPE_PANEL_CONTENT,

    // Shadow drawn at the top of a panel content window to simulate the
    // titlebar window casting a shadow on it.
    TYPE_PANEL_SEPARATOR,
  };

  // Create a new shadow, ownership of which is passed to the caller.
  // The shadow is hidden when first created.
  static Shadow* Create(Compositor* compositor, Type type);
  ~Shadow() {}

  bool is_shown() const { return is_shown_; }
  double opacity() const { return opacity_; }
  int x() const { return x_; }
  int y() const { return y_; }
  int width() const { return width_; }
  int height() const { return height_; }

  // Minimum size that the shadow can take without having overlapping images.
  int min_width() const { return left_inset_ + right_inset_; }
  int min_height() const { return top_inset_ + bottom_inset_; }

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

  // Singleton that creates and stores prototypes and uses them to create
  // Shadow objects.
  class Factory {
   public:
    Factory() {}
    ~Factory() {}

    // Create a new shadow, creating a prototype for the shadow's type
    // first if needed.
    Shadow* CreateShadow(Compositor* compositor, Type type);

   private:
    typedef std::map<Type, std::tr1::shared_ptr<Shadow> > PrototypeMap;
    PrototypeMap prototypes_;

    DISALLOW_COPY_AND_ASSIGN(Factory);
  };

  Shadow(Compositor* compositor);

  // Initialize just the image actors and related variables, by loading
  // images from disk, to create a prototype object that can be used by
  // Factory.  'group_' isn't initialized, for instance.
  void InitAsPrototypeFromDisk(const std::string& images_dir);

  // Clone image actors and copy related variables from an existing shadow.
  // Also initializes 'group_' and adds actors to it.
  void InitFromPrototype(Shadow* prototype);

  // Helper method for InitFromImages().  Given an image directory and the
  // base name of an image file, creates and returns a new ImageActor if
  // the file exists, or NULL if it doesn't.
  Compositor::ImageActor* CreateActor(const std::string& images_dir,
                                      const std::string& filename);

  Compositor* compositor_;  // not owned

  // These are just used by tests.
  bool is_shown_;
  double opacity_;
  int x_;
  int y_;
  int width_;
  int height_;

  // Number of pixels that the shadow extends beyond the edge of the window.
  int top_height_;
  int bottom_height_;
  int left_width_;
  int right_width_;

  // Size in pixels of the transparent inset area in corner images.
  //
  //   +---------+
  //   |   ...xxx|  For example, in the top-left corner image depicted
  //   | .xxXXXXX|  to the left, the inset would be the size of the
  //   | .xXX    |  transparent area in the lower right that should be
  //   | .xXX    |  overlayed over the client window.  'left_inset_' is
  //   +---------+  its width and 'top_inset_' is its height.
  int top_inset_;
  int bottom_inset_;
  int left_inset_;
  int right_inset_;

  // Group containing the image actors.
  scoped_ptr<Compositor::ContainerActor> group_;

  // ImageActors used to display the shadow.
  scoped_ptr<Compositor::Actor> top_actor_;
  scoped_ptr<Compositor::Actor> bottom_actor_;
  scoped_ptr<Compositor::Actor> left_actor_;
  scoped_ptr<Compositor::Actor> right_actor_;
  scoped_ptr<Compositor::Actor> top_left_actor_;
  scoped_ptr<Compositor::Actor> top_right_actor_;
  scoped_ptr<Compositor::Actor> bottom_left_actor_;
  scoped_ptr<Compositor::Actor> bottom_right_actor_;

  DISALLOW_COPY_AND_ASSIGN(Shadow);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_SHADOW_H_
