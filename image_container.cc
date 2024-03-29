// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/image_container.h"

#include <png.h>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"

using std::string;

namespace window_manager {

static const int kPngSignatureSize = 8;

// static
ImageContainer* ImageContainer::CreateContainerFromFile(
    const string& filename) {
  if (PngImageContainer::IsPngImage(filename)) {
    return new PngImageContainer(filename);
  } else {
    LOG(ERROR) << "Unable to determine file type of '" << filename
               << "' in ImageContainer::CreateContainerFromFile()";
    return NULL;
  }
}

ImageContainer::ImageContainer()
    : data_(NULL),
      data_was_allocated_with_malloc_(false),
      width_(0),
      height_(0),
      format_(IMAGE_FORMAT_UNKNOWN) {
}

void ImageContainer::SetData(uint8_t* new_data,
                             bool was_allocated_with_malloc) {
  if (data_) {
    if (data_was_allocated_with_malloc_)
      free(data_);
    else
      delete[] data_;
  }

  data_ = new_data;
  data_was_allocated_with_malloc_ = was_allocated_with_malloc;
}


// static
bool PngImageContainer::IsPngImage(const string& filename) {
  // Load the image.
  FILE* fp = fopen(filename.c_str(), "rb");
  if (!fp) {
    LOG(ERROR) << "Unable to open '" << filename
               << "' for reading in IsPngImage.";
    return false;
  }

  // Allocate a buffer where we can put the file signature.
  png_byte pngsig[kPngSignatureSize];

  // Read the signature from the file into the signature buffer.
  size_t bytes_read = fread(&pngsig[0], sizeof(png_byte),
                            kPngSignatureSize, fp);
  fclose(fp);

  if (bytes_read != (sizeof(png_byte) * kPngSignatureSize)) {
    LOG(ERROR) << "Unable to read data from '" << filename
               << "' in IsPngImage.";
    return false;
  }

  return png_sig_cmp(pngsig, 0, kPngSignatureSize) == 0 ? true : false;
}

PngImageContainer::PngImageContainer(const string& filename)
    : filename_(filename) {
}

static void PngErrorHandler(png_structp container_ptr,
                            png_const_charp error_str) {
  PngImageContainer* container =
      reinterpret_cast<PngImageContainer*>(container_ptr);
  LOG(ERROR) << "PNG error while reading '" << container->filename()
             << "':" << error_str;
}

static void PngWarningHandler(png_structp container_ptr,
                              png_const_charp error_str) {
  PngImageContainer* container =
      reinterpret_cast<PngImageContainer*>(container_ptr);
  LOG(WARNING) << "PNG warning while reading '" << container->filename()
               << "':" << error_str;
}

ImageContainer::Result PngImageContainer::LoadImage() {
  png_structp read_obj = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                                dynamic_cast<void*>(this),
                                                PngErrorHandler,
                                                PngWarningHandler);
  if (!read_obj) {
    LOG(ERROR) << "Couldn't initialize png read struct in LoadImage.";
    return ImageContainer::IMAGE_LOAD_FAILURE;
  }

  png_infop info_obj = png_create_info_struct(read_obj);
  if (!info_obj) {
    LOG(ERROR) << "Couldn't initialize png info struct in LoadImage.";
    png_destroy_read_struct(&read_obj, NULL, NULL);
    return ImageContainer::IMAGE_LOAD_FAILURE;
  }

  // Load the image.
  FILE* fp = fopen(filename_.c_str(), "rb");
  if (!fp) {
    LOG(ERROR) << "Unable to open '" << filename_
               << "' for reading in LoadImage.";
    png_destroy_read_struct(&read_obj, &info_obj, NULL);
    return ImageContainer::IMAGE_LOAD_FAILURE;
  }

  png_init_io(read_obj, fp);
  png_read_info(read_obj, info_obj);
  set_width(png_get_image_width(read_obj, info_obj));
  set_height(png_get_image_height(read_obj, info_obj));
  png_uint_32 color_type = png_get_color_type(read_obj, info_obj);
  png_uint_32 depth = png_get_bit_depth(read_obj, info_obj);

  switch (color_type) {
    case PNG_COLOR_TYPE_PALETTE:
      // Read paletted images as RGB
      png_set_palette_to_rgb(read_obj);
      break;
    case PNG_COLOR_TYPE_GRAY_ALPHA:
    case PNG_COLOR_TYPE_GRAY:
      // Expand smaller bit depths to eight-bit.
      if (depth < 8)
        png_set_gray_1_2_4_to_8(read_obj);
      // Convert grayscale images to RGB.
      png_set_gray_to_rgb(read_obj);
      break;
    default:
      break;
  }

  // Add an opaque alpha channel if there isn't one already.
  if (!(color_type & PNG_COLOR_MASK_ALPHA)) {
    png_set_filler(read_obj, 0xff, PNG_FILLER_AFTER);
    set_format(IMAGE_FORMAT_RGBX_32);
  } else {
    set_format(IMAGE_FORMAT_RGBA_32);
  }

  // If the image has a transparancy color set, convert it to an alpha
  // channel.
  if (png_get_valid(read_obj, info_obj, PNG_INFO_tRNS)) {
    png_set_tRNS_to_alpha(read_obj);
  }

  // We don't support 16 bit precision, so if the image has 16 bits
  // per channel, truncate it to 8 bits.
  if (depth == 16) {
    png_set_strip_16(read_obj);
  }

  scoped_array<uint8_t*> row_pointers(new uint8_t*[height()]);
  SetData(new uint8_t[height() * stride()], false);  // malloc=false

  for (size_t i = 0; i < height(); i++) {
    size_t position = i * stride();
    row_pointers[i] = data() + position;
  }

  png_read_image(read_obj, reinterpret_cast<png_byte**>(row_pointers.get()));

  png_destroy_read_struct(&read_obj, &info_obj, NULL);
  fclose(fp);

  DLOG(INFO) << "Successfully loaded image '" << filename_ << "' ("
             << width() << "x" << height() << ", "
             << bits_per_pixel() << " bit(s)/pixel)";

  return ImageContainer::IMAGE_LOAD_SUCCESS;
}


InMemoryImageContainer::InMemoryImageContainer(
    uint8_t* new_data, size_t new_width, size_t new_height,
    ImageFormat new_format, bool was_allocated_with_malloc) {
  DCHECK(new_data);
  SetData(new_data, was_allocated_with_malloc);
  set_width(new_width);
  set_height(new_height);
  set_format(new_format);
}

}  // namespace window_manager
