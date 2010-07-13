#!/bin/sh

# Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This script can be used to launch the window manager in a nested Xephyr X
# server.

dpi=${dpi:-96}
display=${display:-:1}
resolution=${resolution:-1024x600}

xinit ./xinit.sh -- /usr/bin/Xephyr $display -ac -br \
  -dpi $dpi -screen $resolution -host-cursor
