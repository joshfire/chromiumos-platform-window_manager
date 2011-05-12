// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_COMPOSITOR_XRENDER_XRENDER_VISITOR_H_
#define WINDOW_MANAGER_COMPOSITOR_XRENDER_XRENDER_VISITOR_H_

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/compositor/real_compositor.h"
#include "window_manager/compositor/texture_data.h"
#include "window_manager/x11/x_connection.h"

namespace window_manager {

class ImageContainer;

// This class visits an actor tree and draws it using the XRender extension.
class XRenderDrawVisitor : virtual public RealCompositor::ActorVisitor {
 public:
  XRenderDrawVisitor(RealCompositor* compositor,
                     Compositor::StageActor* stage);
  virtual ~XRenderDrawVisitor();

  void set_has_fullscreen_actor(bool has_fullscreen_actor) {
    has_fullscreen_actor_ = has_fullscreen_actor;
  }
  void set_damaged_region(Rect damaged_region) {
    damaged_region_ = damaged_region;
  }

  void BindImage(const ImageContainer& container,
                 RealCompositor::ImageActor* actor);

  virtual void VisitActor(RealCompositor::Actor* actor) {}
  virtual void VisitStage(RealCompositor::StageActor* actor);
  virtual void VisitContainer(RealCompositor::ContainerActor* actor);
  virtual void VisitImage(RealCompositor::ImageActor* actor);
  virtual void VisitTexturePixmap(RealCompositor::TexturePixmapActor* actor);
  virtual void VisitQuad(RealCompositor::QuadActor* actor);

 private:
  // So it can get access to the config data.
  friend class XRenderPixmapData;

  virtual bool FreeXResources();
  virtual bool AllocateXResources(Compositor::StageActor* stage);

  XWindow root_window_;
  XConnection::WindowGeometry root_geometry_;

  // |back_picture_| the corresponding |back_pixmap_| are used to
  // implement a back/front buffer system.
  XPicture back_picture_;
  XPixmap back_pixmap_;

  // |stage_picture_| is the picture for the front buffer.
  // We don't need a stage_pixmap_ here as it is provided
  // by the common code already.
  XPicture stage_picture_;

  // The visitor should not change settings in the compositor while visiting
  // actors throughout the drawing process because the compositor may decide
  // to skip drawing frames as an optimization.
  RealCompositor* compositor_;  // Not owned.
  XConnection* xconn_;  // Not owned.
  RealCompositor::StageActor* stage_; // Not owned.

  // This is the cumulative opacity of all the ancestors of the
  // currently visited node. It is recalculated each time we enter or
  // leave a container node.
  float ancestor_opacity_;

  // The rectangular region of the screen that is damaged in the frame.
  // This information allows the draw visitor to perform partial updates.
  Rect damaged_region_;

  // This is used to indicate whether the entire screen will be covered by an
  // actor so we can optimize by not clearing the back buffer.
  bool has_fullscreen_actor_;

  DISALLOW_COPY_AND_ASSIGN(XRenderDrawVisitor);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_COMPOSITOR_XRENDER_XRENDER_VISITOR_H_
