// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/panels/panel.h"

#include <algorithm>
#include <limits>
#include <map>
#include <string>

extern "C" {
#include <X11/cursorfont.h>
}
#include <gflags/gflags.h>

#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/focus_manager.h"
#include "window_manager/panels/panel_manager.h"
#include "window_manager/resize_box.h"
#include "window_manager/shadow.h"
#include "window_manager/transient_window_collection.h"
#include "window_manager/util.h"
#include "window_manager/window_manager.h"
#include "window_manager/x11/x_connection.h"

DEFINE_bool(panel_opaque_resize, false, "Resize panels opaquely");

using std::map;
using std::max;
using std::min;
using std::numeric_limits;
using std::string;
using std::vector;
using window_manager::util::XidStr;

namespace {

XID kTopCursor = 0;
XID kTopLeftCursor = 0;
XID kTopRightCursor = 0;
XID kLeftCursor = 0;
XID kRightCursor = 0;

void InitCursors(window_manager::XConnection* xconn) {
  DCHECK(!kTopCursor) << "Cursors were already created";
  kTopCursor = xconn->CreateShapedCursor(XC_top_side);
  kTopLeftCursor = xconn->CreateShapedCursor(XC_top_left_corner);
  kTopRightCursor = xconn->CreateShapedCursor(XC_top_right_corner);
  kLeftCursor = xconn->CreateShapedCursor(XC_left_side);
  kRightCursor = xconn->CreateShapedCursor(XC_right_side);
}

}  // namespace

namespace window_manager {

// Opacity of the box that's displayed while a panel is being resized.
static const double kResizeBoxOpacity = 0.4;

// Frequency with which we should update the size of panels as they're
// being resized.
static const int kResizeUpdateMs = 25;

const int Panel::kResizeBorderWidth = 3;
const int Panel::kResizeCornerSize = 20;

Panel::Panel(PanelManager* panel_manager,
             Window* content_win,
             Window* titlebar_win,
             bool is_expanded)
    : panel_manager_(panel_manager),
      content_win_(content_win),
      titlebar_win_(titlebar_win),
      is_expanded_(is_expanded),
      is_fullscreen_(false),
      is_urgent_(content_win->wm_hint_urgent()),
      resize_event_coalescer_(
          wm()->event_loop(),
          NewPermanentCallback(this, &Panel::ApplyResize),
          kResizeUpdateMs),
      min_content_width_(0),
      min_content_height_(0),
      max_content_width_(0),
      max_content_height_(0),
      // We don't need to select events on any of the drag borders; we'll
      // just install button grabs later.
      top_input_xid_(wm()->CreateInputWindow(Rect(-1, -1, 1, 1), 0)),
      top_left_input_xid_(wm()->CreateInputWindow(Rect(-1, -1, 1, 1), 0)),
      top_right_input_xid_(wm()->CreateInputWindow(Rect(-1, -1, 1, 1), 0)),
      left_input_xid_(wm()->CreateInputWindow(Rect(-1, -1, 1, 1), 0)),
      right_input_xid_(wm()->CreateInputWindow(Rect(-1, -1, 1, 1), 0)),
      resizable_(false),
      horizontal_resize_allowed_(true),
      vertical_resize_allowed_(true),
      composited_windows_set_up_(false),
      being_dragged_to_new_position_(false),
      resize_drag_xid_(0),
      resize_drag_start_x_(0),
      resize_drag_start_y_(0),
      resize_drag_orig_width_(1),
      resize_drag_orig_height_(1),
      resize_drag_last_width_(1),
      resize_drag_last_height_(1),
      event_consumer_registrar_(
          new EventConsumerRegistrar(wm(), panel_manager)),
      transients_(
          new TransientWindowCollection(
              content_win_, titlebar_win_, true, panel_manager)),
      separator_shadow_(
          Shadow::Create(wm()->compositor(), Shadow::TYPE_PANEL_SEPARATOR)) {
  CHECK(content_win_);
  CHECK(titlebar_win_);

  // Register the PanelManager to receive events about the content,
  // titlebar, and input windows, and also to be notified when the WM_HINTS
  // property changes on the content window (it's used to set the urgency
  // hint).
  event_consumer_registrar_->RegisterForWindowEvents(content_xid());
  event_consumer_registrar_->RegisterForWindowEvents(titlebar_xid());
  event_consumer_registrar_->RegisterForWindowEvents(top_input_xid_);
  event_consumer_registrar_->RegisterForWindowEvents(top_left_input_xid_);
  event_consumer_registrar_->RegisterForWindowEvents(top_right_input_xid_);
  event_consumer_registrar_->RegisterForWindowEvents(left_input_xid_);
  event_consumer_registrar_->RegisterForWindowEvents(right_input_xid_);
  event_consumer_registrar_->RegisterForPropertyChanges(
      content_xid(), wm()->GetXAtom(ATOM_WM_HINTS));
  event_consumer_registrar_->RegisterForPropertyChanges(
      content_xid(), wm()->GetXAtom(ATOM_WM_NORMAL_HINTS));

  wm()->xconn()->SelectInputOnWindow(titlebar_win_->xid(),
                                     EnterWindowMask,
                                     true);  // preserve_existing

  // Install passive button grabs on all the resize handles, using
  // asynchronous mode so that we'll continue to receive mouse events while
  // the pointer grab is in effect.  (Note that these button grabs are
  // necessary to avoid a race condition: if we explicitly request an
  // active grab when seeing a button press, the button might already be
  // released by the time that the grab is installed.)
  int event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
  wm()->xconn()->AddButtonGrabOnWindow(top_input_xid_, 1, event_mask, false);
  wm()->xconn()->AddButtonGrabOnWindow(
      top_left_input_xid_, 1, event_mask, false);
  wm()->xconn()->AddButtonGrabOnWindow(
      top_right_input_xid_, 1, event_mask, false);
  wm()->xconn()->AddButtonGrabOnWindow(left_input_xid_, 1, event_mask, false);
  wm()->xconn()->AddButtonGrabOnWindow(right_input_xid_, 1, event_mask, false);

  content_win_->SetVisibility(Window::VISIBILITY_HIDDEN);
  titlebar_win_->SetVisibility(Window::VISIBILITY_HIDDEN);

  content_win_->SetShadowType(Shadow::TYPE_PANEL_CONTENT);
  titlebar_win_->SetShadowType(Shadow::TYPE_PANEL_TITLEBAR);

  // Make sure that the content window's size is within the allowable range.
  UpdateContentWindowSizeLimits();
  const int capped_width =
      min(max(content_win_->client_width(), min_content_width_),
          max_content_width_);
  const int capped_height =
      min(max(content_win_->client_height(), min_content_height_),
          max_content_height_);
  if (capped_width != content_win_->client_width() ||
      capped_height != content_win_->client_height()) {
    content_win_->ResizeClient(capped_width, capped_height, GRAVITY_NORTHWEST);
  }

  content_win_->CopyClientBoundsToRect(&content_bounds_);
  titlebar_win_->CopyClientBoundsToRect(&titlebar_bounds_);

  if (!kTopCursor)
    InitCursors(wm()->xconn());
  wm()->xconn()->SetWindowCursor(top_input_xid_, kTopCursor);
  wm()->xconn()->SetWindowCursor(top_left_input_xid_, kTopLeftCursor);
  wm()->xconn()->SetWindowCursor(top_right_input_xid_, kTopRightCursor);
  wm()->xconn()->SetWindowCursor(left_input_xid_, kLeftCursor);
  wm()->xconn()->SetWindowCursor(right_input_xid_, kRightCursor);

  wm()->SetNamePropertiesForXid(
      top_input_xid_, string("top input window for panel ") + xid_str());
  wm()->SetNamePropertiesForXid(
      top_left_input_xid_,
      string("top-left input window for panel ") + xid_str());
  wm()->SetNamePropertiesForXid(
      top_right_input_xid_,
      string("top-right input window for panel ") + xid_str());
  wm()->SetNamePropertiesForXid(
      left_input_xid_, string("left input window for panel ") + xid_str());
  wm()->SetNamePropertiesForXid(
      right_input_xid_, string("right input window for panel ") + xid_str());

  wm()->focus_manager()->UseClickToFocusForWindow(content_win_);

  if (content_win_->type_params().size() >= 5) {
    chromeos::WmIpcPanelUserResizeType resize_type =
        static_cast<chromeos::WmIpcPanelUserResizeType>(
            content_win_->type_params().at(4));
    switch (resize_type) {
      case chromeos::WM_IPC_PANEL_USER_RESIZE_HORIZONTALLY_AND_VERTICALLY:
        horizontal_resize_allowed_ = true;
        vertical_resize_allowed_ = true;
        break;
      case chromeos::WM_IPC_PANEL_USER_RESIZE_HORIZONTALLY:
        horizontal_resize_allowed_ = true;
        vertical_resize_allowed_ = false;
        break;
      case chromeos::WM_IPC_PANEL_USER_RESIZE_VERTICALLY:
        horizontal_resize_allowed_ = false;
        vertical_resize_allowed_ = true;
        break;
      case chromeos::WM_IPC_PANEL_USER_RESIZE_NONE:
        horizontal_resize_allowed_ = false;
        vertical_resize_allowed_ = false;
        break;
      default:
        LOG(WARNING) << "Unhandled user-resize settings " << resize_type
                     << " for panel " << xid_str();
    }
  }

  // Resize the shadow so it extends across the full width of the content
  // window, and stack it directly on top of it.
  separator_shadow_->Resize(content_width(), 0, 0);
  wm()->stage()->AddActor(separator_shadow_->group());
  separator_shadow_->group()->Raise(content_win_->actor());

  // Notify Chrome about the panel's state.  If we crash and get restarted,
  // we want to make sure that Chrome thinks it's in the same state that we do.
  SendStateMessageToChrome();
  UpdateChromeStateProperty();
}

Panel::~Panel() {
  if (resize_drag_xid_) {
    wm()->xconn()->UngrabPointer(false, 0);
    resize_drag_xid_ = 0;
  }
  transients_->CloseAllWindows();
  wm()->xconn()->DeselectInputOnWindow(titlebar_win_->xid(), EnterWindowMask);
  wm()->xconn()->DestroyWindow(top_input_xid_);
  wm()->xconn()->DestroyWindow(top_left_input_xid_);
  wm()->xconn()->DestroyWindow(top_right_input_xid_);
  wm()->xconn()->DestroyWindow(left_input_xid_);
  wm()->xconn()->DestroyWindow(right_input_xid_);
  content_win_->SetVisibility(Window::VISIBILITY_HIDDEN);
  titlebar_win_->SetVisibility(Window::VISIBILITY_HIDDEN);
  panel_manager_ = NULL;
  content_win_ = NULL;
  titlebar_win_ = NULL;
  top_input_xid_ = 0;
  top_left_input_xid_ = 0;
  top_right_input_xid_ = 0;
  left_input_xid_ = 0;
  right_input_xid_ = 0;
}

void Panel::GetInputWindows(vector<XWindow>* windows_out) {
  CHECK(windows_out);
  windows_out->clear();
  windows_out->reserve(5);
  windows_out->push_back(top_input_xid_);
  windows_out->push_back(top_left_input_xid_);
  windows_out->push_back(top_right_input_xid_);
  windows_out->push_back(left_input_xid_);
  windows_out->push_back(right_input_xid_);
}

void Panel::HandleInputWindowButtonPress(
    XWindow xid, int x, int y, int button, XTime timestamp) {
  if (wm()->IsModalWindowFocused())
    return;
  if (button != 1)
    return;
  DCHECK(resize_drag_xid_ == 0)
      << "Panel " << xid_str() << " got button press in " << XidStr(xid)
      << " but already has resize drag XID " << XidStr(resize_drag_xid_);

  resize_drag_xid_ = xid;
  resize_drag_start_x_ = x;
  resize_drag_start_y_ = y;
  resize_drag_orig_width_ = resize_drag_last_width_ = content_width();
  resize_drag_orig_height_ = resize_drag_last_height_ = content_height();
  resize_event_coalescer_.Start();

  if (!FLAGS_panel_opaque_resize) {
    DCHECK(!resize_box_.get());
    resize_box_.reset(new ResizeBox(wm()->compositor()));
    resize_box_->SetBounds(
        Rect(titlebar_x(), titlebar_y(), content_width(), total_height()), 0);
    wm()->stage()->AddActor(resize_box_->actor());
    resize_box_->actor()->SetOpacity(kResizeBoxOpacity, 0);
    wm()->stacking_manager()->StackActorAtTopOfLayer(
        resize_box_->actor(), StackingManager::LAYER_DRAGGED_PANEL);
    resize_box_->actor()->Show();
  }
}

void Panel::HandleInputWindowButtonRelease(
    XWindow xid, int x, int y, int button, XTime timestamp) {
  if (button != 1)
    return;
  if (xid != resize_drag_xid_) {
    LOG(WARNING) << "Ignoring button release for unexpected input window "
                 << XidStr(xid) << " (currently in resize drag initiated by "
                 << XidStr(resize_drag_xid_) << ")";
    return;
  }
  // GrabButton-initiated asynchronous pointer grabs are automatically removed
  // by the X server when *all* buttons are released, but we specifically
  // want the grab to end when the first button is released, to prevent the
  // user from essentially transferring the grab from one button to
  // another: see http://crosbug.com/4267.
  wm()->xconn()->UngrabPointer(false, timestamp);
  resize_event_coalescer_.StorePosition(x, y);
  resize_event_coalescer_.Stop();
  resize_drag_xid_ = 0;

  if (FLAGS_panel_opaque_resize) {
    ConfigureInputWindows();
  } else {
    DCHECK(resize_box_.get());
    resize_box_.reset(NULL);
    ResizeContent(resize_drag_last_width_, resize_drag_last_height_,
                  resize_drag_gravity_, true);
  }

  // Let the container know about the resize.
  panel_manager_->HandlePanelResizeByUser(this);
}

void Panel::HandleInputWindowPointerMotion(XWindow xid, int x, int y) {
  if (xid != resize_drag_xid_) {
    LOG(WARNING) << "Ignoring motion event for unexpected input window "
                 << XidStr(xid) << " (currently in resize drag initiated by "
                 << XidStr(resize_drag_xid_) << ")";
    return;
  }
  resize_event_coalescer_.StorePosition(x, y);
}

void Panel::Move(int right, int y, int anim_ms) {
  titlebar_bounds_.x = right - titlebar_bounds_.width;
  titlebar_bounds_.y = y;
  content_bounds_.x = right - content_bounds_.width;
  content_bounds_.y = y + titlebar_bounds_.height;

  transients_->CloseAllWindows();

  if (can_configure_windows()) {
    titlebar_win_->Move(titlebar_bounds_.position(), anim_ms);
    content_win_->Move(content_bounds_.position(), anim_ms);
    separator_shadow_->Move(content_bounds_.x, content_bounds_.y, anim_ms);
    if (!composited_windows_set_up_) {
      titlebar_win_->SetVisibility(Window::VISIBILITY_SHOWN);
      content_win_->SetVisibility(Window::VISIBILITY_SHOWN);
      separator_shadow_->Show();
      composited_windows_set_up_ = true;
    }
    if (!being_dragged_to_new_position_)
      ConfigureInputWindows();
  }
}

void Panel::MoveX(int right, int anim_ms) {
  DCHECK(composited_windows_set_up_)
      << "Move() must be called initially to configure composited windows";
  titlebar_bounds_.x = right - titlebar_bounds_.width;
  content_bounds_.x = right - content_bounds_.width;

  transients_->CloseAllWindows();

  if (can_configure_windows()) {
    titlebar_win_->MoveX(titlebar_bounds_.x, anim_ms);
    content_win_->MoveX(content_bounds_.x, anim_ms);
    separator_shadow_->MoveX(content_bounds_.x, anim_ms);
    if (!being_dragged_to_new_position_)
      ConfigureInputWindows();
  }
}

void Panel::MoveY(int y, int anim_ms) {
  DCHECK(composited_windows_set_up_)
      << "Move() must be called initially to configure composited windows";
  titlebar_bounds_.y = y;
  content_bounds_.y = y + titlebar_bounds_.height;

  transients_->CloseAllWindows();

  if (can_configure_windows()) {
    titlebar_win_->MoveY(titlebar_bounds_.y, anim_ms);
    content_win_->MoveY(content_bounds_.y, anim_ms);
    separator_shadow_->MoveY(content_bounds_.y, anim_ms);
    if (!being_dragged_to_new_position_)
      ConfigureInputWindows();
  }
}

void Panel::SetTitlebarWidth(int width) {
  CHECK(width > 0);
  titlebar_bounds_.resize(width, titlebar_bounds_.height, GRAVITY_NORTHEAST);
  if (can_configure_windows()) {
    titlebar_win_->ResizeClient(
        width, titlebar_win_->client_height(), GRAVITY_NORTHEAST);
  }
}

void Panel::SetShadowOpacity(double opacity, int anim_ms) {
  titlebar_win_->SetShadowOpacity(opacity, anim_ms);
  content_win_->SetShadowOpacity(opacity, anim_ms);
}

void Panel::SetResizable(bool resizable) {
  if (resizable != resizable_) {
    resizable_ = resizable;
    ConfigureInputWindows();
  }
}

void Panel::StackAtTopOfLayer(StackingManager::Layer layer) {
  stacking_layer_ = layer;
  if (can_configure_windows()) {
    // Put the titlebar and content in the same layer, but stack the
    // titlebar higher (the stacking between the two is arbitrary but needs
    // to stay in sync with the input window code in StackInputWindows()).
    wm()->stacking_manager()->StackWindowAtTopOfLayer(
        content_win_,
        layer,
        StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
    wm()->stacking_manager()->StackWindowAtTopOfLayer(
        titlebar_win_,
        layer,
        StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
    separator_shadow_->group()->Raise(content_win_->actor());
    StackInputWindows();
  }
}

bool Panel::SetExpandedState(bool expanded) {
  if (expanded == is_expanded_)
    return true;

  is_expanded_ = expanded;

  if (!is_expanded_)
    transients_->CloseAllWindows();

  bool success = SendStateMessageToChrome();
  success &= UpdateChromeStateProperty();
  return success;
}

WindowManager* Panel::wm() {
  return panel_manager_->wm();
}

void Panel::TakeFocus(XTime timestamp) {
  wm()->FocusWindow(content_win_, timestamp);
}

void Panel::ResizeContent(int width, int height,
                          Gravity gravity,
                          bool configure_input_windows) {
  DCHECK_GT(width, 0);
  DCHECK_GT(height, 0);

  const int capped_width =
      min(max(width, min_content_width_), max_content_width_);
  const int capped_height =
      min(max(height, min_content_height_), max_content_height_);

  if (capped_width != width || capped_height != height) {
    LOG(WARNING) << "Capped resize of panel " << xid_str() << " to "
                 << capped_width << "x" << capped_height
                 << " (request was for " << width << "x" << height << ")";
    width = capped_width;
    height = capped_height;
  }

  if (width == content_bounds_.width && height == content_bounds_.height)
    return;

  bool changing_height = (height != content_bounds_.height);

  content_bounds_.resize(width, height, gravity);
  titlebar_bounds_.resize(width, titlebar_bounds_.height, gravity);
  if (changing_height)
    titlebar_bounds_.y = content_bounds_.y - titlebar_bounds_.height;

  transients_->CloseAllWindows();

  if (can_configure_windows()) {
    content_win_->ResizeClient(width, height, gravity);
    titlebar_win_->ResizeClient(width, titlebar_bounds_.height, gravity);
    separator_shadow_->Move(content_x(), content_y(), 0);
    separator_shadow_->Resize(content_width(), 0, 0);

    // TODO: This is broken if we start resizing scaled windows.
    if (changing_height)
      titlebar_win_->Move(titlebar_bounds_.position(), 0);
  }

  if (configure_input_windows)
    ConfigureInputWindows();
}

void Panel::SetFullscreenState(bool fullscreen) {
  if (fullscreen == is_fullscreen_)
    return;

  DLOG(INFO) << "Setting fullscreen state for panel " << xid_str()
             << " to " << fullscreen;
  is_fullscreen_ = fullscreen;

  transients_->CloseAllWindows();

  // Update the EWMH property if needed.
  if (content_win_->wm_state_fullscreen() != is_fullscreen_) {
    map<XAtom, bool> wm_state;
    wm_state[wm()->GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN)] = is_fullscreen_;
    content_win_->ChangeWmState(wm_state);
  }

  if (fullscreen) {
    wm()->stacking_manager()->StackWindowAtTopOfLayer(
        content_win_,
        StackingManager::LAYER_FULLSCREEN_WINDOW,
        StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);
    content_win_->Move(Point(0, 0), 0);
    content_win_->ResizeClient(
        wm()->width(), wm()->height(), GRAVITY_NORTHWEST);
    if (!content_win_->IsFocused()) {
      LOG(WARNING) << "Fullscreening unfocused panel " << xid_str()
                   << ", so automatically giving it the focus";
      wm()->FocusWindow(content_win_, wm()->GetCurrentTimeFromServer());
    }
  } else {
    content_win_->ResizeClient(
        content_bounds_.width, content_bounds_.height, GRAVITY_NORTHWEST);
    content_win_->Move(content_bounds_.position(), 0);
    titlebar_win_->ResizeClient(
        titlebar_bounds_.width, titlebar_bounds_.height, GRAVITY_NORTHWEST);
    titlebar_win_->Move(titlebar_bounds_.position(), 0);
    separator_shadow_->Move(content_x(), content_y(), 0);
    separator_shadow_->Resize(content_width(), 0, 0);
    StackAtTopOfLayer(stacking_layer_);
  }
}

void Panel::HandleScreenResize() {
  if (is_fullscreen_) {
    DLOG(INFO) << "Resizing fullscreen panel to " << wm()->width()
               << "x" << wm()->height() << " in response to screen resize";
    content_win_->ResizeClient(
        wm()->width(), wm()->height(), GRAVITY_NORTHWEST);
  }
}

void Panel::HandleContentWindowSizeHintsChange() {
  UpdateContentWindowSizeLimits();
}

void Panel::HandleDragStart() {
  if (being_dragged_to_new_position_)
    return;
  being_dragged_to_new_position_ = true;
  content_win_->SetUpdateClientPositionForMoves(false);
  titlebar_win_->SetUpdateClientPositionForMoves(false);
}

void Panel::HandleDragEnd() {
  if (!being_dragged_to_new_position_)
    return;
  being_dragged_to_new_position_ = false;
  content_win_->SetUpdateClientPositionForMoves(true);
  titlebar_win_->SetUpdateClientPositionForMoves(true);
  ConfigureInputWindows();
}

void Panel::HandleTransientWindowMap(Window* win) {
  DCHECK(win);
  transients_->AddWindow(win, true);  // stack directly above panel
  if (content_win_->IsFocused())
    transients_->TakeFocus(wm()->GetCurrentTimeFromServer());
}

void Panel::HandleTransientWindowUnmap(Window* win) {
  DCHECK(win);
  transients_->RemoveWindow(win);
}

void Panel::HandleTransientWindowButtonPress(
    Window* win, int button, XTime timestamp) {
  if (wm()->IsModalWindowFocused())
    return;
  DCHECK(win);
  DCHECK(transients_->ContainsWindow(*win));
  transients_->SetPreferredWindowToFocus(win);
  transients_->TakeFocus(timestamp);
}

void Panel::HandleTransientWindowClientMessage(
    Window* win, XAtom message_type, const long data[5]) {
  DCHECK(win);
  DCHECK(transients_->ContainsWindow(*win));

  if (message_type == wm()->GetXAtom(ATOM_NET_ACTIVE_WINDOW)) {
    transients_->SetPreferredWindowToFocus(win);
    transients_->TakeFocus(data[1]);
  } else if (message_type == wm()->GetXAtom(ATOM_NET_WM_STATE)) {
    map<XAtom, bool> states;
    win->ParseWmStateMessage(data, &states);
    map<XAtom, bool>::const_iterator it =
        states.find(wm()->GetXAtom(ATOM_NET_WM_STATE_MODAL));
    if (it != states.end()) {
      map<XAtom, bool> new_state;
      new_state[it->first] = it->second;
      win->ChangeWmState(new_state);
    }
  }
}

void Panel::HandleTransientWindowConfigureRequest(
    Window* win, int req_x, int req_y, int req_width, int req_height) {
  DCHECK(win);
  DCHECK(transients_->ContainsWindow(*win));
  transients_->HandleConfigureRequest(
      win, req_x, req_y, req_width, req_height);
}

void Panel::ConfigureInputWindows() {
  if (!resizable_ ||
      (!horizontal_resize_allowed_ && !vertical_resize_allowed_)) {
    wm()->xconn()->ConfigureWindowOffscreen(top_input_xid_);
    wm()->xconn()->ConfigureWindowOffscreen(top_left_input_xid_);
    wm()->xconn()->ConfigureWindowOffscreen(top_right_input_xid_);
    wm()->xconn()->ConfigureWindowOffscreen(left_input_xid_);
    wm()->xconn()->ConfigureWindowOffscreen(right_input_xid_);
    return;
  }

  const int top_resize_edge_width =
      content_width() + (!horizontal_resize_allowed_ ? 0 :
                         2 * (kResizeBorderWidth - kResizeCornerSize));
  if (!vertical_resize_allowed_ || top_resize_edge_width <= 0) {
    wm()->xconn()->ConfigureWindowOffscreen(top_input_xid_);
  } else {
    wm()->xconn()->ConfigureWindow(
        top_input_xid_,
        Rect(content_x() - (top_resize_edge_width - content_width()) / 2,
             titlebar_y() - kResizeBorderWidth,
             top_resize_edge_width,
             kResizeBorderWidth));
  }

  if (!(vertical_resize_allowed_ && horizontal_resize_allowed_)) {
    wm()->xconn()->ConfigureWindowOffscreen(top_left_input_xid_);
    wm()->xconn()->ConfigureWindowOffscreen(top_right_input_xid_);
  } else {
    wm()->xconn()->ConfigureWindow(
        top_left_input_xid_,
        Rect(content_x() - kResizeBorderWidth,
             titlebar_y() - kResizeBorderWidth,
             kResizeCornerSize,
             kResizeCornerSize));
    wm()->xconn()->ConfigureWindow(
        top_right_input_xid_,
        Rect(right() + kResizeBorderWidth - kResizeCornerSize,
             titlebar_y() - kResizeBorderWidth,
             kResizeCornerSize,
             kResizeCornerSize));
  }

  const int side_resize_edge_height =
      total_height() + (!vertical_resize_allowed_ ? 0 :
                        kResizeBorderWidth - kResizeCornerSize);
  if (!horizontal_resize_allowed_ || side_resize_edge_height <= 0) {
    wm()->xconn()->ConfigureWindowOffscreen(left_input_xid_);
    wm()->xconn()->ConfigureWindowOffscreen(right_input_xid_);
  } else {
    wm()->xconn()->ConfigureWindow(
        left_input_xid_,
        Rect(content_x() - kResizeBorderWidth,
             titlebar_y() + total_height() - side_resize_edge_height,
             kResizeBorderWidth,
             side_resize_edge_height));
    wm()->xconn()->ConfigureWindow(
        right_input_xid_,
        Rect(right(),
             titlebar_y() + total_height() - side_resize_edge_height,
             kResizeBorderWidth,
             side_resize_edge_height));
  }
}

void Panel::StackInputWindows() {
  // Stack all of the input windows directly below the content window
  // (which is stacked beneath the titlebar) -- we don't want the
  // corner windows to occlude the titlebar.
  wm()->xconn()->StackWindow(top_input_xid_, content_win_->xid(), false);
  wm()->xconn()->StackWindow(top_left_input_xid_, content_win_->xid(), false);
  wm()->xconn()->StackWindow(top_right_input_xid_, content_win_->xid(), false);
  wm()->xconn()->StackWindow(left_input_xid_, content_win_->xid(), false);
  wm()->xconn()->StackWindow(right_input_xid_, content_win_->xid(), false);
}

void Panel::ApplyResize() {
  int dx = resize_event_coalescer_.x() - resize_drag_start_x_;
  int dy = resize_event_coalescer_.y() - resize_drag_start_y_;
  resize_drag_gravity_ = GRAVITY_NORTHWEST;

  if (resize_drag_xid_ == top_input_xid_) {
    resize_drag_gravity_ = GRAVITY_SOUTHWEST;
    dx = 0;
    dy *= -1;
  } else if (resize_drag_xid_ == top_left_input_xid_) {
    resize_drag_gravity_ = GRAVITY_SOUTHEAST;
    dx *= -1;
    dy *= -1;
  } else if (resize_drag_xid_ == top_right_input_xid_) {
    resize_drag_gravity_ = GRAVITY_SOUTHWEST;
    dy *= -1;
  } else if (resize_drag_xid_ == left_input_xid_) {
    resize_drag_gravity_ = GRAVITY_NORTHEAST;
    dx *= -1;
    dy = 0;
  } else if (resize_drag_xid_ == right_input_xid_) {
    resize_drag_gravity_ = GRAVITY_NORTHWEST;
    dy = 0;
  }

  resize_drag_last_width_ =
      min(max(resize_drag_orig_width_ + dx, min_content_width_),
          max_content_width_);
  resize_drag_last_height_ =
      min(max(resize_drag_orig_height_ + dy, min_content_height_),
          max_content_height_);

  if (FLAGS_panel_opaque_resize) {
    // Avoid reconfiguring the input windows until the end of the resize; moving
    // them now would affect the positions of subsequent motion events from the
    // drag.
    ResizeContent(resize_drag_last_width_, resize_drag_last_height_,
                  resize_drag_gravity_, false);
  } else {
    if (resize_box_.get()) {
      int actor_x = titlebar_x();
      if (resize_drag_gravity_ == GRAVITY_SOUTHEAST ||
          resize_drag_gravity_ == GRAVITY_NORTHEAST) {
        actor_x -= (resize_drag_last_width_ - resize_drag_orig_width_);
      }
      int actor_y = titlebar_y();
      if (resize_drag_gravity_ == GRAVITY_SOUTHWEST ||
          resize_drag_gravity_ == GRAVITY_SOUTHEAST) {
        actor_y -= (resize_drag_last_height_ - resize_drag_orig_height_);
      }

      Rect bounds(actor_x, actor_y, resize_drag_last_width_,
                  resize_drag_last_height_ + titlebar_height());
      resize_box_->SetBounds(bounds, 0);
    }
  }
}

bool Panel::SendStateMessageToChrome() {
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_PANEL_STATE);
  msg.set_param(0, is_expanded_);
  return wm()->wm_ipc()->SendMessage(content_win_->xid(), msg);
}

bool Panel::UpdateChromeStateProperty() {
  map<XAtom, bool> states;
  states[wm()->GetXAtom(ATOM_CHROME_STATE_COLLAPSED_PANEL)] = !is_expanded_;
  return content_win_->ChangeChromeState(states);
}

void Panel::UpdateContentWindowSizeLimits() {
  min_content_width_ = max(content_win_->size_hints().min_size.width,
                           max(2 * (kResizeCornerSize - kResizeBorderWidth) + 1,
                               content_win_->shadow()->GetMinWidth()));
  min_content_height_ = max(content_win_->size_hints().min_size.height,
                            max(kResizeCornerSize - kResizeBorderWidth + 1,
                                content_win_->shadow()->GetMinHeight()));

  max_content_width_ = content_win_->size_hints().max_size.width > 0 ?
                       content_win_->size_hints().max_size.width :
                       numeric_limits<int>::max();
  max_content_height_ = content_win_->size_hints().max_size.height > 0 ?
                        content_win_->size_hints().max_size.height :
                        numeric_limits<int>::max();
}

}  // namespace window_manager
