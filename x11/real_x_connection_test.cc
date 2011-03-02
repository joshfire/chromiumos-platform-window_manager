// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "window_manager/test_lib.h"
#include "window_manager/x11/real_x_connection.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::vector;

namespace window_manager {

class RealXConnectionTest : public ::testing::Test {};

TEST_F(RealXConnectionTest, GetImageFormat) {
  ImageFormat format = IMAGE_FORMAT_UNKNOWN;

  // Check that we don't support non-32-bit-per-pixel data or drawables
  // with non 24- or 32-bit depths.
  EXPECT_FALSE(RealXConnection::GetImageFormat(true, 0, 32, &format));
  EXPECT_FALSE(RealXConnection::GetImageFormat(true, 24, 32, &format));
  EXPECT_FALSE(RealXConnection::GetImageFormat(true, 40, 32, &format));
  EXPECT_FALSE(RealXConnection::GetImageFormat(true, 32, 0, &format));
  EXPECT_FALSE(RealXConnection::GetImageFormat(true, 32, 16, &format));
  EXPECT_FALSE(RealXConnection::GetImageFormat(true, 32, 40, &format));

  // Now check that we report BGRx for little-endian systems and RGBx for
  // big-endian ones when we have a 24-bit drawable.
  EXPECT_TRUE(RealXConnection::GetImageFormat(true, 32, 24, &format));
  EXPECT_EQ(IMAGE_FORMAT_BGRX_32, format);
  EXPECT_TRUE(RealXConnection::GetImageFormat(false, 32, 24, &format));
  EXPECT_EQ(IMAGE_FORMAT_RGBX_32, format);

  // When we get a drawable with a 32-bit depth, we should report that the
  // data's alpha channel is usable.
  EXPECT_TRUE(RealXConnection::GetImageFormat(true, 32, 32, &format));
  EXPECT_EQ(IMAGE_FORMAT_BGRA_32, format);
  EXPECT_TRUE(RealXConnection::GetImageFormat(false, 32, 32, &format));
  EXPECT_EQ(IMAGE_FORMAT_RGBA_32, format);
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
