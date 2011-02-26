// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_COMPOSITOR_GLES_SHADER_BASE_H_
#define WINDOW_MANAGER_COMPOSITOR_GLES_SHADER_BASE_H_

#include <GLES2/gl2.h>

#include "base/basictypes.h"

namespace window_manager {

class Shader {
 public:
  ~Shader();

  // Call gl(Enable|Disable)VertexAttribArray to enable only the arrays needed
  // for this shader.  The currently enabled vertex attribs are tracked and
  // only minimal calls are made.
  void EnableVertexAttribs();

  // If gl(Enabe|Disable)VertexAttribArray is called from outside of
  // EnableVertexAttribs(), disable all verex attribs and call this function.
  static void ResetActiveVertexAttribs();

  int program() const { return program_; }

 protected:
  Shader(const char* vertex_shader, const char* fragment_shader);

  void SetUsedVertexAttribs(unsigned int used_vertex_attribs) {
    used_vertex_attribs_ = used_vertex_attribs;
  }

 private:
  static unsigned int active_vertex_attribs_;

  GLint program_;
  unsigned int used_vertex_attribs_;

  void AttachShader(const char* source, GLenum type);

  DISALLOW_COPY_AND_ASSIGN(Shader);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_COMPOSITOR_GLES_SHADER_BASE_H_

