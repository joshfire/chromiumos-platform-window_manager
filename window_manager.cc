// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/window_manager.h"

#include <cstring>
#include <ctime>
#include <list>
#include <queue>

extern "C" {
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <X11/XF86keysym.h>
#include <X11/Xcursor/Xcursor.h>
}
#include <gflags/gflags.h>

#include "base/file_path.h"
#include "base/file_util.h"
#include "base/logging.h"
#include "base/string_util.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "metrics/metrics_library.h"
#include "window_manager/callback.h"
#include "window_manager/chrome_watchdog.h"
#include "window_manager/dbus_interface.h"
#include "window_manager/event_consumer.h"
#include "window_manager/event_loop.h"
#include "window_manager/focus_manager.h"
#include "window_manager/geometry.h"
#include "window_manager/image_container.h"
#include "window_manager/image_enums.h"
#include "window_manager/key_bindings.h"
#include "window_manager/layout/layout_manager.h"
#include "window_manager/login/login_controller.h"
#include "window_manager/modality_handler.h"
#include "window_manager/panels/panel_manager.h"
#include "window_manager/profiler.h"
#include "window_manager/screen_locker_handler.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/x11/x_connection.h"

DEFINE_string(background_color, "#000", "Background color");
DEFINE_string(screenshot_binary,
              "/usr/bin/screenshot",
              "Path to the screenshot binary");
DEFINE_string(logged_in_screenshot_output_dir,
              ".", "Output directory for screenshots when logged in");
DEFINE_string(logged_out_screenshot_output_dir,
              ".", "Output directory for screenshots when not logged in");
DEFINE_string(logged_in_log_dir,
              ".", "Directory to write logs to when logged in");
DEFINE_string(logged_out_log_dir,
              ".", "Directory to write logs to when not logged in");
DEFINE_string(unaccelerated_graphics_image,
              "../assets/images/unaccelerated_graphics.png",
              "Image to display when using unaccelerated rendering");
DEFINE_bool(unredirect_fullscreen_window,
            false,
            "Enable/disable compositing optimization that automatically turns"
            "off compositing if a topmost fullscreen window is present");
DEFINE_bool(report_metrics, false, "Report user action metrics via Chrome");

using base::hash_map;
using base::TimeDelta;
using base::TimeTicks;
using chromeos::WmIpcMessageType;
using std::list;
using std::make_pair;
using std::map;
using std::set;
using std::string;
using std::tr1::shared_ptr;
using std::vector;
using window_manager::util::FindWithDefault;
using window_manager::util::GetTimeAsString;
using window_manager::util::GetCurrentTimeSec;
using window_manager::util::GetMonotonicTime;
using window_manager::util::RunCommandInBackground;
using window_manager::util::SetUpLogSymlink;
using window_manager::util::XidStr;

namespace window_manager {

// How many pixels should |unaccelerated_graphics_actor_| be offset from
// the upper-left corner of the screen?
static const int kUnacceleratedGraphicsActorOffsetPixels = 5;

// How long should we wait before hiding |unaccelerated_graphics_actor_|?
static const int kUnacceleratedGraphicsActorHideTimeoutMs = 15000;

// How quickly should we fade out |unaccelerated_graphics_actor_| when
// hiding it?
static const int kUnacceleratedGraphicsActorHideAnimMs = 500;

// How frequently should we send _NET_WM_PING messages to Chrome, and how
// long should we wait for a response to each before killing the process?
static const int kPingChromeFrequencyMs = 5000;
static const int kPingChromeTimeoutMs = 4000;
COMPILE_ASSERT(kPingChromeFrequencyMs > kPingChromeTimeoutMs,
               ping_timeout_is_greater_than_ping_frequency);

// Names of key binding actions that we register.
#ifndef NDEBUG
static const char* kToggleClientWindowDebuggingAction =
    "toggle-client-window-debugging";
static const char* kToggleDamageDebuggingAction = "toggle-damage-debugging";
static const char* kToggleProfilerAction = "toggle-profiler";
#endif

static const char* kTakeRootScreenshotAction = "take-root-screenshot";
static const char* kTakeWindowScreenshotAction = "take-window-screenshot";

const int WindowManager::kVideoTimePropertyUpdateSec = 5;

// Invoke |function_call| for each event consumer in |consumers| (a set).
#define FOR_EACH_EVENT_CONSUMER(consumers, function_call)                      \
  do {                                                                         \
    for (set<EventConsumer*>::iterator it =                                    \
         consumers.begin(); it != consumers.end(); ++it) {                     \
      (*it)->function_call;                                                    \
    }                                                                          \
  } while (0)

// Look up the event consumers that have registered interest in |key| in
// |consumer_map| (one of the WindowManager::*_event_consumers_ member
// variables), and invoke |function_call| (e.g.
// "HandleWindowPropertyChange(e.window, e.atom)") on each.  Helper macro
// used by WindowManager's event-handling methods.
//
// We iterate over a copy of the set of consumers so we won't crash if any of
// the handlers modify the set by registering or unregistering consumers.
#define FOR_EACH_INTERESTED_EVENT_CONSUMER(consumer_map, key, function_call)   \
  do {                                                                         \
    typeof(consumer_map.begin()) it = consumer_map.find(key);                  \
    if (it != consumer_map.end()) {                                            \
      set<EventConsumer*> ecs = it->second;                                    \
      for (set<EventConsumer*>::iterator ec_it = ecs.begin();                  \
           ec_it != ecs.end(); ++ec_it) {                                      \
        (*ec_it)->function_call;                                               \
      }                                                                        \
    }                                                                          \
  } while (0)

// Used by helper functions to generate |case| statements.
#define CASE_RETURN_LABEL(label) \
    case label: return #label

#undef DEBUG_EVENTS  // Turn this on if you want to debug events.
#ifdef DEBUG_EVENTS
static const char* XEventTypeToName(int type) {
  switch (type) {
    CASE_RETURN_LABEL(ButtonPress);
    CASE_RETURN_LABEL(ButtonRelease);
    CASE_RETURN_LABEL(CirculateNotify);
    CASE_RETURN_LABEL(CirculateRequest);
    CASE_RETURN_LABEL(ClientMessage);
    CASE_RETURN_LABEL(ColormapNotify);
    CASE_RETURN_LABEL(ConfigureNotify);
    CASE_RETURN_LABEL(ConfigureRequest);
    CASE_RETURN_LABEL(CreateNotify);
    CASE_RETURN_LABEL(DestroyNotify);
    CASE_RETURN_LABEL(EnterNotify);
    CASE_RETURN_LABEL(Expose);
    CASE_RETURN_LABEL(FocusIn);
    CASE_RETURN_LABEL(FocusOut);
    CASE_RETURN_LABEL(GraphicsExpose);
    CASE_RETURN_LABEL(GravityNotify);
    CASE_RETURN_LABEL(KeymapNotify);
    CASE_RETURN_LABEL(KeyPress);
    CASE_RETURN_LABEL(KeyRelease);
    CASE_RETURN_LABEL(LeaveNotify);
    CASE_RETURN_LABEL(MapNotify);
    CASE_RETURN_LABEL(MappingNotify);
    CASE_RETURN_LABEL(MapRequest);
    CASE_RETURN_LABEL(MotionNotify);
    CASE_RETURN_LABEL(NoExpose);
    CASE_RETURN_LABEL(PropertyNotify);
    CASE_RETURN_LABEL(ReparentNotify);
    CASE_RETURN_LABEL(ResizeRequest);
    CASE_RETURN_LABEL(SelectionClear);
    CASE_RETURN_LABEL(SelectionNotify);
    CASE_RETURN_LABEL(SelectionRequest);
    CASE_RETURN_LABEL(UnmapNotify);
    CASE_RETURN_LABEL(VisibilityNotify);
    default: return "Unknown";
  }
}
#endif

WindowManager::ScopedCompositingRequest::ScopedCompositingRequest(
    WindowManager* wm)
    : wm_(wm) {
  wm_->IncrementCompositingRequests();
}

WindowManager::ScopedCompositingRequest::~ScopedCompositingRequest() {
  wm_->DecrementCompositingRequests();
  wm_ = NULL;
}

WindowManager::WindowManager(EventLoop* event_loop,
                             XConnection* xconn,
                             Compositor* compositor,
                             DBusInterface* dbus)
    : event_loop_(event_loop),
      xconn_(xconn),
      compositor_(compositor),
      dbus_(dbus),
      root_(0),
      wm_xid_(0),
      stage_(NULL),
      stage_xid_(0),
      overlay_xid_(0),
      startup_pixmap_(0),
      mapped_xids_(new Stacker<XWindow>),
      stacked_xids_(new Stacker<XWindow>),
      damage_debugging_enabled_(false),
      active_window_xid_(0),
      query_keyboard_state_timeout_id_(-1),
      unredirected_fullscreen_xid_(0),
      disable_compositing_task_is_pending_(false),
      wm_ipc_version_(1),
      logged_in_(false),
      initialize_logging_(false),
      hide_unaccelerated_graphics_actor_timeout_id_(-1),
      chrome_watchdog_timeout_id_(-1),
      num_compositing_requests_(0) {
  CHECK(event_loop_);
  CHECK(xconn_);
  CHECK(compositor_);
}

WindowManager::~WindowManager() {
  if (startup_pixmap_)
    xconn_->FreePixmap(startup_pixmap_);

  event_loop_->RemoveTimeoutIfSet(&query_keyboard_state_timeout_id_);
  event_loop_->RemoveTimeoutIfSet(&chrome_watchdog_timeout_id_);
  event_loop_->RemoveTimeoutIfSet(
      &hide_unaccelerated_graphics_actor_timeout_id_);

  if (panel_manager_.get())
    panel_manager_->UnregisterAreaChangeListener(this);
  if (compositor_)
    compositor_->UnregisterCompositionChangeListener(this);
}

void WindowManager::HandlePanelManagerAreaChange() {
  SetEwmhWorkareaProperty();
}

void WindowManager::HandleTopFullscreenActorChange(
    const Compositor::TexturePixmapActor* top_fullscreen_actor) {
  const bool was_compositing = unredirected_fullscreen_xid_ == 0;
  bool should_composite = true;
  XWindow window_to_unredirect = 0;

  // If we're debugging damage events, trying to unredirect a window puts us in
  // a flickery loop:
  //
  // 10 We unredirect the browser window, causing a redraw.
  // 20 We display a debugging actor to visualize the redraw.
  // 30 We redirect the browser window since there's another actor onscreen.
  // 40 The debugging actor fades away, leaving us with only the browser window
  //    onscreen.
  // 50 GOTO 10
  const bool unredirection_permitted =
      FLAGS_unredirect_fullscreen_window &&
      !damage_debugging_enabled_ &&
      !num_compositing_requests_;

  if (unredirection_permitted && top_fullscreen_actor) {
    Window* win = GetWindowOwningActor(*top_fullscreen_actor);
    if (win != NULL &&
        win->client_x() == win->composited_x() &&
        win->client_y() == win->composited_y() &&
        win->composited_scale_x() == 1.0 &&
        win->composited_scale_y() == 1.0 &&
        // If we're waiting for the window to be repainted so we can fetch a
        // resized pixmap for it, we don't want to turn off compositing.
        win->client_has_redrawn_after_last_resize()) {
      window_to_unredirect = win->xid();
      should_composite = false;
    }
  }

  if (unredirected_fullscreen_xid_) {
    if (unredirected_fullscreen_xid_ == window_to_unredirect) {
      window_to_unredirect = 0;
    } else {
      Window* win = GetWindow(unredirected_fullscreen_xid_);
      if (win) {
        // Grab the server here to avoid a race condition between Chrome and
        // window manager that result in window been reset while Chrome is
        // writing into it.
        scoped_ptr<XConnection::ScopedServerGrab> grab(
            xconn_->CreateScopedServerGrab());
        xconn_->RedirectWindowForCompositing(unredirected_fullscreen_xid_);
        win->HandleRedirect();
      } else {
        DLOG(WARNING) << "The previously unredirected window with XID="
                      << unredirected_fullscreen_xid_ << " no longer exists";
      }
      unredirected_fullscreen_xid_ = 0;
      // Force the frame to draw when changing from one fullscreen actor to
      // another fullscreen actor in case X does not redraw the entire
      // screen and we get a partially updated frame.
    }
  }

  if (window_to_unredirect) {
    // Don't update should_draw_frame here; we want to draw the current frame
    // before we disable compositing.  The flag is updated in the
    // DisableCompositing callback, which does the actual disabling.
    unredirected_fullscreen_xid_ = window_to_unredirect;
    if (!disable_compositing_task_is_pending_) {
      event_loop_->PostTask(
          NewPermanentCallback(this, &WindowManager::DisableCompositing));
      disable_compositing_task_is_pending_ = true;
    }
  }

  if (!was_compositing && should_composite) {
    xconn_->ResetWindowBoundingRegionToDefault(overlay_xid_);
    DLOG(INFO) << "Turned compositing on";
    compositor_->set_should_draw_frame(true);
  }
}

bool WindowManager::Init() {
  CHECK(!root_) << "Init() may only be called once";
  root_ = xconn_->GetRootWindow();
//  xconn_->SelectRandREventsOnWindow(root_);
  xconn_->SelectInputOnWindow(root_, StructureNotifyMask, true);
  XConnection::WindowGeometry root_geometry;
  CHECK(xconn_->GetWindowGeometry(root_, &root_geometry));
  root_bounds_ = root_geometry.bounds;
  root_depth_ = root_geometry.depth;

  if (FLAGS_unredirect_fullscreen_window) {
    // Disable automatic background painting for the root window.  It
    // should never be visible, and the automatic updates cause flickering
    // when enabling and disabling compositing.
    xconn_->SetWindowBackgroundPixmap(root_, None);
  }

  // Create the atom cache first; RegisterExistence() needs it.
  atom_cache_.reset(new AtomCache(xconn_));

  CHECK(RegisterExistence());
  SetEwmhGeneralProperties();
  SetEwmhSizeProperties();

  // First make sure that we'll get notified if the login state changes
  // and then query its current value.
  CHECK(xconn_->SelectInputOnWindow(root_, PropertyChangeMask, true));
  int logged_in_value = 0;
  xconn_->GetIntProperty(
      root_, GetXAtom(ATOM_CHROME_LOGGED_IN), &logged_in_value);
  logged_in_ = logged_in_value;

  stage_ = compositor_->GetDefaultStage();
  stage_xid_ = stage_->GetStageXWindow();
  stage_->SetName("stage");
  stage_->SetSize(root_bounds_.width, root_bounds_.height);
  stage_->SetStageColor(Compositor::Color(FLAGS_background_color));
  stage_->Show();

  wm_ipc_.reset(new WmIpc(xconn_, atom_cache_.get()));

  stacking_manager_.reset(
      new StackingManager(xconn_, compositor_, atom_cache_.get()));
  focus_manager_.reset(new FocusManager(this));

  if (FLAGS_report_metrics) {
    metrics_library_.reset(new MetricsLibrary);
    metrics_library_->Init();
  }

  if (!logged_in_)
    CreateStartupBackground();

  // Draw the scene first to make sure that it's ready.
  compositor_->ForceDraw();
  CHECK(xconn_->RedirectSubwindowsForCompositing(root_));
  // Create the compositing overlay, put the stage's window inside of it,
  // and make events fall through both to the client windows underneath.
  overlay_xid_ = xconn_->GetCompositingOverlayWindow(root_);
  CHECK(overlay_xid_);
  LOG(INFO) << "Reparenting stage window " << XidStr(stage_xid_)
            << " into Xcomposite overlay window " << XidStr(overlay_xid_);
  CHECK(xconn_->ReparentWindow(stage_xid_, overlay_xid_, Point()));
  CHECK(xconn_->RemoveInputRegionFromWindow(overlay_xid_));
  CHECK(xconn_->RemoveInputRegionFromWindow(stage_xid_));

  if (!compositor_->TexturePixmapActorUsesFastPath() &&
      !FLAGS_unaccelerated_graphics_image.empty()) {
    unaccelerated_graphics_actor_.reset(
        compositor_->CreateImageFromFile(FLAGS_unaccelerated_graphics_image));
    unaccelerated_graphics_actor_->Move(
        kUnacceleratedGraphicsActorOffsetPixels,
        kUnacceleratedGraphicsActorOffsetPixels,
        0);
    unaccelerated_graphics_actor_->SetOpacity(1.f, 0);
    stage_->AddActor(unaccelerated_graphics_actor_.get());
    stacking_manager_->StackActorAtTopOfLayer(
        unaccelerated_graphics_actor_.get(), StackingManager::LAYER_DEBUGGING);
    unaccelerated_graphics_actor_->Show();
    hide_unaccelerated_graphics_actor_timeout_id_ =
        event_loop_->AddTimeout(
            NewPermanentCallback(
                this, &WindowManager::HideUnacceleratedGraphicsActor),
            kUnacceleratedGraphicsActorHideTimeoutMs, 0);
  }

  compositor_->RegisterCompositionChangeListener(this);

  key_bindings_.reset(new KeyBindings(xconn()));
  key_bindings_actions_.reset(
      new KeyBindingsActionRegistrar(key_bindings_.get()));
  RegisterKeyBindings();

  modality_handler_.reset(new ModalityHandler(this));
  event_consumers_.insert(modality_handler_.get());

  SetLoggedInState(logged_in_, true);  // initial=true

  screen_locker_handler_.reset(new ScreenLockerHandler(this));
  event_consumers_.insert(screen_locker_handler_.get());

  chrome_watchdog_.reset(new ChromeWatchdog(this));
  event_consumers_.insert(chrome_watchdog_.get());

#ifndef TOUCH_UI
  // Our X server has been modified to clear the cursor on the root window at
  // startup.  We set it back to the nice (Xcursor-loaded) arrow here so that
  // other windows won't inherit the empty cursor (but note that if we're not
  // logged in at this point, LoginController is also using XFixes to hide the
  // cursor until the user moves the pointer).
  XID cursor = xconn_->CreateShapedCursor(XC_left_ptr);
  xconn_->SetWindowCursor(root_, cursor);
  xconn_->FreeCursor(cursor);
#endif

  chrome_watchdog_timeout_id_ =
      event_loop_->AddTimeout(
          NewPermanentCallback(this, &WindowManager::PingChrome),
          kPingChromeFrequencyMs, kPingChromeFrequencyMs);

  // Select window management events before we look up existing windows --
  // we want to make sure that we eventually hear about any resizes that we
  // did while setting up existing windows so that the Window class will
  // know that it needs to fetch new redirected pixmaps for them.
  scoped_ptr<XConnection::ScopedServerGrab> grab(
      xconn_->CreateScopedServerGrab());
  CHECK(xconn_->SelectInputOnWindow(
            root_,
            SubstructureRedirectMask | SubstructureNotifyMask,
            true));  // preserve the existing event mask
  ManageExistingWindows();
  grab.reset();

  return true;
}

void WindowManager::SetLoggedInState(bool logged_in, bool initial) {
  if (!initial && logged_in_ && !logged_in) {
    LOG(WARNING) << "Ignoring request to transition from logged-in to "
                 << "not-logged-in state";
    return;
  }

  DLOG(INFO) << "User " << (logged_in ? "is" : "isn't") << " logged in";
  if (!initial && logged_in == logged_in_)
    return;

  logged_in_ = logged_in;

  if (initialize_logging_) {
    const string& log_dir = logged_in ?
        FLAGS_logged_in_log_dir :
        FLAGS_logged_out_log_dir;

    const string log_basename = StringPrintf(
        "%s.%s", WindowManager::GetWmName(),
        GetTimeAsString(GetCurrentTimeSec()).c_str());
    if (!file_util::CreateDirectory(FilePath(log_dir))) {
      LOG(ERROR) << "Unable to create logging directory " << log_dir;
    } else {
      SetUpLogSymlink(StringPrintf("%s/%s.LATEST",
                                   log_dir.c_str(),
                                   WindowManager::GetWmName()),
                      log_basename);
    }

    const string log_path = log_dir + "/" + log_basename;
    LOG(INFO) << "Switching to log " << log_path;
    logging::InitLogging(
        log_path.c_str(),
        logging::LOG_ONLY_TO_FILE,
        logging::DONT_LOCK_LOG_FILE,
        logging::APPEND_TO_OLD_LOG_FILE,
        logging::DISABLE_DCHECK_FOR_NON_OFFICIAL_RELEASE_BUILDS);
  }

  if (logged_in_) {
    DCHECK(!panel_manager_.get());
    panel_manager_.reset(new PanelManager(this));
    event_consumers_.insert(panel_manager_.get());
    panel_manager_->RegisterAreaChangeListener(this);
    HandlePanelManagerAreaChange();

    DCHECK(!layout_manager_.get());
    layout_manager_.reset(new LayoutManager(this, panel_manager_.get()));
    event_consumers_.insert(layout_manager_.get());

    // We've probably already dropped the background containing the initial
    // contents of the root window in response to the login background
    // window being shown, but it doesn't hurt to be sure here.
    DropStartupBackground();
  } else {
    DCHECK(!login_controller_.get());
    login_controller_.reset(new LoginController(this));
    event_consumers_.insert(login_controller_.get());
  }

  FOR_EACH_EVENT_CONSUMER(event_consumers_, HandleLoggedInStateChange());
}

void WindowManager::ProcessPendingEvents() {
  while (xconn_->IsEventPending()) {
    XEvent event;
    xconn_->GetNextEvent(&event);
    HandleEvent(&event);
  }
}

void WindowManager::HandleEvent(XEvent* event) {
  DCHECK(root_) << "Init() must be called before events can be handled";
#ifdef DEBUG_EVENTS
  if (event->type == xconn_->damage_event_base() + XDamageNotify) {
    DLOG(INFO) << "Got DAMAGE" << " event (" << event->type << ")";
  } else {
    DLOG(INFO) << "Got " << XEventTypeToName(event->type)
               << " event (" << event->type << ") in window manager.";
  }
#endif
  static int damage_notify = xconn_->damage_event_base() + XDamageNotify;
  static int shape_notify = xconn_->shape_event_base() + ShapeNotify;
//  static int randr_notify = xconn_->randr_event_base() + RRScreenChangeNotify;
  static int sync_alarm_notify = xconn_->sync_event_base() + XSyncAlarmNotify;

  switch (event->type) {
    case ButtonPress:
      HandleButtonPress(event->xbutton); break;
    case ButtonRelease:
      HandleButtonRelease(event->xbutton); break;
    case ClientMessage:
      HandleClientMessage(event->xclient); break;
    case ConfigureNotify:
      HandleConfigureNotify(event->xconfigure); break;
    case ConfigureRequest:
      HandleConfigureRequest(event->xconfigurerequest); break;
    case CreateNotify:
      HandleCreateNotify(event->xcreatewindow); break;
    case DestroyNotify:
      HandleDestroyNotify(event->xdestroywindow); break;
    case EnterNotify:
      HandleEnterNotify(event->xcrossing); break;
    case KeyPress:
      HandleKeyPress(event->xkey); break;
    case KeyRelease:
      HandleKeyRelease(event->xkey); break;
    case LeaveNotify:
      HandleLeaveNotify(event->xcrossing); break;
    case MapNotify:
      HandleMapNotify(event->xmap); break;
    case MapRequest:
      HandleMapRequest(event->xmaprequest); break;
    case MappingNotify:
      HandleMappingNotify(event->xmapping); break;
    case MotionNotify:
      HandleMotionNotify(event->xmotion); break;
    case PropertyNotify:
      HandlePropertyNotify(event->xproperty); break;
    case ReparentNotify:
      HandleReparentNotify(event->xreparent); break;
    case UnmapNotify:
      HandleUnmapNotify(event->xunmap); break;
    default:
      if (event->type == damage_notify) {
        HandleDamageNotify(*(reinterpret_cast<XDamageNotifyEvent*>(event)));
      } else if (event->type == sync_alarm_notify) {
        HandleSyncAlarmNotify(
            *(reinterpret_cast<XSyncAlarmNotifyEvent*>(event)));
      } else if (event->type == shape_notify) {
        HandleShapeNotify(*(reinterpret_cast<XShapeEvent*>(event)));
//      } else if (event->type == randr_notify) {
//        HandleRRScreenChangeNotify(
//            *(reinterpret_cast<XRRScreenChangeNotifyEvent*>(event)));
      }
  }
}

XWindow WindowManager::CreateInputWindow(const Rect& bounds, int event_mask) {
  XWindow xid = xconn_->CreateWindow(
      root_,  // parent
      bounds,
      true,   // override redirect
      true,   // input only
      event_mask,
      0);     // visual
  CHECK(xid);

  // Since the stage has been reparented into the overlay window, we need
  // to stack the input window under the overlay instead of under the stage
  // (because the stage isn't a sibling of the input window).
  CHECK(xconn_->StackWindow(xid, overlay_xid_, false));
  CHECK(xconn_->MapWindow(xid));
  return xid;
}

bool WindowManager::ConfigureInputWindow(XWindow xid, const Rect& bounds) {
  DLOG(INFO) << "Configuring input window " << XidStr(xid) << " to " << bounds;
  return xconn_->ConfigureWindow(xid, bounds);
}

XAtom WindowManager::GetXAtom(Atom atom) {
  return atom_cache_->GetXAtom(atom);
}

const string& WindowManager::GetXAtomName(XAtom xatom) {
  return atom_cache_->GetName(xatom);
}

XTime WindowManager::GetCurrentTimeFromServer() {
  // Just set a bogus property on our window and wait for the
  // PropertyNotify event so we can get its timestamp.
  CHECK(xconn_->SetIntProperty(
            wm_xid_,
            GetXAtom(ATOM_CHROME_GET_SERVER_TIME),    // atom
            XA_ATOM,                                  // type
            GetXAtom(ATOM_CHROME_GET_SERVER_TIME)));  // value
  XTime timestamp = 0;
  xconn_->WaitForPropertyChange(wm_xid_, &timestamp);
  return timestamp;
}

Window* WindowManager::GetWindow(XWindow xid) {
  return FindWithDefault(client_windows_, xid, shared_ptr<Window>()).get();
}

Window* WindowManager::GetWindowOrDie(XWindow xid) {
  Window* win = GetWindow(xid);
  CHECK(win) << "Unable to find window " << XidStr(xid);
  return win;
}

Window* WindowManager::GetWindowOwningActor(
    const Compositor::TexturePixmapActor& actor) {
  for (WindowMap::const_iterator it = client_windows_.begin();
       it != client_windows_.end(); it++) {
    if (it->second->actor() == &actor)
      return it->second.get();
  }
  return NULL;
}

void WindowManager::FocusWindow(Window* win, XTime timestamp) {
  focus_manager_->FocusWindow(win, timestamp);
}

bool WindowManager::IsModalWindowFocused() const {
  return modality_handler_->modal_window_is_focused();
}

void WindowManager::TakeFocus(XTime timestamp) {
  if (layout_manager_.get() && layout_manager_->TakeFocus(timestamp))
    return;
  if (panel_manager_.get() && panel_manager_->TakeFocus(timestamp))
    return;
  focus_manager_->FocusWindow(NULL, timestamp);
}

bool WindowManager::SetActiveWindowProperty(XWindow xid) {
  if (active_window_xid_ == xid)
    return true;

  DLOG(INFO) << "Setting active window to " << XidStr(xid);
  if (!xconn_->SetIntProperty(
          root_, GetXAtom(ATOM_NET_ACTIVE_WINDOW), XA_WINDOW, xid)) {
    return false;
  }
  active_window_xid_ = xid;
  return true;
}

bool WindowManager::SetNamePropertiesForXid(XWindow xid, const string& name) {
  bool success = xconn_->SetStringProperty(
      xid, atom_cache_->GetXAtom(ATOM_WM_NAME), name);
  success &= xconn_->SetStringProperty(
      xid, atom_cache_->GetXAtom(ATOM_NET_WM_NAME), name);
  return success;
}

bool WindowManager::SetVideoTimeProperty(time_t video_time) {
  // Rate-limit how often we'll update the property.
  TimeTicks now = GetMonotonicTime();
  if (video_property_update_time_.is_null() ||
      (now - video_property_update_time_) >
          TimeDelta::FromSeconds(kVideoTimePropertyUpdateSec)) {
    video_property_update_time_ = now;
    XAtom atom = atom_cache_->GetXAtom(ATOM_CHROME_VIDEO_TIME);
    return xconn_->SetIntProperty(
        root_, atom, atom, static_cast<int>(video_time));
  }
  return true;
}

void WindowManager::HandleWindowPixmapFetch(Window* win) {
  DCHECK(win);
  FOR_EACH_INTERESTED_EVENT_CONSUMER(
      window_event_consumers_, win->xid(), HandleWindowPixmapFetch(win));
}

void WindowManager::RegisterEventConsumerForWindowEvents(
    XWindow xid, EventConsumer* event_consumer) {
  DCHECK(event_consumer);
  if (!window_event_consumers_[xid].insert(event_consumer).second) {
    LOG(WARNING) << "Got request to register already-present window event "
                 << "consumer " << event_consumer << " for window "
                 << XidStr(xid);
  }
}

void WindowManager::UnregisterEventConsumerForWindowEvents(
    XWindow xid, EventConsumer* event_consumer) {
  DCHECK(event_consumer);
  WindowEventConsumerMap::iterator it = window_event_consumers_.find(xid);
  if (it == window_event_consumers_.end() ||
      it->second.erase(event_consumer) != 1) {
    LOG(WARNING) << "Got request to unregister not-registered window event "
                 << "consumer " << event_consumer << " for window "
                 << XidStr(xid);
  } else {
    if (it->second.empty())
      window_event_consumers_.erase(it);
  }
}

void WindowManager::RegisterEventConsumerForPropertyChanges(
    XWindow xid, XAtom xatom, EventConsumer* event_consumer) {
  DCHECK(event_consumer);
  if (!property_change_event_consumers_[make_pair(xid, xatom)].insert(
          event_consumer).second) {
    LOG(WARNING) << "Got request to register already-present window property "
                 << "listener " << event_consumer << " for window "
                 << XidStr(xid) << " and atom " << XidStr(xatom) << " ("
                 << GetXAtomName(xatom) << ")";
  }
}

void WindowManager::UnregisterEventConsumerForPropertyChanges(
    XWindow xid, XAtom xatom, EventConsumer* event_consumer) {
  DCHECK(event_consumer);
  PropertyChangeEventConsumerMap::iterator it =
      property_change_event_consumers_.find(make_pair(xid, xatom));
  if (it == property_change_event_consumers_.end() ||
      it->second.erase(event_consumer) != 1) {
    LOG(WARNING) << "Got request to unregister not-registered window property "
                 << "listener " << event_consumer << " for window "
                 << XidStr(xid) << " and atom " << XidStr(xatom) << " ("
                 << GetXAtomName(xatom) << ")";
  } else {
    if (it->second.empty())
      property_change_event_consumers_.erase(it);
  }
}

void WindowManager::RegisterEventConsumerForChromeMessages(
    WmIpcMessageType message_type, EventConsumer* event_consumer) {
  DCHECK(event_consumer);
  if (!chrome_message_event_consumers_[message_type].insert(
          event_consumer).second) {
    LOG(WARNING) << "Got request to register already-present Chrome message "
                 << "event consumer " << event_consumer << " for message type "
                 << message_type;
  }
}

void WindowManager::UnregisterEventConsumerForChromeMessages(
    WmIpcMessageType message_type, EventConsumer* event_consumer) {
  DCHECK(event_consumer);
  ChromeMessageEventConsumerMap::iterator it =
      chrome_message_event_consumers_.find(message_type);
  if (it == chrome_message_event_consumers_.end() ||
      it->second.erase(event_consumer) != 1) {
    LOG(WARNING) << "Got request to unregister not-registered Chrome message "
                 << "event consumer " << event_consumer << " for message type "
                 << message_type;
  } else {
    if (it->second.empty())
      chrome_message_event_consumers_.erase(it);
  }
}

void WindowManager::RegisterEventConsumerForDestroyedWindow(
    XWindow xid, EventConsumer* event_consumer) {
  DCHECK(xid);
  DCHECK(event_consumer);
  CHECK(destroyed_window_event_consumers_.insert(
            make_pair(xid, event_consumer)).second)
      << "Another EventConsumer already requested ownership of window "
      << XidStr(xid) << " after it gets destroyed";
}

void WindowManager::UnregisterEventConsumerForDestroyedWindow(
    XWindow xid, EventConsumer* event_consumer) {
  hash_map<XWindow, EventConsumer*>::iterator it =
      destroyed_window_event_consumers_.find(xid);
  CHECK(it != destroyed_window_event_consumers_.end())
      << "No event consumer requested ownership of window " << XidStr(xid)
      << " after it gets destroyed, but got a request to unregister "
      << event_consumer;
  CHECK(it->second == event_consumer)
      << "Event consumer " << it->second << " requested ownership of window "
      << XidStr(xid) << " after it gets destroyed, but got a request to "
      << "unregister " << event_consumer;
  destroyed_window_event_consumers_.erase(it);
}

void WindowManager::RegisterSyncAlarm(XID alarm_id, Window* win) {
  base::hash_map<XID, Window*>::const_iterator it =
      sync_alarms_to_windows_.find(alarm_id);
  DCHECK(it == sync_alarms_to_windows_.end())
      << "Registering sync alarm " << XidStr(alarm_id) << " for window "
      << win->xid_str() << " while it's already being used by "
      << it->second->xid_str();
  sync_alarms_to_windows_[alarm_id] = win;
}

void WindowManager::UnregisterSyncAlarm(XID alarm_id) {
  size_t num_erased = sync_alarms_to_windows_.erase(alarm_id);
  DCHECK_EQ(num_erased, static_cast<size_t>(1))
      << "Tried to unregister unknown sync alarm " << XidStr(alarm_id);
}

void WindowManager::ToggleClientWindowDebugging() {
  if (client_window_debugging_enabled()) {
    client_window_debugging_actors_.clear();
    return;
  }
  UpdateClientWindowDebugging();
}

void WindowManager::ToggleDamageDebugging() {
  damage_debugging_enabled_ = !damage_debugging_enabled_;
}

void WindowManager::ToggleProfiler() {
#if defined(PROFILE_BUILD)
  Profiler* profiler = Singleton<Profiler>::get();
  if (profiler->status() == Profiler::STATUS_RUN) {
    profiler->Pause();
  } else if (profiler->status() == Profiler::STATUS_SUSPEND) {
    profiler->Resume();
  }
#endif
}

void WindowManager::UpdateClientWindowDebugging() {
  DLOG(INFO) << "Compositing actors:\n" << stage_->GetDebugString(0);

  ActorVector new_actors;

  vector<XWindow> xids;
  if (!xconn_->GetChildWindows(root_, &xids))
    return;

  static const int kDebugFadeMs = 100;

  int cnt = 0;
  float step = 6.f / xids.size();
  for (vector<XWindow>::iterator it = xids.begin(); it != xids.end(); ++it) {
    Compositor::Color bg_color;
    bg_color.SetHsv(cnt++ * step, 1.f, 1.f);
    XConnection::WindowGeometry geometry;
    if (!xconn_->GetWindowGeometry(*it, &geometry))
      continue;

    Compositor::ColoredBoxActor* rect =
        compositor_->CreateColoredBox(
            geometry.bounds.width, geometry.bounds.height, bg_color);
    stage_->AddActor(rect);
    stacking_manager_->StackActorAtTopOfLayer(
        rect, StackingManager::LAYER_DEBUGGING);
    rect->SetName("debug box");
    rect->Move(geometry.bounds.x, geometry.bounds.y, 0);
    rect->SetOpacity(0, 0);
    rect->SetOpacity(0.3, kDebugFadeMs);
    rect->Show();

    new_actors.push_back(shared_ptr<Compositor::Actor>(rect));
  }

  client_window_debugging_actors_.swap(new_actors);
}

void WindowManager::DropStartupBackground() {
  if (startup_background_.get()) {
    startup_background_.reset();
    xconn_->FreePixmap(startup_pixmap_);
    startup_pixmap_ = 0;
  }
}

int WindowManager::GetNumWindows() const {
  int num_windows = 0;
  if (panel_manager_.get())
    num_windows += panel_manager_->num_panels();
  if (layout_manager_.get())
    num_windows += layout_manager_->num_toplevels();
  return num_windows;
}

void WindowManager::DestroyLoginController() {
  event_loop_->PostTask(
      NewPermanentCallback(
          this, &WindowManager::DestroyLoginControllerInternal));
}

void WindowManager::ReportUserAction(const string& action) {
  if (metrics_library_.get())
    metrics_library_->SendUserActionToUMA(action);
}

void WindowManager::IncrementCompositingRequests() {
  num_compositing_requests_++;
  if (num_compositing_requests_ == 1 && unredirected_fullscreen_xid_)
    compositor_->ForceDraw();
}

void WindowManager::DecrementCompositingRequests() {
  DCHECK_GE(num_compositing_requests_, 1);
  num_compositing_requests_--;
}

WindowManager::ScopedCompositingRequest*
WindowManager::CreateScopedCompositingRequest() {
  return new ScopedCompositingRequest(this);
}

bool WindowManager::IsSessionEnding() const {
  if (!screen_locker_handler_.get())
    return false;
  return screen_locker_handler_->session_ending();
}

bool WindowManager::GetManagerSelection(
    XAtom atom, XWindow manager_win, XTime timestamp) {
  // Find the current owner of the selection and select events on it so
  // we'll know when it's gone away.
  XWindow current_manager = xconn_->GetSelectionOwner(atom);
  if (current_manager)
    xconn_->SelectInputOnWindow(current_manager, StructureNotifyMask, false);

  // Take ownership of the selection.
  CHECK(xconn_->SetSelectionOwner(atom, manager_win, timestamp));
  if (xconn_->GetSelectionOwner(atom) != manager_win) {
    LOG(WARNING) << "Couldn't take ownership of "
                 << GetXAtomName(atom) << " selection";
    return false;
  }

  // Announce that we're here.
  long data[5];
  memset(data, 0, sizeof(data));
  data[0] = timestamp;
  data[1] = atom;
  data[2] = manager_win;
  CHECK(xconn_->SendClientMessageEvent(
            root_, root_, GetXAtom(ATOM_MANAGER), data, StructureNotifyMask));

  // If there was an old manager running, wait for its window to go away.
  if (current_manager)
    CHECK(xconn_->WaitForWindowToBeDestroyed(current_manager));

  return true;
}

bool WindowManager::RegisterExistence() {
  // Create an offscreen window to take ownership of the selection and
  // receive properties.
  wm_xid_ = xconn_->CreateWindow(root_,   // parent
                                 Rect(-1, -1, 1, 1),
                                 true,    // override redirect
                                 false,   // input only
                                 PropertyChangeMask,  // event mask
                                 0);      // visual
  CHECK(wm_xid_);
  LOG(INFO) << "Created window " << XidStr(wm_xid_)
            << " for registering ourselves as the window manager";
  wm_xid_destroyer_.reset(new XConnection::WindowDestroyer(xconn_, wm_xid_));

  // Set the window's title and wait for the notify event so we can get a
  // timestamp from the server.
  CHECK(SetNamePropertiesForXid(wm_xid_, GetWmName()));
  XTime timestamp = 0;
  xconn_->WaitForPropertyChange(wm_xid_, &timestamp);

  if (!GetManagerSelection(GetXAtom(ATOM_WM_S0), wm_xid_, timestamp) ||
      !GetManagerSelection(GetXAtom(ATOM_NET_WM_CM_S0), wm_xid_, timestamp)) {
    return false;
  }

  return true;
}

bool WindowManager::SetEwmhGeneralProperties() {
  bool success = true;

  success &= xconn_->SetIntProperty(
      root_, GetXAtom(ATOM_NET_NUMBER_OF_DESKTOPS), XA_CARDINAL, 1);
  success &= xconn_->SetIntProperty(
      root_, GetXAtom(ATOM_NET_CURRENT_DESKTOP), XA_CARDINAL, 0);

  // Let clients know that we're the current WM and that we at least
  // partially conform to EWMH.
  XAtom check_atom = GetXAtom(ATOM_NET_SUPPORTING_WM_CHECK);
  success &= xconn_->SetIntProperty(root_, check_atom, XA_WINDOW, wm_xid_);
  success &= xconn_->SetIntProperty(wm_xid_, check_atom, XA_WINDOW, wm_xid_);

  // State which parts of EWMH we support.
  vector<int> supported;
  supported.push_back(GetXAtom(ATOM_NET_ACTIVE_WINDOW));
  supported.push_back(GetXAtom(ATOM_NET_CLIENT_LIST));
  supported.push_back(GetXAtom(ATOM_NET_CLIENT_LIST_STACKING));
  supported.push_back(GetXAtom(ATOM_NET_CURRENT_DESKTOP));
  supported.push_back(GetXAtom(ATOM_NET_DESKTOP_GEOMETRY));
  supported.push_back(GetXAtom(ATOM_NET_DESKTOP_VIEWPORT));
  supported.push_back(GetXAtom(ATOM_NET_NUMBER_OF_DESKTOPS));
  supported.push_back(GetXAtom(ATOM_NET_WM_MOVERESIZE));
  supported.push_back(GetXAtom(ATOM_NET_WM_NAME));
  supported.push_back(GetXAtom(ATOM_NET_WM_STATE));
  supported.push_back(GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN));
  supported.push_back(GetXAtom(ATOM_NET_WM_STATE_MODAL));
  supported.push_back(GetXAtom(ATOM_NET_WM_SYNC_REQUEST));
  supported.push_back(GetXAtom(ATOM_NET_WM_SYNC_REQUEST_COUNTER));
  supported.push_back(GetXAtom(ATOM_NET_WM_WINDOW_OPACITY));
  supported.push_back(GetXAtom(ATOM_NET_WORKAREA));
  success &= xconn_->SetIntArrayProperty(
      root_, GetXAtom(ATOM_NET_SUPPORTED), XA_ATOM, supported);

  return success;
}

bool WindowManager::SetEwmhSizeProperties() {
  bool success = true;

  // We don't use pseudo-large desktops, so this is just the screen size.
  vector<int> geometry;
  geometry.push_back(root_bounds_.width);
  geometry.push_back(root_bounds_.height);
  success &= xconn_->SetIntArrayProperty(
      root_, GetXAtom(ATOM_NET_DESKTOP_GEOMETRY), XA_CARDINAL, geometry);

  // The viewport (top-left corner of the desktop) is just (0, 0) for us.
  vector<int> viewport(2, 0);
  success &= xconn_->SetIntArrayProperty(
      root_, GetXAtom(ATOM_NET_DESKTOP_VIEWPORT), XA_CARDINAL, viewport);

  success &= SetEwmhWorkareaProperty();

  return success;
}

bool WindowManager::SetEwmhWorkareaProperty() {
  // _NET_WORKAREA describes the region of the screen "minus space occupied
  // by dock and panel windows", so we subtract out the space used by panel
  // docks. :-P  Chrome can use this to guess the initial size for new
  // windows.
  int panel_manager_left_width = 0, panel_manager_right_width = 0;
  if (panel_manager_.get()) {
    // We invoke this before the panel manager has been created to try to
    // get the hints set as soon as possible.
    panel_manager_->GetArea(&panel_manager_left_width,
                            &panel_manager_right_width);
  }
  vector<int> workarea;
  workarea.push_back(panel_manager_left_width);  // x
  workarea.push_back(0);  // y
  workarea.push_back(
      root_bounds_.width -
      panel_manager_left_width -
      panel_manager_right_width);
  workarea.push_back(root_bounds_.height);
  return xconn_->SetIntArrayProperty(
      root_, GetXAtom(ATOM_NET_WORKAREA), XA_CARDINAL, workarea);
}

void WindowManager::RegisterKeyBindings() {
#ifndef NDEBUG
  static const uint32_t kCtrlAltShiftMask =
      KeyBindings::kControlMask |
      KeyBindings::kAltMask |
      KeyBindings::kShiftMask;

  key_bindings_actions_->AddAction(
      kToggleClientWindowDebuggingAction,
      NewPermanentCallback(this, &WindowManager::ToggleClientWindowDebugging),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_w, kCtrlAltShiftMask),
      kToggleClientWindowDebuggingAction);

  key_bindings_actions_->AddAction(
      kToggleDamageDebuggingAction,
      NewPermanentCallback(this, &WindowManager::ToggleDamageDebugging),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_d, kCtrlAltShiftMask),
      kToggleDamageDebuggingAction);

  key_bindings_actions_->AddAction(
      kToggleProfilerAction,
      NewPermanentCallback(this, &WindowManager::ToggleProfiler),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_p, kCtrlAltShiftMask),
      kToggleProfilerAction);
#endif

  key_bindings_actions_->AddAction(
      kTakeRootScreenshotAction,
      NewPermanentCallback(this, &WindowManager::TakeScreenshot, false),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_F5, KeyBindings::kControlMask),
      kTakeRootScreenshotAction);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_Print, 0), kTakeRootScreenshotAction);

  key_bindings_actions_->AddAction(
      kTakeWindowScreenshotAction,
      NewPermanentCallback(this, &WindowManager::TakeScreenshot, true),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_Print, KeyBindings::kShiftMask),
      kTakeWindowScreenshotAction);
}

bool WindowManager::ManageExistingWindows() {
  vector<XWindow> windows;
  if (!xconn_->GetChildWindows(root_, &windows))
    return false;

  // Snapshot and panel content windows that are already mapped.  We defer
  // calling HandleMappedWindow() on these until we've handled all other
  // windows to make sure that we handle the corresponding panel titlebar
  // windows and Chrome toplevel windows first -- the panel code requires
  // that the titlebars be mapped before the content windows, and the
  // snapshot code requires that the toplevel windows are mapped before
  // their snapshots.
  vector<Window*> first_deferred_mapped_windows;
  vector<Window*> second_deferred_mapped_windows;

  LOG(INFO) << "Taking ownership of " << windows.size() << " window"
            << (windows.size() == 1 ? "" : "s");
  for (size_t i = 0; i < windows.size(); ++i) {
    XWindow xid = windows[i];
    XConnection::WindowAttributes attr;
    XConnection::WindowGeometry geometry;
    if (!xconn_->GetWindowAttributes(xid, &attr) ||
        !xconn_->GetWindowGeometry(xid, &geometry))
      continue;

    // XQueryTree() returns child windows in bottom-to-top stacking order.
    stacked_xids_->AddOnTop(xid);
    Window* win = TrackWindow(xid, attr.override_redirect, geometry);
    if (win && win->FetchMapState()) {
      if (win->type() == chromeos::WM_IPC_WINDOW_CHROME_PANEL_CONTENT ||
          win->type() == chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT) {
        first_deferred_mapped_windows.push_back(win);
      } else if (win->type() == chromeos::WM_IPC_WINDOW_CHROME_TAB_TITLE ||
                 win->type() == chromeos::WM_IPC_WINDOW_CHROME_TAB_FAV_ICON) {
        second_deferred_mapped_windows.push_back(win);
      } else {
        HandleMappedWindow(win);
      }
    }
  }

  for (vector<Window*>::iterator it = first_deferred_mapped_windows.begin();
       it != first_deferred_mapped_windows.end(); ++it) {
    HandleMappedWindow(*it);
  }

  for (vector<Window*>::iterator it = second_deferred_mapped_windows.begin();
       it != second_deferred_mapped_windows.end(); ++it) {
    HandleMappedWindow(*it);
  }

  UpdateClientListStackingProperty();
  return true;
}

Window* WindowManager::TrackWindow(XWindow xid,
                                   bool override_redirect,
                                   XConnection::WindowGeometry& geometry) {
  // Don't manage our internal windows.
  if (IsInternalWindow(xid) || stacking_manager_->IsInternalWindow(xid))
    return NULL;
  for (set<EventConsumer*>::const_iterator it = event_consumers_.begin();
       it != event_consumers_.end(); ++it) {
    if ((*it)->IsInputWindow(xid))
      return NULL;
  }

  // We don't care about InputOnly windows either.
  // TODO: Don't call GetWindowAttributes() so many times; we call in it
  // Window's c'tor as well.
  XConnection::WindowAttributes attr;
  if (xconn_->GetWindowAttributes(xid, &attr) &&
      attr.window_class ==
        XConnection::WindowAttributes::WINDOW_CLASS_INPUT_ONLY)
    return NULL;

  DLOG(INFO) << "Managing window " << XidStr(xid);
  Window* win = GetWindow(xid);
  if (win) {
    LOG(WARNING) << "Window " << XidStr(xid) << " is already being managed";
  } else {
    shared_ptr<Window> win_ref(
        new Window(this, xid, override_redirect, geometry));
    client_windows_.insert(make_pair(xid, win_ref));
    win = win_ref.get();
  }
  return win;
}

void WindowManager::HandleMappedWindow(Window* win) {
  CHECK(win);
  CHECK(!win->mapped()) << "Window " << win->xid_str() << " is already mapped";

  win->HandleMapNotify();
  FOR_EACH_EVENT_CONSUMER(event_consumers_, HandleWindowMap(win));

  if (win->override_redirect()) {
    if (!win->is_rgba()) {
      // Check if this window has a menu hint; if so, display a shadow under it.
      const static XAtom combo_xatom = GetXAtom(ATOM_NET_WM_WINDOW_TYPE_COMBO);
      const static XAtom dropdown_xatom =
          GetXAtom(ATOM_NET_WM_WINDOW_TYPE_DROPDOWN_MENU);
      const static XAtom menu_xatom = GetXAtom(ATOM_NET_WM_WINDOW_TYPE_MENU);
      const static XAtom popup_xatom =
          GetXAtom(ATOM_NET_WM_WINDOW_TYPE_POPUP_MENU);
      for (vector<XAtom>::const_iterator it =
             win->wm_window_type_xatoms().begin();
           it != win->wm_window_type_xatoms().end(); ++it) {
        if (*it == combo_xatom || *it == dropdown_xatom || *it == menu_xatom ||
            *it == popup_xatom) {
          win->SetShadowType(Shadow::TYPE_RECTANGULAR);
          break;
        }
      }
    }
    return;
  }

  if (mapped_xids_->Contains(win->xid())) {
    LOG(WARNING) << "Handling mapped window " << win->xid_str()
                 << ", which is already listed in |mapped_xids_|";
  } else {
    mapped_xids_->AddOnTop(win->xid());
    UpdateClientListProperty();
    // This only includes mapped windows, so we need to update it now.
    UpdateClientListStackingProperty();
  }

  SetWmStateProperty(win->xid(), 1);  // NormalState
}

void WindowManager::HandleUnmappedWindow(Window* win) {
  CHECK(win);
  CHECK(win->mapped()) << "Window " << win->xid_str() << " isn't mapped";

  win->HandleUnmapNotify();
  FOR_EACH_EVENT_CONSUMER(event_consumers_, HandleWindowUnmap(win));

  // Notify the focus manager last in case any event consumers need to do
  // something special when they see the focused window getting unmapped.
  focus_manager_->HandleWindowUnmap(win);

  if (!win->override_redirect()) {
    SetWmStateProperty(win->xid(), 0);  // WithdrawnState

    if (mapped_xids_->Contains(win->xid())) {
      mapped_xids_->Remove(win->xid());
      UpdateClientListProperty();
      UpdateClientListStackingProperty();
    }
  }
}

void WindowManager::HandleScreenResize(const Size& new_size) {
  // We move windows offscreen to prevent them from receiving input.
  // Check that the screen doesn't get so huge that they end up onscreen.
  DCHECK_LE(new_size.width, Window::kOffscreenX);
  DCHECK_LE(new_size.height, Window::kOffscreenY);

  // The window manager sometimes tries to fetch an updated
  // pixmap for a resized window while the window is unredirected, resulting
  // in a black screen until compositing is toggled on and off again.
  // To avoid this, disable compositing while resizing.
  scoped_ptr<ScopedCompositingRequest> comp_request(
      CreateScopedCompositingRequest());

  root_bounds_.resize(new_size, GRAVITY_NORTHWEST);
  SetEwmhSizeProperties();
  stage_->SetSize(root_bounds_.width, root_bounds_.height);
  FOR_EACH_EVENT_CONSUMER(event_consumers_, HandleScreenResize());
}

bool WindowManager::SetWmStateProperty(XWindow xid, int state) {
  vector<int> values;
  values.push_back(state);
  values.push_back(0);  // we don't use icons
  XAtom xatom = GetXAtom(ATOM_WM_STATE);
  return xconn_->SetIntArrayProperty(xid, xatom, xatom, values);
}

bool WindowManager::UpdateClientListProperty() {
  vector<int> values;
  const list<XWindow>& xids = mapped_xids_->items();
  // We store windows in most-to-least-recently-mapped order, but
  // _NET_CLIENT_LIST is least-to-most-recently-mapped.
  for (list<XWindow>::const_reverse_iterator it = xids.rbegin();
       it != xids.rend(); ++it) {
    const Window* win = GetWindow(*it);
    if (!win || !win->mapped() || win->override_redirect()) {
      LOG(WARNING) << "Skipping "
                   << (!win ? "missing" :
                       (!win->mapped() ? "unmapped" : "override-redirect"))
                   << " window " << XidStr(*it)
                   << " when updating _NET_CLIENT_LIST";
    } else {
      values.push_back(*it);
    }
  }
  if (!values.empty()) {
    return xconn_->SetIntArrayProperty(
        root_, GetXAtom(ATOM_NET_CLIENT_LIST), XA_WINDOW, values);
  } else {
    return xconn_->DeletePropertyIfExists(
        root_, GetXAtom(ATOM_NET_CLIENT_LIST));
  }
}

bool WindowManager::UpdateClientListStackingProperty() {
  vector<int> values;
  const list<XWindow>& xids = stacked_xids_->items();
  // We store windows in top-to-bottom stacking order, but
  // _NET_CLIENT_LIST_STACKING is bottom-to-top.
  for (list<XWindow>::const_reverse_iterator it = xids.rbegin();
       it != xids.rend(); ++it) {
    const Window* win = GetWindow(*it);
    if (win && win->mapped() && !win->override_redirect())
      values.push_back(*it);
  }
  if (!values.empty()) {
    return xconn_->SetIntArrayProperty(
        root_, GetXAtom(ATOM_NET_CLIENT_LIST_STACKING), XA_WINDOW, values);
  } else {
    return xconn_->DeletePropertyIfExists(
        root_, GetXAtom(ATOM_NET_CLIENT_LIST_STACKING));
  }
}

void WindowManager::HandleButtonPress(const XButtonEvent& e) {
  DLOG(INFO) << "Handling button press in window " << XidStr(e.window)
             << " at relative (" << e.x << ", " << e.y << "), absolute ("
             << e.x_root << ", " << e.y_root << ") with button " << e.button;
  Window* win = GetWindow(e.window);
  if (win)
    focus_manager_->HandleButtonPressInWindow(win, e.time);

  const Point relative_pos(e.x, e.y);
  const Point absolute_pos(e.x_root, e.y_root);
  FOR_EACH_INTERESTED_EVENT_CONSUMER(
      window_event_consumers_,
      e.window,
      HandleButtonPress(
          e.window, relative_pos, absolute_pos, e.button, e.time));
}

void WindowManager::HandleButtonRelease(const XButtonEvent& e) {
  DLOG(INFO) << "Handling button release in window " << XidStr(e.window)
             << " at relative (" << e.x << ", " << e.y << "), absolute ("
             << e.x_root << ", " << e.y_root << ") with button " << e.button;

  const Point relative_pos(e.x, e.y);
  const Point absolute_pos(e.x_root, e.y_root);
  FOR_EACH_INTERESTED_EVENT_CONSUMER(
      window_event_consumers_,
      e.window,
      HandleButtonRelease(
          e.window, relative_pos, absolute_pos, e.button, e.time));
}

void WindowManager::HandleClientMessage(const XClientMessageEvent& e) {
  // _NET_WM_PING responses are spammy; don't log them.
  if (!(e.message_type == GetXAtom(ATOM_WM_PROTOCOLS) &&
        e.format == XConnection::kLongFormat &&
        static_cast<XAtom>(e.data.l[0]) == GetXAtom(ATOM_NET_WM_PING))) {
    DLOG(INFO) << "Handling client message for window " << XidStr(e.window)
               << " with type " << XidStr(e.message_type) << " ("
               << GetXAtomName(e.message_type) << ") and format " << e.format;
  }

  WmIpc::Message msg;
  if (wm_ipc_->GetMessage(e.window, e.message_type, e.format, e.data.l, &msg)) {
    if (msg.type() == chromeos::WM_IPC_MESSAGE_WM_NOTIFY_IPC_VERSION) {
      wm_ipc_version_ = msg.param(0);
      LOG(INFO) << "Got WM_NOTIFY_IPC_VERSION message saying that Chrome is "
                << "using version " << wm_ipc_version_;
    } else {
      DLOG(INFO) << "Decoded " << chromeos::WmIpcMessageTypeToString(msg.type())
                 << " message";
      FOR_EACH_INTERESTED_EVENT_CONSUMER(chrome_message_event_consumers_,
                                         msg.type(),
                                         HandleChromeMessage(msg));
    }
  } else {
    if (static_cast<XAtom>(e.message_type) == GetXAtom(ATOM_MANAGER) &&
        e.format == XConnection::kLongFormat &&
        (static_cast<XAtom>(e.data.l[1]) == GetXAtom(ATOM_WM_S0) ||
         static_cast<XAtom>(e.data.l[1]) == GetXAtom(ATOM_NET_WM_CM_S0))) {
      if (static_cast<XWindow>(e.data.l[2]) != wm_xid_) {
        LOG(WARNING) << "Ignoring client message saying that window "
                     << XidStr(e.data.l[2]) << " got the "
                     << GetXAtomName(e.data.l[1]) << " manager selection";
      }
      return;
    }
    if (e.format == XConnection::kLongFormat) {
      FOR_EACH_INTERESTED_EVENT_CONSUMER(
          window_event_consumers_,
          e.window,
          HandleClientMessage(e.window, e.message_type, e.data.l));
    } else {
      LOG(WARNING) << "Ignoring client message event with unsupported format "
                   << e.format << " (we only handle 32-bit data currently)";
    }
  }
}

void WindowManager::HandleConfigureNotify(const XConfigureEvent& e) {
  const Rect bounds(e.x, e.y, e.width, e.height);

  if (e.window == root_ && bounds.size() != root_bounds_.size()) {
    DLOG(INFO) << "Got configure notify saying that root window has been "
               << "resized to " << bounds.size();
    HandleScreenResize(bounds.size());
    return;
  }

  // Even though _NET_CLIENT_LIST_STACKING only contains client windows
  // that we're managing, we also need to keep track of other (e.g.
  // override-redirect, or even untracked) windows: we receive
  // notifications of the form "X is on top of Y", so we need to know where
  // Y is even if we're not managing it so that we can stack X correctly.

  // If this isn't an immediate child of the root window, ignore the
  // ConfigureNotify.
  if (!stacked_xids_->Contains(e.window))
    return;

  // Did the window get restacked from its previous position in
  // |stacked_xids_|?
  bool restacked = false;

  // Check whether the stacking order changed.
  const XWindow* prev_above = stacked_xids_->GetUnder(e.window);
  // If we're on the bottom but weren't previously, or aren't on the bottom
  // now but previously were or are now above a different sibling, update
  // the order.
  if ((!e.above && prev_above != NULL) ||
      (e.above && (prev_above == NULL || *prev_above != e.above))) {
    restacked = true;
    stacked_xids_->Remove(e.window);

    if (e.above && stacked_xids_->Contains(e.above)) {
      stacked_xids_->AddAbove(e.window, e.above);
    } else {
      // |above| being unset means that the window is stacked beneath its
      // siblings.
      if (e.above) {
        LOG(WARNING) << "ConfigureNotify for " << XidStr(e.window)
                     << " said that it's stacked above " << XidStr(e.above)
                     << ", which we don't know about";
      }
      stacked_xids_->AddOnBottom(e.window);
    }
  }

  Window* win = GetWindow(e.window);
  if (!win)
    return;
  DLOG(INFO) << "Handling configure notify for " << win->xid_str()
             << " to " << bounds << ", above " << XidStr(e.above);

  // Notify the window so it can reset its pixmap if the size changed, or update
  // its actor's position and stacking if it's an override-redirect window.
  win->HandleConfigureNotify(bounds, e.above);

  if (!win->override_redirect() && restacked) {
    // _NET_CLIENT_LIST_STACKING only includes managed (i.e.
    // non-override-redirect) windows, so we only update it when a
    // managed window's stacking position changed.
    UpdateClientListStackingProperty();
  }
}

void WindowManager::HandleConfigureRequest(const XConfigureRequestEvent& e) {
  Window* win = GetWindow(e.window);
  if (!win)
    return;

  DLOG(INFO)
      << "Handling configure request for " << XidStr(e.window)
      << " to pos ("
      << ((e.value_mask & CWX) ? StringPrintf("%d", e.x) : string("undef"))
      << ", "
      << ((e.value_mask & CWY) ? StringPrintf("%d", e.y) : string("undef"))
      << ") and size "
      << ((e.value_mask & CWWidth) ?
          StringPrintf("%d", e.width) : string("undef "))
      << "x"
      << ((e.value_mask & CWHeight) ?
          StringPrintf("%d", e.height) : string(" undef"));
  if (win->override_redirect()) {
    LOG(WARNING) << "Huh?  Got a ConfigureRequest event for override-redirect "
                 << "window " << win->xid_str();
  }

  const Rect requested_bounds(
      (e.value_mask & CWX) ? e.x : win->client_x(),
      (e.value_mask & CWY) ? e.y : win->client_y(),
      (e.value_mask & CWWidth) ? e.width : win->client_width(),
      (e.value_mask & CWHeight) ? e.height : win->client_height());

  // The X server should reject bogus requests before they get to us, but
  // just in case...
  if (requested_bounds.width <= 0 || requested_bounds.height <= 0) {
    LOG(WARNING) << "Ignoring request to resize window " << win->xid_str()
                 << " to " << requested_bounds.size();
    return;
  }

  if (!win->mapped()) {
    // If the window is unmapped, it's unlikely that any event consumers
    // will know what to do with it.  Do whatever we were asked to do.
    if (requested_bounds.position() != win->client_origin()) {
      win->MoveClient(requested_bounds.x, requested_bounds.y);
      win->MoveComposited(requested_bounds.x, requested_bounds.y, 0);
    }
    if (requested_bounds.size() != win->client_size())
      win->Resize(requested_bounds.size(), GRAVITY_NORTHWEST);
  } else {
    FOR_EACH_INTERESTED_EVENT_CONSUMER(
        window_event_consumers_,
        e.window,
        HandleWindowConfigureRequest(win, requested_bounds));
  }
}

void WindowManager::HandleCreateNotify(const XCreateWindowEvent& e) {
  if (GetWindow(e.window)) {
    LOG(WARNING) << "Ignoring create notify for already-known window "
                 << XidStr(e.window);
    return;
  }

  DLOG(INFO) << "Handling create notify for "
             << (e.override_redirect ? "override-redirect" : "normal")
             << " window " << XidStr(e.window) << " at (" << e.x << ", " << e.y
             << ") with size " << e.width << "x" << e.height;

  // Don't bother doing anything else for windows which aren't direct
  // children of the root window.
  if (e.parent != root_)
    return;

  // Grab the server while we're doing all of this; we can get a ton of
  // errors when trying to track short-lived windows otherwise.
  scoped_ptr<XConnection::ScopedServerGrab> grab(
      xconn_->CreateScopedServerGrab());

  // We want to pass the geometry from the CreateNotify event to Window's
  // constructor, but we also need to include the window's depth, which sadly
  // isn't included in CreateNotify events.  Query the geometry again here to
  // get the depth (and also to check that the window still exists).
  XConnection::WindowGeometry new_geometry;
  if (!xconn_->GetWindowGeometry(e.window, &new_geometry)) {
    LOG(WARNING) << "Window " << XidStr(e.window)
                 << " went away while we were handling its CreateNotify event";
    return;
  }

  // CreateWindow stacks the new window on top of its siblings.
  DCHECK(!stacked_xids_->Contains(e.window));
  stacked_xids_->AddOnTop(e.window);

  XConnection::WindowGeometry geometry;
  geometry.bounds.reset(e.x, e.y, e.width, e.height);
  geometry.border_width = e.border_width;
  geometry.depth = new_geometry.depth;

  // override-redirect means that the window manager isn't going to
  // intercept this window's structure events, but we still need to
  // composite the window, so we'll create a Window object for it
  // regardless.
  TrackWindow(e.window, e.override_redirect, geometry);
}

void WindowManager::HandleDamageNotify(const XDamageNotifyEvent& e) {
  Window* win = GetWindow(e.drawable);
  if (!win)
    return;
  win->HandleDamageNotify(
      Rect(e.area.x, e.area.y, e.area.width, e.area.height));
}

void WindowManager::HandleDestroyNotify(const XDestroyWindowEvent& e) {
  DLOG(INFO) << "Handling destroy notify for " << XidStr(e.window);

  Window* win = GetWindow(e.window);

  // Horrible things will happen if we never got an UnmapNotify about the
  // window, since EventConsumers will still be holding references to it.
  // This shouldn't happen, but seems like it might sometimes
  // (http://crosbug.com/13792), so try to handle it as well as we can.
  if (win && win->mapped()) {
    LOG(WARNING) << "Got destroy notify for window " << win->xid_str()
                 << " while it's still mapped";
    HandleUnmappedWindow(win);
  }

  DCHECK(window_event_consumers_.find(e.window) ==
         window_event_consumers_.end())
      << "One or more event consumers are still registered for destroyed "
      << "window " << XidStr(e.window);

  if (stacked_xids_->Contains(e.window))
    stacked_xids_->Remove(e.window);

  // Don't bother doing anything else for windows which aren't direct
  // children of the root window.
  if (!win)
    return;

  if (!win->override_redirect())
    UpdateClientListStackingProperty();

  hash_map<XWindow, EventConsumer*>::iterator ec_it =
      destroyed_window_event_consumers_.find(e.window);
  if (ec_it != destroyed_window_event_consumers_.end()) {
    // Transfer ownership of the window's compositing-related resources to
    // the event consumer that wanted it.
    DestroyedWindow* destroyed_win = win->HandleDestroyNotify();
    ec_it->second->OwnDestroyedWindow(destroyed_win, e.window);
    destroyed_window_event_consumers_.erase(ec_it);
  }

  client_windows_.erase(e.window);
  win = NULL;  // Erasing from |client_windows_| deletes |window|.
}

void WindowManager::HandleEnterNotify(const XEnterWindowEvent& e) {
  DLOG(INFO) << "Handling enter notify for " << XidStr(e.window);
  const Point relative_pos(e.x, e.y);
  const Point absolute_pos(e.x_root, e.y_root);
  FOR_EACH_INTERESTED_EVENT_CONSUMER(
      window_event_consumers_,
      e.window,
      HandlePointerEnter(e.window, relative_pos, absolute_pos, e.time));
}

void WindowManager::HandleKeyPress(const XKeyEvent& e) {
  // We grab the keyboard while shutting down or signing out; ignore any events
  // that we get.
  if (IsSessionEnding())
    return;
  key_bindings_->HandleKeyPress(e.keycode, e.state, e.time);
}

void WindowManager::HandleKeyRelease(const XKeyEvent& e) {
  if (IsSessionEnding())
    return;
  key_bindings_->HandleKeyRelease(e.keycode, e.state, e.time);
}

void WindowManager::HandleLeaveNotify(const XLeaveWindowEvent& e) {
  DLOG(INFO) << "Handling leave notify for " << XidStr(e.window);
  const Point relative_pos(e.x, e.y);
  const Point absolute_pos(e.x_root, e.y_root);
  FOR_EACH_INTERESTED_EVENT_CONSUMER(
      window_event_consumers_,
      e.window,
      HandlePointerLeave(e.window, relative_pos, absolute_pos, e.time));
}

void WindowManager::HandleMapNotify(const XMapEvent& e) {
  DLOG(INFO) << "Handling map notify for " << XidStr(e.window);
  Window* win = GetWindow(e.window);
  if (!win || win->mapped())
    return;
  HandleMappedWindow(win);
}

void WindowManager::HandleMapRequest(const XMapRequestEvent& e) {
  DLOG(INFO) << "Handling map request for " << XidStr(e.window);
  Window* win = GetWindow(e.window);
  if (!win) {
    // This probably won't do much good; if we don't know about the window,
    // we're not going to be compositing it.
    LOG(WARNING) << "Mapping " << XidStr(e.window)
                 << ", which we somehow didn't already know about";
    xconn_->MapWindow(e.window);
    return;
  }

  // We've seen this happen before (see http://crosbug.com/4176), and it
  // can cause problems in code further down the pipe.
  if (win->mapped()) {
    LOG(WARNING) << "Ignoring map request for already-mapped window "
                 << win->xid_str();
    return;
  }

  if (win->override_redirect()) {
    LOG(WARNING) << "Huh?  Got a MapRequest event for override-redirect "
                 << "window " << win->xid_str();
  }

  for (set<EventConsumer*>::iterator it = event_consumers_.begin();
       it != event_consumers_.end(); ++it) {
    if ((*it)->HandleWindowMapRequest(win)) {
      // Map the window if the consumer approved it.  If the request fails
      // (probably because the window has already been destroyed), bail out --
      // we won't get an UnmapNotify event later, so we don't want to
      // incorrectly think that the window is mapped.
      if (win->MapClient()) {
        win->HandleMapRequested();
        HandleMappedWindow(win);
      }
      return;
    }
  }

  // Nobody was interested in the window.
  LOG(WARNING) << "Not mapping window " << win->xid_str() << " with type "
               << win->type();
}

void WindowManager::HandleMappingNotify(const XMappingEvent& e) {
  DLOG(INFO) << "Handling (keyboard) mapping notify";
  xconn()->RefreshKeyboardMap(e.request, e.first_keycode, e.count);
  key_bindings_->RefreshKeyMappings();
}

void WindowManager::HandleMotionNotify(const XMotionEvent& e) {
  const Point relative_pos(e.x, e.y);
  const Point absolute_pos(e.x_root, e.y_root);
  FOR_EACH_INTERESTED_EVENT_CONSUMER(
      window_event_consumers_,
      e.window,
      HandlePointerMotion(e.window, relative_pos, absolute_pos, e.time));
}

void WindowManager::HandlePropertyNotify(const XPropertyEvent& e) {
  if (e.window == root_ && e.atom == GetXAtom(ATOM_CHROME_LOGGED_IN)) {
    int value = 0;
    bool logged_in = xconn_->GetIntProperty(root_, e.atom, &value) && value;
    SetLoggedInState(logged_in, false);  // initial=false
    return;
  }

  // TODO: These can currently be very spammy.  The property is changed in
  // response to user interaction, including scrollwheel events, which can
  // be generated very quickly (thousands per second!).  Exit early for now
  // so we don't get bogged down writing to the log, but this can be
  // deleted later once we have a better solution for handling fast
  // scrolling.
  if (e.atom == GetXAtom(ATOM_NET_WM_USER_TIME))
    return;

  Window* win = GetWindow(e.window);
  if (win) {
    const bool deleted = (e.state == PropertyDelete);
    DLOG(INFO) << "Handling property notify for " << win->xid_str() << " about "
               << (deleted ? "deleted " : "") << "property "
               << XidStr(e.atom) << " (" << GetXAtomName(e.atom) << ")";
    if (e.atom == GetXAtom(ATOM_NET_WM_NAME)) {
      win->FetchAndApplyTitle();
    } else if (e.atom == GetXAtom(ATOM_WM_HINTS)) {
      win->FetchAndApplyWmHints();
    } else if (e.atom == GetXAtom(ATOM_WM_NORMAL_HINTS)) {
      win->FetchAndApplySizeHints();
    } else if (e.atom == GetXAtom(ATOM_WM_TRANSIENT_FOR)) {
      win->FetchAndApplyTransientHint();
    } else if (e.atom == GetXAtom(ATOM_WM_PROTOCOLS)) {
      win->FetchAndApplyWmProtocols();
    } else if (e.atom == GetXAtom(ATOM_CHROME_WINDOW_TYPE)) {
      win->FetchAndApplyWindowType();
    } else if (e.atom == GetXAtom(ATOM_NET_WM_WINDOW_TYPE)) {
      win->FetchAndApplyWmWindowType();
    } else if (e.atom == GetXAtom(ATOM_NET_WM_WINDOW_OPACITY)) {
      win->FetchAndApplyWindowOpacity();
    } else if (e.atom == GetXAtom(ATOM_NET_WM_STATE)) {
      win->FetchAndApplyWmState();
    } else if (e.atom == GetXAtom(ATOM_WM_CLIENT_MACHINE)) {
      win->FetchAndApplyWmClientMachine();
    } else if (e.atom == GetXAtom(ATOM_NET_WM_PID)) {
      win->FetchAndApplyWmPid();
    } else if (e.atom == GetXAtom(ATOM_NET_WM_SYNC_REQUEST_COUNTER)) {
      // Just re-fetch WM_PROTOCOLS here; this property is sometimes only set
      // after we've already read WM_PROTOCOLS -- see comment #3 at
      // http://crosbug.com/5846.
      win->FetchAndApplyWmProtocols();
    } else if (e.atom == GetXAtom(ATOM_CHROME_FREEZE_UPDATES)) {
      win->HandleFreezeUpdatesPropertyChange(!deleted);
    }
  }

  // Notify any event consumers that were interested in this property.
  FOR_EACH_INTERESTED_EVENT_CONSUMER(
      property_change_event_consumers_,
      make_pair(e.window, e.atom),
      HandleWindowPropertyChange(e.window, e.atom));
}

void WindowManager::HandleReparentNotify(const XReparentEvent& e) {
  DLOG(INFO) << "Handling reparent notify for " << XidStr(e.window)
             << " to " << XidStr(e.parent);

  if (e.parent == root_) {
    // If a window got reparented *to* the root window, we want to track
    // it in the stacking order (we don't bother tracking it as a true
    // toplevel window; we don't see this happen much outside of Flash
    // windows).  The window gets stacked on top of its new siblings.
    DCHECK(!stacked_xids_->Contains(e.window));
    stacked_xids_->AddOnTop(e.window);
  } else {
    // Otherwise, if it got reparented away from us, stop tracking it.
    // We ignore windows that aren't immediate children of the root window.
    if (!stacked_xids_->Contains(e.window))
      return;

    stacked_xids_->Remove(e.window);

    if (mapped_xids_->Contains(e.window)) {
      mapped_xids_->Remove(e.window);
      UpdateClientListProperty();
    }

    Window* win = GetWindow(e.window);
    if (win) {
      if (!win->override_redirect())
        UpdateClientListStackingProperty();

      // Make sure that all event consumers know that the window's going away.
      if (win->mapped())
        FOR_EACH_EVENT_CONSUMER(event_consumers_, HandleWindowUnmap(win));
      focus_manager_->HandleWindowUnmap(win);

      client_windows_.erase(e.window);

      // We're not going to be compositing the window anymore, so
      // unredirect it so it'll get drawn using the usual path.
      xconn_->UnredirectWindowForCompositing(e.window);
    }
  }
}

void WindowManager::HandleRRScreenChangeNotify(
    const XRRScreenChangeNotifyEvent& e) {
  DLOG(INFO) << "Got RRScreenChangeNotify event to "
             << e.width << "x" << e.height;
  // TODO: This doesn't seem to work reliably (we're not seeing any events
  // right now), so we get size change notifications via ConfigureNotify
  // events on the root window instead.  Either stop using randr or figure
  // out what's wrong if we need it for e.g. rotate.
}

void WindowManager::HandleShapeNotify(const XShapeEvent& e) {
  Window* win = GetWindow(e.window);
  if (!win)
    return;

  DLOG(INFO) << "Handling " << (e.kind == ShapeBounding ? "bounding" : "clip")
             << " shape notify for " << XidStr(e.window);
  if (e.kind == ShapeBounding)
    win->FetchAndApplyShape();
}

void WindowManager::HandleSyncAlarmNotify(const XSyncAlarmNotifyEvent& e) {
  int64_t value =
      (static_cast<int64_t>(XSyncValueHigh32(e.counter_value)) << 32) |
      XSyncValueLow32(e.counter_value);
  DLOG(INFO) << "Handling sync alarm notify for alarm " << XidStr(e.alarm)
             << " with value " << value;
  Window* win = FindWithDefault(
      sync_alarms_to_windows_, e.alarm, static_cast<Window*>(NULL));
  if (!win) {
    // We get notification that counters have been reset to 0 after the
    // corresponding windows are destroyed; don't log a warning for that.
    if (value != 0)
      LOG(WARNING) << "Ignoring unregistered alarm " << XidStr(e.alarm);
    return;
  }
  win->HandleSyncAlarmNotify(e.alarm, value);
}

void WindowManager::HandleUnmapNotify(const XUnmapEvent& e) {
  DLOG(INFO) << "Handling unmap notify for " << XidStr(e.window);
  Window* win = GetWindow(e.window);
  if (!win)
    return;
  if (!win->mapped()) {
    LOG(WARNING) << "Ignoring unmap notify for non-mapped window "
                 << win->xid_str();
    return;
  }
  HandleUnmappedWindow(win);
}

XWindow WindowManager::GetArbitraryChromeWindow() {
  for (WindowMap::const_iterator i = client_windows_.begin();
       i != client_windows_.end(); ++i) {
    if (WmIpcWindowTypeIsChrome(i->second->type()))
      return i->first;
  }
  return 0;
}

void WindowManager::TakeScreenshot(bool use_active_window) {
  const string& dir = logged_in_ ?
      FLAGS_logged_in_screenshot_output_dir :
      FLAGS_logged_out_screenshot_output_dir;

  if (access(dir.c_str(), F_OK) != 0 &&
      !file_util::CreateDirectory(FilePath(dir))) {
    LOG(ERROR) << "Unable to create screenshot directory " << dir;
    return;
  }

  string command = FLAGS_screenshot_binary;

  if (use_active_window) {
    if (!active_window_xid_) {
      LOG(WARNING) << "No active window to use for screenshot";
      return;
    }
    command += StringPrintf(" --window=0x%lx", active_window_xid_);
  }

  string filename = StringPrintf("%s/screenshot-%s.png", dir.c_str(),
                                 GetTimeAsString(GetCurrentTimeSec()).c_str());
  command += " " + filename + " &";

  if (system(command.c_str()) < 0) {
    LOG(ERROR) << "Taking screenshot via \"" << command << "\" failed";
  } else {
    LOG(INFO) << "Saved screenshot to " << filename;
  }
}

void WindowManager::CreateStartupBackground() {
  startup_pixmap_ =
      xconn_->CreatePixmap(root_, root_bounds_.size(), root_depth_);
  xconn_->CopyArea(root_,            // src
                   startup_pixmap_,  // dest
                   Point(0, 0),      // src_pos
                   Point(0, 0),      // dest_pos
                   root_bounds_.size());
  Compositor::TexturePixmapActor* pixmap_actor =
      compositor_->CreateTexturePixmap();
  pixmap_actor->SetPixmap(startup_pixmap_);
  startup_background_.reset(pixmap_actor);
  startup_background_->SetName("startup background");
  stage_->AddActor(startup_background_.get());
  stacking_manager_->StackActorAtTopOfLayer(
      startup_background_.get(), StackingManager::LAYER_BACKGROUND);
  startup_background_->Show();
}

void WindowManager::HideUnacceleratedGraphicsActor() {
  if (unaccelerated_graphics_actor_.get())
    unaccelerated_graphics_actor_->SetOpacity(
        0.f, kUnacceleratedGraphicsActorHideAnimMs );
  event_loop_->RemoveTimeout(hide_unaccelerated_graphics_actor_timeout_id_);
  hide_unaccelerated_graphics_actor_timeout_id_ = 0;
}

void WindowManager::DisableCompositing() {
  DCHECK(disable_compositing_task_is_pending_);
  disable_compositing_task_is_pending_ = false;

  // Looks like we changed our minds about disabling compositing.
  if (!unredirected_fullscreen_xid_)
    return;

  // Give the overlay window an empty bounding region before unredirection.
  // Unredirection won't create exposure events, so we need to remove the
  // bounding region first to let the client know that it should refresh the
  // content.
  xconn_->SetWindowBoundingRegionToRect(overlay_xid_, Rect());
  xconn_->UnredirectWindowForCompositing(unredirected_fullscreen_xid_);
  compositor_->set_should_draw_frame(false);
  DLOG(INFO) << "Turned compositing off";
}

void WindowManager::DestroyLoginControllerInternal() {
  if (!login_controller_.get())
    return;
  DLOG(INFO) << "Destroying login controller";
  event_consumers_.erase(login_controller_.get());
  login_controller_.reset();
}

void WindowManager::PingChrome() {
  chrome_watchdog_->SendPingToChrome(GetCurrentTimeFromServer(),
                                     kPingChromeTimeoutMs);
}

}  // namespace window_manager
