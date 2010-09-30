// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/gles/opengles_visitor.h"

#include <X11/Xlib.h>
#include <xcb/damage.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "base/logging.h"
#include "window_manager/gles/shaders.h"
#include "window_manager/gles/gles2_interface.h"
#include "window_manager/image_container.h"
#include "window_manager/real_x_connection.h"
#include "window_manager/x_connection.h"

#ifndef COMPOSITOR_OPENGLES
#error Need COMPOSITOR_OPENGLES defined to compile this file
#endif

// Work around broken eglext.h headers
#ifndef EGL_NO_IMAGE_KHR
#define EGL_NO_IMAGE_KHR (static_cast<EGLImageKHR>(0))
#endif
#ifndef EGL_IMAGE_PRESERVED_KHR
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#endif

namespace window_manager {

OpenGlesDrawVisitor::OpenGlesDrawVisitor(Gles2Interface* gl,
                                         RealCompositor* compositor,
                                         Compositor::StageActor* stage)
    : gl_(gl),
      compositor_(compositor),
      stage_(stage),
      x_connection_(compositor_->x_conn()),
      has_fullscreen_actor_(false) {
  CHECK(gl_);
  egl_display_ = gl_->egl_display();

  static const EGLint egl_config_attributes[] = {
    // Use the highest supported color depth.
    EGL_RED_SIZE, 1,
    EGL_GREEN_SIZE, 1,
    EGL_BLUE_SIZE, 1,
    EGL_DEPTH_SIZE, 16,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  EGLConfig egl_config;
  EGLint num_configs = 0;
  CHECK(gl_->EglChooseConfig(egl_display_, egl_config_attributes, &egl_config,
                             1, &num_configs) == EGL_TRUE)
      << "eglChooseConfig() failed: " << eglGetError();
  CHECK(num_configs == 1) << "Couldn't find EGL config.";

  egl_surface_ = gl_->EglCreateWindowSurface(
      egl_display_, egl_config,
      static_cast<EGLNativeWindowType>(stage->GetStageXWindow()),
      NULL);
  CHECK(egl_surface_ != EGL_NO_SURFACE) << "Failed to create EGL window.";

  static const EGLint egl_context_attributes[] = {
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
  };
  egl_context_ = gl_->EglCreateContext(egl_display_, egl_config, EGL_FALSE,
                                       egl_context_attributes);
  CHECK(egl_context_ != EGL_NO_CONTEXT) << "Failed to create EGL context.";

  CHECK(gl_->EglMakeCurrent(egl_display_, egl_surface_, egl_surface_,
                            egl_context_))
      << "eglMakeCurrent() failed: " << eglGetError();

  CHECK(gl_->InitExtensions()) << "Failed to load EGL/GL-ES extensions.";

  // Allocate shaders
  tex_color_shader_ = new TexColorShader();
  tex_shade_shader_ = new TexShadeShader();
  no_alpha_color_shader_ = new NoAlphaColorShader();
  no_alpha_shade_shader_ = new NoAlphaShadeShader();
  gl_->ReleaseShaderCompiler();

  // TODO: Move away from one global Vertex Buffer Object
  gl_->GenBuffers(1, &vertex_buffer_object_);
  CHECK(vertex_buffer_object_ > 0) << "VBO allocation failed.";
  gl_->BindBuffer(GL_ARRAY_BUFFER, vertex_buffer_object_);
  static float kQuad[] = {
    0.f, 0.f,
    0.f, 1.f,
    1.f, 0.f,
    1.f, 1.f,
  };
  gl_->BufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
}

OpenGlesDrawVisitor::~OpenGlesDrawVisitor() {
  delete tex_color_shader_;
  delete tex_shade_shader_;
  delete no_alpha_color_shader_;
  delete no_alpha_shade_shader_;

  gl_->DeleteBuffers(1, &vertex_buffer_object_);

  LOG_IF(ERROR, gl_->EglMakeCurrent(egl_display_, EGL_NO_SURFACE,
                                    EGL_NO_SURFACE,
                                    EGL_NO_CONTEXT) != EGL_TRUE)
      << "eglMakeCurrent() failed: " << eglGetError();
  LOG_IF(ERROR, gl_->EglDestroySurface(egl_display_, egl_surface_) != EGL_TRUE)
      << "eglDestroySurface() failed: " << eglGetError();
  LOG_IF(ERROR, gl_->EglDestroyContext(egl_display_, egl_context_) != EGL_TRUE)
      << "eglDestroyCotnext() failed: " << eglGetError();
}

void OpenGlesDrawVisitor::BindImage(const ImageContainer* container,
                                    RealCompositor::QuadActor* actor) {
  // TODO: Check container->format() and use a shader to swizzle BGR
  // data into RGB.
  GLenum gl_format = 0;
  GLenum gl_type = 0;
  switch (container->format()) {
    case IMAGE_FORMAT_RGBA_32:
    case IMAGE_FORMAT_RGBX_32:
      gl_format = GL_RGBA;
      gl_type = GL_UNSIGNED_BYTE;
      break;
    case IMAGE_FORMAT_BGRA_32:
    case IMAGE_FORMAT_BGRX_32:
      NOTIMPLEMENTED() << "BGR-order image data unsupported";
      gl_format = GL_RGBA;
      gl_type = GL_UNSIGNED_BYTE;
      break;
    case IMAGE_FORMAT_RGB_16:
      gl_format = GL_RGB;
      gl_type = GL_UNSIGNED_SHORT_5_6_5;
      break;
    default:
      NOTREACHED() << "Invalid image data format";
      break;
  }

  GLuint texture = 0;
  gl_->GenTextures(1, &texture);
  CHECK(texture > 0) << "Failed to allocated texture.";
  gl_->BindTexture(GL_TEXTURE_2D, texture);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl_->TexImage2D(GL_TEXTURE_2D, 0, gl_format,
                  container->width(), container->height(),
                  0, gl_format, gl_type, container->data());

  OpenGlesTextureData* data = new OpenGlesTextureData(gl_);
  data->SetTexture(texture);
  data->set_has_alpha(ImageFormatUsesAlpha(container->format()));
  actor->set_texture_data(data);
}

void OpenGlesDrawVisitor::VisitStage(RealCompositor::StageActor* actor) {
  if (!actor->IsVisible())
    return;

  if (actor->stage_color_changed()) {
    const Compositor::Color& color = actor->stage_color();
    gl_->ClearColor(color.red, color.green, color.blue, 1.f);
    actor->unset_stage_color_changed();
  }

  if (actor->was_resized()) {
    gl_->Viewport(0, 0, actor->width(), actor->height());
    actor->unset_was_resized();
  }

  // No need to clear color buffer if something will cover up the screen.
  if (has_fullscreen_actor_)
    gl_->Clear(GL_DEPTH_BUFFER_BIT);
  else
    gl_->Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  projection_ = actor->projection();
  ancestor_opacity_ = actor->opacity();

  // Back to front rendering
  // TODO: Switch to two pass Z-buffered rendering
  gl_->BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  gl_->Enable(GL_BLEND);

  // Back to front rendering
  const RealCompositor::ActorVector children = actor->GetChildren();
  for (RealCompositor::ActorVector::const_reverse_iterator i =
       children.rbegin(); i != children.rend(); ++i) {
    (*i)->Accept(this);
  }

  gl_->EglSwapBuffers(egl_display_, egl_surface_);
}

void OpenGlesDrawVisitor::VisitTexturePixmap(
    RealCompositor::TexturePixmapActor* actor) {
  if (!actor->IsVisible())
    return;

  if (!actor->texture_data()) {
    OpenGlesEglImageData image_data(gl_);

    if (!image_data.Bind(actor))
      return;

    OpenGlesTextureData* texture = new OpenGlesTextureData(gl_);
    image_data.BindTexture(texture, !actor->pixmap_is_opaque());
    actor->set_texture_data(texture);
  }

  VisitQuad(actor);
}

void OpenGlesDrawVisitor::VisitQuad(RealCompositor::QuadActor* actor) {
  if (!actor->IsVisible())
    return;

  // This must live until after the draw call, so it's at the top level
  scoped_array<float> colors;

  // mvp matrix
  Matrix4 mvp = projection_ * actor->model_view();

  // texture
  TextureData* texture_data = actor->texture_data();
  gl_->BindTexture(GL_TEXTURE_2D,
                   texture_data ? texture_data->texture() : 0);
  const bool texture_has_alpha = texture_data ?
                                   texture_data->has_alpha() :
                                   true;

  // shader
  if (actor->dimmed_opacity_begin() == 0.f &&
      actor->dimmed_opacity_end() == 0.f) {
    if (texture_has_alpha) {
      gl_->UseProgram(tex_color_shader_->program());
      gl_->UniformMatrix4fv(tex_color_shader_->MvpLocation(), 1, GL_FALSE,
                            &mvp[0][0]);
      gl_->Uniform1i(tex_color_shader_->SamplerLocation(), 0);
      gl_->Uniform4f(tex_color_shader_->ColorLocation(), actor->color().red,
                     actor->color().green, actor->color().blue,
                     actor->opacity() * ancestor_opacity_);

      gl_->BindBuffer(GL_ARRAY_BUFFER, vertex_buffer_object_);
      gl_->VertexAttribPointer(tex_color_shader_->PosLocation(),
                               2, GL_FLOAT, GL_FALSE, 0, 0);
      gl_->VertexAttribPointer(tex_color_shader_->TexInLocation(),
                               2, GL_FLOAT, GL_FALSE, 0, 0);
      tex_color_shader_->EnableVertexAttribs();
    } else {
      gl_->UseProgram(no_alpha_color_shader_->program());
      gl_->UniformMatrix4fv(no_alpha_color_shader_->MvpLocation(), 1, GL_FALSE,
                            &mvp[0][0]);
      gl_->Uniform1i(no_alpha_color_shader_->SamplerLocation(), 0);
      gl_->Uniform4f(no_alpha_color_shader_->ColorLocation(),
                     actor->color().red, actor->color().green,
                     actor->color().blue,
                     actor->opacity() * ancestor_opacity_);

      gl_->BindBuffer(GL_ARRAY_BUFFER, vertex_buffer_object_);
      gl_->VertexAttribPointer(no_alpha_color_shader_->PosLocation(),
                               2, GL_FLOAT, GL_FALSE, 0, 0);
      gl_->VertexAttribPointer(no_alpha_color_shader_->TexInLocation(),
                               2, GL_FLOAT, GL_FALSE, 0, 0);
      no_alpha_color_shader_->EnableVertexAttribs();
    }
  } else {
    const float actor_opacity = actor->opacity() * ancestor_opacity_;
    const float dimmed_transparency_begin = 1.f - actor->dimmed_opacity_begin();
    const float dimmed_transparency_end = 1.f - actor->dimmed_opacity_end();

    // TODO: Consider managing a ring buffer in a VBO ourselves.  Could be
    // better performance depending on driver quality.
    colors_[ 0] = dimmed_transparency_begin * actor->color().red;
    colors_[ 1] = dimmed_transparency_begin * actor->color().green;
    colors_[ 2] = dimmed_transparency_begin * actor->color().blue;
    colors_[ 3] = actor_opacity;

    colors_[ 4] = dimmed_transparency_begin * actor->color().red;
    colors_[ 5] = dimmed_transparency_begin * actor->color().green;
    colors_[ 6] = dimmed_transparency_begin * actor->color().blue;
    colors_[ 7] = actor_opacity;

    colors_[ 8] = dimmed_transparency_end * actor->color().red;
    colors_[ 9] = dimmed_transparency_end * actor->color().green;
    colors_[10] = dimmed_transparency_end * actor->color().blue;
    colors_[11] = actor_opacity;

    colors_[12] = dimmed_transparency_end * actor->color().red;
    colors_[13] = dimmed_transparency_end * actor->color().green;
    colors_[14] = dimmed_transparency_end * actor->color().blue;
    colors_[15] = actor_opacity;

    if (texture_has_alpha) {
      gl_->UseProgram(tex_shade_shader_->program());
      gl_->UniformMatrix4fv(tex_shade_shader_->MvpLocation(), 1, GL_FALSE,
                            &mvp[0][0]);
      gl_->Uniform1i(tex_shade_shader_->SamplerLocation(), 0);
      gl_->BindBuffer(GL_ARRAY_BUFFER, vertex_buffer_object_);
      gl_->VertexAttribPointer(tex_shade_shader_->PosLocation(),
                               2, GL_FLOAT, GL_FALSE, 0, 0);
      gl_->VertexAttribPointer(tex_shade_shader_->TexInLocation(),
                               2, GL_FLOAT, GL_FALSE, 0, 0);
      gl_->BindBuffer(GL_ARRAY_BUFFER, 0);
      gl_->VertexAttribPointer(tex_shade_shader_->ColorInLocation(),
                               4, GL_FLOAT, GL_FALSE, 0, colors_);
      tex_shade_shader_->EnableVertexAttribs();
    } else {
      gl_->UseProgram(no_alpha_shade_shader_->program());
      gl_->UniformMatrix4fv(no_alpha_shade_shader_->MvpLocation(), 1, GL_FALSE,
                            &mvp[0][0]);
      gl_->Uniform1i(no_alpha_shade_shader_->SamplerLocation(), 0);
      gl_->BindBuffer(GL_ARRAY_BUFFER, vertex_buffer_object_);
      gl_->VertexAttribPointer(no_alpha_shade_shader_->PosLocation(),
                               2, GL_FLOAT, GL_FALSE, 0, 0);
      gl_->VertexAttribPointer(no_alpha_shade_shader_->TexInLocation(),
                               2, GL_FLOAT, GL_FALSE, 0, 0);
      gl_->BindBuffer(GL_ARRAY_BUFFER, 0);
      gl_->VertexAttribPointer(no_alpha_shade_shader_->ColorInLocation(),
                               4, GL_FLOAT, GL_FALSE, 0, colors_);
      no_alpha_shade_shader_->EnableVertexAttribs();
    }
  }

  // Draw
  gl_->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void OpenGlesDrawVisitor::VisitContainer(
    RealCompositor::ContainerActor* actor) {
  if (!actor->IsVisible())
    return;

  LOG(INFO) << "Visit container: " << actor->name();

  const float original_opacity = ancestor_opacity_;
  ancestor_opacity_ *= actor->opacity();

  // Back to front rendering
  const RealCompositor::ActorVector children = actor->GetChildren();
  for (RealCompositor::ActorVector::const_reverse_iterator i =
       children.rbegin(); i != children.rend(); ++i) {
    (*i)->Accept(this);
  }

  // Reset opacity.
  ancestor_opacity_ = original_opacity;
}

OpenGlesTextureData::OpenGlesTextureData(Gles2Interface* gl)
    : gl_(gl) {}

OpenGlesTextureData::~OpenGlesTextureData() {
  gl_->DeleteTextures(1, texture_ptr());
}

void OpenGlesTextureData::SetTexture(GLuint texture) {
  gl_->DeleteTextures(1, texture_ptr());
  set_texture(texture);
}

OpenGlesEglImageData::OpenGlesEglImageData(Gles2Interface* gl)
    : bound_(false),
      gl_(gl),
      egl_image_(EGL_NO_IMAGE_KHR) {
}

OpenGlesEglImageData::~OpenGlesEglImageData() {
  if (egl_image_)
    gl_->EglDestroyImageKHR(gl_->egl_display(), egl_image_);
}

bool OpenGlesEglImageData::Bind(RealCompositor::TexturePixmapActor* actor) {
  DCHECK(actor);
  CHECK(!bound_);

  if (!actor->pixmap()) {
    LOG(INFO) << "No pixmap for actor \"" << actor->name() << "\"";
    return false;
  }

  static const EGLint egl_image_attribs[] = {
    EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
    EGL_NONE
  };
  egl_image_ = gl_->EglCreateImageKHR(
      gl_->egl_display(), EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
      reinterpret_cast<EGLClientBuffer>(actor->pixmap()), egl_image_attribs);
  if (egl_image_ == EGL_NO_IMAGE_KHR) {
    LOG(INFO) << "eglCreateImageKHR() returned EGL_NO_IMAGE_KHR.";
    return false;
  }

  bound_ = true;
  return true;
}

void OpenGlesEglImageData::BindTexture(OpenGlesTextureData* texture_data,
                                       bool has_alpha) {
  CHECK(bound_);

  GLuint texture;
  gl_->GenTextures(1, &texture);
  CHECK(texture > 0) << "Failed to allocated texture.";
  gl_->BindTexture(GL_TEXTURE_2D, texture);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl_->EGLImageTargetTexture2DOES(GL_TEXTURE_2D,
                                  static_cast<GLeglImageOES>(egl_image_));

  texture_data->SetTexture(texture);
  texture_data->set_has_alpha(has_alpha);
}

}  // namespace window_manager
