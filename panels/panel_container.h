// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_PANELS_PANEL_CONTAINER_H_
#define WINDOW_MANAGER_PANELS_PANEL_CONTAINER_H_

#include <vector>

#include "base/basictypes.h"
#include "window_manager/x11/x_types.h"

namespace window_manager {

class Panel;

// Interface for containers that can hold panels.
class PanelContainer {
 public:
  PanelContainer() {}
  virtual ~PanelContainer() {}

  // Fill the passed-in vector with all of this container's input windows
  // (in an arbitrary order).  Input windows belonging to contained panels
  // should not be included.
  //
  // Note that this is only called once, right after the container is
  // constructed.  In other words, containers must create all input windows
  // that they will need in their constructors.
  virtual void GetInputWindows(std::vector<XWindow>* windows_out) = 0;

  // Where did this panel come from?  Determines how it's animated when
  // being added.
  enum PanelSource {
    // Newly-opened panel.
    PANEL_SOURCE_NEW = 0,

    // Panel was attached to this container by being dragged into it, and
    // is still being dragged.
    PANEL_SOURCE_DRAGGED,

    // Panel is being attached to this panel after being dropped.
    PANEL_SOURCE_DROPPED,
  };

  // Add a panel to this container.  Ownership of the object's memory
  // remains with the caller.
  virtual void AddPanel(Panel* panel, PanelSource source) = 0;

  // Remove a panel from this container.  Ownership remains with the
  // caller.  Note that this may be a panel that's currently being dragged.
  virtual void RemovePanel(Panel* panel) = 0;

  // Is the passed-in panel (which isn't currently in any container) being
  // dragged to a position such that it should be added to this container?
  virtual bool ShouldAddDraggedPanel(const Panel* panel,
                                     const Point& drag_pos) = 0;

  // Handle pointer events occurring in the container's input windows.
  virtual void HandleInputWindowButtonPress(XWindow xid,
                                            const Point& relative_pos,
                                            const Point& absolute_pos,
                                            int button,
                                            XTime timestamp) = 0;
  virtual void HandleInputWindowButtonRelease(XWindow xid,
                                              const Point& relative_pos,
                                              const Point& absolute_pos,
                                              int button,
                                              XTime timestamp) = 0;
  virtual void HandleInputWindowPointerEnter(XWindow xid,
                                             const Point& relative_pos,
                                             const Point& absolute_pos,
                                             XTime timestamp) = 0;
  virtual void HandleInputWindowPointerLeave(XWindow xid,
                                             const Point& relative_pos,
                                             const Point& absolute_pos,
                                             XTime timestamp) = 0;

  // Handle a button press or pointer enter in a panel.
  virtual void HandlePanelButtonPress(Panel* panel,
                                      int button,
                                      XTime timestamp) = 0;
  virtual void HandlePanelTitlebarPointerEnter(Panel* panel,
                                               XTime timestamp) = 0;

  // Handle a message asking us to expand or collapse one of our panels.
  virtual void HandleSetPanelStateMessage(Panel* panel, bool expand) = 0;

  // Handle a message from Chrome telling us that a panel has been dragged
  // to a particular location.  If false is returned, it indicates that the
  // panel should be removed from this container (i.e. it's been dragged
  // too far away) -- the container's RemovePanel() method will be invoked
  // to accomplish this.
  virtual bool HandleNotifyPanelDraggedMessage(Panel* panel,
                                               const Point& drag_pos) = 0;

  // Handle a message from Chrome telling us that a panel drag is complete.
  virtual void HandleNotifyPanelDragCompleteMessage(Panel* panel) = 0;

  // Handle a message asking us to focus one of our panels.
  virtual void HandleFocusPanelMessage(Panel* panel, XTime timestamp) = 0;

  // Handle a ConfigureRequest event that asks for a panel's content window
  // to be resized.
  virtual void HandlePanelResizeRequest(Panel* panel,
                                        const Size& requested_size) = 0;

  // Handle the user resizing the panel by dragging one of its resize
  // borders.  This method is invoked at the end of the resize.
  virtual void HandlePanelResizeByUser(Panel* panel) = 0;

  // Handle the screen being resized.
  virtual void HandleScreenResize() = 0;

  // Handle a change to a panel's urgency hint.
  virtual void HandlePanelUrgencyChange(Panel* panel) = 0;

  // Take the input focus if possible.  Returns false if it doesn't make
  // sense to take the focus (suppose there are no panels, or only
  // collapsed panels).
  virtual bool TakeFocus(XTime timestamp) = 0;

  DISALLOW_COPY_AND_ASSIGN(PanelContainer);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_PANELS_PANEL_CONTAINER_H_
