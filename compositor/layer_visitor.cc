// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor/layer_visitor.h"

#include <algorithm>
#include <cmath>

#include "window_manager/compositor/real_compositor.h"
#include "window_manager/geometry.h"
#include "window_manager/util.h"

namespace window_manager {

using std::ceil;
using std::max;
using std::min;
using window_manager::util::NextPowerOfTwo;

enum CullingResult {
  CULLING_WINDOW_OFFSCREEN,
  CULLING_WINDOW_ONSCREEN,
  CULLING_WINDOW_FULLSCREEN
};

const float LayerVisitor::kMinDepth = 0.0f;
const float LayerVisitor::kMaxDepth = 4096.0f + LayerVisitor::kMinDepth;

static inline float min4(float a, float b, float c, float d) {
  return min(min(min(a, b), c), d);
}

static inline float max4(float a, float b, float c, float d) {
  return max(max(max(a, b), c), d);
}

static inline bool IsBoxOnScreen(const LayerVisitor::BoundingBox& a) {
  // The window has corners top left (-1, 1) and bottom right (1, -1).
  return !(a.x_max <= -1.0 || a.x_min >= 1.0 ||
           a.y_max <= -1.0 || a.y_min >= 1.0);
}

static inline bool IsBoxFullScreen(const LayerVisitor::BoundingBox& a) {
  // The bounding box must be equal or greater than the area (-1, 1) - (1, -1)
  // in case of full screen.
  return a.x_max >= 1.0 && a.x_min <= -1.0 &&
         a.y_max >= 1.0 && a.y_min <= -1.0;
}

// The input region is in window coordinates where top_left is (0, 0) and
// bottom_right is (1, 1).  Output is the bounding box of the transformed window
// in GL coordinates where bottom_left is (-1, -1) and top_right is (1, 1).
static LayerVisitor::BoundingBox ComputeTransformedBoundingBox(
    const RealCompositor::StageActor& stage,
    const RealCompositor::QuadActor& actor,
    const LayerVisitor::BoundingBox& region) {
  const Matrix4& transform = stage.projection() * actor.model_view();

  Vector4 v0(region.x_min, region.y_min, 0, 1);
  Vector4 v1(region.x_min, region.y_max, 0, 1);
  Vector4 v2(region.x_max, region.y_max, 0, 1);
  Vector4 v3(region.x_max, region.y_min, 0, 1);

  v0 = transform * v0;
  v1 = transform * v1;
  v2 = transform * v2;
  v3 = transform * v3;

  v0 /= v0[3];
  v1 /= v1[3];
  v2 /= v2[3];
  v3 /= v3[3];

  return LayerVisitor::BoundingBox(min4(v0[0], v1[0], v2[0], v3[0]),
                                   max4(v0[0], v1[0], v2[0], v3[0]),
                                   min4(v0[1], v1[1], v2[1], v3[1]),
                                   max4(v0[1], v1[1], v2[1], v3[1]));
}

static CullingResult PerformActorCullingTest(
    const RealCompositor::StageActor& stage,
    const RealCompositor::QuadActor& actor) {
  static const LayerVisitor::BoundingBox region(0, 1, 0, 1);

  LayerVisitor::BoundingBox box =
    ComputeTransformedBoundingBox(stage, actor, region);

  if (!IsBoxOnScreen(box))
    return CULLING_WINDOW_OFFSCREEN;

  if (IsBoxFullScreen(box))
    return CULLING_WINDOW_FULLSCREEN;

  return CULLING_WINDOW_ONSCREEN;
}

// The input region is defined in the actor's window coordinates.
static LayerVisitor::BoundingBox MapRegionToGlCoordinates(
    const RealCompositor::StageActor& stage,
    const RealCompositor::TexturePixmapActor& actor,
    const Rect& region) {
  DCHECK(actor.width() > 0 && actor.height() > 0);
  float x_min = region.x;
  x_min /= actor.width();
  float x_max = region.x + region.width;
  x_max /= actor.width();
  float y_min = region.y;
  y_min /= actor.height();
  float y_max = region.y + region.height;
  y_max /= actor.height();

  LayerVisitor::BoundingBox box(x_min, x_max, y_min, y_max);
  return ComputeTransformedBoundingBox(stage, actor, box);
}


void LayerVisitor::VisitActor(RealCompositor::Actor* actor) {
  actor->set_z(depth_);
  depth_ += layer_thickness_;
  actor->set_is_opaque(actor->opacity() > 0.999f);
}

void LayerVisitor::VisitStage(
    RealCompositor::StageActor* actor) {
  if (!actor->IsVisible())
    return;

  // This calculates the next power of two for the actor count, so
  // that we can avoid roundoff errors when computing the depth.
  // Also, add two empty layers at the front and the back that we
  // won't use in order to avoid issues at the extremes.  The eventual
  // plan here is to have three depth ranges, one in the front that is
  // 4096 deep, one in the back that is 4096 deep, and the remaining
  // in the middle for drawing 3D UI elements.  Currently, this code
  // represents just the front layer range.  Note that the number of
  // layers is NOT limited to 4096 (this is an arbitrary value that is
  // a power of two) -- the maximum number of layers depends on the
  // number of actors and the bit-depth of the hardware's z-buffer.
  uint32 count = NextPowerOfTwo(static_cast<uint32>(count_ + 2));
  layer_thickness_ = (kMaxDepth - kMinDepth) / count;

  // Don't start at the very edge of the z-buffer depth.
  depth_ = kMinDepth + layer_thickness_;

  stage_actor_ = actor;
  top_fullscreen_actor_ = NULL;
  visiting_top_visible_actor_ = true;
  has_fullscreen_actor_ = false;

  if (use_partial_updates_)
    updated_area_.clear();

  actor->UpdateProjection();
  VisitContainer(actor);
}

void LayerVisitor::VisitContainer(
    RealCompositor::ContainerActor* actor) {
  CHECK(actor);
  if (!actor->IsVisible())
    return;

  // No culling test for ContainerActor because the container does not bound
  // its children actors.  No need to set_z first because container doesn't
  // use z in its model view matrix.
  actor->UpdateModelView();

  RealCompositor::ActorVector children = actor->GetChildren();
  for (RealCompositor::ActorVector::const_iterator it = children.begin();
       it != children.end(); ++it) {
    if (*it)
      (*it)->Accept(this);
  }

  // The containers should be "further" than all their children.
  this->VisitActor(actor);
}

void LayerVisitor::VisitTexturedQuadActor(
    RealCompositor::QuadActor* actor, bool is_texture_opaque) {
  actor->set_culled(has_fullscreen_actor_);
  if (!actor->IsVisible())
    return;

  VisitActor(actor);
  actor->set_is_opaque(actor->is_opaque() && is_texture_opaque);

  // Must update model view matrix before culling test.
  actor->UpdateModelView();
  CullingResult result =
      PerformActorCullingTest(*stage_actor_, *actor);

  actor->set_culled(result == CULLING_WINDOW_OFFSCREEN);
  if (actor->culled())
    return;

  if (actor->is_opaque() && result == CULLING_WINDOW_FULLSCREEN)
    has_fullscreen_actor_ = true;

  visiting_top_visible_actor_ = false;
}

void LayerVisitor::VisitQuad(
    RealCompositor::QuadActor* actor) {
  DCHECK(actor->texture_data() == NULL);
  VisitTexturedQuadActor(actor, true);
}

void LayerVisitor::VisitImage(
    RealCompositor::ImageActor* actor) {
  VisitTexturedQuadActor(actor, actor->IsImageOpaque());
}

void LayerVisitor::VisitTexturePixmap(
    RealCompositor::TexturePixmapActor* actor) {
  bool visiting_top_visible_actor = visiting_top_visible_actor_;
  // OpenGlPixmapData is not created until OpenGlDrawVisitor has traversed
  // through the tree, which happens after the LayerVisitor, so we cannot rely
  // on actor->texture_data()->has_alpha() because texture_data() is NULL
  // in the beginning.
  // TODO: combine VisitQuad and VisitTexturePixmap.
  VisitTexturedQuadActor(actor, actor->pixmap_is_opaque());

  if (!actor->IsVisible() || actor->width() <= 0 || actor->height() <= 0)
    return;

  if (visiting_top_visible_actor && has_fullscreen_actor_)
    top_fullscreen_actor_ = actor;

  if (use_partial_updates_) {
    BoundingBox region = MapRegionToGlCoordinates(
        *stage_actor_,
        *actor,
        actor->GetDamagedRegion());
    updated_area_.merge(region);
  }
  actor->ResetDamagedRegion();
}

Rect LayerVisitor::GetDamagedRegion(int stage_width, int stage_height) {
  Rect region;
  if (use_partial_updates_) {
    float x_min = (updated_area_.x_min + 1.f) / 2.f * stage_width;
    float y_min = (updated_area_.y_min + 1.f) / 2.f * stage_height;
    float x_max = (updated_area_.x_max + 1.f) / 2.f * stage_width;
    float y_max = (updated_area_.y_max + 1.f) / 2.f * stage_height;
    region.x = static_cast<int>(x_min);
    region.y = static_cast<int>(y_min);
    // Important: To be properly conservative, the differences below need to
    // happen after the conversion to int.
    region.width = static_cast<int>(ceil(x_max)) - region.x;
    region.height = static_cast<int>(ceil(y_max)) - region.y;
  }
  return region;
}

}  // namespace window_manager
