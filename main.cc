// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unistd.h>

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
#include "window_manager/compositor.h"
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

DECLARE_bool(wm_use_compositing);  // from window_manager.cc

DEFINE_string(log_dir, ".",
              "Directory where logs should be written; created if it doesn't "
              "exist.");
DEFINE_string(display, "",
              "X Display to connect to (overrides DISPLAY env var).");
DEFINE_bool(logtostderr, false,
            "Write logs to stderr instead of to a file in log_dir.");
DEFINE_string(profile_dir, "./profile",
              "Directory where profiles should be written; created if it "
              "doesn't exist.");
DEFINE_int32(profile_max_samples, 200,
             "Maximum number of samples (buffer size) for profiler.");
DEFINE_int32(pause_at_start, 0,
             "Specify this to pause for N seconds at startup.");
DEFINE_bool(logged_in, true, "Whether Chrome is logged in or not.");

using std::string;
using window_manager::Compositor;
using window_manager::EventLoop;
using window_manager::GetTimeAsString;
using window_manager::MockCompositor;
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

// This should be adjusted according to number of PROFILER_MARKER_*
static const int kMaxNumProfilerSymbols = 100;

// Handler called by Chrome logging code on failed asserts.
static void HandleLogAssert(const string& str) {
  abort();
}

// Helper function to create a symlink pointing from 'symlink_path' (a full
// path) to 'log_basename' (the name of a file that should be in the same
// directory as the symlink).  Removes 'symlink_path' if it already exists.
// Returns true on success.
static bool SetUpLogSymlink(const string& symlink_path,
                            const string& log_basename) {
  if (access(symlink_path.c_str(), F_OK) == 0 &&
      unlink(symlink_path.c_str()) == -1) {
    PLOG(ERROR) << "Unable to unlink " << symlink_path;
    return false;
  }
  if (symlink(log_basename.c_str(), symlink_path.c_str()) == -1) {
    PLOG(ERROR) << "Unable to create symlink " << symlink_path
                << " pointing at " << log_basename;
    return false;
  }
  return true;
}

int main(int argc, char** argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  if (!FLAGS_display.empty())
    setenv("DISPLAY", FLAGS_display.c_str(), 1);

  CommandLine::Init(argc, argv);
  if (FLAGS_pause_at_start > 0)
    sleep(FLAGS_pause_at_start);

  const string log_basename = StringPrintf(
      "%s.%s", WindowManager::GetWmName(),
      GetTimeAsString(::time(NULL)).c_str());
  if (!FLAGS_logtostderr) {
    if (!file_util::CreateDirectory(FilePath(FLAGS_log_dir))) {
      LOG(ERROR) << "Unable to create logging directory " << FLAGS_log_dir;
    } else {
      SetUpLogSymlink(StringPrintf("%s/%s.LATEST",
                                   FLAGS_log_dir.c_str(),
                                   WindowManager::GetWmName()),
                      log_basename);
    }
  }

  const string log_path = FLAGS_log_dir + "/" + log_basename;
  logging::InitLogging(log_path.c_str(),
                       FLAGS_logtostderr ?
                         logging::LOG_ONLY_TO_SYSTEM_DEBUG_LOG :
                         logging::LOG_ONLY_TO_FILE,
                       logging::DONT_LOCK_LOG_FILE,
                       logging::APPEND_TO_OLD_LOG_FILE);

  // Chrome's logging code uses int3 to send SIGTRAP in response to failed
  // asserts, but Breakpad only installs signal handlers for SEGV, ABRT,
  // FPE, ILL, and BUS.  Use our own function to send ABRT instead.
  logging::SetLogAssertHandler(HandleLogAssert);

#if defined(PROFILE_BUILD)
  base::AtExitManager exit_manager;  // this is required to use Singleton
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
  PROFILER_START(profile_path,
                 kMaxNumProfilerSymbols,
                 FLAGS_profile_max_samples);
#endif

  const char* display_name = getenv("DISPLAY");
  Display* display = XOpenDisplay(display_name);
  CHECK(display) << "Unable to open "
                 << (display_name ? display_name : "default display");
  RealXConnection xconn(display);

  // Create the overlay window as soon as possible, to reduce the chances that
  // Chrome will be able to map a window before we've taken over.
  if (FLAGS_wm_use_compositing) {
    XWindow root = xconn.GetRootWindow();
    xconn.GetCompositingOverlayWindow(root);
  }

  EventLoop event_loop;

  scoped_ptr<Compositor> compositor;
#if defined(COMPOSITOR_OPENGL)
  scoped_ptr<RealGLInterface> gl_interface;
#elif defined(COMPOSITOR_OPENGLES)
  scoped_ptr<RealGles2Interface> gl_interface;
#endif

  if (FLAGS_wm_use_compositing) {
#if defined(COMPOSITOR_OPENGL)
    gl_interface.reset(new RealGLInterface(&xconn));
#elif defined(COMPOSITOR_OPENGLES)
    gl_interface.reset(new RealGles2Interface(&xconn));
#endif
    compositor.reset(
        new RealCompositor(&event_loop, &xconn, gl_interface.get()));
  } else {
    compositor.reset(new MockCompositor(&xconn));
  }

  WindowManager wm(&event_loop, &xconn, compositor.get(), FLAGS_logged_in);
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
