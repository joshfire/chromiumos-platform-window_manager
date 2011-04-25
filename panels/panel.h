// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_PANELS_PANEL_H_
#define WINDOW_MANAGER_PANELS_PANEL_H_

#include <string>
#include <vector>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "window_manager/geometry.h"
#include "window_manager/motion_event_coalescer.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/window.h"
#include "window_manager/x11/x_types.h"

namespace window_manager {

class EventConsumerRegistrar;
class PanelManager;
class ResizeBox;
class TransientWindowCollection;
class WindowManager;
class XConnection;

// A panel, representing a pop-up window.  Each panel consists of a content
// window (the panel's contents) and a titlebar window (a small window
// drawn in the bar when the panel is collapsed or at the top of the panel
// when it's expanded).  The right edges of the titlebar and content
// windows are aligned.
class Panel {
 public:
  // The panel's windows will remain untouched until Move() is invoked.
  // (PanelManager would have previously moved the client windows offscreen
  // in response to their map requests, and Window's c'tor makes composited
  // windows invisible.)
  Panel(PanelManager* panel_manager,
        Window* content_win,
        Window* titlebar_win,
        bool is_expanded);
  ~Panel();

  bool is_expanded() const { return is_expanded_; }
  bool is_fullscreen() const { return is_fullscreen_; }

  bool is_urgent() const { return is_urgent_; }
  // Called by PanelManager when the content window's urgency hint changes.
  void set_is_urgent(bool urgent) { is_urgent_ = urgent; }

  const Window* const_content_win() const { return content_win_; }
  Window* content_win() { return content_win_; }
  Window* titlebar_win() { return titlebar_win_; }

  XWindow content_xid() const { return content_win_->xid(); }
  XWindow titlebar_xid() const { return titlebar_win_->xid(); }

  // Get the X ID of the content window.  This is handy for logging.
  const std::string& xid_str() const { return content_win_->xid_str(); }

  // The current position of one pixel beyond the right edge of the panel.
  int right() const { return content_x() + content_width(); }

  // The current left edge of the content or titlebar window (that is, its
  // composited position).
  int content_x() const { return content_bounds_.x; }
  int titlebar_x() const { return titlebar_bounds_.x; }
  int content_center() const { return content_x() + 0.5 * content_width(); }

  int titlebar_y() const { return titlebar_bounds_.y; }
  int content_y() const { return content_bounds_.y; }

  // TODO: Remove content and titlebar width.
  int content_width() const { return content_bounds_.width; }
  int titlebar_width() const { return titlebar_bounds_.width; }
  int width() const { return content_bounds_.width; }

  int content_height() const { return content_bounds_.height; }
  int titlebar_height() const { return titlebar_bounds_.height; }
  int total_height() const { return content_height() + titlebar_height(); }

  bool is_focused() const { return content_win_->IsFocused(); }

  // Is the user currently dragging one of the resize handles?
  bool is_being_resized_by_user() const { return resize_drag_xid_ != 0; }

  // Fill the passed-in vector with all of the panel's input windows (in an
  // arbitrary order).
  void GetInputWindows(std::vector<XWindow>* windows_out);

  // Handle events occurring in one of our input windows.
  void HandleInputWindowButtonPress(
      XWindow xid, int x, int y, int button, XTime timestamp);
  void HandleInputWindowButtonRelease(
      XWindow xid, int x, int y, int button, XTime timestamp);
  void HandleInputWindowPointerMotion(XWindow xid, int x, int y);

  // Move the panel.  |right| is given in terms of one pixel beyond
  // the panel's right edge (since content and titlebar windows share a
  // common right edge), while |y| is the top of the titlebar window.
  // For example, to place the left column of a 10-pixel-wide panel at
  // X-coordinate 0 and the right column at 9, pass 10 for |right|.
  //
  // Note: Move() must be called initially to configure the windows (see
  // the constructor's comment).
  void Move(int right, int y, int anim_ms);
  void MoveX(int right, int anim_ms);
  void MoveY(int y, int anim_ms);

  // Set the titlebar window's width (while keeping it right-aligned with
  // the content window).
  void SetTitlebarWidth(int width);

  // Set the opacity of the titlebar and content windows' drop shadows.
  void SetShadowOpacity(double opacity, int anim_ms);

  // Set whether the panel should be resizable by dragging its borders.
  void SetResizable(bool resizable);

  // Stack the panel's client and composited windows at the top of the
  // passed-in layer.  Input windows are included.
  void StackAtTopOfLayer(StackingManager::Layer layer);

  // Update |is_expanded_|.  If it has changed, also notify Chrome about the
  // panel's current visibility state and update the content window's
  // _CHROME_STATE property.  Returns false if notifying Chrome fails (but
  // still updates the local variable).
  bool SetExpandedState(bool expanded);

  // Give the focus to the content window.
  void TakeFocus(XTime timestamp);

  // Resize the content window to the passed-in dimensions.  The titlebar
  // window is moved above the content window if necessary and resized to
  // match the content window's width.  The input windows are optionally
  // configured.
  void ResizeContent(int width, int height,
                     Gravity gravity,
                     bool configure_input_windows);

  // Make the panel be fullscreen or not fullscreen.  When entering
  // fullscreen mode, we restack the content window and configure it to
  // cover the whole screen.  Any changes to the panel's position or
  // stacking while it's fullscreened are saved to |content_bounds_|,
  // |titlebar_bounds_|, and |stacking_layer_|, but are otherwise deferred
  // until the panel gets unfullscreened.
  void SetFullscreenState(bool fullscreen);

  // Handle the screen being resized.  Most of the time any changes that
  // need to be made to the panel's position will be handled by its
  // container, but this gives fullscreen panels a change to resize
  // themselves to match the new screen size.
  void HandleScreenResize();

  // Handle an update to the content window's WM_NORMAL_HINTS property.
  // We call UpdateContentWindowSizeLimits() but don't resize the content
  // window.
  void HandleContentWindowSizeHintsChange();

  // Handle the start or end of a drag of this panel to a new position.
  // While the panel is being dragged, it avoids updating the position of its
  // underlying X windows in response to calls to Move() in order to reduce
  // unnecessary communication with the X server.  When the drag ends, the
  // windows are moved to the proper locations.
  void HandleDragStart();
  void HandleDragEnd();

  // Handle events referring to one of this panel's transient windows.
  void HandleTransientWindowMap(Window* win);
  void HandleTransientWindowUnmap(Window* win);
  void HandleTransientWindowButtonPress(
      Window* win, int button, XTime timestamp);
  void HandleTransientWindowClientMessage(
      Window* win, XAtom message_type, const long data[5]);
  void HandleTransientWindowConfigureRequest(
      Window* win, int req_x, int req_y, int req_width, int req_height);

 private:
  FRIEND_TEST(PanelBarTest, PackPanelsAfterPanelResize);
  FRIEND_TEST(PanelBarTest, FloatingPanelPositionAfterResize);
  FRIEND_TEST(PanelManagerTest, ChromeInitiatedPanelResize);
  FRIEND_TEST(PanelTest, InputWindows);
  FRIEND_TEST(PanelTest, Resize);
  FRIEND_TEST(PanelTest, SizeLimits);
  FRIEND_TEST(PanelTest, ResizeParameter);
  FRIEND_TEST(PanelTest, SeparatorShadow);

  WindowManager* wm();

  // Can we configure |client_win_| and |titlebar_win_| right now?  If not,
  // we only store changes to their size, position, and stacking in
  // |client_bounds_|, |titlebar_bounds_|, and |stacking_layer_|.
  bool can_configure_windows() const {
    return !is_fullscreen_;
  }

  // Move and resize the input windows appropriately for the panel's
  // current configuration.
  void ConfigureInputWindows();

  // Stack the input windows directly below the content window.
  void StackInputWindows();

  // Called periodically by |resize_event_coalescer_|.
  void ApplyResize();

  // Send a CHROME_NOTIFY_PANEL_STATE message to the content window
  // describing the panel's current expanded/collapsed state.
  bool SendStateMessageToChrome();

  // Update the content window's _CHROME_STATE property according to the
  // current value of |is_expanded_|.
  bool UpdateChromeStateProperty();

  // Update {min,max}_content_{width,height}_ based on the content window's
  // current size hints.
  void UpdateContentWindowSizeLimits();

  PanelManager* panel_manager_;  // not owned
  Window* content_win_;          // not owned
  Window* titlebar_win_;         // not owned

  // Is the panel currently expanded?  The Panel class does little itself
  // with this information; most work is left to PanelContainer
  // implementations.
  bool is_expanded_;

  // Is the content window currently fullscreen?
  bool is_fullscreen_;

  // Is the content window's urgency hint set?
  // (We track this in a separate variable instead of just reaching into
  // |content_win_| to make it easier to tell when the hint changes.)
  bool is_urgent_;

  // Saved position and size of the content and titlebar windows.  Note
  // that these may differ from the actual current configuration of these
  // windows (e.g. the content window may be fullscreened).
  Rect content_bounds_;
  Rect titlebar_bounds_;

  // Stacking layer at which the panel should be stacked.  We use this to
  // restore the panel's stacking once it exits fullscreen mode.
  StackingManager::Layer stacking_layer_;

  // Translucent resize box used when opaque resizing is disabled.
  scoped_ptr<ResizeBox> resize_box_;

  // Batches motion events for resized panels so that we can rate-limit the
  // frequency of their processing.
  MotionEventCoalescer resize_event_coalescer_;

  // Width of the invisible border drawn around a window for use in resizing,
  // in pixels.
  static const int kResizeBorderWidth;

  // Size in pixels of the corner parts of the resize border.
  //
  //       C              W is kResizeBorderWidth
  //   +-------+----      C is kResizeCornerSize
  //   |       | W
  // C |   +---+----
  //   |   |
  //   +---+  titlebar window
  //   | W |
  static const int kResizeCornerSize;

  // Minimum and maximum dimensions to which the content window can be
  // resized.
  int min_content_width_;
  int min_content_height_;
  int max_content_width_;
  int max_content_height_;

  // Used to catch clicks for resizing.
  XWindow top_input_xid_;
  XWindow top_left_input_xid_;
  XWindow top_right_input_xid_;
  XWindow left_input_xid_;
  XWindow right_input_xid_;

  // Should we configure handles around the panel that can be dragged to
  // resize it?  This is something that can be turned on and off by
  // containers.
  bool resizable_;

  // Does Chrome want the user to be able to resize the panel horizontally
  // or vertically?  These are harder limits than |resizable_|; Chrome can
  // use these to entirely disallow user-initiated resizing for a panel
  // even if |resizable_| is true (but note that these have no effect when
  // |resizable_| is false).
  bool horizontal_resize_allowed_;
  bool vertical_resize_allowed_;

  // Have the composited windows been scaled and shown?  We defer doing
  // this until the first time that Move() is called.
  bool composited_windows_set_up_;

  // Are we currently being dragged to a new position?
  // See HandleDragStart() and HandleDragEnd().
  bool being_dragged_to_new_position_;

  // XID of the input window currently being dragged to resize the panel,
  // or 0 if no drag is in progress.
  XWindow resize_drag_xid_;

  // Gravity holding a corner in place as the panel is being resized (e.g.
  // GRAVITY_SOUTHEAST if |top_left_input_xid_| is being dragged).
  Gravity resize_drag_gravity_;

  // Pointer coordinates where the resize drag started.
  int resize_drag_start_x_;
  int resize_drag_start_y_;

  // Initial content window size at the start of the resize.
  int resize_drag_orig_width_;
  int resize_drag_orig_height_;

  // Most-recent content window size during a resize.
  int resize_drag_last_width_;
  int resize_drag_last_height_;

  // PanelManager event registrations related to this panel's windows.
  scoped_ptr<EventConsumerRegistrar> event_consumer_registrar_;

  // Transient windows owned by this panel.
  scoped_ptr<TransientWindowCollection> transients_;

  // Shadow that we draw directly on top of the content window, aligned
  // with its top edge, to simulate the titlebar casting a shadow on it.
  scoped_ptr<Shadow> separator_shadow_;

  DISALLOW_COPY_AND_ASSIGN(Panel);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_PANELS_PANEL_H_
