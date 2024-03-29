// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor/gl/mock_gl_interface.h"

struct __GLinterface;
struct __GLcontextModes;
struct __GLXscreenInfo;
struct __GLXpixmap;
struct __GLXdrawablePrivate;
typedef Screen* ScreenPtr;
struct VisualRec;

// These structures are taken from the GLX source code.
struct __GLXcontextRec {
  struct __GLXcontextRec* last;
  struct __GLXcontextRec* next;
  struct __GLXcontextRec* nextDrawPriv;
  struct __GLXcontextRec* nextReadPriv;
  __GLinterface* gc;
  __GLcontextModes* modes;
  ScreenPtr pScreen;
  __GLXscreenInfo* pGlxScreen;
  VisualRec* pVisual;
  XID id;
  XID share_id;
  VisualID vid;
  GLint screen;
  GLboolean idExists;
  GLboolean isCurrent;
  GLboolean isDirect;
  GLuint pendingState;
  GLboolean hasUnflushedCommands;
  GLenum renderMode;
  GLfloat* feedbackBuf;
  GLint feedbackBufSize;
  GLuint* selectBuf;
  GLint selectBufSize;
  __GLXpixmap* drawPixmap;
  __GLXpixmap* readPixmap;
  __GLXdrawablePrivate* drawPriv;
  __GLXdrawablePrivate* readPriv;
};

struct __GLXFBConfigRec {
  int visualType;
  int transparentType;
  int transparentRed, transparentGreen, transparentBlue, transparentAlpha;
  int transparentIndex;
  int visualCaveat;
  int associatedVisualId;
  int screen;
  int drawableType;
  int renderType;
  int maxPbufferWidth, maxPbufferHeight, maxPbufferPixels;
  int optimalPbufferWidth, optimalPbufferHeight;
  int visualSelectGroup;
  unsigned int id;
  GLboolean rgbMode;
  GLboolean colorIndexMode;
  GLboolean doubleBufferMode;
  GLboolean stereoMode;
  GLboolean haveAccumBuffer;
  GLboolean haveDepthBuffer;
  GLboolean haveStencilBuffer;
  GLint accumRedBits, accumGreenBits, accumBlueBits, accumAlphaBits;
  GLint depthBits;
  GLint stencilBits;
  GLint indexBits;
  GLint redBits, greenBits, blueBits, alphaBits;
  GLuint redMask, greenMask, blueMask, alphaMask;
  GLuint multiSampleSize;
  GLuint nMultiSampleBuffers;
  GLint maxAuxBuffers;
  GLint level;
  GLboolean extendedRange;
  GLdouble minRed, maxRed;
  GLdouble minGreen, maxGreen;
  GLdouble minBlue, maxBlue;
  GLdouble minAlpha, maxAlpha;
};

static __GLXcontextRec kContextRec = {0};
static __GLXFBConfigRec kConfigRec32;
static __GLXFBConfigRec kConfigRec24;

namespace window_manager {

MockGLInterface::MockGLInterface()
    : mock_context_(&kContextRec),
      clear_red_(0.f),
      clear_green_(0.f),
      clear_blue_(0.f),
      clear_alpha_(0.f),
      next_glx_pixmap_id_(1),
      full_updates_count_(0),
      partial_updates_count_(0),
      partial_updates_region_() {
  mock_configs_.reset(new GLXFBConfig[2]);
  kConfigRec24.depthBits = 24;
  kConfigRec24.redBits = 8;
  kConfigRec24.greenBits = 8;
  kConfigRec24.blueBits = 8;
  kConfigRec24.alphaBits = 0;
  mock_configs_[0] = &kConfigRec24;

  kConfigRec32.depthBits = 32;
  kConfigRec32.redBits = 8;
  kConfigRec32.greenBits = 8;
  kConfigRec32.blueBits = 8;
  kConfigRec32.alphaBits = 8;
  mock_configs_[1] = &kConfigRec32;

  mock_visual_info_.depth = kConfigRec32.depthBits;
}

GLXPixmap MockGLInterface::CreateGlxPixmap(GLXFBConfig config,
                                           XPixmap pixmap,
                                           const int* attrib_list) {
  return next_glx_pixmap_id_++;
}

GLXContext MockGLInterface::CreateGlxContext() {
  return mock_context_;
}

Bool MockGLInterface::MakeGlxCurrent(GLXDrawable drawable,
                                     GLXContext ctx) {
  return True;
}

GLXFBConfig* MockGLInterface::GetGlxFbConfigs(int* nelements) {
  *nelements = 2;
  return mock_configs_.get();
}

XVisualInfo* MockGLInterface::GetGlxVisualFromFbConfig(GLXFBConfig config) {
  mock_visual_info_.depth = config->depthBits;
  return &mock_visual_info_;
}

int MockGLInterface::GetGlxFbConfigAttrib(GLXFBConfig config,
                                          int attribute,
                                          int* value) {
  switch (attribute) {
    case GLX_ALPHA_SIZE:
      *value = config->alphaBits;
      break;
    case GLX_BUFFER_SIZE:
      *value = config->redBits + config->greenBits +
               config->blueBits + config->alphaBits;
      break;
    case GLX_BIND_TO_TEXTURE_RGBA_EXT:
      *value = config->depthBits == 32;
      break;
    case GLX_BIND_TO_TEXTURE_RGB_EXT:
      *value = config->depthBits == 24;
      break;
    case GLX_X_VISUAL_TYPE:
      *value = GLX_TRUE_COLOR;
      break;
    default:
      *value = 0;
      break;
  }
  return Success;
}

void MockGLInterface::Viewport(GLint x, GLint y,
                               GLsizei width, GLsizei height) {
  viewport_.reset(x, y, width, height);
}
}  // namespace window_manager
