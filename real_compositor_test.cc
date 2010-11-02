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
#include "window_manager/layer_visitor.h"
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
using window_manager::util::SetMonotonicTimeMsForTest;

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

class RealCompositorTest : public BasicCompositingTest {};
class RealCompositorTreeTest : public BasicCompositingTreeTest {};

TEST_F(RealCompositorTreeTest, LayerDepth) {
  // Test lower-level layer-setting routines
  int32 count = 0;
  stage_->Update(&count, 0LL);
  EXPECT_EQ(8, count);
  RealCompositor::ActorVector actors;

  // Code uses a depth range of kMinDepth to kMaxDepth.  Layers are
  // disributed evenly within that range, except we don't use the
  // frontmost or backmost values in that range.
  uint32 max_count = NextPowerOfTwo(static_cast<uint32>(count + 2));
  float thickness = (LayerVisitor::kMaxDepth -
                     LayerVisitor::kMinDepth) / max_count;
  float depth = LayerVisitor::kMinDepth + thickness;

  // First we test the layer visitor directly.
  LayerVisitor layer_visitor(count, false);
  stage_->Accept(&layer_visitor);

  // rect3 is fullscreen and opaque, so rect2 and rect1 are culled.
  EXPECT_TRUE(layer_visitor.has_fullscreen_actor());
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
  depth = LayerVisitor::kMinDepth + thickness;
  compositor_->Draw();
  EXPECT_EQ(8, compositor_->actor_count());

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

TEST_F(RealCompositorTreeTest, LayerDepthWithOpacity) {
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
  float thickness = (LayerVisitor::kMaxDepth -
                     LayerVisitor::kMinDepth) / max_count;
  float depth = LayerVisitor::kMinDepth + thickness;

  // First we test the layer visitor directly.
  LayerVisitor layer_visitor(count, false);
  stage_->Accept(&layer_visitor);

  // rect3 is fullscreen but not opaque, so rect2 is not culled.
  // rect2 is fullscreen and opaque, so rect1 is culled.
  EXPECT_TRUE(layer_visitor.has_fullscreen_actor());
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
  depth = LayerVisitor::kMinDepth + thickness;
  compositor_->Draw();
  EXPECT_EQ(8, compositor_->actor_count());

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

TEST_F(RealCompositorTreeTest, ActorVisitor) {
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

TEST_F(RealCompositorTreeTest, ActorAttributes) {
  LayerVisitor layer_visitor(compositor_->actor_count(),
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

TEST_F(RealCompositorTreeTest, ContainerActorAttributes) {
  LayerVisitor layer_visitor(compositor_->actor_count(),
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

TEST_F(RealCompositorTreeTest, CloneTest) {
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
  compositor_->Draw();
  EXPECT_FALSE(compositor_->dirty());

  // Now create an texture pixmap actor and add it to the stage.
  scoped_ptr<Compositor::TexturePixmapActor> actor(
      compositor_->CreateTexturePixmap());

  RealCompositor::TexturePixmapActor* cast_actor =
      dynamic_cast<RealCompositor::TexturePixmapActor*>(actor.get());
  CHECK(cast_actor);
  cast_actor->Show();
  compositor_->GetDefaultStage()->AddActor(cast_actor);
  EXPECT_TRUE(compositor_->dirty());
  compositor_->Draw();
  EXPECT_FALSE(compositor_->dirty());

  XWindow xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),  // parent
      Rect(0, 0, 400, 300),
      false,  // override_redirect=false
      false,  // input_only=false
      0, 0);  // event_mask, visual
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  XID pixmap_id = xconn_->GetCompositingPixmapForWindow(xid);

  // After we bind the actor to the window's pixmap, the actor's size
  // should be updated and the compositor should be marked dirty.
  cast_actor->SetPixmap(pixmap_id);
  EXPECT_EQ(pixmap_id, cast_actor->pixmap());
  EXPECT_EQ(info->bounds.width, cast_actor->GetWidth());
  EXPECT_EQ(info->bounds.height, cast_actor->GetHeight());
  EXPECT_TRUE(cast_actor->texture_data() == NULL);
  EXPECT_TRUE(compositor_->dirty());

  // The visitor should initialize the texture data from the actor's pixmap.
  compositor_->Draw();
  EXPECT_TRUE(cast_actor->texture_data() != NULL);
  EXPECT_FALSE(compositor_->dirty());

  // Now resize the window.  The new pixmap should be loaded and the old
  // texture data should be discarded.
  ASSERT_TRUE(xconn_->ResizeWindow(xid,
                                   Size(info->bounds.width + 20,
                                        info->bounds.height + 10)));
  ASSERT_TRUE(xconn_->FreePixmap(pixmap_id));
  pixmap_id = xconn_->GetCompositingPixmapForWindow(xid);
  actor->SetPixmap(pixmap_id);
  EXPECT_EQ(pixmap_id, cast_actor->pixmap());
  EXPECT_EQ(info->bounds.width, cast_actor->GetWidth());
  EXPECT_EQ(info->bounds.height, cast_actor->GetHeight());
  EXPECT_FALSE(cast_actor->texture_data());
  EXPECT_TRUE(compositor_->dirty());

  // Now tell the actor to stop tracking the window.
  cast_actor->SetPixmap(0);
  EXPECT_EQ(0, cast_actor->pixmap());
  EXPECT_FALSE(cast_actor->texture_data());
  EXPECT_TRUE(compositor_->dirty());

  actor.reset();
  EXPECT_TRUE(compositor_->dirty());
}

// Check that we don't crash when we delete a group that contains a child.
TEST_F(RealCompositorTest, DeleteGroup) {
  scoped_ptr<RealCompositor::ContainerActor> group(compositor_->CreateGroup());
  scoped_ptr<RealCompositor::ColoredBoxActor> rect(
      compositor_->CreateColoredBox(1, 1, Compositor::Color()));

  compositor_->GetDefaultStage()->AddActor(group.get());
  group->AddActor(rect.get());

  EXPECT_TRUE(rect->parent() == group.get());
  group.reset();
  EXPECT_TRUE(rect->parent() == NULL);
  compositor_->Draw();
}

// Test that we enable and disable the draw timeout as needed.
TEST_F(RealCompositorTest, DrawTimeout) {
  int64_t now = 1000;  // arbitrary
  SetMonotonicTimeMsForTest(now);

  // The compositor should create a draw timeout and draw just once
  // initially.
  EXPECT_GE(compositor_->draw_timeout_id(), 0);
  EXPECT_TRUE(compositor_->draw_timeout_enabled());
  compositor_->Draw();
  EXPECT_FALSE(compositor_->draw_timeout_enabled());

  // After we add an actor, we should draw another frame.
  scoped_ptr<RealCompositor::Actor> actor(
      compositor_->CreateColoredBox(1, 1, Compositor::Color()));
  compositor_->GetDefaultStage()->AddActor(actor.get());
  EXPECT_TRUE(compositor_->draw_timeout_enabled());
  compositor_->Draw();
  EXPECT_FALSE(compositor_->draw_timeout_enabled());

  // Now animate the actor's X position over 100 ms and its Y position over
  // 200 ms.
  actor->MoveX(300, 100);
  actor->MoveY(400, 150);
  EXPECT_TRUE(compositor_->draw_timeout_enabled());

  // If we draw 50 ms later, both animations should still be active, as
  // well as the timeout.
  now += 50;
  SetMonotonicTimeMsForTest(now);
  compositor_->Draw();
  EXPECT_TRUE(compositor_->draw_timeout_enabled());

  // After drawing 51 ms later, the first animation will be gone, but we
  // still keep the timeout alive for the second animation.
  now += 51;
  SetMonotonicTimeMsForTest(now);
  compositor_->Draw();
  EXPECT_TRUE(compositor_->draw_timeout_enabled());

  // 100 ms later, the second animation has ended, so we should remove the
  // timeout after drawing.
  now += 100;
  SetMonotonicTimeMsForTest(now);
  compositor_->Draw();
  EXPECT_FALSE(compositor_->draw_timeout_enabled());

  // If we move the actor instantaneously, we should draw a single frame.
  actor->Move(500, 600, 0);
  EXPECT_TRUE(compositor_->draw_timeout_enabled());
  compositor_->Draw();
  EXPECT_FALSE(compositor_->draw_timeout_enabled());

  // We should also draw one more time after deleting the actor.
  actor.reset();
  EXPECT_TRUE(compositor_->draw_timeout_enabled());
  compositor_->Draw();
  EXPECT_FALSE(compositor_->draw_timeout_enabled());

  // TODO: Test the durations that we set for for the timeout.
}

// Test that we replace existing animations rather than creating
// overlapping animations for the same field.
TEST_F(RealCompositorTest, ReplaceAnimations) {
  int64_t now = 1000;  // arbitrary
  SetMonotonicTimeMsForTest(now);

  scoped_ptr<RealCompositor::Actor> actor(
      compositor_->CreateColoredBox(1, 1, Compositor::Color()));
  compositor_->GetDefaultStage()->AddActor(actor.get());
  compositor_->Draw();

  // Create 500-ms animations of the actor's X position to 200 and its
  // Y position to 300, but then replace the Y animation with one that goes
  // to 800 in just 100 ms.
  actor->Move(200, 300, 500);
  actor->MoveY(800, 100);

  // 101 ms later, the actor should be at the final Y position but not yet
  // at the final X position.
  now += 101;
  SetMonotonicTimeMsForTest(now);
  compositor_->Draw();
  EXPECT_EQ(800, actor->GetY());
  EXPECT_LT(actor->GetX(), 200);

  // 400 ms later (501 since we started the animations), the actor should
  // be in the final position.  Check that its Y position is still 800
  // (i.e. the longer-running animation to 300 was replaced by the one to
  // 800).
  now += 400;
  SetMonotonicTimeMsForTest(now);
  compositor_->Draw();
  EXPECT_EQ(200, actor->GetX());
  EXPECT_EQ(800, actor->GetY());

  // Start 200-ms animations reducing the actor to half its original scale.
  // After 100 ms, we should be halfway to the final scale (at 3/4 scale).
  actor->Scale(0.5, 0.5, 200);
  now += 100;
  SetMonotonicTimeMsForTest(now);
  compositor_->Draw();
  EXPECT_FLOAT_EQ(0.75, actor->GetXScale());
  EXPECT_FLOAT_EQ(0.75, actor->GetYScale());

  // Now interrupt the animation with another one going back to the
  // original scale.  100 ms later, we should be halfway between the scale
  // at the time the previous animation was interrupted and the original
  // scale.
  actor->Scale(1.0, 1.0, 200);
  now += 100;
  SetMonotonicTimeMsForTest(now);
  compositor_->Draw();
  EXPECT_FLOAT_EQ(0.875, actor->GetXScale());
  EXPECT_FLOAT_EQ(0.875, actor->GetYScale());

  // After another 100 ms, we should be back at the original scale.
  now += 100;
  SetMonotonicTimeMsForTest(now);
  compositor_->Draw();
  EXPECT_FLOAT_EQ(1, actor->GetXScale());
  EXPECT_FLOAT_EQ(1, actor->GetYScale());
}

TEST_F(RealCompositorTest, SkipUnneededAnimations) {
  int64_t now = 1000;  // arbitrary
  SetMonotonicTimeMsForTest(now);

  // After we add an actor, we should draw a frame.
  scoped_ptr<RealCompositor::Actor> actor(
      compositor_->CreateColoredBox(1, 1, Compositor::Color()));
  compositor_->GetDefaultStage()->AddActor(actor.get());
  EXPECT_TRUE(compositor_->draw_timeout_enabled());
  compositor_->Draw();
  EXPECT_FALSE(compositor_->draw_timeout_enabled());

  // Set the actor's X position.  We should draw just once.
  // 200 ms.
  actor->MoveX(300, 0);
  EXPECT_TRUE(compositor_->draw_timeout_enabled());
  compositor_->Draw();
  EXPECT_FALSE(compositor_->draw_timeout_enabled());

  // We shouldn't do any drawing if we animate to the same position that
  // we're already in.
  actor->MoveX(300, 200);
  EXPECT_FALSE(compositor_->draw_timeout_enabled());
}

// Test that the compositor handles visibility groups correctly.
TEST_F(RealCompositorTest, VisibilityGroups) {
  // Add an actor and check that it's initially visible.
  scoped_ptr<RealCompositor::Actor> actor(
      compositor_->CreateColoredBox(1, 1, Compositor::Color()));
  compositor_->GetDefaultStage()->AddActor(actor.get());
  EXPECT_TRUE(compositor_->dirty());
  compositor_->Draw();
  EXPECT_FALSE(compositor_->dirty());
  EXPECT_TRUE(actor->IsVisible());

  // Adding or removing the actor from a visibility group while the
  // compositor isn't using visibility groups should have no effect.
  actor->AddToVisibilityGroup(1);
  EXPECT_FALSE(compositor_->dirty());
  EXPECT_TRUE(actor->IsVisible());
  actor->RemoveFromVisibilityGroup(1);
  EXPECT_FALSE(compositor_->dirty());

  // Now tell the compositor to only show visibility group 1.  The actor
  // isn't in that group anymore, so it should be invisible.
  unordered_set<int> groups;
  groups.insert(1);
  compositor_->SetActiveVisibilityGroups(groups);
  EXPECT_TRUE(compositor_->dirty());
  EXPECT_FALSE(actor->IsVisible());
  compositor_->Draw();

  // The stage shouldn't care about visibility groups.
  EXPECT_TRUE(compositor_->GetDefaultStage()->IsVisible());

  // Add the actor to visibility group 2 and make sure that it's still hidden.
  actor->AddToVisibilityGroup(2);
  EXPECT_TRUE(compositor_->dirty());
  EXPECT_FALSE(actor->IsVisible());
  compositor_->Draw();

  // Now add it to visibility group 1 and make sure that it gets shown.
  actor->AddToVisibilityGroup(1);
  EXPECT_TRUE(compositor_->dirty());
  EXPECT_TRUE(actor->IsVisible());
  compositor_->Draw();

  // Remove it from both groups and check that it's hidden again.
  actor->RemoveFromVisibilityGroup(1);
  actor->RemoveFromVisibilityGroup(2);
  EXPECT_TRUE(compositor_->dirty());
  EXPECT_FALSE(actor->IsVisible());
  compositor_->Draw();

  // Now disable visibility groups in the compositor and check that the
  // actor is visible.
  groups.clear();
  compositor_->SetActiveVisibilityGroups(groups);
  EXPECT_TRUE(compositor_->dirty());
  EXPECT_TRUE(actor->IsVisible());
  compositor_->Draw();
}

// Test RealCompositor's handling of partial updates.
TEST_F(RealCompositorTest, PartialUpdates) {
  // Need to set the stage actor's size large enough to test partial updates.
  const int stage_width = 1366;
  const int stage_height = 768;
  compositor_->GetDefaultStage()->SetSize(stage_width, stage_height);
  ASSERT_EQ(stage_width, compositor_->GetDefaultStage()->GetWidth());
  ASSERT_EQ(stage_height, compositor_->GetDefaultStage()->GetHeight());

  // Now create an texture pixmap actor and add it to the stage.
  scoped_ptr<Compositor::TexturePixmapActor> actor(
      compositor_->CreateTexturePixmap());

  RealCompositor::TexturePixmapActor* cast_actor =
      dynamic_cast<RealCompositor::TexturePixmapActor*>(actor.get());
  CHECK(cast_actor);
  cast_actor->Show();
  compositor_->GetDefaultStage()->AddActor(cast_actor);
  compositor_->Draw();
  EXPECT_FALSE(compositor_->dirty());

  XWindow xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),  // parent
      Rect(0, 0, 1366, 768),
      false,      // override_redirect=false
      false,      // input_only=false
      0, 0);      // event_mask, visual
  XID pixmap_id = xconn_->GetCompositingPixmapForWindow(xid);

  // After we bind the actor to the window's pixmap, the actor's size
  // should be updated and the compositor should be marked dirty.
  cast_actor->SetPixmap(pixmap_id);
  int full_updates_count = gl_->full_updates_count();
  int partial_updates_count = gl_->partial_updates_count();
  compositor_->Draw();
  EXPECT_FALSE(compositor_->dirty());
  ++full_updates_count;
  EXPECT_EQ(gl_->full_updates_count(), full_updates_count);
  EXPECT_EQ(gl_->partial_updates_count(), partial_updates_count);

  // Mark part of the window as dirty. The next time we draw, partial updates
  // should happen.
  EXPECT_TRUE(gl_->IsCapableOfPartialUpdates());
  Rect damaged_region(44, 28, 12, 13);
  cast_actor->MergeDamagedRegion(damaged_region);
  compositor_->SetPartiallyDirty();
  EXPECT_FALSE(compositor_->dirty());
  compositor_->Draw();
  ++partial_updates_count;
  EXPECT_FALSE(compositor_->dirty());
  EXPECT_EQ(gl_->full_updates_count(), full_updates_count);
  EXPECT_EQ(gl_->partial_updates_count(), partial_updates_count);
  const Rect& updated_region = gl_->partial_updates_region();
  // Damaged region is defined relative to the window where (0, 0) is top_left
  // and (w, h) is bottom_right.  CopyGlxSubBuffer's region is defined relative
  // to the screen where (0, 0) is bottom_left and (w, h) is top_right.
  int expected_min_x = damaged_region.x + cast_actor->GetX();
  int expected_min_y = stage_height - damaged_region.height -
                   (damaged_region.y + cast_actor->GetY());
  int expected_max_x = damaged_region.x + damaged_region.width +
      cast_actor->GetX();
  int expected_max_y = stage_height - (damaged_region.y + cast_actor->GetY());
  EXPECT_GE(expected_min_x, updated_region.x);
  EXPECT_GE(expected_min_y, updated_region.y);
  EXPECT_LE(expected_max_x, updated_region.x + updated_region.width);
  EXPECT_LE(expected_max_y, updated_region.y + updated_region.height);
}

// Test LayerVisitor's top fullscreen window.
TEST_F(RealCompositorTest, LayerVisitorTopFullscreenWindow) {
  // Now create texture pixmap actors and add them to the stage.
  scoped_ptr<RealCompositor::TexturePixmapActor> actor1(
      compositor_->CreateTexturePixmap());
  scoped_ptr<RealCompositor::TexturePixmapActor> actor2(
      compositor_->CreateTexturePixmap());
  scoped_ptr<RealCompositor::TexturePixmapActor> actor3(
      compositor_->CreateTexturePixmap());
  actor1->Show();
  actor2->Show();
  actor3->Show();

  // The order from top to bottom is: actor3, actor2, and actor1.
  RealCompositor::StageActor* stage = compositor_->GetDefaultStage();
  stage->AddActor(actor1.get());
  stage->AddActor(actor2.get());
  stage->AddActor(actor3.get());

  // xwin1 is fullscreen and opaque.
  // xwin2 is fullscreen and transparent.
  // xwin3 is non-fullscreen and opaque.
  XWindow xwin1 = xconn_->CreateWindow(
      xconn_->GetRootWindow(),  // parent
      Rect(0, 0, stage->width(), stage->height()),
      false,     // override_redirect=false
      false,     // input_only=false
      0, 0);     // event_mask, visual
  XWindow xwin2 = xconn_->CreateWindow(
      xconn_->GetRootWindow(),  // parent
      Rect(0, 0, stage->width(), stage->height()),
      false,     // override_redirect=false
      false,     // input_only=false
      0, 0);     // event_mask, visual
  XWindow xwin3 = xconn_->CreateWindow(
      xconn_->GetRootWindow(),  // parent
      Rect(0, 0, 300, 400),
      false,     // override_redirect=false
      false,     // input_only=false
      0, 0);     // event_mask, visual

  // Make xwin1 and xwin3 opaque.
  xconn_->GetWindowInfoOrDie(xwin1)->depth = 24;
  xconn_->GetWindowInfoOrDie(xwin2)->depth = 32;
  xconn_->GetWindowInfoOrDie(xwin3)->depth = 24;

  actor1->SetPixmap(xconn_->GetCompositingPixmapForWindow(xwin1));
  actor2->SetPixmap(xconn_->GetCompositingPixmapForWindow(xwin2));
  actor3->SetPixmap(xconn_->GetCompositingPixmapForWindow(xwin3));

  compositor_->Draw();
  EXPECT_TRUE(actor1->is_opaque());
  EXPECT_FALSE(actor2->is_opaque());
  EXPECT_TRUE(actor3->is_opaque());

  // Test a fullscreen transparent actor on top of another fullscreen actor.
  actor3->Hide();
  LayerVisitor layer_visitor(compositor_->actor_count(), false);
  stage->Accept(&layer_visitor);
  EXPECT_TRUE(layer_visitor.has_fullscreen_actor());
  EXPECT_TRUE(layer_visitor.top_fullscreen_actor() == NULL);

  // Test a non-fullscreen opaque actor on top of a fullscreen actor.
  actor2->Hide();
  actor3->Show();
  stage->Accept(&layer_visitor);
  EXPECT_TRUE(layer_visitor.has_fullscreen_actor());
  EXPECT_TRUE(layer_visitor.top_fullscreen_actor() == NULL);

  // Test a fullscreen opaque actor on top.
  actor3->Hide();
  stage->Accept(&layer_visitor);
  EXPECT_TRUE(layer_visitor.has_fullscreen_actor());
  EXPECT_EQ(layer_visitor.top_fullscreen_actor(), actor1.get());

  // Test no fullscreen opaque actor on top.
  actor1->Hide();
  actor2->Show();
  actor3->Show();
  stage->Accept(&layer_visitor);
  EXPECT_FALSE(layer_visitor.has_fullscreen_actor());
  EXPECT_TRUE(layer_visitor.top_fullscreen_actor() == NULL);
}

}  // end namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
