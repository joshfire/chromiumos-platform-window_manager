// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_IMAGE_CONTAINER_H_
#define WINDOW_MANAGER_IMAGE_CONTAINER_H_

#include <string>

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "window_manager/image_enums.h"

namespace window_manager {

// This base class contains image data.  Given a filename, it's able to
// infer its type and construct an appropriate container object.
class ImageContainer {
 public:
  enum Result {
    IMAGE_LOAD_SUCCESS = 0,
    IMAGE_LOAD_FAILURE,
  };

  // This determines the type of image container to use automatically
  // from the file contents, and returns a newly allocated image
  // container of the correct type.  The caller is responsible for
  // deleting the returned image container.  Returns NULL if unable to
  // determine file type or access the file.  Note that the image data
  // isn't loaded until the LoadImage method returns successfully.
  static ImageContainer* CreateContainerFromFile(const std::string& filename);

  ImageContainer();
  virtual ~ImageContainer() { SetData(NULL, false); }

  // Loads the image, and returns a result code.
  virtual Result LoadImage() = 0;

  uint8_t* data() const { return data_; }
  size_t width() const { return width_; }
  size_t height() const { return height_; }

  // Return stride in bytes of a row of pixels in the image data.
  size_t stride() const {
    return bits_per_pixel() * width() / 8;
  }

  // The number of bits per pixel in the image.
  int bits_per_pixel() const { return GetBitsPerPixelInImageFormat(format_); }

  // Currently, this class only supports 32-bit formats as well as 16-bit RGB.
  ImageFormat format() const { return format_; }

 protected:
  // Set parameters read from image.
  void set_width(size_t new_width) { width_ = new_width; }
  void set_height(size_t new_height) { height_ = new_height; }
  void set_format(ImageFormat format) {
    format_ = format;
  }

  // Takes ownership of the given array.
  void SetData(uint8_t* new_data, bool was_allocated_with_malloc);

 private:
  // 16 or 32-bit-per-pixel image data, oriented with (0, 0) at the beginning of
  // the array.  We own this data.
  uint8_t* data_;

  // Was |data_| allocated using malloc() (rather than new[])?
  bool data_was_allocated_with_malloc_;

  // Image width in pixels.
  size_t width_;

  // Image height in pixels.
  size_t height_;

  // Format of the image (from image_enums.h).
  ImageFormat format_;

  DISALLOW_COPY_AND_ASSIGN(ImageContainer);
};

// This is the PNG-specific version of the image container.  It can
// detect PNG image files from their contents, and load them into
// memory, converting them to the proper form for the ImageContainer
// class.
class PngImageContainer : public virtual ImageContainer {
 public:
  // Determines if the given file is a PNG image.
  static bool IsPngImage(const std::string& filename);

  explicit PngImageContainer(const std::string& filename);
  virtual ~PngImageContainer() {}

  const std::string& filename() { return filename_; }

  virtual Result LoadImage();

 private:
  // Name of the file being loaded.
  std::string filename_;

  DISALLOW_COPY_AND_ASSIGN(PngImageContainer);
};

// This is an implementation of ImageContainer that can be constructed
// directly from raw, already-loaded data.
class InMemoryImageContainer : public virtual ImageContainer {
 public:
  // Takes ownership of |new_data|, which must be 32-bit image data.
  InMemoryImageContainer(uint8_t* new_data, size_t new_width, size_t new_height,
                         ImageFormat new_format,
                         bool was_allocated_using_malloc);
  virtual ~InMemoryImageContainer() {}

  // This doesn't need to be called.
  virtual Result LoadImage() { return IMAGE_LOAD_SUCCESS; }

 private:
  DISALLOW_COPY_AND_ASSIGN(InMemoryImageContainer);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_IMAGE_CONTAINER_H_
