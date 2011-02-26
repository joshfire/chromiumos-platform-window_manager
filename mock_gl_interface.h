// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_MOCK_GL_INTERFACE_H_
#define WINDOW_MANAGER_MOCK_GL_INTERFACE_H_

#include "window_manager/compositor/gl/gl_interface.h"
#include "window_manager/geometry.h"

namespace window_manager {

// This wraps a mock interface for GLX.

class MockGLInterface : public GLInterface {
 public:
  MockGLInterface();
  virtual ~MockGLInterface() {}

  // Begin GLInterface methods.
  virtual XVisualID GetVisual() { return 1; }
  virtual void GlxFree(void* item) {}

  virtual GLXPixmap CreateGlxPixmap(GLXFBConfig config,
                                    XPixmap pixmap,
                                    const int* attrib_list);
  virtual void DestroyGlxPixmap(GLXPixmap pixmap) {}
  virtual GLXContext CreateGlxContext();
  virtual void DestroyGlxContext(GLXContext context) {}
  virtual Bool IsGlxDirect(GLXContext context) { return true; }
  virtual void SwapGlxBuffers(GLXDrawable drawable) { ++full_updates_count_; }
  virtual Bool MakeGlxCurrent(GLXDrawable drawable,
                              GLXContext ctx);
  virtual GLXFBConfig* GetGlxFbConfigs(int* nelements);
  virtual XVisualInfo* GetGlxVisualFromFbConfig(GLXFBConfig config);
  virtual int GetGlxFbConfigAttrib(GLXFBConfig config,
                           int attribute,
                           int* value);
  virtual void BindGlxTexImage(GLXDrawable drawable,
                               int buffer,
                               int* attrib_list) {}
  virtual void ReleaseGlxTexImage(GLXDrawable drawable, int buffer) {}
  virtual bool IsCapableOfPartialUpdates() { return true; }
  virtual void CopyGlxSubBuffer(GLXDrawable drawable,
                                int x,
                                int y,
                                int width,
                                int height) {
    ++partial_updates_count_;
    partial_updates_region_.reset(x, y, width, height);
  }

  // GL functions we use.
  virtual void Viewport(GLint x, GLint y, GLsizei width, GLsizei height);
  virtual void BindBuffer(GLenum target, GLuint buffer) {}
  virtual void BindTexture(GLenum target, GLuint texture) {}
  virtual void BlendFunc(GLenum sfactor, GLenum dfactor) {}
  virtual void BufferData(GLenum target, GLsizeiptr size, const GLvoid* data,
                          GLenum usage) {}
  virtual void Clear(GLbitfield mask) {}
  virtual void ClearColor(GLfloat red, GLfloat green, GLfloat blue,
                          GLfloat alpha) {
    clear_red_ = red;
    clear_green_ = green;
    clear_blue_ = blue;
    clear_alpha_ = alpha;
  }
  virtual void Color4f(GLfloat red, GLfloat green, GLfloat blue,
                       GLfloat alpha) {}
  virtual void DeleteBuffers(GLsizei n, const GLuint* buffers) {}
  virtual void DeleteTextures(GLsizei n, const GLuint* textures) {}
  virtual void DepthMask(GLboolean flag) {}
  virtual void Disable(GLenum cap) {}
  virtual void DisableClientState(GLenum array) {}
  virtual void DrawArrays(GLenum mode, GLint first, GLsizei count) {}
  virtual void Enable(GLenum cap) {}
  virtual void EnableClientState(GLenum cap) {}
  virtual void Finish() {}
  virtual void GenBuffers(GLsizei n, GLuint* buffers) {}
  virtual void GenTextures(GLsizei n, GLuint* textures) {}
  virtual GLenum GetError() { return GL_NO_ERROR; }
  virtual void LoadIdentity() {}
  virtual void LoadMatrixf(const GLfloat* m) {}
  virtual void MultMatrixf(GLfloat* matrix) {}
  virtual void MatrixMode(GLenum mode) {}
  virtual void Ortho(GLdouble left, GLdouble right, GLdouble bottom,
                     GLdouble top, GLdouble near, GLdouble far) {}
  virtual void PushMatrix() {}
  virtual void PopMatrix() {}
  virtual void Rotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {}
  virtual void Scalef(GLfloat x, GLfloat y, GLfloat z) {}
  virtual void Scissor(GLint x, GLint y, GLint width, GLint height) {}
  virtual void TexCoordPointer(GLint size, GLenum type, GLsizei stride,
                               const GLvoid* pointer) {}
  virtual void TexParameteri(GLenum target, GLenum pname, GLint param) {}
  virtual void TexParameterf(GLenum target, GLenum pname, GLfloat param) {}
  virtual void TexEnvf(GLenum target, GLenum pname, GLfloat param) {}
  virtual void TexImage2D(GLenum target,
                          GLint level,
                          GLint internalFormat,
                          GLsizei width,
                          GLsizei height,
                          GLint border,
                          GLenum format,
                          GLenum type,
                          const GLvoid* pixels) {}
  virtual void EnableAnisotropicFiltering() {}
  virtual void Translatef(GLfloat x, GLfloat y, GLfloat z) {}
  virtual void VertexPointer(GLint size, GLenum type, GLsizei stride,
                             const GLvoid* pointer) {}
  virtual void ColorPointer(GLint size, GLenum type, GLsizei stride,
                            const GLvoid* pointer) {}
  // End GLInterface methods.

  // Begin test-only methods.
  Rect viewport() const { return viewport_; }
  GLfloat clear_red() const { return clear_red_; }
  GLfloat clear_green() const { return clear_green_; }
  GLfloat clear_blue() const { return clear_blue_; }
  GLfloat clear_alpha() const { return clear_alpha_; }
  int full_updates_count() const { return full_updates_count_; }
  int partial_updates_count() const { return partial_updates_count_; }
  const Rect& partial_updates_region() const { return partial_updates_region_; }
  // End test-only methods.

 private:
  XVisualInfo mock_visual_info_;
  scoped_array<GLXFBConfig> mock_configs_;
  GLXContext mock_context_;

  // Most recent dimensions set using Viewport().
  Rect viewport_;

  // Most recent color set using ClearColor().
  GLfloat clear_red_, clear_green_, clear_blue_, clear_alpha_;

  // Next ID to hand out in CreateGlxPixmap().
  GLXPixmap next_glx_pixmap_id_;

  // The number of times SwapGlxBuffers() is called.
  int full_updates_count_;

  // The number of times CopyGlxSubBuffer() is called.
  int partial_updates_count_;

  // Most recent CopyGlxSubBuffer() region.
  Rect partial_updates_region_;

  DISALLOW_COPY_AND_ASSIGN(MockGLInterface);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_MOCK_GL_INTERFACE_H_
