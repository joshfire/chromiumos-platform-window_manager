// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_LAYER_VISITOR_H_
#define WINDOW_MANAGER_LAYER_VISITOR_H_

#include "window_manager/compositor/real_compositor.h"

namespace window_manager {

// LayerVisitor is used to update actors' opacities, z-depths, transformation
// matrices and culling information.  It traverses through the actor tree
// before DrawVisitor on each frame.  LayerVisitor keeps information about the
// composition of the actors during the traversal, and the information is used
// to help RealCompositor and DrawCompositor perform optimizations.
class LayerVisitor : virtual public RealCompositor::ActorVisitor {
 public:
  struct BoundingBox {
    BoundingBox() : x_min(0.0f), x_max(0.0f), y_min(0.0f), y_max(0.0f) {}
    BoundingBox(float x0, float x1, float y0, float y1)
        : x_min(x0),
          x_max(x1),
          y_min(y0),
          y_max(y1) {}

    void merge(const BoundingBox& other) {
      if (other.x_min == other.x_max || other.y_min == other.y_max)
        return;
      if (x_min == x_max || y_min == y_max) {
        x_min = other.x_min;
        x_max = other.x_max;
        y_min = other.y_min;
        y_max = other.y_max;
      } else {
        x_min = std::min(x_min, other.x_min);
        x_max = std::max(x_max, other.x_max);
        y_min = std::min(y_min, other.y_min);
        y_max = std::max(y_max, other.y_max);
      }
    }

    void clear() {
      x_min = 0.f;
      x_max = 0.f;
      y_min = 0.f;
      y_max = 0.f;
    }

    float x_min;
    float x_max;
    float y_min;
    float y_max;
  };

  static const float kMinDepth;
  static const float kMaxDepth;

  LayerVisitor(int32 count, bool use_partial_updates)
      : depth_(0.0f),
        layer_thickness_(0.0f),
        count_(count),
        has_fullscreen_actor_(false),
        stage_actor_(NULL),
        visiting_top_visible_actor_(true),
        top_fullscreen_actor_(NULL),
        updated_area_(0.0f, 0.0f, 0.0f, 0.0f),
        use_partial_updates_(use_partial_updates) {}
  virtual ~LayerVisitor() {}

  bool has_fullscreen_actor() const { return has_fullscreen_actor_; }
  const RealCompositor::TexturePixmapActor* top_fullscreen_actor() const {
    return top_fullscreen_actor_;
  }

  virtual void VisitActor(RealCompositor::Actor* actor);
  virtual void VisitStage(RealCompositor::StageActor* actor);
  virtual void VisitContainer(RealCompositor::ContainerActor* actor);
  virtual void VisitQuad(RealCompositor::QuadActor* actor);
  virtual void VisitImage(RealCompositor::ImageActor* actor);
  virtual void VisitTexturePixmap(RealCompositor::TexturePixmapActor* actor);

  void VisitTexturedQuadActor(RealCompositor::QuadActor* actor,
      bool is_texture_opaque);

  // Get the damaged region in screen coordinates where (0, 0) is bottom_left
  // and (w-1, h-1) is top_right.
  Rect GetDamagedRegion(int stage_width, int stage_height);

 private:
  float depth_;
  float layer_thickness_;
  int32 count_;
  bool has_fullscreen_actor_;
  const RealCompositor::StageActor* stage_actor_;

  // This flag indicates whether the actor being visited is the topmost
  // visible actor.
  bool visiting_top_visible_actor_;

  // This keeps track of the actor that is fullscreen and topmost visible
  // during the traversal. It is set to NULL if either of the criteria is
  // not satisfied.
  const RealCompositor::TexturePixmapActor* top_fullscreen_actor_;

  // This restores the dirty region union of all actors from the most
  // recent VisitStage.  It's defined in GL coordinates where (-1, -1) is
  // bottom_left and (1, 1) is top_right.
  BoundingBox updated_area_;

  bool use_partial_updates_;

  DISALLOW_COPY_AND_ASSIGN(LayerVisitor);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_LAYER_VISITOR_H_
