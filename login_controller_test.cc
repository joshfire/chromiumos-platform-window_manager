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

  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();
    // Use a WindowManager object that thinks that Chrome isn't logged in
    // yet so that LoginController will manage non-login windows as well.
    wm_.reset(new WindowManager(event_loop_.get(),
                                xconn_.get(),
                                compositor_.get(),
                                false));  // logged_in=false
    CHECK(wm_->Init());

    login_controller_ = wm_->login_controller_.get();

    background_xid_ = 0;
    guest_xid_ = 0;
  }

  // Create the set of windows expected by LoginController.
  void CreateLoginWindows(int num_entries,
                          bool background_is_ready,
                          bool create_guest_window) {
    CHECK(background_xid_ == 0);
    background_xid_ = CreateBasicWindow(0, 0, wm_->width(), wm_->height());
    vector<int> background_params;
    background_params.push_back(background_is_ready ? 1 : 0);
    wm_->wm_ipc()->SetWindowType(background_xid_,
                                 chromeos::WM_IPC_WINDOW_LOGIN_BACKGROUND,
                                 &background_params);
    SendInitialEventsForWindow(background_xid_);

    if (create_guest_window) {
      guest_xid_ = CreateBasicWindow(0, 0, wm_->width() / 2, wm_->height() / 2);
      wm_->wm_ipc()->SetWindowType(guest_xid_,
                                   chromeos::WM_IPC_WINDOW_LOGIN_GUEST,
                                   NULL);
      SendInitialEventsForWindow(guest_xid_);
    }

    for (int i = 0; i < num_entries; ++i) {
      EntryWindows entry;
      entry.border_xid = CreateSimpleWindow();
      entry.image_xid = CreateSimpleWindow();
      entry.controls_xid = CreateSimpleWindow();
      entry.label_xid = CreateSimpleWindow();
      entry.unselected_label_xid = CreateSimpleWindow();

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

      // The first border window stores some additional parameters.
      if (i == 0) {
        params.push_back(num_entries);
        params.push_back(kUnselectedImageSize);
        params.push_back(kGapBetweenImageAndControls);
      }
      wm_->wm_ipc()->SetWindowType(
          entry.border_xid,
          chromeos::WM_IPC_WINDOW_LOGIN_BORDER,
          &params);

      SendInitialEventsForWindow(entry.border_xid);
      SendInitialEventsForWindow(entry.image_xid);
      SendInitialEventsForWindow(entry.controls_xid);
      SendInitialEventsForWindow(entry.label_xid);
      SendInitialEventsForWindow(entry.unselected_label_xid);

      entries_.push_back(entry);
    }

    // LoginController registers a timeout to call this, so we need to call
    // it manually.
    // TODO: It'd be better to make it so that tests can manually run
    // timeouts that have been posted to EventLoop.
    if (num_entries > 0)
      login_controller_->InitialShow();
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

// Check that LoginController does some half-baked handling of any other
// windows that get mapped before Chrome is in a logged-in state.
TEST_F(LoginControllerTest, OtherWindows) {
  const int initial_x = 20;
  const int initial_y = 30;
  const int initial_width = 300;
  const int initial_height = 200;
  const XWindow xid =
      CreateBasicWindow(initial_x, initial_y, initial_width, initial_height);
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  ASSERT_FALSE(info->mapped);

  XEvent event;
  MockXConnection::InitCreateWindowEvent(&event, *info);
  wm_->HandleEvent(&event);
  Window* win = wm_->GetWindowOrDie(xid);
  MockCompositor::Actor* actor =
      dynamic_cast<MockCompositor::Actor*>(win->actor());
  CHECK(actor);

  // If LoginManager sees a MapRequest event before Chrome is logged in,
  // check that it maps the window in the requested location.
  MockXConnection::InitMapRequestEvent(&event, *info);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(info->mapped);
  EXPECT_EQ(initial_x, info->x);
  EXPECT_EQ(initial_y, info->y);
  EXPECT_EQ(initial_width, info->width);
  EXPECT_EQ(initial_height, info->height);

  // The window should still be in the same spot after it's mapped, and it
  // should be visible too.
  MockXConnection::InitMapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(initial_x, info->x);
  EXPECT_EQ(initial_y, info->y);
  EXPECT_EQ(initial_width, info->width);
  EXPECT_EQ(initial_height, info->height);
  EXPECT_EQ(initial_x, actor->x());
  EXPECT_EQ(initial_y, actor->y());
  EXPECT_EQ(initial_width, actor->GetWidth());
  EXPECT_EQ(initial_height, actor->GetHeight());
  EXPECT_TRUE(actor->visible());
  EXPECT_DOUBLE_EQ(1, actor->opacity());

  // Check that the client is able to move and resize itself.
  const int new_x = 40;
  const int new_y = 50;
  const int new_width = 500;
  const int new_height = 400;
  MockXConnection::InitConfigureRequestEvent(
      &event, xid, new_x, new_y, new_width, new_height);
  wm_->HandleEvent(&event);
  EXPECT_EQ(new_x, info->x);
  EXPECT_EQ(new_y, info->y);
  EXPECT_EQ(new_width, info->width);
  EXPECT_EQ(new_height, info->height);

  MockXConnection::InitConfigureNotifyEvent(&event, *info);
  wm_->HandleEvent(&event);
  EXPECT_EQ(new_x, actor->x());
  EXPECT_EQ(new_y, actor->y());
  EXPECT_EQ(new_width, actor->GetWidth());
  EXPECT_EQ(new_height, actor->GetHeight());

  MockXConnection::InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
}

// Test that the login controller assigns the focus correctly in a few cases.
TEST_F(LoginControllerTest, Focus) {
  CreateLoginWindows(3, true, false);

  // Initially, the first entry's controls window should be focused.
  EXPECT_EQ(entries_[0].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[0].controls_xid, GetActiveWindowProperty());

  // Click on the second entry's input window.
  ASSERT_GE(static_cast<int>(login_controller_->entries_.size()), 2);
  XWindow input_xid = login_controller_->entries_[1].input_window_xid;
  XEvent event;
  MockXConnection::InitButtonPressEvent(
      &event, *(xconn_->GetWindowInfoOrDie(input_xid)), 0, 0, 1);
  wm_->HandleEvent(&event);

  // The second entry should be focused now.
  EXPECT_EQ(entries_[1].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[1].controls_xid, GetActiveWindowProperty());

  // Now open a non-login window.  It should be automatically focused.
  const XWindow other_xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* other_info =
      xconn_->GetWindowInfoOrDie(other_xid);
  SendInitialEventsForWindow(other_xid);
  EXPECT_EQ(other_xid, xconn_->focused_xid());
  EXPECT_EQ(other_xid, GetActiveWindowProperty());
  EXPECT_FALSE(other_info->button_is_grabbed(0));

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
  xconn_->set_pointer_grab_xid(background_xid_);
  MockXConnection::InitButtonPressEvent(&event, *background_info, 0, 0, 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(entries_[1].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[1].controls_xid, GetActiveWindowProperty());
  EXPECT_FALSE(controls_info->button_is_grabbed(0));
  EXPECT_TRUE(other_info->button_is_grabbed(0));
}

// Test that the login controller focuses the guest window when no entries
// are created.
TEST_F(LoginControllerTest, FocusInitialGuestWindow) {
  CreateLoginWindows(0, true, true);
  EXPECT_EQ(guest_xid_, xconn_->focused_xid());
  EXPECT_EQ(guest_xid_, GetActiveWindowProperty());
}

TEST_F(LoginControllerTest, FocusTransientParent) {
  CreateLoginWindows(2, true, false);

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
  MockXConnection::InitUnmapEvent(&event, nested_transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());

  // Now unmap the first dialog and check that the focus goes back to the
  // controls window.
  MockXConnection::InitUnmapEvent(&event, transient_xid);
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
  MockXConnection::InitUnmapEvent(&event, bg_transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(entries_[0].controls_xid, xconn_->focused_xid());
  EXPECT_EQ(entries_[0].controls_xid, GetActiveWindowProperty());
}

TEST_F(LoginControllerTest, Modality) {
  CreateLoginWindows(2, true, false);
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
  MockXConnection::InitClientMessageEvent(
      &event, transient_xid, wm_->GetXAtom(ATOM_NET_WM_STATE),
      1, wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL), None, None, None);
  wm_->HandleEvent(&event);
  ASSERT_TRUE(wm_->GetWindowOrDie(transient_xid)->wm_state_modal());

  // Click in the controls window and check that the transient window keeps
  // the focus.  We also check that the click doesn't get replayed for the
  // controls window.
  int initial_num_replays = xconn_->num_pointer_ungrabs_with_replayed_events();
  xconn_->set_pointer_grab_xid(controls_xid);
  MockXConnection::InitButtonPressEvent(&event, *controls_info, 0, 0, 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(transient_xid, GetActiveWindowProperty());
  EXPECT_TRUE(controls_info->button_is_grabbed(0));
  EXPECT_FALSE(transient_info->button_is_grabbed(0));
  EXPECT_EQ(initial_num_replays,
            xconn_->num_pointer_ungrabs_with_replayed_events());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
