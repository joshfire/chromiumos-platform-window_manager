// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_LOGIN_LOGIN_ENTRY_H_
#define WINDOW_MANAGER_LOGIN_LOGIN_ENTRY_H_

#include <cstddef>

#include "base/logging.h"
#include "window_manager/geometry.h"
#include "window_manager/x11/x_types.h"

namespace window_manager {

class EventConsumerRegistrar;
class Window;
class WindowManager;

// All windows associated with a particular user are grouped in a LoginEntry.
class LoginEntry {
 public:
  LoginEntry(WindowManager* wm, EventConsumerRegistrar* registrar);
  ~LoginEntry();

  // Returns the index of the user the window belongs to or -1 if the window
  // does not have a parameter specifying the index.
  static size_t GetUserIndex(Window* win);

  Window* border_window() const { return border_window_; }
  Window* image_window() const { return image_window_; }
  Window* controls_window() const { return controls_window_; }
  Window* label_window() const { return label_window_; }
  Window* unselected_label_window() const { return unselected_label_window_; }

  // Have all the windows been assigned?
  bool has_all_windows() const {
    return border_window_ && image_window_ && controls_window_ &&
           label_window_ && unselected_label_window_;
  }

  // Are all the windows null?
  bool has_no_windows() const {
    return !border_window_ && !image_window_ && !controls_window_ &&
           !label_window_ && !unselected_label_window_;
  }

  // Do all the windows have pixmaps?
  bool HasAllPixmaps() const;

  int selected_width() const { return border_width_; }
  int selected_height() const { return border_height_; }
  int unselected_width() const { return unselected_border_width_; }
  int unselected_height() const { return unselected_border_height_; }
  int padding() const { return padding_; }

  // Get number of users.
  size_t GetUserCount() const;
  bool IsNewUser() const;

  // Set corresponding windows for the entry.
  void SetBorderWindow(Window* win);
  void SetImageWindow(Window* win);
  void SetControlsWindow(Window* win);
  void SetLabelWindow(Window* win);
  void SetUnselectedLabelWindow(Window* win);

  // Check unmapped window, return true if |win| belonged to the entry.
  bool HandleWindowUnmap(Window* win);

  // Move and scale composite windows to new origin.
  void UpdatePositionAndScale(const Point& origin, bool is_selected,
                              int anim_ms);

  // Fade in composite windows and put client windows onscreen for the entry.
  void FadeIn(const Point& origin, bool is_selected, int anim_ms);

  // Fade out composite windows and move client windows offscreen for the entry.
  void FadeOut(int anim_ms);

  // Do selection animation.
  void Select(const Point& origin, int anim_ms);

  // Do deselection animation.
  void Deselect(const Point& origin, int anim_ms);

  // Invoked when the selection change completes.
  void ProcessSelectionChangeCompleted(bool is_selected);

  // Stacks the windows. The stacking we care about is:
  // 1. the image_window is above the border_window;
  // 2. the controls_window is above the border window;
  // 3. the label_window is above the image_window.
  void StackWindows();

 private:
  // Caches size information. This is invoked when all the windows have been
  // created but not shown.
  void InitSizes();

  // Update scale for composite windows.
  void ScaleCompositeWindows(bool is_selected, int anim_ms);

  // Update positions of client windows.
  void UpdateClientWindows(const Point& origin, bool is_selected);

  WindowManager* wm_;
  EventConsumerRegistrar* registrar_;

  Window* border_window_;
  Window* image_window_;
  Window* controls_window_;
  Window* label_window_;
  Window* unselected_label_window_;

  bool sizes_initialized_;

  // Padding between the entries.
  int padding_;

  // Size of the border window.
  int border_width_;
  int border_height_;
  int controls_height_;
  int unselected_border_width_;
  int unselected_border_height_;

  // Gap between border and image.
  int border_to_image_gap_;
  int border_to_unselected_image_gap_;

  // Various scales.
  float unselected_border_scale_x_;
  float unselected_border_scale_y_;
  float unselected_image_scale_x_;
  float unselected_image_scale_y_;
  float unselected_label_scale_x_;
  float unselected_label_scale_y_;

  DISALLOW_COPY_AND_ASSIGN(LoginEntry);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_LOGIN_LOGIN_ENTRY_H_
