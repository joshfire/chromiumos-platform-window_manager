// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/opengl_visitor.h"

#include <sys/time.h>

#include <algorithm>
#include <ctime>
#include <string>

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <gflags/gflags.h>
#include <xcb/damage.h>

#include "base/basictypes.h"
#include "base/logging.h"
#include "window_manager/gl_interface.h"
#include "window_manager/image_container.h"
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

namespace window_manager {

OpenGlQuadDrawingData::OpenGlQuadDrawingData(GLInterface* gl_interface)
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

OpenGlQuadDrawingData::~OpenGlQuadDrawingData() {
  if (vertex_buffer_)
    gl_interface_->DeleteBuffers(1, &vertex_buffer_);
}

void OpenGlQuadDrawingData::set_vertex_color(int index,
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

OpenGlPixmapData::OpenGlPixmapData(GLInterface* gl_interface,
                                   XConnection* x_conn)
    : gl_interface_(gl_interface),
      x_conn_(x_conn),
      texture_(0),
      pixmap_(XCB_NONE),
      glx_pixmap_(XCB_NONE),
      damage_(XCB_NONE),
      has_alpha_(false) {}

OpenGlPixmapData::~OpenGlPixmapData() {
  if (damage_) {
    x_conn_->DestroyDamage(damage_);
    damage_ = XCB_NONE;
  }
  if (texture_) {
    gl_interface_->DeleteTextures(1, &texture_);
    texture_ = 0;
  }
  if (glx_pixmap_) {
    gl_interface_->DestroyGlxPixmap(glx_pixmap_);
    glx_pixmap_ = XCB_NONE;
  }
  if (pixmap_) {
    x_conn_->FreePixmap(pixmap_);
    pixmap_ = XCB_NONE;
  }
}

void OpenGlPixmapData::Refresh() {
  LOG_IF(ERROR, !texture_) << "Refreshing with no texture.";
  if (!texture_)
    return;

  gl_interface_->BindTexture(GL_TEXTURE_2D, texture_);
  gl_interface_->ReleaseGlxTexImage(glx_pixmap_, GLX_FRONT_LEFT_EXT);
  gl_interface_->BindGlxTexImage(glx_pixmap_, GLX_FRONT_LEFT_EXT, NULL);
  if (damage_) {
    x_conn_->SubtractRegionFromDamage(damage_, XCB_NONE, XCB_NONE);
  }
  CHECK_GL_ERROR(gl_interface_);
}

void OpenGlPixmapData::SetTexture(GLuint texture, bool has_alpha) {
  if (texture_ && texture_ != texture) {
    gl_interface_->DeleteTextures(1, &texture_);
  }
  texture_ = texture;
  has_alpha_ = has_alpha;
  Refresh();
}

// static
bool OpenGlPixmapData::BindToPixmap(
    OpenGlDrawVisitor* visitor,
    RealCompositor::TexturePixmapActor* actor) {
  GLInterface* gl_interface = visitor->gl_interface_;
  XConnection* x_conn = visitor->x_conn_;

  CHECK(actor);
  if (!actor->texture_pixmap_window()) {
    // This just means that the window hasn't been mapped yet, so
    // we don't have a pixmap to bind to yet.
    return false;
  }

  // Clear out the existing drawing data.
  actor->EraseDrawingData(OpenGlDrawVisitor::PIXMAP_DATA);

  scoped_ptr<OpenGlPixmapData> data(new OpenGlPixmapData(gl_interface, x_conn));

  data->pixmap_ = x_conn->GetCompositingPixmapForWindow(
      actor->texture_pixmap_window());
  if (data->pixmap_ == XCB_NONE) {
    return false;
  }

  XConnection::WindowGeometry geometry;
  x_conn->GetWindowGeometry(data->pixmap_, &geometry);
  bool is_rgba = (geometry.depth == 32);
  int attribs[] = {
    GLX_TEXTURE_FORMAT_EXT,
    is_rgba ? GLX_TEXTURE_FORMAT_RGBA_EXT : GLX_TEXTURE_FORMAT_RGB_EXT,
    GLX_TEXTURE_TARGET_EXT,
    GLX_TEXTURE_2D_EXT,
    0
  };
  data->has_alpha_ = is_rgba;
  data->glx_pixmap_ = gl_interface->CreateGlxPixmap(
      is_rgba ?
        visitor->framebuffer_config_rgba_ :
        visitor->framebuffer_config_rgb_,
      data->pixmap_,
      attribs);
  CHECK_GL_ERROR(gl_interface);
  if (data->glx_pixmap_ == XCB_NONE) {
    // TODO: Figure out what causes this.  Perhaps the window was destroyed
    // by the time that we tried to use its pixmap.
    LOG(WARNING) << "Failed to create GLX pixmap for window "
                 << XidStr(actor->texture_pixmap_window()) << " using pixmap "
                 << XidStr(data->pixmap_);
    return false;
  }

  gl_interface->GenTextures(1, &data->texture_);
  gl_interface->BindTexture(GL_TEXTURE_2D, data->texture_);
  gl_interface->TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl_interface->TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl_interface->TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                              GL_CLAMP_TO_EDGE);
  gl_interface->TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                              GL_CLAMP_TO_EDGE);
  gl_interface->BindGlxTexImage(data->glx_pixmap_, GLX_FRONT_LEFT_EXT, NULL);
  CHECK_GL_ERROR(gl_interface);
  data->damage_ = x_conn->CreateDamage(actor->texture_pixmap_window(),
                                       XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
  if (data->damage_ == XCB_NONE) {
    LOG(WARNING) << "Failed to create damage object for window "
                 << XidStr(actor->texture_pixmap_window());
    return false;
  }

#ifdef EXTRA_LOGGING
  LOG(INFO) << "Adding pixmap data that is "
            << (data->has_alpha_ ? "transparent" : "opaque")
            << " (" << geometry.depth << "-bit) to " << actor->name();
#endif
  // Set this in case we are adding the pixmap data as part of the pass.
  actor->set_is_opaque(actor->opacity() > 0.999f && !data->has_alpha_);
  actor->SetDrawingData(OpenGlDrawVisitor::PIXMAP_DATA,
                        RealCompositor::DrawingDataPtr(data.release()));
  actor->set_pixmap_invalid(false);
  actor->SetDirty();
  return true;
}

OpenGlTextureData::OpenGlTextureData(GLInterface* gl_interface)
    : gl_interface_(gl_interface),
      texture_(0),
      has_alpha_(false) {}

OpenGlTextureData::~OpenGlTextureData() {
  if (texture_) {
    gl_interface_->DeleteTextures(1, &texture_);
  }
}

void OpenGlTextureData::SetTexture(GLuint texture, bool has_alpha) {
  if (texture_ && texture_ != texture) {
    gl_interface_->DeleteTextures(1, &texture_);
  }
  texture_ = texture;
  has_alpha_ = has_alpha;
}

OpenGlDrawVisitor::OpenGlDrawVisitor(GLInterface* gl_interface,
                                     RealCompositor* compositor,
                                     Compositor::StageActor* stage)
    : gl_interface_(gl_interface),
      compositor_(compositor),
      x_conn_(compositor_->x_conn()),
      stage_(NULL),
      framebuffer_config_rgb_(0),
      framebuffer_config_rgba_(0),
      context_(0),
      visit_opaque_(false),
      ancestor_opacity_(1.0f),
      num_frames_drawn_(0) {
  CHECK(gl_interface_);
  context_ = gl_interface_->CreateGlxContext();
  CHECK(context_) << "Unable to create a context from the available visuals.";

  gl_interface_->MakeGlxCurrent(stage->GetStageXWindow(), context_);

  FindFramebufferConfigurations();

  gl_interface_->Enable(GL_DEPTH_TEST);
  gl_interface_->BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  CHECK_GL_ERROR(gl_interface_);

  quad_drawing_data_.reset(new OpenGlQuadDrawingData(gl_interface_));
}

void OpenGlDrawVisitor::FindFramebufferConfigurations() {
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
}

OpenGlDrawVisitor::~OpenGlDrawVisitor() {
  gl_interface_->Finish();
  // Make sure the vertex buffer is deleted.
  quad_drawing_data_ = RealCompositor::DrawingDataPtr();
  CHECK_GL_ERROR(gl_interface_);
  gl_interface_->MakeGlxCurrent(0, 0);
  if (context_) {
    gl_interface_->DestroyGlxContext(context_);
  }
}

void OpenGlDrawVisitor::BindImage(const ImageContainer* container,
                                  RealCompositor::QuadActor* actor) {
  // Create an OpenGL texture with the loaded image data.
  GLuint new_texture;
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
                            container->width(), container->height(),
                            0, GL_RGBA, GL_UNSIGNED_BYTE,
                            container->data());
  CHECK_GL_ERROR(gl_interface_);
  OpenGlTextureData* data = new OpenGlTextureData(gl_interface_);
  // TODO: once ImageContainer supports non-alpha images, calculate
  // whether or not this texture has alpha (instead of just passing
  // 'true').
  data->SetTexture(new_texture, true);
  actor->SetSize(container->width(), container->height());
  actor->SetDrawingData(OpenGlDrawVisitor::TEXTURE_DATA,
                        RealCompositor::DrawingDataPtr(data));
  DLOG(INFO) << "Binding image " << container->filename()
             << " to texture " << new_texture;
}

void OpenGlDrawVisitor::VisitActor(RealCompositor::Actor* actor) {
  // Base actors actually don't have anything to draw.
}

void OpenGlDrawVisitor::VisitTexturePixmap(
    RealCompositor::TexturePixmapActor* actor) {
  if (!actor->IsVisible()) return;
  // Make sure there's a bound texture.
  if (!actor->GetDrawingData(PIXMAP_DATA).get() ||
      actor->is_pixmap_invalid()) {
    if (!OpenGlPixmapData::BindToPixmap(this, actor)) {
      // We didn't find a bound pixmap, so let's just skip drawing this
      // actor.  (it's probably because it hasn't been mapped).
      return;
    }
  }

  // All texture pixmaps are also QuadActors, and so we let the
  // QuadActor do all the actual drawing.
  VisitQuad(actor);
}

void OpenGlDrawVisitor::VisitQuad(RealCompositor::QuadActor* actor) {
  if (!actor->IsVisible()) return;
#ifdef EXTRA_LOGGING
  DLOG(INFO) << "Drawing quad " << actor->name() << ".";
#endif

  RealCompositor::DrawingData* generic_drawing_data =
      actor->GetDrawingData(DRAWING_DATA).get();
  if (!generic_drawing_data) {
    // This actor hasn't been here before, so let's set the drawing data on it.
    actor->SetDrawingData(DRAWING_DATA, quad_drawing_data_);
    generic_drawing_data = quad_drawing_data_.get();
  }
  OpenGlQuadDrawingData* drawing_data =
      dynamic_cast<OpenGlQuadDrawingData*>(generic_drawing_data);
  CHECK(drawing_data);

  // Calculate the vertex colors, taking into account the actor color,
  // opacity and the dimming gradient.
  float actor_opacity = actor->is_opaque() ? 1.0f :
                        actor->opacity() * ancestor_opacity_;
  float dimmed_transparency = 1.f - actor->dimmed_opacity();
  float red = actor->color().red;
  float green = actor->color().green;
  float blue = actor->color().blue;
  DCHECK(actor_opacity <= 1.f);
  DCHECK(actor_opacity >= 0.f);
  DCHECK(dimmed_transparency <= 1.f);
  DCHECK(dimmed_transparency >= 0.f);
  DCHECK(red <= 1.f);
  DCHECK(red >= 0.f);
  DCHECK(green <= 1.f);
  DCHECK(green >= 0.f);
  DCHECK(blue <= 1.f);
  DCHECK(blue >= 0.f);

  // Scale the vertex colors on the right by the transparency, since
  // we want it to fade to black as transparency of the dimming
  // overlay goes to zero. (note that the dimming is not *really* an
  // overlay -- it's just multiplied in here to simulate that).
  float dim_red = red * dimmed_transparency;
  float dim_green = green * dimmed_transparency;
  float dim_blue = blue * dimmed_transparency;

  drawing_data->set_vertex_color(
      0, red, green, blue, actor_opacity);
  drawing_data->set_vertex_color(
      1, red, green, blue, actor_opacity);
  drawing_data->set_vertex_color(
      2, dim_red, dim_green, dim_blue, actor_opacity);
  drawing_data->set_vertex_color(
      3, dim_red, dim_green, dim_blue, actor_opacity);

  gl_interface_->EnableClientState(GL_COLOR_ARRAY);
  // Have to un-bind the array buffer to set the color pointer so that
  // it uses the color buffer instead of the vertex buffer memory.
  gl_interface_->BindBuffer(GL_ARRAY_BUFFER, 0);
  gl_interface_->ColorPointer(4, GL_FLOAT, 0, drawing_data->color_buffer());
  gl_interface_->BindBuffer(GL_ARRAY_BUFFER, drawing_data->vertex_buffer());
  CHECK_GL_ERROR(gl_interface_);

  // Find out if this quad has pixmap or texture data to bind.
  RealCompositor::DrawingData* generic_pixmap_data =
      actor->GetDrawingData(PIXMAP_DATA).get();
  OpenGlPixmapData* pixmap_data =
      dynamic_cast<OpenGlPixmapData*>(generic_pixmap_data);
  if (generic_pixmap_data)
    CHECK(pixmap_data);
  if (pixmap_data && pixmap_data->texture()) {
    // Actor has a pixmap texture to bind.
    gl_interface_->Enable(GL_TEXTURE_2D);
    gl_interface_->BindTexture(GL_TEXTURE_2D, pixmap_data->texture());
  } else {
    RealCompositor::DrawingData* generic_texture_data =
        actor->GetDrawingData(TEXTURE_DATA).get();
    OpenGlTextureData* texture_data =
        dynamic_cast<OpenGlTextureData*>(generic_texture_data);
    if (generic_texture_data)
      CHECK(texture_data);
    if (texture_data && texture_data->texture()) {
      // Actor has a texture to bind.
      gl_interface_->Enable(GL_TEXTURE_2D);
      gl_interface_->BindTexture(GL_TEXTURE_2D, texture_data->texture());
    } else {
      // Actor has no texture.
      gl_interface_->Disable(GL_TEXTURE_2D);
    }
  }
  gl_interface_->PushMatrix();
  gl_interface_->Translatef(actor->x(), actor->y(), actor->z());
  gl_interface_->Scalef(actor->width() * actor->scale_x(),
                        actor->height() * actor->scale_y(), 1.f);
#ifdef EXTRA_LOGGING
  DLOG(INFO) << "  at: (" << actor->x() << ", "  << actor->y()
             << ", " << actor->z() << ") with scale: ("
             << actor->scale_x() << ", "  << actor->scale_y() << ") at size ("
             << actor->width() << "x"  << actor->height()
             << ") and opacity " << actor_opacity;
#endif

  if (actor->tilt() > 0.001f) {
    // Post-multiply a perspective matrix onto the model view matrix,
    // and a rotation in Y so that all the other model view ops happen
    // outside of the perspective transform.

    // This matrix is the result of a translate by 0.5 in Y, followed
    // by a simple perspective transform, followed by a translate in
    // -0.5 in Y, so that the perspective foreshortening is centered
    // vertically on the quad.
    static float tilt_matrix [] = {
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, -0.2f, 0.0f, -0.4f,
      0.0f, 0.0f, 0.0f, 1.0f
    };
    gl_interface_->MultMatrixf(tilt_matrix);
    gl_interface_->Rotatef(90.0f * actor->tilt(),
                           0.f, 1.f, 0.f);
  }

  gl_interface_->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  gl_interface_->DisableClientState(GL_COLOR_ARRAY);
  gl_interface_->PopMatrix();
  CHECK_GL_ERROR(gl_interface_);
}

void OpenGlDrawVisitor::DrawNeedle() {
  OpenGlQuadDrawingData* drawing_data =
      dynamic_cast<OpenGlQuadDrawingData*>(quad_drawing_data_.get());
  CHECK(drawing_data);
  gl_interface_->BindBuffer(GL_ARRAY_BUFFER, drawing_data->vertex_buffer());
  gl_interface_->EnableClientState(GL_VERTEX_ARRAY);
  gl_interface_->VertexPointer(2, GL_FLOAT, 0, 0);
  gl_interface_->DisableClientState(GL_TEXTURE_COORD_ARRAY);
  gl_interface_->DisableClientState(GL_COLOR_ARRAY);
  gl_interface_->Disable(GL_TEXTURE_2D);
  gl_interface_->PushMatrix();
  gl_interface_->Disable(GL_DEPTH_TEST);
  gl_interface_->Translatef(30, 30, 0);
  gl_interface_->Rotatef(num_frames_drawn_, 0.f, 0.f, 1.f);
  gl_interface_->Scalef(30, 3, 1.f);
  gl_interface_->Color4f(1.f, 0.f, 0.f, 0.8f);
  gl_interface_->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  gl_interface_->Enable(GL_DEPTH_TEST);
  gl_interface_->PopMatrix();
}

void OpenGlDrawVisitor::VisitStage(RealCompositor::StageActor* actor) {
  if (!actor->IsVisible()) return;

  stage_ = actor;
  OpenGlQuadDrawingData* drawing_data =
      dynamic_cast<OpenGlQuadDrawingData*>(quad_drawing_data_.get());
  CHECK(drawing_data);

  if (actor->was_resized()) {
    gl_interface_->Viewport(0, 0, actor->width(), actor->height());
    actor->unset_was_resized();
  }

  gl_interface_->MatrixMode(GL_PROJECTION);
  gl_interface_->LoadIdentity();
  gl_interface_->Ortho(0, actor->width(), actor->height(), 0,
                       -RealCompositor::LayerVisitor::kMinDepth,
                       -RealCompositor::LayerVisitor::kMaxDepth);
  gl_interface_->MatrixMode(GL_MODELVIEW);
  gl_interface_->LoadIdentity();
  gl_interface_->Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  gl_interface_->BindBuffer(GL_ARRAY_BUFFER, drawing_data->vertex_buffer());
  gl_interface_->EnableClientState(GL_VERTEX_ARRAY);
  gl_interface_->VertexPointer(2, GL_FLOAT, 0, 0);
  gl_interface_->EnableClientState(GL_TEXTURE_COORD_ARRAY);
  gl_interface_->TexCoordPointer(2, GL_FLOAT, 0, 0);
  gl_interface_->DepthMask(GL_TRUE);
  CHECK_GL_ERROR(gl_interface_);

  // Set the z-depths for the actors, update is_opaque.
  RealCompositor::LayerVisitor layer_visitor(compositor_->actor_count());
  actor->Accept(&layer_visitor);

#ifdef EXTRA_LOGGING
  DLOG(INFO) << "Starting OPAQUE pass.";
#endif
  // Disable blending because these actors are all opaque, and we're
  // drawing them front to back.
  gl_interface_->Disable(GL_BLEND);

  // For the first pass, we want to collect only opaque actors, in
  // front to back order.
  visit_opaque_ = true;
  VisitContainer(actor);

#ifdef EXTRA_LOGGING
  DLOG(INFO) << "Ending OPAQUE pass.";
  DLOG(INFO) << "Starting TRANSPARENT pass.";
#endif
  // Visiting back to front now, with no z-buffer, but with blending.
  ancestor_opacity_ = actor->opacity();
  gl_interface_->DepthMask(GL_FALSE);
  gl_interface_->Enable(GL_BLEND);
  visit_opaque_ = false;
  VisitContainer(actor);

  // Turn the depth mask back on now.
  gl_interface_->DepthMask(GL_TRUE);
  CHECK_GL_ERROR(gl_interface_);

  if (FLAGS_compositor_display_debug_needle) {
    DrawNeedle();
  }
  gl_interface_->SwapGlxBuffers(actor->GetStageXWindow());
  ++num_frames_drawn_;
#ifdef EXTRA_LOGGING
  DLOG(INFO) << "Ending TRANSPARENT pass.";
#endif
  stage_ = NULL;
}

void OpenGlDrawVisitor::VisitContainer(RealCompositor::ContainerActor* actor) {
  if (!actor->IsVisible()) {
    return;
  }

  if (actor != stage_) {
    gl_interface_->PushMatrix();
    // Don't translate by Z because the actors already have their
    // absolute Z values from the layer calculation.
    gl_interface_->Translatef(actor->x(), actor->y(), 0.0f);
    gl_interface_->Scalef(actor->width() * actor->scale_x(),
                          actor->height() * actor->scale_y(), 1.f);
  }

#ifdef EXTRA_LOGGING
  DLOG(INFO) << "Drawing container " << actor->name() << ".";
  DLOG(INFO) << "  at: (" << actor->x() << ", "  << actor->y()
             << ", " << actor->z() << ") with scale: ("
             << actor->scale_x() << ", "  << actor->scale_y() << ") at size ("
             << actor->width() << "x"  << actor->height() << ")";
#endif
  RealCompositor::ActorVector children = actor->GetChildren();
  if (visit_opaque_) {
    for (RealCompositor::ActorVector::const_iterator iterator =
           children.begin(); iterator != children.end(); ++iterator) {
      RealCompositor::Actor* child = *iterator;
      // Only traverse if the child is visible, and opaque.
      if (child->IsVisible() && child->is_opaque()) {
#ifdef EXTRA_LOGGING
        DLOG(INFO) << "Drawing opaque child " << child->name()
                   << " (visible: " << child->IsVisible()
                   << ", opacity: " << child->opacity()
                   << ", is_opaque: " << child->is_opaque() << ")";
#endif
        child->Accept(this);
      } else {
#ifdef EXTRA_LOGGING
        DLOG(INFO) << "NOT drawing transparent child " << child->name()
                   << " (visible: " << child->IsVisible()
                   << ", opacity: " << child->opacity()
                   << ", is_opaque: " << child->is_opaque() << ")";
#endif
      }
      CHECK_GL_ERROR(gl_interface_);
    }
  } else {
    float original_opacity = ancestor_opacity_;
    ancestor_opacity_ *= actor->opacity();

    // Walk backwards so we go back to front.
    RealCompositor::ActorVector::const_reverse_iterator iterator;
    for (iterator = children.rbegin(); iterator != children.rend();
         ++iterator) {
      RealCompositor::Actor* child = *iterator;
      // Only traverse if child is visible, and either transparent or
      // has children that might be transparent.
      if (child->IsVisible() &&
          (ancestor_opacity_ <= 0.999 || child->has_children() ||
           !child->is_opaque())) {
#ifdef EXTRA_LOGGING
        DLOG(INFO) << "Drawing transparent child " << child->name()
                   << " (visible: " << child->IsVisible()
                   << ", has_children: " << child->has_children()
                   << ", opacity: " << child->opacity()
                   << ", ancestor_opacity: " << ancestor_opacity_
                   << ", is_opaque: " << child->is_opaque() << ")";
#endif
        child->Accept(this);
      } else {
#ifdef EXTRA_LOGGING
        DLOG(INFO) << "NOT drawing opaque child " << child->name()
                   << " (visible: " << child->IsVisible()
                   << ", has_children: " << child->has_children()
                   << ", opacity: " << child->opacity()
                   << ", ancestor_opacity: " << ancestor_opacity_
                   << ", is_opaque: " << child->is_opaque() << ")";
#endif
      }
      CHECK_GL_ERROR(gl_interface_);
    }

    // Reset ancestor opacity.
    ancestor_opacity_ = original_opacity;
  }

  if (actor != stage_) {
    gl_interface_->PopMatrix();
  }
}

}  // namespace window_manager
