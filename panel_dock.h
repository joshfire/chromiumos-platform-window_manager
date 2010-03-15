// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_PANEL_DOCK_H_
#define WINDOW_MANAGER_PANEL_DOCK_H_

#include <vector>
#include <tr1/memory>

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "window_manager/clutter_interface.h"
#include "window_manager/panel_container.h"
#include "window_manager/x_types.h"

namespace window_manager {

class EventConsumerRegistrar;
class Panel;
class PanelManager;
class Shadow;
class Window;
class WindowManager;

// Panel docks handle panels that are pinned to the left and right sides of
// the screen.
class PanelDock : public PanelContainer {
 public:
  // Distance between the panel and the edge of the screen at which we
  // detach it.
  static const int kDetachThresholdPixels;

  // Distance between the panel and the edge of the screen at which we
  // attach it.
  static const int kAttachThresholdPixels;

  enum DockType {
    DOCK_TYPE_LEFT = 0,
    DOCK_TYPE_RIGHT,
  };
  PanelDock(PanelManager* panel_manager, DockType type, int width);
  ~PanelDock();

  int width() const { return width_; }

  // Is the dock currently visible?
  bool IsVisible() const { return !panels_.empty(); }

  // Begin PanelContainer implementation.
  void GetInputWindows(std::vector<XWindow>* windows_out);
  void AddPanel(Panel* panel, PanelSource source);
  void RemovePanel(Panel* panel);
  bool ShouldAddDraggedPanel(const Panel* panel, int drag_x, int drag_y);
  void HandleInputWindowButtonPress(XWindow xid,
                                    int x, int y,
                                    int x_root, int y_root,
                                    int button,
                                    XTime timestamp) {}
  void HandleInputWindowButtonRelease(XWindow xid,
                                      int x, int y,
                                      int x_root, int y_root,
                                      int button,
                                      XTime timestamp) {}
  void HandleInputWindowPointerEnter(XWindow xid,
                                     int x, int y,
                                     int x_root, int y_root,
                                     XTime timestamp) {}
  void HandleInputWindowPointerLeave(XWindow xid,
                                     int x, int y,
                                     int x_root, int y_root,
                                     XTime timestamp) {}
  void HandlePanelButtonPress(Panel* panel, int button, XTime timestamp);
  void HandlePanelTitlebarPointerEnter(Panel* panel, XTime timestamp) {}
  void HandlePanelFocusChange(Panel* panel, bool focus_in);
  void HandleSetPanelStateMessage(Panel* panel, bool expand);
  bool HandleNotifyPanelDraggedMessage(Panel* panel, int drag_x, int drag_y);
  void HandleNotifyPanelDragCompleteMessage(Panel* panel);
  void HandleFocusPanelMessage(Panel* panel, XTime timestamp);
  void HandlePanelResize(Panel* panel);
  void HandleScreenResize();
  void HandlePanelUrgencyChange(Panel* panel) {}
  // End PanelContainer implementation.

 private:
  typedef std::vector<Panel*> Panels;

  // PanelDock-specific information about a panel.
  struct PanelInfo {
    PanelInfo() : snapped_y(0) {}

    // Y position where the panel's titlebar wants to be.  For panels that
    // are being dragged, this may be different from the actual composited
    // position -- we only snap the panels to this position when the drag
    // is complete.
    int snapped_y;
  };

  WindowManager* wm();

  // Get the PanelInfo object for a panel, crashing if it's not present.
  PanelInfo* GetPanelInfoOrDie(Panel* panel);

  // Update the position of 'fixed_panel' within 'panels_' based on its
  // current position.
  void ReorderPanel(Panel* fixed_panel);

  // Pack all panels except 'fixed_panel' to their snapped positions in the
  // dock, starting from the top.
  void PackPanels(Panel* fixed_panel);

  // Focus a panel.
  void FocusPanel(Panel* panel, bool remove_pointer_grab, XTime timestamp);

  PanelManager* panel_manager_;  // not owned

  DockType type_;

  // The dock's position and size.  Note that if the dock contains no
  // panels, it will hide to the side of its default position ('type_'
  // determines whether it'll hide to the left or right).
  int x_;
  int y_;
  int width_;
  int height_;

  // The total height of all panels in the dock.
  int total_panel_height_;

  // Panels, in top-to-bottom order.
  Panels panels_;

  // Information about our panels that doesn't belong in the Panel class
  // itself.
  std::map<Panel*, std::tr1::shared_ptr<PanelInfo> > panel_infos_;

  // The currently-dragged panel, or NULL if no panel in the dock is being
  // dragged.
  Panel* dragged_panel_;

  // The dock's background image and its drop shadow.
  scoped_ptr<ClutterInterface::Actor> bg_actor_;
  scoped_ptr<Shadow> bg_shadow_;

  // An input window at the same position as the dock.  Currently just used
  // to catch and discard input events so they don't fall through.
  XWindow bg_input_xid_;

  // PanelManager event registrations related to the dock's input windows.
  scoped_ptr<EventConsumerRegistrar> event_consumer_registrar_;

  DISALLOW_COPY_AND_ASSIGN(PanelDock);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_PANEL_DOCK_H_
