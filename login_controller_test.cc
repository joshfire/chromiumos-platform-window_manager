// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/scoped_ptr.h"
#include "base/logging.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/login_controller.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/shadow.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::vector;

namespace window_manager {

class LoginControllerTest : public BasicWindowManagerTest {
 protected:
  static const int kUnselectedImageSize;
  static const int kGapBetweenImageAndControls;
  static const int kImageSize;
  static const int kControlsSize;

  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();
    wm_.reset(NULL);
    // Use a WindowManager object that thinks that Chrome isn't logged in
    // yet so that LoginController will manage non-login windows as well.
    SetLoggedInState(false);
    CreateAndInitNewWm();

    login_controller_ = wm_->login_controller_.get();

    background_xid_ = 0;
    guest_xid_ = 0;
  }

  // Create the set of windows expected by LoginController.
  void CreateLoginWindows(int num_entries,
                          bool background_is_ready,
                          bool entry_pixmaps_are_ready,
                          bool create_guest_window) {
    CHECK(num_entries == 0 || num_entries >= 2);

    if (!background_xid_) {
      background_xid_ = CreateBasicWindow(0, 0, wm_->width(), wm_->height());
      vector<int> background_params;
      background_params.push_back(background_is_ready ? 1 : 0);
      wm_->wm_ipc()->SetWindowType(background_xid_,
                                   chromeos::WM_IPC_WINDOW_LOGIN_BACKGROUND,
                                   &background_params);
      SendInitialEventsForWindow(background_xid_);
    }

    if (create_guest_window) {
      guest_xid_ = CreateBasicWindow(0, 0, wm_->width() / 2, wm_->height() / 2);
      wm_->wm_ipc()->SetWindowType(guest_xid_,
                                   chromeos::WM_IPC_WINDOW_LOGIN_GUEST,
                                   NULL);
      SendInitialEventsForWindow(guest_xid_);
    }

    for (int i = 0; i < num_entries; ++i) {
      EntryWindows entry;
      entry.border_xid = CreateBasicWindow(0, 0,
          kImageSize + 2 * kGapBetweenImageAndControls,
          kImageSize + kControlsSize + 3 * kGapBetweenImageAndControls);
      entry.image_xid = CreateBasicWindow(0, 0, kImageSize, kImageSize);
      entry.controls_xid = CreateBasicWindow(0, 0, kImageSize, kControlsSize);
      entry.label_xid = CreateBasicWindow(0, 0, kImageSize, kControlsSize);
      entry.unselected_label_xid = CreateBasicWindow(0, 0, kImageSize,
                                                     kControlsSize);

      vector<int> params;
      params.push_back(i);  // entry index
      wm_->wm_ipc()->SetWindowType(
          entry.image_xid,
          chromeos::WM_IPC_WINDOW_LOGIN_IMAGE,
          &params);
      wm_->wm_ipc()->SetWindowType(
          entry.controls_xid,
          chromeos::WM_IPC_WINDOW_LOGIN_CONTROLS,
          &params);
      wm_->wm_ipc()->SetWindowType(
          entry.label_xid,
          chromeos::WM_IPC_WINDOW_LOGIN_LABEL,
          &params);
      wm_->wm_ipc()->SetWindowType(
          entry.unselected_label_xid,
          chromeos::WM_IPC_WINDOW_LOGIN_UNSELECTED_LABEL,
          &params);

      // The border window stores some additional parameters.
      params.push_back(num_entries);
      params.push_back(kUnselectedImageSize);
      params.push_back(kGapBetweenImageAndControls);
      wm_->wm_ipc()->SetWindowType(
          entry.border_xid,
          chromeos::WM_IPC_WINDOW_LOGIN_BORDER,
          &params);

      ConfigureWindowForSyncRequestProtocol(entry.border_xid);
      ConfigureWindowForSyncRequestProtocol(entry.image_xid);
      ConfigureWindowForSyncRequestProtocol(entry.controls_xid);
      ConfigureWindowForSyncRequestProtocol(entry.label_xid);
      ConfigureWindowForSyncRequestProtocol(entry.unselected_label_xid);

      SendInitialEventsForWindow(entry.border_xid);
      SendInitialEventsForWindow(entry.image_xid);
      SendInitialEventsForWindow(entry.controls_xid);
      SendInitialEventsForWindow(entry.label_xid);
      SendInitialEventsForWindow(entry.unselected_label_xid);

      entries_.push_back(entry);
    }

    if (entry_pixmaps_are_ready) {
      for (int i = 0; i < num_entries; ++i)
        SendInitialPixmapEventForEntry(i);
      // LoginController registers a timeout to call this, so we need to call
      // it manually.
      // TODO: It'd be better to make it so that tests can manually run
      // timeouts that have been posted to EventLoop.
      if (num_entries > 0)
        login_controller_->InitialShow();
    }
  }

  void SendInitialPixmapEventForEntry(size_t entry_index) {
    ASSERT_LT(entry_index, entries_.size());

    ASSERT_FALSE(
        wm_->GetWindowOrDie(
            entries_[entry_index].border_xid)->has_initial_pixmap());
    SendSyncRequestProtocolAlarm(entries_[entry_index].border_xid);

    ASSERT_FALSE(
        wm_->GetWindowOrDie(
            entries_[entry_index].image_xid)->has_initial_pixmap());
    SendSyncRequestProtocolAlarm(entries_[entry_index].image_xid);

    ASSERT_FALSE(
        wm_->GetWindowOrDie(
            entries_[entry_index].controls_xid)->has_initial_pixmap());
    SendSyncRequestProtocolAlarm(entries_[entry_index].controls_xid);

    ASSERT_FALSE(
        wm_->GetWindowOrDie(
            entries_[entry_index].label_xid)->has_initial_pixmap());
    SendSyncRequestProtocolAlarm(entries_[entry_index].label_xid);

    ASSERT_FALSE(
        wm_->GetWindowOrDie(
            entries_[entry_index].unselected_label_xid)->has_initial_pixmap());
    SendSyncRequestProtocolAlarm(entries_[entry_index].unselected_label_xid);
  }

  void UnmapLoginEntry(int i) {
    XEvent event;

    if (entries_[i].border_xid) {
      xconn_->UnmapWindow(entries_[i].border_xid);
      xconn_->InitUnmapEvent(&event, entries_[i].border_xid);
      wm_->HandleEvent(&event);
    }

    if (entries_[i].image_xid) {
      xconn_->UnmapWindow(entries_[i].image_xid);
      xconn_->InitUnmapEvent(&event, entries_[i].image_xid);
      wm_->HandleEvent(&event);
    }

    if (entries_[i].controls_xid) {
      xconn_->UnmapWindow(entries_[i].controls_xid);
      xconn_->InitUnmapEvent(&event, entries_[i].controls_xid);
      wm_->HandleEvent(&event);
    }

    if (entries_[i].label_xid) {
      xconn_->UnmapWindow(entries_[i].label_xid);
      xconn_->InitUnmapEvent(&event, entries_[i].label_xid);
      wm_->HandleEvent(&event);
    }

    if (entries_[i].unselected_label_xid) {
      xconn_->UnmapWindow(entries_[i].unselected_label_xid);
      xconn_->InitUnmapEvent(&event, entries_[i].unselected_label_xid);
      wm_->HandleEvent(&event);
    }
  }

  // Selects user entry with the specified index by sending IPC message to
  // wm.
  void SelectEntry(int index) {
    WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_SELECT_LOGIN_USER);
    msg.set_param(0, index);
    SendWmIpcMessage(msg);
  }

  // Checks if composited window for the specified xid is shown.
  bool IsCompositedShown(XWindow xid) const {
    Window* window = wm_->GetWindowOrDie(xid);
    return window->composited_shown();
  }

  double GetCompositedOpacity(XWindow xid) const {
    Window* window = wm_->GetWindowOrDie(xid);
    return window->composited_opacity();
  }

  // A collection of windows for a single login entry.
  struct EntryWindows {
    EntryWindows()
        : border_xid(0),
          image_xid(0),
          controls_xid(0),
          label_xid(0),
          unselected_label_xid(0) {
    }

    XWindow border_xid;
    XWindow image_xid;
    XWindow controls_xid;
    XWindow label_xid;
    XWindow unselected_label_xid;
  };

  LoginController* login_controller_;  // owned by 'wm_'

  XWindow background_xid_;
  XWindow guest_xid_;
  vector<EntryWindows> entries_;
};

const int LoginControllerTest::kUnselectedImageSize = 100;
const int LoginControllerTest::kGapBetweenImageAndControls = 5;
const int LoginControllerTest::kImageSize = 260;
const int LoginControllerTest::kControlsSize = 30;

// Check that border windows have shadows but other login windows don't.
TEST_F(LoginControllerTest, Shadow) {
  CreateLoginWindows(2, true, true, true);

  EXPECT_TRUE(wm_->GetWindowOrDie(entries_[0].border_xid)->shadow() != NULL);
  EXPECT_TRUE(wm_->GetWindowOrDie(entries_[0].image_xid)->shadow() == NULL);
  EXPECT_TRUE(wm_->GetWindowOrDie(entries_[0].controls_xid)->shadow() == NULL);
  EXPECT_TRUE(wm_->GetWindowOrDie(entries_[0].label_xid)->shadow() == NULL);
  EXPECT_TRUE(wm_->GetWindowOrDie(entries_[0].unselected_label_xid)->
                  shadow() == NULL);

  EXPECT_TRUE(wm_->GetWindowOrDie(guest_xid_)->shadow() == NULL);
  EXPECT_TRUE(wm_->GetWindowOrDie(background_xid_)->shadow() == NULL);
}

// Check that LoginController does some half-baked handling of transient
// windows that get mapped before Chrome is in a logged-in state.
TEST_F(LoginControllerTest, OtherWindows) {
  CreateLoginWindows(2, true, true, true);

  const int initial_width = 300;
  const int initial_height = 200;
  const XWindow xid = CreateBasicWindow(0, 0, initial_width, initial_height);
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  info->transient_for = background_xid_;
  ASSERT_FALSE(info->mapped);

  XEvent event;
  xconn_->InitCreateWindowEvent(&event, xid);
  wm_->HandleEvent(&event);
  Window* win = wm_->GetWindowOrDie(xid);
  MockCompositor::Actor* actor = GetMockActorForWindow(win);

  // If LoginManager sees a MapRequest event before Chrome is logged in,
  // check that it maps the window centered over its owner.
  xconn_->InitMapRequestEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(info->mapped);
  EXPECT_EQ((wm_->width() - initial_width) / 2, info->x);
  EXPECT_EQ((wm_->height() - initial_height) / 2, info->y);
  EXPECT_EQ(initial_width, info->width);
  EXPECT_EQ(initial_height, info->height);

  // The window should still be in the same spot after it's mapped, and it
  // should be visible and have a shadow too.
  xconn_->InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ((wm_->width() - initial_width) / 2, info->x);
  EXPECT_EQ((wm_->height() - initial_height) / 2, info->y);
  EXPECT_EQ(initial_width, info->width);
  EXPECT_EQ(initial_height, info->height);
  EXPECT_EQ((wm_->width() - initial_width) / 2, info->x);
  EXPECT_EQ((wm_->height() - initial_height) / 2, info->y);
  EXPECT_EQ(initial_width, actor->GetWidth());
  EXPECT_EQ(initial_height, actor->GetHeight());
  EXPECT_TRUE(actor->is_shown());
  EXPECT_DOUBLE_EQ(1, actor->opacity());
  ASSERT_TRUE(win->shadow() != NULL);
  EXPECT_TRUE(win->shadow()->is_shown());

  // Check that the client is able to move and resize itself.
  const int new_x = 40;
  const int new_y = 50;
  const int new_width = 500;
  const int new_height = 400;
  xconn_->InitConfigureRequestEvent(
      &event, xid, new_x, new_y, new_width, new_height);
  wm_->HandleEvent(&event);
  EXPECT_EQ(new_x, info->x);
  EXPECT_EQ(new_y, info->y);
  EXPECT_EQ(new_width, info->width);
  EXPECT_EQ(new_height, info->height);

  xconn_->InitConfigureNotifyEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(new_x, actor->x());
  EXPECT_EQ(new_y, actor->y());
  EXPECT_EQ(new_width, actor->GetWidth());
  EXPECT_EQ(new_height, actor->GetHeight());

  xconn_->InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_FALSE(actor->is_shown());

  // Info bubbles shouldn't get shadows.
  XWindow info_bubble_xid = CreateSimpleWindow();
  ASSERT_TRUE(wm_->wm_ipc()->SetWindowType(
      info_bubble_xid,
      chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE,
      NULL));
  xconn_->GetWindowInfoOrDie(info_bubble_xid)->transient_for = background_xid_;
  SendInitialEventsForWindow(info_bubble_xid);
  EXPECT_TRUE(wm_->GetWindowOrDie(info_bubble_xid)->shadow() == NULL);

  // Non-transient non-login windows should be ignored by the login
  // controller.
  XWindow non_transient_xid = CreateSimpleWindow();
  xconn_->InitCreateWindowEvent(&event, non_transient_xid);
  wm_->HandleEvent(&event);
  xconn_->InitMapRequestEvent(&event, non_transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_FALSE(xconn_->GetWindowInfoOrDie(non_transient_xid)->mapped);
  Window* non_transient_win = wm_->GetWindowOrDie(non_transient_xid);
  EXPECT_FALSE(GetMockActorForWindow(non_transient_win)->is_shown());

  // Even after the user has logged in, we should continue to manage
  // transient windows belonging to login windows.
  SetLoggedInState(true);
  XWindow post_login_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* post_login_info =
      xconn_->GetWindowInfoOrDie(post_login_xid);
  post_login_info->transient_for = background_xid_;
  SendInitialEventsForWindow(post_login_xid);

  Window* post_login_win = wm_->GetWindowOrDie(post_login_xid);
  MockCompositor::Actor* post_login_actor =
      GetMockActorForWindow(post_login_win);
  EXPECT_TRUE(post_login_info->mapped);
  EXPECT_TRUE(post_login_actor->is_shown());
}

// Test that the login controller assigns the focus correctly in a few cases.
TEST_F(LoginControllerTest, Focus) {
  CreateLoginWindows(3, true, true, false);

  // Initially, the first entry's controls window should be focused.
  EXPECT_EQ(entries_[0].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[0].controls_xid, GetActiveWindowProperty());

  // Click on the second entry's input window.
  ASSERT_GE(static_cast<int>(login_controller_->entries_.size()), 2);
  SelectEntry(1);

  // The second entry should be focused now.
  EXPECT_EQ(entries_[1].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[1].controls_xid, GetActiveWindowProperty());

  // Now open a non-login window.  It should be automatically focused.
  const XWindow other_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* other_info =
      xconn_->GetWindowInfoOrDie(other_xid);
  other_info->transient_for = background_xid_;
  SendInitialEventsForWindow(other_xid);
  EXPECT_EQ(other_xid, xconn_->focused_xid());
  EXPECT_EQ(other_xid, GetActiveWindowProperty());
  EXPECT_FALSE(other_info->button_is_grabbed(0));

  // Check that override-redirect non-login window (i.e. tooltip) won't be
  // focused.
  const XWindow override_redirect_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* override_redirect_info =
      xconn_->GetWindowInfoOrDie(override_redirect_xid);
  override_redirect_info->override_redirect = true;
  ASSERT_TRUE(xconn_->MapWindow(override_redirect_xid));
  SendInitialEventsForWindow(override_redirect_xid);
  EXPECT_NE(override_redirect_xid, xconn_->focused_xid());
  EXPECT_NE(override_redirect_xid, GetActiveWindowProperty());

  // Button grabs should be installed on the background and controls windows.
  MockXConnection::WindowInfo* background_info =
      xconn_->GetWindowInfoOrDie(background_xid_);
  MockXConnection::WindowInfo* controls_info =
      xconn_->GetWindowInfoOrDie(entries_[1].controls_xid);
  EXPECT_TRUE(background_info->button_is_grabbed(0));
  EXPECT_TRUE(controls_info->button_is_grabbed(0));

  // After we click on the background, the second entry's controls window
  // should be refocused and a button grab should be installed on the
  // non-login window.
  XEvent event;
  xconn_->set_pointer_grab_xid(background_xid_);
  xconn_->InitButtonPressEvent(&event, background_xid_, 0, 0, 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(entries_[1].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[1].controls_xid, GetActiveWindowProperty());
  EXPECT_FALSE(controls_info->button_is_grabbed(0));
  EXPECT_TRUE(other_info->button_is_grabbed(0));
}

// Test that the login controller focuses the guest window when no entries
// are created.
TEST_F(LoginControllerTest, FocusInitialGuestWindow) {
  CreateLoginWindows(0, true, true, true);
  EXPECT_EQ(guest_xid_, xconn_->focused_xid());
  EXPECT_EQ(guest_xid_, GetActiveWindowProperty());
}

TEST_F(LoginControllerTest, FocusTransientParent) {
  CreateLoginWindows(2, true, true, false);

  // When we open a transient dialog, it should get the focus.
  const XWindow transient_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = entries_[0].controls_xid;
  SendInitialEventsForWindow(transient_xid);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());

  // Now open another dialog that's transient for the first dialog.
  const XWindow nested_transient_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* nested_transient_info =
      xconn_->GetWindowInfoOrDie(nested_transient_xid);
  nested_transient_info->transient_for = transient_xid;
  SendInitialEventsForWindow(nested_transient_xid);
  EXPECT_EQ(nested_transient_xid, xconn_->focused_xid());
  EXPECT_EQ(nested_transient_xid, GetActiveWindowProperty());

  // If we unmap the nested dialog, the focus should go back to the first
  // dialog.
  XEvent event;
  xconn_->InitUnmapEvent(&event, nested_transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());

  // Now unmap the first dialog and check that the focus goes back to the
  // controls window.
  xconn_->InitUnmapEvent(&event, transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(entries_[0].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[0].controls_xid, GetActiveWindowProperty());

  // Open a transient dialog, but make it owned by the background window.
  const XWindow bg_transient_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* bg_transient_info =
      xconn_->GetWindowInfoOrDie(bg_transient_xid);
  bg_transient_info->transient_for = background_xid_;
  SendInitialEventsForWindow(bg_transient_xid);
  EXPECT_EQ(bg_transient_xid, xconn_->focused_xid());
  EXPECT_EQ(bg_transient_xid, GetActiveWindowProperty());

  // We never want to focus the background.  When the dialog gets unmapped,
  // we should focus the previously-focused controls window instead.
  xconn_->InitUnmapEvent(&event, bg_transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(entries_[0].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[0].controls_xid, GetActiveWindowProperty());
}

TEST_F(LoginControllerTest, Modality) {
  CreateLoginWindows(2, true, true, false);
  const XWindow controls_xid = entries_[0].controls_xid;
  MockXConnection::WindowInfo* controls_info =
      xconn_->GetWindowInfoOrDie(controls_xid);

  // Map a transient window and check that it gets the focus.
  const XWindow transient_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = entries_[0].controls_xid;
  SendInitialEventsForWindow(transient_xid);
  ASSERT_EQ(transient_xid, xconn_->focused_xid());
  ASSERT_EQ(transient_xid, GetActiveWindowProperty());

  // Now ask the WM to make the transient window modal.
  XEvent event;
  xconn_->InitClientMessageEvent(
      &event, transient_xid, wm_->GetXAtom(ATOM_NET_WM_STATE),
      1, wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL), None, None, None);
  wm_->HandleEvent(&event);
  ASSERT_TRUE(wm_->GetWindowOrDie(transient_xid)->wm_state_modal());

  // Click in the controls window and check that the transient window keeps
  // the focus.  We also check that the click doesn't get replayed for the
  // controls window.
  int initial_num_replays = xconn_->num_pointer_ungrabs_with_replayed_events();
  xconn_->set_pointer_grab_xid(controls_xid);
  xconn_->InitButtonPressEvent(&event, controls_xid, 0, 0, 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());
  EXPECT_TRUE(controls_info->button_is_grabbed(0));
  EXPECT_FALSE(transient_info->button_is_grabbed(0));
  EXPECT_EQ(initial_num_replays,
            xconn_->num_pointer_ungrabs_with_replayed_events());
}

TEST_F(LoginControllerTest, HideAfterLogin) {
  // We should show the windows after they're mapped.
  CreateLoginWindows(2, true, true, false);
  EXPECT_FALSE(WindowIsOffscreen(background_xid_));

  // They should still be shown even after the user logs in.
  SetLoggedInState(true);
  EXPECT_FALSE(WindowIsOffscreen(background_xid_));

  // But we should hide them after the first Chrome window is created.
  XWindow xid = CreateToplevelWindow(1, 0,  // tab_count, selected_tab
                                     0, 0, 200, 200);  // position and size
  SendInitialEventsForWindow(xid);
  EXPECT_TRUE(WindowIsOffscreen(background_xid_));
}

TEST_F(LoginControllerTest, ShowDestroyedWindows) {
  // Create some login windows and then tell the window manager that the
  // user has logged in.
  CreateLoginWindows(2, true, true, false);
  MockCompositor::TexturePixmapActor* background_actor =
      GetMockActorForWindow(wm_->GetWindowOrDie(background_xid_));
  SetLoggedInState(true);

  EXPECT_TRUE(wm_->GetWindow(background_xid_) != NULL);
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  EXPECT_TRUE(stage->stacked_children()->Contains(background_actor));

  // Now unmap and destroy the background window.
  XEvent event;
  xconn_->InitUnmapEvent(&event, background_xid_);
  wm_->HandleEvent(&event);
  xconn_->InitDestroyWindowEvent(&event, background_xid_);
  wm_->HandleEvent(&event);

  // Even though the background window has been destroyed, its actor should
  // still be displayed.
  EXPECT_TRUE(wm_->GetWindow(background_xid_) == NULL);
  EXPECT_TRUE(stage->stacked_children()->Contains(background_actor));

  // After the initial browser window gets mapped (but not yet painted), we
  // should still show the background.
  XWindow xid = CreateToplevelWindow(1, 0,  // tab_count, selected_tab
                                     0, 0, 200, 200);  // position and size
  ConfigureWindowForSyncRequestProtocol(xid);
  SendInitialEventsForWindow(xid);
  EXPECT_TRUE(stage->stacked_children()->Contains(background_actor));

  // After it's painted, the login actors should be destroyed.
  SendSyncRequestProtocolAlarm(xid);
  EXPECT_FALSE(stage->stacked_children()->Contains(background_actor));
}

TEST_F(LoginControllerTest, SelectGuest) {
  // Create two entries for new Chrome.
  CreateLoginWindows(2, true, true, false);

  // The first entry should initially be focused.
  EXPECT_EQ(entries_[0].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[0].controls_xid, GetActiveWindowProperty());

  // Click on the entry for the guest window.
  SelectEntry(1);

  // The guest entry should be focused.
  EXPECT_EQ(entries_[1].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[1].controls_xid, GetActiveWindowProperty());

  // Click on the first entry.
  SelectEntry(0);

  // The first entry should be focused.
  EXPECT_EQ(entries_[0].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[0].controls_xid, GetActiveWindowProperty());

  // Click on the entry for the guest window again.
  SelectEntry(1);

  // The guest entry should be focused.
  EXPECT_EQ(entries_[1].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[1].controls_xid, GetActiveWindowProperty());

  // Create guest window.
  guest_xid_ = CreateBasicWindow(0, 0, wm_->width() / 2, wm_->height() / 2);
  wm_->wm_ipc()->SetWindowType(guest_xid_,
                               chromeos::WM_IPC_WINDOW_LOGIN_GUEST,
                               NULL);
  SendInitialEventsForWindow(guest_xid_);

  // The guest window should be focused.
  EXPECT_EQ(guest_xid_, xconn_->focused_xid());
  EXPECT_EQ(guest_xid_, GetActiveWindowProperty());
}

TEST_F(LoginControllerTest, RemoveUser) {
  // Create 3 entries for new Chrome.
  CreateLoginWindows(3, true, true, false);

  // The first entry should initially be focused.
  EXPECT_EQ(entries_[0].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[0].controls_xid, GetActiveWindowProperty());

  UnmapLoginEntry(0);
  EXPECT_EQ(entries_[1].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[1].controls_xid, GetActiveWindowProperty());

  UnmapLoginEntry(1);
  // The guest entry should be focused.
  EXPECT_EQ(entries_[2].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[2].controls_xid, GetActiveWindowProperty());

  // Create guest window.
  guest_xid_ = CreateBasicWindow(0, 0, wm_->width() / 2, wm_->height() / 2);
  wm_->wm_ipc()->SetWindowType(guest_xid_,
                               chromeos::WM_IPC_WINDOW_LOGIN_GUEST,
                               NULL);
  SendInitialEventsForWindow(guest_xid_);
  UnmapLoginEntry(2);

  // The guest window should be focused.
  EXPECT_EQ(guest_xid_, xconn_->focused_xid());
  EXPECT_EQ(guest_xid_, GetActiveWindowProperty());
}

// Test which windows of selected and unselected entry should be off or on
// screen.
TEST_F(LoginControllerTest, ClientOnOffScreen) {
  // Create two entries for new Chrome.
  CreateLoginWindows(2, true, true, false);  // Only need usual entry windows.

  // The first entry is selected. Test that controls, image and label
  // windows are on screen and the rest windows are off screen.
  EXPECT_TRUE(WindowIsOffscreen(entries_[0].border_xid));
  EXPECT_FALSE(WindowIsOffscreen(entries_[0].image_xid));
  EXPECT_FALSE(WindowIsOffscreen(entries_[0].controls_xid));
  EXPECT_FALSE(WindowIsOffscreen(entries_[0].label_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[0].unselected_label_xid));

  // For the second unselected entry, only image and unselected label windows
  // must be on screen.
  EXPECT_TRUE(WindowIsOffscreen(entries_[1].border_xid));
  EXPECT_FALSE(WindowIsOffscreen(entries_[1].image_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[1].controls_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[1].label_xid));
  EXPECT_FALSE(WindowIsOffscreen(entries_[1].unselected_label_xid));

  // Click on the second entry to change the selection.
  SelectEntry(1);

  // Now the same should be checked for both entries but with the second as
  // the selected one.
  EXPECT_TRUE(WindowIsOffscreen(entries_[1].border_xid));
  EXPECT_FALSE(WindowIsOffscreen(entries_[1].image_xid));
  EXPECT_FALSE(WindowIsOffscreen(entries_[1].controls_xid));
  EXPECT_FALSE(WindowIsOffscreen(entries_[1].label_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[1].unselected_label_xid));

  EXPECT_TRUE(WindowIsOffscreen(entries_[0].border_xid));
  EXPECT_FALSE(WindowIsOffscreen(entries_[0].image_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[0].controls_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[0].label_xid));
  EXPECT_FALSE(WindowIsOffscreen(entries_[0].unselected_label_xid));

  // Now check that for both entries windows are hidden when login succeeded
  // and the first Chrome window is shown.
  SetLoggedInState(true);
  XWindow xid = CreateToplevelWindow(1, 0,  // tab_count, selected_tab
                                     0, 0, 200, 200);  // position and size
  SendInitialEventsForWindow(xid);

  EXPECT_TRUE(WindowIsOffscreen(entries_[0].border_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[0].image_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[0].controls_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[0].label_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[0].unselected_label_xid));

  EXPECT_TRUE(WindowIsOffscreen(entries_[1].border_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[1].image_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[1].controls_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[1].label_xid));
  EXPECT_TRUE(WindowIsOffscreen(entries_[1].unselected_label_xid));
}

TEST_F(LoginControllerTest, SelectTwice) {
  CreateLoginWindows(2, true, true, false);

  // The first entry is selected now by default.
  EXPECT_TRUE(IsCompositedShown(entries_[0].border_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[0].image_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[0].controls_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[0].label_xid));
  EXPECT_FALSE(IsCompositedShown(entries_[0].unselected_label_xid));
  EXPECT_EQ(1.0, GetCompositedOpacity(entries_[0].controls_xid));

  EXPECT_TRUE(IsCompositedShown(entries_[1].border_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[1].image_xid));
  EXPECT_FALSE(IsCompositedShown(entries_[1].controls_xid));
  EXPECT_FALSE(IsCompositedShown(entries_[1].label_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[1].unselected_label_xid));
  EXPECT_EQ(0.0, GetCompositedOpacity(entries_[1].controls_xid));

  // Select it again.
  SelectEntry(0);
  login_controller_->ProcessSelectionChangeCompleted(0);

  EXPECT_TRUE(IsCompositedShown(entries_[0].border_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[0].image_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[0].controls_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[0].label_xid));
  EXPECT_FALSE(IsCompositedShown(entries_[0].unselected_label_xid));
  EXPECT_EQ(1.0, GetCompositedOpacity(entries_[0].controls_xid));

  EXPECT_TRUE(IsCompositedShown(entries_[1].border_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[1].image_xid));
  EXPECT_FALSE(IsCompositedShown(entries_[1].controls_xid));
  EXPECT_FALSE(IsCompositedShown(entries_[1].label_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[1].unselected_label_xid));
  EXPECT_EQ(0.0, GetCompositedOpacity(entries_[1].controls_xid));

  // And again.
  SelectEntry(0);
  login_controller_->ProcessSelectionChangeCompleted(0);

  EXPECT_TRUE(IsCompositedShown(entries_[0].border_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[0].image_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[0].controls_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[0].label_xid));
  EXPECT_FALSE(IsCompositedShown(entries_[0].unselected_label_xid));
  EXPECT_EQ(1.0, GetCompositedOpacity(entries_[0].controls_xid));

  EXPECT_TRUE(IsCompositedShown(entries_[1].border_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[1].image_xid));
  EXPECT_FALSE(IsCompositedShown(entries_[1].controls_xid));
  EXPECT_FALSE(IsCompositedShown(entries_[1].label_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[1].unselected_label_xid));
  EXPECT_EQ(0.0, GetCompositedOpacity(entries_[1].controls_xid));

  // Now select the guest entry and check that selection is actually changed.
  // Check that image window is hidden for selected guest entry.
  SelectEntry(1);
  login_controller_->ProcessSelectionChangeCompleted(0);

  // Some changes to windows happen when timer shoots which doesn't work when
  // running the test manually, so we check for properties that change without
  // timer only.
  EXPECT_FALSE(IsCompositedShown(entries_[0].label_xid));
  EXPECT_EQ(0.0, GetCompositedOpacity(entries_[0].label_xid));
  EXPECT_TRUE(IsCompositedShown(entries_[0].unselected_label_xid));
  EXPECT_FALSE(IsCompositedShown(entries_[0].controls_xid));
  EXPECT_EQ(0.0, GetCompositedOpacity(entries_[0].controls_xid));

  EXPECT_TRUE(IsCompositedShown(entries_[1].label_xid));
  EXPECT_FALSE(IsCompositedShown(entries_[1].image_xid));
  EXPECT_FALSE(IsCompositedShown(entries_[1].unselected_label_xid));
}

// Test that we don't crash when Chrome crashes and the login entry windows
// are unmapped in a random order (see http://crosbug.com/5117).
TEST_F(LoginControllerTest, NoCrashOnInconsistenEntry) {

  CreateLoginWindows(3, true, true, false);

  // Unmap border window for second entry.
  XEvent event;
  xconn_->UnmapWindow(entries_[1].border_xid);
  xconn_->InitUnmapEvent(&event, entries_[1].border_xid);
  wm_->HandleEvent(&event);
  entries_[1].border_xid = NULL;

  // Unmap all other windows.
  UnmapLoginEntry(0);
  UnmapLoginEntry(1);
  UnmapLoginEntry(2);
}

// Test that we don't crash if the guest entry is active and an unmap event
// happens for some reason (e.g. Chrome crashes).
TEST_F(LoginControllerTest, NoCrashOnReverseOrderEntryDelete) {
  CreateLoginWindows(3, true, true, false);

  // Select guest entry.
  SelectEntry(2);

  // Unmap windows in reverse order.
  UnmapLoginEntry(2);
  UnmapLoginEntry(1);
  UnmapLoginEntry(0);
}

// Test that we don't animate new entry windows getting selected when the
// previously-selected entry is unmapped after the user has logged in.
// (Otherwise, weird animations happen while Chrome is cleaning up right
// before mapping the initial browser window.)
TEST_F(LoginControllerTest, DontSelectEntryAfterLogin) {
  CreateLoginWindows(3, true, true, false);
  SelectEntry(0);

  // Grab the original position of the client window and the actor
  // containing the user's picture.
  XConnection::WindowGeometry orig_geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(entries_[1].controls_xid,
                                        &orig_geometry));

  MockCompositor::Actor* image_actor =
      GetMockActorForWindow(wm_->GetWindowOrDie(entries_[1].image_xid));
  const float orig_actor_x = image_actor->x();
  const float orig_actor_y = image_actor->y();
  const float orig_actor_scale_x = image_actor->scale_x();
  const float orig_actor_scale_y = image_actor->scale_y();

  // Now tell the WM that we're logged in and unmap the first entry, which
  // should result in the second entry getting selected.
  SetLoggedInState(true);
  UnmapLoginEntry(0);

  // Check that the second entry's window and actor didn't get moved.
  XConnection::WindowGeometry new_geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(entries_[1].controls_xid,
                                        &new_geometry));
  EXPECT_EQ(orig_geometry.x, new_geometry.x);
  EXPECT_EQ(orig_geometry.y, new_geometry.y);

  EXPECT_FLOAT_EQ(orig_actor_x, image_actor->x());
  EXPECT_FLOAT_EQ(orig_actor_y, image_actor->y());
  EXPECT_FLOAT_EQ(orig_actor_scale_x, image_actor->scale_x());
  EXPECT_FLOAT_EQ(orig_actor_scale_y, image_actor->scale_y());
}

TEST_F(LoginControllerTest, ShowEntriesAfterTheyGetPixmaps) {
  const int kEntriesCount = 3;
  CreateLoginWindows(kEntriesCount, true, false, false);
  EXPECT_TRUE(WindowIsOffscreen(background_xid_));
  // Begin sending messages that entry windows get pixmaps.
  for (int i = 0; i < kEntriesCount; ++i) {
    EXPECT_FALSE(login_controller_->all_windows_are_ready_)
        << "Entry index " << i;
    EXPECT_TRUE(WindowIsOffscreen(entries_[i].border_xid))
        << "Entry index " << i;
    EXPECT_TRUE(WindowIsOffscreen(entries_[i].image_xid))
        << "Entry index " << i;
    EXPECT_TRUE(WindowIsOffscreen(entries_[i].controls_xid))
        << "Entry index " << i;
    EXPECT_TRUE(WindowIsOffscreen(entries_[i].label_xid))
        << "Entry index " << i;
    EXPECT_TRUE(WindowIsOffscreen(entries_[i].unselected_label_xid))
        << "Entry index " << i;

    SendInitialPixmapEventForEntry(i);
  }
  // Check that all needed windows are on the screen.
  EXPECT_TRUE(login_controller_->all_windows_are_ready_);
  EXPECT_FALSE(WindowIsOffscreen(background_xid_));
  for (int i = 0; i < kEntriesCount; ++i) {
    EXPECT_FALSE(WindowIsOffscreen(entries_[i].image_xid));
    if (i == 0) {  // Selected entry.
      EXPECT_FALSE(WindowIsOffscreen(entries_[i].controls_xid));
      EXPECT_FALSE(WindowIsOffscreen(entries_[i].label_xid));
      EXPECT_TRUE(WindowIsOffscreen(entries_[i].unselected_label_xid));
    } else {
      EXPECT_TRUE(WindowIsOffscreen(entries_[i].label_xid));
      EXPECT_FALSE(WindowIsOffscreen(entries_[i].unselected_label_xid));
    }
  }
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
