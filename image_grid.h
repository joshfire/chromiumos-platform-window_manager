// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_IMAGE_GRID_H_
#define WINDOW_MANAGER_IMAGE_GRID_H_

#include <string>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/geometry.h"

namespace window_manager {

// An ImageGrid is a 3x3 array of Compositor::ImageActor objects.
//
// As the grid is resized, its actors fill the requested space:
// - corner actors are not scaled
// - top and bottom actors are scaled horizontally
// - left and right actors are scaled vertically
// - center actor is scaled in both directions
//
// If one of the non-center actors is smaller than the largest actor in its
// row or column, it will be aligned with the outside of the grid.  For
// example, given 4x4 top-left and top-right actors and a 1x2 top actor:
//
//   +--------+---------------------+--------+
//   |        |         top         |        |
//   | top-   +---------------------+  top-  +
//   | left   |                     | right  |
//   +----+---+                     +---+----+
//   |    |                             |    |
//   ...
//
// This may seem odd at first, but it lets ImageGrid be used to draw shadows
// with curved corners that extend inwards beyond a window's borders.  In the
// below example, the top-left corner image is overlayed on top of the window's
// top-left corner:
//
//   +---------+-----------------------
//   |    ..xxx|XXXXXXXXXXXXXXXXXX
//   |  .xXXXXX|XXXXXXXXXXXXXXXXXX_____
//   | .xXX    |                    ^ window's top edge
//   | .xXX    |
//   +---------+
//   | xXX|
//   | xXX|< window's left edge
//   | xXX|
//   ...
//
class ImageGrid {
 public:
  // Adding the grid to the compositor's stage is the caller's responsibility.
  ImageGrid(Compositor* compositor);
  ~ImageGrid();

  int top_height() const { return top_height_; }
  int bottom_height() const { return bottom_height_; }
  int left_width() const { return left_width_; }
  int right_width() const { return right_width_; }

  // Get the sizes of various actors, or 0 if they're unset.
  // Used by the Shadow class.
  int top_actor_height() const {
    return top_actor_.get() ? top_actor_->GetHeight() : 0;
  }
  int bottom_actor_height() const {
    return bottom_actor_.get() ? bottom_actor_->GetHeight() : 0;
  }
  int left_actor_width() const {
    return left_actor_.get() ? left_actor_->GetWidth() : 0;
  }
  int right_actor_width() const {
    return right_actor_.get() ? right_actor_->GetWidth() : 0;
  }

  // Construct a grid using images loaded from a directory on disk.
  // We look for the following files within |images_dir|:
  //
  //   top.png       bottom.png     left.png         right.png
  //   top_left.png  top_right.png  bottom_left.png  bottom_right.png
  //   center.png
  //
  // Missing images are skipped.
  void InitFromFiles(const std::string& images_dir);

  // Construct a grid using ImageActors cloned from an existing grid.
  // This can be used to avoid loading the same files from disk repeatedly for
  // common sets of images (e.g. shadows).
  void InitFromExisting(const ImageGrid& src);

  // Get the actor that can be used to add the grid to a stage, move it, stack
  // it, change its opacity, etc.
  Compositor::Actor* group() const { return group_.get(); }

  const Size& size() const { return size_; }

  // Resize the grid over |anim_ms| milliseconds.
  void Resize(const Size& size, int anim_ms);

 private:
  friend class ImageGridTest;
  FRIEND_TEST(ImageGridTest, Basic);
  FRIEND_TEST(ImageGridTest, SingleImage);
  FRIEND_TEST(ImageGridTest, SmallerSides);
  FRIEND_TEST(ImageGridTest, InitFromExisting);

  // Names of the different image files that we expect to find in a directory.
  static const char kTopFilename[];
  static const char kBottomFilename[];
  static const char kLeftFilename[];
  static const char kRightFilename[];
  static const char kTopLeftFilename[];
  static const char kTopRightFilename[];
  static const char kBottomLeftFilename[];
  static const char kBottomRightFilename[];
  static const char kCenterFilename[];

  // Helper method for InitFromFiles().  Given an image directory and the
  // base name of an image file, creates and returns a new ImageActor
  // (added to |group_|) if the file exists or NULL if it doesn't.
  Compositor::Actor* CreateActor(const std::string& images_dir,
                                 const std::string& filename);

  // Helper method for InitFromExisting().  If |src| is NULL, returns NULL.
  // Otherwise, clones it, adds the new actor to |group_|, and returns the
  // new actor.
  Compositor::Actor* CloneActor(Compositor::Actor* src);

  Compositor* compositor_;  // not owned

  // Has InitFromFiles() or InitFromExisting() been called?
  bool initialized_;

  // The grid's current size.  Used for testing.
  Size size_;

  // Sizes of the tallest image in the top and bottom rows and the widest in the
  // left and right columns.
  int top_height_;
  int bottom_height_;
  int left_width_;
  int right_width_;

  // Group containing the image actors.
  scoped_ptr<Compositor::ContainerActor> group_;

  // ImageActors displayed within the grid.
  scoped_ptr<Compositor::Actor> top_actor_;
  scoped_ptr<Compositor::Actor> bottom_actor_;
  scoped_ptr<Compositor::Actor> left_actor_;
  scoped_ptr<Compositor::Actor> right_actor_;
  scoped_ptr<Compositor::Actor> top_left_actor_;
  scoped_ptr<Compositor::Actor> top_right_actor_;
  scoped_ptr<Compositor::Actor> bottom_left_actor_;
  scoped_ptr<Compositor::Actor> bottom_right_actor_;
  scoped_ptr<Compositor::Actor> center_actor_;

  DISALLOW_COPY_AND_ASSIGN(ImageGrid);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_IMAGE_GRID_H_
