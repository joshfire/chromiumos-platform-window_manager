// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "base/string_util.h"
#include "window_manager/compositor.h"
#include "window_manager/test_lib.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

namespace window_manager {

class CompositorTest : public ::testing::Test {};

TEST_F(CompositorTest, HexColors) {
  Compositor::Color color;

  ASSERT_TRUE(color.SetHex("#ffffff"));
  EXPECT_FLOAT_EQ(1.0f, color.red);
  EXPECT_FLOAT_EQ(1.0f, color.green);
  EXPECT_FLOAT_EQ(1.0f, color.blue);

  ASSERT_TRUE(color.SetHex("#a03"));
  EXPECT_FLOAT_EQ(170.0f / 255, color.red);
  EXPECT_FLOAT_EQ(0.0f, color.green);
  EXPECT_FLOAT_EQ(51.0f / 255, color.blue);

  ASSERT_TRUE(color.SetHex("49A31B"));
  EXPECT_FLOAT_EQ(73.0f / 255, color.red);
  EXPECT_FLOAT_EQ(163.0f / 255, color.green);
  EXPECT_FLOAT_EQ(27.0f / 255, color.blue);

  ASSERT_TRUE(color.SetHex("000"));
  EXPECT_FLOAT_EQ(0.0f, color.red);
  EXPECT_FLOAT_EQ(0.0f, color.green);
  EXPECT_FLOAT_EQ(0.0f, color.blue);

  EXPECT_FALSE(color.SetHex(""));
  EXPECT_FALSE(color.SetHex("11"));
  EXPECT_FALSE(color.SetHex("1111"));
  EXPECT_FALSE(color.SetHex("11111"));
  EXPECT_FALSE(color.SetHex("1111111"));
  EXPECT_FALSE(color.SetHex("45g"));
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
