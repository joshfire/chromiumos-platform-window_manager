// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "window_manager/compositor/animation.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using base::TimeDelta;
using base::TimeTicks;
using window_manager::util::CreateTimeTicksFromMs;

namespace window_manager {

typedef BasicWindowManagerTest AnimationTest;

TEST_F(AnimationTest, Basic) {
  // Animate from -10.0 at time 0 to 10.0 at time 20.
  TimeTicks now = CreateTimeTicksFromMs(0);
  Animation anim(-10.0f, now);
  anim.AppendKeyframe(10.0f, TimeDelta::FromMilliseconds(20));

  EXPECT_FALSE(anim.IsDone(now));
  EXPECT_FLOAT_EQ(-10.0f, anim.GetValue(now));

  now = CreateTimeTicksFromMs(5);
  EXPECT_FALSE(anim.IsDone(now));
  EXPECT_FLOAT_EQ(-sqrt(50.0f), anim.GetValue(now));

  now = CreateTimeTicksFromMs(10);
  EXPECT_FALSE(anim.IsDone(now));
  EXPECT_FLOAT_EQ(0.0f, anim.GetValue(now));

  now = CreateTimeTicksFromMs(15);
  EXPECT_FALSE(anim.IsDone(now));
  EXPECT_FLOAT_EQ(sqrt(50.0f), anim.GetValue(now));

  now = CreateTimeTicksFromMs(20);
  EXPECT_TRUE(anim.IsDone(now));
  EXPECT_FLOAT_EQ(10.0f, anim.GetValue(now));

  now = CreateTimeTicksFromMs(25);
  EXPECT_TRUE(anim.IsDone(now));
  EXPECT_FLOAT_EQ(10.0f, anim.GetValue(now));

  EXPECT_FLOAT_EQ(10.0f, anim.GetEndValue());
}

TEST_F(AnimationTest, MultipleKeyframes) {
  TimeTicks now = CreateTimeTicksFromMs(0);
  Animation anim(0, now);
  anim.AppendKeyframe(20, TimeDelta::FromMilliseconds(10));
  anim.AppendKeyframe(60, TimeDelta::FromMilliseconds(20));

  EXPECT_FALSE(anim.IsDone(now));
  EXPECT_FLOAT_EQ(0, anim.GetValue(now));

  now = CreateTimeTicksFromMs(5);
  EXPECT_FALSE(anim.IsDone(now));
  EXPECT_FLOAT_EQ(10, anim.GetValue(now));

  now = CreateTimeTicksFromMs(10);
  EXPECT_FALSE(anim.IsDone(now));
  EXPECT_FLOAT_EQ(20, anim.GetValue(now));

  now = CreateTimeTicksFromMs(20);
  EXPECT_FALSE(anim.IsDone(now));
  EXPECT_FLOAT_EQ(40, anim.GetValue(now));

  now = CreateTimeTicksFromMs(30);
  EXPECT_TRUE(anim.IsDone(now));
  EXPECT_FLOAT_EQ(60, anim.GetValue(now));
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
