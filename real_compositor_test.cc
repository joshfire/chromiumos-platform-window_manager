// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <string>
#include <tr1/unordered_set>
#include <vector>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/command_line.h"
#include "base/scoped_ptr.h"
#include "base/logging.h"
#include "window_manager/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/mock_gl_interface.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/real_compositor.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::set;
using std::string;
using std::tr1::unordered_set;
using std::vector;
using window_manager::util::NextPowerOfTwo;

namespace window_manager {

class NameCheckVisitor : virtual public RealCompositor::ActorVisitor {
 public:
  NameCheckVisitor() {}
  virtual ~NameCheckVisitor() {}
  virtual void VisitActor(RealCompositor::Actor* actor) {
    results_.push_back(actor->name());
  }
  const vector<string>& results() { return results_; }
 private:
  vector<string> results_;
  DISALLOW_COPY_AND_ASSIGN(NameCheckVisitor);
};

class RealCompositorTest : public ::testing::Test {
 public:
  RealCompositorTest() {}
  virtual ~RealCompositorTest() {}

  virtual void SetUp() {
    gl_interface_.reset(new MockGLInterface);
    xconn_.reset(new MockXConnection);
    event_loop_.reset(new EventLoop);
    compositor_.reset(new RealCompositor(event_loop_.get(),
                                         xconn_.get(),
                                         gl_interface_.get()));
  }

  virtual void TearDown() {
    compositor_.reset();
  }

  RealCompositor* compositor() { return compositor_.get(); }
  MockXConnection* xconn() { return xconn_.get(); }
  EventLoop* event_loop() { return event_loop_.get(); }
  MockGLInterface* gl_interface() { return gl_interface_.get(); }

 private:
  scoped_ptr<MockGLInterface> gl_interface_;
  scoped_ptr<MockXConnection> xconn_;
  scoped_ptr<EventLoop> event_loop_;
  scoped_ptr<RealCompositor> compositor_;
};

class RealCompositorTestTree : public RealCompositorTest {
 public:
  RealCompositorTestTree() {}
  virtual ~RealCompositorTestTree() {}

  virtual void SetUp() {
    RealCompositorTest::SetUp();

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

  virtual void TearDown() {
    // This is in reverse order of creation on purpose...
    rect3_.reset(NULL);
    rect2_.reset(NULL);
    group4_.reset(NULL);
    rect1_.reset(NULL);
    group2_.reset(NULL);
    group3_.reset(NULL);
    group1_.reset(NULL);
    stage_ = NULL;

    RealCompositorTest::TearDown();
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

TEST_F(RealCompositorTestTree, LayerDepth) {
  // Test lower-level layer-setting routines
  int32 count = 0;
  stage_->Update(&count, 0LL);
  EXPECT_EQ(8, count);
  RealCompositor::ActorVector actors;

  // Code uses a depth range of kMinDepth to kMaxDepth.  Layers are
  // disributed evenly within that range, except we don't use the
  // frontmost or backmost values in that range.
  uint32 max_count = NextPowerOfTwo(static_cast<uint32>(count + 2));
  float thickness = (RealCompositor::LayerVisitor::kMaxDepth -
                     RealCompositor::LayerVisitor::kMinDepth) / max_count;
  float depth = RealCompositor::LayerVisitor::kMinDepth + thickness;

  // First we test the layer visitor directly.
  RealCompositor::LayerVisitor layer_visitor(count, false);
  stage_->Accept(&layer_visitor);

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

  // Now we test higher-level layer depth results.
  depth = RealCompositor::LayerVisitor::kMinDepth + thickness;
  compositor()->Draw();
  EXPECT_EQ(8, compositor()->actor_count());

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

TEST_F(RealCompositorTestTree, LayerDepthWithOpacity) {
  rect3_->SetOpacity(0.5f, 0);

  // Test lower-level layer-setting routines
  int32 count = 0;
  stage_->Update(&count, 0LL);
  EXPECT_EQ(8, count);
  RealCompositor::ActorVector actors;

  // Code uses a depth range of kMinDepth to kMaxDepth.  Layers are
  // disributed evenly within that range, except we don't use the
  // frontmost or backmost values in that range.
  uint32 max_count = NextPowerOfTwo(static_cast<uint32>(count + 2));
  float thickness = (RealCompositor::LayerVisitor::kMaxDepth -
                     RealCompositor::LayerVisitor::kMinDepth) / max_count;
  float depth = RealCompositor::LayerVisitor::kMinDepth + thickness;

  // First we test the layer visitor directly.
  RealCompositor::LayerVisitor layer_visitor(count, false);
  stage_->Accept(&layer_visitor);

  EXPECT_FLOAT_EQ(depth, rect3_->z());
  depth += thickness;
  EXPECT_FLOAT_EQ(depth, rect2_->z());
  depth += thickness;
  EXPECT_FLOAT_EQ(depth, group4_->z());
  depth += thickness;
  EXPECT_FLOAT_EQ(depth, group3_->z());
  depth += thickness;
  EXPECT_TRUE(rect1_->culled());
  EXPECT_FLOAT_EQ(depth, group2_->z());
  depth += thickness;
  EXPECT_FLOAT_EQ(depth, group1_->z());

  // Now we test higher-level layer depth results.
  depth = RealCompositor::LayerVisitor::kMinDepth + thickness;
  compositor()->Draw();
  EXPECT_EQ(8, compositor()->actor_count());

  EXPECT_FLOAT_EQ(depth, rect3_->z());
  depth += thickness;
  EXPECT_FLOAT_EQ(depth, rect2_->z());
  depth += thickness;
  EXPECT_FLOAT_EQ(depth, group4_->z());
  depth += thickness;
  EXPECT_FLOAT_EQ(depth, group3_->z());
  depth += thickness;
  EXPECT_TRUE(rect1_->culled());
  EXPECT_FLOAT_EQ(depth, group2_->z());
  depth += thickness;
  EXPECT_FLOAT_EQ(depth, group1_->z());
}

TEST_F(RealCompositorTestTree, ActorVisitor) {
  NameCheckVisitor visitor;
  stage_->Accept(&visitor);

  vector<string> expected;
  expected.push_back("stage");
  expected.push_back("group3");
  expected.push_back("group4");
  expected.push_back("rect3");
  expected.push_back("rect2");
  expected.push_back("group1");
  expected.push_back("group2");
  expected.push_back("rect1");

  const vector<string>& results = visitor.results();
  EXPECT_EQ(expected.size(), results.size());
  // Yes, this could be a loop, but then it gets harder to know which
  // one failed.  And there's only eight of them.
  EXPECT_EQ(expected[0], results[0]);
  EXPECT_EQ(expected[1], results[1]);
  EXPECT_EQ(expected[2], results[2]);
  EXPECT_EQ(expected[3], results[3]);
  EXPECT_EQ(expected[4], results[4]);
  EXPECT_EQ(expected[5], results[5]);
  EXPECT_EQ(expected[6], results[6]);
  EXPECT_EQ(expected[7], results[7]);
}

TEST_F(RealCompositorTestTree, ActorAttributes) {
  RealCompositor::LayerVisitor layer_visitor(compositor()->actor_count(),
                                             false);
  stage_->Accept(&layer_visitor);

  // Make sure width and height set the right parameters.
  rect1_->SetSize(12, 13);
  EXPECT_EQ(12, rect1_->width());
  EXPECT_EQ(13, rect1_->height());

  // Make sure scale is independent of width and height.
  rect1_->Scale(2.0f, 3.0f, 0);
  EXPECT_EQ(2.0f, rect1_->scale_x());
  EXPECT_EQ(3.0f, rect1_->scale_y());
  EXPECT_EQ(12, rect1_->width());
  EXPECT_EQ(13, rect1_->height());

  // Make sure Move isn't relative, and works on both axes.
  rect1_->MoveX(2, 0);
  rect1_->MoveX(2, 0);
  rect1_->MoveY(2, 0);
  rect1_->MoveY(2, 0);
  EXPECT_EQ(2, rect1_->x());
  EXPECT_EQ(2, rect1_->y());
  EXPECT_EQ(12, rect1_->width());
  EXPECT_EQ(13, rect1_->height());
  rect1_->Move(4, 4, 0);
  rect1_->Move(4, 4, 0);
  EXPECT_EQ(4, rect1_->x());
  EXPECT_EQ(4, rect1_->y());

  // Test depth setting.
  rect1_->set_z(14.0f);
  EXPECT_EQ(14.0f, rect1_->z());

  // TODO: add test cases to test the new LayerVisitor implementation.
  // Test opacity setting.
  rect1_->SetOpacity(0.6f, 0);
  // Have to traverse the tree to update is_opaque.
  stage_->Accept(&layer_visitor);
  EXPECT_EQ(0.6f, rect1_->opacity());
  rect1_->SetOpacity(1.0f, 0);
  stage_->Accept(&layer_visitor);
  EXPECT_EQ(1.0f, rect1_->opacity());

  // Test visibility setting.
  rect1_->Show();
  stage_->Accept(&layer_visitor);
  EXPECT_FALSE(rect1_->IsVisible());
  rect1_->Hide();
  stage_->Accept(&layer_visitor);
  EXPECT_FALSE(rect1_->IsVisible());
  rect1_->Show();
  rect1_->SetOpacity(0.00001f, 0);
  stage_->Accept(&layer_visitor);
  EXPECT_FALSE(rect1_->IsVisible());
}

TEST_F(RealCompositorTestTree, ContainerActorAttributes) {
  RealCompositor::LayerVisitor layer_visitor(compositor()->actor_count(),
                                             false);
  stage_->Accept(&layer_visitor);
  rect1_->SetSize(10, 5);
  // Make sure width and height set the right parameters.
  group1_->SetSize(12, 13);
  // Groups ignore SetSize.
  EXPECT_EQ(1, group1_->width());
  EXPECT_EQ(1, group1_->height());
  EXPECT_EQ(10, rect1_->width());
  EXPECT_EQ(5, rect1_->height());

  // Make sure scale is independent of width and height.
  group1_->Scale(2.0f, 3.0f, 0);
  EXPECT_EQ(2.0f, group1_->scale_x());
  EXPECT_EQ(3.0f, group1_->scale_y());
  EXPECT_EQ(1, group1_->width());
  EXPECT_EQ(1, group1_->height());
  EXPECT_EQ(10, rect1_->width());
  EXPECT_EQ(5, rect1_->height());
  EXPECT_EQ(1.0f, rect1_->scale_x());
  EXPECT_EQ(1.0f, rect1_->scale_y());

  // Make sure Move isn't relative, and works on both axes.
  group1_->MoveX(2, 0);
  group1_->MoveX(2, 0);
  group1_->MoveY(2, 0);
  group1_->MoveY(2, 0);
  EXPECT_EQ(2, group1_->x());
  EXPECT_EQ(2, group1_->y());
  group1_->Move(4, 4, 0);
  group1_->Move(4, 4, 0);
  EXPECT_EQ(4, group1_->x());
  EXPECT_EQ(4, group1_->y());

  // Test depth setting.
  group1_->set_z(14.0f);
  EXPECT_EQ(14.0f, group1_->z());

  // TODO: add test cases to test the new LayerVisitor implementation.
  // Test opacity setting.
  group1_->SetOpacity(0.6f, 0);
  stage_->Accept(&layer_visitor);
  EXPECT_EQ(0.6f, group1_->opacity());
  group1_->SetOpacity(1.0f, 0);
  stage_->Accept(&layer_visitor);
  EXPECT_EQ(1.0f, group1_->opacity());

  // Test visibility setting.
  group1_->Show();
  stage_->Accept(&layer_visitor);
  EXPECT_TRUE(group1_->IsVisible());
  group1_->Hide();
  stage_->Accept(&layer_visitor);
  EXPECT_FALSE(group1_->IsVisible());
  group1_->Show();
  group1_->SetOpacity(0.00001f, 0);
  stage_->Accept(&layer_visitor);
  EXPECT_FALSE(group1_->IsVisible());
}

TEST_F(RealCompositorTest, FloatAnimation) {
  float value = -10.0f;
  RealCompositor::Animation<float> anim(&value, 10.0f, 0, 20);
  EXPECT_FALSE(anim.Eval(0));
  EXPECT_FLOAT_EQ(-10.0f, value);
  EXPECT_FALSE(anim.Eval(5));
  EXPECT_FLOAT_EQ(-sqrt(50.0f), value);
  EXPECT_FALSE(anim.Eval(10));

  // The standard epsilon is just a little too small here..
  EXPECT_NEAR(0.0f, value, 1.0e-6);

  EXPECT_FALSE(anim.Eval(15));
  EXPECT_FLOAT_EQ(sqrt(50.0f), value);
  EXPECT_TRUE(anim.Eval(20));
  EXPECT_FLOAT_EQ(10.0f, value);
}

TEST_F(RealCompositorTest, IntAnimation) {
  int value = -10;
  RealCompositor::Animation<int> anim(&value, 10, 0, 200);
  EXPECT_FALSE(anim.Eval(0));
  EXPECT_EQ(-10, value);
  EXPECT_FALSE(anim.Eval(50));
  EXPECT_EQ(-7, value);
  EXPECT_FALSE(anim.Eval(100));
  EXPECT_EQ(0, value);
  EXPECT_FALSE(anim.Eval(150));
  EXPECT_EQ(7, value);
  // Test that we round to the nearest value instead of truncating.
  EXPECT_FALSE(anim.Eval(199));
  EXPECT_EQ(10, value);
  EXPECT_TRUE(anim.Eval(200));
  EXPECT_EQ(10, value);
}

TEST_F(RealCompositorTestTree, CloneTest) {
  rect1_->Move(10, 20, 0);
  rect1_->SetSize(100, 200);
  RealCompositor::Actor* clone = rect1_->Clone();
  EXPECT_EQ(10, clone->x());
  EXPECT_EQ(20, clone->y());
  EXPECT_EQ(100, clone->width());
  EXPECT_EQ(200, clone->height());
}

// Test RealCompositor's handling of X events concerning composited windows.
TEST_F(RealCompositorTest, HandleXEvents) {
  // Draw once initially to make sure that the compositor isn't dirty.
  compositor()->Draw();
  EXPECT_FALSE(compositor()->dirty());

  // Now create an texture pixmap actor and add it to the stage.
  scoped_ptr<Compositor::TexturePixmapActor> actor(
      compositor()->CreateTexturePixmap());

  RealCompositor::TexturePixmapActor* cast_actor =
      dynamic_cast<RealCompositor::TexturePixmapActor*>(actor.get());
  CHECK(cast_actor);
  cast_actor->Show();
  compositor()->GetDefaultStage()->AddActor(cast_actor);
  EXPECT_TRUE(compositor()->dirty());
  compositor()->Draw();
  EXPECT_FALSE(compositor()->dirty());

  XWindow xid = xconn()->CreateWindow(
      xconn()->GetRootWindow(),  // parent
      0, 0,      // x, y
      400, 300,  // width, height
      false,     // override_redirect=false
      false,     // input_only=false
      0);        // event_mask
  MockXConnection::WindowInfo* info = xconn()->GetWindowInfoOrDie(xid);
  XID pixmap_id = xconn()->GetCompositingPixmapForWindow(xid);

  // After we bind the actor to the window's pixmap, the actor's size
  // should be updated and the compositor should be marked dirty.
  cast_actor->SetPixmap(pixmap_id);
  EXPECT_EQ(pixmap_id, cast_actor->pixmap());
  EXPECT_EQ(info->width, cast_actor->GetWidth());
  EXPECT_EQ(info->height, cast_actor->GetHeight());
  EXPECT_TRUE(cast_actor->texture_data() == NULL);
  EXPECT_TRUE(compositor()->dirty());

  // The visitor should initialize the texture data from the actor's pixmap.
  compositor()->Draw();
  EXPECT_TRUE(cast_actor->texture_data() != NULL);
  EXPECT_FALSE(compositor()->dirty());

  // Now resize the window.  The new pixmap should be loaded and the old
  // texture data should be discarded.
  ASSERT_TRUE(xconn()->ResizeWindow(xid, info->width + 20, info->height + 10));
  ASSERT_TRUE(xconn()->FreePixmap(pixmap_id));
  pixmap_id = xconn()->GetCompositingPixmapForWindow(xid);
  actor->SetPixmap(pixmap_id);
  EXPECT_EQ(pixmap_id, cast_actor->pixmap());
  EXPECT_EQ(info->width, cast_actor->GetWidth());
  EXPECT_EQ(info->height, cast_actor->GetHeight());
  EXPECT_FALSE(cast_actor->texture_data());
  EXPECT_TRUE(compositor()->dirty());

  // Now tell the actor to stop tracking the window.
  cast_actor->SetPixmap(0);
  EXPECT_EQ(0, cast_actor->pixmap());
  EXPECT_FALSE(cast_actor->texture_data());
  EXPECT_TRUE(compositor()->dirty());

  actor.reset();
  EXPECT_TRUE(compositor()->dirty());
}

// Check that we don't crash when we delete a group that contains a child.
TEST_F(RealCompositorTest, DeleteGroup) {
  scoped_ptr<RealCompositor::ContainerActor> group(compositor()->CreateGroup());
  scoped_ptr<RealCompositor::Actor> rect(
    compositor()->CreateRectangle(Compositor::Color(), Compositor::Color(), 0));

  compositor()->GetDefaultStage()->AddActor(group.get());
  group->AddActor(rect.get());

  EXPECT_TRUE(rect->parent() == group.get());
  group.reset();
  EXPECT_TRUE(rect->parent() == NULL);
  compositor()->Draw();
}

// Test that we enable and disable the draw timeout as needed.
TEST_F(RealCompositorTest, DrawTimeout) {
  int64_t now = 1000;  // arbitrary
  compositor()->set_current_time_ms_for_testing(now);

  // The compositor should create a draw timeout and draw just once
  // initially.
  EXPECT_GE(compositor()->draw_timeout_id(), 0);
  EXPECT_TRUE(compositor()->draw_timeout_enabled());
  compositor()->Draw();
  EXPECT_FALSE(compositor()->draw_timeout_enabled());

  // After we add an actor, we should draw another frame.
  scoped_ptr<RealCompositor::Actor> actor(
      compositor()->CreateRectangle(
          Compositor::Color(), Compositor::Color(), 0));
  compositor()->GetDefaultStage()->AddActor(actor.get());
  EXPECT_TRUE(compositor()->draw_timeout_enabled());
  compositor()->Draw();
  EXPECT_FALSE(compositor()->draw_timeout_enabled());

  // Now animate the actor's X position over 100 ms and its Y position over
  // 200 ms.
  actor->MoveX(300, 100);
  actor->MoveY(400, 150);
  EXPECT_TRUE(compositor()->draw_timeout_enabled());

  // If we draw 50 ms later, both animations should still be active, as
  // well as the timeout.
  now += 50;
  compositor()->set_current_time_ms_for_testing(now);
  compositor()->Draw();
  EXPECT_TRUE(compositor()->draw_timeout_enabled());

  // After drawing 51 ms later, the first animation will be gone, but we
  // still keep the timeout alive for the second animation.
  now += 51;
  compositor()->set_current_time_ms_for_testing(now);
  compositor()->Draw();
  EXPECT_TRUE(compositor()->draw_timeout_enabled());

  // 100 ms later, the second animation has ended, so we should remove the
  // timeout after drawing.
  now += 100;
  compositor()->set_current_time_ms_for_testing(now);
  compositor()->Draw();
  EXPECT_FALSE(compositor()->draw_timeout_enabled());

  // If we move the actor instantaneously, we should draw a single frame.
  actor->Move(500, 600, 0);
  EXPECT_TRUE(compositor()->draw_timeout_enabled());
  compositor()->Draw();
  EXPECT_FALSE(compositor()->draw_timeout_enabled());

  // We should also draw one more time after deleting the actor.
  actor.reset();
  EXPECT_TRUE(compositor()->draw_timeout_enabled());
  compositor()->Draw();
  EXPECT_FALSE(compositor()->draw_timeout_enabled());

  // TODO: Test the durations that we set for for the timeout.
}

// Test that we replace existing animations rather than creating
// overlapping animations for the same field.
TEST_F(RealCompositorTest, ReplaceAnimations) {
  int64_t now = 1000;  // arbitrary
  compositor()->set_current_time_ms_for_testing(now);

  scoped_ptr<RealCompositor::Actor> actor(
      compositor()->CreateRectangle(
          Compositor::Color(), Compositor::Color(), 0));
  compositor()->GetDefaultStage()->AddActor(actor.get());
  compositor()->Draw();

  // Create 500-ms animations of the actor's X position to 200 and its
  // Y position to 300, but then replace the Y animation with one that goes
  // to 800 in just 100 ms.
  actor->Move(200, 300, 500);
  actor->MoveY(800, 100);

  // 101 ms later, the actor should be at the final Y position but not yet
  // at the final X position.
  now += 101;
  compositor()->set_current_time_ms_for_testing(now);
  compositor()->Draw();
  EXPECT_EQ(800, actor->GetY());
  EXPECT_LT(actor->GetX(), 200);

  // 400 ms later (501 since we started the animations), the actor should
  // be in the final position.  Check that its Y position is still 800
  // (i.e. the longer-running animation to 300 was replaced by the one to
  // 800).
  now += 400;
  compositor()->set_current_time_ms_for_testing(now);
  compositor()->Draw();
  EXPECT_EQ(200, actor->GetX());
  EXPECT_EQ(800, actor->GetY());

  // Start 200-ms animations reducing the actor to half its original scale.
  // After 100 ms, we should be halfway to the final scale (at 3/4 scale).
  actor->Scale(0.5, 0.5, 200);
  now += 100;
  compositor()->set_current_time_ms_for_testing(now);
  compositor()->Draw();
  EXPECT_FLOAT_EQ(0.75, actor->GetXScale());
  EXPECT_FLOAT_EQ(0.75, actor->GetYScale());

  // Now interrupt the animation with another one going back to the
  // original scale.  100 ms later, we should be halfway between the scale
  // at the time the previous animation was interrupted and the original
  // scale.
  actor->Scale(1.0, 1.0, 200);
  now += 100;
  compositor()->set_current_time_ms_for_testing(now);
  compositor()->Draw();
  EXPECT_FLOAT_EQ(0.875, actor->GetXScale());
  EXPECT_FLOAT_EQ(0.875, actor->GetYScale());

  // After another 100 ms, we should be back at the original scale.
  now += 100;
  compositor()->set_current_time_ms_for_testing(now);
  compositor()->Draw();
  EXPECT_FLOAT_EQ(1, actor->GetXScale());
  EXPECT_FLOAT_EQ(1, actor->GetYScale());
}

TEST_F(RealCompositorTest, SkipUnneededAnimations) {
  int64_t now = 1000;  // arbitrary
  compositor()->set_current_time_ms_for_testing(now);

  // After we add an actor, we should draw a frame.
  scoped_ptr<RealCompositor::Actor> actor(
      compositor()->CreateRectangle(
          Compositor::Color(), Compositor::Color(), 0));
  compositor()->GetDefaultStage()->AddActor(actor.get());
  EXPECT_TRUE(compositor()->draw_timeout_enabled());
  compositor()->Draw();
  EXPECT_FALSE(compositor()->draw_timeout_enabled());

  // Set the actor's X position.  We should draw just once.
  // 200 ms.
  actor->MoveX(300, 0);
  EXPECT_TRUE(compositor()->draw_timeout_enabled());
  compositor()->Draw();
  EXPECT_FALSE(compositor()->draw_timeout_enabled());

  // We shouldn't do any drawing if we animate to the same position that
  // we're already in.
  actor->MoveX(300, 200);
  EXPECT_FALSE(compositor()->draw_timeout_enabled());
}

// Test that the compositor handles visibility groups correctly.
TEST_F(RealCompositorTest, VisibilityGroups) {
  // Add an actor and check that it's initially visible.
  scoped_ptr<RealCompositor::Actor> actor(
      compositor()->CreateRectangle(
          Compositor::Color(), Compositor::Color(), 0));
  compositor()->GetDefaultStage()->AddActor(actor.get());
  EXPECT_TRUE(compositor()->dirty());
  compositor()->Draw();
  EXPECT_FALSE(compositor()->dirty());
  EXPECT_TRUE(actor->IsVisible());

  // Adding or removing the actor from a visibility group while the
  // compositor isn't using visibility groups should have no effect.
  actor->AddToVisibilityGroup(1);
  EXPECT_FALSE(compositor()->dirty());
  EXPECT_TRUE(actor->IsVisible());
  actor->RemoveFromVisibilityGroup(1);
  EXPECT_FALSE(compositor()->dirty());

  // Now tell the compositor to only show visibility group 1.  The actor
  // isn't in that group anymore, so it should be invisible.
  unordered_set<int> groups;
  groups.insert(1);
  compositor()->SetActiveVisibilityGroups(groups);
  EXPECT_TRUE(compositor()->dirty());
  EXPECT_FALSE(actor->IsVisible());
  compositor()->Draw();

  // The stage shouldn't care about visibility groups.
  EXPECT_TRUE(compositor()->GetDefaultStage()->IsVisible());

  // Add the actor to visibility group 2 and make sure that it's still hidden.
  actor->AddToVisibilityGroup(2);
  EXPECT_TRUE(compositor()->dirty());
  EXPECT_FALSE(actor->IsVisible());
  compositor()->Draw();

  // Now add it to visibility group 1 and make sure that it gets shown.
  actor->AddToVisibilityGroup(1);
  EXPECT_TRUE(compositor()->dirty());
  EXPECT_TRUE(actor->IsVisible());
  compositor()->Draw();

  // Remove it from both groups and check that it's hidden again.
  actor->RemoveFromVisibilityGroup(1);
  actor->RemoveFromVisibilityGroup(2);
  EXPECT_TRUE(compositor()->dirty());
  EXPECT_FALSE(actor->IsVisible());
  compositor()->Draw();

  // Now disable visibility groups in the compositor and check that the
  // actor is visible.
  groups.clear();
  compositor()->SetActiveVisibilityGroups(groups);
  EXPECT_TRUE(compositor()->dirty());
  EXPECT_TRUE(actor->IsVisible());
  compositor()->Draw();
}

// Test RealCompositor's handling of partial updates.
TEST_F(RealCompositorTest, PartialUpdates) {
  // Need to set the stage actor's size large enough to test partial updates.
  const int stage_width = 1000;
  const int stage_height = 1000;
  compositor()->GetDefaultStage()->SetSize(stage_width, stage_height);
  ASSERT_EQ(stage_width, compositor()->GetDefaultStage()->GetWidth());
  ASSERT_EQ(stage_height, compositor()->GetDefaultStage()->GetHeight());

  // Now create an texture pixmap actor and add it to the stage.
  scoped_ptr<Compositor::TexturePixmapActor> actor(
      compositor()->CreateTexturePixmap());

  RealCompositor::TexturePixmapActor* cast_actor =
      dynamic_cast<RealCompositor::TexturePixmapActor*>(actor.get());
  CHECK(cast_actor);
  cast_actor->Show();
  compositor()->GetDefaultStage()->AddActor(cast_actor);
  compositor()->Draw();
  EXPECT_FALSE(compositor()->dirty());

  XWindow xid = xconn()->CreateWindow(
      xconn()->GetRootWindow(),  // parent
      0, 0,      // x, y
      400, 300,  // width, height
      false,     // override_redirect=false
      false,     // input_only=false
      0);        // event_mask
  XID pixmap_id = xconn()->GetCompositingPixmapForWindow(xid);

  // After we bind the actor to the window's pixmap, the actor's size
  // should be updated and the compositor should be marked dirty.
  cast_actor->SetPixmap(pixmap_id);
  int full_updates_count = gl_interface()->full_updates_count();
  int partial_updates_count = gl_interface()->partial_updates_count();
  compositor()->Draw();
  EXPECT_FALSE(compositor()->dirty());
  ++full_updates_count;
  EXPECT_EQ(gl_interface()->full_updates_count(), full_updates_count);
  EXPECT_EQ(gl_interface()->partial_updates_count(), partial_updates_count);

  // Mark part of the window as dirty. The next time we draw, partial updates
  // should happen.
  EXPECT_TRUE(gl_interface()->IsCapableOfPartialUpdates());
  Rect damaged_region(50, 50, 100, 100);
  cast_actor->MergeDamagedRegion(damaged_region);
  compositor()->SetPartiallyDirty();
  EXPECT_FALSE(compositor()->dirty());
  compositor()->Draw();
  ++partial_updates_count;
  EXPECT_FALSE(compositor()->dirty());
  EXPECT_EQ(gl_interface()->full_updates_count(), full_updates_count);
  EXPECT_EQ(gl_interface()->partial_updates_count(), partial_updates_count);
  const Rect& updated_region = gl_interface()->partial_updates_region();
  // Damaged region is defined relative to the window where (0, 0) is top_left
  // and (w, h) is bottom_right.  CopyGlxSubBuffer's region is defined relative
  // to the screen where (0, 0) is bottom_left and (w, h) is top_right.
  int expected_x = damaged_region.x + cast_actor->GetX();
  int expected_y = stage_height - damaged_region.height -
                   (damaged_region.y + cast_actor->GetY());
  EXPECT_EQ(expected_x, updated_region.x);
  EXPECT_EQ(expected_y, updated_region.y);
  EXPECT_EQ(damaged_region.width, updated_region.width);
  EXPECT_EQ(damaged_region.height, updated_region.height);
}

}  // end namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
