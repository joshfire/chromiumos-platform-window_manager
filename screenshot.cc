// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>
#include <sstream>

#include <cairo/cairo.h>
#include <gflags/gflags.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "base/logging.h"

DEFINE_string(window, "",
              "Window to capture, as a hexadecimal X ID "
              "(if empty, the root window is captured)");

using std::hex;
using std::istringstream;

static const char* kUsage =
    "Usage: screenshot [FLAGS] FILENAME.png\n"
    "\n"
    "Saves the contents of the entire screen or of a window to a file.";

int main(int argc, char** argv) {
  google::SetUsageMessage(kUsage);
  google::ParseCommandLineFlags(&argc, &argv, true);
  if (argc != 2) {
    google::ShowUsageWithFlags(argv[0]);
    return 1;
  }

  Display* display = XOpenDisplay(NULL);
  CHECK(display);

  const char* filename = argv[1];

  Window win = None;
  if (FLAGS_window.empty()) {
    win = DefaultRootWindow(display);
  } else {
    istringstream input(FLAGS_window);
    CHECK(!(input >> hex >> win).fail())
        << "Unable to parse \"" << input << "\" as window "
        << "(should be hexadecimal X ID)";
  }

  Window root = None;
  int x = 0, y = 0;
  unsigned int width = 0, height = 0, border_width = 0, depth = 0;
  CHECK(XGetGeometry(display, win, &root, &x, &y, &width, &height,
                     &border_width, &depth));
  XImage* image = XGetImage(
      display, win, 0, 0, width, height, AllPlanes, ZPixmap);
  CHECK(image);
  CHECK(image->depth == 24 || image->depth == 32)
      << "Unsupported image depth " << image->depth;

  cairo_surface_t* surface =
      cairo_image_surface_create_for_data(
          reinterpret_cast<unsigned char*>(image->data),
          image->depth == 24 ? CAIRO_FORMAT_RGB24 : CAIRO_FORMAT_ARGB32,
          image->width,
          image->height,
          image->bytes_per_line);
  CHECK(surface) << "Unable to create Cairo surface from XImage data";
  CHECK(cairo_surface_write_to_png(surface, filename) == CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy(surface);

  XDestroyImage(image);
  XCloseDisplay(display);
  return 0;
}
