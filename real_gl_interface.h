// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_REAL_GL_INTERFACE_H_
#define WINDOW_MANAGER_REAL_GL_INTERFACE_H_

#include "window_manager/gl_interface.h"

namespace window_manager {

class RealXConnection;

// This wraps an actual GL interface so that we can mock it and use it
// for testing.
class RealGLInterface : public GLInterface {
 public:
  explicit RealGLInterface(RealXConnection* connection);
  virtual ~RealGLInterface() {}

  // Begin GLInterface methods.
  virtual void GlxFree(void* item);

  virtual GLXPixmap CreateGlxPixmap(GLXFBConfig config,
                                    XPixmap pixmap,
                                    const int* attrib_list);
  virtual void DestroyGlxPixmap(GLXPixmap pixmap);
  virtual GLXContext CreateGlxContext();
  virtual void DestroyGlxContext(GLXContext context);
  virtual Bool IsGlxDirect(GLXContext context);
  virtual void SwapGlxBuffers(GLXDrawable drawable);
  virtual Bool MakeGlxCurrent(GLXDrawable drawable, GLXContext ctx);
  virtual GLXFBConfig* GetGlxFbConfigs(int* nelements);
  virtual XVisualInfo* GetGlxVisualFromFbConfig(GLXFBConfig config);
  virtual int GetGlxFbConfigAttrib(GLXFBConfig config,
                                   int attribute,
                                   int* value);
  virtual void BindGlxTexImage(GLXDrawable drawable,
                               int buffer,
                               int* attrib_list);
  virtual void ReleaseGlxTexImage(GLXDrawable drawable, int buffer);

  // GL Functions
  virtual void Viewport(GLint x, GLint y, GLsizei width, GLsizei height);
  virtual void BindBuffer(GLenum target, GLuint buffer);
  virtual void BindTexture(GLenum target, GLuint texture);
  virtual void BlendFunc(GLenum sfactor, GLenum dfactor);
  virtual void BufferData(GLenum target, GLsizeiptr size, const GLvoid* data,
                          GLenum usage);
  virtual void Clear(GLbitfield mask);
  virtual void ClearColor(GLfloat red, GLfloat green, GLfloat blue,
                          GLfloat alpha);
  virtual void Color4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
  virtual void DeleteBuffers(GLsizei n, const GLuint* buffers);
  virtual void DeleteTextures(GLsizei n, const GLuint* textures);
  virtual void DepthMask(GLboolean flag);
  virtual void Disable(GLenum cap);
  virtual void DisableClientState(GLenum array);
  virtual void DrawArrays(GLenum mode, GLint first, GLsizei count);
  virtual void Enable(GLenum cap);
  virtual void EnableClientState(GLenum cap);
  virtual void Finish();
  virtual void GenBuffers(GLsizei n, GLuint* buffers);
  virtual void GenTextures(GLsizei n, GLuint* textures);
  virtual GLenum GetError();
  virtual void LoadIdentity();
  virtual void LoadMatrixf(const GLfloat* m);
  virtual void MultMatrixf(GLfloat* matrix);
  virtual void MatrixMode(GLenum mode);
  virtual void Ortho(GLdouble left, GLdouble right, GLdouble bottom,
                     GLdouble top, GLdouble near, GLdouble far);
  virtual void PushMatrix();
  virtual void PopMatrix();
  virtual void Rotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
  virtual void Scalef(GLfloat x, GLfloat y, GLfloat z);
  virtual void TexCoordPointer(GLint size, GLenum type, GLsizei stride,
                               const GLvoid* pointer);
  virtual void TexParameteri(GLenum target, GLenum pname, GLint param);
  virtual void TexParameterf(GLenum target, GLenum pname, GLfloat param);
  virtual void TexEnvf(GLenum target, GLenum pname, GLfloat param);
  virtual void TexImage2D(GLenum target,
                          GLint level,
                          GLint internalFormat,
                          GLsizei width,
                          GLsizei height,
                          GLint border,
                          GLenum format,
                          GLenum type,
                          const GLvoid* pixels);
  virtual void EnableAnisotropicFiltering();
  virtual void Translatef(GLfloat x, GLfloat y, GLfloat z);
  virtual void VertexPointer(GLint size, GLenum type, GLsizei stride,
                             const GLvoid* pointer);
  virtual void ColorPointer(GLint size, GLenum type, GLsizei stride,
                            const GLvoid* pointer);
  // End GLInterface methods.

 private:
  RealXConnection* xconn_;
  DISALLOW_COPY_AND_ASSIGN(RealGLInterface);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_REAL_GL_INTERFACE_H_
