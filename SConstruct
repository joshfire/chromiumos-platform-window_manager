# Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import fnmatch
import itertools
import os

import make_shaders

# Create a base environment including things that are likely to be common
# to all of the objects in this directory. We pull in overrides from the
# environment to enable cross-compile.
base_env = Environment()
for key in Split('CC CXX AR RANLIB LD NM'):
  value = os.environ.get(key)
  if value != None:
    base_env[key] = value
for key in Split('CFLAGS CCFLAGS CPPPATH LIBPATH'):
  value = os.environ.get(key)
  if value != None:
    base_env[key] = Split(value)

# Fix issue with scons not passing some vars through the environment.
for key in Split('PKG_CONFIG_LIBDIR PKG_CONFIG_PATH SYSROOT'):
  if os.environ.has_key(key):
    base_env['ENV'][key] = os.environ[key]

pkgconfig = os.environ.get('PKG_CONFIG', 'pkg-config')

base_env.Append(
    CCFLAGS=Split('-Wall -Werror -Wnon-virtual-dtor -Woverloaded-virtual'))

# Unless we disable strict aliasing, we get warnings about some of the
# program's command line flags processing code that look like:
#   'dereferencing type-punned pointer will break strict-aliasing rules'
base_env.Append(CCFLAGS=['-fno-strict-aliasing'])

base_env.Append(CPPPATH=['..'])

# We need glib-2.0 ONLY to satisfy libbase.
# TODO(derat): Weep.
base_env.Append(LIBS=Split('base gflags pthread rt'))
base_env.ParseConfig(pkgconfig + ' --cflags --libs glib-2.0 x11')

# Fork off a new environment, add Cairo to it, and build the screenshot
# program.
screenshot_env = base_env.Clone()
screenshot_env.ParseConfig(pkgconfig + ' --cflags --libs cairo')
screenshot_env.Program('screenshot', 'screenshot.cc')

# Check for BACKEND on the build line
backend = ARGUMENTS.get('BACKEND', 'OPENGL').lower()

# Start a new environment for the window manager.
wm_env = base_env.Clone()

wm_env.Append(LIBS=Split('chromeos metrics protobuf'))
wm_env.ParseConfig(pkgconfig + ' --cflags --libs dbus-1 libpcrecpp libpng12 ' +
                   'xcb x11-xcb xcb-composite xcb-randr xcb-shape xcb-damage ' +
                   'xcb-sync xcomposite xdamage xext xrender')

if backend == 'opengl':
  # This is needed so that glext headers include glBindBuffer and
  # related APIs.
  wm_env.Append(CPPDEFINES=['GL_GLEXT_PROTOTYPES'])
  wm_env.ParseConfig(pkgconfig + ' --cflags --libs gl')
elif backend == 'opengles':
  # Add builder for .glsl* files, and GLESv2 libraries
  make_shaders.AddBuildRules(wm_env)
  wm_env.Append(LIBS=['EGL', 'GLESv2'])

# Define an IPC library that will be used both by the WM and by client apps.
srcs = Split('''\
  atom_cache.cc
  geometry.cc
  util.cc
  wm_ipc.cc
  x11/real_x_connection.cc
  x11/x_connection.cc
  x11/x_connection_internal.cc
''')
libwm_ipc = wm_env.Library('wm_ipc', srcs)

# Create a library with just the additional files needed by the window
# manager.  This is a bit ugly; we can't include any source files that are
# also compiled in different environments here (and hence we just get e.g.
# atom_cache.cc and util.cc via libwm_ipc).
srcs = Split('''\
  chrome_watchdog.cc
  compositor/animation.cc
  compositor/compositor.cc
  compositor/gl_interface_base.cc
  compositor/layer_visitor.cc
  compositor/real_compositor.cc
  event_consumer_registrar.cc
  event_loop.cc
  focus_manager.cc
  image_container.cc
  image_grid.cc
  key_bindings.cc
  layout/layout_manager.cc
  layout/separator.cc
  layout/snapshot_window.cc
  layout/toplevel_window.cc
  login/login_controller.cc
  login/login_entry.cc
  modality_handler.cc
  motion_event_coalescer.cc
  panels/panel.cc
  panels/panel_bar.cc
  panels/panel_dock.cc
  panels/panel_manager.cc
  pointer_position_watcher.cc
  profiler.cc
  real_dbus_interface.cc
  resize_box.cc
  screen_locker_handler.cc
  shadow.cc
  stacking_manager.cc
  transient_window_collection.cc
  window.cc
  window_manager.cc
''')
if backend == 'opengl':
  srcs.append(Split('''\
    compositor/gl/opengl_visitor.cc
    compositor/gl/real_gl_interface.cc
  '''))
elif backend == 'opengles':
  srcs.append(Split('''\
    compositor/gles/opengles_visitor.cc
    compositor/gles/real_gles2_interface.cc
    compositor/gles/shader_base.cc
    compositor/gles/shaders.cc
  '''))
  # SCons doesn't figure out this dependency on its own, since
  # opengles_visitor.cc includes "window_manager/compositor/gles/shaders.h", 
  # while the shaders builder just provides "compositor/gles/shaders.h".
  Depends('compositor/gles/opengles_visitor.o', 'compositor/gles/shaders.h')
elif backend == 'xrender':
  srcs.append(Split('''\
    compositor/xrender/xrender_visitor.cc
  '''))

libwm_core = wm_env.Library('wm_core', srcs)

# Define a library to be used by tests.
srcs = Split('''\
  compositor/gl/mock_gl_interface.cc
  compositor/mock_compositor.cc
  mock_dbus_interface.cc
  test_lib.cc
  x11/mock_x_connection.cc
''')
libtest = wm_env.Library('test', Split(srcs))

wm_env.Prepend(LIBS=[libwm_core, libwm_ipc])

backend_defines = {'opengl': ['COMPOSITOR_OPENGL'],
                   'opengles': ['COMPOSITOR_OPENGLES'],
                   'xrender': ['COMPOSITOR_XRENDER']}
wm_env.Append(CPPDEFINES=backend_defines[backend])

test_env = wm_env.Clone()

wm_env.Program('wm', 'main.cc')

test_env.Append(LIBS=['gtest'])
# libtest needs to be listed first since it depends on wm_core and wm_ipc.
test_env.Prepend(LIBS=[libtest])
tests = []

# These are tests that only get built when we use particular backends.
backend_tests = {'opengl': ['real_compositor_test.cc',
                            'opengl_visitor_test.cc'],
                 'opengles': [],
                 'xrender': []}
all_backend_tests = set(itertools.chain(*backend_tests.values()))
for root, dirnames, filenames in os.walk('.'):
  for filename in fnmatch.filter(filenames, '*_test.cc'):
    if filename in all_backend_tests and filename not in backend_tests[backend]:
      continue
    tests += test_env.Program(os.path.join(root, filename))

# Create a 'tests' target that will build all tests.
test_env.Alias('tests', tests)

# mock_chrome is just a small program that developers can use to test
# interaction between the window manager and Chrome.  We only define a
# target for it if gtkmm is installed so that this SConstruct file can
# still be parsed in the chroot build environment, which shouldn't contain
# gtkmm.
if os.system(pkgconfig + ' --exists gtkmm-2.4') == 0:
  mock_chrome_env = wm_env.Clone()
  mock_chrome_env.ParseConfig(pkgconfig + ' --cflags --libs gtkmm-2.4')
  mock_chrome_env.Program('mock_chrome', 'mock_chrome.cc')
