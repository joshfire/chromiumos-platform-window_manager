// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/layout_manager.h"

#include <algorithm>
#include <cmath>
#include <tr1/memory>
extern "C" {
#include <X11/Xatom.h>
}

#include <gflags/gflags.h>

#include "base/string_util.h"
#include "base/logging.h"
#include "window_manager/atom_cache.h"
#include "window_manager/callback.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/motion_event_coalescer.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/system_metrics.pb.h"
#include "window_manager/toplevel_window.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x_connection.h"

DEFINE_bool(lm_honor_window_size_hints, false,
            "When maximizing a client window, constrain its size according to "
            "the size hints that the client app has provided (e.g. max size, "
            "size increment, etc.) instead of automatically making it fill the "
            "screen");

DEFINE_string(lm_overview_gradient_image,
              "../assets/images/window_overview_gradient.png",
              "Image to use for gradients on inactive windows in "
              "overview mode");

namespace window_manager {

using std::tr1::shared_ptr;

// Amount of padding that should be used between windows in overview mode.
static const int kWindowPadding = 10;

// What's the maximum fraction of the manager's total size that a window
// should be scaled to in overview mode?
static const double kOverviewWindowMaxSizeRatio = 0.5;

// What fraction of the manager's total width should each window use for
// peeking out underneath the window on top of it in overview mode?
static const double kOverviewExposedWindowRatio = 0.1;

// Padding between the create browser window and the bottom of the screen.
static const int kCreateBrowserWindowVerticalPadding = 10;

// Duration between panning updates while a drag is occurring on the
// background window in overview mode.
static const int kOverviewDragUpdateMs = 50;

// Animation speed used for windows.
const int LayoutManager::kWindowAnimMs = 200;

LayoutManager::LayoutManager
    (WindowManager* wm, int x, int y, int width, int height)
    : wm_(wm),
      mode_(MODE_ACTIVE),
      x_(x),
      y_(y),
      width_(-1),
      height_(-1),
      magnified_toplevel_(NULL),
      active_toplevel_(NULL),
      create_browser_window_(NULL),
      overview_panning_offset_(0),
      overview_background_event_coalescer_(
          new MotionEventCoalescer(
              wm_->event_loop(),
              NewPermanentCallback(
                  this, &LayoutManager::UpdateOverviewPanningForMotion),
              kOverviewDragUpdateMs)),
      overview_drag_last_x_(-1),
      saw_map_request_(false),
      event_consumer_registrar_(new EventConsumerRegistrar(wm, this)) {
  event_consumer_registrar_->RegisterForChromeMessages(
      WmIpc::Message::WM_SWITCH_TO_OVERVIEW_MODE);

  MoveAndResize(x, y, width, height);

  int event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
  wm_->xconn()->AddButtonGrabOnWindow(
      wm_->background_xid(), 1, event_mask, false);
  event_consumer_registrar_->RegisterForWindowEvents(wm_->background_xid());

  KeyBindings* kb = wm_->key_bindings();
  kb->AddAction(
      "switch-to-overview-mode",
      NewPermanentCallback(this, &LayoutManager::SetMode, MODE_OVERVIEW),
      NULL, NULL);
  kb->AddAction(
      "switch-to-active-mode",
      NewPermanentCallback(this, &LayoutManager::SwitchToActiveMode, false),
      NULL, NULL);
  kb->AddAction(
      "cycle-active-forward",
      NewPermanentCallback(
          this, &LayoutManager::CycleActiveToplevelWindow, true),
      NULL, NULL);
  kb->AddAction(
      "cycle-active-backward",
      NewPermanentCallback(
          this, &LayoutManager::CycleActiveToplevelWindow, false),
      NULL, NULL);
  kb->AddAction(
      "cycle-magnification-forward",
      NewPermanentCallback(
          this, &LayoutManager::CycleMagnifiedToplevelWindow, true),
      NULL, NULL);
  kb->AddAction(
      "cycle-magnification-backward",
      NewPermanentCallback(
          this, &LayoutManager::CycleMagnifiedToplevelWindow, false),
      NULL, NULL);
  kb->AddAction(
      "switch-to-active-mode-for-magnified",
      NewPermanentCallback(this, &LayoutManager::SwitchToActiveMode, true),
      NULL, NULL);
  for (int i = 0; i < 8; ++i) {
    kb->AddAction(
        StringPrintf("activate-toplevel-with-index-%d", i),
        NewPermanentCallback(
            this, &LayoutManager::ActivateToplevelWindowByIndex, i),
        NULL, NULL);
    kb->AddAction(
        StringPrintf("magnify-toplevel-with-index-%d", i),
        NewPermanentCallback(
            this, &LayoutManager::MagnifyToplevelWindowByIndex, i),
        NULL, NULL);
  }
  kb->AddAction(
      "activate-last-toplevel",
      NewPermanentCallback(
          this, &LayoutManager::ActivateToplevelWindowByIndex, -1),
      NULL, NULL);
  kb->AddAction(
      "magnify-last-toplevel",
      NewPermanentCallback(
          this, &LayoutManager::MagnifyToplevelWindowByIndex, -1),
      NULL, NULL);
  kb->AddAction(
      "delete-active-window",
      NewPermanentCallback(
          this, &LayoutManager::SendDeleteRequestToActiveWindow),
      NULL, NULL);
  kb->AddAction(
      "pan-overview-mode-left",
      NewPermanentCallback(this, &LayoutManager::PanOverviewMode, -50),
      NULL, NULL);
  kb->AddAction(
      "pan-overview-mode-right",
      NewPermanentCallback(this, &LayoutManager::PanOverviewMode, 50),
      NULL, NULL);

  SetMode(MODE_ACTIVE);
}

LayoutManager::~LayoutManager() {
  wm_->xconn()->RemoveButtonGrabOnWindow(wm_->background_xid(), 1);

  KeyBindings* kb = wm_->key_bindings();
  kb->RemoveAction("switch-to-overview-mode");
  kb->RemoveAction("switch-to-active-mode");
  kb->RemoveAction("cycle-active-forward");
  kb->RemoveAction("cycle-active-backward");
  kb->RemoveAction("cycle-magnification-forward");
  kb->RemoveAction("cycle-magnification-backward");
  kb->RemoveAction("switch-to-active-mode-for-magnified");
  kb->RemoveAction("delete-active-window");
  kb->RemoveAction("pan-overview-mode-left");
  kb->RemoveAction("pan-overview-mode-right");

  toplevels_.clear();
  magnified_toplevel_ = NULL;
  active_toplevel_ = NULL;
}

bool LayoutManager::IsInputWindow(XWindow xid) {
  return (GetToplevelWindowByInputXid(xid) != NULL);
}

bool LayoutManager::HandleWindowMapRequest(Window* win) {
  saw_map_request_ = true;
  if (!IsHandledWindowType(win->type()))
    return false;

  DoInitialSetupForWindow(win);
  win->MapClient();
  return true;
}

void LayoutManager::HandleWindowMap(Window* win) {
  CHECK(win);

  // Just show override-redirect windows; they're already positioned
  // according to client apps' wishes.
  if (win->override_redirect()) {
    win->ShowComposited();
    return;
  }

  if (!IsHandledWindowType(win->type()))
    return;

  // Perform initial setup of windows that were already mapped at startup
  // (so we never saw MapRequest events for them).
  if (!saw_map_request_)
    DoInitialSetupForWindow(win);

  switch (win->type()) {
    case WmIpc::WINDOW_TYPE_CREATE_BROWSER_WINDOW: {
      if (create_browser_window_) {
        LOG(WARNING) << "Got second create-browser window " << win->xid_str()
                     << " (previous was " << create_browser_window_->xid_str()
                     << ")";
        create_browser_window_->HideComposited();
      }
      create_browser_window_ = win;
      wm_->stacking_manager()->StackWindowAtTopOfLayer(
          create_browser_window_, StackingManager::LAYER_TOPLEVEL_WINDOW);
      if (mode_ == MODE_OVERVIEW) {
        create_browser_window_->ShowComposited();
        LayoutToplevelWindowsForOverviewMode(-1);
      }
      break;
    }
    case WmIpc::WINDOW_TYPE_CHROME_TOPLEVEL:
    case WmIpc::WINDOW_TYPE_CHROME_INFO_BUBBLE:
    case WmIpc::WINDOW_TYPE_UNKNOWN: {
      if (win->transient_for_xid() != None) {
        ToplevelWindow* toplevel_owner =
            GetToplevelWindowByXid(win->transient_for_xid());
        if (toplevel_owner) {
          transient_to_toplevel_[win->xid()] = toplevel_owner;
          toplevel_owner->AddTransientWindow(win, mode_ == MODE_OVERVIEW);

          if (mode_ == MODE_ACTIVE &&
              active_toplevel_ != NULL &&
              active_toplevel_->IsWindowOrTransientFocused()) {
            active_toplevel_->TakeFocus(wm_->GetCurrentTimeFromServer());
          }
          break;
        } else {
          LOG(WARNING) << "Ignoring " << win->xid_str()
                       << "'s WM_TRANSIENT_FOR hint of "
                       << XidStr(win->transient_for_xid())
                       << ", which isn't a toplevel window";
          // Continue on and treat the transient as a toplevel window.
        }
      }

      shared_ptr<ToplevelWindow> toplevel(
          new ToplevelWindow(win, this));
      input_to_toplevel_[toplevel->input_xid()] = toplevel.get();

      switch (mode_) {
        case MODE_ACTIVE:
          // Activate the new window, adding it to the right of the
          // currently-active window.
          if (active_toplevel_) {
            int old_index = GetIndexForToplevelWindow(*active_toplevel_);
            CHECK(old_index >= 0);
            ToplevelWindows::iterator it = toplevels_.begin() + old_index + 1;
            toplevels_.insert(it, toplevel);
          } else {
            toplevels_.push_back(toplevel);
          }
          SetActiveToplevelWindow(
              toplevel.get(),
              ToplevelWindow::STATE_ACTIVE_MODE_IN_FROM_RIGHT,
              ToplevelWindow::STATE_ACTIVE_MODE_OUT_TO_LEFT);
          break;
        case MODE_OVERVIEW:
          // In overview mode, just put new windows on the right.
          toplevels_.push_back(toplevel);
          LayoutToplevelWindowsForOverviewMode(-1);
          break;
      }
      break;
    }
    default:
      NOTREACHED() << "Unexpected window type " << win->type();
      break;
  }
}

void LayoutManager::HandleWindowUnmap(Window* win) {
  CHECK(win);

  // If necessary, reset some pointers to non-toplevels windows first.
  if (create_browser_window_ == win) {
    create_browser_window_ = NULL;
    if (mode_ == MODE_OVERVIEW)
      LayoutToplevelWindowsForOverviewMode(-1);
  }

  ToplevelWindow* toplevel_owner = GetToplevelWindowOwningTransientWindow(*win);
  if (toplevel_owner) {
    bool transient_had_focus = win->focused();
    toplevel_owner->RemoveTransientWindow(win);
    if (transient_to_toplevel_.erase(win->xid()) != 1)
      LOG(WARNING) << "No transient-to-toplevel mapping for " << win->xid_str();
    if (transient_had_focus)
      toplevel_owner->TakeFocus(wm_->GetCurrentTimeFromServer());
  }

  ToplevelWindow* toplevel = GetToplevelWindowByWindow(*win);
  if (toplevel) {
    if (magnified_toplevel_ == toplevel)
      SetMagnifiedToplevelWindow(NULL);
    if (active_toplevel_ == toplevel)
      active_toplevel_ = NULL;

    const int index = GetIndexForToplevelWindow(*toplevel);
    CHECK(input_to_toplevel_.erase(toplevel->input_xid()) == 1);
    toplevels_.erase(toplevels_.begin() + index);

    if (mode_ == MODE_OVERVIEW) {
      LayoutToplevelWindowsForOverviewMode(-1);
    } else if (mode_ == MODE_ACTIVE) {
      // If there's no active window now, then this was probably active
      // previously.  Choose a new active window if possible; relinquish
      // the focus otherwise.
      if (!active_toplevel_) {
        if (!toplevels_.empty()) {
          const int new_index =
              (index + toplevels_.size() - 1) % toplevels_.size();
          SetActiveToplevelWindow(
              toplevels_[new_index].get(),
              ToplevelWindow::STATE_ACTIVE_MODE_IN_FROM_LEFT,
              ToplevelWindow::STATE_ACTIVE_MODE_OUT_TO_RIGHT);
        } else {
          if (win->focused()) {
            wm_->SetActiveWindowProperty(None);
            wm_->TakeFocus();
          }
        }
      }
    }
  }
}

void LayoutManager::HandleWindowConfigureRequest(
    Window* win, int req_x, int req_y, int req_width, int req_height) {
  ToplevelWindow* toplevel_owner = GetToplevelWindowOwningTransientWindow(*win);
  if (toplevel_owner) {
    toplevel_owner->HandleTransientWindowConfigureRequest(
        win, req_x, req_y, req_width, req_height);
    return;
  }

  // Let toplevel windows resize themselves to work around issue 449, where
  // the Chrome options window doesn't repaint if it doesn't get resized
  // after it's already mapped.
  // TODO: Remove this after Chrome has been fixed.
  ToplevelWindow* toplevel = GetToplevelWindowByWindow(*win);
  if (toplevel) {
    if (req_width != toplevel->win()->client_width() ||
        req_height != toplevel->win()->client_height()) {
      toplevel->win()->ResizeClient(
          req_width, req_height, Window::GRAVITY_NORTHWEST);
    }
    return;
  }
}

void LayoutManager::HandleButtonPress(XWindow xid,
                                      int x, int y,
                                      int x_root, int y_root,
                                      int button,
                                      XTime timestamp) {
  ToplevelWindow* toplevel = GetToplevelWindowByInputXid(xid);
  if (toplevel) {
    if (button == 1) {
      if (mode_ != MODE_OVERVIEW) {
        LOG(WARNING) << "Got a click in input window " << XidStr(xid)
                     << " for toplevel window " << toplevel->win()->xid_str()
                     << " while not in overview mode";
        return;
      }
      if (toplevel != magnified_toplevel_) {
        SetMagnifiedToplevelWindow(toplevel);
        LayoutToplevelWindowsForOverviewMode(std::max(x_root - x_, 0));
      } else {
        active_toplevel_ = toplevel;
        SetMode(MODE_ACTIVE);
      }
    }
    return;
  }

  if (xid == wm_->background_xid() && button == 1) {
    overview_drag_last_x_ = x;
    overview_background_event_coalescer_->Start();
    return;
  }

  // Otherwise, it probably means that the user previously focused a panel
  // and then clicked back on a toplevel or transient window.
  Window* win = wm_->GetWindow(xid);
  if (!win)
    return;
  toplevel = GetToplevelWindowOwningTransientWindow(*win);
  if (!toplevel)
    toplevel = GetToplevelWindowByWindow(*win);
  if (!toplevel)
    return;
  toplevel->HandleButtonPress(win, timestamp);
}

void LayoutManager::HandleButtonRelease(XWindow xid,
                                        int x, int y,
                                        int x_root, int y_root,
                                        int button,
                                        XTime timestamp) {
  if (xid != wm_->background_xid() || button != 1)
    return;

  // The X server automatically removes our asynchronous pointer grab when
  // the mouse buttons are released.
  overview_background_event_coalescer_->Stop();

  // We need to do one last configure to update the input windows'
  // positions, which we didn't bother doing while panning.
  ConfigureWindowsForOverviewMode(false);

  return;
}

void LayoutManager::HandlePointerEnter(XWindow xid,
                                       int x, int y,
                                       int x_root, int y_root,
                                       XTime timestamp) {
  ToplevelWindow* toplevel = GetToplevelWindowByInputXid(xid);
  if (!toplevel)
    return;
  if (mode_ != MODE_OVERVIEW) {
    LOG(WARNING) << "Got pointer enter in input window " << XidStr(xid)
                 << " for toplevel window " << toplevel->win()->xid_str()
                 << " while not in overview mode";
    return;
  }
}

void LayoutManager::HandlePointerMotion(XWindow xid,
                                        int x, int y,
                                        int x_root, int y_root,
                                        XTime timestamp) {
  if (xid == wm_->background_xid())
    overview_background_event_coalescer_->StorePosition(x, y);
}

void LayoutManager::HandleFocusChange(XWindow xid, bool focus_in) {
  Window* win = wm_->GetWindow(xid);
  if (!win)
    return;

  ToplevelWindow* toplevel = GetToplevelWindowOwningTransientWindow(*win);
  if (!toplevel)
    toplevel = GetToplevelWindowByWindow(*win);

  // If this is neither a toplevel nor transient window, we don't care
  // about the focus change.
  if (!toplevel)
    return;
  toplevel->HandleFocusChange(win, focus_in);

  // Announce that the new window is the "active" window (in the
  // _NET_ACTIVE_WINDOW sense), regardless of whether it's a toplevel
  // window or a transient.
  if (focus_in)
    wm_->SetActiveWindowProperty(win->xid());
}

void LayoutManager::HandleChromeMessage(const WmIpc::Message& msg) {
  switch (msg.type()) {
    case WmIpc::Message::WM_SWITCH_TO_OVERVIEW_MODE: {
      SetMode(MODE_OVERVIEW);
      Window* win = wm_->GetWindow(msg.param(0));
      if (!win) {
        LOG(WARNING) << "Ignoring request to magnify unknown window "
                     << XidStr(msg.param(0))
                     << " while switching to overview mode";
        return;
      }

      ToplevelWindow* toplevel = GetToplevelWindowByWindow(*win);
      if (!toplevel) {
        LOG(WARNING) << "Ignoring request to magnify non-toplevel window "
                     << XidStr(msg.param(0))
                     << " while switching to overview mode";
        return;
      }

      SetMagnifiedToplevelWindow(toplevel);
      break;
    }
    default:
      return;
  }
}

void LayoutManager::HandleClientMessage(XWindow xid,
                                        XAtom message_type,
                                        const long data[5]) {
  Window* win = wm_->GetWindow(xid);
  if (!win)
    return;

  if (message_type == wm_->GetXAtom(ATOM_NET_WM_STATE)) {
    win->HandleWmStateMessage(data);
  } else if (message_type == wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW)) {
    DLOG(INFO) << "Got _NET_ACTIVE_WINDOW request to focus " << XidStr(xid)
               << " (requestor says its currently-active window is "
               << XidStr(data[2]) << "; real active window is "
               << XidStr(wm_->active_window_xid()) << ")";

    // If we got a _NET_ACTIVE_WINDOW request for a transient, switch to
    // its owner instead.
    ToplevelWindow* toplevel = GetToplevelWindowOwningTransientWindow(*win);
    if (toplevel)
      toplevel->SetPreferredTransientWindowToFocus(win);
    else
      toplevel = GetToplevelWindowByWindow(*win);

    // If we don't know anything about this window, give up.
    if (!toplevel)
      return;

    if (mode_ == MODE_ACTIVE) {
      if (toplevel != active_toplevel_) {
        SetActiveToplevelWindow(toplevel,
                                ToplevelWindow::STATE_ACTIVE_MODE_IN_FADE,
                                ToplevelWindow::STATE_ACTIVE_MODE_OUT_FADE);
      } else {
        toplevel->TakeFocus(data[1]);
      }
    } else {
      active_toplevel_ = toplevel;
      SetMode(MODE_ACTIVE);
    }
  }
}

Window* LayoutManager::GetChromeWindow() {
  for (size_t i = 0; i < toplevels_.size(); ++i) {
    if (toplevels_[i]->win()->type() == WmIpc::WINDOW_TYPE_CHROME_TOPLEVEL)
      return toplevels_[i]->win();
  }
  return NULL;
}

bool LayoutManager::TakeFocus() {
  if (mode_ != MODE_ACTIVE || !active_toplevel_)
    return false;

  active_toplevel_->TakeFocus(wm_->GetCurrentTimeFromServer());
  return true;
}

void LayoutManager::MoveAndResize(int x, int y, int width, int height) {
  if (x == x_ && y == y_ && width == width_ && height == height_)
    return;

  // If there's a larger difference between our new and old left edge than
  // between the new and old right edge, then we keep the right sides of the
  // windows fixed while resizing.
  Window::Gravity resize_gravity =
      abs(x - x_) > abs(x + width - (x_ + width_)) ?
      Window::GRAVITY_NORTHEAST :
      Window::GRAVITY_NORTHWEST;

  x_ = x;
  y_ = y;
  width_ = width;
  height_ = height;

  for (ToplevelWindows::iterator it = toplevels_.begin();
       it != toplevels_.end(); ++it) {
    int width = width_;
    int height = height_;
    if (FLAGS_lm_honor_window_size_hints)
      (*it)->win()->GetMaxSize(width_, height_, &width, &height);
    (*it)->win()->ResizeClient(width, height, resize_gravity);
  }

  switch (mode_) {
    case MODE_ACTIVE:
      LayoutToplevelWindowsForActiveMode(false);  // update_focus=false
      break;
    case MODE_OVERVIEW:
      LayoutToplevelWindowsForOverviewMode(-1);
      break;
    default:
      DCHECK(false) << "Unhandled mode " << mode_ << " during resize";
  }
}

// static
bool LayoutManager::IsHandledWindowType(WmIpc::WindowType type) {
  return (type == WmIpc::WINDOW_TYPE_CHROME_INFO_BUBBLE ||
          type == WmIpc::WINDOW_TYPE_CHROME_TOPLEVEL ||
          type == WmIpc::WINDOW_TYPE_CREATE_BROWSER_WINDOW ||
          type == WmIpc::WINDOW_TYPE_UNKNOWN);
}

LayoutManager::ToplevelWindow* LayoutManager::GetToplevelWindowByInputXid(
    XWindow xid) {
  return FindWithDefault(
      input_to_toplevel_, xid, static_cast<ToplevelWindow*>(NULL));
}

int LayoutManager::GetIndexForToplevelWindow(
    const ToplevelWindow& toplevel) const {
  for (size_t i = 0; i < toplevels_.size(); ++i)
    if (toplevels_[i].get() == &toplevel)
      return static_cast<int>(i);
  return -1;
}

LayoutManager::ToplevelWindow* LayoutManager::GetToplevelWindowByWindow(
    const Window& win) {
  for (size_t i = 0; i < toplevels_.size(); ++i)
    if (toplevels_[i]->win() == &win)
      return toplevels_[i].get();
  return NULL;
}

LayoutManager::ToplevelWindow* LayoutManager::GetToplevelWindowByXid(
    XWindow xid) {
  const Window* win = wm_->GetWindow(xid);
  if (!win)
    return NULL;
  return GetToplevelWindowByWindow(*win);
}

LayoutManager::ToplevelWindow*
    LayoutManager::GetToplevelWindowOwningTransientWindow(const Window& win) {
  return FindWithDefault(
      transient_to_toplevel_, win.xid(), static_cast<ToplevelWindow*>(NULL));
}

XWindow LayoutManager::GetInputXidForWindow(const Window& win) {
  ToplevelWindow* toplevel = GetToplevelWindowByWindow(win);
  return toplevel ? toplevel->input_xid() : None;
}

void LayoutManager::DoInitialSetupForWindow(Window* win) {
  // We preserve info bubbles' initial locations even though they're
  // ultimately transient windows.
  if (win->type() != WmIpc::WINDOW_TYPE_CHROME_INFO_BUBBLE)
    win->MoveClientOffscreen();
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      win, StackingManager::LAYER_TOPLEVEL_WINDOW);
}

void LayoutManager::SetActiveToplevelWindow(
    ToplevelWindow* toplevel,
    int state_for_new_win,
    int state_for_old_win) {
  CHECK(toplevel);

  if (mode_ != MODE_ACTIVE || active_toplevel_ == toplevel)
    return;

  if (active_toplevel_)
    active_toplevel_->set_state(
        static_cast<ToplevelWindow::State>(state_for_old_win));
  toplevel->set_state(static_cast<ToplevelWindow::State>(state_for_new_win));
  active_toplevel_ = toplevel;
  LayoutToplevelWindowsForActiveMode(true);  // update_focus=true
}

void LayoutManager::SwitchToActiveMode(bool activate_magnified_win) {
  if (mode_ == MODE_ACTIVE)
    return;
  if (activate_magnified_win && magnified_toplevel_)
    active_toplevel_ = magnified_toplevel_;
  SetMode(MODE_ACTIVE);
}

void LayoutManager::ActivateToplevelWindowByIndex(int index) {
  if (toplevels_.empty() || mode_ != MODE_ACTIVE)
    return;

  if (index < 0)
    index = static_cast<int>(toplevels_.size()) + index;
  if (index < 0 || index >= static_cast<int>(toplevels_.size()))
    return;
  if (toplevels_[index].get() == active_toplevel_)
    return;

  SetActiveToplevelWindow(toplevels_[index].get(),
                          ToplevelWindow::STATE_ACTIVE_MODE_IN_FADE,
                          ToplevelWindow::STATE_ACTIVE_MODE_OUT_FADE);
}

void LayoutManager::MagnifyToplevelWindowByIndex(int index) {
  if (toplevels_.empty() || mode_ != MODE_OVERVIEW)
    return;

  if (index < 0)
    index = static_cast<int>(toplevels_.size()) + index;
  if (index < 0 || index >= static_cast<int>(toplevels_.size()))
    return;
  if (toplevels_[index].get() == magnified_toplevel_)
    return;

  SetMagnifiedToplevelWindow(toplevels_[index].get());
  LayoutToplevelWindowsForOverviewMode(0.5 * width_);
}

void LayoutManager::Metrics::Populate(chrome_os_pb::SystemMetrics *metrics_pb) {
  CHECK(metrics_pb);
  metrics_pb->Clear();
  metrics_pb->set_overview_keystroke_count(overview_by_keystroke_count);
  metrics_pb->set_overview_exit_mouse_count(overview_exit_by_mouse_count);
  metrics_pb->set_overview_exit_keystroke_count(
      overview_exit_by_keystroke_count);
  metrics_pb->set_keystroke_window_cycling_count(
      window_cycle_by_keystroke_count);
}

void LayoutManager::SetMode(Mode mode) {
  RemoveKeyBindingsForMode(mode_);
  mode_ = mode;
  switch (mode_) {
    case MODE_ACTIVE: {
      if (create_browser_window_) {
        create_browser_window_->HideComposited();
        create_browser_window_->MoveClientOffscreen();
      }
      if (magnified_toplevel_)
        active_toplevel_ = magnified_toplevel_;
      if (!active_toplevel_ && !toplevels_.empty())
        active_toplevel_ = toplevels_[0].get();
      LayoutToplevelWindowsForActiveMode(true);  // update_focus=true
      break;
    }
    case MODE_OVERVIEW: {
      if (create_browser_window_)
        create_browser_window_->ShowComposited();
      SetMagnifiedToplevelWindow(active_toplevel_);
      // Leave 'active_toplevel_' alone, so we can activate the same window
      // if we return to active mode on an Escape keypress.

      if (active_toplevel_ && active_toplevel_->IsWindowOrTransientFocused()) {
        // We need to take the input focus away here; otherwise the
        // previously-focused window would continue to get keyboard events
        // in overview mode.  Let the WindowManager decide what to do with it.
        wm_->SetActiveWindowProperty(None);
        wm_->TakeFocus();
      }
      LayoutToplevelWindowsForOverviewMode(0.5 * width_);
      break;
    }
  }
  AddKeyBindingsForMode(mode_);

  // Let all Chrome windows know about the new layout mode.
  for (ToplevelWindows::iterator it = toplevels_.begin();
       it != toplevels_.end(); ++it) {
    if ((*it)->win()->type() == WmIpc::WINDOW_TYPE_CHROME_TOPLEVEL)
      SendModeMessage(it->get());
  }
}

void LayoutManager::LayoutToplevelWindowsForActiveMode(bool update_focus) {
  DLOG(INFO) << "Laying out windows for active mode";
  if (toplevels_.empty())
    return;
  if (!active_toplevel_)
    active_toplevel_ = toplevels_[0].get();

  bool saw_active = false;
  for (ToplevelWindows::iterator it = toplevels_.begin();
       it != toplevels_.end(); ++it) {
    bool is_active = it->get() == active_toplevel_;
    (*it)->ConfigureForActiveMode(is_active, !saw_active, update_focus);
    if (is_active)
      saw_active = true;
  }
}

void LayoutManager::LayoutToplevelWindowsForOverviewMode(
    int magnified_x) {
  DLOG(INFO) << "Laying out windows for overview mode";
  CalculatePositionsForOverviewMode(magnified_x);
  ConfigureWindowsForOverviewMode(false);
}

void LayoutManager::CalculatePositionsForOverviewMode(int magnified_x) {
  if (toplevels_.empty())
    return;

  const int width_limit =
      std::min(static_cast<double>(width_) / sqrt(toplevels_.size()),
               kOverviewWindowMaxSizeRatio * width_);
  const int height_limit =
      std::min(static_cast<double>(height_) / sqrt(toplevels_.size()),
               kOverviewWindowMaxSizeRatio * height_);
  int running_width = kWindowPadding;

  for (int i = 0; static_cast<size_t>(i) < toplevels_.size(); ++i) {
    ToplevelWindow* toplevel = toplevels_[i].get();
    bool is_magnified = (toplevel == magnified_toplevel_);

    toplevel->UpdateOverviewScaling(width_limit, height_limit);
    toplevel->UpdateOverviewPosition(
        running_width, 0.5 * (height_ - toplevel->overview_height()));
    running_width += is_magnified ?
        toplevel->overview_width() :
        (kOverviewExposedWindowRatio * width_ *
          (width_limit / (kOverviewWindowMaxSizeRatio * width_)));
    if (is_magnified && magnified_x >= 0) {
      // If the window will be under 'magnified_x' when centered, just
      // center it.  Otherwise, move it as close to centered as possible
      // while still being under 'magnified_x'.
      if (0.5 * (width_ - toplevel->overview_width()) < magnified_x &&
          0.5 * (width_ + toplevel->overview_width()) >= magnified_x) {
        overview_panning_offset_ =
            toplevel->overview_x() +
            0.5 * toplevel->overview_width() -
            0.5 * width_;
      } else if (0.5 * (width_ - toplevel->overview_width()) > magnified_x) {
        overview_panning_offset_ = toplevel->overview_x() - magnified_x + 1;
      } else {
        overview_panning_offset_ = toplevel->overview_x() - magnified_x +
                                   toplevel->overview_width() - 1;
      }
    }
  }
}

void LayoutManager::ConfigureWindowsForOverviewMode(bool incremental) {
  ToplevelWindow* toplevel_to_right = NULL;
  // We iterate through the windows in descending stacking order
  // (right-to-left).  Otherwise, we'd get spurious pointer enter events as
  // a result of stacking a window underneath the pointer immediately
  // before we stack the window to its right directly on top of it.
  for (ToplevelWindows::reverse_iterator it = toplevels_.rbegin();
       it != toplevels_.rend(); ++it) {
    ToplevelWindow* toplevel = it->get();
    toplevel->ConfigureForOverviewMode(
        (it->get() == magnified_toplevel_),  // window_is_magnified
        (magnified_toplevel_ != NULL),       // dim_if_unmagnified
        toplevel_to_right,
        incremental);
    toplevel_to_right = toplevel;
  }
  if (!incremental && create_browser_window_) {
    // The 'create browser window' is always anchored to the right side
    // of the screen.
    create_browser_window_->MoveComposited(
        x_ + width_ - create_browser_window_->client_width() - kWindowPadding,
        y_ + height_ - create_browser_window_->client_height() -
        kCreateBrowserWindowVerticalPadding,
        0);
    create_browser_window_->MoveClientToComposited();
  }
}

LayoutManager::ToplevelWindow* LayoutManager::GetOverviewToplevelWindowAtPoint(
    int x, int y) const {
  for (int i = 0; static_cast<size_t>(i) < toplevels_.size(); ++i)
    if (toplevels_[i]->OverviewWindowContainsPoint(x, y))
      return toplevels_[i].get();
  return NULL;
}

void LayoutManager::AddKeyBindingsForMode(Mode mode) {
  DLOG(INFO) << "Adding key bindings for mode " << mode;
  KeyBindings* kb = wm_->key_bindings();

  switch (mode) {
    case MODE_ACTIVE:
      kb->AddBinding(KeyBindings::KeyCombo(XK_F12, 0),
                     "switch-to-overview-mode");
      kb->AddBinding(KeyBindings::KeyCombo(XK_Tab, KeyBindings::kAltMask),
                     "cycle-active-forward");
      kb->AddBinding(KeyBindings::KeyCombo(XK_F2, KeyBindings::kAltMask),
                     "cycle-active-forward");
      kb->AddBinding(
          KeyBindings::KeyCombo(
              XK_Tab, KeyBindings::kAltMask | KeyBindings::kShiftMask),
          "cycle-active-backward");
      kb->AddBinding(KeyBindings::KeyCombo(XK_F1, KeyBindings::kAltMask),
                     "cycle-active-backward");
      for (int i = 0; i < 8; ++i) {
        kb->AddBinding(KeyBindings::KeyCombo(XK_1 + i, KeyBindings::kAltMask),
                       StringPrintf("activate-toplevel-with-index-%d", i));
      }
      kb->AddBinding(KeyBindings::KeyCombo(XK_9, KeyBindings::kAltMask),
                     "activate-last-toplevel");
      kb->AddBinding(
          KeyBindings::KeyCombo(
              XK_w, KeyBindings::kControlMask | KeyBindings::kShiftMask),
              "delete-active-window");
      break;
    case MODE_OVERVIEW:
      kb->AddBinding(KeyBindings::KeyCombo(XK_Escape, 0),
                     "switch-to-active-mode");
      kb->AddBinding(KeyBindings::KeyCombo(XK_F12, 0),
                     "switch-to-active-mode");
      kb->AddBinding(KeyBindings::KeyCombo(XK_Return, 0),
                     "switch-to-active-mode-for-magnified");
      kb->AddBinding(KeyBindings::KeyCombo(XK_Right, 0),
                     "cycle-magnification-forward");
      kb->AddBinding(KeyBindings::KeyCombo(XK_Tab, KeyBindings::kAltMask),
                     "cycle-magnification-forward");
      kb->AddBinding(KeyBindings::KeyCombo(XK_F2, KeyBindings::kAltMask),
                     "cycle-magnification-forward");
      kb->AddBinding(KeyBindings::KeyCombo(XK_Left, 0),
                     "cycle-magnification-backward");
      kb->AddBinding(
          KeyBindings::KeyCombo(
              XK_Tab, KeyBindings::kAltMask | KeyBindings::kShiftMask),
          "cycle-magnification-backward");
      kb->AddBinding(KeyBindings::KeyCombo(XK_F1, KeyBindings::kAltMask),
                     "cycle-magnification-backward");
      for (int i = 0; i < 8; ++i) {
        kb->AddBinding(KeyBindings::KeyCombo(XK_1 + i, KeyBindings::kAltMask),
                       StringPrintf("magnify-toplevel-with-index-%d", i));
      }
      kb->AddBinding(KeyBindings::KeyCombo(XK_9, KeyBindings::kAltMask),
                     "magnify-last-toplevel");
      kb->AddBinding(KeyBindings::KeyCombo(XK_h, KeyBindings::kAltMask),
                     "pan-overview-mode-left");
      kb->AddBinding(KeyBindings::KeyCombo(XK_l, KeyBindings::kAltMask),
                     "pan-overview-mode-right");
      break;
  }
}

void LayoutManager::RemoveKeyBindingsForMode(Mode mode) {
  DLOG(INFO) << "Removing key bindings for mode " << mode;
  KeyBindings* kb = wm_->key_bindings();

  switch (mode) {
    case MODE_ACTIVE:
      // TODO: Add some sort of "key bindings group" feature to the
      // KeyBindings class so that we don't need to explicitly repeat all
      // of the bindings from AddKeyBindingsForMode() here.
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_F12, 0));
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_Tab, KeyBindings::kAltMask));
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_F2, KeyBindings::kAltMask));
      kb->RemoveBinding(
          KeyBindings::KeyCombo(
              XK_Tab, KeyBindings::kAltMask | KeyBindings::kShiftMask));
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_F1, KeyBindings::kAltMask));
      for (int i = 0; i < 9; ++i) {
        kb->RemoveBinding(
            KeyBindings::KeyCombo(XK_1 + i, KeyBindings::kAltMask));
      }
      kb->RemoveBinding(
          KeyBindings::KeyCombo(
              XK_w, KeyBindings::kControlMask | KeyBindings::kShiftMask));
      break;
    case MODE_OVERVIEW:
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_Escape, 0));
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_F12, 0));
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_Return, 0));
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_Right, 0));
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_Tab, KeyBindings::kAltMask));
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_F2, KeyBindings::kAltMask));
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_Left, 0));
      kb->RemoveBinding(
          KeyBindings::KeyCombo(
              XK_Tab, KeyBindings::kAltMask | KeyBindings::kShiftMask));
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_F1, KeyBindings::kAltMask));
      for (int i = 0; i < 9; ++i) {
        kb->RemoveBinding(
            KeyBindings::KeyCombo(XK_1 + i, KeyBindings::kAltMask));
      }
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_h, KeyBindings::kAltMask));
      kb->RemoveBinding(KeyBindings::KeyCombo(XK_l, KeyBindings::kAltMask));
      break;
  }
}

void LayoutManager::CycleActiveToplevelWindow(bool forward) {
  if (mode_ != MODE_ACTIVE) {
    LOG(WARNING) << "Ignoring request to cycle active toplevel outside of "
                 << "active mode (current mode is " << mode_ << ")";
    return;
  }
  if (toplevels_.empty())
    return;

  ToplevelWindow* toplevel = NULL;
  if (!active_toplevel_) {
    toplevel = forward ?
        toplevels_[0].get() :
        toplevels_[toplevels_.size()-1].get();
  } else {
    if (toplevels_.size() == 1)
      return;
    int old_index = GetIndexForToplevelWindow(*active_toplevel_);
    int new_index = (toplevels_.size() + old_index + (forward ? 1 : -1)) %
                    toplevels_.size();
    toplevel = toplevels_[new_index].get();
  }
  CHECK(toplevel);

  SetActiveToplevelWindow(
      toplevel,
      forward ?
        ToplevelWindow::STATE_ACTIVE_MODE_IN_FROM_RIGHT :
        ToplevelWindow::STATE_ACTIVE_MODE_IN_FROM_LEFT,
      forward ?
        ToplevelWindow::STATE_ACTIVE_MODE_OUT_TO_LEFT :
        ToplevelWindow::STATE_ACTIVE_MODE_OUT_TO_RIGHT);
}

void LayoutManager::CycleMagnifiedToplevelWindow(bool forward) {
  if (mode_ != MODE_OVERVIEW) {
    LOG(WARNING) << "Ignoring request to cycle magnified toplevel outside of "
                 << "overview mode (current mode is " << mode_ << ")";
    return;
  }
  if (toplevels_.empty())
    return;
  if (magnified_toplevel_ && toplevels_.size() == 1)
    return;

  if (!magnified_toplevel_ && !active_toplevel_) {
    // If we have no clue about which window to magnify, just choose the
    // first one.
    SetMagnifiedToplevelWindow(toplevels_[0].get());
  } else {
    if (!magnified_toplevel_) {
      // If no toplevel window is magnified, pretend like the active
      // toplevel was magnified so we'll move either to its left or its
      // right.
      magnified_toplevel_ = active_toplevel_;
    }
    CHECK(magnified_toplevel_);
    int old_index = GetIndexForToplevelWindow(*magnified_toplevel_);
    int new_index = (toplevels_.size() + old_index + (forward ? 1 : -1)) %
                    toplevels_.size();
    SetMagnifiedToplevelWindow(toplevels_[new_index].get());
  }
  LayoutToplevelWindowsForOverviewMode(0.5 * width_);
}

void LayoutManager::SetMagnifiedToplevelWindow(ToplevelWindow* toplevel) {
  if (magnified_toplevel_ == toplevel)
    return;
  magnified_toplevel_ = toplevel;
}

void LayoutManager::SendModeMessage(ToplevelWindow* toplevel) {
  if (!toplevel ||
      toplevel->win()->type() != WmIpc::WINDOW_TYPE_CHROME_TOPLEVEL) {
    return;
  }

  WmIpc::Message msg(WmIpc::Message::CHROME_NOTIFY_LAYOUT_MODE);
  switch (mode_) {
    // Set the mode in the message using the appropriate value from wm_ipc.h.
    case MODE_ACTIVE:
      msg.set_param(0, 0);
      break;
    case MODE_OVERVIEW:
      msg.set_param(0, 1);
      break;
    default:
      CHECK(false) << "Unhandled mode " << mode_;
  }
  wm_->wm_ipc()->SendMessage(toplevel->win()->xid(), msg);
}

void LayoutManager::SendDeleteRequestToActiveWindow() {
  // TODO: If there's a focused transient window, the message should get
  // sent to it instead.
  if (mode_ == MODE_ACTIVE && active_toplevel_)
    active_toplevel_->win()->SendDeleteRequest(wm_->GetCurrentTimeFromServer());
}

void LayoutManager::PanOverviewMode(int offset) {
  overview_panning_offset_ += offset;
  if (mode_ == MODE_OVERVIEW)
    ConfigureWindowsForOverviewMode(false);  // incremental=false
}

void LayoutManager::UpdateOverviewPanningForMotion() {
  int dx = overview_background_event_coalescer_->x() - overview_drag_last_x_;
  overview_drag_last_x_ = overview_background_event_coalescer_->x();
  overview_panning_offset_ -= dx;
  ConfigureWindowsForOverviewMode(true);  // incremental=true
}

}  // namespace window_manager
