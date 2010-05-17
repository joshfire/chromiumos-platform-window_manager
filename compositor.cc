// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor.h"

#include <cmath>

namespace window_manager {

void Compositor::Color::SetHsv(float hue, float saturation, float value) {
  float channel1, channel2, fraction;
  double int_part;
  fraction = modf(hue, &int_part);
  int hue_int = int_part;

  if (hue_int % 2 == 0)
    fraction = 1.0 - fraction;

  channel1 = value * (1 - saturation);
  channel2 = value * (1 - saturation * fraction);

  switch (hue_int % 6) {
    case 0:
      red = value;
      green = channel2;
      blue = channel1;
      break;
    case 1:
      red = channel2;
      green = value;
      blue = channel1;
      break;
    case 2:
      red = channel1;
      green = value;
      blue = channel2;
      break;
    case 3:
      red = channel1;
      green = channel2;
      blue = value;
      break;
    case 4:
      red = channel2;
      green = channel1;
      blue = value;
      break;
    case 5:
      red = value;
      green = channel1;
      blue = channel2;
      break;
  }
}

int Compositor::Actor::GetTiltedWidth(int width, double tilt) {
  // Correct for the effect of the given tilt on the width.  This is
  // basically the x-axis component of the perspective transform for
  // the tilt.
  double theta = tilt * M_PI / 2.0;
  double x_scale_factor = cos(theta) / (0.4 * sin(theta) + 1.0);
  return static_cast<int>(width * x_scale_factor + 0.5);
}

}  // namespace window_manager
