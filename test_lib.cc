// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/test_lib.h"

#include <vector>

#include <gflags/gflags.h>

#include "base/command_line.h"
#include "base/file_util.h"
#include "base/file_path.h"
#include "base/string_util.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/motion_event_coalescer.h"
#include "window_manager/panel.h"
#include "window_manager/panel_bar.h"
#include "window_manager/panel_manager.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"

using std::string;
using std::vector;

namespace window_manager {

testing::AssertionResult BytesAreEqual(
    const char* expected_expr,
    const char* actual_expr,
    const char* size_expr,
    const unsigned char* expected,
    const unsigned char* actual,
    size_t size) {
  for (size_t i = 0; i < size; ++i) {
    if (expected[i] != actual[i]) {
      testing::Message msg;
      string expected_str, actual_str, hl_str;
      bool first = true;
      for (size_t j = 0; j < size; ++j) {
        expected_str +=
            StringPrintf(" %02x", static_cast<unsigned char>(expected[j]));
        actual_str +=
            StringPrintf(" %02x", static_cast<unsigned char>(actual[j]));
        hl_str += (expected[j] == actual[j]) ? "   " : " ^^";
        if ((j % 16) == 15 || j == size - 1) {
          msg << (first ? "Expected:" : "\n         ") << expected_str << "\n"
              << (first ? "  Actual:" : "         ") << actual_str << "\n"
              << "         " << hl_str;
          expected_str = actual_str = hl_str = "";
          first = false;
        }
      }
      return testing::AssertionFailure(msg);
    }
  }
  return testing::AssertionSuccess();
}

int InitAndRunTests(int* argc, char** argv, bool* log_to_stderr) {
  google::ParseCommandLineFlags(argc, &argv, true);
  CommandLine::Init(*argc, argv);
  logging::InitLogging(NULL,
                       (log_to_stderr && *log_to_stderr) ?
                         logging::LOG_ONLY_TO_SYSTEM_DEBUG_LOG :
                         logging::LOG_NONE,
                       logging::DONT_LOCK_LOG_FILE,
                       logging::APPEND_TO_OLD_LOG_FILE);
  ::testing::InitGoogleTest(argc, argv);
  return RUN_ALL_TESTS();
}


BasicWindowManagerTest::ScopedTempDirectory::ScopedTempDirectory() {
  CHECK(file_util::CreateNewTempDirectory(string(), &path_));
}

BasicWindowManagerTest::ScopedTempDirectory::~ScopedTempDirectory() {
  if (!file_util::Delete(path_, true))  // recursive=true
    LOG(ERROR) << "Failed to delete path " << path_.value();
}


void BasicWindowManagerTest::SetUp() {
  event_loop_.reset(new EventLoop);
  xconn_.reset(new MockXConnection);

  // Register some fake mappings for common keysyms so that we won't get a
  // bunch of errors in the logs when we try to add bindings for them.
  KeyCode next_keycode = 1;
  for (int i = 0; i < 26; ++i, ++next_keycode) {
    xconn_->AddKeyMapping(next_keycode, XK_A + i);
    xconn_->AddKeyMapping(next_keycode, XK_a + i);
  }
  for (int i = 0; i < 10; ++i, ++next_keycode)
    xconn_->AddKeyMapping(next_keycode, XK_0 + i);
  for (int i = 0; i < 12; ++i, ++next_keycode)
    xconn_->AddKeyMapping(next_keycode, XK_F1 + i);
  xconn_->AddKeyMapping(next_keycode++, XK_Print);
  xconn_->AddKeyMapping(next_keycode++, XK_Tab);

  SetLoggedInState(true);
  compositor_.reset(new MockCompositor(xconn_.get()));
  wm_.reset(new WindowManager(event_loop_.get(),
                              xconn_.get(),
                              compositor_.get()));
  CHECK(wm_->Init());

  // Tell the WM that we implement a recent-enough version of the IPC
  // messages that we'll be giving it the position of the right-hand edge
  // of panels in drag messages.
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_IPC_VERSION);
  msg.set_param(0, 1);
  SendWmIpcMessage(msg);

  // Make the PanelManager's event coalescer run in synchronous mode; its
  // timer will never get triggered from within a test.
  wm_->panel_manager_->dragged_panel_event_coalescer_->set_synchronous(true);
}

void BasicWindowManagerTest::CreateAndInitNewWm() {
  wm_.reset(new WindowManager(event_loop_.get(),
                              xconn_.get(),
                              compositor_.get()));
  ASSERT_TRUE(wm_->Init());
}

XWindow BasicWindowManagerTest::CreateSimpleWindow() {
  return CreateBasicWindow(0, 0, 640, 480);
}

XWindow BasicWindowManagerTest::CreateBasicWindow(int x, int y,
                                                  int width, int height) {
  return xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      x, y,
      width, height,
      false,  // override redirect
      false,  // input only
      0);     // event mask
}

XWindow BasicWindowManagerTest::CreateToplevelWindow(int tab_count,
                                                     int selected_tab,
                                                     int x, int y,
                                                     int width, int height) {
  XWindow xid = CreateBasicWindow(x, y, width, height);
  ChangeTabInfo(xid, tab_count, selected_tab, wm_->GetCurrentTimeFromServer());
  return xid;
}

void BasicWindowManagerTest::ChangeTabInfo(XWindow toplevel_xid,
                                           int tab_count,
                                           int selected_tab,
                                           uint32 timestamp) {
  std::vector<int> params;
  params.push_back(tab_count);
  params.push_back(selected_tab);
  params.push_back(timestamp);
  wm_->wm_ipc()->SetWindowType(
      toplevel_xid, chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL, &params);
}

XWindow BasicWindowManagerTest::CreateSnapshotWindow(XWindow parent_xid,
                                                     int index,
                                                     int x, int y,
                                                     int width, int height) {
  XWindow xid = CreateBasicWindow(x, y, width, height);
  std::vector<int> params;
  params.push_back(parent_xid);
  params.push_back(index);
  wm_->wm_ipc()->SetWindowType(
      xid, chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT, &params);

  return xid;
}

XWindow BasicWindowManagerTest::CreateSimpleSnapshotWindow(XWindow parent_xid,
                                                           int index) {
  return CreateSnapshotWindow(parent_xid, index, 0, 0, 320, 240);
}

XWindow BasicWindowManagerTest::CreatePanelTitlebarWindow(
    int width, int height) {
  XWindow xid = CreateBasicWindow(0, 0, width, height);
  wm_->wm_ipc()->SetWindowType(
      xid, chromeos::WM_IPC_WINDOW_CHROME_PANEL_TITLEBAR, NULL);
  return xid;
}

XWindow BasicWindowManagerTest::CreatePanelContentWindow(
    int width, int height,
    XWindow titlebar_xid,
    bool expanded,
    bool take_focus) {
  XWindow xid = CreateBasicWindow(0, 0, width, height);
  std::vector<int> params;
  params.push_back(titlebar_xid);
  params.push_back(expanded ? 1 : 0);
  params.push_back(take_focus ? 1 : 0);
  wm_->wm_ipc()->SetWindowType(
      xid, chromeos::WM_IPC_WINDOW_CHROME_PANEL_CONTENT, &params);
  return xid;
}

Panel* BasicWindowManagerTest::CreatePanel(int width,
                                           int titlebar_height,
                                           int content_height,
                                           bool expanded,
                                           bool take_focus) {
  XWindow titlebar_xid = CreatePanelTitlebarWindow(width, titlebar_height);
  SendInitialEventsForWindow(titlebar_xid);
  XWindow content_xid = CreatePanelContentWindow(
      width, content_height, titlebar_xid, expanded, take_focus);
  SendInitialEventsForWindow(content_xid);
  Panel* panel = wm_->panel_manager_->panel_bar_->GetPanelByWindow(
      *(wm_->GetWindow(content_xid)));
  CHECK(panel);
  return panel;
}

void BasicWindowManagerTest::SendInitialEventsForWindow(XWindow xid) {
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  XEvent event;

  // Send a CreateWindowEvent, a MapRequest event (if this is a
  // non-override-redirect window), and a MapNotify event (if the window
  // got mapped).  After each event, send a ConfigureNotify if the window
  // was configured by the window manager.
  xconn_->InitCreateWindowEvent(&event, xid);
  int initial_num_configures = info->num_configures;
  wm_->HandleEvent(&event);
  if (info->num_configures != initial_num_configures) {
    xconn_->InitConfigureNotifyEvent(&event, xid);
    wm_->HandleEvent(&event);
    initial_num_configures = info->num_configures;
  }

  if (!info->override_redirect) {
    xconn_->InitMapRequestEvent(&event, xid);
    wm_->HandleEvent(&event);
    EXPECT_TRUE(info->mapped);

    if (info->num_configures != initial_num_configures) {
      xconn_->InitConfigureNotifyEvent(&event, xid);
      wm_->HandleEvent(&event);
      initial_num_configures = info->num_configures;
    }
  }

  if (info->mapped) {
    xconn_->InitMapEvent(&event, xid);
    wm_->HandleEvent(&event);
    if (info->num_configures != initial_num_configures) {
      xconn_->InitConfigureNotifyEvent(&event, xid);
      wm_->HandleEvent(&event);
      initial_num_configures = info->num_configures;
    }
  }
}

void BasicWindowManagerTest::SendWindowTypeEvent(XWindow xid) {
  XEvent event;
  xconn_->InitPropertyNotifyEvent(
      &event, xid, wm_->GetXAtom(ATOM_CHROME_WINDOW_TYPE));
  wm_->HandleEvent(&event);
}

void BasicWindowManagerTest::SendWmIpcMessage(const WmIpc::Message& msg) {
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(wm_->wm_xid());
  const size_t orig_num_messages = info->client_messages.size();

  // First, send the message using WmIpc.
  ASSERT_TRUE(wm_->wm_ipc()->SendMessage(wm_->wm_xid(), msg));

  // Next, copy it from where MockXConnection saved it and pass it to the
  // window manager.
  XEvent event;
  CHECK(info->client_messages.size() == orig_num_messages + 1);
  memcpy(&(event.xclient),
         &(info->client_messages.back()),
         sizeof(XClientMessageEvent));
  wm_->HandleEvent(&event);
}

void BasicWindowManagerTest::SendSetPanelStateMessage(Panel* panel,
                                                      bool expanded) {
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_SET_PANEL_STATE);
  msg.set_param(0, panel->content_xid());
  msg.set_param(1, expanded);
  SendWmIpcMessage(msg);
}

void BasicWindowManagerTest::SendPanelDraggedMessage(
    Panel* panel, int x, int y) {
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_PANEL_DRAGGED);
  msg.set_param(0, panel->content_xid());
  msg.set_param(1, x);
  msg.set_param(2, y);
  SendWmIpcMessage(msg);
}

void BasicWindowManagerTest::SendPanelDragCompleteMessage(Panel* panel) {
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_NOTIFY_PANEL_DRAG_COMPLETE);
  msg.set_param(0, panel->content_xid());
  SendWmIpcMessage(msg);
}

void BasicWindowManagerTest::SendSetLoginStateMessage(bool entries_selectable) {
  WmIpc::Message msg(chromeos::WM_IPC_MESSAGE_WM_SET_LOGIN_STATE);
  msg.set_param(0, entries_selectable);
  SendWmIpcMessage(msg);
}

void BasicWindowManagerTest::SendKey(XWindow xid,
                                     KeyBindings::KeyCombo key,
                                     XTime press_timestamp,
                                     XTime release_timestamp) {
  XEvent key_event;
  unsigned int keycode = xconn_->GetKeyCodeFromKeySym(key.keysym);
  unsigned int keymask = key.modifiers;
  xconn_->InitKeyPressEvent(&key_event, xid, keycode, keymask, press_timestamp);
  wm_->HandleEvent(&key_event);

  xconn_->InitKeyReleaseEvent(&key_event, xid, keycode, keymask,
                              release_timestamp);
  wm_->HandleEvent(&key_event);
}

void BasicWindowManagerTest::SendActiveWindowMessage(XWindow xid) {
  XEvent event;
  xconn_->InitClientMessageEvent(
      &event, xid, wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW),
      1,      // source indication (1 is from application)
      0,      // timestamp
      0,      // requestor's currently-active window
      0, 0);  // unused
  wm_->HandleEvent(&event);
}

void BasicWindowManagerTest::NotifyWindowAboutSize(Window* win) {
  win->HandleConfigureNotify(win->client_width(), win->client_height());
}

void BasicWindowManagerTest::SetLoggedInState(bool logged_in) {
  XAtom logged_in_xatom = 0;
  CHECK(xconn_->GetAtom("_CHROME_LOGGED_IN", &logged_in_xatom));
  xconn_->SetIntProperty(xconn_->GetRootWindow(),
                         logged_in_xatom,
                         logged_in_xatom,  // type; arbitrary
                         logged_in ? 1 : 0);

  if (wm_.get()) {
    XEvent event;
    xconn_->InitPropertyNotifyEvent(&event,
                                    xconn_->GetRootWindow(),
                                    logged_in_xatom);
    wm_->HandleEvent(&event);
  }
}

XWindow BasicWindowManagerTest::GetActiveWindowProperty() {
  int active_window;
  if (!xconn_->GetIntProperty(xconn_->GetRootWindow(),
                              wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW),
                              &active_window)) {
    return None;
  }
  return active_window;
}

bool BasicWindowManagerTest::WindowIsInLayer(Window* win,
                                             StackingManager::Layer layer) {
  const StackingManager::Layer next_layer =
      static_cast<StackingManager::Layer>(layer + 1);

  int win_index = xconn_->stacked_xids().GetIndex(win->xid());
  int layer_index = xconn_->stacked_xids().GetIndex(
      wm_->stacking_manager()->GetXidForLayer(layer));
  int next_layer_index = xconn_->stacked_xids().GetIndex(
      wm_->stacking_manager()->GetXidForLayer(next_layer));
  if (win_index <= layer_index || win_index >= next_layer_index)
    return false;

  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  win_index = stage->GetStackingIndex(win->actor());
  layer_index = stage->GetStackingIndex(
      wm_->stacking_manager()->GetActorForLayer(layer));
  next_layer_index = stage->GetStackingIndex(
      wm_->stacking_manager()->GetActorForLayer(next_layer));
  if (win_index <= layer_index || win_index >= next_layer_index)
    return false;

  return true;
}

bool BasicWindowManagerTest::WindowIsOffscreen(XWindow xid) {
  const MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  const MockXConnection::WindowInfo* root_info =
      xconn_->GetWindowInfoOrDie(xconn_->GetRootWindow());
  return (info->x + info->width <= 0 ||
          info->y + info->height <= 0 ||
          info->x >= root_info->width ||
          info->y >= root_info->height);
}

void BasicWindowManagerTest::TestIntArrayProperty(
    XWindow xid, XAtom atom, int num_values, ...) {
  vector<int> expected;

  va_list args;
  va_start(args, num_values);
  CHECK(num_values >= 0);
  for (; num_values; num_values--) {
    int arg = va_arg(args, int);
    expected.push_back(arg);
  }
  va_end(args);

  vector<int> actual;
  int exists = xconn_->GetIntArrayProperty(xid, atom, &actual);
  if (expected.empty()) {
    EXPECT_FALSE(exists);
  } else {
    EXPECT_TRUE(exists);
    ASSERT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < actual.size(); ++i)
      EXPECT_EQ(expected[i], actual[i]);
  }
}

void BasicWindowManagerTest::TestPanelContentBounds(
    Panel* panel, int x, int y, int width, int height) {
  EXPECT_EQ(x, panel->content_win()->client_x());
  EXPECT_EQ(y, panel->content_win()->client_y());
  EXPECT_EQ(width, panel->content_win()->client_width());
  EXPECT_EQ(height, panel->content_win()->client_height());

  EXPECT_EQ(x, panel->content_win()->actor()->GetX());
  EXPECT_EQ(y, panel->content_win()->actor()->GetY());
  EXPECT_EQ(width, panel->content_win()->actor()->GetWidth());
  EXPECT_EQ(height, panel->content_win()->actor()->GetHeight());
}

MockCompositor::TexturePixmapActor*
BasicWindowManagerTest::GetMockActorForWindow(Window* win) {
  CHECK(win);
  MockCompositor::TexturePixmapActor* cast_actor =
      dynamic_cast<MockCompositor::TexturePixmapActor*>(win->actor());
  CHECK(cast_actor);
  return cast_actor;
}

}  // namespace window_manager
