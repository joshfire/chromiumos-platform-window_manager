// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/image_grid.h"
#include "window_manager/shadow.h"
#include "window_manager/test_lib.h"
#include "window_manager/x11/mock_x_connection.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

DECLARE_string(rectangular_shadow_image_dir);  // from shadow.cc

namespace window_manager {

class ShadowTest : public ::testing::Test {
 protected:
  virtual void SetUp() {
    xconn_.reset(new MockXConnection);
    compositor_.reset(new MockCompositor(xconn_.get()));
    compositor_->set_should_load_images(true);
  }

  scoped_ptr<MockXConnection> xconn_;
  scoped_ptr<MockCompositor> compositor_;
};

TEST_F(ShadowTest, Basic) {
  const int kShadowTopHeight = 2;
  const int kShadowBottomHeight = 6;
  const int kShadowLeftWidth = 4;
  const int kShadowRightWidth = 4;
  scoped_ptr<Shadow> shadow(
      Shadow::Create(compositor_.get(), Shadow::TYPE_RECTANGULAR));

  Rect kBounds(10, 20, 200, 100);
  shadow->Move(kBounds.x, kBounds.y, 0);
  shadow->Resize(kBounds.width, kBounds.height, 0);

  double kOpacity = 0.75;
  shadow->SetOpacity(kOpacity, 0);

  // Check the group's position and scale.  It should be offset based on the
  // size of the shadow's top and left edges.
  EXPECT_EQ(Point(kBounds.x - kShadowLeftWidth,
                  kBounds.y - kShadowTopHeight),
            shadow->group()->GetBounds().position());
  EXPECT_DOUBLE_EQ(1.0, shadow->group()->GetXScale());
  EXPECT_DOUBLE_EQ(1.0, shadow->group()->GetYScale());

  // Check the size of the ImageGrid.  It should be the size of the window plus
  // the size of the shadow's edges.
  EXPECT_EQ(Size(kBounds.width + kShadowLeftWidth + kShadowRightWidth,
                 kBounds.height + kShadowTopHeight + kShadowBottomHeight),
            shadow->grid_->size());

  EXPECT_DOUBLE_EQ(kOpacity, shadow->group()->GetOpacity());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  // We need the image files for rectangular shadows to be present so that
  // the image actors will be initialized.
  FLAGS_rectangular_shadow_image_dir = "data/rectangular_shadow";
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
