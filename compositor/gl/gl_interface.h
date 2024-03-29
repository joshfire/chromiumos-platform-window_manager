// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_COMPOSITOR_GL_GL_INTERFACE_H_
#define WINDOW_MANAGER_COMPOSITOR_GL_GL_INTERFACE_H_

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>

#include "window_manager/compositor/gl_interface_base.h"
#include "window_manager/x11/x_types.h"

namespace window_manager {

// This is an abstract base class representing a GL interface.
class GLInterface : virtual public GLInterfaceBase {
 public:
  GLInterface() {}
  virtual ~GLInterface() {}

  // Is GLX_EXT_texture_from_pixmap available?
  virtual bool HasTextureFromPixmapExtension() { return true; }

  // Use this function to free objects obtained from this interface,
  // such as from GetGlxFbConfigs and GetGlxVisualFromFbConfig.  In
  // other words, call this when you would have called "XFree" on an
  // object returned from GLX.
  virtual void GlxFree(void* item) = 0;

  // Gets a visual ID that is compatible with GL.
  virtual XVisualID GetVisual() = 0;

  // GLX functions that we use.
  // These functions all correspond to similar glX* functions (without the Glx).
  virtual GLXPixmap CreateGlxPixmap(GLXFBConfig config,
                                    XPixmap pixmap,
                                    const int* attrib_list) = 0;
  virtual void DestroyGlxPixmap(GLXPixmap pixmap) = 0;
  virtual GLXContext CreateGlxContext() = 0;
  virtual void DestroyGlxContext(GLXContext context) = 0;
  virtual Bool IsGlxDirect(GLXContext context) = 0;
  virtual void SwapGlxBuffers(GLXDrawable drawable) = 0;
  virtual Bool MakeGlxCurrent(GLXDrawable drawable,
                              GLXContext ctx) = 0;
  virtual void CopyGlxSubBuffer(GLXDrawable drawable,
                                int x,
                                int y,
                                int width,
                                int height) = 0;

  // The caller assumes ownership of objects obtained from
  // GetGlxFbConfigs and GetGlxVisualFromFbConfig and must call the
  // GlxFree function above to free them.
  virtual GLXFBConfig* GetGlxFbConfigs(int* nelements) = 0;
  virtual XVisualInfo* GetGlxVisualFromFbConfig(GLXFBConfig config) = 0;

  virtual int GetGlxFbConfigAttrib(GLXFBConfig config,
                                   int attribute,
                                   int* value) = 0;
  virtual void BindGlxTexImage(GLXDrawable drawable,
                               int buffer,
                               int* attrib_list) = 0;
  virtual void ReleaseGlxTexImage(GLXDrawable drawable,
                                  int buffer) = 0;

  // GL Functions that we use.
  virtual void Viewport(GLint x, GLint y, GLsizei width, GLsizei height) = 0;
  virtual void BindBuffer(GLenum target, GLuint buffer) = 0;
  virtual void BindTexture(GLenum target, GLuint texture) = 0;
  virtual void BlendFunc(GLenum sfactor, GLenum dfactor) = 0;
  virtual void BufferData(GLenum target, GLsizeiptr size, const GLvoid* data,
                          GLenum usage) = 0;
  virtual void Clear(GLbitfield mask) = 0;
  virtual void ClearColor(GLfloat red, GLfloat green, GLfloat blue,
                          GLfloat alpha) = 0;
  virtual void Color4f(GLfloat red, GLfloat green, GLfloat blue,
                       GLfloat alpha) = 0;
  virtual void DeleteBuffers(GLsizei n, const GLuint* buffers) = 0;
  virtual void DeleteTextures(GLsizei n, const GLuint* textures) = 0;
  virtual void DepthMask(GLboolean flag) = 0;
  virtual void Disable(GLenum cap) = 0;
  virtual void DisableClientState(GLenum array) = 0;
  virtual void DrawArrays(GLenum mode, GLint first, GLsizei count) = 0;
  virtual void Enable(GLenum cap) = 0;
  virtual void EnableClientState(GLenum cap) = 0;
  virtual void Finish() = 0;
  virtual void GenBuffers(GLsizei n, GLuint* buffers) = 0;
  virtual void GenTextures(GLsizei n, GLuint* textures) = 0;
  virtual GLenum GetError() = 0;
  virtual void LoadIdentity() = 0;
  virtual void LoadMatrixf(const GLfloat* m) = 0;
  virtual void MatrixMode(GLenum mode) = 0;
  virtual void MultMatrixf(GLfloat* matrix) = 0;
  virtual void Ortho(GLdouble left, GLdouble right, GLdouble bottom,
                     GLdouble top, GLdouble near, GLdouble far) = 0;
  virtual void PushMatrix() = 0;
  virtual void PopMatrix() = 0;
  virtual void Rotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) = 0;
  virtual void Scalef(GLfloat x, GLfloat y, GLfloat z) = 0;
  virtual void Scissor(GLint x, GLint y, GLint width, GLint height) = 0;
  virtual void TexCoordPointer(GLint size, GLenum type, GLsizei stride,
                               const GLvoid* pointer) = 0;
  virtual void TexParameteri(GLenum target, GLenum pname, GLint param) = 0;
  virtual void TexParameterf(GLenum target, GLenum pname, GLfloat param) = 0;
  virtual void TexEnvf(GLenum target, GLenum pname, GLfloat param) = 0;
  virtual void TexImage2D(GLenum target,
                          GLint level,
                          GLint internalFormat,
                          GLsizei width,
                          GLsizei height,
                          GLint border,
                          GLenum format,
                          GLenum type,
                          const GLvoid* pixels) = 0;
  virtual void EnableAnisotropicFiltering() = 0;
  virtual void Translatef(GLfloat x, GLfloat y, GLfloat z) = 0;
  virtual void VertexPointer(GLint size, GLenum type, GLsizei stride,
                             const GLvoid* pointer) = 0;
  virtual void ColorPointer(GLint size, GLenum type, GLsizei stride,
                             const GLvoid* pointer) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(GLInterface);
};

};  // namespace window_manager

#endif  //  WINDOW_MANAGER_COMPOSITOR_GL_GL_INTERFACE_H_
