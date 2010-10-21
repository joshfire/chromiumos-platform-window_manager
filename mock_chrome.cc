// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/mock_chrome.h"

#include <algorithm>
#include <string>

#include <cairomm/context.h>

#include "base/command_line.h"
#include "base/string_util.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"
#include "window_manager/real_x_connection.h"
#include "window_manager/util.h"
#include "window_manager/wm_ipc.h"

DEFINE_string(image_dir, "data/", "Path to static image files");
DEFINE_string(new_panel_image, "data/panel_chat.png",
              "Image to use when creating a new panel");
DEFINE_int32(num_panels, 5, "Number of panels to open");
DEFINE_int32(num_windows, 3, "Number of windows to open");
DEFINE_string(panel_images, "data/panel_chat.png",
              "Comma-separated images to use for panels");
DEFINE_string(panel_titles, "Chat",
              "Comma-separated titles to use for panels");
DEFINE_string(screen_locker_image, "data/screen_locker.jpg",
              "Image to use for screen locker windows");
DEFINE_string(tab_images,
              "data/chrome_page_google.png,"
              "data/chrome_page_gmail.png,"
              "data/chrome_page_chrome.png",
              "Comma-separated images to use for tabs");
DEFINE_string(tab_titles, "Google,Gmail,Google Chrome",
              "Comma-separated titles to use for tabs");
DEFINE_int32(tabs_per_window, 3, "Number of tabs to add to each window");
DEFINE_int32(window_height, 640, "Window height");
DEFINE_int32(window_width, 920, "Window width");

using std::make_pair;
using std::max;
using std::string;
using std::tr1::shared_ptr;
using std::vector;
using window_manager::AtomCache;
using window_manager::RealXConnection;
using window_manager::WmIpc;
using window_manager::util::GetMonotonicTimeMs;

namespace mock_chrome {

const int PanelTitlebar::kWidth = 150;
const int PanelTitlebar::kHeight = 26;
Glib::RefPtr<Gdk::Pixbuf> PanelTitlebar::image_bg_;
Glib::RefPtr<Gdk::Pixbuf> PanelTitlebar::image_bg_focused_;
const char* PanelTitlebar::kFontFace = "Arial";
const double PanelTitlebar::kFontSize = 13;
const double PanelTitlebar::kFontPadding = 6;
const int PanelTitlebar::kDragThreshold = 10;

const int ChromeWindow::kTabDragThreshold = 10;
const char* ChromeWindow::kTabFontFace = "DejaVu Sans";
const double ChromeWindow::kTabFontSize = 13;
const int ChromeWindow::kTabFontPadding = 5;
const int ChromeWindow::kLockTimeoutMs = 750;
const int ChromeWindow::kShutdownTimeoutMs = 750;
const int ChromeWindow::kLockToShutdownThresholdMs = 200;

// Static images.
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_nav_bg_;
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_nav_left_;
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_nav_right_;
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_tab_bg_;
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_tab_hl_;
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_tab_nohl_;
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_tab_right_hl_left_nohl_;
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_tab_right_hl_left_none_;
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_tab_right_nohl_left_hl_;
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_tab_right_nohl_left_nohl_;
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_tab_right_nohl_left_none_;
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_tab_right_none_left_hl_;
Glib::RefPtr<Gdk::Pixbuf> ChromeWindow::image_tab_right_none_left_nohl_;

int ChromeWindow::kTabHeight = 0;
int ChromeWindow::kNavHeight = 0;

// Copy a GdkEventClient struct into an XEvent.
static bool GetWmIpcMessage(const GdkEventClient& event,
                            WmIpc* wm_ipc,
                            WmIpc::Message* msg_out) {
  return wm_ipc->GetMessage(
      GDK_WINDOW_XID(event.window),
      gdk_x11_atom_to_xatom(event.message_type),
      event.data_format,
      event.data.l,
      msg_out);
}

static void DrawImage(Glib::RefPtr<Gdk::Pixbuf>& image,  // NOLINT
                      Gtk::Widget* widget,
                      int dest_x, int dest_y,
                      int dest_width, int dest_height) {
  CHECK(widget);
  CHECK(dest_width > 0);
  CHECK(dest_height > 0);

  // Only scale the original image if we have to.
  Glib::RefPtr<Gdk::Pixbuf> scaled_image = image;
  if (dest_width != image->get_width() || dest_height != image->get_height()) {
    scaled_image = image->scale_simple(
        dest_width, dest_height, Gdk::INTERP_BILINEAR);
  }
  scaled_image->render_to_drawable(
      widget->get_window(),
      widget->get_style()->get_black_gc(),
      0, 0,             // src position
      dest_x, dest_y,   // dest position
      dest_width, dest_height,
      Gdk::RGB_DITHER_NONE,
      0, 0);  // x and y dither
}

Tab::Tab(const string& image_filename, const string& title)
    : image_(Gdk::Pixbuf::create_from_file(image_filename)),
      title_(title) {
}

void Tab::RenderToGtkWidget(Gtk::Widget* widget,
                            int x, int y,
                            int width, int height) {
  DrawImage(image_, widget, x, y, width, height);
}

ChromeWindow::ChromeWindow(MockChrome* chrome, int width, int height)
    : chrome_(chrome),
      width_(width),
      height_(height),
      active_tab_index_(-1),
      dragging_tab_(false),
      tab_drag_start_offset_x_(0),
      tab_drag_start_offset_y_(0),
      fullscreen_(false),
      power_button_is_pressed_(false),
      lock_timeout_id_(-1),
      lock_to_shutdown_timeout_id_(-1),
      shutdown_timeout_id_(-1) {
  if (!image_nav_bg_) {
    InitImages();
  }

  set_size_request(width_, height_);

  realize();
  xid_ = GDK_WINDOW_XWINDOW(Glib::unwrap(get_window()));
  CHECK(chrome_->wm_ipc()->SetWindowType(
            xid(), chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL, NULL));
  add_events(Gdk::BUTTON_PRESS_MASK |
             Gdk::BUTTON_RELEASE_MASK |
             Gdk::POINTER_MOTION_MASK);

  show_all();
}

void ChromeWindow::InsertTab(Tab* tab, size_t index) {
  shared_ptr<TabInfo> info(new TabInfo(tab));
  if (index > tabs_.size()) {
    index = tabs_.size();
  }

  tabs_.insert(tabs_.begin() + index, info);
  if (static_cast<int>(index) <= active_tab_index_) {
    active_tab_index_++;
  }
  if (active_tab_index_ < 0) {
    active_tab_index_ = 0;
    DrawView();
  }
  DrawTabs();
}

Tab* ChromeWindow::RemoveTab(size_t index) {
  CHECK(index < tabs_.size());
  shared_ptr<TabInfo> info = tabs_[index];
  tabs_.erase(tabs_.begin() + index);
  if (active_tab_index_ >= static_cast<int>(tabs_.size())) {
    active_tab_index_ = static_cast<int>(tabs_.size()) - 1;
  }
  return info->tab.release();
}

void ChromeWindow::ActivateTab(int index) {
  CHECK(index >= 0);
  CHECK(index < static_cast<int>(tabs_.size()));
  if (index == active_tab_index_) {
    return;
  }
  active_tab_index_ = index;
  DrawTabs();
  DrawView();
}

// static
void ChromeWindow::InitImages() {
  CHECK(!image_nav_bg_);

  image_nav_bg_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_nav_bg.png");
  image_nav_left_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_nav_left.png");
  image_nav_right_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_nav_right.png");
  image_tab_bg_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_tab_bg.png");
  image_tab_hl_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_tab_hl.png");
  image_tab_nohl_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_tab_nohl.png");
  image_tab_right_hl_left_nohl_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_tab_right_hl_left_nohl.png");
  image_tab_right_hl_left_none_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_tab_right_hl_left_none.png");
  image_tab_right_nohl_left_hl_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_tab_right_nohl_left_hl.png");
  image_tab_right_nohl_left_nohl_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_tab_right_nohl_left_nohl.png");
  image_tab_right_nohl_left_none_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_tab_right_nohl_left_none.png");
  image_tab_right_none_left_hl_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_tab_right_none_left_hl.png");
  image_tab_right_none_left_nohl_ = Gdk::Pixbuf::create_from_file(
      FLAGS_image_dir + "chrome_tab_right_none_left_nohl.png");

  kTabHeight = image_tab_hl_->get_height();
  kNavHeight = image_nav_left_->get_height();
}

void ChromeWindow::DrawTabs() {
  Cairo::RefPtr<Cairo::Context> cr =
      get_window()->create_cairo_context();
  cr->select_font_face(kTabFontFace,
                       Cairo::FONT_SLANT_NORMAL,
                       Cairo::FONT_WEIGHT_NORMAL);
  cr->set_font_size(kTabFontSize);

  Cairo::FontOptions font_options;
  font_options.set_hint_style(Cairo::HINT_STYLE_MEDIUM);
  font_options.set_hint_metrics(Cairo::HINT_METRICS_ON);
  font_options.set_antialias(Cairo::ANTIALIAS_GRAY);
  cr->set_font_options(font_options);

  Cairo::FontExtents extents;
  cr->get_font_extents(extents);

  cr->set_source_rgb(0, 0, 0);

  int x_offset = 0;
  for (int i = 0; static_cast<size_t>(i) < tabs_.size(); ++i) {
    bool active = (i == active_tab_index_);
    tabs_[i]->start_x = x_offset;

    // Draw the image on the left.
    if (i == 0) {
      Glib::RefPtr<Gdk::Pixbuf> left_image =
          active ?
          image_tab_right_none_left_hl_ :
          image_tab_right_none_left_nohl_;
      DrawImage(left_image,
                this,
                x_offset, 0,
                left_image->get_width(), left_image->get_height());
      x_offset += left_image->get_width();
    }

    // Draw the tab's background and its title.
    Glib::RefPtr<Gdk::Pixbuf> image = active ? image_tab_hl_ : image_tab_nohl_;
    DrawImage(image,
              this,
              x_offset, 0,
              image->get_width(), image->get_height());
    cr->move_to(x_offset + kTabFontPadding, extents.ascent + kTabFontPadding);
    cr->show_text(tabs_[i]->tab->title());
    x_offset += image->get_width();

    // Draw the image on the right.
    Glib::RefPtr<Gdk::Pixbuf> right_image;
    if (static_cast<size_t>(i) == tabs_.size() - 1) {
      // Last tab.
      if (active) {
        right_image = image_tab_right_hl_left_none_;
      } else {
        right_image = image_tab_right_nohl_left_none_;
      }
    } else if (active) {
      // Active tab.
      right_image = image_tab_right_hl_left_nohl_;
    } else if (i + 1 == active_tab_index_) {
      // Next tab is active.
      right_image = image_tab_right_nohl_left_hl_;
    } else {
      // Neither tab is active.
      right_image = image_tab_right_nohl_left_nohl_;
    }
    DrawImage(right_image,
              this,
              x_offset, 0,  // x, y
              right_image->get_width(), right_image->get_height());
    x_offset += right_image->get_width();

    tabs_[i]->width = x_offset - tabs_[i]->start_x;
  }

  if (x_offset < width_) {
    DrawImage(image_tab_bg_, this,
              x_offset, 0,  // x, y
              width_ - x_offset, image_tab_bg_->get_height());
  }
}

void ChromeWindow::DrawNavBar() {
  DrawImage(image_nav_bg_, this,
            0, kTabHeight,  // x, y
            width_, image_nav_bg_->get_height());

  DrawImage(image_nav_left_, this,
            0, kTabHeight,  // x, y
            image_nav_left_->get_width(), image_nav_left_->get_height());
  DrawImage(image_nav_right_, this,
            width_ - image_nav_right_->get_width(), kTabHeight,  // x, y
            image_nav_right_->get_width(), image_nav_right_->get_height());
}

void ChromeWindow::DrawView() {
  int x = 0;
  int y = kTabHeight + kNavHeight;
  int width = width_;
  int height = height_ - y;

  if (active_tab_index_ >= 0) {
    CHECK(active_tab_index_ < static_cast<int>(tabs_.size()));
    tabs_[active_tab_index_]->tab->RenderToGtkWidget(this, x, y, width, height);
  } else {
    get_window()->clear_area(x, y, width, height);
  }
}

int ChromeWindow::GetTabIndexAtXPosition(int x) const {
  if (x < 0) {
    return -1;
  }

  for (int i = 0; static_cast<size_t>(i) < tabs_.size(); ++i) {
    if (x >= tabs_[i]->start_x && x < tabs_[i]->start_x + tabs_[i]->width) {
      return i;
    }
  }

  return tabs_.size();
}

void ChromeWindow::OnLockTimeout() {
  lock_timeout_id_ = -1;
  chrome_->LockScreen();
  lock_to_shutdown_timeout_id_ =
      g_timeout_add(kLockToShutdownThresholdMs,
                    ChromeWindow::OnPreShutdownTimeoutThunk,
                    this);
}

void ChromeWindow::OnLockToShutdownTimeout() {
  lock_to_shutdown_timeout_id_ = -1;
  AddShutdownTimeout();
}

void ChromeWindow::OnShutdownTimeout() {
  shutdown_timeout_id_ = -1;
  chrome_->ShutDown();
}

void ChromeWindow::AddShutdownTimeout() {
  WmIpc::Message msg(
      chromeos::WM_IPC_MESSAGE_WM_NOTIFY_POWER_BUTTON_STATE);
  msg.set_param(0, chromeos::WM_IPC_POWER_BUTTON_PRE_SHUTDOWN);
  chrome_->wm_ipc()->SendMessage(chrome_->wm_ipc()->wm_window(), msg);
  shutdown_timeout_id_ =
      g_timeout_add(kShutdownTimeoutMs,
                    ChromeWindow::OnShutdownTimeoutThunk,
                    this);
}

bool ChromeWindow::on_button_press_event(GdkEventButton* event) {
  if (event->button == 2) {
    chrome_->CloseWindow(this);
    return true;
  } else if (event->button != 1) {
    return false;
  }

  DLOG(INFO) << "Got mouse down at (" << event->x << ", " << event->y << ")";
  if (event->y < 0 || event->y > kTabHeight) {
    // Don't do anything for clicks outside of the tab bar.
    return false;
  }

  int tab_index = GetTabIndexAtXPosition(event->x);
  if (tab_index < 0 || tab_index >= static_cast<int>(tabs_.size())) {
    // Ignore clicks outside of tabs.
    return false;
  }

  dragging_tab_ = true;
  tab_drag_start_offset_x_ = event->x - tabs_[tab_index]->start_x;
  tab_drag_start_offset_y_ = event->y;
  if (tab_index != active_tab_index_) {
    CHECK(tab_index < static_cast<int>(tabs_.size()));
    active_tab_index_ = tab_index;
    DrawTabs();
    DrawView();
  }
  return true;
}

bool ChromeWindow::on_button_release_event(GdkEventButton* event) {
  if (event->button != 1) {
    return false;
  }
  DLOG(INFO) << "Got mouse up at (" << event->x << ", " << event->y << ")";
  dragging_tab_ = false;
  return true;
}

bool ChromeWindow::on_motion_notify_event(GdkEventMotion* event) {
  if (!dragging_tab_) return false;

  DLOG(INFO) << "Got motion at (" << event->x << ", " << event->y << ")";
  if (active_tab_index_ >= 0) {
    int tab_index = GetTabIndexAtXPosition(event->x);
    // The tab is still within the tab bar; move it to a new position.
    if (tab_index >= static_cast<int>(tabs_.size())) {
      // GetTabIndexAtXPosition() returns tabs_.size() for positions in
      // the empty space at the right of the tab bar, but we need to
      // treat that space as belonging to the last tab when reordering.
      tab_index = tabs_.size() - 1;
    } else if (tab_index < 0) {
      tab_index = 0;
    }
    if (tab_index != active_tab_index_) {
      Tab* tab = RemoveTab(active_tab_index_);
      InsertTab(tab, tab_index);
      active_tab_index_ = tab_index;
      DrawTabs();
    }
  }
  return true;
}

bool ChromeWindow::on_key_press_event(GdkEventKey* event) {
  if (strcmp(event->string, "p") == 0) {
    // Create a new panel.
    chrome_->CreatePanel(FLAGS_new_panel_image, "New Panel", true);
    // Create a new window.
  } else if (strcmp(event->string, "w") == 0) {
    chrome_->CreateWindow(width_, height_);
    // Toggle fullscreen mode.
  } else if (strcmp(event->string, "f") == 0) {
    if (fullscreen_) {
      unfullscreen();
    } else {
      fullscreen();
    }
  } else if (strcmp(event->string, "l") == 0) {
    // Pretend that the power button has been pressed.
    if (!power_button_is_pressed_) {
      power_button_is_pressed_ = true;
      if (!chrome_->is_locked()) {
        WmIpc::Message msg(
            chromeos::WM_IPC_MESSAGE_WM_NOTIFY_POWER_BUTTON_STATE);
        msg.set_param(0, chromeos::WM_IPC_POWER_BUTTON_PRE_LOCK);
        chrome_->wm_ipc()->SendMessage(chrome_->wm_ipc()->wm_window(), msg);
        lock_timeout_id_ = g_timeout_add(kLockTimeoutMs,
                                         ChromeWindow::OnLockTimeoutThunk,
                                         this);
      } else if (!chrome_->is_shutting_down()) {
        AddShutdownTimeout();
      }
    }
  } else if (strcmp(event->string, "u") == 0) {
    // Unlock the screen.
    if (chrome_->is_locked())
      chrome_->UnlockScreen();
  }
  return true;
}

bool ChromeWindow::on_key_release_event(GdkEventKey* event) {
  // X reports autorepeated key events similarly to individual key presses, but
  // we can detect that a release event is part of an autorepeated sequence by
  // checking if the next event in the queue is a press event with a matching
  // timestamp.
  bool repeated = false;
  if (XPending(GDK_DISPLAY())) {
    XEvent xevent;
    XPeekEvent(GDK_DISPLAY(), &xevent);
    if (xevent.type == KeyPress &&
        xevent.xkey.keycode == event->hardware_keycode &&
        xevent.xkey.time == event->time) {
      repeated = true;
    }
  }

  if (strcmp(event->string, "l") == 0) {
    // Pretend that the power button has been released.
    if (!repeated) {
      power_button_is_pressed_ = false;
      if (lock_timeout_id_ >= 0) {
        g_source_remove(lock_timeout_id_);
        lock_timeout_id_ = -1;
        WmIpc::Message msg(
            chromeos::WM_IPC_MESSAGE_WM_NOTIFY_POWER_BUTTON_STATE);
        msg.set_param(0, chromeos::WM_IPC_POWER_BUTTON_ABORTED_LOCK);
        chrome_->wm_ipc()->SendMessage(chrome_->wm_ipc()->wm_window(), msg);
      } else if (lock_to_shutdown_timeout_id_ >= 0) {
        g_source_remove(lock_to_shutdown_timeout_id_);
        lock_to_shutdown_timeout_id_ = -1;
      } else if (shutdown_timeout_id_ >= 0) {
        g_source_remove(shutdown_timeout_id_);
        shutdown_timeout_id_ = -1;
        WmIpc::Message msg(
            chromeos::WM_IPC_MESSAGE_WM_NOTIFY_POWER_BUTTON_STATE);
        msg.set_param(0, chromeos::WM_IPC_POWER_BUTTON_ABORTED_SHUTDOWN);
        chrome_->wm_ipc()->SendMessage(chrome_->wm_ipc()->wm_window(), msg);
      }
    }
  }
  return true;
}

bool ChromeWindow::on_expose_event(GdkEventExpose* event) {
  DrawTabs();
  DrawNavBar();
  DrawView();
  return true;
}

bool ChromeWindow::on_client_event(GdkEventClient* event) {
  WmIpc::Message msg;
  if (!GetWmIpcMessage(*event, chrome_->wm_ipc(), &msg))
    return false;

  DLOG(INFO) << "Got message of type " << msg.type();
  switch (msg.type()) {
    default:
      LOG(WARNING) << "Ignoring WM message of unknown type " << msg.type();
      return false;
  }
  return true;
}

bool ChromeWindow::on_configure_event(GdkEventConfigure* event) {
  width_ = event->width;
  height_ = event->height;
  DrawView();
  Gtk::Window::on_configure_event(event);
  return false;
}

bool ChromeWindow::on_window_state_event(GdkEventWindowState* event) {
  fullscreen_ = (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN);
  DLOG(INFO) << "Fullscreen mode set to " << fullscreen_;
  return true;
}

PanelTitlebar::PanelTitlebar(Panel* panel)
    : panel_(panel),
      mouse_down_(false),
      mouse_down_abs_x_(0),
      mouse_down_abs_y_(0),
      mouse_down_offset_x_(0),
      mouse_down_offset_y_(0),
      dragging_(false),
      focused_(false) {
  if (!image_bg_) {
    image_bg_ = Gdk::Pixbuf::create_from_file(
        FLAGS_image_dir + "panel_titlebar_bg.png");
    image_bg_focused_ = Gdk::Pixbuf::create_from_file(
        FLAGS_image_dir + "panel_titlebar_bg_focused.png");
  }
  set_size_request(kWidth, kHeight);
  realize();
  xid_ = GDK_WINDOW_XWINDOW(Glib::unwrap(get_window()));
  CHECK(panel_->chrome()->wm_ipc()->SetWindowType(
            xid_, chromeos::WM_IPC_WINDOW_CHROME_PANEL_TITLEBAR, NULL));
  add_events(Gdk::BUTTON_PRESS_MASK |
             Gdk::BUTTON_RELEASE_MASK |
             Gdk::POINTER_MOTION_MASK);
  show_all();
}

void PanelTitlebar::Draw() {
  DrawImage((focused_ ? image_bg_focused_ : image_bg_),
            this, 0, 0, get_width(), get_height());

  Cairo::RefPtr<Cairo::Context> cr =
      get_window()->create_cairo_context();
  cr->select_font_face(kFontFace,
                       Cairo::FONT_SLANT_NORMAL,
                       Cairo::FONT_WEIGHT_BOLD);
  cr->set_font_size(kFontSize);

  Cairo::FontOptions font_options;
  font_options.set_hint_style(Cairo::HINT_STYLE_MEDIUM);
  font_options.set_hint_metrics(Cairo::HINT_METRICS_ON);
  font_options.set_antialias(Cairo::ANTIALIAS_GRAY);
  cr->set_font_options(font_options);

  Cairo::FontExtents extents;
  cr->get_font_extents(extents);
  int x = kFontPadding;
  int y = kFontPadding + extents.ascent;

  cr->set_source_rgb(1, 1, 1);
  cr->move_to(x, y);
  cr->show_text(panel_->title());
}

bool PanelTitlebar::on_expose_event(GdkEventExpose* event) {
  Draw();
  return true;
}

bool PanelTitlebar::on_button_press_event(GdkEventButton* event) {
  if (event->button == 2) {
    panel_->chrome()->ClosePanel(panel_);
    return true;
  } else if (event->button != 1) {
    return false;
  }
  mouse_down_ = true;
  mouse_down_abs_x_ = event->x_root;
  mouse_down_abs_y_ = event->y_root;

  int width = 1, height = 1;
  get_size(width, height);
  mouse_down_offset_x_ = event->x - width;
  mouse_down_offset_y_ = event->y;
  dragging_ = false;
  return true;
}

bool PanelTitlebar::on_button_release_event(GdkEventButton* event) {
  if (event->button != 1) {
    return false;
  }
  // Only handle clicks that started in our window.
  if (!mouse_down_) {
    return false;
  }

  mouse_down_ = false;
  if (!dragging_) {
    WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_SET_PANEL_STATE);
    msg.set_param(0, panel_->xid());
    msg.set_param(1, !(panel_->expanded()));
    CHECK(panel_->chrome()->wm_ipc()->SendMessage(
              panel_->chrome()->wm_ipc()->wm_window(), msg));

    // If the panel is getting expanded, tell the WM to focus it.
    if (!panel_->expanded())
      panel_->present();
  } else {
    WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_PANEL_DRAG_COMPLETE);
    msg.set_param(0, panel_->xid());
    CHECK(panel_->chrome()->wm_ipc()->SendMessage(
              panel_->chrome()->wm_ipc()->wm_window(), msg));
    dragging_ = false;
  }
  return true;
}

bool PanelTitlebar::on_motion_notify_event(GdkEventMotion* event) {
  if (!mouse_down_) {
    return false;
  }

  if (!dragging_) {
    if (abs(event->x_root - mouse_down_abs_x_) >= kDragThreshold ||
        abs(event->y_root - mouse_down_abs_y_) >= kDragThreshold) {
      dragging_ = true;
    }
  }
  if (dragging_) {
    WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_PANEL_DRAGGED);
    msg.set_param(0, panel_->xid());
    msg.set_param(1, event->x_root - mouse_down_offset_x_);
    msg.set_param(2, event->y_root - mouse_down_offset_y_);
    CHECK(panel_->chrome()->wm_ipc()->SendMessage(
              panel_->chrome()->wm_ipc()->wm_window(), msg));
  }
  return true;
}

Panel::Panel(MockChrome* chrome,
             const string& image_filename,
             const string& title,
             bool expanded)
    : chrome_(chrome),
      titlebar_(new PanelTitlebar(this)),
      image_(Gdk::Pixbuf::create_from_file(image_filename)),
      width_(image_->get_width()),
      height_(image_->get_height()),
      expanded_(false),
      title_(title),
      fullscreen_(false) {
  set_size_request(width_, height_);
  realize();
  xid_ = GDK_WINDOW_XWINDOW(Glib::unwrap(get_window()));
  vector<int> type_params;
  type_params.push_back(titlebar_->xid());
  type_params.push_back(expanded);
  CHECK(chrome_->wm_ipc()->SetWindowType(
            xid_, chromeos::WM_IPC_WINDOW_CHROME_PANEL_CONTENT, &type_params));
  add_events(Gdk::BUTTON_PRESS_MASK);
  show_all();
}

bool Panel::on_expose_event(GdkEventExpose* event) {
  DrawImage(image_, this, 0, 0, width_, height_);
  return true;
}

bool Panel::on_button_press_event(GdkEventButton* event) {
  DLOG(INFO) << "Panel " << xid_ << " got button " << event->button;
  if (event->button == 2) {
    chrome_->ClosePanel(this);
  }
  return true;
}

bool Panel::on_key_press_event(GdkEventKey* event) {
  if (strcmp(event->string, "+") == 0) {
    width_ += 10;
    height_ += 10;
    resize(width_, height_);
  } else if (strcmp(event->string, "-") == 0) {
    width_ = max(width_ - 10, 1);
    height_ = max(height_ - 10, 1);
    resize(width_, height_);
  } else if (strcmp(event->string, "f") == 0) {
    fullscreen_ = !fullscreen_;
    if (fullscreen_)
      fullscreen();
    else
      unfullscreen();
  } else if (strcmp(event->string, "u") == 0) {
    set_urgency_hint(!get_urgency_hint());
  } else {
    DLOG(INFO) << "Panel " << xid_ << " got key press " << event->string;
  }
  return true;
}

bool Panel::on_client_event(GdkEventClient* event) {
  WmIpc::Message msg;
  if (!GetWmIpcMessage(*event, chrome_->wm_ipc(), &msg))
    return false;

  DLOG(INFO) << "Got message of type " << msg.type();
  switch (msg.type()) {
    case chromeos::WM_IPC_MESSAGE_CHROME_NOTIFY_PANEL_STATE: {
      expanded_ = msg.param(0);
      break;
    }
    default:
      LOG(WARNING) << "Ignoring WM message of unknown type " << msg.type();
      return false;
  }
  return true;
}

bool Panel::on_focus_in_event(GdkEventFocus* event) {
  titlebar_->set_focused(true);
  titlebar_->Draw();
  return true;
}

bool Panel::on_focus_out_event(GdkEventFocus* event) {
  titlebar_->set_focused(false);
  titlebar_->Draw();
  return true;
}

bool Panel::on_window_state_event(GdkEventWindowState* event) {
  fullscreen_ = (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN);
  return true;
}


ScreenLockWindow::ScreenLockWindow(MockChrome* chrome)
    : chrome_(chrome),
      image_(Gdk::Pixbuf::create_from_file(FLAGS_screen_locker_image)) {
  GdkScreen* screen = gdk_screen_get_default();
  set_size_request(gdk_screen_get_width(screen),
                   gdk_screen_get_height(screen));
  realize();
  xid_ = GDK_WINDOW_XWINDOW(Glib::unwrap(get_window()));
  CHECK(chrome_->wm_ipc()->SetWindowType(
            xid(), chromeos::WM_IPC_WINDOW_CHROME_SCREEN_LOCKER, NULL));
  show_all();
}

void ScreenLockWindow::Draw() {
  DrawImage(image_,
            this,  // dest
            0, 0,  // x, y
            get_width(), get_height());
}

bool ScreenLockWindow::on_expose_event(GdkEventExpose* event) {
  Draw();
  return false;
}

bool ScreenLockWindow::on_configure_event(GdkEventConfigure* event) {
  Draw();
  Gtk::Window::on_configure_event(event);
  return false;
}


MockChrome::MockChrome()
    : xconn_(new RealXConnection(GDK_DISPLAY())),
      atom_cache_(new AtomCache(xconn_.get())),
      wm_ipc_(new WmIpc(xconn_.get(), atom_cache_.get())),
      is_shutting_down_(false) {
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_IPC_VERSION);
  msg.set_param(0, 1);
  wm_ipc_->SendMessage(wm_ipc_->wm_window(), msg);
}

ChromeWindow* MockChrome::CreateWindow(int width, int height) {
  shared_ptr<ChromeWindow> win(new ChromeWindow(this, width, height));
  CHECK(windows_.insert(make_pair(win->xid(), win)).second);
  return win.get();
}

void MockChrome::CloseWindow(ChromeWindow* win) {
  CHECK(win);
  CHECK(windows_.erase(win->xid()) == 1);
}

Panel* MockChrome::CreatePanel(const string& image_filename,
                               const string& title,
                               bool expanded) {
  shared_ptr<Panel> panel(
      new Panel(this, image_filename, title, expanded));
  CHECK(panels_.insert(make_pair(panel->xid(), panel)).second);
  return panel.get();
}

void MockChrome::ClosePanel(Panel* panel) {
  CHECK(panel);
  CHECK(panels_.erase(panel->xid()) == 1);
}

void MockChrome::LockScreen() {
  if (screen_lock_window_.get())
    return;

  LOG(INFO) << "Locking screen";
  screen_lock_window_.reset(new ScreenLockWindow(this));
}

void MockChrome::UnlockScreen() {
  if (!screen_lock_window_.get())
    return;

  LOG(INFO) << "Unlocking screen";
  screen_lock_window_.reset();
}

void MockChrome::ShutDown() {
  if (is_shutting_down_)
    return;

  LOG(INFO) << "Shutting down";
  is_shutting_down_ = true;
  WmIpc::Message msg(
      chromeos::WM_IPC_MESSAGE_WM_NOTIFY_SHUTTING_DOWN);
  wm_ipc_->SendMessage(wm_ipc_->wm_window(), msg);
}

}  // namespace mock_chrome


int main(int argc, char** argv) {
  Gtk::Main kit(argc, argv);
  google::ParseCommandLineFlags(&argc, &argv, true);
  CommandLine::Init(argc, argv);
  logging::InitLogging(NULL,
                       logging::LOG_ONLY_TO_SYSTEM_DEBUG_LOG,
                       logging::DONT_LOCK_LOG_FILE,
                       logging::APPEND_TO_OLD_LOG_FILE);

  vector<string> filenames;
  SplitString(FLAGS_tab_images, ',', &filenames);
  CHECK(!filenames.empty())
      << "At least one image must be supplied using --tab_images";

  vector<string> titles;
  SplitString(FLAGS_tab_titles, ',', &titles);
  CHECK(filenames.size() == titles.size())
      << "Must specify same number of tab images and titles";

  mock_chrome::MockChrome mock_chrome;
  for (int i = 0; i < FLAGS_num_windows; ++i) {
    mock_chrome::ChromeWindow* win =
        mock_chrome.CreateWindow(FLAGS_window_width, FLAGS_window_height);
    for (int j = 0; j < FLAGS_tabs_per_window; ++j) {
      win->InsertTab(new mock_chrome::Tab(filenames[j % filenames.size()],
                                          titles[j % titles.size()]),
                     win->num_tabs());
    }
    win->ActivateTab(i % win->num_tabs());
  }

  filenames.clear();
  SplitString(FLAGS_panel_images, ',', &filenames);
  CHECK(!filenames.empty())
      << "At least one image must be supplied using --panel_images";

  titles.clear();
  SplitString(FLAGS_panel_titles, ',', &titles);
  CHECK(filenames.size() == titles.size())
      << "Must specify same number of panel images and titles";

  for (int i = 0; i < FLAGS_num_panels; ++i) {
    mock_chrome.CreatePanel(filenames[i % filenames.size()],
                            titles[i % titles.size()],
                            false);  // expanded=false
  }

  Gtk::Main::run();
  return 0;
}
