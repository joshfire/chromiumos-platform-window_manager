// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_LOGIN_CONTROLLER_H_
#define WINDOW_MANAGER_LOGIN_CONTROLLER_H_

#include <set>
#include <tr1/memory>
#include <vector>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "window_manager/event_consumer.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/login_entry.h"

namespace window_manager {

struct Point;
struct Rect;
class WindowManager;

// LoginController is an EventConsumer responsible for positioning the windows
// used during login. LoginController collects all the windows of type
// WINDOW_TYPE_LOGIN_XXX and adds them to Entrys. When LoginController sees a
// message of type WM_SHOW_LOGIN all the windows are shown.
class LoginController : public EventConsumer {
 public:
  explicit LoginController(WindowManager* wm);
  ~LoginController();

  // Begin EventConsumer implementation.
  virtual bool IsInputWindow(XWindow xid);
  virtual void HandleScreenResize();
  virtual void HandleLoggedInStateChange();
  virtual bool HandleWindowMapRequest(Window* win);
  virtual void HandleWindowMap(Window* win);
  virtual void HandleWindowUnmap(Window* win);
  virtual void HandleWindowConfigureRequest(Window* win,
                                            int req_x, int req_y,
                                            int req_width, int req_height);
  virtual void HandleButtonPress(XWindow xid,
                                 int x, int y,
                                 int x_root, int y_root,
                                 int button,
                                 XTime timestamp);
  virtual void HandleButtonRelease(XWindow xid,
                                   int x, int y,
                                   int x_root, int y_root,
                                   int button,
                                   XTime timestamp) {}
  virtual void HandlePointerEnter(XWindow xid,
                                  int x, int y,
                                  int x_root, int y_root,
                                  XTime timestamp) {}
  virtual void HandlePointerLeave(XWindow xid,
                                  int x, int y,
                                  int x_root, int y_root,
                                  XTime timestamp) {}
  virtual void HandlePointerMotion(XWindow xid,
                                   int x, int y,
                                   int x_root, int y_root,
                                   XTime timestamp) {}
  virtual void HandleChromeMessage(const WmIpc::Message& msg);
  virtual void HandleClientMessage(XWindow xid,
                                   XAtom message_type,
                                   const long data[5]);
  virtual void HandleWindowPropertyChange(XWindow xid, XAtom xatom);
  // End EventConsumer implementation.

 private:
  friend class LoginControllerTest;  // runs InitialShow() manually
  FRIEND_TEST(LoginControllerTest, Focus);
  FRIEND_TEST(LoginControllerTest, KeyBindingsDuringStateChange);
  FRIEND_TEST(LoginControllerTest, SelectGuest);
  FRIEND_TEST(LoginControllerTest, RemoveUser);
  FRIEND_TEST(LoginControllerTest, ClientOnOffScreen);

  // SelectionChangedManager is used to cleanup after the selection changes.
  // When the selection changes |Schedule| is invoked on the
  // SelectionChangedManager. SelectionChangedManager then invokes
  // ProcessSelectionChangeCompleted back on the LoginController after a delay
  // to do cleanup.
  class SelectionChangedManager {
   public:
    explicit SelectionChangedManager(LoginController* layout);
    ~SelectionChangedManager();

    // Schedules a selection change for the specified index. If the selection
    // has changed but not been committed (Run has not been invoked yet), it is
    // committed.
    void Schedule(size_t selected_index);

    // Stops any pending runs.
    void Stop();

    bool is_scheduled() const { return timeout_id_ != -1; }
    int selected_index() const { return selected_index_; }

   private:
    // Callback when the timer fires. Notifies the LoginController.
    void Run();

    LoginController* layout_;

    // If not kNoTimer indicates there is a pending run.
    int timeout_id_;

    // Last index passed to Schedule.
    size_t selected_index_;

    DISALLOW_COPY_AND_ASSIGN(SelectionChangedManager);
  };

  typedef std::vector<std::tr1::shared_ptr<LoginEntry> > Entries;

  // Copies login_xids_ and non_login_xids_ into the passed-in set.
  void get_all_xids(std::set<XWindow>* xids_out) {
    DCHECK(xids_out);
    xids_out->clear();
    xids_out->insert(login_xids_.begin(), login_xids_.end());
    xids_out->insert(non_login_xids_.begin(), non_login_xids_.end());
  }

  // Invoked to handle the initial show.
  void InitialShow();

  // Set up the background window's position and visibility.
  void ConfigureBackgroundWindow();

  // Stacks the windows. The only stacking we care about is that the
  // image_window is above the border_window and the controls_window is above
  // the border window.
  void StackWindows();

  // Selects the entry at the specified index. Does nothing if index is already
  // selected. This invokes SelectGuest if index corresponds to the guest.
  void SelectEntryAt(size_t index);

  // Selects the guest entry.
  void SelectGuest();

  // Hides all the windows (except for the guest).
  void Hide();

  // Sets whether the user can select other entries.
  void SetEntrySelectionEnabled(bool enable);

  // Calculate and returns the origin for entries.
  void CalculateIdealOrigins(std::vector<Point>* bounds);

  // Returns true if |window| is a a login window.
  bool IsLoginWindow(Window* window) const;

  // Returns true if |index| is the index of the guest login window.
  bool IsGuestEntryIndex(size_t index) const;

  // Returns the entry for the specified win. This returns an entry based on the
  // index stored in the window's parameters.
  LoginEntry* GetEntryForWindow(Window* win);

  // Returns the entry in |entries_| at the specified index, creating one if
  // necessary.
  LoginEntry* GetEntryAt(size_t index);

  // Invoked when the selection change completes. |last_selected_index| is the
  // index of the selection before the selection changes.
  void ProcessSelectionChangeCompleted(size_t last_selected_index);

  // Have we gotten all the windows we need?
  bool HasAllWindows();

  // Invoked when a new window is mapped, or a property changes on the
  // background window. This may do one of the following:
  // . If we just got all the windows, this stacks the windows and starts the
  //   initial animation.
  // . If the background and guest windows are ready, they are shown.
  void OnGotNewWindowOrPropertyChange();

  // Returns true if the background window is valid and has painted.
  bool IsBackgroundWindowReady();

  // Focus a window and save it to login_window_to_focus_.
  void FocusLoginWindow(Window* win);

  // Hide all of our windows and give up the focus if we have it.
  // Invoked after we see the initial non-login Chrome window get mapped.
  void HideWindowsAfterLogin();

  WindowManager* wm_;

  EventConsumerRegistrar registrar_;

  // The set of login windows we know about. This is all the windows in
  // |entries_| along with the guest window and background window.
  std::set<XWindow> login_xids_;

  // Other, non-login-specific windows that we're managing when Chrome is
  // in a not-logged-in state.
  std::set<XWindow> non_login_xids_;

  Entries entries_;

  // Did we get all the windows and show them?
  bool has_all_windows_;

  // Are we waiting for the guest to load? This is set to true if the user
  // clicks the guest entry and the guest_window_ has not been loaded.
  bool waiting_for_guest_;

  // Index of the selected entry.
  size_t selected_entry_index_;

  // Used when the selection changes.
  SelectionChangedManager selection_changed_manager_;

  // The guest window.
  Window* guest_window_;

  // Window placed in the background.
  Window* background_window_;

  // The controls or guest window that we've most recently focused.  We
  // track this so that if a transient window takes the focus and then gets
  // closed, we can re-focus the window that had the focus before.
  Window* login_window_to_focus_;

  // Are we waiting for the initial post-login Chrome window to get mapped
  // so we can hide the login windows?
  bool waiting_to_hide_windows_;

  // Determines if entry selection is enabled at the moment.
  bool is_entry_selection_enabled_;

  DISALLOW_COPY_AND_ASSIGN(LoginController);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_LOGIN_CONTROLLER_H_
