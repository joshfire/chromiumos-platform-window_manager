// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "window_manager/test_lib.h"
#include "window_manager/real_x_connection.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::vector;

namespace window_manager {

class RealXConnectionTest : public ::testing::Test {};

TEST_F(RealXConnectionTest, GetImageFormatFromColorMasks) {
  ImageFormat format = IMAGE_FORMAT_RGBA_32;

  // Check that we don't support non-32-bit-per-pixel data or drawables
  // with non 24- or 32-bit depths.
  EXPECT_FALSE(RealXConnection::GetImageFormatFromColorMasks(
      true, 0, 0xff, 0xff00, 0xff0000, 32, &format));
  EXPECT_FALSE(RealXConnection::GetImageFormatFromColorMasks(
      true, 24, 0xff, 0xff00, 0xff0000, 32, &format));
  EXPECT_FALSE(RealXConnection::GetImageFormatFromColorMasks(
      true, 40, 0xff, 0xff00, 0xff0000, 32, &format));
  EXPECT_FALSE(RealXConnection::GetImageFormatFromColorMasks(
      true, 32, 0xff, 0xff00, 0xff0000, 0, &format));
  EXPECT_FALSE(RealXConnection::GetImageFormatFromColorMasks(
      true, 32, 0xff, 0xff00, 0xff0000, 16, &format));
  EXPECT_FALSE(RealXConnection::GetImageFormatFromColorMasks(
      true, 32, 0xff, 0xff00, 0xff0000, 40, &format));

  // Test some nonsensical masks (no bits for each color, or all bits for
  // each color).
  EXPECT_FALSE(RealXConnection::GetImageFormatFromColorMasks(
      true, 32, 0, 0, 0, 32, &format));
  EXPECT_FALSE(RealXConnection::GetImageFormatFromColorMasks(
      true, 32, 0xffffffff, 0xffffffff, 0xffffffff, 32, &format));

  // Unsupported formats like xBGR should also fail.
  EXPECT_FALSE(RealXConnection::GetImageFormatFromColorMasks(
      true, 32, 0xff000000, 0xff0000, 0xff00, 24, &format));
  EXPECT_FALSE(RealXConnection::GetImageFormatFromColorMasks(
      false, 32, 0xff, 0xff00, 0xff0000, 24, &format));

  // Now check that we recognize RGBx and BGRx for both little- and
  // big-endian systems.
  EXPECT_TRUE(RealXConnection::GetImageFormatFromColorMasks(
      true, 32, 0xff, 0xff00, 0xff0000, 24, &format));
  EXPECT_EQ(IMAGE_FORMAT_RGBX_32, format);
  EXPECT_TRUE(RealXConnection::GetImageFormatFromColorMasks(
      true, 32, 0xff0000, 0xff00, 0xff, 24, &format));
  EXPECT_EQ(IMAGE_FORMAT_BGRX_32, format);

  EXPECT_TRUE(RealXConnection::GetImageFormatFromColorMasks(
      false, 32, 0xff000000, 0xff0000, 0xff00, 24, &format));
  EXPECT_EQ(IMAGE_FORMAT_RGBX_32, format);
  EXPECT_TRUE(RealXConnection::GetImageFormatFromColorMasks(
      false, 32, 0xff00, 0xff0000, 0xff000000, 24, &format));
  EXPECT_EQ(IMAGE_FORMAT_BGRX_32, format);

  // When we get a drawable with a 32-bit depth, we should report that the
  // data's alpha channel is usable.
  EXPECT_TRUE(RealXConnection::GetImageFormatFromColorMasks(
      true, 32, 0xff, 0xff00, 0xff0000, 32, &format));
  EXPECT_EQ(IMAGE_FORMAT_RGBA_32, format);
  EXPECT_TRUE(RealXConnection::GetImageFormatFromColorMasks(
      true, 32, 0xff0000, 0xff00, 0xff, 32, &format));
  EXPECT_EQ(IMAGE_FORMAT_BGRA_32, format);
  EXPECT_TRUE(RealXConnection::GetImageFormatFromColorMasks(
      false, 32, 0xff000000, 0xff0000, 0xff00, 32, &format));
  EXPECT_EQ(IMAGE_FORMAT_RGBA_32, format);
  EXPECT_TRUE(RealXConnection::GetImageFormatFromColorMasks(
      false, 32, 0xff00, 0xff0000, 0xff000000, 32, &format));
  EXPECT_EQ(IMAGE_FORMAT_BGRA_32, format);
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
