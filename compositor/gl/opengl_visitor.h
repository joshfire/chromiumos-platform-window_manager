// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_COMPOSITOR_GL_OPENGL_VISITOR_H_
#define WINDOW_MANAGER_COMPOSITOR_GL_OPENGL_VISITOR_H_

#include <GL/glx.h>

#include <vector>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/compositor/gl/gl_interface.h"
#include "window_manager/compositor/real_compositor.h"
#include "window_manager/compositor/texture_data.h"
#include "window_manager/x11/x_connection.h"

namespace window_manager {

class ImageContainer;

class OpenGlTextureData : public TextureData {
 public:
  explicit OpenGlTextureData(GLInterface* gl_interface);
  virtual ~OpenGlTextureData();

  void SetTexture(GLuint texture);

 private:
  // This is the GL interface to use for communicating with GL.
  GLInterface* gl_interface_;
};

class OpenGlPixmapData : public TextureData {
 public:
  OpenGlPixmapData(OpenGlDrawVisitor* visitor);
  virtual ~OpenGlPixmapData();

  // Begin TextureData methods.
  virtual void Refresh();
  // End TextureData methods.

  // Initialize our texture and make it contain the current contents of the
  // passed-in actor's pixmap.  False is returned if the process fails (in
  // which case this object should be thrown away).
  bool Init(RealCompositor::TexturePixmapActor* actor);

 private:
  // Fetch the contents of |pixmap_| from the X server and copy them to our
  // texture.  This is the slower implementation used when the
  // texture-from-pixmap extension is unavailable.  Returns true on success.
  bool CopyPixmapImageToTexture();

  OpenGlDrawVisitor* visitor_;  // not owned
  GLInterface* gl_;             // not owned

  // The actor's X pixmap.  Ownership of the pixmap remains with the caller.
  XPixmap pixmap_;

  // GLX pixmap created from the actor's X pixmap if the
  // texture-from-pixmap extension is available.
  GLXPixmap glx_pixmap_;

  // Dimensions and depth of |pixmap_|.  This is only initialized if
  // |glx_pixmap_| isn't being used.
  XConnection::WindowGeometry pixmap_geometry_;
};

// This class visits an actor tree and draws it using OpenGL.
class OpenGlDrawVisitor : virtual public RealCompositor::ActorVisitor {
 public:
  OpenGlDrawVisitor(GLInterface* gl_interface,
                    RealCompositor* compositor,
                    Compositor::StageActor* stage);
  virtual ~OpenGlDrawVisitor();

  XConnection* xconn() { return xconn_; }
  void set_has_fullscreen_actor(bool has_fullscreen_actor) {
    has_fullscreen_actor_ = has_fullscreen_actor;
  }
  void set_damaged_region(const Rect& damaged_region) {
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
  class OpenGlQuadDrawingData {
   public:
    explicit OpenGlQuadDrawingData(GLInterface* gl_interface);
    virtual ~OpenGlQuadDrawingData();

    GLuint vertex_buffer() { return vertex_buffer_; }
    float* color_buffer() { return color_buffer_.get(); }

    // Sets the vertex color of the give vertex index.
    // Has no effect on drawing until the next call to SetBufferData.
    void set_vertex_color(int index, float r, float g, float b, float a);

   private:
    // This is the gl interface to use for communicating with GL.
    GLInterface* gl_interface_;

    // This is the vertex buffer that holds the rect we use for
    // rendering quads.
    GLuint vertex_buffer_;

    scoped_array<float> color_buffer_;
  };

  class OpenGlStateCache {
   public:
    OpenGlStateCache();
    void Invalidate();
    bool ColorStateChanged(float actor_opacity,
                           float dimmed_transparency_begin,
                           float dimmed_transparency_end,
                           float red, float green, float blue);
   private:
    float actor_opacity_;
    float dimmed_transparency_begin_;
    float dimmed_transparency_end_;
    float red_, green_, blue_;
  };

  // So it can get access to the config data.
  friend class OpenGlPixmapData;

  // This draws a debugging "needle" in the upper left corner.
  void DrawNeedle();

  // Finds an appropriate framebuffer configurations for the current
  // display.  Sets framebuffer_config_rgba_ and framebuffer_config_rgb_.
  void FindFramebufferConfigurations();

  // The visitor should not change settings in the compositor while visiting
  // actors throughout the drawing process because the compositor may decide
  // to skip drawing frames as an optimization.
  RealCompositor* compositor_;  // Not owned.
  GLInterface* gl_interface_;  // Not owned.
  XConnection* xconn_;  // Not owned.
  RealCompositor::StageActor* stage_; // Not owned.

  // This holds the drawing data used for quads.  Note that only
  // QuadActors use this drawing data, and they all share the same
  // one (to keep from allocating a lot of quad vertex buffers).
  scoped_ptr<OpenGlQuadDrawingData> quad_drawing_data_;

  // The framebuffer configs to use with this display.
  GLXFBConfig framebuffer_config_rgb_;
  GLXFBConfig framebuffer_config_rgba_;
  GLXContext context_;

  // This is the cumulative opacity of all the ancestors of the
  // currently visited node. It is recalculated each time we enter or
  // leave a container node.
  float ancestor_opacity_;

  // This keeps track of the number of frames drawn so we can draw the
  // debugging needle.
  int num_frames_drawn_;

  // Use this struct to store OpenGl states from the previous quad. The stored
  // states can be used to compare against states of the current quad, and we
  // only need to reset OpenGl states that are changed to avoid unnecessary GL
  // state changes.
  OpenGlStateCache state_cache_;

  // The rectangular region of the screen that is damaged in the frame.
  // This information allows the draw visitor to perform partial updates.
  Rect damaged_region_;

  // Used to track whether the current projection matrix is a pass-through
  // matrix.  Pass-through means the output of the model view transform will
  // map directly to window coordinates, e.g, if the model view transform
  // yields the point [4, 4], a vertex will be placed at pixel [4, 4] in the
  // window.
  bool using_passthrough_projection_;

  // This is used to indicate whether the entire screen will be covered by an
  // actor so we can optimize by not clearing the COLOR_BUFFER_BIT.
  bool has_fullscreen_actor_;

  DISALLOW_COPY_AND_ASSIGN(OpenGlDrawVisitor);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_COMPOSITOR_GL_OPENGL_VISITOR_H_
