// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor/gl/real_gl_interface.h"

#include <string>

#include "base/logging.h"
#include "window_manager/x11/real_x_connection.h"

using std::string;

namespace window_manager {

static string kGlxExtensions;
static string kGlExtensions;
static PFNGLXBINDTEXIMAGEEXTPROC _gl_bind_tex_image = NULL;
static PFNGLXRELEASETEXIMAGEEXTPROC _gl_release_tex_image = NULL;
static PFNGLXCREATEPIXMAPPROC _gl_create_pixmap = NULL;
static PFNGLXDESTROYPIXMAPPROC _gl_destroy_pixmap = NULL;
static PFNGLXCOPYSUBBUFFERMESAPROC _gl_copy_sub_buffer = NULL;

// True if GL supports the anisotropic sampling extension.
static bool supports_anisotropy = false;

// The maximum anisotropy sampling value.  The value 1.0 means no
// anisotropic sampling is performed.  See the following URL for more
// information:
// http://www.opengl.org/registry/specs/EXT/texture_filter_anisotropic.txt
static float max_anisotropy = 1.0f;

RealGLInterface::RealGLInterface(RealXConnection* connection)
    : xconn_(connection),
      has_texture_from_pixmap_extension_(false) {
  Display* display = xconn_->GetDisplay();
  if (kGlxExtensions.size() == 0) {
    kGlxExtensions = string(glXQueryExtensionsString(
        display, DefaultScreen(display)));
    LOG(INFO) << "Supported GLX extensions: " << kGlxExtensions;
  }
  if (kGlxExtensions.find("GLX_EXT_texture_from_pixmap") != string::npos) {
    if (_gl_bind_tex_image == NULL) {
      _gl_bind_tex_image = reinterpret_cast<PFNGLXBINDTEXIMAGEEXTPROC>(
          glXGetProcAddress(
              reinterpret_cast<const GLubyte*>("glXBindTexImageEXT")));
    }
    CHECK(_gl_bind_tex_image)
        << "Unable to find proc address for glXBindTexImageEXT";

    if (_gl_release_tex_image == NULL) {
      _gl_release_tex_image = reinterpret_cast<PFNGLXRELEASETEXIMAGEEXTPROC>(
          glXGetProcAddress(
              reinterpret_cast<const GLubyte*>("glXReleaseTexImageEXT")));
    }
    CHECK(_gl_release_tex_image)
        << "Unable to find proc address for glXReleaseTexImageEXT";
    has_texture_from_pixmap_extension_ = true;
  }
  if (kGlxExtensions.find("GLX_SGIX_fbconfig") != string::npos) {
    if (_gl_create_pixmap == NULL) {
      _gl_create_pixmap = reinterpret_cast<PFNGLXCREATEPIXMAPPROC>(
          glXGetProcAddress(
              reinterpret_cast<const GLubyte*>("glXCreatePixmap")));
    }
    CHECK(_gl_create_pixmap)
        << "Unable to find proc address for glXCreatePixmap";

    if (_gl_destroy_pixmap == NULL) {
      _gl_destroy_pixmap = reinterpret_cast<PFNGLXDESTROYPIXMAPPROC>(
          glXGetProcAddress(
              reinterpret_cast<const GLubyte*>("glXDestroyPixmap")));
    }
    CHECK(_gl_destroy_pixmap)
        << "Unable to find proc address for glXDestroyPixmap";
  } else {
    CHECK(false) << "FBConfig not supported on this device.";
  }
  if (kGlxExtensions.find("GLX_MESA_copy_sub_buffer") != string::npos) {
    if (_gl_copy_sub_buffer == NULL) {
      _gl_copy_sub_buffer = reinterpret_cast<PFNGLXCOPYSUBBUFFERMESAPROC>(
          glXGetProcAddress(
              reinterpret_cast<const GLubyte*>("glXCopySubBufferMESA")));
    }
    if (_gl_copy_sub_buffer == NULL) {
      LOG(INFO) << "glXCopySubBufferMESA is un-available: "
                << "unable to find proc address.";
    } else {
      LOG(INFO) << "glXCopySubBufferMESA is available.";
    }
  } else {
      LOG(INFO) << "glXCopySubBufferMESA is un-available: "
                << "not supported on this device.";
  }
  GLint attributes[] = {
    GLX_RGBA,
    GLX_DOUBLEBUFFER,
    GLX_RED_SIZE, 8,
    GLX_GREEN_SIZE, 8,
    GLX_BLUE_SIZE, 8,
    None
  };
  visual_info_ = glXChooseVisual(display, DefaultScreen(display), attributes);
  CHECK(visual_info_) << "Did not find a suitable GL visual";
  LOG(INFO) << "Chose visual " << visual_info_->visualid;
}

RealGLInterface::~RealGLInterface() {
  XFree(visual_info_);
}

void RealGLInterface::GlxFree(void* item) {
  XFree(item);
}

XVisualID RealGLInterface::GetVisual() {
  return visual_info_->visualid;
}

GLXPixmap RealGLInterface::CreateGlxPixmap(GLXFBConfig config,
                                           XPixmap pixmap,
                                           const int* attrib_list) {
  xconn_->TrapErrors();
  GLXPixmap result = _gl_create_pixmap(xconn_->GetDisplay(), config,
                                       pixmap, attrib_list);
  if (int error = xconn_->UntrapErrors()) {
    LOG(WARNING) << "Got X error while creating a GL pixmap: "
                 << xconn_->GetErrorText(error);
    return None;
  }
  return result;
}

void RealGLInterface::DestroyGlxPixmap(GLXPixmap pixmap) {
  xconn_->TrapErrors();
  _gl_destroy_pixmap(xconn_->GetDisplay(), pixmap);
  if (int error = xconn_->UntrapErrors()) {
    LOG(WARNING) << "Got X error while destroying a GL pixmap: "
                 << xconn_->GetErrorText(error);
  }
}

GLXContext RealGLInterface::CreateGlxContext() {
  xconn_->TrapErrors();
  GLXContext context = glXCreateContext(xconn_->GetDisplay(), visual_info_,
                                        NULL, True);
  if (int error = xconn_->UntrapErrors()) {
    LOG(WARNING) << "Got X error while creating a GL context: "
                 << xconn_->GetErrorText(error);
  }
  return context;
}

void RealGLInterface::DestroyGlxContext(GLXContext context) {
  xconn_->TrapErrors();
  glXDestroyContext(xconn_->GetDisplay(), context);
  if (int error = xconn_->UntrapErrors()) {
    LOG(WARNING) << "Got X error while destroying a GL context: "
                 << xconn_->GetErrorText(error);
  }
}

Bool RealGLInterface::IsGlxDirect(GLXContext context) {
  return glXIsDirect(xconn_->GetDisplay(), context);
}

void RealGLInterface::SwapGlxBuffers(GLXDrawable drawable) {
  xconn_->TrapErrors();
  glXSwapBuffers(xconn_->GetDisplay(), drawable);
  if (int error = xconn_->UntrapErrors()) {
    LOG(WARNING) << "Got X error while swapping buffers: "
                 << xconn_->GetErrorText(error);
  }
}

Bool RealGLInterface::MakeGlxCurrent(GLXDrawable drawable,
                                     GLXContext ctx) {
  xconn_->TrapErrors();
  Bool current = glXMakeCurrent(xconn_->GetDisplay(), drawable, ctx);
  if (int error = xconn_->UntrapErrors()) {
    LOG(WARNING) << "Got X error while making a GL context current: "
                 << xconn_->GetErrorText(error);
    return False;
  }
  // Now that we've got a current context, check for extensions, but
  // only once.
  if (kGlExtensions.size() == 0) {
    const char* extensions =
        reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (extensions) {
      kGlExtensions = string(extensions);
      LOG(INFO) << "Supported GL extensions: " << kGlExtensions;
      supports_anisotropy =
          kGlExtensions.find("GL_EXT_texture_filter_anisotropic") !=
          string::npos;
      if (supports_anisotropy)
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_anisotropy);
    }
  }
  return current;
}

GLXFBConfig* RealGLInterface::GetGlxFbConfigs(int* nelements) {
  xconn_->TrapErrors();
  GLXFBConfig* result = glXGetFBConfigs(xconn_->GetDisplay(),
                                        DefaultScreen(xconn_->GetDisplay()),
                                        nelements);
  if (int error = xconn_->UntrapErrors()) {
    LOG(WARNING) << "Got X error while getting GLX framebuffer configs: "
                 << xconn_->GetErrorText(error);
    return NULL;
  }
  return result;
}

XVisualInfo* RealGLInterface::GetGlxVisualFromFbConfig(GLXFBConfig config) {
  xconn_->TrapErrors();
  XVisualInfo* result = glXGetVisualFromFBConfig(xconn_->GetDisplay(), config);
  if (int error = xconn_->UntrapErrors()) {
    LOG(WARNING) << "Got X error while getting GLX visual from "
                 << "framebuffer config: "
                 << xconn_->GetErrorText(error);
    return NULL;
  }
  return result;
}

int RealGLInterface::GetGlxFbConfigAttrib(GLXFBConfig config,
                                          int attribute,
                                          int* value) {
  xconn_->TrapErrors();
  int result = glXGetFBConfigAttrib(xconn_->GetDisplay(),
                                    config, attribute, value);
  if (int error = xconn_->UntrapErrors()) {
    LOG(WARNING) << "Got X error while getting GLX framebuffer attribute: "
                 << xconn_->GetErrorText(error);
    return None;
  }
  return result;
}

void RealGLInterface::BindGlxTexImage(GLXDrawable drawable,
                                      int buffer,
                                      int* attrib_list) {
  xconn_->TrapErrors();
  CHECK(_gl_bind_tex_image != NULL);
  _gl_bind_tex_image(xconn_->GetDisplay(),
                     drawable, buffer, attrib_list);
  if (int error = xconn_->UntrapErrors()) {
    LOG(WARNING) << "Got X error while binding GLX texture image: "
                 << xconn_->GetErrorText(error);
  }
}

void RealGLInterface::ReleaseGlxTexImage(GLXDrawable drawable,
                                         int buffer) {
  xconn_->TrapErrors();
  CHECK(_gl_release_tex_image != NULL);
  _gl_release_tex_image(xconn_->GetDisplay(), drawable, buffer);
  if (int error = xconn_->UntrapErrors()) {
    LOG(WARNING) << "Got X error while releasing GLX texture image: "
                 << xconn_->GetErrorText(error);
  }
}

bool RealGLInterface::IsCapableOfPartialUpdates() {
  return (_gl_copy_sub_buffer != NULL);
}

void RealGLInterface::CopyGlxSubBuffer(GLXDrawable drawable,
                                       int x,
                                       int y,
                                       int width,
                                       int height) {
  xconn_->TrapErrors();
  CHECK(_gl_copy_sub_buffer != NULL);
  _gl_copy_sub_buffer(xconn_->GetDisplay(), drawable, x, y, width, height);
  if (int error = xconn_->UntrapErrors()) {
    LOG(WARNING) << "Got X error while copying GLX sub buffer: "
                 << xconn_->GetErrorText(error);
  }
}

// GL Functions.

void RealGLInterface::Viewport(
    GLint x, GLint y, GLsizei width, GLsizei height) {
  glViewport(x, y, width, height);
}

void RealGLInterface::BindBuffer(GLenum target, GLuint buffer) {
  glBindBuffer(target, buffer);
}

void RealGLInterface::BindTexture(GLenum target, GLuint texture) {
  glBindTexture(target, texture);
}

void RealGLInterface::BlendFunc(GLenum sfactor, GLenum dfactor) {
  glBlendFunc(sfactor, dfactor);
}

void RealGLInterface::BufferData(GLenum target, GLsizeiptr size,
                                 const GLvoid* data, GLenum usage) {
  glBufferData(target, size, data, usage);
}

void RealGLInterface::Clear(GLbitfield mask) {
  glClear(mask);
}

void RealGLInterface::ClearColor(GLfloat red, GLfloat green, GLfloat blue,
                                 GLfloat alpha) {
  glClearColor(red, green, blue, alpha);
}

void RealGLInterface::Color4f(GLfloat red, GLfloat green, GLfloat blue,
                              GLfloat alpha) {
  glColor4f(red, green, blue, alpha);
}

void RealGLInterface::DeleteBuffers(GLsizei n, const GLuint* buffers) {
  glDeleteBuffers(n, buffers);
}

void RealGLInterface::DeleteTextures(GLsizei n, const GLuint* textures) {
  glDeleteTextures(n, textures);
}

void RealGLInterface::DepthMask(GLboolean flag) {
  glDepthMask(flag);
}

void RealGLInterface::Disable(GLenum cap) {
  glDisable(cap);
}

void RealGLInterface::DisableClientState(GLenum array) {
  glDisableClientState(array);
}

void RealGLInterface::DrawArrays(GLenum mode, GLint first, GLsizei count) {
  glDrawArrays(mode, first, count);
}

void RealGLInterface::Enable(GLenum cap) {
  glEnable(cap);
}

void RealGLInterface::EnableClientState(GLenum cap) {
  glEnableClientState(cap);
}

void RealGLInterface::Finish() {
  glFinish();
}

void RealGLInterface::GenBuffers(GLsizei n, GLuint* buffers) {
  glGenBuffers(n, buffers);
}

void RealGLInterface::GenTextures(GLsizei n, GLuint* textures) {
  glGenTextures(n, textures);
}

GLenum RealGLInterface::GetError() {
  return glGetError();
}

void RealGLInterface::LoadIdentity() {
  glLoadIdentity();
}

void RealGLInterface::LoadMatrixf(const GLfloat* m) {
  glLoadMatrixf(m);
}

void RealGLInterface::MultMatrixf(GLfloat* matrix) {
  glMultMatrixf(matrix);
}

void RealGLInterface::MatrixMode(GLenum mode) {
  glMatrixMode(mode);
}

void RealGLInterface::Ortho(GLdouble left, GLdouble right, GLdouble bottom,
                            GLdouble top, GLdouble near, GLdouble far) {
  glOrtho(left, right, bottom, top, near, far);
}

void RealGLInterface::PushMatrix() {
  glPushMatrix();
}

void RealGLInterface::PopMatrix() {
  glPopMatrix();
}

void RealGLInterface::Rotatef(GLfloat angle, GLfloat x,
                              GLfloat y, GLfloat z) {
  glRotatef(angle, x, y, z);
}

void RealGLInterface::Scalef(GLfloat x, GLfloat y, GLfloat z) {
  glScalef(x, y, z);
}

void RealGLInterface::Scissor(GLint x, GLint y, GLint width, GLint height) {
  glScissor(x, y, width, height);
}

void RealGLInterface::TexCoordPointer(GLint size, GLenum type,
                                      GLsizei stride, const GLvoid* pointer) {
  glTexCoordPointer(size, type, stride, pointer);
}

void RealGLInterface::TexParameteri(GLenum target, GLenum pname, GLint param) {
  glTexParameteri(target, pname, param);
}

void RealGLInterface::TexParameterf(GLenum target, GLenum pname,
                                    GLfloat param) {
  glTexParameterf(target, pname, param);
}

void RealGLInterface::TexEnvf(GLenum target, GLenum pname, GLfloat param) {
  glTexEnvf(target, pname, param);
}

void RealGLInterface::TexImage2D(GLenum target,
                                 GLint level,
                                 GLint internalFormat,
                                 GLsizei width,
                                 GLsizei height,
                                 GLint border,
                                 GLenum format,
                                 GLenum type,
                                 const GLvoid* pixels) {
  glTexImage2D(target, level, internalFormat, width, height,
               border, format, type, pixels);
}

void RealGLInterface::EnableAnisotropicFiltering() {
  if (supports_anisotropy)
    TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, max_anisotropy);
}


void RealGLInterface::Translatef(GLfloat x, GLfloat y, GLfloat z) {
  glTranslatef(x, y, z);
}

void RealGLInterface::VertexPointer(GLint size, GLenum type,
                                    GLsizei stride, const GLvoid* pointer) {
  glVertexPointer(size, type, stride, pointer);
}

void RealGLInterface::ColorPointer(GLint size, GLenum type,
                                   GLsizei stride, const GLvoid* pointer) {
  glColorPointer(size, type, stride, pointer);
}

}  // namespace window_manager
