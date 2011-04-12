// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/file_util.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/geometry.h"
#include "window_manager/image_grid.h"
#include "window_manager/test_lib.h"
#include "window_manager/x11/mock_x_connection.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::string;

namespace window_manager {

class ImageGridTest : public ::testing::Test {
 protected:
  // Directory where our test image files are stored.
  static const char kImageDir[];

  // Names of test image files in |kImageDir|.
  static const char k1x1Filename[];
  static const char k1x2Filename[];
  static const char k2x1Filename[];
  static const char k2x2Filename[];

  virtual void SetUp() {
    xconn_.reset(new MockXConnection);
    compositor_.reset(new MockCompositor(xconn_.get()));
    compositor_->set_should_load_images(true);
    dir_.reset(new ScopedTempDirectory);
  }

  // Copy one of our test images into |dir_|.
  // |src_filename| should be one of the k1x1Filename, etc. strings from above.
  // |dest_filename| should be one of the kTopFilename, etc. strings from
  // ImageGrid.
  void CopyImage(const string& src_filename, const string& dest_filename) {
    FilePath src_path = FilePath(kImageDir).Append(src_filename);
    FilePath dest_path = dir_->path().Append(dest_filename);
    CHECK(file_util::CopyFile(src_path, dest_path))
        << "Failed to copy " << src_path.value() << " to " << dest_path.value();
  }

  scoped_ptr<MockXConnection> xconn_;
  scoped_ptr<MockCompositor> compositor_;
  scoped_ptr<ScopedTempDirectory> dir_;
};

const char ImageGridTest::kImageDir[] = "data/image_grid";
const char ImageGridTest::k1x1Filename[] = "1x1.png";
const char ImageGridTest::k1x2Filename[] = "1x2.png";
const char ImageGridTest::k2x1Filename[] = "2x1.png";
const char ImageGridTest::k2x2Filename[] = "2x2.png";

// Test that an ImageGrid's actors are moved and scaled correctly when Resize()
// is called.
TEST_F(ImageGridTest, Basic) {
  CopyImage(k1x2Filename, ImageGrid::kTopFilename);
  CopyImage(k1x2Filename, ImageGrid::kBottomFilename);
  CopyImage(k2x1Filename, ImageGrid::kLeftFilename);
  CopyImage(k2x1Filename, ImageGrid::kRightFilename);
  CopyImage(k2x2Filename, ImageGrid::kTopLeftFilename);
  CopyImage(k2x2Filename, ImageGrid::kTopRightFilename);
  CopyImage(k2x2Filename, ImageGrid::kBottomLeftFilename);
  CopyImage(k2x2Filename, ImageGrid::kBottomRightFilename);
  CopyImage(k1x1Filename, ImageGrid::kCenterFilename);

  ImageGrid grid(compositor_.get());
  grid.InitFromFiles(dir_->path().value());
  ASSERT_TRUE(grid.top_actor_.get() != NULL);
  ASSERT_TRUE(grid.bottom_actor_.get() != NULL);
  ASSERT_TRUE(grid.left_actor_.get() != NULL);
  ASSERT_TRUE(grid.right_actor_.get() != NULL);
  ASSERT_TRUE(grid.top_left_actor_.get() != NULL);
  ASSERT_TRUE(grid.top_right_actor_.get() != NULL);
  ASSERT_TRUE(grid.bottom_left_actor_.get() != NULL);
  ASSERT_TRUE(grid.bottom_right_actor_.get() != NULL);
  ASSERT_TRUE(grid.center_actor_.get() != NULL);

  const int kWidth = 20;
  const int kHeight = 30;
  grid.Resize(Size(kWidth, kHeight), 0);

  // The top-left actor should be flush with the top-left corner and unscaled.
  EXPECT_EQ(Point(0, 0), grid.top_left_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(1.0, grid.top_left_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(1.0, grid.top_left_actor_.get()->GetYScale());

  // The top actor should be flush with the top edge and stretched horizontally
  // between the two top corners.
  EXPECT_EQ(Point(2, 0), grid.top_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(static_cast<double>(kWidth - 4),
                   grid.top_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(1.0, grid.top_actor_.get()->GetYScale());

  // The top-right actor should be flush with the top-right corner and unscaled.
  EXPECT_EQ(Point(kWidth - 2, 0),
            grid.top_right_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(1.0, grid.top_right_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(1.0, grid.top_right_actor_.get()->GetYScale());

  // The left actor should be flush with the left edge and stretched vertically
  // between the two left corners.
  EXPECT_EQ(Point(0, 2), grid.left_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(1.0, grid.left_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(static_cast<double>(kHeight - 4),
                   grid.left_actor_.get()->GetYScale());

  // The center actor should fill the space in the middle of the grid.
  EXPECT_EQ(Point(2, 2), grid.center_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(static_cast<double>(kWidth - 4),
                   grid.center_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(static_cast<double>(kHeight - 4),
                   grid.center_actor_.get()->GetYScale());

  // The right actor should be flush with the right edge and stretched
  // vertically between the two right corners.
  EXPECT_EQ(Point(kWidth - 2, 2),
            grid.right_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(1.0, grid.right_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(static_cast<double>(kHeight - 4),
                   grid.right_actor_.get()->GetYScale());

  // The bottom-left actor should be flush with the bottom-left corner and
  // unscaled.
  EXPECT_EQ(Point(0, kHeight - 2),
            grid.bottom_left_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(1.0, grid.bottom_left_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(1.0, grid.bottom_left_actor_.get()->GetYScale());

  // The bottom actor should be flush with the bottom edge and stretched
  // horizontally between the two bottom corners.
  EXPECT_EQ(Point(2, kHeight - 2),
            grid.bottom_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(static_cast<double>(kWidth - 4),
                   grid.bottom_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(1.0, grid.bottom_actor_.get()->GetYScale());

  // The bottom-right actor should be flush with the bottom-right corner and
  // unscaled.
  EXPECT_EQ(Point(kWidth - 2, kHeight - 2),
            grid.bottom_right_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(1.0, grid.bottom_right_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(1.0, grid.bottom_right_actor_.get()->GetYScale());
}

// Check that we don't crash if only a single image is supplied.
TEST_F(ImageGridTest, SingleImage) {
  CopyImage(k1x1Filename, ImageGrid::kTopFilename);

  ImageGrid grid(compositor_.get());
  grid.InitFromFiles(dir_->path().value());
  ASSERT_TRUE(grid.top_actor_.get() != NULL);
  EXPECT_TRUE(grid.bottom_actor_.get() == NULL);
  EXPECT_TRUE(grid.left_actor_.get() == NULL);
  EXPECT_TRUE(grid.right_actor_.get() == NULL);
  EXPECT_TRUE(grid.top_left_actor_.get() == NULL);
  EXPECT_TRUE(grid.top_right_actor_.get() == NULL);
  EXPECT_TRUE(grid.bottom_left_actor_.get() == NULL);
  EXPECT_TRUE(grid.bottom_right_actor_.get() == NULL);
  EXPECT_TRUE(grid.center_actor_.get() == NULL);

  // The top actor should be scaled horizontally across the entire width, but it
  // shouldn't be scaled vertically.
  Size kSize(10, 10);
  grid.Resize(kSize, 0);
  EXPECT_EQ(Point(0, 0), grid.top_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(static_cast<double>(kSize.width),
                   grid.top_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(1.0, grid.top_actor_.get()->GetYScale());
}

// Test that side (top, left, right, bottom) actors that are narrower than their
// adjacent corner actors stay pinned to the outside edges instead of getting
// moved inwards or scaled.  This exercises the scenario used for shadows.
TEST_F(ImageGridTest, SmallerSides) {
  CopyImage(k1x1Filename, ImageGrid::kTopFilename);
  CopyImage(k1x1Filename, ImageGrid::kLeftFilename);
  CopyImage(k1x1Filename, ImageGrid::kRightFilename);
  CopyImage(k2x2Filename, ImageGrid::kTopLeftFilename);
  CopyImage(k2x2Filename, ImageGrid::kTopRightFilename);

  ImageGrid grid(compositor_.get());
  grid.InitFromFiles(dir_->path().value());
  const int kWidth = 20;
  const int kHeight = 30;
  grid.Resize(Size(kWidth, kHeight), 0);

  // The top actor should be flush with the top edge and stretched horizontally
  // between the two top corners.
  EXPECT_EQ(Point(2, 0), grid.top_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(static_cast<double>(kWidth - 4),
                   grid.top_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(1.0, grid.top_actor_.get()->GetYScale());

  // The left actor should be flush with the left edge and stretched vertically
  // between the top left corner and the bottom.
  EXPECT_EQ(Point(0, 2), grid.left_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(1.0, grid.left_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(static_cast<double>(kHeight - 2),
                   grid.left_actor_.get()->GetYScale());

  // The right actor should be flush with the right edge and stretched
  // vertically between the top right corner and the bottom.
  EXPECT_EQ(Point(kWidth - grid.right_actor_.get()->GetWidth(), 2),
            grid.right_actor_.get()->GetBounds().position());
  EXPECT_DOUBLE_EQ(1.0, grid.right_actor_.get()->GetXScale());
  EXPECT_DOUBLE_EQ(static_cast<double>(kHeight - 2),
                   grid.right_actor_.get()->GetYScale());
}

// Test that the InitFromExisting() method works.
TEST_F(ImageGridTest, InitFromExisting) {
  const int kCornerSize = 2;
  CopyImage(k1x2Filename, ImageGrid::kTopFilename);
  CopyImage(k1x2Filename, ImageGrid::kBottomFilename);
  CopyImage(k2x1Filename, ImageGrid::kLeftFilename);
  CopyImage(k2x1Filename, ImageGrid::kRightFilename);
  CopyImage(k2x2Filename, ImageGrid::kTopLeftFilename);
  CopyImage(k2x2Filename, ImageGrid::kTopRightFilename);
  CopyImage(k2x2Filename, ImageGrid::kBottomLeftFilename);
  CopyImage(k2x2Filename, ImageGrid::kBottomRightFilename);
  CopyImage(k1x1Filename, ImageGrid::kCenterFilename);

  // Create a grid using images loaded from disk.
  ImageGrid grid1(compositor_.get());
  grid1.InitFromFiles(dir_->path().value());

  // Create a second grid that clones the images from the first one.
  ImageGrid grid2(compositor_.get());
  grid2.InitFromExisting(grid1);

  // Now resize both grids to different sizes and check that their bottom-right
  // actors are at different positions (i.e. they're actually distinct actors
  // and we didn't just reuse the same actor from the first grid or something
  // dumb like that).
  Size size1(10, 20);
  grid1.Resize(size1, 0);

  Size size2(30, 40);
  grid2.Resize(size2, 0);

  EXPECT_EQ(Point(size1.width - kCornerSize, size1.height - kCornerSize),
            grid1.bottom_right_actor_.get()->GetBounds().position());
  EXPECT_EQ(Point(size2.width - kCornerSize, size2.height - kCornerSize),
            grid2.bottom_right_actor_.get()->GetBounds().position());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
