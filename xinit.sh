#!/bin/sh

# Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This script gets executed by run_in_xephyr.sh.

# We lose the changes made by these commands if we don't wait until after
# the window manager is already running, since the X server resets its
# state when the last client disconnects.
(sleep 1 && if [ -e $HOME/.Xresources ]; then xrdb -load $HOME/.Xresources; fi)
(sleep 1 && xmodmap -e 'add mod4 = Super_L Super_R')

xterm &
xprop -root -f _CHROME_LOGGED_IN 32i -set _CHROME_LOGGED_IN 1

# Uncomment to dump all communication between the WM and X server to a file.
#XTRACE="xtrace -n -o /tmp/wm_xtrace.log"

exec $XTRACE ./wm \
  --background_image=../assets/images/background_1024x600.png \
  --logtostderr
