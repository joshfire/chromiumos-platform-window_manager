// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_MOCK_GL_INTERFACE_H_
#define WINDOW_MANAGER_MOCK_GL_INTERFACE_H_

#include "window_manager/geometry.h"
#include "window_manager/gl_interface.h"

namespace window_manager {

// This wraps a mock interface for GLX.

class MockGLInterface : public GLInterface {
 public:
  MockGLInterface();
  virtual ~MockGLInterface() {}

  void GlxFree(void* item) {}

  // These functions with "Glx" in the name all correspond to similar
  // glX* functions (without the Glx).
  GLXPixmap CreateGlxPixmap(GLXFBConfig config,
                            XPixmap pixmap,
                            const int* attrib_list);
  void DestroyGlxPixmap(GLXPixmap pixmap) {}
  GLXContext CreateGlxContext();
  void DestroyGlxContext(GLXContext context) {}
  void SwapGlxBuffers(GLXDrawable drawable) {}
  Bool MakeGlxCurrent(GLXDrawable drawable,
                      GLXContext ctx);
  GLXFBConfig* GetGlxFbConfigs(int* nelements);
  XVisualInfo* GetGlxVisualFromFbConfig(GLXFBConfig config);
  int GetGlxFbConfigAttrib(GLXFBConfig config,
                           int attribute,
                           int* value);
  void BindGlxTexImage(GLXDrawable drawable,
                       int buffer,
                       int* attrib_list) {}
  void ReleaseGlxTexImage(GLXDrawable drawable,
                          int buffer) {}

  // GL functions we use.
  void Viewport(GLint x, GLint y, GLsizei width, GLsizei height);
  void BindBuffer(GLenum target, GLuint buffer) {}
  void BindTexture(GLenum target, GLuint texture) {}
  void BlendFunc(GLenum sfactor, GLenum dfactor) {}
  void BufferData(GLenum target, GLsizeiptr size, const GLvoid* data,
                  GLenum usage) {}
  void Clear(GLbitfield mask) {}
  void ClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    clear_red_ = red;
    clear_green_ = green;
    clear_blue_ = blue;
    clear_alpha_ = alpha;
  }
  void Color4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {}
  void DeleteBuffers(GLsizei n, const GLuint* buffers) {}
  void DeleteTextures(GLsizei n, const GLuint* textures) {}
  void DepthMask(GLboolean flag) {}
  void Disable(GLenum cap) {}
  void DisableClientState(GLenum array) {}
  void DrawArrays(GLenum mode, GLint first, GLsizei count) {}
  void Enable(GLenum cap) {}
  void EnableClientState(GLenum cap) {}
  void Finish() {}
  void GenBuffers(GLsizei n, GLuint* buffers) {}
  void GenTextures(GLsizei n, GLuint* textures) {}
  GLenum GetError() { return GL_NO_ERROR; }
  void LoadIdentity() {}
  void MultMatrixf(GLfloat* matrix) {}
  void MatrixMode(GLenum mode) {}
  void Ortho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble near, GLdouble far) {}
  void PushMatrix() {}
  void PopMatrix() {}
  void Rotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {}
  void Scalef(GLfloat x, GLfloat y, GLfloat z) {}
  void TexCoordPointer(GLint size, GLenum type, GLsizei stride,
                       const GLvoid* pointer) {}
  void TexParameteri(GLenum target, GLenum pname, GLint param) {}
  void TexParameterf(GLenum target, GLenum pname, GLfloat param) {}
  void TexEnvf(GLenum target, GLenum pname, GLfloat param) {}
  void TexImage2D(GLenum target,
                  GLint level,
                  GLint internalFormat,
                  GLsizei width,
                  GLsizei height,
                  GLint border,
                  GLenum format,
                  GLenum type,
                  const GLvoid* pixels) {}
  void EnableAnisotropicFiltering() {}
  void Translatef(GLfloat x, GLfloat y, GLfloat z) {}
  void VertexPointer(GLint size, GLenum type, GLsizei stride,
                     const GLvoid* pointer) {}
  void ColorPointer(GLint size, GLenum type, GLsizei stride,
                    const GLvoid* pointer) {}

  // Begin test-only methods.
  Rect viewport() const { return viewport_; }
  GLfloat clear_red() const { return clear_red_; }
  GLfloat clear_green() const { return clear_green_; }
  GLfloat clear_blue() const { return clear_blue_; }
  GLfloat clear_alpha() const { return clear_alpha_; }
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

  DISALLOW_COPY_AND_ASSIGN(MockGLInterface);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_MOCK_GL_INTERFACE_H_
