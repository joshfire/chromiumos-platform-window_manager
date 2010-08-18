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
#include "window_manager/focus_manager.h"
#include "window_manager/geometry.h"
#include "window_manager/shadow.h"
#include "window_manager/util.h"
#include "window_manager/window_manager.h"
#include "window_manager/x_connection.h"

using std::map;
using std::max;
using std::min;
using std::set;
using std::string;
using std::vector;
using window_manager::util::GetCurrentTimeSec;
using window_manager::util::XidStr;

namespace window_manager {

const int Window::kVideoMinWidth = 300;
const int Window::kVideoMinHeight = 225;
const int Window::kVideoMinFramerate = 15;

Window::Window(WindowManager* wm, XWindow xid, bool override_redirect,
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
      client_x_(geometry.x),
      client_y_(geometry.y),
      client_width_(geometry.width),
      client_height_(geometry.height),
      client_opacity_(1.0),
      composited_shown_(false),
      composited_x_(geometry.x),
      composited_y_(geometry.y),
      composited_scale_x_(1.0),
      composited_scale_y_(1.0),
      composited_opacity_(1.0),
      shadow_opacity_(1.0),
      supports_wm_take_focus_(false),
      supports_wm_delete_window_(false),
      wm_state_fullscreen_(false),
      wm_state_maximized_horz_(false),
      wm_state_maximized_vert_(false),
      wm_state_modal_(false),
      wm_hint_urgent_(false),
      damage_(0),
      pixmap_(0),
      num_video_damage_events_(0),
      video_damage_start_time_(-1),
      wm_sync_request_alarm_(0),
      current_wm_sync_num_(0),
      client_has_redrawn_after_last_resize_(true) {
  DCHECK(xid_);
  DLOG(INFO) << "Constructing object to track "
             << (override_redirect_ ? "override-redirect " : "")
             << "window " << xid_str() << " "
             << "at (" << client_x_ << ", " << client_y_ << ") "
             << "with dimensions " << client_width_ << "x" << client_height_;

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

  // This will update the actor's name based on the current title and xid.
  SetTitle(title_);
  actor_->Move(composited_x_, composited_y_, 0);
  actor_->Hide();
  wm_->stage()->AddActor(actor_.get());

  // Various properties could've been set on this window after it was
  // created but before we selected PropertyChangeMask, so we need to query
  // them here.
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
}

Window::~Window() {
  if (damage_)
    wm_->xconn()->DestroyDamage(damage_);
  if (pixmap_)
    wm_->xconn()->FreePixmap(pixmap_);
  DestroyWmSyncRequestAlarm();
}

void Window::SetTitle(const string& title) {
  DCHECK(actor_.get());
  title_ = title;
  if (title_.empty()) {
    actor_->SetName(string("window ") + xid_str());
  } else {
    actor_->SetName(string("window '") + title_ + "' (" + xid_str() + ")");
  }
}

bool Window::IsFocused() const {
  return wm_->focus_manager()->focused_win() == this;
}

bool Window::FetchAndApplySizeHints() {
  DCHECK(xid_);
  if (!wm_->xconn()->GetSizeHintsForWindow(xid_, &size_hints_))
    return false;

  // If windows are override-redirect or have already been mapped, they
  // should just make/request any desired changes directly.  Also ignore
  // position, aspect ratio, etc. hints for now.
  if (!mapped_ && !override_redirect_ &&
      (size_hints_.width > 0 && size_hints_.height > 0)) {
    DLOG(INFO) << "Got size hints for " << xid_str() << ": "
               << size_hints_.width << "x" << size_hints_.height;
    ResizeClient(size_hints_.width, size_hints_.height, GRAVITY_NORTHWEST);
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
  DLOG(INFO) << "Window " << xid_str() << " has type " << type_;
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
  bool supports_wm_sync_request = false;

  vector<int> wm_protocols;
  if (!wm_->xconn()->GetIntArrayProperty(
          xid_, wm_->GetXAtom(ATOM_WM_PROTOCOLS), &wm_protocols)) {
    return;
  }

  const XAtom wm_take_focus = wm_->GetXAtom(ATOM_WM_TAKE_FOCUS);
  const XAtom wm_delete_window = wm_->GetXAtom(ATOM_WM_DELETE_WINDOW);
  const XAtom wm_sync_request = wm_->GetXAtom(ATOM_NET_WM_SYNC_REQUEST);
  for (vector<int>::const_iterator it = wm_protocols.begin();
       it != wm_protocols.end(); ++it) {
    if (static_cast<XAtom>(*it) == wm_take_focus) {
      DLOG(INFO) << "Window " << xid_str() << " supports WM_TAKE_FOCUS";
      supports_wm_take_focus_ = true;
    } else if (static_cast<XAtom>(*it) == wm_delete_window) {
      DLOG(INFO) << "Window " << xid_str() << " supports WM_DELETE_WINDOW";
      supports_wm_delete_window_ = true;
    } else if (static_cast<XAtom>(*it) == wm_sync_request) {
      supports_wm_sync_request = true;
      DLOG(INFO) << "Window " << xid_str() << " supports _NET_WM_SYNC_REQUEST";
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

void Window::FetchAndApplyShape() {
  DCHECK(xid_);
  DCHECK(actor_.get());
  shaped_ = false;
  ByteMap bytemap(client_width_, client_height_);

  // We don't grab the server around these two requests, so it's possible
  // that a shaped window will have become unshaped between them and we'll
  // think that the window is shaped but get back an unshaped region.  This
  // should be okay; we should get another ShapeNotify event for the window
  // becoming unshaped and clear the useless mask then.
  if (wm_->xconn()->IsWindowShaped(xid_) &&
      wm_->xconn()->GetWindowBoundingRegion(xid_, &bytemap)) {
    shaped_ = true;
  }

  if (!shaped_) {
    actor_->ClearAlphaMask();
  } else {
    DLOG(INFO) << "Got shape for " << xid_str();
    actor_->SetAlphaMask(bytemap.bytes(), bytemap.width(), bytemap.height());
  }
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

  if (size_hints_.max_width > 0)
    desired_width = min(size_hints_.max_width, desired_width);
  if (size_hints_.min_width > 0)
    desired_width = max(size_hints_.min_width, desired_width);

  if (size_hints_.width_increment > 0) {
    int base_width =
        (size_hints_.base_width > 0) ? size_hints_.base_width :
        (size_hints_.min_width > 0) ? size_hints_.min_width :
        0;
    *width_out = base_width +
        ((desired_width - base_width) / size_hints_.width_increment) *
        size_hints_.width_increment;
  } else {
    *width_out = desired_width;
  }

  if (size_hints_.max_height > 0)
    desired_height = min(size_hints_.max_height, desired_height);
  if (size_hints_.min_height > 0)
    desired_height = max(size_hints_.min_height, desired_height);

  if (size_hints_.height_increment > 0) {
    int base_height =
        (size_hints_.base_height > 0) ? size_hints_.base_height :
        (size_hints_.min_height > 0) ? size_hints_.min_height :
        0;
    *height_out = base_height +
        ((desired_height - base_height) / size_hints_.height_increment) *
        size_hints_.height_increment;
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

bool Window::MoveClient(int x, int y) {
  DLOG(INFO) << "Moving " << xid_str() << "'s client window to ("
             << x << ", " << y << ")";
  DCHECK(xid_);
  if (!wm_->xconn()->MoveWindow(xid_, x, y))
    return false;
  SaveClientPosition(x, y);
  return true;
}

bool Window::MoveClientOffscreen() {
  return MoveClient(wm_->width(), wm_->height());
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
            xid_, client_x_ - dx, client_y_ - dy, width, height)) {
      return false;
    }
    SaveClientPosition(client_x_ - dx, client_y_ - dy);
    // TODO: Test whether this works for scaled windows.
    MoveComposited(composited_x_ - composited_scale_x_ * dx,
                   composited_y_ - composited_scale_y_ * dy,
                   0);
  } else  {
    if (!wm_->xconn()->ResizeWindow(xid_, width, height))
      return false;
  }

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
  DLOG(INFO) << "Moving " << xid_str() << "'s composited window to ("
             << x << ", " << y << ") over " << anim_ms << " ms";
  DCHECK(actor_.get());
  composited_x_ = x;
  composited_y_ = y;
  actor_->Move(x, y, anim_ms);
  if (shadow_.get())
    shadow_->Move(x, y, anim_ms);
}

void Window::MoveCompositedX(int x, int anim_ms) {
  DLOG(INFO) << "Setting " << xid_str() << "'s composited window's X "
             << "position to " << x << " over " << anim_ms << " ms";
  DCHECK(actor_.get());
  composited_x_ = x;
  actor_->MoveX(x, anim_ms);
  if (shadow_.get())
    shadow_->MoveX(x, anim_ms);
}

void Window::MoveCompositedY(int y, int anim_ms) {
  DLOG(INFO) << "Setting " << xid_str() << "'s composited window's Y "
             << "position to " << y << " over " << anim_ms << " ms";
  DCHECK(actor_.get());
  composited_y_ = y;
  actor_->MoveY(y, anim_ms);
  if (shadow_.get())
    shadow_->MoveY(y, anim_ms);
}

void Window::MoveCompositedToClient() {
  MoveComposited(client_x_, client_y_, 0);
}

void Window::ShowComposited() {
  DLOG(INFO) << "Showing " << xid_str() << "'s composited window";
  DCHECK(actor_.get());
  actor_->Show();
  composited_shown_ = true;
  UpdateShadowVisibility();
}

void Window::HideComposited() {
  DLOG(INFO) << "Hiding " << xid_str() << "'s composited window";
  DCHECK(actor_.get());
  actor_->Hide();
  composited_shown_ = false;
  UpdateShadowVisibility();
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
}

void Window::ScaleComposited(double scale_x, double scale_y, int anim_ms) {
  DLOG(INFO) << "Scaling " << xid_str() << "'s composited window by ("
             << scale_x << ", " << scale_y << ") over " << anim_ms << " ms";
  DCHECK(actor_.get());
  composited_scale_x_ = scale_x;
  composited_scale_y_ = scale_y;

  actor_->Scale(scale_x, scale_y, anim_ms);
  if (shadow_.get())
    shadow_->Resize(scale_x * client_width_, scale_y * client_height_, anim_ms);
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
  mapped_ = true;

  // If we're still waiting for the client to redraw the window (probably
  // in response to the _NET_WM_SYNC_REQUEST message that we sent in
  // HandleMapRequested()), then hold off on fetching the pixmap.  This
  // makes us not composite new windows until clients have painted them.
  if (client_has_redrawn_after_last_resize_) {
    ResetPixmap();
    UpdateShadowVisibility();
  }
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

  ResetPixmap();

  // If the window is in the middle of an animation (sliding offscreen),
  // its client position is already updated to the final position, and its
  // composited position is one frame into the animation because we've
  // already updated the coordinates prior to calling this method. However,
  // the content of the root window has not yet repainted, so using the
  // coordinates of the root window (0, 0)-(width, height) for the copying
  // will work while the coordinates of the window will not.
  wm_->xconn()->CopyArea(wm_->root(), pixmap_,
                         0, 0,  // src_x, src_y
                         0, 0,  // dest_x, dest_y
                         wm_->width(), wm_->height());
}

void Window::HandleConfigureNotify(int width, int height) {
  DCHECK(actor_.get());
  const bool size_changed =
      actor_->GetWidth() != width || actor_->GetHeight() != height;
  // Hold off on grabbing the window's contents if we haven't received
  // notification that the client has drawn to the new pixmap yet.
  if (size_changed && client_has_redrawn_after_last_resize_)
    ResetPixmap();
}

void Window::HandleDamageNotify(const Rect& bounding_box) {
  DCHECK(actor_.get());
  wm_->xconn()->ClearDamage(damage_);
  actor_->UpdateTexture();
  actor_->MergeDamagedRegion(bounding_box);

  // Check if this update could indicate that a video is playing.
  if (bounding_box.width >= kVideoMinWidth &&
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
  shadow_->Resize(composited_scale_x_ * client_width_,
                  composited_scale_y_ * client_height_, 0);
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

  // If we didn't have a pixmap already, then we're showing the window for
  // the first time and may need to show the shadow as well.
  const bool fetching_initial_pixmap = (pixmap_ == 0);
  ResetPixmap();
  if (fetching_initial_pixmap) {
    DLOG(INFO) << "Fetching initial pixmap for already-mapped " << xid_str();
    UpdateShadowVisibility();
    wm_->HandleWindowInitialPixmap(this);
  }
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

void Window::ResetPixmap() {
  DCHECK(xid_);
  DCHECK(actor_.get());
  if (!mapped_)
    return;

  XID old_pixmap = pixmap_;
  pixmap_ = wm_->xconn()->GetCompositingPixmapForWindow(xid_);
  actor_->SetPixmap(pixmap_);
  if (shadow_.get()) {
    shadow_->Resize(composited_scale_x_ * actor_->GetWidth(),
                    composited_scale_y_ * actor_->GetHeight(),
                    0);  // anim_ms
  }
  if (old_pixmap)
    wm_->xconn()->FreePixmap(old_pixmap);
}

void Window::UpdateShadowVisibility() {
  // If nobody requested that this window have a shadow, shadow_ will just
  // be NULL.
  if (!shadow_.get())
    return;

  // Even if it was requested, there may be other reasons not to show it
  // (maybe the window isn't mapped yet, or it's shaped, or it's hidden).
  const bool should_show = pixmap_ && !shaped_ && composited_shown_;

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

void Window::SendSyntheticConfigureNotify() {
  const XWindow* xid_under_us_ptr = wm_->stacked_xids().GetUnder(xid_);
  const XWindow xid_under_us = xid_under_us_ptr ? *xid_under_us_ptr : 0;
  DLOG(INFO) << "Sending synthetic configure notify for " << xid_str() << ": "
             << "(" << client_x_ << ", " << client_y_ << ") " << client_width_
             << "x" << client_height_ << ", above " << XidStr(xid_under_us);
  wm_->xconn()->SendConfigureNotifyEvent(
      xid_,
      client_x_, client_y_,
      client_width_, client_height_,
      0,  // border_width
      xid_under_us,
      false);  // override_redirect
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
