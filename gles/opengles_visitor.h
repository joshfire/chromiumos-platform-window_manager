// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_GLES_GLES_VISITOR_H_
#define WINDOW_MANAGER_GLES_GLES_VISITOR_H_

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "base/logging.h"
#include "base/scoped_ptr.h"

#include "window_manager/tidy_interface.h"
#include "window_manager/gles/math_types.h"

namespace window_manager {

class ImageContainer;
class TexColorShader;
class TexShadeShader;
class Gles2Interface;

// This class vists an actor tree and draws it using OpenGLES
class OpenGlesDrawVisitor : virtual public RealCompositor::ActorVisitor {
 public:
  // IDs for storing drawing data
  enum DataId {
    kTextureData = 1,
    kEglImageData
  };

  OpenGlesDrawVisitor(Gles2Interface* gl,
                      RealCompositor* compositor,
                      Compositor::StageActor* stage);
  virtual ~OpenGlesDrawVisitor();

  void BindImage(const ImageContainer* container,
                 RealCompositor::QuadActor* actor);

  virtual void VisitActor(RealCompositor::Actor* actor) {}
  virtual void VisitStage(RealCompositor::StageActor* actor);
  virtual void VisitContainer(RealCompositor::ContainerActor* actor);
  virtual void VisitTexturePixmap(RealCompositor::TexturePixmapActor* actor);
  virtual void VisitQuad(RealCompositor::QuadActor* actor);

 private:
  Gles2Interface* gl_;  // Not owned.
  RealCompositor* compositor_;  // Not owned.
  Compositor::StageActor* stage_;  // Not owned.
  XConnection* x_connection_;  // Not owned.

  TexColorShader* tex_color_shader_;
  TexShadeShader* tex_shade_shader_;

  EGLDisplay egl_display_;
  EGLSurface egl_surface_;
  EGLContext egl_context_;

  // Matrix state
  Matrix4 perspective_;
  Matrix4 model_view_;

  // Cumulative opacity of the ancestors
  float ancestor_opacity_;

  // global vertex buffer object
  GLuint vertex_buffer_object_;

  // Temporary storage for client side vertex colors
  GLfloat colors_[4 * 4];

  DISALLOW_COPY_AND_ASSIGN(OpenGlesDrawVisitor);
};

class OpenGlesTextureData : public RealCompositor::DrawingData {
 public:
  explicit OpenGlesTextureData(Gles2Interface* gl);
  virtual ~OpenGlesTextureData();

  void SetTexture(GLuint texture, bool has_alpha);

  GLuint texture() const { return texture_; }
  bool has_alpha() const { return has_alpha_; }

 private:
  Gles2Interface* gl_;  // Not owned.

  // Texture ID of the wrapped texture; this takes ownership of the texture
  // handle
  GLuint texture_;

  // Does this texture require alpha-blending?
  bool has_alpha_;

  DISALLOW_COPY_AND_ASSIGN(OpenGlesTextureData);
};

class OpenGlesEglImageData : public RealCompositor::DrawingData {
 public:
  OpenGlesEglImageData(XConnection* x, Gles2Interface* gl);
  virtual ~OpenGlesEglImageData();

  // Bind to a window
  // HACK: work around broken eglCreateImageKHR calls that need the context
  bool Bind(RealCompositor::TexturePixmapActor* actor, EGLContext egl_context);

  // Has this been successfully bound?
  bool bound() const { return bound_; }

  // Create and bind a GL texture
  void BindTexture(OpenGlesTextureData* texture);

  // Respond to damage events
  void Refresh();

 private:
  // Has Bind() returned successfully
  bool bound_;

  // X Connection to manage the damage region.  Not owned.
  XConnection* x_;

  // Not owned.
  Gles2Interface* gl_;

  // ID of the damage region
  XID damage_;

  // Named X pixmap
  // TODO: lift as much as we can of the pixmap allocation and damage
  // region stuff to the RealCompositor layer
  XID pixmap_;

  // EGLImage
  EGLImageKHR egl_image_;
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_GLES_GLES_VISITOR_H_

