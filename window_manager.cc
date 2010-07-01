// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
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
#include "window_manager/callback.h"
#include "window_manager/event_consumer.h"
#include "window_manager/event_loop.h"
#include "window_manager/focus_manager.h"
#include "window_manager/geometry.h"
#include "window_manager/hotkey_overlay.h"
#include "window_manager/key_bindings.h"
#include "window_manager/layout_manager.h"
#include "window_manager/login_controller.h"
#include "window_manager/panel_manager.h"
#include "window_manager/profiler.h"
#include "window_manager/screen_locker_handler.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/x_connection.h"

DEFINE_string(xterm_command, "xterm", "Command for hotkey xterm spawn.");
DEFINE_string(background_color, "#000", "Background color to use");
DEFINE_string(lock_screen_command, "powerd_lock_screen",
              "Command to lock the screen");
DEFINE_string(configure_monitor_command,
              "/usr/sbin/monitor_reconfigure",
              "Command to configure an external monitor");
DEFINE_string(screenshot_binary,
              "/usr/bin/screenshot",
              "Path to the screenshot binary");
DEFINE_string(logged_in_screenshot_output_dir,
              ".", "Output directory for screenshots when logged in");
DEFINE_string(logged_out_screenshot_output_dir,
              ".", "Output directory for screenshots when not logged in");
DEFINE_string(logged_in_log_dir,
              ".", "Directory to write logs to when logged in.");
DEFINE_string(logged_out_log_dir,
              ".", "Directory to write logs to when not logged in.");

DEFINE_bool(use_compositing, true, "Use compositing");

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
using window_manager::util::SetUpLogSymlink;
using window_manager::util::XidStr;

namespace window_manager {

// Time to spend fading the hotkey overlay in or out, in milliseconds.
static const int kHotkeyOverlayAnimMs = 100;

// Interval with which we query the keyboard state from the X server to
// update the hotkey overlay (when it's being shown).
static const int kHotkeyOverlayPollMs = 100;

const int WindowManager::kVideoTimePropertyUpdateSec = 5;

// Invoke 'function_call' for each event consumer in 'consumers' (a set).
#define FOR_EACH_EVENT_CONSUMER(consumers, function_call)                      \
  do {                                                                         \
    for (set<EventConsumer*>::iterator it =                                    \
         consumers.begin(); it != consumers.end(); ++it) {                     \
      (*it)->function_call;                                                    \
    }                                                                          \
  } while (0)

// Look up the event consumers that have registered interest in 'key' in
// 'consumer_map' (one of the WindowManager::*_event_consumers_ member
// variables), and invoke 'function_call' (e.g.
// "HandleWindowPropertyChange(e.window, e.atom)") on each.  Helper macro
// used by WindowManager's event-handling methods.
#define FOR_EACH_INTERESTED_EVENT_CONSUMER(consumer_map, key, function_call)   \
  do {                                                                         \
    typeof(consumer_map.begin()) it = consumer_map.find(key);                  \
    if (it != consumer_map.end()) {                                            \
      for (set<EventConsumer*>::iterator ec_it =                               \
           it->second.begin(); ec_it != it->second.end(); ++ec_it) {           \
        (*ec_it)->function_call;                                               \
      }                                                                        \
    }                                                                          \
  } while (0)

// Used by helper functions to generate 'case' statements.
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

WindowManager::WindowManager(EventLoop* event_loop,
                             XConnection* xconn,
                             Compositor* compositor)
    : event_loop_(event_loop),
      xconn_(xconn),
      compositor_(compositor),
      root_(0),
      width_(0),
      height_(0),
      wm_xid_(0),
      stage_(NULL),
      stage_xid_(0),
      overlay_xid_(0),
      mapped_xids_(new Stacker<XWindow>),
      stacked_xids_(new Stacker<XWindow>),
      active_window_xid_(0),
      query_keyboard_state_timeout_id_(-1),
      showing_hotkey_overlay_(false),
      wm_ipc_version_(1),
      logged_in_(false),
      initialize_logging_(false),
      last_video_time_(-1) {
  CHECK(event_loop_);
  CHECK(xconn_);
  CHECK(compositor_);
}

WindowManager::~WindowManager() {
  if (wm_xid_)
    xconn_->DestroyWindow(wm_xid_);
  if (startup_pixmap_)
    xconn_->FreePixmap(startup_pixmap_);
  if (query_keyboard_state_timeout_id_ >= 0)
    event_loop_->RemoveTimeout(query_keyboard_state_timeout_id_);
  if (panel_manager_.get())
    panel_manager_->UnregisterAreaChangeListener(this);
}

void WindowManager::HandlePanelManagerAreaChange() {
  SetEwmhWorkareaProperty();
}

bool WindowManager::Init() {
  CHECK(!root_) << "Init() may only be called once";
  root_ = xconn_->GetRootWindow();
  xconn_->SelectRandREventsOnWindow(root_);
  XConnection::WindowGeometry root_geometry;
  CHECK(xconn_->GetWindowGeometry(root_, &root_geometry));
  width_ = root_geometry.width;
  height_ = root_geometry.height;

  // Create the atom cache first; RegisterExistence() needs it.
  atom_cache_.reset(new AtomCache(xconn_));

  CHECK(RegisterExistence());
  SetEwmhGeneralProperties();
  SetEwmhSizeProperties();

  // First make sure that we'll get notified if the login state changes
  // and then query its current value.
  CHECK(xconn_->SelectInputOnWindow(root_, PropertyChangeMask, true));
  int logged_in_value = 0;
  SetLoggedInState(xconn_->GetIntProperty(root_,
                                          GetXAtom(ATOM_CHROME_LOGGED_IN),
                                          &logged_in_value) && logged_in_value,
                   true);  // initial=true

  // Set root window's cursor to left pointer.
  xconn_->SetWindowCursor(root_, XC_left_ptr);

  stage_ = compositor_->GetDefaultStage();
  stage_xid_ = stage_->GetStageXWindow();
  stage_->SetName("stage");
  stage_->SetSize(width_, height_);
  stage_->SetStageColor(Compositor::Color(FLAGS_background_color));
  stage_->Show();

  wm_ipc_.reset(new WmIpc(xconn_, atom_cache_.get()));

  stacking_manager_.reset(
      new StackingManager(xconn_, compositor_, atom_cache_.get()));
  focus_manager_.reset(new FocusManager(this));

  if (!logged_in_) {
    startup_pixmap_ = xconn_->CreatePixmap(root_, width_, height_,
                                           root_geometry.depth);
    xconn_->CopyArea(root_, startup_pixmap_, 0, 0, 0, 0, width_, height_);
    startup_background_.reset(compositor_->CreateTexturePixmap());
    startup_background_->SetName("startup background");
    startup_background_->SetPixmap(startup_pixmap_);
    startup_background_->Show();
    stage_->AddActor(startup_background_.get());
    stacking_manager_->StackActorAtTopOfLayer(
        startup_background_.get(), StackingManager::LAYER_BACKGROUND);
  }

  if (FLAGS_use_compositing) {
    // Draw the scene first to make sure that it's ready.
    compositor_->Draw();
    CHECK(xconn_->RedirectSubwindowsForCompositing(root_));
    // Create the compositing overlay, put the stage's window inside of it,
    // and make events fall through both to the client windows underneath.
    overlay_xid_ = xconn_->GetCompositingOverlayWindow(root_);
    CHECK(overlay_xid_);
    LOG(INFO) << "Reparenting stage window " << XidStr(stage_xid_)
              << " into Xcomposite overlay window " << XidStr(overlay_xid_);
    CHECK(xconn_->ReparentWindow(stage_xid_, overlay_xid_, 0, 0));
    CHECK(xconn_->RemoveInputRegionFromWindow(overlay_xid_));
    CHECK(xconn_->RemoveInputRegionFromWindow(stage_xid_));
  }

  key_bindings_.reset(new KeyBindings(xconn()));
  logged_in_key_bindings_group_.reset(
      new KeyBindingsGroup(key_bindings_.get()));
  if (!logged_in_)
    logged_in_key_bindings_group_->Disable();
  RegisterKeyBindings();

  panel_manager_.reset(new PanelManager(this));
  event_consumers_.insert(panel_manager_.get());
  panel_manager_->RegisterAreaChangeListener(this);
  HandlePanelManagerAreaChange();

  layout_manager_.reset(new LayoutManager(this, panel_manager_.get()));
  event_consumers_.insert(layout_manager_.get());

  login_controller_.reset(new LoginController(this));
  event_consumers_.insert(login_controller_.get());

  screen_locker_handler_.reset(new ScreenLockerHandler(this));
  event_consumers_.insert(screen_locker_handler_.get());

  hotkey_overlay_.reset(new HotkeyOverlay(xconn_, compositor_));
  stage_->AddActor(hotkey_overlay_->group());
  stacking_manager_->StackActorAtTopOfLayer(
      hotkey_overlay_->group(), StackingManager::LAYER_HOTKEY_OVERLAY);
  hotkey_overlay_->group()->Move(width_ / 2, height_ / 2, 0);

  // Look up existing windows (note that this includes windows created
  // earlier in this method) and select window management events.
  scoped_ptr<XConnection::ScopedServerGrab> grab(
      xconn_->CreateScopedServerGrab());
  ManageExistingWindows();
  CHECK(xconn_->SelectInputOnWindow(
            root_,
            SubstructureRedirectMask | StructureNotifyMask |
              SubstructureNotifyMask,
            true));  // preserve the existing event mask
  grab.reset();

  return true;
}

void WindowManager::SetLoggedInState(bool logged_in, bool initial) {
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
    logging::InitLogging(log_path.c_str(),
                         logging::LOG_ONLY_TO_FILE,
                         logging::DONT_LOCK_LOG_FILE,
                         logging::APPEND_TO_OLD_LOG_FILE);
  }

  if (logged_in_key_bindings_group_.get()) {
    if (logged_in_)
      logged_in_key_bindings_group_->Enable();
    else
      logged_in_key_bindings_group_->Disable();
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
  static int randr_notify = xconn_->randr_event_base() + RRScreenChangeNotify;

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
      } else if (event->type == shape_notify) {
        HandleShapeNotify(*(reinterpret_cast<XShapeEvent*>(event)));
      } else if (event->type == randr_notify) {
        HandleRRScreenChangeNotify(
            *(reinterpret_cast<XRRScreenChangeNotifyEvent*>(event)));
      }
  }
}

XWindow WindowManager::CreateInputWindow(
    int x, int y, int width, int height, int event_mask) {
  XWindow xid = xconn_->CreateWindow(
      root_,  // parent
      x, y,
      width, height,
      true,   // override redirect
      true,   // input only
      event_mask);
  CHECK(xid);

  if (FLAGS_use_compositing) {
    // If the stage has been reparented into the overlay window, we need to
    // stack the input window under the overlay instead of under the stage
    // (because the stage isn't a sibling of the input window).
    XWindow top_win = overlay_xid_ ? overlay_xid_ : stage_xid_;
    CHECK(xconn_->StackWindow(xid, top_win, false));
  }
  CHECK(xconn_->MapWindow(xid));
  return xid;
}

bool WindowManager::ConfigureInputWindow(
    XWindow xid, int x, int y, int width, int height) {
  DLOG(INFO) << "Configuring input window " << XidStr(xid)
             << " to (" << x << ", " << y << ") and size "
             << width << "x" << height;
  return xconn_->ConfigureWindow(xid, x, y, width, height);
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

void WindowManager::FocusWindow(Window* win, XTime timestamp) {
  focus_manager_->FocusWindow(win, timestamp);
}

void WindowManager::TakeFocus(XTime timestamp) {
  if (!layout_manager_->TakeFocus(timestamp) &&
      !panel_manager_->TakeFocus(timestamp)) {
    focus_manager_->FocusWindow(NULL, timestamp);
  }
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
  if (video_time >= last_video_time_ + kVideoTimePropertyUpdateSec) {
    last_video_time_ = video_time;
    XAtom atom = atom_cache_->GetXAtom(ATOM_CHROME_VIDEO_TIME);
    return xconn_->SetIntProperty(
        root_, atom, atom, static_cast<int>(video_time));
  }
  return true;
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

void WindowManager::ToggleClientWindowDebugging() {
  if (client_window_debugging_enabled()) {
    client_window_debugging_actors_.clear();
    return;
  }
  UpdateClientWindowDebugging();
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
  static const Compositor::Color kFgColor(0.f, 0.f, 0.f);

  int cnt = 0;
  float step = 6.f/xids.size();
  for (vector<XWindow>::iterator it = xids.begin(); it != xids.end(); ++it) {
    Compositor::Color bg_color;
    bg_color.SetHsv(cnt++ * step, 1.f, 1.f);
    XConnection::WindowGeometry geometry;
    if (!xconn_->GetWindowGeometry(*it, &geometry))
      continue;

    Compositor::ContainerActor* group = compositor_->CreateGroup();
    stage_->AddActor(group);
    stacking_manager_->StackActorAtTopOfLayer(
        group, StackingManager::LAYER_DEBUGGING);
    group->SetName("debug group");
    group->Move(geometry.x, geometry.y, 0);
    group->SetSize(geometry.width, geometry.height);
    group->Show();
    group->SetClip(0, 0, geometry.width, geometry.height);

    Compositor::Actor* rect =
        compositor_->CreateRectangle(bg_color, kFgColor, 1);
    group->AddActor(rect);
    rect->SetName("debug box");
    rect->Move(0, 0, 0);
    rect->SetSize(geometry.width, geometry.height);
    rect->SetOpacity(0, 0);
    rect->SetOpacity(0.3, kDebugFadeMs);
    rect->Show();

    Compositor::Actor* text =
        compositor_->CreateText("Sans 10pt", XidStr(*it), kFgColor);
    group->AddActor(text);
    text->SetName("debug label");
    text->Move(3, 3, 0);
    text->SetOpacity(0, 0);
    text->SetOpacity(1, kDebugFadeMs);
    text->Show();
    text->Raise(rect);

    new_actors.push_back(shared_ptr<Compositor::Actor>(group));
    new_actors.push_back(shared_ptr<Compositor::Actor>(rect));
    new_actors.push_back(shared_ptr<Compositor::Actor>(text));
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
                                 -1, -1,  // position
                                 1, 1,    // dimensions
                                 true,    // override redirect
                                 false,   // input only
                                 PropertyChangeMask);  // event mask
  CHECK(wm_xid_);
  LOG(INFO) << "Created window " << XidStr(wm_xid_)
            << " for registering ourselves as the window manager";

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
  supported.push_back(GetXAtom(ATOM_NET_WM_NAME));
  supported.push_back(GetXAtom(ATOM_NET_WM_STATE));
  supported.push_back(GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN));
  supported.push_back(GetXAtom(ATOM_NET_WM_STATE_MODAL));
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
  geometry.push_back(width_);
  geometry.push_back(height_);
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
      width_ - panel_manager_left_width - panel_manager_right_width);
  workarea.push_back(height_);
  return xconn_->SetIntArrayProperty(
      root_, GetXAtom(ATOM_NET_WORKAREA), XA_CARDINAL, workarea);
}

void WindowManager::RegisterKeyBindings() {
  key_bindings_->AddAction(
      "launch-terminal",
      NewPermanentCallback(
          this, &WindowManager::RunCommand, FLAGS_xterm_command),
      NULL, NULL);
  logged_in_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(
          XK_t, KeyBindings::kControlMask | KeyBindings::kAltMask),
      "launch-terminal");

  key_bindings_->AddAction(
      "toggle-client-window-debugging",
      NewPermanentCallback(this, &WindowManager::ToggleClientWindowDebugging),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_F9), "toggle-client-window-debugging");

  key_bindings_->AddAction(
      "toggle-profiler",
      NewPermanentCallback(this, &WindowManager::ToggleProfiler),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_F10), "toggle-profiler");

  key_bindings_->AddAction(
      "lock-screen",
      NewPermanentCallback(
          this, &WindowManager::RunCommand, FLAGS_lock_screen_command),
      NULL, NULL);
  logged_in_key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(
          XK_l, KeyBindings::kControlMask | KeyBindings::kAltMask),
      "lock-screen");

  key_bindings_->AddAction(
      "configure-monitor",
      NewPermanentCallback(this, &WindowManager::RunCommand,
                           FLAGS_configure_monitor_command),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(
          XK_m, KeyBindings::kControlMask | KeyBindings::kAltMask),
      "configure-monitor");

  key_bindings_->AddAction(
      "toggle-hotkey-overlay",
      NewPermanentCallback(this, &WindowManager::ToggleHotkeyOverlay),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_F8), "toggle-hotkey-overlay");

  key_bindings_->AddAction(
      "take-root-screenshot",
      NewPermanentCallback(this, &WindowManager::TakeScreenshot, false),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_Print), "take-root-screenshot");

  key_bindings_->AddAction(
      "take-window-screenshot",
      NewPermanentCallback(this, &WindowManager::TakeScreenshot, true),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_Print, KeyBindings::kShiftMask),
      "take-window-screenshot");

  key_bindings_->AddAction(
      "increase-audio-volume",
      NewPermanentCallback(
          this, &WindowManager::SendNotifySyskeyMessage,
          chromeos::WM_IPC_SYSTEM_KEY_VOLUME_UP),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XF86XK_AudioRaiseVolume), "increase-audio-volume");

  key_bindings_->AddAction(
      "decrease-audio-volume",
      NewPermanentCallback(
          this, &WindowManager::SendNotifySyskeyMessage,
          chromeos::WM_IPC_SYSTEM_KEY_VOLUME_DOWN),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XF86XK_AudioLowerVolume), "decrease-audio-volume");

  key_bindings_->AddAction(
      "mute-audio",
      NewPermanentCallback(
          this, &WindowManager::SendNotifySyskeyMessage,
          chromeos::WM_IPC_SYSTEM_KEY_VOLUME_MUTE),
      NULL, NULL);
  key_bindings_->AddBinding(
      KeyBindings::KeyCombo(XF86XK_AudioMute), "mute-audio");
}

bool WindowManager::ManageExistingWindows() {
  vector<XWindow> windows;
  if (!xconn_->GetChildWindows(root_, &windows)) {
    return false;
  }

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

Window* WindowManager::TrackWindow(XWindow xid, bool override_redirect,
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
  // We need to get a new pixmap for the window.
  win->HandleMapNotify();
  FOR_EACH_EVENT_CONSUMER(event_consumers_, HandleWindowMap(win));

  if (win->override_redirect()) {
    win->ShowComposited();
    return;
  }

  if (mapped_xids_->Contains(win->xid())) {
    LOG(WARNING) << "Handling mapped window " << win->xid_str()
                 << ", which is already listed in 'mapped_xids_'";
  } else {
    mapped_xids_->AddOnTop(win->xid());
    UpdateClientListProperty();
    // This only includes mapped windows, so we need to update it now.
    UpdateClientListStackingProperty();
  }

  SetWmStateProperty(win->xid(), 1);  // NormalState
}

void WindowManager::HandleScreenResize(int new_width, int new_height) {
  width_ = new_width;
  height_ = new_height;
  SetEwmhSizeProperties();
  stage_->SetSize(width_, height_);
  hotkey_overlay_->group()->Move(width_ / 2, height_ / 2, 0);

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

  FOR_EACH_INTERESTED_EVENT_CONSUMER(
      window_event_consumers_,
      e.window,
      HandleButtonPress(e.window, e.x, e.y, e.x_root,
                        e.y_root, e.button, e.time));
}

void WindowManager::HandleButtonRelease(const XButtonEvent& e) {
  DLOG(INFO) << "Handling button release in window " << XidStr(e.window)
             << " at relative (" << e.x << ", " << e.y << "), absolute ("
             << e.x_root << ", " << e.y_root << ") with button " << e.button;
  FOR_EACH_INTERESTED_EVENT_CONSUMER(
      window_event_consumers_,
      e.window,
      HandleButtonRelease(e.window, e.x, e.y, e.x_root,
                          e.y_root, e.button, e.time));
}

void WindowManager::HandleClientMessage(const XClientMessageEvent& e) {
  DLOG(INFO) << "Handling client message for window " << XidStr(e.window)
             << " with type " << XidStr(e.message_type) << " ("
             << GetXAtomName(e.message_type) << ") and format " << e.format;
  WmIpc::Message msg;
  if (wm_ipc_->GetMessage(e.window, e.message_type, e.format, e.data.l, &msg)) {
    if (msg.type() == chromeos::WM_IPC_MESSAGE_WM_NOTIFY_IPC_VERSION) {
      wm_ipc_version_ = msg.param(0);
      LOG(INFO) << "Got WM_NOTIFY_IPC_VERSION message saying that Chrome is "
                << "using version " << wm_ipc_version_;
    } else {
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
  if (e.window == root_ && (e.width != width_ || e.height != height_)) {
    DLOG(INFO) << "Got configure notify saying that root window has been "
               << "resized to " << e.width << "x" << e.height;
    HandleScreenResize(e.width, e.height);
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
  // 'stacked_xids_'?
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
      // 'above' being unset means that the window is stacked beneath its
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
  DLOG(INFO) << "Handling configure notify for " << XidStr(e.window)
             << " to pos (" << e.x << ", " << e.y << ") and size "
             << e.width << "x" << e.height << ", above " << XidStr(e.above);

  // There are several cases to consider here:
  //
  // - Override-redirect windows' calls to configure themselves are honored
  //   by the X server without any intervention on our part, so we only
  //   need to update their composited positions here.
  // - Regular non-override-redirect windows' configuration calls are
  //   passed to us as ConfigureRequest events, so we would've already
  //   updated both their X and composited configuration in
  //   HandleConfigureRequest().  We don't need to do anything here.
  // - For both types of window, we may have decided to move or resize the
  //   window ourselves earlier through a direct call to Window::Move() or
  //   Resize().  In that case, we would've already updated their
  //   composited position (or at least started the animation) then.

  if (win->override_redirect()) {
    win->MoveComposited(e.x, e.y, 0);
    win->SaveClientPosition(e.x, e.y);
    win->SaveClientSize(e.width, e.height);

    // When we see a stacking change for an override-redirect window, we
    // attempt to restack its actor correspondingly.  If we don't have an
    // actor for the X window directly under it, we walk down the stack
    // until we find one.
    XWindow above_xid = e.above;
    while (above_xid) {
      Window* above_win = GetWindow(above_xid);
      Compositor::Actor* above_actor =
          above_win ? above_win->actor() :
          stacking_manager_->GetActorIfLayerXid(above_xid);

      if (above_actor) {
        DLOG(INFO) << "Stacking override-redirect window " << win->xid_str()
                   << "'s actor above window " << XidStr(above_xid) << "'s";
        win->StackCompositedAbove(above_actor, NULL, false);
        break;
      }
      const XWindow* above_ptr = stacked_xids_->GetUnder(above_xid);
      above_xid = above_ptr ? *above_ptr : 0;
    }
  } else {
    if (restacked) {
      // _NET_CLIENT_LIST_STACKING only includes managed (i.e.
      // non-override-redirect) windows, so we only update it when a
      // managed window's stacking position changed.
      UpdateClientListStackingProperty();
    }
  }

  win->HandleConfigureNotify(e.width, e.height);
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

  const int req_x = (e.value_mask & CWX) ? e.x : win->client_x();
  const int req_y = (e.value_mask & CWY) ? e.y : win->client_y();
  const int req_width =
      (e.value_mask & CWWidth) ? e.width : win->client_width();
  const int req_height =
      (e.value_mask & CWHeight) ? e.height : win->client_height();

  // The X server should reject bogus requests before they get to us, but
  // just in case...
  if (req_width <= 0 || req_height <= 0) {
    LOG(WARNING) << "Ignoring request to resize window " << win->xid_str()
                 << " to " << req_width << "x" << req_height;
    return;
  }

  if (!win->mapped()) {
    // If the window is unmapped, it's unlikely that any event consumers
    // will know what to do with it.  Do whatever we were asked to do.
    if (req_x != win->client_x() || req_y != win->client_y())
      win->MoveClient(req_x, req_y);
    if (req_width != win->client_width() || req_height != win->client_height())
      win->ResizeClient(req_width, req_height, GRAVITY_NORTHWEST);
  } else {
    FOR_EACH_INTERESTED_EVENT_CONSUMER(
        window_event_consumers_,
        e.window,
        HandleWindowConfigureRequest(win, req_x, req_y, req_width, req_height));
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

  XConnection::WindowAttributes attr;
  if (!xconn_->GetWindowAttributes(e.window, &attr)) {
    LOG(WARNING) << "Window " << XidStr(e.window)
                 << " went away while we were handling its CreateNotify event";
    return;
  }

  // CreateWindow stacks the new window on top of its siblings.
  DCHECK(!stacked_xids_->Contains(e.window));
  stacked_xids_->AddOnTop(e.window);

  XConnection::WindowGeometry geometry;
  geometry.x = e.x;
  geometry.y = e.y;
  geometry.width = e.width;
  geometry.height = e.height;
  geometry.border_width = e.border_width;
  // We don't get the depth in the event, but at least for now, Window's
  // constructor doesn't need it.

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

  DCHECK(window_event_consumers_.find(e.window) ==
         window_event_consumers_.end())
      << "One or more event consumers are still registered for destroyed "
      << "window " << XidStr(e.window);

  if (stacked_xids_->Contains(e.window))
    stacked_xids_->Remove(e.window);

  // Don't bother doing anything else for windows which aren't direct
  // children of the root window.
  Window* win = GetWindow(e.window);
  if (!win)
    return;

  if (!win->override_redirect())
    UpdateClientListStackingProperty();

  // TODO: If the code to remove a window gets more involved, move it into
  // a separate RemoveWindow() method -- window_test.cc currently erases
  // windows from 'client_windows_' directly to simulate windows being
  // destroyed.
  client_windows_.erase(e.window);
  win = NULL;  // erasing from client_windows_ deletes window.
}

void WindowManager::HandleEnterNotify(const XEnterWindowEvent& e) {
  DLOG(INFO) << "Handling enter notify for " << XidStr(e.window);
  FOR_EACH_INTERESTED_EVENT_CONSUMER(
      window_event_consumers_,
      e.window,
      HandlePointerEnter(e.window, e.x, e.y, e.x_root, e.y_root, e.time));
}

void WindowManager::HandleKeyPress(const XKeyEvent& e) {
  KeySym keysym = xconn_->GetKeySymFromKeyCode(e.keycode);
  key_bindings_->HandleKeyPress(keysym, e.state, e.time);
}

void WindowManager::HandleKeyRelease(const XKeyEvent& e) {
  KeySym keysym = xconn_->GetKeySymFromKeyCode(e.keycode);
  key_bindings_->HandleKeyRelease(keysym, e.state, e.time);
}

void WindowManager::HandleLeaveNotify(const XLeaveWindowEvent& e) {
  DLOG(INFO) << "Handling leave notify for " << XidStr(e.window);
  FOR_EACH_INTERESTED_EVENT_CONSUMER(
      window_event_consumers_,
      e.window,
      HandlePointerLeave(e.window, e.x, e.y, e.x_root, e.y_root, e.time));
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
      // If one of the event consumers tells us that it's sent a request to
      // map the window, we act as if the window is already mapped.  This
      // is safe (it'll be mapped by the time that any subsequent requests
      // make it to the X server) and avoids races where we can get
      // messages from Chrome about windows before we've received MapNotify
      // events about them.
      HandleMappedWindow(win);
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
  hotkey_overlay_->RefreshKeyMappings();
  key_bindings_->RefreshKeyMappings();
}

void WindowManager::HandleMotionNotify(const XMotionEvent& e) {
  FOR_EACH_INTERESTED_EVENT_CONSUMER(
      window_event_consumers_,
      e.window,
      HandlePointerMotion(e.window, e.x, e.y, e.x_root, e.y_root, e.time));
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
    bool deleted = (e.state == PropertyDelete);
    DLOG(INFO) << "Handling property notify for " << win->xid_str() << " about "
               << (deleted ? "deleted " : "") << "property "
               << XidStr(e.atom) << " (" << GetXAtomName(e.atom) << ")";
    if (e.atom == GetXAtom(ATOM_NET_WM_NAME)) {
      string title;
      if (deleted || !xconn_->GetStringProperty(win->xid(), e.atom, &title))
        win->SetTitle("");
      else
        win->SetTitle(title);
    } else if (e.atom == GetXAtom(ATOM_WM_HINTS)) {
      win->FetchAndApplyWmHints();
    } else if (e.atom == GetXAtom(ATOM_WM_NORMAL_HINTS)) {
      win->FetchAndApplySizeHints();
    } else if (e.atom == GetXAtom(ATOM_WM_TRANSIENT_FOR)) {
      win->FetchAndApplyTransientHint();
    } else if (e.atom == GetXAtom(ATOM_CHROME_WINDOW_TYPE)) {
      win->FetchAndApplyWindowType(true);  // update_shadow
    } else if (e.atom == GetXAtom(ATOM_NET_WM_WINDOW_TYPE)) {
      win->FetchAndApplyWmWindowType(true);  // update_shadow
    } else if (e.atom == GetXAtom(ATOM_NET_WM_WINDOW_OPACITY)) {
      win->FetchAndApplyWindowOpacity();
    } else if (e.atom == GetXAtom(ATOM_NET_WM_STATE)) {
      win->FetchAndApplyWmState();
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
      if (FLAGS_use_compositing)
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
    win->FetchAndApplyShape(true);  // update_shadow
}

void WindowManager::HandleUnmapNotify(const XUnmapEvent& e) {
  DLOG(INFO) << "Handling unmap notify for " << XidStr(e.window);
  Window* win = GetWindow(e.window);
  if (!win)
    return;

  win->HandleUnmapNotify();
  FOR_EACH_EVENT_CONSUMER(event_consumers_, HandleWindowUnmap(win));

  if (win->override_redirect()) {
    win->HideComposited();
    return;
  }

  SetWmStateProperty(e.window, 0);  // WithdrawnState

  // Notify the focus manager last in case any event consumers need to do
  // something special when they see the focused window getting unmapped.
  focus_manager_->HandleWindowUnmap(win);

  if (mapped_xids_->Contains(win->xid())) {
    mapped_xids_->Remove(win->xid());
    UpdateClientListProperty();
    UpdateClientListStackingProperty();
  }
}

void WindowManager::RunCommand(string command) {
  if (command.empty())
    return;

  command += " &";
  DLOG(INFO) << "Running command \"" << command << "\"";
  if (system(command.c_str()) < 0)
    LOG(WARNING) << "Got error while running \"" << command << "\"";
}

XWindow WindowManager::GetArbitraryChromeWindow() {
  for (WindowMap::const_iterator i = client_windows_.begin();
       i != client_windows_.end(); ++i) {
    if (WmIpcWindowTypeIsChrome(i->second->type()))
      return i->first;
  }
  return 0;
}

void WindowManager::SendNotifySyskeyMessage(chromeos::WmIpcSystemKey key) {
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_SYSKEY_PRESSED);
  msg.set_param(0, key);
  const XWindow chrome_window = GetArbitraryChromeWindow();
  if (chrome_window) {
    DLOG(INFO) << "Sending syskey notification with param " << key;
    wm_ipc()->SendMessage(chrome_window, msg);
  } else {
    LOG(WARNING) << "Not sending syskey notification: "
                 << "Chrome currently doesn't have any windows open.";
  }
}

void WindowManager::ToggleHotkeyOverlay() {
  Compositor::Actor* group = hotkey_overlay_->group();
  showing_hotkey_overlay_ = !showing_hotkey_overlay_;
  if (showing_hotkey_overlay_) {
    QueryKeyboardState();
    group->SetOpacity(0, 0);
    group->Show();
    group->SetOpacity(1, kHotkeyOverlayAnimMs);
    DCHECK_EQ(query_keyboard_state_timeout_id_, -1);
    query_keyboard_state_timeout_id_ =
        event_loop_->AddTimeout(
            NewPermanentCallback(this, &WindowManager::QueryKeyboardState),
            0, kHotkeyOverlayPollMs);
  } else {
    group->SetOpacity(0, kHotkeyOverlayAnimMs);
    DCHECK_GE(query_keyboard_state_timeout_id_, 0);
    event_loop_->RemoveTimeout(query_keyboard_state_timeout_id_);
    query_keyboard_state_timeout_id_ = -1;
  }
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
  command += " " + filename;

  if (system(command.c_str()) < 0) {
    LOG(ERROR) << "Taking screenshot via \"" << command << "\" failed";
  } else {
    LOG(INFO) << "Saved screenshot to " << filename;
  }
}

void WindowManager::QueryKeyboardState() {
  vector<uint8_t> keycodes;
  xconn_->QueryKeyboardState(&keycodes);
  hotkey_overlay_->HandleKeyboardState(keycodes);
}

}  // namespace window_manager
