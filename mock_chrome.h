// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_MOCK_CHROME_H_
#define WINDOW_MANAGER_MOCK_CHROME_H_

#include <map>
#include <string>
#include <tr1/memory>
#include <vector>

#include <gflags/gflags.h>
#include <gtkmm.h>
extern "C" {
#include <gdk/gdkx.h>
}

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "window_manager/util.h"

// This file implements a small app that displays windows containing Chrome
// screenshots and allows tabs to be dragged between them.  Its intent is
// to provide a way to quickly mock out different types of interactions
// between Chrome and the window manager.

typedef ::Window XWindow;

namespace window_manager {
class AtomCache;
class WmIpc;
class XConnection;
}

namespace mock_chrome {

class ChromeWindow;
class MockChrome;
class Panel;

// A tab is just a wrapper around an image.  Each tab is owned by a window.
class Tab {
 public:
  Tab(const std::string& image_filename, const std::string& title);

  const std::string& title() const { return title_; }

  // Draw the tab's image to the passed-in widget.  The image can be
  // positioned and scaled within the widget.
  void RenderToGtkWidget(
      Gtk::Widget* widget, int x, int y, int width, int height);

 private:
  Glib::RefPtr<Gdk::Pixbuf> image_;
  std::string title_;

  DISALLOW_COPY_AND_ASSIGN(Tab);
};

// This is an actual GTK+ window that holds a collection of tabs, one of
// which is active and rendered inside of the window.
class ChromeWindow : public Gtk::Window {
 public:
  ChromeWindow(MockChrome* chrome, int width, int height);
  virtual ~ChromeWindow() {}

  MockChrome* chrome() { return chrome_; }
  XWindow xid() { return xid_; }
  int width() const { return width_; }
  int height() const { return height_; }
  size_t num_tabs() const { return tabs_.size(); }
  Tab* tab(size_t index) { return tabs_[index]->tab.get(); }
  int active_tab_index() const { return active_tab_index_; }

  // Insert a tab into this window.  The window takes ownership of the tab.
  // |index| values greater than the current number of tabs will result in
  // the tab being appended at the end.
  // TODO: Clean up which methods do redraws and which don't.
  void InsertTab(Tab* tab, size_t index);

  // Remove a tab from the window.  Ownership of the tab is transferred to
  // the caller.
  Tab* RemoveTab(size_t index);

  void ActivateTab(int index);

 private:
  struct TabInfo {
    explicit TabInfo(Tab* tab)
        : tab(tab),
          start_x(0),
          width(0) {
    }

    scoped_ptr<Tab> tab;
    int start_x;
    int width;

    DISALLOW_COPY_AND_ASSIGN(TabInfo);
  };

  // Initialize static image data.
  static void InitImages();

  // Draw the tab strip.  Also updates tab position info inside of |tabs_|.
  void DrawTabs();

  // Draw the navigation bar underneath the tab strip.
  void DrawNavBar();

  // Draw the page contents.  If |active_tab_index_| >= 0, this will be the
  // image from the currently-selected tab; otherwise it will just be a
  // gray box.
  void DrawView();

  // Get the 0-indexed number of the tab at the given position, relative to
  // the left side of the window.  The portion of the tab bar to the right
  // of any tabs returns an index equal to the number of tabs.
  int GetTabIndexAtXPosition(int x) const;

  // Lock the screen and start the lock-to-shutdown timeout.
  static gboolean OnLockTimeoutThunk(gpointer data) {
    reinterpret_cast<ChromeWindow*>(data)->OnLockTimeout();
    return FALSE;
  }
  void OnLockTimeout();

  // Call AddShutdownTimeout().
  static gboolean OnLockToShutdownTimeoutThunk(gpointer data) {
    reinterpret_cast<ChromeWindow*>(data)->OnLockToShutdownTimeout();
    return FALSE;
  }
  void OnLockToShutdownTimeout();

  // Tell the window manager that we're shutting down.
  static gboolean OnShutdownTimeoutThunk(gpointer data) {
    reinterpret_cast<ChromeWindow*>(data)->OnShutdownTimeout();
    return FALSE;
  }
  void OnShutdownTimeout();

  // Tell the window manager to display the pre-shutdown animation and add a
  // timeout for shutting down.
  void AddShutdownTimeout();

  // Overridden from Gtk::Window.
  virtual bool on_button_press_event(GdkEventButton* event);
  virtual bool on_button_release_event(GdkEventButton* event);
  virtual bool on_motion_notify_event(GdkEventMotion* event);
  virtual bool on_key_press_event(GdkEventKey* event);
  virtual bool on_key_release_event(GdkEventKey* event);
  virtual bool on_expose_event(GdkEventExpose* event);
  virtual bool on_client_event(GdkEventClient* event);
  virtual bool on_configure_event(GdkEventConfigure* event);
  virtual bool on_window_state_event(GdkEventWindowState* event);

  MockChrome* chrome_;  // not owned

  XWindow xid_;

  int width_;
  int height_;

  std::vector<std::tr1::shared_ptr<TabInfo> > tabs_;

  int active_tab_index_;

  // Is a tab currently being dragged?
  bool dragging_tab_;

  // Cursor's offset from the upper-left corner of the tab at the start of
  // the drag.
  int tab_drag_start_offset_x_;
  int tab_drag_start_offset_y_;

  // Is the window currently in fullscreen mode?
  bool fullscreen_;

  // Is the "power button" currently pressed?
  bool power_button_is_pressed_;

  // IDs of timeouts for running On*TimeoutThunk() methods, or 0 if unset.
  gint lock_timeout_id_;
  gint lock_to_shutdown_timeout_id_;
  gint shutdown_timeout_id_;

  // TODO: Rename these to e.g. kImageNavBg?
  static Glib::RefPtr<Gdk::Pixbuf> image_nav_bg_;
  static Glib::RefPtr<Gdk::Pixbuf> image_nav_left_;
  static Glib::RefPtr<Gdk::Pixbuf> image_nav_right_;
  static Glib::RefPtr<Gdk::Pixbuf> image_tab_bg_;
  static Glib::RefPtr<Gdk::Pixbuf> image_tab_hl_;
  static Glib::RefPtr<Gdk::Pixbuf> image_tab_nohl_;
  static Glib::RefPtr<Gdk::Pixbuf> image_tab_right_hl_left_nohl_;
  static Glib::RefPtr<Gdk::Pixbuf> image_tab_right_hl_left_none_;
  static Glib::RefPtr<Gdk::Pixbuf> image_tab_right_nohl_left_hl_;
  static Glib::RefPtr<Gdk::Pixbuf> image_tab_right_nohl_left_nohl_;
  static Glib::RefPtr<Gdk::Pixbuf> image_tab_right_nohl_left_none_;
  static Glib::RefPtr<Gdk::Pixbuf> image_tab_right_none_left_hl_;
  static Glib::RefPtr<Gdk::Pixbuf> image_tab_right_none_left_nohl_;

  // Height of the tab and navigation bars.
  static int kTabHeight;
  static int kNavHeight;

  // Distance above and below the tab bar that a tab can be dragged before
  // we detach it.
  static const int kTabDragThreshold;

  static const char* kTabFontFace;
  static const double kTabFontSize;
  static const int kTabFontPadding;

  // How long does the power button need to be held before we start locking
  // the screen or shutting down?
  static const int kLockTimeoutMs;
  static const int kShutdownTimeoutMs;

  // If the user holds the power button all the way through the lock and
  // shutdown sequences, how long of a delay should there be once the
  // screen is locked before we start displaying the pre-shutdown
  // animation?
  static const int kLockToShutdownThresholdMs;

  DISALLOW_COPY_AND_ASSIGN(ChromeWindow);
};

class PanelTitlebar : public Gtk::Window {
 public:
  explicit PanelTitlebar(Panel* panel);
  virtual ~PanelTitlebar() {}

  XWindow xid() const { return xid_; }
  void set_focused(bool focused) { focused_ = focused; }

  void Draw();

 private:
  static const int kWidth;
  static const int kHeight;
  static Glib::RefPtr<Gdk::Pixbuf> image_bg_;
  static Glib::RefPtr<Gdk::Pixbuf> image_bg_focused_;
  static const char* kFontFace;
  static const double kFontSize;
  static const double kFontPadding;
  static const int kDragThreshold;

  // Overridden from Gtk::Window.
  virtual bool on_expose_event(GdkEventExpose* event);
  virtual bool on_button_press_event(GdkEventButton* event);
  virtual bool on_button_release_event(GdkEventButton* event);
  virtual bool on_motion_notify_event(GdkEventMotion* event);

  Panel* panel_;  // not owned
  XWindow xid_;

  // Is the mouse button currently down?
  bool mouse_down_;

  // Pointer's absolute position when the mouse button was pressed.
  int mouse_down_abs_x_;
  int mouse_down_abs_y_;

  // Pointer's offset from the upper-right corner of the titlebar when the
  // mouse button was pressed.
  int mouse_down_offset_x_;
  int mouse_down_offset_y_;

  // Is the titlebar currently being dragged?  That is, has the cursor
  // moved more than kDragThreshold away from its starting position?
  bool dragging_;

  // Is this panel focused?  We draw ourselves differently if it is.
  bool focused_;

  DISALLOW_COPY_AND_ASSIGN(PanelTitlebar);
};

class Panel : public Gtk::Window {
 public:
  Panel(MockChrome* chrome,
        const std::string& image_filename,
        const std::string& title,
        bool expanded);
  virtual ~Panel() {}

  XWindow xid() const { return xid_; }
  MockChrome* chrome() { return chrome_; }
  bool expanded() const { return expanded_; }
  const std::string& title() const { return title_; }

 private:
  // Overridden from Gtk::Window.
  virtual bool on_expose_event(GdkEventExpose* event);
  virtual bool on_button_press_event(GdkEventButton* event);
  virtual bool on_key_press_event(GdkEventKey* event);
  virtual bool on_client_event(GdkEventClient* event);
  virtual bool on_focus_in_event(GdkEventFocus* event);
  virtual bool on_focus_out_event(GdkEventFocus* event);
  virtual bool on_window_state_event(GdkEventWindowState* event);

  MockChrome* chrome_;  // not owned
  XWindow xid_;
  scoped_ptr<PanelTitlebar> titlebar_;
  Glib::RefPtr<Gdk::Pixbuf> image_;
  int width_;
  int height_;
  bool expanded_;
  std::string title_;
  bool fullscreen_;

  DISALLOW_COPY_AND_ASSIGN(Panel);
};

// This mimics the screen locker window that Chrome maps when the screen has
// been locked.
class ScreenLockWindow : public Gtk::Window {
 public:
  explicit ScreenLockWindow(MockChrome* chrome);
  virtual ~ScreenLockWindow() {}

  XWindow xid() const { return xid_; }

 private:
  void Draw();

  // Overridden from Gtk::Window.
  virtual bool on_expose_event(GdkEventExpose* event);
  virtual bool on_configure_event(GdkEventConfigure* event);

  MockChrome* chrome_;  // not owned
  XWindow xid_;
  Glib::RefPtr<Gdk::Pixbuf> image_;

  DISALLOW_COPY_AND_ASSIGN(ScreenLockWindow);
};

class MockChrome {
 public:
  MockChrome();

  window_manager::WmIpc* wm_ipc() { return wm_ipc_.get(); }

  bool is_locked() const { return screen_lock_window_.get() != NULL; }
  bool is_shutting_down() const { return is_shutting_down_; }

  // Create a new window, ownership of which remains with the MockChrome
  // object.
  ChromeWindow* CreateWindow(int width, int height);

  // Close a window.
  void CloseWindow(ChromeWindow* win);

  // Create a new panel, ownership of which remains with the MockChrome
  // object.
  Panel* CreatePanel(const std::string& image_filename,
                     const std::string &title,
                     bool expanded);

  // Close a panel.
  void ClosePanel(Panel* panel);

  void LockScreen();
  void UnlockScreen();

  void ShutDown();

 private:
  scoped_ptr<window_manager::XConnection> xconn_;
  scoped_ptr<window_manager::AtomCache> atom_cache_;
  scoped_ptr<window_manager::WmIpc> wm_ipc_;

  typedef std::map<XWindow, std::tr1::shared_ptr<ChromeWindow> > ChromeWindows;
  ChromeWindows windows_;

  // Map from the panel window's XID to the corresponding Panel object.
  typedef std::map<XWindow, std::tr1::shared_ptr<Panel> > Panels;
  Panels panels_;

  scoped_ptr<ScreenLockWindow> screen_lock_window_;

  bool is_shutting_down_;

  DISALLOW_COPY_AND_ASSIGN(MockChrome);
};

}  // namespace mock_chrome

#endif  // WINDOW_MANAGER_MOCK_CHROME_H_
