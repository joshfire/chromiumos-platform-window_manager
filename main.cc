// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cmath>
#include <cstdlib>
#include <ctime>

extern "C" {
#include <X11/Xlib.h>
}

#include <gflags/gflags.h>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/file_path.h"
#include "base/file_util.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/string_util.h"
#include "window_manager/callback.h"
#include "window_manager/event_loop.h"
#include "window_manager/profiler.h"
#include "window_manager/real_compositor.h"
#if defined(COMPOSITOR_OPENGL)
#include "window_manager/real_gl_interface.h"
#elif defined(COMPOSITOR_OPENGLES)
#include "window_manager/gles/real_gles2_interface.h"
#endif
#include "window_manager/real_x_connection.h"
#include "window_manager/util.h"
#include "window_manager/window_manager.h"

DEFINE_string(display, "",
              "X Display to connect to (overrides DISPLAY env var).");
DEFINE_bool(logtostderr, false,
            "Log to stderr (see --logged_{in,out}_log_dir otherwise)");
DEFINE_string(profile_dir, "./profile",
              "Directory where profiles should be written; created if it "
              "doesn't exist.");
DEFINE_int32(profile_max_samples, 200,
             "Maximum number of samples (buffer size) for profiler.");
DEFINE_bool(start_profiler, false,
            "Start profiler at window manager startup.");
DEFINE_int32(pause_at_start, 0,
             "Specify this to pause for N seconds at startup.");

using std::string;
using window_manager::EventLoop;
using window_manager::RealCompositor;
#if defined(COMPOSITOR_OPENGL)
using window_manager::RealGLInterface;
#elif defined(COMPOSITOR_OPENGLES)
using window_manager::RealGles2Interface;
#else
#error COMPOSITOR_OPENGL or COMPOSITOR_OPENGLES must be defined
#endif
using window_manager::RealXConnection;
using window_manager::WindowManager;
using window_manager::util::GetTimeAsString;
using window_manager::util::SetUpLogSymlink;

// This should be adjusted according to number of PROFILER_MARKER_*
static const int kMaxNumProfilerSymbols = 100;

// Handler called by Chrome logging code on failed asserts.
static void HandleLogAssert(const string& str) {
  abort();
}

// Handler called in response to Xlib I/O errors.  We install this so we won't
// generate a window manager crash dump whenever the X server crashes.
static int HandleXIOError(Display* display) {
  LOG(ERROR) << "Got X I/O error (probably lost connection to server); exiting";
  exit(EXIT_FAILURE);
}

int main(int argc, char** argv) {
  base::AtExitManager exit_manager;  // needed by base::Singleton

  google::ParseCommandLineFlags(&argc, &argv, true);
  if (!FLAGS_display.empty())
    setenv("DISPLAY", FLAGS_display.c_str(), 1);

  CommandLine::Init(argc, argv);
  if (FLAGS_pause_at_start > 0)
    sleep(FLAGS_pause_at_start);

  // Just log to stderr initially; WindowManager will re-initialize to
  // switch to a file once we know whether we're logged in or not if
  // --logtostderr is false.
  logging::InitLogging(NULL,
                       logging::LOG_ONLY_TO_SYSTEM_DEBUG_LOG,
                       logging::DONT_LOCK_LOG_FILE,
                       logging::APPEND_TO_OLD_LOG_FILE);

  // Chrome's logging code uses int3 to send SIGTRAP in response to failed
  // asserts, but Breakpad only installs signal handlers for SEGV, ABRT,
  // FPE, ILL, and BUS.  Use our own function to send ABRT instead.
  logging::SetLogAssertHandler(HandleLogAssert);

#if defined(PROFILE_BUILD)
  const string profile_basename = StringPrintf(
      "prof_%s.%s", WindowManager::GetWmName(),
      GetTimeAsString(::time(NULL)).c_str());

  if (!file_util::CreateDirectory(FilePath(FLAGS_profile_dir))) {
    LOG(ERROR) << "Unable to create profiling directory " << FLAGS_profile_dir;
  } else {
    SetUpLogSymlink(StringPrintf("%s/prof_%s.LATEST",
                                 FLAGS_profile_dir.c_str(),
                                 WindowManager::GetWmName()),
                    profile_basename);
  }

  const string profile_path = FLAGS_profile_dir + "/" + profile_basename;
  Singleton<window_manager::Profiler>()->Start(
      new window_manager::ProfilerWriter(FilePath(profile_path)),
      kMaxNumProfilerSymbols, FLAGS_profile_max_samples);

  Singleton<window_manager::DynamicMarker>()->set_profiler(
      Singleton<window_manager::Profiler>::get());

  if (!FLAGS_start_profiler) {
    Singleton<window_manager::Profiler>()->Pause();
  }
#endif

  const char* display_name = getenv("DISPLAY");
  Display* display = XOpenDisplay(display_name);
  if (!display) {
    LOG(ERROR) << "Unable to open "
               << (display_name ? display_name : "default display");
    exit(EXIT_FAILURE);
  }
  XSetIOErrorHandler(HandleXIOError);

  RealXConnection xconn(display);
  EventLoop event_loop;
#if defined(COMPOSITOR_OPENGL)
  RealGLInterface gl_interface(&xconn);
#elif defined(COMPOSITOR_OPENGLES)
  RealGles2Interface gl_interface(&xconn);
#endif
  RealCompositor compositor(&event_loop, &xconn, &gl_interface);

  WindowManager wm(&event_loop, &xconn, &compositor);
  wm.set_initialize_logging(!FLAGS_logtostderr);
  wm.Init();

  // TODO: Need to also use XAddConnectionWatch()?
  const int x11_fd = xconn.GetConnectionFileDescriptor();
  LOG(INFO) << "X11 connection is on fd " << x11_fd;
  event_loop.AddFileDescriptor(
      x11_fd, NewPermanentCallback(&wm, &WindowManager::ProcessPendingEvents));
  event_loop.AddPrePollCallback(
      NewPermanentCallback(&wm, &WindowManager::ProcessPendingEvents));

  event_loop.Run();
  return 0;
}
