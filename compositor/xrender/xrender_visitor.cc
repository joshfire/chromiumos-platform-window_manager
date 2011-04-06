// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor/xrender/xrender_visitor.h"

#include <X11/extensions/Xrender.h>

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "window_manager/image_container.h"
#include "window_manager/image_enums.h"
#include "window_manager/util.h"
#include "window_manager/x11/x_connection.h"


#ifndef COMPOSITOR_XRENDER
#error Need COMPOSITOR_XRENDER defined to compile this file
#endif

namespace window_manager {

const int kRGBPictureBitDepth = 24;
const int kRGBAPictureBitDepth = 32;

class XRenderPictureData : public TextureData {
 public:
  XRenderPictureData(XConnection* xconn)
      : pixmap_(0),
        xconn_(xconn) {
  }
  virtual ~XRenderPictureData() {
    xconn_->RenderFreePicture(texture());
  }

  // Initialize our texture and make it contain the current contents of the
  // passed-in pixmap.  False is returned if the process fails (in
  // which case this object should be thrown away).
  bool Init(XPixmap pixmap, int bpp) {
    XPicture picture = xconn_->RenderCreatePicture(pixmap, bpp);
    set_texture(picture);
    return (picture != None);
  }

 private:
  // The actor's X pixmap.  Ownership of the pixmap remains with the caller.
  XPixmap pixmap_;

  XConnection* xconn_;  // Not owned.
};

XRenderDrawVisitor::XRenderDrawVisitor(RealCompositor* compositor,
                                       Compositor::StageActor* stage)
    : root_window_(None),
      back_picture_(None),
      back_pixmap_(None),
      stage_picture_(None),
      compositor_(NULL),
      xconn_(compositor->x_conn()),
      stage_(NULL),
      ancestor_opacity_(1.0),
      has_fullscreen_actor_(false) {
  // Check for the XRender extension.
  CHECK(xconn_->RenderQueryExtension());

  CHECK(AllocateXResources(stage));
}

XRenderDrawVisitor::~XRenderDrawVisitor() {
  CHECK(FreeXResources());
}

void XRenderDrawVisitor::BindImage(const ImageContainer& container,
                                   RealCompositor::ImageActor* actor) {
  XPixmap pixmap = xconn_->CreatePixmapFromContainer(container);

  scoped_ptr<window_manager::XRenderPictureData> data(
      new XRenderPictureData(this->xconn_));
  data->Init(pixmap, kRGBAPictureBitDepth);
  actor->set_texture_data(data.release());
}

void XRenderDrawVisitor::VisitStage(RealCompositor::StageActor* actor) {
  if (!actor->IsVisible())
    return;

  stage_ = actor;

  if (actor->was_resized()) {
    CHECK(FreeXResources());
    CHECK(AllocateXResources(actor));
    actor->unset_was_resized();
  }

  // If we don't have a full screen actor we do a fill with the stage color.
  if (!has_fullscreen_actor_) {
    const Compositor::Color& color = actor->stage_color();
    xconn_->RenderFillRectangle(back_picture_,
                                color.red,
                                color.green,
                                color.blue,
                                Point(0, 0),
                                root_geometry_.bounds.size());
  }

#ifdef EXTRA_LOGGING
  DLOG(INFO) << "Starting Render pass.";
#endif

  ancestor_opacity_ = actor->opacity();

  // Walk the actors and render them
  VisitContainer(actor);

#ifdef EXTRA_LOGGING
  DLOG(INFO) << "Ending Render pass.";
#endif

  if (!damaged_region_.empty()) {
    Matrix4 identity = Matrix4::identity();
    identity[0][0] = damaged_region_.width;
    identity[1][1] = damaged_region_.height;
    identity[3][0] = damaged_region_.x;
    identity[3][1] = root_geometry_.bounds.height -
                     damaged_region_.y - damaged_region_.height;

    xconn_->RenderComposite(false,
                            back_picture_,
                            None,
                            stage_picture_,
                            Point(damaged_region_.x,
                                  root_geometry_.bounds.height -
                                  damaged_region_.y -
                                  damaged_region_.height),
                            Point(0, 0),
                            identity,
                            damaged_region_.size());
  } else {
    Matrix4 identity = Vectormath::Aos::Matrix4::identity();
    identity[0][0] = root_geometry_.bounds.width;
    identity[1][1] = root_geometry_.bounds.height;
    xconn_->RenderComposite(false,
                            back_picture_,
                            None,
                            stage_picture_,
                            Point(0, 0),
                            Point(0, 0),
                            identity,
                            root_geometry_.bounds.size());
  }

  stage_ = NULL;
}

void XRenderDrawVisitor::VisitContainer(RealCompositor::ContainerActor* actor) {
  if (!actor->IsVisible())
    return;

#ifdef EXTRA_LOGGING
  DLOG(INFO) << "Drawing container " << actor->name() << ".";
  DLOG(INFO) << "  at: (" << actor->x() << ", "  << actor->y()
             << ", " << actor->z() << ") with scale: ("
             << actor->scale_x() << ", "  << actor->scale_y() << ") at size ("
             << actor->width() << "x"  << actor->height() << ")";
#endif
  RealCompositor::ActorVector children = actor->GetChildren();

  float original_opacity = ancestor_opacity_;
  ancestor_opacity_ *= actor->opacity();

  // Walk backwards so we go back to front.
  RealCompositor::ActorVector::const_reverse_iterator iterator;
  for (iterator = children.rbegin(); iterator != children.rend();
       ++iterator) {
    RealCompositor::Actor* child = *iterator;

    if (child->IsVisible()) {
#ifdef EXTRA_LOGGING
      DLOG(INFO) << "Drawing child " << child->name()
                 << " (visible: " << child->IsVisible()
                 << ", has_children: " << child->has_children()
                 << ", opacity: " << child->opacity()
                 << ", ancestor_opacity: " << ancestor_opacity_
                 << ", is_opaque: " << child->is_opaque() << ")";
#endif
      child->Accept(this);
    } else {
#ifdef EXTRA_LOGGING
      DLOG(INFO) << "NOT drawing child " << child->name()
                 << " (visible: " << child->IsVisible()
                 << ", has_children: " << child->has_children()
                 << ", opacity: " << child->opacity()
                 << ", ancestor_opacity: " << ancestor_opacity_
                 << ", is_opaque: " << child->is_opaque() << ")";
#endif
    }

    // Reset ancestor opacity.
    ancestor_opacity_ = original_opacity;
  }
}

void XRenderDrawVisitor::VisitImage(RealCompositor::ImageActor* actor) {
  if (!actor->IsVisible())
    return;

  // All ImageActors are also QuadActors, and so we let the
  // QuadActor do all the actual drawing.
  VisitQuad(actor);
}

void XRenderDrawVisitor::VisitTexturePixmap(
    RealCompositor::TexturePixmapActor* actor) {
  if (!actor->IsVisible())
    return;

  // Make sure we have an XRender pic for this pixmap
  if (!actor->texture_data())  {
    if (actor->pixmap()) {
      scoped_ptr<window_manager::XRenderPictureData>
          data(new XRenderPictureData(this->xconn_));
      data->Init(actor->pixmap(),
                 actor->pixmap_is_opaque() ?
                 kRGBPictureBitDepth : kRGBAPictureBitDepth);
      actor->set_texture_data(data.release());
    }
  }

  // All texture pixmaps are also QuadActors, and so we let the
  // QuadActor do all the actual drawing.
  VisitQuad(actor);
}

void XRenderDrawVisitor::VisitQuad(RealCompositor::QuadActor* actor) {
  if (!actor->IsVisible())
    return;

#ifdef EXTRA_LOGGING
  DLOG(INFO) << "Drawing quad " << actor->name() << ".";
#endif

  // Calculate the vertex colors, taking into account the actor color,
  // opacity and the dimming gradient.
  float actor_opacity = actor->is_opaque() ?
                        1.f :
                        actor->opacity() * ancestor_opacity_;
  float dimmed_transparency_begin = 1.f - actor->dimmed_opacity_begin();
  float dimmed_transparency_end = 1.f - actor->dimmed_opacity_end();
  float red = actor->color().red;
  float green = actor->color().green;
  float blue = actor->color().blue;
  DCHECK_LE(actor_opacity, 1.f);
  DCHECK_GE(actor_opacity, 0.f);
  DCHECK_LE(dimmed_transparency_begin, 1.f);
  DCHECK_GE(dimmed_transparency_begin, 0.f);
  DCHECK_LE(dimmed_transparency_end, 1.f);
  DCHECK_GE(dimmed_transparency_end, 0.f);
  DCHECK_LE(red, 1.f);
  DCHECK_GE(red, 0.f);
  DCHECK_LE(green, 1.f);
  DCHECK_GE(green, 0.f);
  DCHECK_LE(blue, 1.f);
  DCHECK_GE(blue, 0.f);

  xconn_->RenderComposite(
      !actor->is_opaque(),
      static_cast<XPicture>(actor->texture_data()->texture()),
      static_cast<XPicture>(None),
      back_picture_,
      Point(0, 0),
      Point(0, 0),
      actor->model_view(),
      actor->GetBounds().size());
}

bool XRenderDrawVisitor::FreeXResources() {
  return xconn_->FreePixmap(back_pixmap_) &&
         xconn_->RenderFreePicture(back_picture_) &&
         xconn_->RenderFreePicture(stage_picture_);
}

bool XRenderDrawVisitor::AllocateXResources(Compositor::StageActor* stage) {
  // Find root window geometry.
  root_window_ = stage->GetStageXWindow();
  xconn_->GetWindowGeometry(root_window_, &root_geometry_);

  // Create back pixmap.
  back_pixmap_ =
      xconn_->CreatePixmap(
          root_window_,
          root_geometry_.bounds.size(),
          root_geometry_.depth);

  // Create back picture.
  back_picture_ = xconn_->RenderCreatePicture(back_pixmap_,
                                              kRGBPictureBitDepth);

  // Create stage picture.
  stage_picture_ = xconn_->RenderCreatePicture(root_window_,
                                               kRGBPictureBitDepth);

  return (back_pixmap_ != None) &&
         (back_picture_ != None) &&
         (stage_picture_ != None);
}


}  // namespace window_manager
