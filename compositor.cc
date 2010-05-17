// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor.h"

#include <cmath>

namespace window_manager {

int Compositor::Actor::GetTiltedWidth(int width, double tilt) {
  // Correct for the effect of the given tilt on the width.  This is
  // basically the x-axis component of the perspective transform for
  // the tilt.
  double theta = tilt * M_PI / 2.0;
  double x_scale_factor = cos(theta) / (0.4 * sin(theta) + 1.0);
  return static_cast<int>(width * x_scale_factor + 0.5);
}

}  // namespace window_manager
