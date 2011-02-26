// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor/compositor.h"

#include <cmath>
#include <cstdio>

using std::string;
using std::tr1::unordered_set;

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

bool Compositor::Color::SetHex(const string& hex_str) {
  const char* str = hex_str.c_str();
  if (str[0] == '#')
    str++;

  unsigned int r, g, b;
  if (strlen(str) == 3) {
    if (sscanf(str, "%01x%01x%01x", &r, &g, &b) != 3)
      return false;
    r = r * 16 + r;
    g = g * 16 + g;
    b = b * 16 + b;
  } else if (strlen(str) == 6) {
    if (sscanf(str, "%02x%02x%02x", &r, &g, &b) != 3)
      return false;
  } else {
    return false;
  }

  red = r / 255.0f;
  green = g / 255.0f;
  blue = b / 255.0f;
  return true;
}

int Compositor::Actor::GetTiltedWidth(int width, double tilt) {
  // Correct for the effect of the given tilt on the width.  This is
  // basically the x-axis component of the perspective transform for
  // the tilt.
  double theta = tilt * M_PI / 2.0;
  double x_scale_factor = cos(theta) / (0.4 * sin(theta) + 1.0);
  return static_cast<int>(width * x_scale_factor + 0.5);
}

void Compositor::ResetActiveVisibilityGroups() {
  unordered_set<int> groups;
  SetActiveVisibilityGroups(groups);
}

void Compositor::SetActiveVisibilityGroup(int group) {
  unordered_set<int> groups;
  groups.insert(group);
  SetActiveVisibilityGroups(groups);
}

}  // namespace window_manager
