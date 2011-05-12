// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "base/string_split.h"
#include "base/string_util.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using base::SplitString;
using std::list;
using std::string;
using std::vector;

namespace window_manager {

class UtilTest : public ::testing::Test {
 protected:
  // Helper function for the Stacker test.
  // |expected| is a space-separated list of strings in the order in which
  // they should appear in |actual|.
  void CheckStackerOutput(const list<string>& actual,
                          const string& expected) {
    vector<string> expected_parts;
    SplitString(expected, ' ', &expected_parts);
    ASSERT_EQ(actual.size(), expected_parts.size());

    int i = 0;
    for (list<string>::const_iterator it = actual.begin();
         it != actual.end(); ++it, ++i) {
      EXPECT_EQ(*it, expected_parts[i]);
    }
  }
};

TEST_F(UtilTest, Stacker) {
  Stacker<string> stacker;

  stacker.AddOnTop("b");
  stacker.AddOnBottom("c");
  stacker.AddOnTop("a");
  stacker.AddOnBottom("d");
  CheckStackerOutput(stacker.items(), "a b c d");
  EXPECT_EQ(0, stacker.GetIndex("a"));
  EXPECT_EQ(1, stacker.GetIndex("b"));
  EXPECT_EQ(2, stacker.GetIndex("c"));
  EXPECT_EQ(3, stacker.GetIndex("d"));

  stacker.AddBelow("a2", "a");
  stacker.AddBelow("b2", "b");
  stacker.AddBelow("c2", "c");
  stacker.AddBelow("d2", "d");
  CheckStackerOutput(stacker.items(), "a a2 b b2 c c2 d d2");

  stacker.Remove("a");
  stacker.Remove("c");
  stacker.Remove("d2");
  CheckStackerOutput(stacker.items(), "a2 b b2 c2 d");

  EXPECT_EQ(NULL, stacker.GetUnder("not-present"));
  EXPECT_EQ(NULL, stacker.GetUnder("d"));
  const string* str = NULL;
  ASSERT_TRUE((str = stacker.GetUnder("c2")) != NULL);
  EXPECT_EQ("d", *str);
  ASSERT_TRUE((str = stacker.GetUnder("b")) != NULL);
  EXPECT_EQ("b2", *str);
  ASSERT_TRUE((str = stacker.GetUnder("a2")) != NULL);
  EXPECT_EQ("b", *str);

  stacker.AddAbove("a3", "a2");
  stacker.AddAbove("b3", "b2");
  stacker.AddAbove("d3", "d");
  CheckStackerOutput(stacker.items(), "a3 a2 b b3 b2 c2 d3 d");
}

TEST_F(UtilTest, ByteMap) {
  Size size(4, 3);
  ByteMap bytemap(size);
  EXPECT_EQ(size, bytemap.size());
  EXPECT_PRED_FORMAT3(
      BytesAreEqual,
      reinterpret_cast<unsigned const char*>("\x00\x00\x00\x00"
                                             "\x00\x00\x00\x00"
                                             "\x00\x00\x00\x00"),
      bytemap.bytes(),
      size.area());

  // Set a few rectangles that are bogus or fall entirely outside of the
  // region.
  bytemap.SetRectangle(Rect(-size.width, 0, size.width, size.height), 0xff);
  bytemap.SetRectangle(Rect(size.width, 0, size.width, size.height), 0xff);
  bytemap.SetRectangle(Rect(0, -size.height, size.width, size.height), 0xff);
  bytemap.SetRectangle(Rect(0, size.height, size.width, size.height), 0xff);
  bytemap.SetRectangle(Rect(0, 0, size.width, -1), 0xff);
  bytemap.SetRectangle(Rect(0, 0, -1, size.height), 0xff);
  EXPECT_PRED_FORMAT3(
      BytesAreEqual,
      reinterpret_cast<unsigned const char*>("\x00\x00\x00\x00"
                                             "\x00\x00\x00\x00"
                                             "\x00\x00\x00\x00"),
      bytemap.bytes(),
      size.area());

  // Set a few rectangles that partially cover the region and then one
  // that matches its size.
  bytemap.SetRectangle(Rect(-2, -3, 3, 4), 0xf0);
  EXPECT_PRED_FORMAT3(
      BytesAreEqual,
      reinterpret_cast<unsigned const char*>("\xf0\x00\x00\x00"
                                             "\x00\x00\x00\x00"
                                             "\x00\x00\x00\x00"),
      bytemap.bytes(),
      size.area());
  bytemap.SetRectangle(Rect(size.width - 3, size.height - 1, 10, 10), 0xff);
  EXPECT_PRED_FORMAT3(
      BytesAreEqual,
      reinterpret_cast<unsigned const char*>("\xf0\x00\x00\x00"
                                             "\x00\x00\x00\x00"
                                             "\x00\xff\xff\xff"),
      bytemap.bytes(),
      size.area());
  bytemap.SetRectangle(Rect(0, 0, size.width, size.height), 0xaa);
  EXPECT_PRED_FORMAT3(
      BytesAreEqual,
      reinterpret_cast<unsigned const char*>("\xaa\xaa\xaa\xaa"
                                             "\xaa\xaa\xaa\xaa"
                                             "\xaa\xaa\xaa\xaa"),
      bytemap.bytes(),
      size.area());

  // Now clear the map to a particular value.
  bytemap.Clear(0x01);
  EXPECT_PRED_FORMAT3(
      BytesAreEqual,
      reinterpret_cast<unsigned const char*>("\x01\x01\x01\x01"
                                             "\x01\x01\x01\x01"
                                             "\x01\x01\x01\x01"),
      bytemap.bytes(),
      size.area());

  // Copy an equal-sized bytemap.
  bytemap.Clear(0);
  ByteMap equal(size);
  equal.Clear(0x01);
  bytemap.Copy(equal);
  EXPECT_PRED_FORMAT3(
      BytesAreEqual,
      reinterpret_cast<unsigned const char*>("\x01\x01\x01\x01"
                                             "\x01\x01\x01\x01"
                                             "\x01\x01\x01\x01"),
      bytemap.bytes(),
      size.area());

  // Copy a smaller bytemap.
  bytemap.Clear(0);
  ByteMap smaller(Size(3, 2));
  smaller.Clear(0x01);
  bytemap.Copy(smaller);
  EXPECT_PRED_FORMAT3(
      BytesAreEqual,
      reinterpret_cast<unsigned const char*>("\x01\x01\x01\x00"
                                             "\x01\x01\x01\x00"
                                             "\x00\x00\x00\x00"),
      bytemap.bytes(),
      size.area());

  // Copy a larger bytemap.
  bytemap.Clear(0);
  ByteMap larger(Size(5, 5));
  larger.Clear(0x01);
  bytemap.Copy(larger);
  EXPECT_PRED_FORMAT3(
      BytesAreEqual,
      reinterpret_cast<unsigned const char*>("\x01\x01\x01\x01"
                                             "\x01\x01\x01\x01"
                                             "\x01\x01\x01\x01"),
      bytemap.bytes(),
      size.area());

  // Resize the bytemap.
  Size new_size(3, 2);
  bytemap.Resize(new_size);
  bytemap.Clear(0x01);
  EXPECT_PRED_FORMAT3(
      BytesAreEqual,
      reinterpret_cast<unsigned const char*>("\x01\x01\x01"
                                             "\x01\x01\x01"),
      bytemap.bytes(),
      new_size.area());

  // Try to copy an empty bytemap to it and check that we don't crash.
  ByteMap empty(Size(0, 0));
  bytemap.Copy(empty);
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
