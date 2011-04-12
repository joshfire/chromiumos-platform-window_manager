// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
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
#include "window_manager/compositor/compositor.h"
#include "window_manager/geometry.h"

namespace window_manager {

class ImageGrid;

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
  ~Shadow();

  bool is_shown() const { return is_shown_; }
  double opacity() const { return opacity_; }
  int x() const { return x_; }
  int y() const { return y_; }
  int width() const { return width_; }
  int height() const { return height_; }
  Rect bounds() const { return Rect(x_, y_, width_, height_); }

  // Get the group containing all of the actors.
  Compositor::Actor* group() const;

  void Show();
  void Hide();
  void Move(int x, int y, int anim_ms);
  void MoveX(int x, int anim_ms);
  void MoveY(int y, int anim_ms);
  void Resize(int width, int height, int anim_ms);
  void SetOpacity(double opacity, int anim_ms);

  // Get the minimum width or height of an object for which this shadow can
  // be displayed.
  int GetMinWidth() const;
  int GetMinHeight() const;

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

  void InitFromFiles(const std::string& images_dir);
  void InitFromExisting(const Shadow& shadow);

  Compositor* compositor_;  // not owned

  // These are just used by tests.
  bool is_shown_;
  double opacity_;
  int x_;
  int y_;
  int width_;
  int height_;

  // ImageGrid containing the image actors.
  scoped_ptr<ImageGrid> grid_;

  DISALLOW_COPY_AND_ASSIGN(Shadow);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_SHADOW_H_
