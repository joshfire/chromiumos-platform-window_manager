// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_LOGIN_LOGIN_CONTROLLER_H_
#define WINDOW_MANAGER_LOGIN_LOGIN_CONTROLLER_H_

#include <set>
#include <tr1/memory>
#include <vector>

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "window_manager/event_consumer.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/login/login_entry.h"

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
  virtual ~LoginController();

  // Begin EventConsumer implementation.
  virtual bool IsInputWindow(XWindow xid);
  virtual void HandleScreenResize();
  virtual void HandleLoggedInStateChange();
  virtual bool HandleWindowMapRequest(Window* win);
  virtual void HandleWindowMap(Window* win);
  virtual void HandleWindowUnmap(Window* win);
  virtual void HandleWindowPixmapFetch(Window* win);
  virtual void HandleWindowConfigureRequest(Window* win,
                                            const Rect& requested_bounds);
  virtual void HandleButtonPress(XWindow xid,
                                 const Point& relative_pos,
                                 const Point& absolute_pos,
                                 int button,
                                 XTime timestamp);
  virtual void HandleButtonRelease(XWindow xid,
                                   const Point& relative_pos,
                                   const Point& absolute_pos,
                                   int button,
                                   XTime timestamp) {}
  virtual void HandlePointerEnter(XWindow xid,
                                  const Point& relative_pos,
                                  const Point& absolute_pos,
                                  XTime timestamp) {}
  virtual void HandlePointerLeave(XWindow xid,
                                  const Point& relative_pos,
                                  const Point& absolute_pos,
                                  XTime timestamp);
  virtual void HandlePointerMotion(XWindow xid,
                                   const Point& relative_pos,
                                   const Point& absolute_pos,
                                   XTime timestamp);
  virtual void HandleChromeMessage(const WmIpc::Message& msg);
  virtual void HandleClientMessage(XWindow xid,
                                   XAtom message_type,
                                   const long data[5]);
  virtual void HandleWindowPropertyChange(XWindow xid, XAtom xatom) {}
  virtual void OwnDestroyedWindow(DestroyedWindow* destroyed_win, XWindow xid);
  // End EventConsumer implementation.

 private:
  friend class LoginControllerTest;  // runs InitialShow() manually
  FRIEND_TEST(LoginControllerTest, AreViewsWindowsReady);
  FRIEND_TEST(LoginControllerTest, ClientOnOffScreen);
  FRIEND_TEST(LoginControllerTest, Focus);
  FRIEND_TEST(LoginControllerTest, KeyBindingsDuringStateChange);
  FRIEND_TEST(LoginControllerTest, LoginEntryRelativePositions);
  FRIEND_TEST(LoginControllerTest, RemoveUser);
  FRIEND_TEST(LoginControllerTest, SelectGuest);
  FRIEND_TEST(LoginControllerTest, SelectTwice);
  FRIEND_TEST(LoginControllerTest, ShowEntriesAfterTheyGetPixmaps);
  FRIEND_TEST(LoginControllerTest, UnhideCursorOnBrowserWindowVisible);
  FRIEND_TEST(LoginControllerTest, UnhideCursorOnLeave);
  FRIEND_TEST(LoginControllerTest, HandleWindowMapRequestsWebUILoginWindow);

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

  // Selects the wizard window.
  void SelectWizardWindow();

  // Sets whether the user can select other entries.
  void SetEntrySelectionEnabled(bool enable);

  // Calculate and returns the origin for entries.
  void CalculateIdealOrigins(std::vector<Point>* bounds);

  // Returns true if |window| is a a login window.
  bool IsLoginWindow(Window* window) const;

  // Returns true if |index| is the index of the guest login window.
  bool IsGuestEntryIndex(size_t index) const;

  // Returns the entry for the specified |win| or NULL if |win| doesn't belong
  // to any entry. This returns an entry based on the index stored in the
  // window's parameters. If |possibly_insert| is true and Chrome is attempting
  // to add a new entry, the function will create a new LoginEntry object.
  LoginEntry* GetEntryForWindow(Window* win, bool possibly_insert);

  // Returns the entry in |entries_| at the specified index, creating one if
  // necessary.
  LoginEntry* GetEntryAt(size_t index);

  // Invoked when the selection change completes. |last_selected_index| is the
  // index of the selection before the selection changes.
  void ProcessSelectionChangeCompleted(size_t last_selected_index);

  // Have we gotten all the windows we need and are they ready?
  bool AreViewsWindowsReady();

  // Does initial setup for windows if they have already gotten pixmaps.
  // Invoked when some window gets its pixmap. This may do one of the following:
  // - If the entry windows are ready, this stacks the windows and starts the
  //   initial animation.
  // - If the background and guest windows are ready, they are shown.
  void DoInitialSetupIfWindowsAreReady();

  // Returns true if the background window is valid and has painted.
  bool IsBackgroundWindowReady();

  // Returns true if the WebUIBrowser window is valid and has painted.
  bool IsWebUIWindowReady();

  // Focus a window and save it to login_window_to_focus_.
  void FocusLoginWindow(Window* win);

  // Stop hiding the mouse cursor if it's hidden and destroy ourselves.
  // Don't access |this| after calling this method!  Invoked when the first
  // browser window becomes visible.
  void HandleInitialBrowserWindowVisible();

  // Re-show the mouse cursor and destroy |hide_mouse_cursor_xid_|.
  void ShowMouseCursor();

  // Hide all login-related windows and ask the window manager to destroy us.
  // Called when we see the pixmap for a browser window get loaded.
  void HideWindowsAndRequestDestruction();

  // Send a D-Bus message to the session manager notifying it that the login
  // windows are visible.
  void NotifySessionManager();

  WindowManager* wm_;

  EventConsumerRegistrar registrar_;

  // The set of login windows we know about. This is all the windows in
  // |entries_| along with the guest window and background window.
  std::set<XWindow> login_xids_;

  // Other, non-login-specific windows that we're managing when Chrome is
  // in a not-logged-in state.
  std::set<XWindow> non_login_xids_;

  // Current login entries. Each entry consists of 5 windows, each window in
  // type params has index of the entry it belongs to. Usually the index in
  // window matches entry index in this array. But it may vary during short
  // period of time when some entry is removed or inserted. Chrome at first
  // updates indexes for all entries and then maps or unmaps all windows for
  // the entry.
  Entries entries_;

  // Did we get all the regular login (i.e. non-wizard, views based) windows and
  // show them?
  bool views_windows_are_ready_;

  // Index of the selected entry.
  size_t selected_entry_index_;

  // Used when the selection changes.
  SelectionChangedManager selection_changed_manager_;

  // One of the OOBE/wizard screens ("Take picture" or "Create account").
  // "Guest mode" or "guest user" windows are represented with LoginEntry.
  Window* wizard_window_;

  // Window placed in the background.
  Window* background_window_;

  // Window that is a WebUI Browser. This is used in WebUI based login.
  Window* webui_window_;

  // The controls or guest window that we've most recently focused.  We
  // track this so that if a transient window takes the focus and then gets
  // closed, we can re-focus the window that had the focus before.
  Window* login_window_to_focus_;

  // Are we waiting for a post-login browser window to get mapped and
  // painted so we can hide the login windows and destroy the login
  // controller?
  bool waiting_for_browser_window_;

  // Has HideWindowsAndRequestDestruction() been called?
  bool requested_destruction_;

  // Determines if entry selection is enabled at the moment.
  bool is_entry_selection_enabled_;

  // Index of the entry that was inserted or kNoSelection if no such entry.
  size_t last_inserted_entry_;

  // ID of an input window created so we can hide the mouse cursor until the
  // user starts using it.
  XWindow hide_mouse_cursor_xid_;

  // Login windows that have been destroyed post-login but that we're
  // holding on to, so we can continue displaying their actors onscreen
  // until the browser window has been painted.
  std::set<std::tr1::shared_ptr<DestroyedWindow> > destroyed_windows_;

  // XIDs of login windows that we've asked to take ownership of after they're
  // destroyed (i.e. windows that will eventually end up in
  // |destroyed_windows_|).  We track this so we can avoid double-registering a
  // window if it's remapped (http://crosbug.com/13093).
  std::set<XWindow> registered_destroyed_xids_;

  // Chrome browser windows that we're watching.  We wait for one of the
  // browser windows to get painted and then destroy ourselves.
  std::set<XWindow> browser_xids_;

  DISALLOW_COPY_AND_ASSIGN(LoginController);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_LOGIN_LOGIN_CONTROLLER_H_
