// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor/gl/opengl_visitor.h"

#include <sys/time.h>

#include <algorithm>
#include <ctime>
#include <string>

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <gflags/gflags.h>

#include "base/basictypes.h"
#include "base/logging.h"
#include "window_manager/compositor/gl/gl_interface.h"
#include "window_manager/image_container.h"
#include "window_manager/image_enums.h"
#include "window_manager/profiler.h"
#include "window_manager/util.h"

DECLARE_bool(compositor_display_debug_needle);

#ifndef COMPOSITOR_OPENGL
#error Need COMPOSITOR_OPENGL defined to compile this file
#endif

// Turn this on if you want to debug something in this file in depth.
#undef EXTRA_LOGGING

// Turn this on to do GL debugging.
#undef GL_ERROR_DEBUGGING
#ifdef GL_ERROR_DEBUGGING
#define CHECK_GL_ERROR(gl_interface) do {                                  \
    GLenum gl_error = gl_interface->GetError();                            \
    LOG_IF(ERROR, gl_error != GL_NO_ERROR) << "GL Error :" << gl_error;    \
  } while (0)
#else  // GL_ERROR_DEBUGGING
#define CHECK_GL_ERROR(gl_interface_) void(0)
#endif  // GL_ERROR_DEBUGGING

using window_manager::util::XidStr;

namespace window_manager {

OpenGlPixmapData::OpenGlPixmapData(OpenGlDrawVisitor* visitor)
    : visitor_(visitor),
      gl_(visitor_->gl_interface_),
      pixmap_(0),
      glx_pixmap_(0) {
  DCHECK(visitor);
}

OpenGlPixmapData::~OpenGlPixmapData() {
  if (texture())
    gl_->DeleteTextures(1, texture_ptr());
  if (glx_pixmap_) {
    gl_->DestroyGlxPixmap(glx_pixmap_);
    glx_pixmap_ = 0;
  }
  visitor_ = NULL;
  gl_ = NULL;
  pixmap_ = 0;
}

void OpenGlPixmapData::Refresh() {
  DCHECK(texture());
  gl_->BindTexture(GL_TEXTURE_2D, texture());

  if (gl_->HasTextureFromPixmapExtension()) {
    DCHECK(glx_pixmap_);
    gl_->ReleaseGlxTexImage(glx_pixmap_, GLX_FRONT_LEFT_EXT);
    gl_->BindGlxTexImage(glx_pixmap_, GLX_FRONT_LEFT_EXT, NULL);
  } else {
    CopyPixmapImageToTexture();
  }

  CHECK_GL_ERROR(gl_);
}

bool OpenGlPixmapData::Init(RealCompositor::TexturePixmapActor* actor) {
  DCHECK(actor);
  if (!actor->pixmap()) {
    LOG(WARNING) << "Can't create GLX pixmap for actor \"" << actor->name()
                 << "\", since it doesn't have an X pixmap";
    return false;
  }

  CHECK(!pixmap_) << "Pixmap data was already initialized";
  pixmap_ = actor->pixmap();

  const bool use_glx_pixmap = gl_->HasTextureFromPixmapExtension();
  if (use_glx_pixmap) {
    const int kGlxPixmapAttribs[] = {
      GLX_TEXTURE_FORMAT_EXT,
      actor->pixmap_is_opaque() ?
        GLX_TEXTURE_FORMAT_RGB_EXT :
        GLX_TEXTURE_FORMAT_RGBA_EXT,
      GLX_TEXTURE_TARGET_EXT,
      GLX_TEXTURE_2D_EXT,
      0
    };
    glx_pixmap_ = gl_->CreateGlxPixmap(
        actor->pixmap_is_opaque() ?
          visitor_->framebuffer_config_rgb_ :
          visitor_->framebuffer_config_rgba_,
        actor->pixmap(),
        kGlxPixmapAttribs);
    CHECK_GL_ERROR(gl_);
    if (!glx_pixmap_) {
      LOG(WARNING) << "Failed to create GLX pixmap for actor \""
                   << actor->name() << "\" using pixmap "
                   << XidStr(actor->pixmap());
      return false;
    }
  } else {
    if (!visitor_->xconn()->GetWindowGeometry(pixmap_, &pixmap_geometry_)) {
      LOG(WARNING) << "Unable to fetch geometry for pixmap " << XidStr(pixmap_);
      return false;
    }
  }

  GLuint new_texture = 0;
  gl_->GenTextures(1, &new_texture);
  gl_->BindTexture(GL_TEXTURE_2D, new_texture);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (use_glx_pixmap) {
    gl_->BindGlxTexImage(glx_pixmap_, GLX_FRONT_LEFT_EXT, NULL);
  } else {
    if (!CopyPixmapImageToTexture())
      return false;
  }

  CHECK_GL_ERROR(gl_);
  set_texture(new_texture);
  return true;
}

bool OpenGlPixmapData::CopyPixmapImageToTexture() {
  DCHECK(pixmap_);
  DCHECK(!gl_->HasTextureFromPixmapExtension());

  scoped_ptr_malloc<uint8_t> data;
  ImageFormat format = IMAGE_FORMAT_UNKNOWN;
  if (!visitor_->xconn()->GetImage(
          pixmap_,
          Rect(Point(0, 0), pixmap_geometry_.bounds.size()),
          pixmap_geometry_.depth,
          &data,
          &format)) {
    LOG(WARNING) << "Unable to fetch image from pixmap " << XidStr(pixmap_);
    return false;
  }

  InMemoryImageContainer image_container(
      data.release(),
      pixmap_geometry_.bounds.width,
      pixmap_geometry_.bounds.height,
      format,
      true);

  GLenum pixel_data_format = 0;
  GLenum pixel_data_type = GL_UNSIGNED_BYTE;
  GLenum internal_format = GL_RGBA;
  switch (format) {
    case IMAGE_FORMAT_RGBA_32:  // fallthrough
    case IMAGE_FORMAT_RGBX_32:
      pixel_data_format = GL_RGBA;
      break;
    case IMAGE_FORMAT_BGRA_32:  // fallthrough
    case IMAGE_FORMAT_BGRX_32:
      pixel_data_format = GL_BGRA;
      break;
    case IMAGE_FORMAT_RGB_16:
      internal_format = GL_RGB;
      pixel_data_format = GL_RGB;
      pixel_data_type = GL_UNSIGNED_SHORT_5_6_5;
      break;
    default:
      NOTREACHED() << "Unhandled image container data format " << format;
      return false;
  }

  gl_->TexImage2D(GL_TEXTURE_2D, 0, internal_format,
                  image_container.width(), image_container.height(),
                  0, pixel_data_format, pixel_data_type,
                  image_container.data());
  return true;
}


OpenGlTextureData::OpenGlTextureData(GLInterface* gl_interface)
    : gl_interface_(gl_interface) {}

OpenGlTextureData::~OpenGlTextureData() {
  if (texture())
    gl_interface_->DeleteTextures(1, texture_ptr());
}

void OpenGlTextureData::SetTexture(GLuint texture) {
  if (this->texture() && this->texture() != texture)
    gl_interface_->DeleteTextures(1, texture_ptr());
  set_texture(texture);
}

OpenGlDrawVisitor::OpenGlQuadDrawingData::OpenGlQuadDrawingData(
    GLInterface* gl_interface)
    : gl_interface_(gl_interface) {
  gl_interface_->GenBuffers(1, &vertex_buffer_);
  gl_interface_->BindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);

  static const float kQuad[] = {
    0.f, 0.f,
    0.f, 1.f,
    1.f, 0.f,
    1.f, 1.f,
  };

  gl_interface_->BufferData(GL_ARRAY_BUFFER, sizeof(kQuad),
                            kQuad, GL_STATIC_DRAW);
  color_buffer_.reset(new float[4*4]);
  CHECK_GL_ERROR(gl_interface_);
}

OpenGlDrawVisitor::OpenGlQuadDrawingData::~OpenGlQuadDrawingData() {
  if (vertex_buffer_)
    gl_interface_->DeleteBuffers(1, &vertex_buffer_);
}

void OpenGlDrawVisitor::OpenGlQuadDrawingData::set_vertex_color(int index,
                                                                float r,
                                                                float g,
                                                                float b,
                                                                float a) {
  // Calculate member index
  index *= 4;
  // Assign the color.
  color_buffer_[index++] = r;
  color_buffer_[index++] = g;
  color_buffer_[index++] = b;
  color_buffer_[index] = a;
}

OpenGlDrawVisitor::OpenGlStateCache::OpenGlStateCache() {
  Invalidate();
}

void OpenGlDrawVisitor::OpenGlStateCache::Invalidate() {
  actor_opacity_ = -1.f;
  dimmed_transparency_begin_ = -1.f;
  dimmed_transparency_end_ = -1.f;
  red_ = -1.f;
  green_ = -1.f;
  blue_ = -1.f;
}

bool OpenGlDrawVisitor::OpenGlStateCache::ColorStateChanged(
    float actor_opacity,
    float dimmed_transparency_begin,
    float dimmed_transparency_end,
    float red, float green, float blue) {
  if (actor_opacity != actor_opacity_ ||
      dimmed_transparency_begin != dimmed_transparency_begin_ ||
      dimmed_transparency_end != dimmed_transparency_end_ ||
      red != red_ || green != green_ || blue != blue_) {
    actor_opacity_ = actor_opacity;
    dimmed_transparency_begin_ = dimmed_transparency_begin;
    dimmed_transparency_end_ = dimmed_transparency_end;
    red_ = red;
    green_ = green;
    blue_ = blue;
    return true;
  }
  return false;
}

OpenGlDrawVisitor::OpenGlDrawVisitor(GLInterface* gl_interface,
                                     RealCompositor* compositor,
                                     Compositor::StageActor* stage)
    : compositor_(compositor),
      gl_interface_(gl_interface),
      xconn_(compositor_->x_conn()),
      stage_(NULL),
      framebuffer_config_rgb_(0),
      framebuffer_config_rgba_(0),
      context_(0),
      ancestor_opacity_(1.0f),
      num_frames_drawn_(0),
      has_fullscreen_actor_(false) {
  CHECK(gl_interface_);
  context_ = gl_interface_->CreateGlxContext();
  CHECK(context_) << "Unable to create a context from the available visuals.";
  CHECK(gl_interface_->IsGlxDirect(context_))
      << "Direct rendering is required (indirect mode doesn't support vertex "
      << "buffer objects).";

  gl_interface_->MakeGlxCurrent(stage->GetStageXWindow(), context_);

  if (gl_interface_->HasTextureFromPixmapExtension())
    FindFramebufferConfigurations();

  gl_interface_->BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  CHECK_GL_ERROR(gl_interface_);

  quad_drawing_data_.reset(new OpenGlQuadDrawingData(gl_interface_));
}

void OpenGlDrawVisitor::FindFramebufferConfigurations() {
  PROFILER_MARKER_BEGIN(FindFramebufferConfigurations);
  int num_fb_configs;
  GLXFBConfig config_32 = 0;
  GLXFBConfig config_24 = 0;
  GLXFBConfig* fb_configs = gl_interface_->GetGlxFbConfigs(&num_fb_configs);
  bool rgba = false;
  for (int i = 0; i < num_fb_configs; ++i) {
    XVisualInfo* visual_info =
        gl_interface_->GetGlxVisualFromFbConfig(fb_configs[i]);
    if (!visual_info)
      continue;

    int visual_depth = visual_info->depth;
    gl_interface_->GlxFree(visual_info);
    if (visual_depth != 32 && visual_depth != 24)
      continue;

    int alpha = 0;
    int buffer_size = 0;
    gl_interface_->GetGlxFbConfigAttrib(fb_configs[i], GLX_ALPHA_SIZE, &alpha);
    gl_interface_->GetGlxFbConfigAttrib(fb_configs[i], GLX_BUFFER_SIZE,
                                        &buffer_size);

    if (buffer_size != visual_depth && (buffer_size - alpha) != visual_depth)
      continue;

    int x_visual = 0;
    gl_interface_->GetGlxFbConfigAttrib(fb_configs[i], GLX_X_VISUAL_TYPE,
                                        &x_visual);
    if (x_visual != GLX_TRUE_COLOR)
      continue;

    int has_rgba = 0;
    if (visual_depth == 32) {
      gl_interface_->GetGlxFbConfigAttrib(fb_configs[i],
                                          GLX_BIND_TO_TEXTURE_RGBA_EXT,
                                          &has_rgba);
      if (has_rgba)
        rgba = true;
    }

    if (!has_rgba) {
      if (rgba)
        continue;

      int has_rgb = 0;
      gl_interface_->GetGlxFbConfigAttrib(fb_configs[i],
                                          GLX_BIND_TO_TEXTURE_RGB_EXT,
                                          &has_rgb);
      if (!has_rgb)
        continue;
    }
    if (visual_depth == 32) {
      config_32 = fb_configs[i];
    } else {
      config_24 = fb_configs[i];
    }
  }
  gl_interface_->GlxFree(fb_configs);

  CHECK(config_24)
      << "Unable to obtain appropriate RGB framebuffer configuration.";

  CHECK(config_32)
      << "Unable to obtain appropriate RGBA framebuffer configuration.";

  framebuffer_config_rgba_ = config_32;
  framebuffer_config_rgb_ = config_24;
  PROFILER_MARKER_END(FindFramebufferConfigurations);
}

OpenGlDrawVisitor::~OpenGlDrawVisitor() {
  gl_interface_->Finish();
  // Make sure the vertex buffer is deleted.
  quad_drawing_data_.reset(NULL);
  CHECK_GL_ERROR(gl_interface_);
  gl_interface_->MakeGlxCurrent(0, 0);
  if (context_) {
    gl_interface_->DestroyGlxContext(context_);
  }
}

void OpenGlDrawVisitor::BindImage(const ImageContainer& container,
                                  RealCompositor::ImageActor* actor) {
  GLenum pixel_data_format = 0;
  switch (container.format()) {
    case IMAGE_FORMAT_RGBA_32:  // fallthrough
    case IMAGE_FORMAT_RGBX_32:
      pixel_data_format = GL_RGBA;
      break;
    case IMAGE_FORMAT_BGRA_32:  // fallthrough
    case IMAGE_FORMAT_BGRX_32:
      pixel_data_format = GL_BGRA;
      break;
    default:
      NOTREACHED() << "Unhandled image container data format "
                   << container.format();
  }

  // Create an OpenGL texture with the loaded image data.
  GLuint new_texture = 0;
  gl_interface_->Enable(GL_TEXTURE_2D);
  gl_interface_->GenTextures(1, &new_texture);
  gl_interface_->BindTexture(GL_TEXTURE_2D, new_texture);
  gl_interface_->TexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  gl_interface_->TexParameterf(GL_TEXTURE_2D,
                               GL_TEXTURE_MIN_FILTER,
                               GL_LINEAR);
  gl_interface_->TexParameterf(GL_TEXTURE_2D,
                               GL_TEXTURE_MAG_FILTER,
                               GL_LINEAR);
  gl_interface_->TexParameterf(GL_TEXTURE_2D,
                               GL_TEXTURE_WRAP_S,
                               GL_CLAMP_TO_EDGE);
  gl_interface_->TexParameterf(GL_TEXTURE_2D,
                               GL_TEXTURE_WRAP_T,
                               GL_CLAMP_TO_EDGE);
  gl_interface_->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                            container.width(), container.height(),
                            0, pixel_data_format, GL_UNSIGNED_BYTE,
                            container.data());
  CHECK_GL_ERROR(gl_interface_);
  scoped_ptr<OpenGlTextureData> data(new OpenGlTextureData(gl_interface_));
  data->SetTexture(new_texture);
  data->set_has_alpha(ImageFormatUsesAlpha(container.format()));
  actor->set_texture_data(data.release());
}

void OpenGlDrawVisitor::DrawNeedle() {
  PROFILER_MARKER_BEGIN(DrawNeedle);
  gl_interface_->BindBuffer(GL_ARRAY_BUFFER,
                            quad_drawing_data_->vertex_buffer());
  gl_interface_->EnableClientState(GL_VERTEX_ARRAY);
  gl_interface_->VertexPointer(2, GL_FLOAT, 0, 0);
  gl_interface_->DisableClientState(GL_TEXTURE_COORD_ARRAY);
  gl_interface_->DisableClientState(GL_COLOR_ARRAY);
  gl_interface_->Disable(GL_TEXTURE_2D);
  gl_interface_->PushMatrix();
  gl_interface_->Translatef(30, 30, 0);
  gl_interface_->Rotatef(num_frames_drawn_, 0.f, 0.f, 1.f);
  gl_interface_->Scalef(30, 3, 1.f);
  gl_interface_->Color4f(1.f, 0.f, 0.f, 0.8f);
  gl_interface_->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  gl_interface_->PopMatrix();
  PROFILER_MARKER_END(DrawNeedle);
}

void OpenGlDrawVisitor::VisitStage(RealCompositor::StageActor* actor) {
  if (!actor->IsVisible()) return;

  PROFILER_MARKER_BEGIN(VisitStage);
  stage_ = actor;

  if (actor->stage_color_changed()) {
    const Compositor::Color& color = actor->stage_color();
    gl_interface_->ClearColor(color.red, color.green, color.blue, 1.f);
    actor->unset_stage_color_changed();
  }

  if (actor->was_resized()) {
    gl_interface_->Viewport(0, 0, actor->width(), actor->height());
    actor->unset_was_resized();
  }

  state_cache_.Invalidate();

  if (gl_interface_->IsCapableOfPartialUpdates() && !damaged_region_.empty()) {
    gl_interface_->Enable(GL_SCISSOR_TEST);
    gl_interface_->Scissor(damaged_region_.x, damaged_region_.y,
                           damaged_region_.width, damaged_region_.height);
  }

  // No need to clear color buffer if something will cover up the screen.
  if (!has_fullscreen_actor_)
    gl_interface_->Clear(GL_COLOR_BUFFER_BIT);

  gl_interface_->MatrixMode(GL_PROJECTION);
  gl_interface_->LoadIdentity();
  // operator[] in Matrix4 returns by value in const version.
  Matrix4 projection = actor->projection();
  gl_interface_->LoadMatrixf(&projection[0][0]);
  gl_interface_->MatrixMode(GL_MODELVIEW);
  gl_interface_->LoadIdentity();
  gl_interface_->BindBuffer(GL_ARRAY_BUFFER,
                            quad_drawing_data_->vertex_buffer());
  gl_interface_->EnableClientState(GL_VERTEX_ARRAY);
  gl_interface_->VertexPointer(2, GL_FLOAT, 0, 0);
  gl_interface_->EnableClientState(GL_TEXTURE_COORD_ARRAY);
  gl_interface_->TexCoordPointer(2, GL_FLOAT, 0, 0);
  gl_interface_->EnableClientState(GL_COLOR_ARRAY);
  CHECK_GL_ERROR(gl_interface_);

  // Visiting back to front with no z-buffer.
  ancestor_opacity_ = actor->opacity();
  PROFILER_MARKER_BEGIN(Rendering_Pass);
  VisitContainer(actor);
  PROFILER_MARKER_END(Rendering_Pass);

  CHECK_GL_ERROR(gl_interface_);

  if (FLAGS_compositor_display_debug_needle)
    DrawNeedle();

  PROFILER_MARKER_BEGIN(Swap_Buffer);
  if (gl_interface_->IsCapableOfPartialUpdates() && !damaged_region_.empty()) {
    gl_interface_->Disable(GL_SCISSOR_TEST);
    gl_interface_->CopyGlxSubBuffer(actor->GetStageXWindow(),
                                    damaged_region_.x,
                                    damaged_region_.y,
                                    damaged_region_.width,
                                    damaged_region_.height);
#ifdef EXTRA_LOGGING
    DLOG(INFO) << "Partial updates: "
               << damaged_region_.x << ", "
               << damaged_region_.y << ", "
               << damaged_region_.width << ", "
               << damaged_region_.height << ".";
#endif
  } else {
    gl_interface_->SwapGlxBuffers(actor->GetStageXWindow());
#ifdef EXTRA_LOGGING
    DLOG(INFO) << "Full updates.";
#endif
  }
  PROFILER_MARKER_END(Swap_Buffer);
  ++num_frames_drawn_;
#ifdef EXTRA_LOGGING
  DLOG(INFO) << "Ending TRANSPARENT pass.";
#endif
  PROFILER_MARKER_END(VisitStage);
  // The profiler is flushed explicitly every 100 frames, or flushed
  // implicitly when the internal buffer is full.
  if (num_frames_drawn_ % 100 == 0)
    PROFILER_FLUSH();
  stage_ = NULL;
}

void OpenGlDrawVisitor::VisitContainer(RealCompositor::ContainerActor* actor) {
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
    if (!child->IsVisible())
      continue;
#ifdef EXTRA_LOGGING
    DLOG(INFO) << "Drawing child " << child->name()
               << " (visible: " << child->IsVisible()
               << ", opacity: " << child->opacity()
               << ", is_opaque: " << child->is_opaque() << ")";
#endif

    // TODO: move this down into the Visit* functions
    if (child->is_opaque() && child->opacity() * ancestor_opacity_ > 0.999)
      gl_interface_->Disable(GL_BLEND);
    else
      gl_interface_->Enable(GL_BLEND);
    child->Accept(this);
    CHECK_GL_ERROR(gl_interface_);
  }

  // Reset ancestor opacity.
  ancestor_opacity_ = original_opacity;
}

void OpenGlDrawVisitor::VisitImage(RealCompositor::ImageActor* actor) {
  if (!actor->IsVisible())
    return;

  PROFILER_MARKER_BEGIN(VisitImage);

  // All ImageActors are also QuadActors, and so we let the
  // QuadActor do all the actual drawing.
  VisitQuad(actor);
  PROFILER_MARKER_END(VisitImage);
}

void OpenGlDrawVisitor::VisitTexturePixmap(
    RealCompositor::TexturePixmapActor* actor) {
  if (!actor->IsVisible())
    return;

  PROFILER_MARKER_BEGIN(VisitTexturePixmap);

  // Make sure there's a bound texture.
  if (!actor->texture_data()) {
    if (!actor->pixmap()) {
      PROFILER_MARKER_END(VisitTexturePixmap);
      return;
    }

    scoped_ptr<OpenGlPixmapData> data(new OpenGlPixmapData(this));
    if (!data->Init(actor)) {
      PROFILER_MARKER_END(VisitTexturePixmap);
      return;
    }
    data->set_has_alpha(!actor->pixmap_is_opaque());
    actor->set_texture_data(data.release());
  }

  // All texture pixmaps are also QuadActors, and so we let the
  // QuadActor do all the actual drawing.
  VisitQuad(actor);
  PROFILER_MARKER_END(VisitTexturePixmap);
}

void OpenGlDrawVisitor::VisitQuad(RealCompositor::QuadActor* actor) {
  if (!actor->IsVisible())
    return;

#ifdef EXTRA_LOGGING
  DLOG(INFO) << "Drawing quad " << actor->name() << ".";
#endif
  PROFILER_DYNAMIC_MARKER_BEGIN(actor->name().c_str());

  // Calculate the vertex colors, taking into account the actor color,
  // opacity and the dimming gradient.
  float actor_opacity = actor->is_opaque() ? 1.0f :
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

  if (state_cache_.ColorStateChanged(actor_opacity,
                                     dimmed_transparency_begin,
                                     dimmed_transparency_end,
                                     red, green, blue)) {
    // Scale the vertex colors on the right by the transparency, since
    // we want it to fade to black as transparency of the dimming
    // overlay goes to zero. (note that the dimming is not *really* an
    // overlay -- it's just multiplied in here to simulate that).
    float dim_red_begin = red * dimmed_transparency_begin;
    float dim_green_begin = green * dimmed_transparency_begin;
    float dim_blue_begin = blue * dimmed_transparency_begin;
    float dim_red_end = red * dimmed_transparency_end;
    float dim_green_end = green * dimmed_transparency_end;
    float dim_blue_end = blue * dimmed_transparency_end;

    quad_drawing_data_->set_vertex_color(
        0, dim_red_begin, dim_green_begin, dim_blue_begin, actor_opacity);
    quad_drawing_data_->set_vertex_color(
        1, dim_red_begin, dim_green_begin, dim_blue_begin, actor_opacity);
    quad_drawing_data_->set_vertex_color(
        2, dim_red_end, dim_green_end, dim_blue_end, actor_opacity);
    quad_drawing_data_->set_vertex_color(
        3, dim_red_end, dim_green_end, dim_blue_end, actor_opacity);

    gl_interface_->EnableClientState(GL_COLOR_ARRAY);
    // Have to un-bind the array buffer to set the color pointer so that
    // it uses the color buffer instead of the vertex buffer memory.
    gl_interface_->BindBuffer(GL_ARRAY_BUFFER, 0);
    gl_interface_->ColorPointer(4, GL_FLOAT, 0,
                                quad_drawing_data_->color_buffer());
  }

  gl_interface_->BindBuffer(GL_ARRAY_BUFFER,
                            quad_drawing_data_->vertex_buffer());
  CHECK_GL_ERROR(gl_interface_);

  // Find out if this quad has pixmap or texture data to bind.
  if (actor->texture_data()) {
    // Actor has a texture to bind.
    gl_interface_->Enable(GL_TEXTURE_2D);
    gl_interface_->BindTexture(GL_TEXTURE_2D,
                               actor->texture_data()->texture());
  } else {
    // Actor has no texture.
    gl_interface_->Disable(GL_TEXTURE_2D);
  }

#ifdef EXTRA_LOGGING
  DLOG(INFO) << "  at: (" << actor->x() << ", "  << actor->y()
             << ", " << actor->z() << ") with scale: ("
             << actor->scale_x() << ", "  << actor->scale_y() << ") at size ("
             << actor->width() << "x"  << actor->height()
             << ") and opacity " << actor_opacity;
#endif

  gl_interface_->PushMatrix();
  // operator[] in Matrix4 returns by value in const version.
  Matrix4 model_view = actor->model_view();
  gl_interface_->LoadMatrixf(&model_view[0][0]);
  gl_interface_->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  gl_interface_->PopMatrix();
  CHECK_GL_ERROR(gl_interface_);
  PROFILER_DYNAMIC_MARKER_END();
}

}  // namespace window_manager
