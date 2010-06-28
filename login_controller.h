// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_LOGIN_CONTROLLER_H_
#define WINDOW_MANAGER_LOGIN_CONTROLLER_H_

#include <set>
#include <vector>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "window_manager/event_consumer.h"
#include "window_manager/event_consumer_registrar.h"

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
  FRIEND_TEST(LoginControllerTest, SelectGuestWindowOldChrome);
  FRIEND_TEST(LoginControllerTest, SelectGuestWindowNewChrome);
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

  // All windows associated with a particular user are grouped in an Entry.
  //
  // NOTE: LoginController assumes all the windows for an entry are created,
  // then the windows in the next entry. For example, LoginController hits
  // an assert if chrome were to create two windows of type
  // WINDOW_TYPE_LOGIN_BORDER in a row.
  struct Entry {
    Entry()
        : border_window(NULL),
          image_window(NULL),
          controls_window(NULL),
          label_window(NULL),
          unselected_label_window(NULL) {
    }

    // Have all the windows been assigned?
    bool has_all_windows() const {
      return border_window && image_window && controls_window &&
          label_window && unselected_label_window;
    }

    // Are all the windows null?
    bool has_no_windows() const {
      return !border_window && !image_window && !controls_window &&
          !label_window && !unselected_label_window;
    }

    Window* border_window;
    Window* image_window;
    Window* controls_window;
    Window* label_window;
    Window* unselected_label_window;
  };

  typedef std::vector<Entry> Entries;

  // Copies login_xids_ and non_login_xids_ into the passed-in set.
  void get_all_xids(std::set<XWindow>* xids_out) {
    DCHECK(xids_out);
    xids_out->clear();
    xids_out->insert(login_xids_.begin(), login_xids_.end());
    xids_out->insert(non_login_xids_.begin(), non_login_xids_.end());
  }

  // Caches size information. This is invoked when all the windows have been
  // created but not shown.
  void InitSizes(int unselected_image_size, int padding);

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

  // Returns the origin for |view_count| entries with the entry at
  // |selected_index| selected.
  void CalculateIdealOrigins(size_t view_count,
                             size_t selected_index,
                             std::vector<Point>* bounds);

  // Returns the bounds for the various windows given an origin at |origin|.
  void CalculateEntryBounds(const Point& origin,
                            bool selected,
                            Rect* border_bounds,
                            Rect* image_bounds,
                            Rect* controls_bounds,
                            Rect* label_bounds);

  // Used by both InitialShow and SelectEntryAt to scale the windows of an
  // unselected entry.
  void ScaleUnselectedEntry(const Entry& entry,
                            const Rect& border_bounds,
                            const Rect& label_bounds,
                            bool initial);

  // Returns true if |window| is a a login window.
  bool IsLoginWindow(Window* window) const;

  // Returns true if |index| is the index of the guest login window.
  bool IsGuestEntryIndex(size_t index) const;

  // Returns the entry for the specified win. This returns an entry based on the
  // index stored in the window's parameters.
  Entry* GetEntryForWindow(Window* win);

  // Returns the entry in |entries_| at the specified index, creating one if
  // necessary.
  Entry* GetEntryAt(int index);

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
  void FocusLoginWindow(Window* win, XTime timestamp);

  // Hide all of our windows and give up the focus if we have it.
  // Invoked after we see the initial non-login Chrome window get mapped.
  void HideWindowsAfterLogin();

  // Return true if old version of Chrome is detected and we should preserve
  // old behavior for backward compatibility. has_all_windows_ must be true
  // before calling this method.
  bool IsOldChrome();

  // Moves client window of the image window on screen to the right position
  // depending on whether the entry is selected or not.
  // If it is, client window matches the corresponding composited window.
  // Otherwise, its origin matches composited window of entry's border and
  // input area is cut to cover the border and the space between the border
  // and the label.
  void MoveImageClientWindow(bool selected,
                             const Rect& border_bounds,
                             const Rect& label_bounds,
                             Window* image_window);

  WindowManager* wm_;

  EventConsumerRegistrar registrar_;

  // The set of login windows we know about. This is all the windows in
  // |entries_| along with the guest window and background window.
  std::set<XWindow> login_xids_;

  // Other, non-login-specific windows that we're managing when Chrome is
  // in a not-logged-in state.
  std::set<XWindow> non_login_xids_;

  Entries entries_;

  // Have the sizes been calculated yet?
  bool inited_sizes_;

  // Did we get all the windows and show them?
  bool has_all_windows_;

  // Padding between the entries.
  int padding_;

  // Size of the border window.
  int border_width_;
  int border_height_;
  int unselected_border_width_;
  int unselected_border_height_;

  // Gap between border and image.
  int border_to_controls_gap_;

  // Height of the controls window.
  int controls_height_;

  // Size of the label window.
  int label_height_;
  int unselected_label_height_;

  // Various scales.
  float unselected_border_scale_x_;
  float unselected_border_scale_y_;
  float unselected_image_scale_x_;
  float unselected_image_scale_y_;
  float unselected_label_scale_x_;
  float unselected_label_scale_y_;

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
