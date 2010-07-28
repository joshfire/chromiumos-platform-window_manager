// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_FOCUS_MANAGER_H_
#define WINDOW_MANAGER_FOCUS_MANAGER_H_

#include <set>

#include "base/basictypes.h"
#include "base/logging.h"
#include "window_manager/x_types.h"

namespace window_manager {

class Window;
class WindowManager;

// Interface for classes that need to be notified when the focused window
// changes.
class FocusChangeListener {
  public:
   virtual void HandleFocusChange() = 0;

  protected:
   ~FocusChangeListener() {}
};

// This class is used to assign the input focus to windows.
class FocusManager {
 public:
  explicit FocusManager(WindowManager* wm);
  ~FocusManager();

  Window* focused_win() { return focused_win_; }

  // Assign the input focus to a window and update the _NET_ACTIVE_WINDOW
  // property.  If 'win' is NULL, the focus will be assigned to the root
  // window instead.  'timestamp' should be the time from the event that
  // triggered the focus change.  If no such time is available, a timestamp
  // can be obtained from WindowManager::GetCurrentTimeFromServer().
  void FocusWindow(Window* win, XTime timestamp);

  // Use click-to-focus for a window.  We install a button grab on the
  // window so that we'll be notified if it gets clicked.  The caller
  // remains responsible for seeing the button press later and deciding to
  // focus the window by calling FocusWindow(); we just handle adding and
  // removing the button grab as needed when the window loses or gains the
  // focus.  This is reset when the window gets unmapped.
  void UseClickToFocusForWindow(Window* win);

  // Handle a window being unmapped.  Called by WindowManager.
  void HandleWindowUnmap(Window* win);

  // Handle a button press in a window.  Called by WindowManager.
  // If this was a window that was using click-to-focus, then its button
  // grab has been upgraded to a pointer grab.  We ungrab the pointer and
  // (if the currently-focused window isn't modal) replay the click so that
  // 'win' will receive it.
  void HandleButtonPressInWindow(Window* win, XTime timestamp);

  // Register or unregister a listener that will be notified after a focus
  // change.
  void RegisterFocusChangeListener(FocusChangeListener* listener);
  void UnregisterFocusChangeListener(FocusChangeListener* listener);

 private:
  WindowManager* wm_;  // not owned

  // The currently-focused window, or NULL if no window is focused.
  Window* focused_win_;  // not owned

  // Windows using click-to-focus.
  std::set<Window*> click_to_focus_windows_;  // not owned

  // Listeners that will be notified when the focus changes.
  std::set<FocusChangeListener*> focus_change_listeners_;  // not owned

  // The last timestamp that was used in a call to FocusWindow().
  // Initially 0.
  XTime last_focus_timestamp_;

  DISALLOW_COPY_AND_ASSIGN(FocusManager);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_FOCUS_MANAGER_H_
