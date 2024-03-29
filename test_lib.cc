// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/test_lib.h"

#include <vector>

extern "C" {
#include <X11/XF86keysym.h>
}
#include <gflags/gflags.h>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/file_util.h"
#include "base/file_path.h"
#include "base/string_util.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/motion_event_coalescer.h"
#include "window_manager/panels/panel.h"
#include "window_manager/panels/panel_bar.h"
#include "window_manager/panels/panel_manager.h"
#include "window_manager/util.h"
#include "window_manager/window_manager.h"

DECLARE_bool(allow_panels_to_be_detached);  // from panel_bar.cc

using std::string;
using std::vector;
using window_manager::util::SetCurrentTimeForTest;

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
  base::AtExitManager exit_manager;  // needed by base::Singleton
  google::ParseCommandLineFlags(argc, &argv, true);
  CommandLine::Init(*argc, argv);
  logging::InitLogging(NULL,
                       (log_to_stderr && *log_to_stderr) ?
                         logging::LOG_ONLY_TO_SYSTEM_DEBUG_LOG :
                         logging::LOG_NONE,
                       logging::DONT_LOCK_LOG_FILE,
                       logging::APPEND_TO_OLD_LOG_FILE,
                       logging::DISABLE_DCHECK_FOR_NON_OFFICIAL_RELEASE_BUILDS);
  logging::SetLogItems(false,  // enable_process_id
                       false,  // enable_thread_id
                       true,   // enable_timestamp
                       true);  // enable_tickcount
  ::testing::InitGoogleTest(argc, argv);
  return RUN_ALL_TESTS();
}


ScopedTempDirectory::ScopedTempDirectory() {
  CHECK(file_util::CreateNewTempDirectory(string(), &path_));
}

ScopedTempDirectory::~ScopedTempDirectory() {
  if (!file_util::Delete(path_, true))  // recursive=true
    LOG(ERROR) << "Failed to delete path " << path_.value();
}


void BasicWindowManagerTest::SetUp() {
  // Detaching panels from the panel bar to dock them on the side of the
  // screen is disabled for now, but will probably be coming back later.
  // Leave it enabled for (most) tests.
  FLAGS_allow_panels_to_be_detached = true;

  new_panels_should_be_expanded_ = true;
  new_panels_should_take_focus_ = true;
  creator_content_xid_for_new_panels_ = 0;
  resize_type_for_new_panels_ =
      chromeos::WM_IPC_PANEL_USER_RESIZE_HORIZONTALLY_AND_VERTICALLY;

  SetCurrentTimeForTest(-1, 0);
  dbus_.reset(new MockDBusInterface);
  CHECK(dbus_->Init());
  event_loop_.reset(new EventLoop);
  xconn_.reset(new MockXConnection);
  RegisterCommonKeySyms();

  SetLoggedInState(true);
  compositor_.reset(new MockCompositor(xconn_.get()));
  CreateAndInitNewWm();

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

void BasicWindowManagerTest::RegisterCommonKeySyms() {
  CHECK(xconn_.get());

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
  xconn_->AddKeyMapping(next_keycode++, XK_Return);
  xconn_->AddKeyMapping(next_keycode++, XK_Escape);
  xconn_->AddKeyMapping(next_keycode++, XK_Left);
  xconn_->AddKeyMapping(next_keycode++, XK_Right);
  xconn_->AddKeyMapping(next_keycode++, XF86XK_AudioLowerVolume);
  xconn_->AddKeyMapping(next_keycode++, XF86XK_AudioMute);
  xconn_->AddKeyMapping(next_keycode++, XF86XK_AudioRaiseVolume);
}

void BasicWindowManagerTest::CreateNewWm() {
  wm_.reset(new WindowManager(event_loop_.get(),
                              xconn_.get(),
                              compositor_.get(),
                              dbus_.get()));
}

void BasicWindowManagerTest::CreateAndInitNewWm() {
  CreateNewWm();
  ASSERT_TRUE(wm_->Init());
}

XWindow BasicWindowManagerTest::CreateSimpleWindow() {
  return CreateBasicWindow(Rect(0, 0, 640, 480));
}

XWindow BasicWindowManagerTest::CreateBasicWindow(const Rect& bounds) {
  return xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      bounds,
      false,  // override redirect
      false,  // input only
      0, 0);  // event mask, visual
}

XWindow BasicWindowManagerTest::CreateToplevelWindow(int tab_count,
                                                     int selected_tab,
                                                     const Rect& bounds) {
  XWindow xid = CreateBasicWindow(bounds);
  ChangeTabInfo(xid, tab_count, selected_tab, wm_->GetCurrentTimeFromServer());
  return xid;
}

void BasicWindowManagerTest::ChangeTabInfo(XWindow toplevel_xid,
                                           int tab_count,
                                           int selected_tab,
                                           uint32_t timestamp) {
  std::vector<int> params;
  params.push_back(tab_count);
  params.push_back(selected_tab);
  params.push_back(timestamp);
  wm_->wm_ipc()->SetWindowType(
      toplevel_xid, chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL, &params);
}

XWindow BasicWindowManagerTest::CreateFavIconWindow(XWindow snapshot_xid,
                                                    const Size& size) {
  return CreateDecorationWindow(snapshot_xid,
                                chromeos::WM_IPC_WINDOW_CHROME_TAB_FAV_ICON,
                                size);
}

XWindow BasicWindowManagerTest::CreateTitleWindow(XWindow snapshot_xid,
                                                  const Size& size) {
  return CreateDecorationWindow(snapshot_xid,
                                chromeos::WM_IPC_WINDOW_CHROME_TAB_TITLE,
                                size);
}

XWindow BasicWindowManagerTest::CreateDecorationWindow(
    XWindow snapshot_xid,
    chromeos::WmIpcWindowType type,
    const Size& size) {
  XWindow xid = CreateBasicWindow(Rect(Point(0, 0), size));
  std::vector<int> params;
  params.push_back(snapshot_xid);
  wm_->wm_ipc()->SetWindowType(xid, type, &params);

  return xid;
}

XWindow BasicWindowManagerTest::CreateSnapshotWindow(XWindow parent_xid,
                                                     int index,
                                                     const Rect& bounds) {
  XWindow xid = CreateBasicWindow(bounds);
  std::vector<int> params;
  params.push_back(parent_xid);
  params.push_back(index);
  wm_->wm_ipc()->SetWindowType(
      xid, chromeos::WM_IPC_WINDOW_CHROME_TAB_SNAPSHOT, &params);

  return xid;
}

XWindow BasicWindowManagerTest::CreateSimpleSnapshotWindow(XWindow parent_xid,
                                                           int index) {
  return CreateSnapshotWindow(parent_xid, index, Rect(0, 0, 320, 240));
}

XWindow BasicWindowManagerTest::CreatePanelTitlebarWindow(const Size& size) {
  XWindow xid = CreateBasicWindow(Rect(Point(0, 0), size));
  wm_->wm_ipc()->SetWindowType(
      xid, chromeos::WM_IPC_WINDOW_CHROME_PANEL_TITLEBAR, NULL);
  return xid;
}

XWindow BasicWindowManagerTest::CreatePanelContentWindow(const Size& size,
                                                         XWindow titlebar_xid) {
  XWindow xid = CreateBasicWindow(Rect(Point(0, 0), size));
  std::vector<int> params;
  params.push_back(titlebar_xid);
  params.push_back(new_panels_should_be_expanded_ ? 1 : 0);
  params.push_back(new_panels_should_take_focus_ ? 1 : 0);
  params.push_back(creator_content_xid_for_new_panels_);
  params.push_back(resize_type_for_new_panels_);
  wm_->wm_ipc()->SetWindowType(
      xid, chromeos::WM_IPC_WINDOW_CHROME_PANEL_CONTENT, &params);
  return xid;
}

Panel* BasicWindowManagerTest::CreatePanel(
    int width, int titlebar_height, int content_height) {
  XWindow titlebar_xid =
      CreatePanelTitlebarWindow(Size(width, titlebar_height));
  SendInitialEventsForWindow(titlebar_xid);
  XWindow content_xid =
      CreatePanelContentWindow(Size(width, content_height), titlebar_xid);
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
    SendConfigureNotifyEvent(xid);
    initial_num_configures = info->num_configures;
  }

  if (!info->override_redirect) {
    xconn_->InitMapRequestEvent(&event, xid);
    wm_->HandleEvent(&event);
    EXPECT_TRUE(info->mapped);

    if (info->num_configures != initial_num_configures) {
      SendConfigureNotifyEvent(xid);
      initial_num_configures = info->num_configures;
    }
  }

  if (info->mapped) {
    xconn_->InitMapEvent(&event, xid);
    wm_->HandleEvent(&event);
    if (info->num_configures != initial_num_configures) {
      SendConfigureNotifyEvent(xid);
      initial_num_configures = info->num_configures;
    }
  }
}

void BasicWindowManagerTest::SendUnmapAndDestroyEventsForWindow(XWindow xid) {
  XEvent event;
  xconn_->InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
  xconn_->InitDestroyWindowEvent(&event, xid);
  wm_->HandleEvent(&event);
}

void BasicWindowManagerTest::SendWindowTypeEvent(XWindow xid) {
  XEvent event;
  xconn_->InitPropertyNotifyEvent(
      &event, xid, xconn_->GetAtomOrDie("_CHROME_WINDOW_TYPE"));
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
  const KeyCode key_code = xconn_->GetKeyCodeFromKeySym(key.keysym);
  const uint32_t mods = key.modifiers;

  XEvent event;
  xconn_->InitKeyPressEvent(&event, xid, key_code, mods, press_timestamp);
  wm_->HandleEvent(&event);
  xconn_->InitKeyReleaseEvent(&event, xid, key_code, mods, release_timestamp);
  wm_->HandleEvent(&event);
}

void BasicWindowManagerTest::SendActiveWindowMessage(XWindow xid) {
  XEvent event;
  xconn_->InitClientMessageEvent(
      &event, xid, xconn_->GetAtomOrDie("_NET_ACTIVE_WINDOW"),
      1,      // source indication (1 is from application)
      0,      // timestamp
      0,      // requestor's currently-active window
      0, 0);  // unused
  wm_->HandleEvent(&event);
}

void BasicWindowManagerTest::SendConfigureNotifyEvent(XWindow xid) {
  XEvent event;
  xconn_->InitConfigureNotifyEvent(&event, xid);
  if (xconn_->stacked_xids().Contains(xid)) {
    const XWindow* above_xid = xconn_->stacked_xids().GetUnder(xid);
    event.xconfigure.above = above_xid ? *above_xid : 0;
  }
  wm_->HandleEvent(&event);
}

void BasicWindowManagerTest::SetLoggedInState(bool logged_in) {
  XAtom logged_in_xatom = xconn_->GetAtomOrDie("_CHROME_LOGGED_IN");
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

void BasicWindowManagerTest::AppendAtomToProperty(XWindow xid,
                                                  XAtom property_atom,
                                                  XAtom atom_to_add) {
  vector<int> values;
  xconn_->GetIntArrayProperty(xid, property_atom, &values);
  values.push_back(atom_to_add);
  CHECK(xconn_->SetIntArrayProperty(
            xid,
            property_atom,                 // atom
            xconn_->GetAtomOrDie("ATOM"),  // type
            values));
}

void BasicWindowManagerTest::ConfigureWindowForSyncRequestProtocol(
    XWindow xid) {
  AppendAtomToProperty(xid,
                       xconn_->GetAtomOrDie("WM_PROTOCOLS"),
                       xconn_->GetAtomOrDie("_NET_WM_SYNC_REQUEST"));
  CHECK(xconn_->SetIntProperty(
            xid,
            xconn_->GetAtomOrDie("_NET_WM_SYNC_REQUEST_COUNTER"),  // atom
            xconn_->GetAtomOrDie("CARDINAL"),                      // type
            50));  // arbitrary counter ID
}

void BasicWindowManagerTest::SendSyncRequestProtocolAlarm(XWindow xid) {
  Window* win = wm_->GetWindowOrDie(xid);
  XEvent event;
  xconn_->InitSyncAlarmNotifyEvent(
      &event, win->wm_sync_request_alarm_, win->current_wm_sync_num_);
  wm_->HandleEvent(&event);
}

XWindow BasicWindowManagerTest::GetActiveWindowProperty() {
  int active_window;
  if (!xconn_->GetIntProperty(xconn_->GetRootWindow(),
                              xconn_->GetAtomOrDie("_NET_ACTIVE_WINDOW"),
                              &active_window)) {
    return None;
  }
  return active_window;
}

int BasicWindowManagerTest::GetNumDeleteWindowMessagesForWindow(XWindow xid) {
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);

  int num_deletes = 0;
  for (vector<XClientMessageEvent>::const_iterator it =
         info->client_messages.begin();
       it != info->client_messages.end(); ++it) {
    if (it->message_type == xconn_->GetAtomOrDie("WM_PROTOCOLS") &&
        it->format == XConnection::kLongFormat &&
        (static_cast<XAtom>(it->data.l[0]) ==
            xconn_->GetAtomOrDie("WM_DELETE_WINDOW"))) {
      num_deletes++;
    }
  }
  return num_deletes;
}

bool BasicWindowManagerTest::GetFirstWmIpcMessageOfType(
    XWindow xid, chromeos::WmIpcMessageType type, WmIpc::Message* msg_out) {
  CHECK(msg_out);
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  for (vector<XClientMessageEvent>::const_iterator it =
         info->client_messages.begin();
       it != info->client_messages.end(); ++it) {
    if (wm_->wm_ipc()->GetMessage(it->window,
                                  it->message_type,
                                  it->format,
                                  it->data.l,
                                  msg_out)) {
      if (msg_out->type() == type)
        return true;
    }
  }
  return false;
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
  return (info->bounds.x + info->bounds.width <= 0 ||
          info->bounds.y + info->bounds.height <= 0 ||
          info->bounds.x >= root_info->bounds.width ||
          info->bounds.y >= root_info->bounds.height);
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

bool BasicWindowManagerTest::PanelClientAndCompositedWindowsHaveSamePositions(
    Panel* panel) {
  CHECK(panel);
  return (panel->content_win()->composited_x() ==
          panel->content_win()->client_x()) &&
         (panel->content_win()->composited_y() ==
          panel->content_win()->client_y()) &&
         (panel->titlebar_win()->composited_x() ==
          panel->titlebar_win()->client_x()) &&
         (panel->titlebar_win()->composited_y() ==
          panel->titlebar_win()->client_y());
}

bool BasicWindowManagerTest::DecodeWmIpcMessage(
    const XClientMessageEvent& event, WmIpc::Message* msg_out) {
  CHECK(msg_out);
  return wm_->wm_ipc()->GetMessage(event.window,
                                   event.message_type,
                                   event.format,
                                   event.data.l,
                                   msg_out);
}

MockCompositor::TexturePixmapActor*
BasicWindowManagerTest::GetMockActorForWindow(Window* win) {
  CHECK(win);
  MockCompositor::TexturePixmapActor* cast_actor =
      dynamic_cast<MockCompositor::TexturePixmapActor*>(win->actor());
  CHECK(cast_actor);
  return cast_actor;
}

Rect BasicWindowManagerTest::GetCompositedWindowBounds(XWindow xid) {
  Window* win = wm_->GetWindowOrDie(xid);
  MockCompositor::TexturePixmapActor* actor = GetMockActorForWindow(win);
  return actor->GetBounds();
}


BasicCompositingTest::~BasicCompositingTest() {}

void BasicCompositingTest::SetUp() {
  // Make sure that RealCompositor's destructor isn't mucking around with
  // an already-deleted EventLoop when we start a new test case.
  // TODO: This originally happened in TearDown(), but that method doesn't
  // appear to be getting invoked in all cases when it should be, or
  // perhaps doesn't work as expected in derived classes.
  compositor_.reset(NULL);

  gl_.reset(new MockGLInterface);
  xconn_.reset(new MockXConnection);
  event_loop_.reset(new EventLoop);
  compositor_.reset(
      new RealCompositor(event_loop_.get(), xconn_.get(), gl_.get()));
}

void BasicCompositingTest::Draw() {
  compositor_->Draw();
}


BasicCompositingTreeTest::~BasicCompositingTreeTest() {}

void BasicCompositingTreeTest::SetUp() {
  BasicCompositingTest::SetUp();

  // Create an actor tree to test.
  stage_ = compositor_->GetDefaultStage();
  group1_.reset(compositor_->CreateGroup());
  group2_.reset(compositor_->CreateGroup());
  group3_.reset(compositor_->CreateGroup());
  group4_.reset(compositor_->CreateGroup());
  rect1_.reset(
      compositor_->CreateColoredBox(
          stage_->GetWidth(), stage_->GetHeight(), Compositor::Color()));
  rect2_.reset(
      compositor_->CreateColoredBox(
          stage_->GetWidth(), stage_->GetHeight(), Compositor::Color()));
  rect3_.reset(
      compositor_->CreateColoredBox(
          stage_->GetWidth(), stage_->GetHeight(), Compositor::Color()));

  stage_->SetName("stage");
  group1_->SetName("group1");
  group2_->SetName("group2");
  group3_->SetName("group3");
  group4_->SetName("group4");
  rect1_->SetName("rect1");
  rect2_->SetName("rect2");
  rect3_->SetName("rect3");

  stage_->AddActor(group1_.get());
  stage_->AddActor(group3_.get());
  group1_->AddActor(group2_.get());
  group2_->AddActor(rect1_.get());
  group3_->AddActor(group4_.get());
  group4_->AddActor(rect2_.get());
  group4_->AddActor(rect3_.get());
}

}  // namespace window_manager
