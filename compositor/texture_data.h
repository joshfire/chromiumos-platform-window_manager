// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_COMPOSITOR_TEXTURE_DATA_H_
#define WINDOW_MANAGER_COMPOSITOR_TEXTURE_DATA_H_

#include <stdint.h>

namespace window_manager {

class TextureData {
 public:
  virtual ~TextureData() {}
  uint32_t texture() const { return texture_; }

  bool has_alpha() const { return has_alpha_; }
  void set_has_alpha(bool has_alpha) { has_alpha_ = has_alpha; }
  virtual void Refresh() {}

 protected:
  // TextureData is not allowed to be instantiated.
  TextureData() : texture_(0), has_alpha_(true) {}
  void set_texture(uint32_t texture) { texture_ = texture; }
  const uint32_t* texture_ptr() { return &texture_; }

 private:
  uint32_t texture_;
  bool has_alpha_;
  DISALLOW_COPY_AND_ASSIGN(TextureData);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_COMPOSITOR_TEXTURE_DATA_H_
