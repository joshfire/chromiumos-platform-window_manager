// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/panel.h"

#include <algorithm>
#include <map>
#include <string>

extern "C" {
#include <X11/cursorfont.h>
}
#include <gflags/gflags.h>

#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/focus_manager.h"
#include "window_manager/panel_manager.h"
#include "window_manager/util.h"
#include "window_manager/window_manager.h"
#include "window_manager/x_connection.h"

DEFINE_int32(panel_max_width, -1,
             "Maximum width for panels (0 or less means unconstrained)");
DEFINE_int32(panel_max_height, -1,
             "Maximum height for panels (0 or less means unconstrained)");
DEFINE_bool(panel_opaque_resize, false, "Resize panels opaquely");

using std::map;
using std::max;
using std::min;
using std::string;
using std::vector;
using window_manager::util::XidStr;

namespace window_manager {

// Amount of time to take to fade in the actor used for non-opaque resizes.
static const int kResizeActorOpacityAnimMs = 150;

// Minimum dimensions to which a panel content window can be resized.
static const int kPanelMinWidth = 20;
static const int kPanelMinHeight = 20;

// Frequency with which we should update the size of panels as they're
// being resized.
static const int kResizeUpdateMs = 25;

// Appearance of the box used for non-opaque resizing.

// Equivalent to "#4181f5".
static const Compositor::Color kResizeBoxBgColor(0.254902,
                                                 0.505882,
                                                 0.960784);

// Equivalent to "#234583".
static const Compositor::Color kResizeBoxBorderColor(0.137255,
                                                     0.270588,
                                                     0.513725);
static const double kResizeBoxOpacity = 0.3;

const int Panel::kResizeBorderWidth = 5;
const int Panel::kResizeCornerSize = 25;

Panel::Panel(PanelManager* panel_manager,
             Window* content_win,
             Window* titlebar_win,
             bool is_expanded)
    : panel_manager_(panel_manager),
      content_win_(content_win),
      titlebar_win_(titlebar_win),
      is_expanded_(is_expanded),
      is_fullscreen_(false),
      resize_actor_(NULL),
      resize_event_coalescer_(
          wm()->event_loop(),
          NewPermanentCallback(this, &Panel::ApplyResize),
          kResizeUpdateMs),
      // We don't need to select events on any of the drag borders; we'll
      // just install button grabs later.
      top_input_xid_(wm()->CreateInputWindow(-1, -1, 1, 1, 0)),
      top_left_input_xid_(wm()->CreateInputWindow(-1, -1, 1, 1, 0)),
      top_right_input_xid_(wm()->CreateInputWindow(-1, -1, 1, 1, 0)),
      left_input_xid_(wm()->CreateInputWindow(-1, -1, 1, 1, 0)),
      right_input_xid_(wm()->CreateInputWindow(-1, -1, 1, 1, 0)),
      resizable_(false),
      composited_windows_set_up_(false),
      drag_xid_(0),
      drag_start_x_(0),
      drag_start_y_(0),
      drag_orig_width_(1),
      drag_orig_height_(1),
      drag_last_width_(1),
      drag_last_height_(1),
      event_consumer_registrar_(
          new EventConsumerRegistrar(wm(), panel_manager)) {
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

  // Constrain the size of the content if we've been requested to do so.
  int capped_width = (FLAGS_panel_max_width > 0) ?
      min(content_win_->client_width(), FLAGS_panel_max_width) :
      content_win_->client_width();
  int capped_height = (FLAGS_panel_max_height > 0) ?
      min(content_win_->client_height(), FLAGS_panel_max_height) :
      content_win_->client_height();
  if (capped_width != content_win_->client_width() ||
      capped_height != content_win_->client_height()) {
    content_win_->ResizeClient(capped_width, capped_height, GRAVITY_NORTHWEST);
  }

  content_win_->CopyClientBoundsToRect(&content_bounds_);
  titlebar_win_->CopyClientBoundsToRect(&titlebar_bounds_);

  wm()->xconn()->SetWindowCursor(top_input_xid_, XC_top_side);
  wm()->xconn()->SetWindowCursor(top_left_input_xid_, XC_top_left_corner);
  wm()->xconn()->SetWindowCursor(top_right_input_xid_, XC_top_right_corner);
  wm()->xconn()->SetWindowCursor(left_input_xid_, XC_left_side);
  wm()->xconn()->SetWindowCursor(right_input_xid_, XC_right_side);

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
  UpdateChromeStateProperty();
}

Panel::~Panel() {
  if (drag_xid_) {
    wm()->xconn()->RemovePointerGrab(false, CurrentTime);
    drag_xid_ = None;
  }
  wm()->xconn()->DeselectInputOnWindow(titlebar_win_->xid(), EnterWindowMask);
  wm()->xconn()->DestroyWindow(top_input_xid_);
  wm()->xconn()->DestroyWindow(top_left_input_xid_);
  wm()->xconn()->DestroyWindow(top_right_input_xid_);
  wm()->xconn()->DestroyWindow(left_input_xid_);
  wm()->xconn()->DestroyWindow(right_input_xid_);
  panel_manager_ = NULL;
  content_win_ = NULL;
  titlebar_win_ = NULL;
  top_input_xid_ = None;
  top_left_input_xid_ = None;
  top_right_input_xid_ = None;
  left_input_xid_ = None;
  right_input_xid_ = None;
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
  if (button != 1)
    return;
  DCHECK(drag_xid_ == None)
      << "Panel " << xid_str() << " got button press in " << XidStr(xid)
      << " but already has drag XID " << XidStr(drag_xid_);

  drag_xid_ = xid;
  drag_start_x_ = x;
  drag_start_y_ = y;
  drag_orig_width_ = drag_last_width_ = content_width();
  drag_orig_height_ = drag_last_height_ = content_height();
  resize_event_coalescer_.Start();

  if (!FLAGS_panel_opaque_resize) {
    DCHECK(!resize_actor_.get());
    resize_actor_.reset(
        wm()->compositor()->CreateRectangle(
            kResizeBoxBgColor, kResizeBoxBorderColor, 1));  // border_width
    wm()->stage()->AddActor(resize_actor_.get());
    resize_actor_->Move(titlebar_x(), titlebar_y(), 0);
    resize_actor_->SetSize(content_width(), total_height());
    resize_actor_->SetOpacity(0, 0);
    resize_actor_->SetOpacity(kResizeBoxOpacity, kResizeActorOpacityAnimMs);
    wm()->stacking_manager()->StackActorAtTopOfLayer(
        resize_actor_.get(), StackingManager::LAYER_DRAGGED_PANEL);
    resize_actor_->SetVisibility(true);
  }
}

void Panel::HandleInputWindowButtonRelease(
    XWindow xid, int x, int y, int button, XTime timestamp) {
  if (button != 1)
    return;
  if (xid != drag_xid_) {
    LOG(WARNING) << "Ignoring button release for unexpected input window "
                 << XidStr(xid) << " (currently in drag initiated by "
                 << XidStr(drag_xid_) << ")";
    return;
  }
  // GrabButton-initiated asynchronous pointer grabs are automatically removed
  // by the X server on button release.
  resize_event_coalescer_.StorePosition(x, y);
  resize_event_coalescer_.Stop();
  drag_xid_ = None;

  if (!FLAGS_panel_opaque_resize) {
    DCHECK(resize_actor_.get());
    resize_actor_.reset(NULL);
    ResizeContent(drag_last_width_, drag_last_height_, drag_gravity_);
  }
}

void Panel::HandleInputWindowPointerMotion(XWindow xid, int x, int y) {
  if (xid != drag_xid_) {
    LOG(WARNING) << "Ignoring motion event for unexpected input window "
                 << XidStr(xid) << " (currently in drag initiated by "
                 << XidStr(drag_xid_) << ")";
    return;
  }
  resize_event_coalescer_.StorePosition(x, y);
}

void Panel::HandleWindowConfigureRequest(
    Window* win, int req_x, int req_y, int req_width, int req_height) {
  DCHECK(win);
  if (drag_xid_ != None) {
    LOG(WARNING) << "Ignoring configure request for " << win->xid_str()
                 << " in panel " << xid_str() << " because the panel is being "
                 << "resized by the user";
    return;
  }
  if (win != content_win_) {
    LOG(WARNING) << "Ignoring configure request for non-content window "
                 << win->xid_str() << " in panel " << xid_str();
    return;
  }

  if (req_width != content_bounds_.width ||
      req_height != content_bounds_.height) {
    ResizeContent(req_width, req_height, GRAVITY_SOUTHEAST);
  }
}

void Panel::Move(int right, int y, bool move_client_windows, int anim_ms) {
  titlebar_bounds_.x = right - titlebar_bounds_.width;
  titlebar_bounds_.y = y;
  content_bounds_.x = right - content_bounds_.width;
  content_bounds_.y = y + titlebar_bounds_.height;

  if (CanConfigureWindows()) {
    titlebar_win_->MoveComposited(
        titlebar_bounds_.x, titlebar_bounds_.y, anim_ms);
    content_win_->MoveComposited(content_bounds_.x, content_bounds_.y, anim_ms);
    if (!composited_windows_set_up_) {
      titlebar_win_->ScaleComposited(1.0, 1.0, 0);
      titlebar_win_->SetCompositedOpacity(1.0, 0);
      titlebar_win_->ShowComposited();
      content_win_->ScaleComposited(1.0, 1.0, 0);
      content_win_->SetCompositedOpacity(1.0, 0);
      content_win_->ShowComposited();
      composited_windows_set_up_ = true;
    }
    if (move_client_windows) {
      titlebar_win_->MoveClientToComposited();
      content_win_->MoveClientToComposited();
      ConfigureInputWindows();
    }
  }
}

void Panel::MoveX(int right, bool move_client_windows, int anim_ms) {
  DCHECK(composited_windows_set_up_)
      << "Move() must be called initially to configure composited windows";
  titlebar_bounds_.x = right - titlebar_bounds_.width;
  content_bounds_.x = right - content_bounds_.width;

  if (CanConfigureWindows()) {
    titlebar_win_->MoveCompositedX(titlebar_bounds_.x, anim_ms);
    content_win_->MoveCompositedX(content_bounds_.x, anim_ms);
    if (move_client_windows) {
      titlebar_win_->MoveClientToComposited();
      content_win_->MoveClientToComposited();
      ConfigureInputWindows();
    }
  }
}

void Panel::MoveY(int y, bool move_client_windows, int anim_ms) {
  DCHECK(composited_windows_set_up_)
      << "Move() must be called initially to configure composited windows";
  titlebar_bounds_.y = y;
  content_bounds_.y = y + titlebar_bounds_.height;

  if (CanConfigureWindows()) {
    titlebar_win_->MoveCompositedY(titlebar_bounds_.y, anim_ms);
    content_win_->MoveCompositedY(content_bounds_.y, anim_ms);
    if (move_client_windows) {
      titlebar_win_->MoveClientToComposited();
      content_win_->MoveClientToComposited();
      ConfigureInputWindows();
    }
  }
}

void Panel::SetTitlebarWidth(int width) {
  CHECK(width > 0);
  titlebar_bounds_.resize(width, titlebar_bounds_.height, GRAVITY_NORTHEAST);
  if (CanConfigureWindows()) {
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
  if (CanConfigureWindows()) {
    // Put the titlebar and content in the same layer, but stack the
    // titlebar higher (the stacking between the two is arbitrary but needs
    // to stay in sync with the input window code in StackInputWindows()).
    wm()->stacking_manager()->StackWindowAtTopOfLayer(content_win_, layer);
    wm()->stacking_manager()->StackWindowAtTopOfLayer(titlebar_win_, layer);
    StackInputWindows();
  }
}

bool Panel::SetExpandedState(bool expanded) {
  if (expanded == is_expanded_)
    return true;

  is_expanded_ = expanded;

  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_PANEL_STATE);
  msg.set_param(0, expanded);
  bool success = wm()->wm_ipc()->SendMessage(content_win_->xid(), msg);

  success &= UpdateChromeStateProperty();

  return success;
}

WindowManager* Panel::wm() {
  return panel_manager_->wm();
}

void Panel::TakeFocus(XTime timestamp) {
  wm()->FocusWindow(content_win_, timestamp);
}

void Panel::ResizeContent(int width, int height, Gravity gravity) {
  DCHECK_GT(width, 0);
  DCHECK_GT(height, 0);

  bool changing_height = (height != content_bounds_.height);

  content_bounds_.resize(width, height, gravity);
  titlebar_bounds_.resize(width, titlebar_bounds_.height, gravity);
  if (changing_height)
    titlebar_bounds_.y = content_bounds_.y - titlebar_bounds_.height;

  if (CanConfigureWindows()) {
    content_win_->ResizeClient(width, height, gravity);
    titlebar_win_->ResizeClient(width, titlebar_bounds_.height, gravity);

    // TODO: This is broken if we start resizing scaled windows.
    if (changing_height) {
      titlebar_win_->MoveCompositedY(titlebar_bounds_.y, 0);
      titlebar_win_->MoveClientToComposited();
    }
  }

  ConfigureInputWindows();
  panel_manager_->HandlePanelResize(this);
}

void Panel::SetFullscreenState(bool fullscreen) {
  if (fullscreen == is_fullscreen_)
    return;

  DLOG(INFO) << "Setting fullscreen state for panel " << xid_str()
             << " to " << fullscreen;
  is_fullscreen_ = fullscreen;

  // Update the EWMH property if needed.
  if (content_win_->wm_state_fullscreen() != is_fullscreen_) {
    map<XAtom, bool> wm_state;
    wm_state[wm()->GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN)] = is_fullscreen_;
    content_win_->ChangeWmState(wm_state);
  }

  if (fullscreen) {
    wm()->stacking_manager()->StackWindowAtTopOfLayer(
        content_win_, StackingManager::LAYER_FULLSCREEN_PANEL);
    content_win_->MoveComposited(0, 0, 0);
    content_win_->MoveClient(0, 0);
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
    content_win_->MoveComposited(content_bounds_.x, content_bounds_.y, 0);
    content_win_->MoveClientToComposited();
    titlebar_win_->ResizeClient(
        titlebar_bounds_.width, titlebar_bounds_.height, GRAVITY_NORTHWEST);
    titlebar_win_->MoveComposited(titlebar_bounds_.x, titlebar_bounds_.y, 0);
    titlebar_win_->MoveClientToComposited();
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

void Panel::ConfigureInputWindows() {
  if (!resizable_) {
    wm()->xconn()->ConfigureWindowOffscreen(top_input_xid_);
    wm()->xconn()->ConfigureWindowOffscreen(top_left_input_xid_);
    wm()->xconn()->ConfigureWindowOffscreen(top_right_input_xid_);
    wm()->xconn()->ConfigureWindowOffscreen(left_input_xid_);
    wm()->xconn()->ConfigureWindowOffscreen(right_input_xid_);
    return;
  }

  if (content_width() + 2 * (kResizeBorderWidth - kResizeCornerSize) <= 0) {
    wm()->xconn()->ConfigureWindowOffscreen(top_input_xid_);
  } else {
    wm()->xconn()->ConfigureWindow(
        top_input_xid_,
        content_x() - kResizeBorderWidth + kResizeCornerSize,
        titlebar_y() - kResizeBorderWidth,
        content_width() + 2 * (kResizeBorderWidth - kResizeCornerSize),
        kResizeBorderWidth);
  }

  wm()->xconn()->ConfigureWindow(
      top_left_input_xid_,
      content_x() - kResizeBorderWidth,
      titlebar_y() - kResizeBorderWidth,
      kResizeCornerSize,
      kResizeCornerSize);
  wm()->xconn()->ConfigureWindow(
      top_right_input_xid_,
      right() + kResizeBorderWidth - kResizeCornerSize,
      titlebar_y() - kResizeBorderWidth,
      kResizeCornerSize,
      kResizeCornerSize);

  int resize_edge_height =
      total_height() + kResizeBorderWidth - kResizeCornerSize;
  if (resize_edge_height <= 0) {
    wm()->xconn()->ConfigureWindowOffscreen(left_input_xid_);
    wm()->xconn()->ConfigureWindowOffscreen(right_input_xid_);
  } else {
    wm()->xconn()->ConfigureWindow(
        left_input_xid_,
        content_x() - kResizeBorderWidth,
        titlebar_y() - kResizeBorderWidth + kResizeCornerSize,
        kResizeBorderWidth,
        resize_edge_height);
    wm()->xconn()->ConfigureWindow(
        right_input_xid_,
        right(),
        titlebar_y() - kResizeBorderWidth + kResizeCornerSize,
        kResizeBorderWidth,
        resize_edge_height);
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
  int dx = resize_event_coalescer_.x() - drag_start_x_;
  int dy = resize_event_coalescer_.y() - drag_start_y_;
  drag_gravity_ = GRAVITY_NORTHWEST;

  if (drag_xid_ == top_input_xid_) {
    drag_gravity_ = GRAVITY_SOUTHWEST;
    dx = 0;
    dy *= -1;
  } else if (drag_xid_ == top_left_input_xid_) {
    drag_gravity_ = GRAVITY_SOUTHEAST;
    dx *= -1;
    dy *= -1;
  } else if (drag_xid_ == top_right_input_xid_) {
    drag_gravity_ = GRAVITY_SOUTHWEST;
    dy *= -1;
  } else if (drag_xid_ == left_input_xid_) {
    drag_gravity_ = GRAVITY_NORTHEAST;
    dx *= -1;
    dy = 0;
  } else if (drag_xid_ == right_input_xid_) {
    drag_gravity_ = GRAVITY_NORTHWEST;
    dy = 0;
  }

  drag_last_width_ = max(drag_orig_width_ + dx, kPanelMinWidth);
  drag_last_height_ = max(drag_orig_height_ + dy, kPanelMinHeight);

  if (FLAGS_panel_opaque_resize) {
    // TODO: We don't use opaque resizing currently, but if we ever start,
    // we're doing extra configuration of the input windows during each
    // step of the resize here that we don't really need to do until it's
    // done.
    ResizeContent(drag_last_width_, drag_last_height_, drag_gravity_);
  } else {
    if (resize_actor_.get()) {
      int actor_x = titlebar_x();
      if (drag_gravity_ == GRAVITY_SOUTHEAST ||
          drag_gravity_ == GRAVITY_NORTHEAST) {
        actor_x -= (drag_last_width_ - drag_orig_width_);
      }
      int actor_y = titlebar_y();
      if (drag_gravity_ == GRAVITY_SOUTHWEST ||
          drag_gravity_ == GRAVITY_SOUTHEAST) {
        actor_y -= (drag_last_height_ - drag_orig_height_);
      }
      resize_actor_->Move(actor_x, actor_y, 0);
      resize_actor_->SetSize(
          drag_last_width_, drag_last_height_ + titlebar_height());
    }
  }
}

bool Panel::UpdateChromeStateProperty() {
  map<XAtom, bool> states;
  states[wm()->GetXAtom(ATOM_CHROME_STATE_COLLAPSED_PANEL)] = !is_expanded_;
  return content_win_->ChangeChromeState(states);
}

}  // namespace window_manager
