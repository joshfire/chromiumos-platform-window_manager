# Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os
import itertools

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

base_env.Append(CCFLAGS=Split('-Wall -Werror'))

# Unless we disable strict aliasing, we get warnings about some of the
# program's command line flags processing code that look like:
#   'dereferencing type-punned pointer will break strict-aliasing rules'
base_env.Append(CCFLAGS=['-fno-strict-aliasing'])

base_env.Append(CPPPATH=['..'])

base_env.Append(LIBS=Split('base gflags rt'))

base_env.ParseConfig('pkg-config --cflags --libs x11')

# Fork off a new environment, add Cairo to it, and build the screenshot
# program.
screenshot_env = base_env.Clone()
screenshot_env.ParseConfig('pkg-config --cflags --libs cairo')
screenshot_env.Program('screenshot', 'screenshot.cc')

# Check for BACKEND on the build line
backend = ARGUMENTS.get('BACKEND', 'OPENGL').lower()

# Start a new environment for the window manager.
wm_env = base_env.Clone()

wm_env.Append(LIBS=['protobuf'])
wm_env.ParseConfig('pkg-config --cflags --libs libpcrecpp libpng12 ' +
                   'xcb x11-xcb xcb-composite xcb-randr xcb-shape xcb-damage ' +
                   'xdamage xext')


if backend == 'opengl':
  # This is needed so that glext headers include glBindBuffer and
  # related APIs.
  wm_env.Append(CPPDEFINES=['GL_GLEXT_PROTOTYPES'])
  wm_env.ParseConfig('pkg-config --cflags --libs gl')
elif backend == 'opengles':
  # Add builder for .glsl* files, and GLESv2 libraries
  make_shaders.AddBuildRules(wm_env)
  wm_env.Append(LIBS=['EGL', 'GLESv2'])

# Define an IPC library that will be used both by the WM and by client apps.
srcs = Split('''\
  atom_cache.cc
  real_x_connection.cc
  util.cc
  wm_ipc.cc
  x_connection.cc
''')
libwm_ipc = wm_env.Library('wm_ipc', srcs)

# Create a library with just the additional files needed by the window
# manager.  This is a bit ugly; we can't include any source files that are
# also compiled in different environments here (and hence we just get e.g.
# atom_cache.cc and util.cc via libwm_ipc).
srcs = Split('''\
  compositor.cc
  event_consumer_registrar.cc
  event_loop.cc
  focus_manager.cc
  gl_interface_base.cc
  hotkey_overlay.cc
  image_container.cc
  key_bindings.cc
  layout_manager.cc
  login_controller.cc
  login_entry.cc
  motion_event_coalescer.cc
  panel.cc
  panel_bar.cc
  panel_dock.cc
  panel_manager.cc
  pointer_position_watcher.cc
  profiler.cc
  real_compositor.cc
  screen_locker_handler.cc
  separator.cc
  shadow.cc
  snapshot_window.cc
  stacking_manager.cc
  toplevel_window.cc
  transient_window_collection.cc
  window.cc
  window_manager.cc
''')
if backend == 'opengl':
  srcs.append(Split('''\
    opengl_visitor.cc
    real_gl_interface.cc
  '''))
elif backend == 'opengles':
  srcs.append(Split('''\
    gles/opengles_visitor.cc
    gles/shader_base.cc
    gles/shaders.cc
    gles/real_gles2_interface.cc
  '''))
  # SCons doesn't figure out this dependency on its own, since
  # opengles_visitor.cc includes "window_manager/gles/shaders.h", while the
  # shaders builder just provides "gles/shaders.h".
  Depends('gles/opengles_visitor.o', 'gles/shaders.h')

libwm_core = wm_env.Library('wm_core', srcs)

# Define a library to be used by tests.
srcs = Split('''\
  mock_compositor.cc
  mock_gl_interface.cc
  mock_x_connection.cc
  test_lib.cc
''')
libtest = wm_env.Library('test', Split(srcs))

wm_env.Prepend(LIBS=[libwm_core, libwm_ipc])

backend_defines = {'opengl': ['COMPOSITOR_OPENGL'],
                   'opengles': ['COMPOSITOR_OPENGLES']}
wm_env.Append(CPPDEFINES=backend_defines[backend])

# Do not include crash dumper in the unit test environment
test_env = wm_env.Clone()
wm_env.Append(LIBS=['libcrash'])

wm_env.Program('wm', 'main.cc')

test_env.Append(LIBS=['gtest'])
# libtest needs to be listed first since it depends on wm_core and wm_ipc.
test_env.Prepend(LIBS=[libtest])
tests = []

# These are tests that only get built when we use particular backends
backend_tests = {'opengl': ['real_compositor_test.cc',
                            'opengl_visitor_test.cc'],
                 'opengles': []}
all_backend_tests = set(itertools.chain(*backend_tests.values()))
for test_src in Glob('*_test.cc', strings=True):
  if test_src in all_backend_tests and test_src not in backend_tests[backend]:
    continue
  tests += test_env.Program(test_src)
# Create a 'tests' target that will build all tests.
test_env.Alias('tests', tests)

# mock_chrome is just a small program that developers can use to test
# interaction between the window manager and Chrome.  We only define a
# target for it if gtkmm is installed so that this SConstruct file can
# still be parsed in the chroot build environment, which shouldn't contain
# gtkmm.
if os.system('pkg-config --exists gtkmm-2.4') == 0:
  mock_chrome_env = wm_env.Clone()
  mock_chrome_env.ParseConfig('pkg-config --cflags --libs gtkmm-2.4')
  mock_chrome_env.Program('mock_chrome', 'mock_chrome.cc')
