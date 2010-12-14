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

#include "window_manager/compositor.h"
#include "window_manager/math_types.h"
#include "window_manager/real_compositor.h"
#include "window_manager/texture_data.h"

namespace window_manager {

class ImageContainer;
class TexColorShader;
class TexShadeShader;
class NoAlphaColorShader;
class NoAlphaShadeShader;
class Gles2Interface;

// This class vists an actor tree and draws it using OpenGLES
class OpenGlesDrawVisitor : virtual public RealCompositor::ActorVisitor {
 public:
  OpenGlesDrawVisitor(Gles2Interface* gl,
                      RealCompositor* compositor,
                      Compositor::StageActor* stage);
  virtual ~OpenGlesDrawVisitor();
  void set_has_fullscreen_actor(bool has_fullscreen_actor) {
    has_fullscreen_actor_ = has_fullscreen_actor;
  }
  void set_damaged_region(Rect damaged_region) {
    damaged_region_ = damaged_region;
  }

  void BindImage(const ImageContainer* container,
                 RealCompositor::QuadActor* actor);

  virtual void VisitActor(RealCompositor::Actor* actor) {}
  virtual void VisitStage(RealCompositor::StageActor* actor);

  void DrawQuad(RealCompositor::QuadActor* actor,
                float ancestor_opacity) const;
  void CreateTextureData(RealCompositor::TexturePixmapActor *actor) const;

 protected:
  // Manage the scissor rect stack. Pushing a rect on the stack intersects the
  // new rect with the current rect (if any) and enables the GL scissor test
  // if it isn't already. Popping restores the previous rect or disables
  // scissoring if the stack is now empty.
  void PushScissorRect(const Rect& scissor);
  void PopScissorRect();

 private:
  Gles2Interface* gl_;  // Not owned.
  RealCompositor* compositor_;  // Not owned.
  Compositor::StageActor* stage_;  // Not owned.
  XConnection* x_connection_;  // Not owned.

  TexColorShader* tex_color_shader_;
  TexShadeShader* tex_shade_shader_;
  NoAlphaColorShader* no_alpha_color_shader_;
  NoAlphaShadeShader* no_alpha_shade_shader_;

  EGLDisplay egl_display_;
  EGLSurface egl_surface_;
  bool egl_surface_is_capable_of_partial_updates_;
  EGLContext egl_context_;

  // Matrix state
  Matrix4 projection_;

  // Scissor rect data.
  std::vector<Rect> scissor_stack_;

  // global vertex buffer object
  GLuint vertex_buffer_object_;

  // This is used to indicate whether the entire screen will be covered by an
  // actor so we can optimize by not clearing the COLOR_BUFFER_BIT.
  bool has_fullscreen_actor_;

  // The rectangular region of the screen that is damaged in the frame.
  // This information allows the draw visitor to perform partial updates.
  Rect damaged_region_;

  DISALLOW_COPY_AND_ASSIGN(OpenGlesDrawVisitor);
};

// TODO: further combine texture class between GL and GLES after common GL
// functions are combined.
class OpenGlesTextureData : public TextureData {
 public:
  explicit OpenGlesTextureData(Gles2Interface* gl);
  virtual ~OpenGlesTextureData();

  void SetTexture(GLuint texture);

 private:
  Gles2Interface* gl_;  // Not owned.

  DISALLOW_COPY_AND_ASSIGN(OpenGlesTextureData);
};

class OpenGlesEglImageData : public TextureData {
 public:
  OpenGlesEglImageData(Gles2Interface* gl);
  virtual ~OpenGlesEglImageData();

  // Bind to a pixmap.
  bool Bind(RealCompositor::TexturePixmapActor* actor);

  // Has this been successfully bound?
  bool bound() const { return bound_; }

  // Create and bind a GL texture
  void BindTexture(OpenGlesTextureData* texture, bool has_alpha);

 private:
  // Has Bind() returned successfully
  bool bound_;

  // Not owned.
  Gles2Interface* gl_;

  // EGLImage
  EGLImageKHR egl_image_;
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_GLES_GLES_VISITOR_H_

