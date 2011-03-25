// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/window.h"

#include <algorithm>

#include <gflags/gflags.h>

#include "base/logging.h"
#include "base/string_util.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"
#include "window_manager/compositor/animation.h"
#include "window_manager/focus_manager.h"
#include "window_manager/geometry.h"
#include "window_manager/shadow.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window_manager.h"
#include "window_manager/x11/x_connection.h"

using std::map;
using std::max;
using std::min;
using std::set;
using std::string;
using std::tr1::shared_ptr;
using std::vector;
using window_manager::util::GetCurrentTimeSec;
using window_manager::util::XidStr;

DEFINE_bool(load_window_shapes, false,
            "Should we use the Shape extension to load shaped windows' "
            "bounding regions?  The compositing code doesn't currently support "
            "using these regions to mask windows, and we favor RGBA windows "
            "instead.");

namespace window_manager {

// We could technically just move windows to (XConnection::kMaxPosition,
// XConnection::kMaxPosition) to keep them offscreen (X11 appears to allow
// window contents to go beyond the 2**15 limit; it's just the origin that needs
// to fall within it), but GTK sometimes arranges override-redirect windows
// relative to offscreen windows, and it happily overflows the limit in this
// case, ending up with negative coordinates.
const int Window::kOffscreenX = (XConnection::kMaxPosition + 1) / 2;
const int Window::kOffscreenY = (XConnection::kMaxPosition + 1) / 2;

const int Window::kVideoMinWidth = 300;
const int Window::kVideoMinHeight = 225;
const int Window::kVideoMinFramerate = 15;

// Maximum size of |damage_debug_actors_|.  This is effectively the maximum
// number of damage events that we'll show onscreen at once for this window.
static const size_t kMaxDamageDebugActors = 8;

// Color for damage actors.
static const char kDamageDebugColor[] = "#d60";

// Starting opacity for damage actors.
static const double kDamageDebugOpacity = 0.25;

// Duration in milliseconds over which a damage actor's opacity fades to 0.
static const int kDamageDebugFadeMs = 200;

Window::Window(WindowManager* wm,
               XWindow xid,
               bool override_redirect,
               const XConnection::WindowGeometry& geometry)
    : xid_(xid),
      xid_str_(XidStr(xid_)),
      wm_(wm),
      actor_(wm_->compositor()->CreateTexturePixmap()),
      transient_for_xid_(None),
      override_redirect_(override_redirect),
      mapped_(false),
      shaped_(false),
      type_(chromeos::WM_IPC_WINDOW_UNKNOWN),
      visibility_(VISIBILITY_UNSET),
      update_client_position_for_moves_(true),
      client_x_(geometry.bounds.x),
      client_y_(geometry.bounds.y),
      client_width_(geometry.bounds.width),
      client_height_(geometry.bounds.height),
      client_depth_(geometry.depth),
      client_opacity_(1.0),
      composited_shown_(false),
      composited_x_(geometry.bounds.x),
      composited_y_(geometry.bounds.y),
      composited_scale_x_(1.0),
      composited_scale_y_(1.0),
      composited_opacity_(1.0),
      actor_gravity_(GRAVITY_NORTHWEST),
      shadow_opacity_(1.0),
      supports_wm_take_focus_(false),
      supports_wm_delete_window_(false),
      supports_wm_ping_(false),
      wm_state_fullscreen_(false),
      wm_state_maximized_horz_(false),
      wm_state_maximized_vert_(false),
      wm_state_modal_(false),
      wm_hint_urgent_(false),
      damage_(0),
      pixmap_(0),
      need_to_reset_pixmap_(false),
      wm_sync_request_alarm_(0),
      current_wm_sync_num_(0),
      client_has_redrawn_after_last_resize_(true),
      updates_frozen_(false),
      client_pid_(-1),
      num_video_damage_events_(0),
      video_damage_start_time_(-1) {
  DCHECK(xid_);
  DLOG(INFO) << "Constructing object to track "
             << (override_redirect_ ? "override-redirect " : "")
             << "window " << xid_str() << " at " << geometry.bounds;

  // Listen for property and shape changes on this window.
  wm_->xconn()->SelectInputOnWindow(xid_, PropertyChangeMask, true);
  wm_->xconn()->SelectShapeEventsOnWindow(xid_);

  // If the window has a border, remove it -- borders make things more
  // confusing (we'd need to include the border when telling the compositor
  // the window's position, but it's not included when telling X to resize
  // the window, etc.).
  if (geometry.border_width > 0)
    wm_->xconn()->SetWindowBorderWidth(xid_, 0);

  damage_ = wm_->xconn()->CreateDamage(
      xid_, XConnection::DAMAGE_REPORT_LEVEL_BOUNDING_BOX);

  actor_->Move(composited_x_, composited_y_, 0);
  actor_->Hide();
  // TODO(derat): Move this stuff to WindowManager::TrackWindow() instead.
  wm_->stage()->AddActor(actor_.get());
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      this, StackingManager::LAYER_TOP_CLIENT_WINDOW);

  // Various properties could've been set on this window after it was
  // created but before we selected PropertyChangeMask, so we need to query
  // them here.
  FetchAndApplyTitle();
  FetchAndApplyWindowType();
  FetchAndApplyShape();
  FetchAndApplyWindowOpacity();
  FetchAndApplySizeHints();
  FetchAndApplyWmProtocols();
  FetchAndApplyWmState();
  FetchAndApplyChromeState();
  FetchAndApplyTransientHint();
  FetchAndApplyWmHints();
  FetchAndApplyWmWindowType();
  FetchAndApplyWmClientMachine();
  FetchAndApplyWmPid();
  FetchAndApplyChromeFreezeUpdates();
}

Window::~Window() {
  if (damage_)
    wm_->xconn()->DestroyDamage(damage_);
  if (pixmap_)
    wm_->xconn()->FreePixmap(pixmap_);
  DestroyWmSyncRequestAlarm();
}

bool Window::IsFocused() const {
  return wm_->focus_manager()->focused_win() == this;
}

void Window::FetchAndApplyTitle() {
  DCHECK(xid_);
  DCHECK(actor_.get());

  title_.clear();
  wm_->xconn()->GetStringProperty(
      xid_, wm_->GetXAtom(ATOM_NET_WM_NAME), &title_);

  if (title_.empty())
    actor_->SetName(string("window ") + xid_str());
  else
    actor_->SetName(string("window '") + title_ + "' (" + xid_str() + ")");
}

bool Window::FetchAndApplySizeHints() {
  DCHECK(xid_);
  if (!wm_->xconn()->GetSizeHintsForWindow(xid_, &size_hints_))
    return false;

  const XConnection::SizeHints& h = size_hints_;
  DLOG(INFO) << "Got size hints for " << xid_str() << ":"
             << " size=" << h.size
             << " min_size=" << h.min_size
             << " max_size=" << h.max_size
             << " inc=" << h.size_increment
             << " min_aspect=" << h.min_aspect_ratio
             << " max_aspect=" << h.max_aspect_ratio
             << " base=" << h.base_size;

  // If windows are override-redirect or have already been mapped, they
  // should just make/request any desired changes directly.  Also ignore
  // position, aspect ratio, etc. hints for now.
  if (!mapped_ && !override_redirect_ &&
      (size_hints_.size.width > 0 && size_hints_.size.height > 0)) {
    ResizeClient(size_hints_.size.width, size_hints_.size.height,
                 GRAVITY_NORTHWEST);
  }

  return true;
}

bool Window::FetchAndApplyTransientHint() {
  DCHECK(xid_);
  XWindow prev_transient_for_xid = transient_for_xid_;
  if (!wm_->xconn()->GetTransientHintForWindow(xid_, &transient_for_xid_))
    return false;
  if (transient_for_xid_ != prev_transient_for_xid) {
    DLOG(INFO) << "Window " << xid_str() << " is transient for "
               << XidStr(transient_for_xid_);
  }
  return true;
}

bool Window::FetchAndApplyWindowType() {
  DCHECK(xid_);
  bool result = wm_->wm_ipc()->GetWindowType(xid_, &type_, &type_params_);
  DLOG(INFO) << "Window " << xid_str() << " has type " << type_
             << " (" << type_str() << ")";
  return result;
}

void Window::FetchAndApplyWindowOpacity() {
  DCHECK(xid_);
  static const uint32 kMaxOpacity = 0xffffffffU;

  uint32 opacity = kMaxOpacity;
  wm_->xconn()->GetIntProperty(
      xid_,
      wm_->GetXAtom(ATOM_NET_WM_WINDOW_OPACITY),
      reinterpret_cast<int32*>(&opacity));
  client_opacity_ = (opacity == kMaxOpacity) ?
      1.0 : static_cast<double>(opacity) / kMaxOpacity;

  // TODO: It'd be nicer if we didn't interrupt any in-progress opacity
  // animations.
  SetCompositedOpacity(composited_opacity_, 0);
}

void Window::FetchAndApplyWmHints() {
  DCHECK(xid_);
  vector<int> wm_hints;
  if (!wm_->xconn()->GetIntArrayProperty(
          xid_, wm_->GetXAtom(ATOM_WM_HINTS), &wm_hints)) {
    return;
  }

  const uint32_t flags = wm_hints[0];
  wm_hint_urgent_ = flags & (1L << 8);  // XUrgencyHint from Xutil.h
}

void Window::FetchAndApplyWmProtocols() {
  DCHECK(xid_);
  supports_wm_take_focus_ = false;
  supports_wm_delete_window_ = false;
  supports_wm_ping_ = false;
  bool supports_wm_sync_request = false;

  vector<int> wm_protocols;
  if (!wm_->xconn()->GetIntArrayProperty(
          xid_, wm_->GetXAtom(ATOM_WM_PROTOCOLS), &wm_protocols)) {
    return;
  }

  const XAtom wm_take_focus = wm_->GetXAtom(ATOM_WM_TAKE_FOCUS);
  const XAtom wm_delete_window = wm_->GetXAtom(ATOM_WM_DELETE_WINDOW);
  const XAtom wm_ping = wm_->GetXAtom(ATOM_NET_WM_PING);
  const XAtom wm_sync_request = wm_->GetXAtom(ATOM_NET_WM_SYNC_REQUEST);
  for (vector<int>::const_iterator it = wm_protocols.begin();
       it != wm_protocols.end(); ++it) {
    if (static_cast<XAtom>(*it) == wm_take_focus) {
      DLOG(INFO) << "Window " << xid_str() << " supports WM_TAKE_FOCUS";
      supports_wm_take_focus_ = true;
    } else if (static_cast<XAtom>(*it) == wm_delete_window) {
      DLOG(INFO) << "Window " << xid_str() << " supports WM_DELETE_WINDOW";
      supports_wm_delete_window_ = true;
    } else if (static_cast<XAtom>(*it) == wm_ping) {
      DLOG(INFO) << "Window " << xid_str() << " supports _NET_WM_PING";
      supports_wm_ping_ = true;
    } else if (static_cast<XAtom>(*it) == wm_sync_request) {
      DLOG(INFO) << "Window " << xid_str() << " supports _NET_WM_SYNC_REQUEST";
      supports_wm_sync_request = true;
    }
  }

  // Don't check the property again if we already have a counter.
  if (supports_wm_sync_request && !wm_sync_request_alarm_) {
    if (!FetchAndApplyWmSyncRequestCounterProperty())
      supports_wm_sync_request = false;
  }

  if (!supports_wm_sync_request && wm_sync_request_alarm_)
    DestroyWmSyncRequestAlarm();
}

bool Window::FetchAndApplyWmSyncRequestCounterProperty() {
  DCHECK(!wm_sync_request_alarm_);

  int counter = 0;
  if (!wm_->xconn()->GetIntProperty(
          xid_, wm_->GetXAtom(ATOM_NET_WM_SYNC_REQUEST_COUNTER), &counter)) {
    LOG(WARNING) << "Didn't find a _NET_WM_SYNC_REQUEST_COUNTER property on "
                 << "window " << xid_str();
    return false;
  }

  XID counter_xid = static_cast<XID>(counter);
  current_wm_sync_num_ = 10;  // arbitrary, but not the default of 0
  wm_->xconn()->SetSyncCounter(counter_xid, current_wm_sync_num_);
  wm_sync_request_alarm_ = wm_->xconn()->CreateSyncCounterAlarm(
      counter_xid, current_wm_sync_num_ + 1);
  if (!wm_sync_request_alarm_)
    return false;
  wm_->RegisterSyncAlarm(wm_sync_request_alarm_, this);

  DLOG(INFO) << "Created sync alarm " << XidStr(wm_sync_request_alarm_)
             << " on counter " << XidStr(counter_xid) << " for window "
             << xid_str();
  return true;
}

void Window::FetchAndApplyWmState() {
  DCHECK(xid_);
  wm_state_fullscreen_ = false;
  wm_state_maximized_horz_ = false;
  wm_state_maximized_vert_ = false;
  wm_state_modal_ = false;

  vector<int> state_atoms;
  if (!wm_->xconn()->GetIntArrayProperty(
          xid_, wm_->GetXAtom(ATOM_NET_WM_STATE), &state_atoms)) {
    return;
  }

  XAtom fullscreen_atom = wm_->GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN);
  XAtom max_horz_atom = wm_->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_HORZ);
  XAtom max_vert_atom = wm_->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_VERT);
  XAtom modal_atom = wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL);
  for (vector<int>::const_iterator it = state_atoms.begin();
       it != state_atoms.end(); ++it) {
    XAtom atom = static_cast<XAtom>(*it);
    if (atom == fullscreen_atom)
      wm_state_fullscreen_ = true;
    if (atom == max_horz_atom)
      wm_state_maximized_horz_ = true;
    if (atom == max_vert_atom)
      wm_state_maximized_vert_ = true;
    else if (atom == modal_atom)
      wm_state_modal_ = true;
  }

  DLOG(INFO) << "Fetched _NET_WM_STATE for " << xid_str() << ":"
             << " fullscreen=" << wm_state_fullscreen_
             << " maximized_horz=" << wm_state_maximized_horz_
             << " maximized_vert=" << wm_state_maximized_vert_
             << " modal=" << wm_state_modal_;
}

void Window::FetchAndApplyWmWindowType() {
  DCHECK(xid_);
  wm_window_type_xatoms_.clear();

  vector<int> window_type_ints;
  if (!wm_->xconn()->GetIntArrayProperty(
          xid_, wm_->GetXAtom(ATOM_NET_WM_WINDOW_TYPE), &window_type_ints)) {
    return;
  }

  for (vector<int>::const_iterator it = window_type_ints.begin();
       it != window_type_ints.end(); ++it) {
    wm_window_type_xatoms_.push_back(static_cast<XAtom>(*it));
  }
}

void Window::FetchAndApplyChromeState() {
  DCHECK(xid_);
  XAtom state_xatom = wm_->GetXAtom(ATOM_CHROME_STATE);
  chrome_state_xatoms_.clear();
  vector<int> state_xatoms;
  if (!wm_->xconn()->GetIntArrayProperty(xid_, state_xatom, &state_xatoms))
    return;

  string debug_str;
  for (vector<int>::const_iterator it = state_xatoms.begin();
       it != state_xatoms.end(); ++it) {
    chrome_state_xatoms_.insert(static_cast<XAtom>(*it));
    if (!debug_str.empty())
      debug_str += " ";
    debug_str += wm_->GetXAtomName(static_cast<XAtom>(*it));
  }
  DLOG(INFO) << "Fetched " << wm_->GetXAtomName(state_xatom) << " for "
             << xid_str() << ": " << debug_str;
}

void Window::FetchAndApplyWmClientMachine() {
  DCHECK(xid_);
  client_hostname_.clear();
  wm_->xconn()->GetStringProperty(
      xid_, wm_->GetXAtom(ATOM_WM_CLIENT_MACHINE), &client_hostname_);
  if (!client_hostname_.empty()) {
    DLOG(INFO) << "Client owning window " << xid_str() << " is running on "
               << "host \"" << client_hostname_ << "\"";
  }
}

void Window::FetchAndApplyWmPid() {
  DCHECK(xid_);
  client_pid_ = -1;
  wm_->xconn()->GetIntProperty(
      xid_, wm_->GetXAtom(ATOM_NET_WM_PID), &client_pid_);
  DLOG(INFO) << "Client owning window " << xid_str() << " has PID "
             << client_pid_;
}

void Window::FetchAndApplyChromeFreezeUpdates() {
  DCHECK(xid_);
  int dummy_value = 0;
  bool property_exists =
      wm_->xconn()->GetIntProperty(
          xid_, wm_->GetXAtom(ATOM_CHROME_FREEZE_UPDATES), &dummy_value);
  HandleFreezeUpdatesPropertyChange(property_exists);
}

void Window::FetchAndApplyShape() {
  DCHECK(xid_);
  DCHECK(actor_.get());
  shaped_ = false;

  // We don't grab the server around these two requests, so it's possible
  // that a shaped window will have become unshaped between them and we'll
  // think that the window is shaped but get back an unshaped region.  This
  // should be okay; we should get another ShapeNotify event for the window
  // becoming unshaped and clear the useless mask then.
  if (wm_->xconn()->IsWindowShaped(xid_)) {
    shaped_ = true;

    if (FLAGS_load_window_shapes) {
      ByteMap bytemap(client_width_, client_height_);
      if (wm_->xconn()->GetWindowBoundingRegion(xid_, &bytemap)) {
        DLOG(INFO) << "Got shape for " << xid_str();
        actor_->SetAlphaMask(
            bytemap.bytes(), bytemap.width(), bytemap.height());
      } else {
        shaped_ = false;
      }
    }
  }

  if (FLAGS_load_window_shapes && !shaped_)
    actor_->ClearAlphaMask();

  UpdateShadowVisibility();
}

bool Window::FetchMapState() {
  DCHECK(xid_);
  XConnection::WindowAttributes attr;
  if (!wm_->xconn()->GetWindowAttributes(xid_, &attr))
    return false;
  return (attr.map_state != XConnection::WindowAttributes::MAP_STATE_UNMAPPED);
}

void Window::ParseWmStateMessage(const long data[5],
                                 map<XAtom, bool>* states_out) const {
  DCHECK(xid_);
  DCHECK(states_out);
  states_out->clear();

  XAtom fullscreen_atom = wm_->GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN);
  if (static_cast<XAtom>(data[1]) == fullscreen_atom ||
      static_cast<XAtom>(data[2]) == fullscreen_atom) {
    bool value = wm_state_fullscreen_;
    SetWmStateInternal(data[0], &value);
    (*states_out)[fullscreen_atom] = value;
  }
  XAtom modal_atom = wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL);
  if (static_cast<XAtom>(data[1]) == modal_atom ||
      static_cast<XAtom>(data[2]) == modal_atom) {
    bool value = wm_state_modal_;
    SetWmStateInternal(data[0], &value);
    (*states_out)[modal_atom] = value;
  }

  // We don't let clients toggle their maximized state currently.
}

bool Window::ChangeWmState(const map<XAtom, bool>& states) {
  DCHECK(xid_);
  for (map<XAtom, bool>::const_iterator it = states.begin();
       it != states.end(); ++it) {
    XAtom xatom = it->first;
    int action = it->second;  // 0 is remove, 1 is add

    if (xatom == wm_->GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN))
      SetWmStateInternal(action, &wm_state_fullscreen_);
    else if (xatom == wm_->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_HORZ))
      SetWmStateInternal(action, &wm_state_maximized_horz_);
    else if (xatom == wm_->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_VERT))
      SetWmStateInternal(action, &wm_state_maximized_vert_);
    else if (xatom == wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL))
      SetWmStateInternal(action, &wm_state_modal_);
    else
      LOG(ERROR) << "Unsupported _NET_WM_STATE " << xatom
                 << " for " << xid_str();
  }
  return UpdateWmStateProperty();
}

bool Window::ChangeChromeState(const map<XAtom, bool>& states) {
  DCHECK(xid_);
  for (map<XAtom, bool>::const_iterator it = states.begin();
       it != states.end(); ++it) {
    if (it->second)
      chrome_state_xatoms_.insert(it->first);
    else
      chrome_state_xatoms_.erase(it->first);
  }
  return UpdateChromeStateProperty();
}

bool Window::TakeFocus(XTime timestamp) {
  DLOG(INFO) << "Focusing " << xid_str() << " using time " << timestamp;
  DCHECK(xid_);
  if (supports_wm_take_focus_) {
    long data[5];
    memset(data, 0, sizeof(data));
    data[0] = wm_->GetXAtom(ATOM_WM_TAKE_FOCUS);
    data[1] = timestamp;
    if (!wm_->xconn()->SendClientMessageEvent(
             xid_, xid_, wm_->GetXAtom(ATOM_WM_PROTOCOLS), data, 0)) {
      return false;
    }
  } else {
    if (!wm_->xconn()->FocusWindow(xid_, timestamp))
      return false;
  }
  return true;
}

bool Window::SendDeleteRequest(XTime timestamp) {
  DLOG(INFO) << "Maybe asking " << xid_str() << " to delete itself with time "
             << timestamp;
  DCHECK(xid_);
  if (!supports_wm_delete_window_)
    return false;

  long data[5];
  memset(data, 0, sizeof(data));
  data[0] = wm_->GetXAtom(ATOM_WM_DELETE_WINDOW);
  data[1] = timestamp;
  return wm_->xconn()->SendClientMessageEvent(
            xid_, xid_, wm_->GetXAtom(ATOM_WM_PROTOCOLS), data, 0);
}

bool Window::SendPing(XTime timestamp) {
  DCHECK(xid_);
  if (!supports_wm_ping_)
    return false;

  long data[5];
  memset(data, 0, sizeof(data));
  data[0] = wm_->GetXAtom(ATOM_NET_WM_PING);
  data[1] = timestamp;
  data[2] = xid_;
  return wm_->xconn()->SendClientMessageEvent(
            xid_, xid_, wm_->GetXAtom(ATOM_WM_PROTOCOLS), data, 0);
}

bool Window::AddButtonGrab() {
  DLOG(INFO) << "Adding button grab for " << xid_str();
  DCHECK(xid_);
  return wm_->xconn()->AddButtonGrabOnWindow(
      xid_, AnyButton, ButtonPressMask, true);  // synchronous=true
}

bool Window::RemoveButtonGrab() {
  DLOG(INFO) << "Removing button grab for " << xid_str();
  DCHECK(xid_);
  return wm_->xconn()->RemoveButtonGrabOnWindow(xid_, AnyButton);
}

void Window::GetMaxSize(int desired_width, int desired_height,
                        int* width_out, int* height_out) const {
  CHECK(desired_width > 0);
  CHECK(desired_height > 0);

  if (size_hints_.max_size.width > 0)
    desired_width = min(size_hints_.max_size.width, desired_width);
  if (size_hints_.min_size.width > 0)
    desired_width = max(size_hints_.min_size.width, desired_width);

  if (size_hints_.size_increment.width > 0) {
    int base_width =
        (size_hints_.base_size.width > 0) ? size_hints_.base_size.width :
        (size_hints_.min_size.width > 0) ? size_hints_.min_size.width :
        0;
    *width_out = base_width +
        ((desired_width - base_width) / size_hints_.size_increment.width) *
        size_hints_.size_increment.width;
  } else {
    *width_out = desired_width;
  }

  if (size_hints_.max_size.height > 0)
    desired_height = min(size_hints_.max_size.height, desired_height);
  if (size_hints_.min_size.height > 0)
    desired_height = max(size_hints_.min_size.height, desired_height);

  if (size_hints_.size_increment.height > 0) {
    int base_height =
        (size_hints_.base_size.height > 0) ? size_hints_.base_size.height :
        (size_hints_.min_size.height > 0) ? size_hints_.min_size.height :
        0;
    *height_out = base_height +
        ((desired_height - base_height) / size_hints_.size_increment.height) *
        size_hints_.size_increment.height;
  } else {
    *height_out = desired_height;
  }

  DLOG(INFO) << "Max size for " << xid_str() << " is "
             << *width_out << "x" << *height_out
             << " (desired was " << desired_width << "x"
             << desired_height << ")";
}

bool Window::MapClient() {
  DLOG(INFO) << "Mapping " << xid_str();
  DCHECK(xid_);
  if (!wm_->xconn()->MapWindow(xid_))
    return false;
  return true;
}

bool Window::UnmapClient() {
  DLOG(INFO) << "Unmapping " << xid_str();
  DCHECK(xid_);
  if (!wm_->xconn()->UnmapWindow(xid_))
    return false;
  return true;
}

void Window::SetVisibility(Visibility visibility) {
  DCHECK_NE(visibility, VISIBILITY_UNSET) << " xid=" << xid_str_;
  if (visibility == visibility_)
    return;

  visibility_ = visibility;

  DCHECK(actor_.get());
  switch (visibility) {
    case VISIBILITY_SHOWN:  // fallthrough
    case VISIBILITY_SHOWN_NO_INPUT:
      actor_->Show();
      if (damage_debug_group_.get())
        damage_debug_group_->Show();
      break;
    case VISIBILITY_HIDDEN:
      actor_->Hide();
      if (damage_debug_group_.get())
        damage_debug_group_->Hide();
      break;
    default:
      NOTREACHED() << "Unknown visibility setting " << visibility;
  }
  UpdateShadowVisibility();
  UpdateClientWindowPosition();
}

void Window::SetUpdateClientPositionForMoves(bool update) {
  DCHECK_NE(visibility_, VISIBILITY_UNSET) << " xid=" << xid_str_;
  if (update_client_position_for_moves_ == update)
    return;
  update_client_position_for_moves_ = update;
  if (update_client_position_for_moves_)
    UpdateClientWindowPosition();
}

void Window::Move(const Point& origin, int anim_ms) {
  DCHECK_NE(visibility_, VISIBILITY_UNSET) << " xid=" << xid_str_;
  MoveCompositedInternal(origin, MOVE_DIMENSIONS_X_AND_Y, anim_ms);
  if (update_client_position_for_moves_)
    UpdateClientWindowPosition();
}

void Window::MoveX(int x, int anim_ms) {
  DCHECK_NE(visibility_, VISIBILITY_UNSET) << " xid=" << xid_str_;
  MoveCompositedInternal(Point(x, 0), MOVE_DIMENSIONS_X_ONLY, anim_ms);
  if (update_client_position_for_moves_)
    UpdateClientWindowPosition();
}

void Window::MoveY(int y, int anim_ms) {
  DCHECK_NE(visibility_, VISIBILITY_UNSET) << " xid=" << xid_str_;
  MoveCompositedInternal(Point(0, y), MOVE_DIMENSIONS_Y_ONLY, anim_ms);
  if (update_client_position_for_moves_)
    UpdateClientWindowPosition();
}

bool Window::MoveClient(int x, int y) {
  DCHECK_EQ(visibility_, VISIBILITY_UNSET) << " xid=" << xid_str_;
  return MoveClientInternal(Point(x, y));
}

bool Window::MoveClientOffscreen() {
  return MoveClient(kOffscreenX, kOffscreenY);
}

bool Window::MoveClientToComposited() {
  return MoveClient(composited_x_, composited_y_);
}

bool Window::CenterClientOverWindow(Window* win) {
  CHECK(win);
  int center_x = win->client_x() + 0.5 * win->client_width();
  int center_y = win->client_y() + 0.5 * win->client_height();
  return MoveClient(center_x - 0.5 * client_width_,
                    center_y - 0.5 * client_height_);
}

bool Window::ResizeClient(int width, int height, Gravity gravity) {
  DCHECK(xid_);

  // Bail out early if this is a no-op.  (No-op resizes won't generate
  // ConfigureNotify events, which means that the client won't know to
  // redraw and update the _NET_WM_SYNC_REQUEST counter.)
  if (width == client_width_ && height == client_height_)
    return true;

  SendWmSyncRequestMessage();

  int dx = (gravity == GRAVITY_NORTHEAST || gravity == GRAVITY_SOUTHEAST) ?
      width - client_width_ : 0;
  int dy = (gravity == GRAVITY_SOUTHWEST || gravity == GRAVITY_SOUTHEAST) ?
      height - client_height_ : 0;

  DLOG(INFO) << "Resizing " << xid_str() << "'s client window to "
             << width << "x" << height;
  if (dx || dy) {
    // If we need to move the window as well due to gravity, do it all in
    // one ConfigureWindow request to the server.
    if (!wm_->xconn()->ConfigureWindow(
            xid_, Rect(client_x_ - dx, client_y_ - dy, width, height))) {
      return false;
    }
    SaveClientPosition(client_x_ - dx, client_y_ - dy);
    composited_x_ -= dx * composited_scale_x_;
    composited_y_ -= dy * composited_scale_y_;
  } else  {
    if (!wm_->xconn()->ResizeWindow(xid_, Size(width, height)))
      return false;
  }

  actor_gravity_ = gravity;
  SaveClientSize(width, height);
  return true;
}

bool Window::StackClientAbove(XWindow sibling_xid) {
  DCHECK(xid_);
  CHECK(sibling_xid != None);
  bool result = wm_->xconn()->StackWindow(xid_, sibling_xid, true);
  return result;
}

bool Window::StackClientBelow(XWindow sibling_xid) {
  DCHECK(xid_);
  CHECK(sibling_xid != None);
  bool result = wm_->xconn()->StackWindow(xid_, sibling_xid, false);
  return result;
}

void Window::MoveComposited(int x, int y, int anim_ms) {
  DCHECK_EQ(visibility_, VISIBILITY_UNSET) << " xid=" << xid_str_;
  MoveCompositedInternal(Point(x, y), MOVE_DIMENSIONS_X_AND_Y, anim_ms);
}

void Window::MoveCompositedX(int x, int anim_ms) {
  DCHECK_EQ(visibility_, VISIBILITY_UNSET) << " xid=" << xid_str_;
  MoveCompositedInternal(Point(x, 0), MOVE_DIMENSIONS_X_ONLY, anim_ms);
}

void Window::MoveCompositedY(int y, int anim_ms) {
  DCHECK_EQ(visibility_, VISIBILITY_UNSET) << " xid=" << xid_str_;
  MoveCompositedInternal(Point(0, y), MOVE_DIMENSIONS_Y_ONLY, anim_ms);
}

void Window::MoveCompositedToClient() {
  MoveComposited(client_x_, client_y_, 0);
}

void Window::ShowComposited() {
  DLOG(INFO) << "Showing " << xid_str() << "'s composited window";
  DCHECK(actor_.get());
  DCHECK_EQ(visibility_, VISIBILITY_UNSET) << " xid=" << xid_str_;
  actor_->Show();
  composited_shown_ = true;
  UpdateShadowVisibility();
  if (damage_debug_group_.get())
    damage_debug_group_->Show();
}

void Window::HideComposited() {
  DLOG(INFO) << "Hiding " << xid_str() << "'s composited window";
  DCHECK(actor_.get());
  DCHECK_EQ(visibility_, VISIBILITY_UNSET) << " xid=" << xid_str_;
  actor_->Hide();
  composited_shown_ = false;
  UpdateShadowVisibility();
  if (damage_debug_group_.get())
    damage_debug_group_->Hide();
}

void Window::SetCompositedOpacity(double opacity, int anim_ms) {
  composited_opacity_ = opacity;
  DLOG(INFO) << "Setting " << xid_str() << "'s composited window opacity to "
             << opacity << " (combined is " << combined_opacity() << ") over "
             << anim_ms << " ms";
  DCHECK(actor_.get());

  actor_->SetOpacity(combined_opacity(), anim_ms);
  if (shadow_.get())
    shadow_->SetOpacity(combined_opacity() * shadow_opacity_, anim_ms);

  // If the window became completely transparent (or was and now isn't), we may
  // need to move the client window offscreen or back onscreen.
  if (visibility_ != VISIBILITY_UNSET)
    UpdateClientWindowPosition();

  if (damage_debug_group_.get())
    damage_debug_group_->SetOpacity(combined_opacity(), anim_ms);
}

void Window::ScaleComposited(double scale_x, double scale_y, int anim_ms) {
  DLOG(INFO) << "Scaling " << xid_str() << "'s composited window by ("
             << scale_x << ", " << scale_y << ") over " << anim_ms << " ms";
  DCHECK(actor_.get());
  DCHECK_GE(composited_scale_x_, 0.0);
  DCHECK_GE(composited_scale_y_, 0.0);
  composited_scale_x_ = scale_x;
  composited_scale_y_ = scale_y;

  actor_->Scale(scale_x, scale_y, anim_ms);
  if (shadow_.get())
    shadow_->Resize(scale_x * client_width_, scale_y * client_height_, anim_ms);

  // When the window's scale changes, we may need to move the client window
  // offscreen or back onscreen.
  if (visibility_ != VISIBILITY_UNSET)
    UpdateClientWindowPosition();

  if (damage_debug_group_.get())
    damage_debug_group_->Scale(scale_x, scale_y, anim_ms);
}

AnimationPair* Window::CreateMoveCompositedAnimation() {
  DCHECK(actor_.get());
  DCHECK(!shadow_.get());
  return actor_->CreateMoveAnimation();
}

void Window::SetMoveCompositedAnimation(AnimationPair* animations) {
  DCHECK(animations);
  DCHECK(actor_.get());
  composited_x_ = animations->first_animation().GetEndValue();
  composited_y_ = animations->second_animation().GetEndValue();
  DLOG(INFO) << "Setting custom animation to eventually move " << xid_str()
             << "'s composited window to (" << composited_x_ << "x"
             << composited_y_ << ")";
  actor_->SetMoveAnimation(animations);

  // Make sure that the client window is in the right position.
  if (visibility_ != VISIBILITY_UNSET)
    UpdateClientWindowPosition();

  if (damage_debug_group_.get())
    damage_debug_group_->Move(composited_x_, composited_y_, 0);
}

void Window::HandleMapRequested() {
  DCHECK(xid_);
  DCHECK(!override_redirect_);

  // Tell the client to notify us after it's repainted in response to the
  // next ConfigureNotify that it receives, and then send a synthetic
  // ConfigureNotify event to the window.  This lets us avoid compositing
  // new windows until the client has painted them.
  if (wm_sync_request_alarm_) {
    SendWmSyncRequestMessage();
    SendSyntheticConfigureNotify();
  }
}

void Window::HandleMapNotify() {
  DCHECK(xid_);
  if (mapped_)
    return;

  mapped_ = true;
  need_to_reset_pixmap_ = true;

  // If we're still waiting for the client to redraw the window (probably in
  // response to the _NET_WM_SYNC_REQUEST message that we sent in
  // HandleMapRequested() or due to the client setting _CHROME_FREEZE_UPDATES
  // before mapping), then hold off on fetching the pixmap.  This makes us not
  // composite new windows until clients have painted them.
  if (able_to_reset_pixmap())
    ResetPixmap();
}

void Window::HandleUnmapNotify() {
  DCHECK(xid_);
  mapped_ = false;
  // We could potentially show a window onscreen even after it's been
  // unmapped, so we avoid hiding the shadow here.
}

void Window::HandleRedirect() {
  if (!mapped_)
    return;

  need_to_reset_pixmap_ = true;
  ResetPixmap();

  // If the window is in the middle of an animation (sliding offscreen),
  // its client position is already updated to the final position, and its
  // composited position is one frame into the animation because we've
  // already updated the coordinates prior to calling this method. However,
  // the content of the root window has not yet repainted, so using the
  // coordinates of the root window (0, 0)-(width, height) for the copying
  // will work while the coordinates of the window will not.
  wm_->xconn()->CopyArea(wm_->root(),
                         pixmap_,
                         Point(0, 0),  // src
                         Point(0, 0),  // dest
                         Size(wm_->width(), wm_->height()));
}

void Window::HandleConfigureNotify(int width, int height) {
  DCHECK(actor_.get());
  const bool size_changed =
      actor_->GetWidth() != width || actor_->GetHeight() != height;
  // Hold off on grabbing the window's contents if we haven't received
  // notification that the client has drawn to the new pixmap yet.
  if (size_changed) {
    need_to_reset_pixmap_ = true;
    if (able_to_reset_pixmap())
      ResetPixmap();
  }
}

void Window::HandleDamageNotify(const Rect& bounding_box) {
  DCHECK(actor_.get());
  wm_->xconn()->ClearDamage(damage_);
  actor_->UpdateTexture();
  actor_->MergeDamagedRegion(bounding_box);

  if (wm_->damage_debugging_enabled())
    UpdateDamageDebugging(bounding_box);

  // Check if this update could indicate that a video is playing.
  if (!IsClientWindowOffscreen() &&
      bounding_box.width >= kVideoMinWidth &&
      bounding_box.height >= kVideoMinHeight) {
    const time_t now = GetCurrentTimeSec();
    if (now != video_damage_start_time_) {
      video_damage_start_time_ = now;
      num_video_damage_events_ = 0;
    }
    num_video_damage_events_++;
    if (num_video_damage_events_ == kVideoMinFramerate)
      wm_->SetVideoTimeProperty(now);
  }
}

void Window::HandleFreezeUpdatesPropertyChange(bool frozen) {
  if (frozen == updates_frozen_)
    return;

  DLOG(INFO) << "Updates are " << (frozen ? "" : "un") << "frozen on window "
             << xid_str_;
  updates_frozen_ = frozen;

  if (need_to_reset_pixmap_ && able_to_reset_pixmap())
    ResetPixmap();
}

DestroyedWindow* Window::HandleDestroyNotify() {
  DCHECK(xid_);
  DCHECK(actor_.get());
  DestroyedWindow* destroyed_win =
      new DestroyedWindow(
          wm_, xid_, actor_.release(), shadow_.release(), pixmap_);
  pixmap_ = 0;
  xid_ = 0;
  return destroyed_win;
}

void Window::SetShadowType(Shadow::Type type) {
  DCHECK(actor_.get());

  shadow_.reset(Shadow::Create(wm_->compositor(), type));
  shadow_->group()->SetName(string("shadow group for window " + xid_str()));
  wm_->stage()->AddActor(shadow_->group());
  shadow_->group()->Lower(actor_.get());
  shadow_->Move(composited_x_, composited_y_, 0);
  shadow_->SetOpacity(combined_opacity() * shadow_opacity_, 0);
  shadow_->Resize(composited_scale_x_ * actor_->GetWidth(),
                  composited_scale_y_ * actor_->GetHeight(), 0);
  UpdateShadowVisibility();
}

void Window::DisableShadow() {
  shadow_.reset(NULL);
}

void Window::SetShadowOpacity(double opacity, int anim_ms) {
  DLOG(INFO) << "Setting " << xid_str() << "'s shadow opacity to " << opacity
             << " over " << anim_ms << " ms";
  shadow_opacity_ = opacity;
  if (shadow_.get())
    shadow_->SetOpacity(combined_opacity() * shadow_opacity_, anim_ms);
}

void Window::StackCompositedAbove(Compositor::Actor* actor,
                                  Compositor::Actor* shadow_actor,
                                  bool stack_above_shadow_actor) {
  DCHECK(actor_.get());
  if (actor)
    actor_->Raise(actor);
  if (shadow_.get()) {
    if (!shadow_actor || !stack_above_shadow_actor) {
      shadow_->group()->Lower(shadow_actor ? shadow_actor : actor_.get());
    } else {
      shadow_->group()->Raise(shadow_actor);
    }
  }
  if (damage_debug_group_.get())
    damage_debug_group_->Raise(actor_.get());
}

void Window::StackCompositedBelow(Compositor::Actor* actor,
                                  Compositor::Actor* shadow_actor,
                                  bool stack_above_shadow_actor) {
  DCHECK(actor_.get());
  if (actor)
    actor_->Lower(actor);
  if (shadow_.get()) {
    if (!shadow_actor || !stack_above_shadow_actor) {
      shadow_->group()->Lower(shadow_actor ? shadow_actor : actor_.get());
    } else {
      shadow_->group()->Raise(shadow_actor);
    }
  }
  if (damage_debug_group_.get())
    damage_debug_group_->Raise(actor_.get());
}

Compositor::Actor* Window::GetTopActor() {
  DCHECK(actor_.get());
  return damage_debug_group_.get() ?
      static_cast<Compositor::Actor*>(damage_debug_group_.get()) :
      static_cast<Compositor::Actor*>(actor_.get());
}

Compositor::Actor* Window::GetBottomActor() {
  DCHECK(actor_.get());
  return (shadow_.get() ? shadow_->group() : actor_.get());
}

void Window::CopyClientBoundsToRect(Rect* rect) const {
  DCHECK(rect);
  rect->x = client_x_;
  rect->y = client_y_;
  rect->width = client_width_;
  rect->height = client_height_;
}

void Window::HandleSyncAlarmNotify(XID alarm_id, int64_t value) {
  if (alarm_id != wm_sync_request_alarm_) {
    LOG(WARNING) << "Window " << xid_str() << " got sync alarm notify for "
                 << " unknown alarm " << XidStr(alarm_id);
    return;
  }

  DLOG(INFO) << "Window " << xid_str() << " handling sync alarm notify with "
             << "value " << value << " (current sync num is "
             << current_wm_sync_num_ << ")";
  if (value != current_wm_sync_num_ || client_has_redrawn_after_last_resize_)
    return;

  client_has_redrawn_after_last_resize_ = true;
  if (able_to_reset_pixmap())
    ResetPixmap();
}

void Window::SendSyntheticConfigureNotify() {
  const XWindow* xid_under_us_ptr = wm_->stacked_xids().GetUnder(xid_);
  const XWindow xid_under_us = xid_under_us_ptr ? *xid_under_us_ptr : 0;
  Rect rect(client_x_, client_y_, client_width_, client_height_);
  DLOG(INFO) << "Sending synthetic configure notify for " << xid_str() << ": "
             << rect << ", above " << XidStr(xid_under_us);
  wm_->xconn()->SendConfigureNotifyEvent(
      xid_,
      rect,
      0,  // border_width
      xid_under_us,
      false);  // override_redirect
}

bool Window::IsClientWindowOffscreen() const {
  return (client_x_ >= wm_->width() || client_x_ + client_width_ < 0 ||
          client_y_ >= wm_->height() || client_y_ + client_height_ < 0);
}

void Window::SetWmStateInternal(int action, bool* value) const {
  switch (action) {
    case 0:  // _NET_WM_STATE_REMOVE
      *value = false;
      break;
    case 1:  // _NET_WM_STATE_ADD
      *value = true;
      break;
    case 2:  // _NET_WM_STATE_TOGGLE
      *value = !(*value);
      break;
    default:
      LOG(WARNING) << "Got _NET_WM_STATE message for " << xid_str()
                   << " with invalid action " << action;
  }
}

bool Window::MoveClientInternal(const Point& origin) {
  DLOG(INFO) << "Moving " << xid_str() << "'s client window to " << origin;
  DCHECK(xid_);
  if (!wm_->xconn()->MoveWindow(xid_, origin))
    return false;
  SaveClientPosition(origin.x, origin.y);
  return true;
}

void Window::MoveCompositedInternal(const Point& origin,
                                    MoveDimensions dimensions,
                                    int anim_ms) {
  switch (dimensions) {
    case MOVE_DIMENSIONS_X_AND_Y:
      DLOG(INFO) << "Moving " << xid_str() << "'s composited window to "
                 << origin << " over " << anim_ms << " ms";
      composited_x_ = origin.x;
      composited_y_ = origin.y;
      break;
    case MOVE_DIMENSIONS_X_ONLY:
      DLOG(INFO) << "Moving " << xid_str() << "'s composited window's X "
                 << "position to " << origin.x << " over " << anim_ms << " ms";
      composited_x_ = origin.x;
      break;
    case MOVE_DIMENSIONS_Y_ONLY:
      DLOG(INFO) << "Moving " << xid_str() << "'s composited window's Y "
                 << "position to " << origin.y << " over " << anim_ms << " ms";
      composited_y_ = origin.y;
      break;
    default:
      NOTREACHED() << "Unknown move dimensions " << dimensions;
  }

  DCHECK(actor_.get());
  MoveActorToAdjustedPosition(dimensions, anim_ms);
}

void Window::UpdateClientWindowPosition() {
  DCHECK_NE(visibility_, VISIBILITY_UNSET) << " xid=" << xid_str_;
  if (override_redirect_)
    return;

  // Without support in X11 for transforming input events, scaled windows can't
  // receive input.
  const bool should_be_onscreen =
      visibility_ == VISIBILITY_SHOWN &&
      composited_width() == client_width_ &&
      composited_height() == client_height_ &&
      combined_opacity() > 0.0;

  Point cur_pos(client_x_, client_y_);
  Point new_pos = cur_pos;
  if (should_be_onscreen)
    new_pos.reset(composited_x_, composited_y_);
  else
    new_pos.reset(kOffscreenX, kOffscreenY);

  if (new_pos != cur_pos)
    MoveClientInternal(new_pos);
}

bool Window::UpdateWmStateProperty() {
  DCHECK(xid_);
  vector<int> values;
  if (wm_state_fullscreen_)
    values.push_back(wm_->GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN));
  if (wm_state_maximized_horz_)
    values.push_back(wm_->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_HORZ));
  if (wm_state_maximized_vert_)
    values.push_back(wm_->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_VERT));
  if (wm_state_modal_)
    values.push_back(wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL));

  DLOG(INFO) << "Updating _NET_WM_STATE for " << xid_str() << ":"
             << " fullscreen=" << wm_state_fullscreen_
             << " maximized_horz=" << wm_state_maximized_horz_
             << " maximized_vert=" << wm_state_maximized_vert_
             << " modal=" << wm_state_modal_;
  XAtom wm_state_atom = wm_->GetXAtom(ATOM_NET_WM_STATE);
  if (!values.empty()) {
    return wm_->xconn()->SetIntArrayProperty(
        xid_, wm_state_atom, wm_->GetXAtom(ATOM_ATOM), values);
  } else {
    return wm_->xconn()->DeletePropertyIfExists(xid_, wm_state_atom);
  }
}

bool Window::UpdateChromeStateProperty() {
  DCHECK(xid_);
  vector<int> values;
  for (set<XAtom>::const_iterator it = chrome_state_xatoms_.begin();
       it != chrome_state_xatoms_.end(); ++it) {
    values.push_back(static_cast<int>(*it));
  }

  XAtom state_xatom = wm_->GetXAtom(ATOM_CHROME_STATE);
  if (!values.empty()) {
    return wm_->xconn()->SetIntArrayProperty(
        xid_, state_xatom, wm_->GetXAtom(ATOM_ATOM), values);
  } else {
    return wm_->xconn()->DeletePropertyIfExists(xid_, state_xatom);
  }
}

void Window::DestroyWmSyncRequestAlarm() {
  if (!wm_sync_request_alarm_)
    return;
  wm_->xconn()->DestroySyncCounterAlarm(wm_sync_request_alarm_);
  wm_->UnregisterSyncAlarm(wm_sync_request_alarm_);
  wm_sync_request_alarm_ = 0;
  client_has_redrawn_after_last_resize_ = true;
}

void Window::MoveActorToAdjustedPosition(MoveDimensions dimensions,
                                         int anim_ms) {
  DCHECK(actor_.get());

  // Get the region that would be occupied by the actor if it were the same
  // size as the client window.
  Rect scaled_rect(composited_x_, composited_y_,
                   client_width_ * composited_scale_x_,
                   client_height_ * composited_scale_y_);

  // Now resize that region accordingly for the actor's actual size and its
  // gravity.
  scaled_rect.resize(actor_->GetWidth() * composited_scale_x_,
                     actor_->GetHeight() * composited_scale_y_,
                     actor_gravity_);

  switch (dimensions) {
    case MOVE_DIMENSIONS_X_AND_Y:
      actor_->Move(scaled_rect.x, scaled_rect.y, anim_ms);
      if (shadow_.get())
        shadow_->Move(scaled_rect.x, scaled_rect.y, anim_ms);
      break;
    case MOVE_DIMENSIONS_X_ONLY:
      actor_->MoveX(scaled_rect.x, anim_ms);
      if (shadow_.get())
        shadow_->MoveX(scaled_rect.x, anim_ms);
      break;
    case MOVE_DIMENSIONS_Y_ONLY:
      actor_->MoveY(scaled_rect.y, anim_ms);
      if (shadow_.get())
        shadow_->MoveY(scaled_rect.y, anim_ms);
      break;
    default:
      NOTREACHED() << "Unknown move dimensions " << dimensions;
  }

  if (damage_debug_group_.get())
    damage_debug_group_->Move(scaled_rect.x, scaled_rect.y, anim_ms);
}

void Window::ResetPixmap() {
  DCHECK(xid_);
  DCHECK(actor_.get());
  if (!mapped_)
    return;

  XID old_pixmap = pixmap_;
  pixmap_ = wm_->xconn()->GetCompositingPixmapForWindow(xid_);

  Size old_size(actor_->GetWidth(), actor_->GetHeight());
  actor_->SetPixmap(pixmap_);
  if (shadow_.get()) {
    shadow_->Resize(composited_scale_x_ * actor_->GetWidth(),
                    composited_scale_y_ * actor_->GetHeight(),
                    0);  // anim_ms
  }

  if (actor_gravity_ != GRAVITY_NORTHWEST &&
      Size(actor_->GetWidth(), actor_->GetHeight()) != old_size)
    MoveActorToAdjustedPosition(MOVE_DIMENSIONS_X_AND_Y, 0);

  if (old_pixmap) {
    wm_->xconn()->FreePixmap(old_pixmap);
  } else {
    // If we didn't have a pixmap already, then we're showing the window for
    // the first time and may need to show the shadow as well.
    DLOG(INFO) << "Fetched initial pixmap for already-mapped " << xid_str_;
    UpdateShadowVisibility();
    wm_->HandleWindowInitialPixmap(this);
  }

  need_to_reset_pixmap_ = false;
}

void Window::UpdateShadowVisibility() {
  // If nobody requested that this window have a shadow, |shadow_| will just
  // be NULL.
  if (!shadow_.get())
    return;

  // Even if it was requested, there may be other reasons not to show it
  // (maybe the window isn't mapped yet, or it's shaped, or it's hidden).
  const bool should_show = pixmap_ && !shaped_ && actor_is_shown();

  if (!shadow_->is_shown() && should_show)
    shadow_->Show();
  else if (shadow_->is_shown() && !should_show)
    shadow_->Hide();
}

void Window::SendWmSyncRequestMessage() {
  if (!wm_sync_request_alarm_)
    return;

  current_wm_sync_num_++;

  long data[5];
  memset(data, 0, sizeof(data));
  data[0] = wm_->GetXAtom(ATOM_NET_WM_SYNC_REQUEST);
  data[1] = wm_->GetCurrentTimeFromServer();
  data[2] = current_wm_sync_num_ & 0xffffffff;
  data[3] = (current_wm_sync_num_ >> 32) & 0xffffffff;
  DLOG(INFO) << "Asking " << xid_str() << " to notify us after it's redrawn "
             << "using sync num " << current_wm_sync_num_;
  wm_->xconn()->SendClientMessageEvent(
      xid_, xid_, wm_->GetXAtom(ATOM_WM_PROTOCOLS), data, 0);
  client_has_redrawn_after_last_resize_ = false;
}

void Window::UpdateDamageDebugging(const Rect& bounding_box) {
  // If we don't have a group for transforming all of the actors at once,
  // initialize one.
  if (!damage_debug_group_.get()) {
    damage_debug_group_.reset(wm_->compositor()->CreateGroup());
    damage_debug_group_->SetName("damage debug group for window " + xid_str_);
    damage_debug_group_->Move(composited_x_, composited_y_, 0);
    damage_debug_group_->Scale(composited_scale_x_, composited_scale_y_, 0);
    damage_debug_group_->SetOpacity(combined_opacity(), 0);
    if (actor_is_shown())
      damage_debug_group_->Show();
    else
      damage_debug_group_->Hide();

    wm_->stage()->AddActor(damage_debug_group_.get());
    damage_debug_group_->Raise(actor_.get());
  }

  // Create a new actor if we're not yet at the limit; recycle the oldest one
  // otherwise.
  shared_ptr<Compositor::ColoredBoxActor> debug_actor;
  if (damage_debug_actors_.size() < kMaxDamageDebugActors) {
    debug_actor.reset(
        wm_->compositor()->CreateColoredBox(
            bounding_box.width, bounding_box.height,
            Compositor::Color(kDamageDebugColor)));
    damage_debug_group_->AddActor(debug_actor.get());
    debug_actor->Show();
  } else {
    debug_actor = damage_debug_actors_[0];
    damage_debug_actors_.pop_front();
  }
  damage_debug_actors_.push_back(debug_actor);

  debug_actor->Move(bounding_box.x, bounding_box.y, 0);
  debug_actor->SetSize(bounding_box.width, bounding_box.height);
  debug_actor->SetOpacity(kDamageDebugOpacity, 0);
  debug_actor->SetOpacity(0.0, kDamageDebugFadeMs);
}


DestroyedWindow::DestroyedWindow(WindowManager* wm,
                                 XWindow xid,
                                 Compositor::TexturePixmapActor* actor,
                                 Shadow* shadow,
                                 XID pixmap)
    : wm_(wm),
      actor_(actor),
      shadow_(shadow),
      pixmap_(pixmap) {
  DCHECK(wm);
  DCHECK(actor);
  actor_->SetName(string("destroyed window ") + XidStr(xid));
}

DestroyedWindow::~DestroyedWindow() {
  if (pixmap_)
    wm_->xconn()->FreePixmap(pixmap_);
}

}  // namespace window_manager
