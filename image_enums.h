// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_IMAGE_ENUMS_H_
#define WINDOW_MANAGER_IMAGE_ENUMS_H_

#include "base/logging.h"

namespace window_manager {

// Different image data formats that we support.
enum ImageFormat {
  IMAGE_FORMAT_UNKNOWN = 0,
  IMAGE_FORMAT_RGBA_32,
  IMAGE_FORMAT_RGBX_32,  // RGB data with opaque alpha in fourth byte
  IMAGE_FORMAT_BGRA_32,
  IMAGE_FORMAT_BGRX_32,
  IMAGE_FORMAT_RGB_16,  // R5 G6 B5 packed in an unsigned short (R msb).
};

// Does the passed-in image format use an alpha channel?
inline bool ImageFormatUsesAlpha(ImageFormat format) {
  switch (format) {
    case IMAGE_FORMAT_RGBA_32:  // fallthrough
    case IMAGE_FORMAT_BGRA_32:
      return true;
    case IMAGE_FORMAT_RGBX_32:  // fallthrough
    case IMAGE_FORMAT_BGRX_32:  // fallthrough
    case IMAGE_FORMAT_RGB_16:
      return false;
    default:
      NOTREACHED() << "Unhandled image format " << format;
      return false;
  }
}

// Get the number of bits per pixel in the passed-in image format.
inline int GetBitsPerPixelInImageFormat(ImageFormat format) {
  switch (format) {
    case IMAGE_FORMAT_RGBA_32:  // fallthrough
    case IMAGE_FORMAT_RGBX_32:  // fallthrough
    case IMAGE_FORMAT_BGRA_32:  // fallthrough
    case IMAGE_FORMAT_BGRX_32:
      return 32;
    case IMAGE_FORMAT_RGB_16:
      return 16;
    default:
      NOTREACHED() << "Unhandled image format " << format;
      return 0;
  }
}

}  // namespace window_manager

#endif  // WINDOW_MANAGER_IMAGE_ENUMS_H_
