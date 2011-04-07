#!/bin/sh

# Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

export DATA_DIR=/home/chronos
export HOME=/home/chronos/user
export XAUTHORITY=/home/chronos/.Xauthority
export DISPLAY=:0.0
export USER_ID=1000

WM="/usr/bin/chromeos-wm"
IMAGES="/usr/share/chromeos-assets/images"
INITIAL_CHROME_FILE="/var/run/state/windowmanager/initial-chrome-window-mapped"
PROFILE_DIR="${DATA_DIR}/profile"
LOGGED_IN_LOG_DIR="${HOME}/log"
LOGGED_OUT_LOG_DIR="/var/log/window_manager"
LOGGED_IN_SCREENSHOT_DIR="${HOME}/Downloads/Screenshots"
LOGGED_OUT_SCREENSHOT_DIR="/tmp"
XTERM_COMMAND="/usr/bin/cros-term"

# If there's an ICC color profile installed, use it to configure the display.
COLOR_PROFILE="/usr/share/color/icc/internal_display.icm"
CALIBRATE_DISPLAY_COMMAND=
if [ -e "$COLOR_PROFILE" ]; then
  CALIBRATE_DISPLAY_COMMAND="/usr/bin/xcalib $COLOR_PROFILE"
fi

exec "${WM}"                                                            \
  --background_image="${IMAGES}/background_1024x600.png"                \
  --calibrate_display_command="${CALIBRATE_DISPLAY_COMMAND}"            \
  --initial_chrome_window_mapped_file="${INITIAL_CHROME_FILE}"          \
  --logged_in_log_dir="${LOGGED_IN_LOG_DIR}"                            \
  --logged_in_screenshot_output_dir="${LOGGED_IN_SCREENSHOT_DIR}"       \
  --logged_out_log_dir="${LOGGED_OUT_LOG_DIR}"                          \
  --logged_out_screenshot_output_dir="${LOGGED_OUT_SCREENSHOT_DIR}"     \
  --panel_anchor_image="${IMAGES}/panel_anchor.png"                     \
  --panel_dock_background_image="${IMAGES}/panel_dock_bg.png"           \
  --panel_content_shadow_image_dir="${IMAGES}/panel_content_shadow"     \
  --panel_separator_shadow_image_dir="${IMAGES}/panel_separator_shadow" \
  --panel_titlebar_shadow_image_dir="${IMAGES}/panel_titlebar_shadow"   \
  --profile_dir="${PROFILE_DIR}"                                        \
  --rectangular_shadow_image_dir="${IMAGES}/rectangular_shadow"         \
  --report_metrics                                                      \
  --separator_image="${IMAGES}/separator.png"                           \
  --unaccelerated_graphics_image="${IMAGES}/unaccelerated_graphics.png" \
  --unredirect_fullscreen_window=true                                   \
  --xterm_command="${XTERM_COMMAND}"
