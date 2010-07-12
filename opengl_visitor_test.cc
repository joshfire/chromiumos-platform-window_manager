// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cstdarg>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/command_line.h"
#include "base/scoped_ptr.h"
#include "base/logging.h"
#include "window_manager/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/geometry.h"
#include "window_manager/opengl_visitor.h"
#include "window_manager/mock_gl_interface.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/real_compositor.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using window_manager::util::NextPowerOfTwo;

namespace window_manager {

class OpenGlVisitorTest : public ::testing::Test {
 public:
  OpenGlVisitorTest()
      : gl_interface_(new MockGLInterface),
        x_connection_(new MockXConnection),
        event_loop_(new EventLoop),
        compositor_(new RealCompositor(event_loop_.get(),
                                       x_connection_.get(),
                                       gl_interface_.get())) {
  }
  virtual ~OpenGlVisitorTest() {
  }

  RealCompositor* compositor() { return compositor_.get(); }
  MockGLInterface* gl_interface() { return gl_interface_.get(); }

 private:
  scoped_ptr<MockGLInterface> gl_interface_;
  scoped_ptr<MockXConnection> x_connection_;
  scoped_ptr<EventLoop> event_loop_;
  scoped_ptr<RealCompositor> compositor_;
};

class OpenGlVisitorTestTree : public OpenGlVisitorTest {
 public:
  OpenGlVisitorTestTree() {}
  virtual ~OpenGlVisitorTestTree() {}

  void SetUp() {
    // Create an actor tree to test.
    stage_ = compositor()->GetDefaultStage();
    group1_.reset(compositor()->CreateGroup());
    group2_.reset(compositor()->CreateGroup());
    group3_.reset(compositor()->CreateGroup());
    group4_.reset(compositor()->CreateGroup());
    rect1_.reset(compositor()->CreateRectangle(Compositor::Color(),
                                               Compositor::Color(), 0));
    rect2_.reset(compositor()->CreateRectangle(Compositor::Color(),
                                               Compositor::Color(), 0));
    rect3_.reset(compositor()->CreateRectangle(Compositor::Color(),
                                               Compositor::Color(), 0));

    rect1_->SetSize(stage_->GetWidth(), stage_->GetHeight());
    rect2_->SetSize(stage_->GetWidth(), stage_->GetHeight());
    rect3_->SetSize(stage_->GetWidth(), stage_->GetHeight());

    stage_->SetName("stage");
    group1_->SetName("group1");
    group2_->SetName("group2");
    group3_->SetName("group3");
    group4_->SetName("group4");
    rect1_->SetName("rect1");
    rect2_->SetName("rect2");
    rect3_->SetName("rect3");

    //     stage (0)
    //     |          |
    // group1(256)  group3(1024)
    //    |            |
    // group2(512)    group4(1280)
    //   |              |      |
    // rect1(768)  rect2(1536) rect3(1792)

    // depth order (furthest to nearest) should be:
    // rect3 = 1792
    // rect2 = 1536
    // group4 = 1280
    // group3 = 1024
    // rect1 = 768
    // group2 = 512
    // group1 = 256
    // stage = 0

    stage_->AddActor(group1_.get());
    stage_->AddActor(group3_.get());
    group1_->AddActor(group2_.get());
    group2_->AddActor(rect1_.get());
    group3_->AddActor(group4_.get());
    group4_->AddActor(rect2_.get());
    group4_->AddActor(rect3_.get());
  }

  void TearDown() {
    // This is in reverse order of creation on purpose...
    rect3_.reset(NULL);
    rect2_.reset(NULL);
    group4_.reset(NULL);
    rect1_.reset(NULL);
    group2_.reset(NULL);
    group3_.reset(NULL);
    group1_.reset(NULL);
    stage_ = NULL;
  }

 protected:
  RealCompositor::StageActor* stage_;
  scoped_ptr<RealCompositor::ContainerActor> group1_;
  scoped_ptr<RealCompositor::ContainerActor> group2_;
  scoped_ptr<RealCompositor::ContainerActor> group3_;
  scoped_ptr<RealCompositor::ContainerActor> group4_;
  scoped_ptr<RealCompositor::Actor> rect1_;
  scoped_ptr<RealCompositor::Actor> rect2_;
  scoped_ptr<RealCompositor::Actor> rect3_;
};

TEST_F(OpenGlVisitorTestTree, LayerDepth) {
  int32 count = 0;
  stage_->Update(&count, 0LL);
  EXPECT_EQ(8, count);
  compositor()->set_actor_count(count);

  OpenGlDrawVisitor visitor(gl_interface(), compositor(), stage_);
  stage_->Accept(&visitor);

  // Code uses a depth range of kMinDepth to kMaxDepth.  Layers are
  // disributed evenly within that range, except we don't use the
  // frontmost or backmost values in that range.
  uint32 max_count = NextPowerOfTwo(static_cast<uint32>(8 + 2));
  float thickness = (RealCompositor::LayerVisitor::kMaxDepth -
                     RealCompositor::LayerVisitor::kMinDepth) / max_count;
  float depth = RealCompositor::LayerVisitor::kMinDepth + thickness;

  EXPECT_FLOAT_EQ(depth, rect3_->z());
  depth += thickness;
  EXPECT_TRUE(rect2_->culled());
  EXPECT_FLOAT_EQ(depth, group4_->z());
  depth += thickness;
  EXPECT_FLOAT_EQ(depth, group3_->z());
  depth += thickness;
  EXPECT_TRUE(rect1_->culled());
  EXPECT_FLOAT_EQ(depth, group2_->z());
  depth += thickness;
  EXPECT_FLOAT_EQ(depth, group1_->z());
}

// Check that the viewport gets resized correctly when the stage is resized.
TEST_F(OpenGlVisitorTest, ResizeViewport) {
  RealCompositor::StageActor* stage = compositor()->GetDefaultStage();
  OpenGlDrawVisitor visitor(gl_interface(), compositor(), stage);

  stage->Accept(&visitor);
  EXPECT_EQ(0, gl_interface()->viewport().x);
  EXPECT_EQ(0, gl_interface()->viewport().y);
  EXPECT_EQ(stage->width(), gl_interface()->viewport().width);
  EXPECT_EQ(stage->height(), gl_interface()->viewport().height);

  int new_width = stage->width() + 20;
  int new_height = stage->height() + 10;
  stage->SetSize(new_width, new_height);

  stage->Accept(&visitor);
  EXPECT_EQ(0, gl_interface()->viewport().x);
  EXPECT_EQ(0, gl_interface()->viewport().y);
  EXPECT_EQ(new_width, gl_interface()->viewport().width);
  EXPECT_EQ(new_height, gl_interface()->viewport().height);
}

// Check that the stage background color is used.
TEST_F(OpenGlVisitorTest, StageColor) {
  RealCompositor::StageActor* stage = compositor()->GetDefaultStage();

  stage->SetStageColor(Compositor::Color(0.f, 0.f, 0.f));
  OpenGlDrawVisitor visitor(gl_interface(), compositor(), stage);
  stage->Accept(&visitor);
  EXPECT_FLOAT_EQ(0.f, gl_interface()->clear_red());
  EXPECT_FLOAT_EQ(0.f, gl_interface()->clear_green());
  EXPECT_FLOAT_EQ(0.f, gl_interface()->clear_blue());
  EXPECT_FLOAT_EQ(1.f, gl_interface()->clear_alpha());

  const float new_red = 0.86f, new_green = 0.24f, new_blue = 0.51f;
  stage->SetStageColor(Compositor::Color(new_red, new_green, new_blue));
  stage->Accept(&visitor);
  EXPECT_FLOAT_EQ(new_red, gl_interface()->clear_red());
  EXPECT_FLOAT_EQ(new_green, gl_interface()->clear_green());
  EXPECT_FLOAT_EQ(new_blue, gl_interface()->clear_blue());
  EXPECT_FLOAT_EQ(1.f, gl_interface()->clear_alpha());
}

}  // end namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
