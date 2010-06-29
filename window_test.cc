// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <vector>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/shadow.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::map;
using std::vector;

namespace window_manager {

class WindowTest : public BasicWindowManagerTest {};

TEST_F(WindowTest, WindowType) {
  XWindow xid = CreateSimpleWindow();
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);

  // Without a window type, we should have a shadow.
  EXPECT_EQ(chromeos::WM_IPC_WINDOW_UNKNOWN, win.type());
  EXPECT_TRUE(win.using_shadow());

  // Toplevel windows shouldn't have shadows.
  ASSERT_TRUE(wm_->wm_ipc()->SetWindowType(
                  xid, chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL, NULL));
  EXPECT_TRUE(win.FetchAndApplyWindowType(true));  // update_shadow
  EXPECT_EQ(chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL, win.type());
  EXPECT_FALSE(win.using_shadow());

  // Info bubbles shouldn't have shadows.
  ASSERT_TRUE(wm_->wm_ipc()->SetWindowType(
                  xid, chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE, NULL));
  EXPECT_TRUE(win.FetchAndApplyWindowType(true));  // update_shadow
  EXPECT_EQ(chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE, win.type());
  EXPECT_FALSE(win.using_shadow());
}

TEST_F(WindowTest, ChangeClient) {
  XWindow xid = CreateBasicWindow(10, 20, 30, 40);
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window window(wm_.get(), xid, false, geometry);

  // Make sure that the window's initial attributes are loaded correctly.
  EXPECT_EQ(xid, window.xid());
  EXPECT_EQ(10, window.client_x());
  EXPECT_EQ(20, window.client_y());
  EXPECT_EQ(30, window.client_width());
  EXPECT_EQ(40, window.client_height());
  EXPECT_EQ(false, window.mapped());

  EXPECT_TRUE(window.MapClient());
  EXPECT_TRUE(info->mapped);

  // Move the window.
  EXPECT_TRUE(window.MoveClient(100, 200));
  EXPECT_EQ(100, info->x);
  EXPECT_EQ(200, info->y);
  EXPECT_EQ(100, window.client_x());
  EXPECT_EQ(200, window.client_y());

  // Resize the window.
  EXPECT_TRUE(window.ResizeClient(300, 400, GRAVITY_NORTHWEST));
  EXPECT_EQ(300, info->width);
  EXPECT_EQ(400, info->height);
  EXPECT_EQ(300, window.client_width());
  EXPECT_EQ(400, window.client_height());

  // We need to be able to update windows' local geometry variables in
  // response to ConfigureNotify events to be able to handle
  // override-redirect windows, so make sure that that works correctly.
  window.SaveClientPosition(50, 60);
  window.SaveClientSize(70, 80);
  EXPECT_EQ(50, window.client_x());
  EXPECT_EQ(60, window.client_y());
  EXPECT_EQ(70, window.client_width());
  EXPECT_EQ(80, window.client_height());
}

TEST_F(WindowTest, ChangeComposited) {
  XWindow xid = CreateBasicWindow(10, 20, 30, 40);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window window(wm_.get(), xid, false, geometry);
  xconn_->MapWindow(xid);
  window.HandleMapNotify();

  MockCompositor::TexturePixmapActor* actor = GetMockActorForWindow(&window);

  // Initially, we should place the composited window at the same location
  // as the client window.
  EXPECT_EQ(10, actor->x());
  EXPECT_EQ(20, actor->y());
  EXPECT_EQ(10, window.composited_x());
  EXPECT_EQ(20, window.composited_y());
  EXPECT_EQ(30, window.actor()->GetWidth());
  EXPECT_EQ(40, window.actor()->GetHeight());
  EXPECT_DOUBLE_EQ(1.0, actor->scale_x());
  EXPECT_DOUBLE_EQ(1.0, actor->scale_y());
  EXPECT_DOUBLE_EQ(1.0, window.composited_scale_x());
  EXPECT_DOUBLE_EQ(1.0, window.composited_scale_y());

  // Move the composited window to a new spot.
  window.MoveComposited(40, 50, 0);
  EXPECT_EQ(40, actor->x());
  EXPECT_EQ(50, actor->y());
  EXPECT_EQ(40, window.composited_x());
  EXPECT_EQ(50, window.composited_y());

  window.ScaleComposited(0.75, 0.25, 0);
  EXPECT_EQ(0.75, actor->scale_x());
  EXPECT_EQ(0.25, actor->scale_y());
  EXPECT_EQ(0.75, window.composited_scale_x());
  EXPECT_EQ(0.25, window.composited_scale_y());
}

TEST_F(WindowTest, TransientFor) {
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);

  XWindow owner_xid = 1234;  // arbitrary ID
  info->transient_for = owner_xid;
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  EXPECT_EQ(owner_xid, win.transient_for_xid());

  XWindow new_owner_xid = 5678;
  info->transient_for = new_owner_xid;
  EXPECT_TRUE(win.FetchAndApplyTransientHint());
  EXPECT_EQ(new_owner_xid, win.transient_for_xid());
}

TEST_F(WindowTest, GetMaxSize) {
  XWindow xid = CreateBasicWindow(10, 20, 30, 40);

  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  info->size_hints.min_width = 400;
  info->size_hints.min_height = 300;
  info->size_hints.max_width = 800;
  info->size_hints.max_height = 600;
  info->size_hints.width_increment = 10;
  info->size_hints.height_increment = 5;
  info->size_hints.base_width = 40;
  info->size_hints.base_width = 30;

  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  ASSERT_TRUE(win.FetchAndApplySizeHints());
  int width = 0, height = 0;

  // We should get the minimum size if we request a size smaller than it.
  win.GetMaxSize(300, 200, &width, &height);
  EXPECT_EQ(400, width);
  EXPECT_EQ(300, height);

  // And the maximum size if we request one larger than it.
  win.GetMaxSize(1000, 800, &width, &height);
  EXPECT_EQ(800, width);
  EXPECT_EQ(600, height);

  // Test that the size increment hints are honored when we request a size
  // that's not equal to the base size plus some multiple of the size
  // increments.
  win.GetMaxSize(609, 409, &width, &height);
  EXPECT_EQ(600, width);
  EXPECT_EQ(405, height);
}

// Test WM_DELETE_WINDOW and WM_TAKE_FOCUS from ICCCM's WM_PROTOCOLS.
TEST_F(WindowTest, WmProtocols) {
  // Create a window.
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);

  // Set its WM_PROTOCOLS property to indicate that it supports both
  // message types.
  vector<int> values;
  values.push_back(static_cast<int>(wm_->GetXAtom(ATOM_WM_DELETE_WINDOW)));
  values.push_back(static_cast<int>(wm_->GetXAtom(ATOM_WM_TAKE_FOCUS)));
  xconn_->SetIntArrayProperty(xid,
                              wm_->GetXAtom(ATOM_WM_PROTOCOLS),  // atom
                              wm_->GetXAtom(ATOM_ATOM),          // type
                              values);

  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);

  // Send a WM_DELETE_WINDOW message to the window and check that its
  // contents are correct.
  XTime timestamp = 43;  // arbitrary
  EXPECT_TRUE(win.SendDeleteRequest(timestamp));
  ASSERT_EQ(1, info->client_messages.size());
  const XClientMessageEvent& delete_msg = info->client_messages[0];
  EXPECT_EQ(wm_->GetXAtom(ATOM_WM_PROTOCOLS), delete_msg.message_type);
  EXPECT_EQ(XConnection::kLongFormat, delete_msg.format);
  EXPECT_EQ(wm_->GetXAtom(ATOM_WM_DELETE_WINDOW), delete_msg.data.l[0]);
  EXPECT_EQ(timestamp, delete_msg.data.l[1]);

  // Now do the same thing with WM_TAKE_FOCUS.
  timestamp = 98;  // arbitrary
  info->client_messages.clear();
  EXPECT_TRUE(win.TakeFocus(timestamp));
  ASSERT_EQ(1, info->client_messages.size());
  const XClientMessageEvent& focus_msg = info->client_messages[0];
  EXPECT_EQ(wm_->GetXAtom(ATOM_WM_PROTOCOLS), focus_msg.message_type);
  EXPECT_EQ(XConnection::kLongFormat, focus_msg.format);
  EXPECT_EQ(wm_->GetXAtom(ATOM_WM_TAKE_FOCUS), focus_msg.data.l[0]);
  EXPECT_EQ(timestamp, focus_msg.data.l[1]);

  // Get rid of the window's WM_PROTOCOLS support.
  xconn_->DeletePropertyIfExists(xid, wm_->GetXAtom(ATOM_WM_PROTOCOLS));
  win.FetchAndApplyWmProtocols();
  info->client_messages.clear();

  // SendDeleteRequest() should fail outright if the window doesn't support
  // WM_DELETE_WINDOW.
  EXPECT_FALSE(win.SendDeleteRequest(1));
  EXPECT_EQ(0, info->client_messages.size());

  // TakeFocus() should manually assign the focus with a SetInputFocus
  // request instead of sending a message.
  EXPECT_EQ(None, xconn_->focused_xid());
  EXPECT_TRUE(win.TakeFocus(timestamp));
  EXPECT_EQ(0, info->client_messages.size());
  EXPECT_EQ(xid, xconn_->focused_xid());
}

TEST_F(WindowTest, WmHints) {
  // Set the urgency flag on a window and check that it gets loaded correctly.
  const XAtom wm_hints_atom = wm_->GetXAtom(ATOM_WM_HINTS);
  XWindow xid = CreateSimpleWindow();
  xconn_->SetIntProperty(xid,
                         wm_hints_atom,  // atom
                         wm_hints_atom,  // type
                         256);  // UrgencyHint flag from ICCCM 4.1.2.4
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  EXPECT_TRUE(win.wm_hint_urgent());

  // Now clear the UrgencyHint flag and set another flag that we don't care
  // about and check that the window loads the change.
  vector<int> values;
  values.push_back(2);  // StateHint flag
  values.push_back(1);  // NormalState
  xconn_->SetIntArrayProperty(xid, wm_hints_atom, wm_hints_atom, values);
  win.FetchAndApplyWmHints();
  EXPECT_FALSE(win.wm_hint_urgent());

  // Set it one more time.
  xconn_->SetIntProperty(xid, wm_hints_atom, wm_hints_atom, 256);
  win.FetchAndApplyWmHints();
  EXPECT_TRUE(win.wm_hint_urgent());
}

TEST_F(WindowTest, WmState) {
  const XAtom wm_state_atom = wm_->GetXAtom(ATOM_NET_WM_STATE);
  const XAtom fullscreen_atom = wm_->GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN);
  const XAtom max_horz_atom = wm_->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_HORZ);
  const XAtom max_vert_atom = wm_->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_VERT);
  const XAtom modal_atom = wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL);

  // Create a window with its _NET_WM_STATE property set to only
  // _NET_WM_STATE_MODAL and make sure that it's correctly loaded in the
  // constructor.
  XWindow xid = CreateSimpleWindow();
  xconn_->SetIntProperty(xid,
                         wm_state_atom,             // atom
                         wm_->GetXAtom(ATOM_ATOM),  // type
                         modal_atom);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  EXPECT_FALSE(win.wm_state_fullscreen());
  EXPECT_TRUE(win.wm_state_modal());

  // Now make the Window object handle a message removing the modal
  // state...
  long data[5];
  memset(data, 0, sizeof(data));
  data[0] = 0;  // remove
  data[1] = modal_atom;
  map<XAtom, bool> states;
  win.ParseWmStateMessage(data, &states);
  EXPECT_TRUE(win.ChangeWmState(states));
  EXPECT_FALSE(win.wm_state_fullscreen());
  EXPECT_FALSE(win.wm_state_modal());

  // ... and one adding the fullscreen state.
  data[0] = 1;  // add
  data[1] = fullscreen_atom;
  win.ParseWmStateMessage(data, &states);
  EXPECT_TRUE(win.ChangeWmState(states));
  EXPECT_TRUE(win.wm_state_fullscreen());
  EXPECT_FALSE(win.wm_state_modal());

  // Check that the window's _NET_WM_STATE property was updated in response
  // to the messages.
  vector<int> values;
  ASSERT_TRUE(xconn_->GetIntArrayProperty(xid, wm_state_atom, &values));
  ASSERT_EQ(1, values.size());
  EXPECT_EQ(fullscreen_atom, values[0]);

  // Test that we can toggle states (and that we process messages listing
  // multiple states correctly).
  data[0] = 2;  // toggle
  data[1] = fullscreen_atom;
  data[2] = modal_atom;
  win.ParseWmStateMessage(data, &states);
  EXPECT_TRUE(win.ChangeWmState(states));
  EXPECT_FALSE(win.wm_state_fullscreen());
  EXPECT_TRUE(win.wm_state_modal());

  values.clear();
  ASSERT_TRUE(xconn_->GetIntArrayProperty(xid, wm_state_atom, &values));
  ASSERT_EQ(1, values.size());
  EXPECT_EQ(modal_atom, values[0]);

  // Test that ChangeWmState() works for clearing the modal state and
  // setting both maximized states.
  map<XAtom, bool> changed_states;
  changed_states[modal_atom] = false;
  changed_states[max_horz_atom] = true;
  changed_states[max_vert_atom] = true;
  EXPECT_TRUE(win.ChangeWmState(changed_states));
  values.clear();
  ASSERT_TRUE(xconn_->GetIntArrayProperty(xid, wm_state_atom, &values));
  ASSERT_EQ(2, values.size());
  EXPECT_EQ(max_horz_atom, values[0]);
  EXPECT_EQ(max_vert_atom, values[1]);
}

TEST_F(WindowTest, WmWindowType) {
  const XAtom wm_window_type_atom = wm_->GetXAtom(ATOM_NET_WM_WINDOW_TYPE);
  const XAtom combo_atom = wm_->GetXAtom(ATOM_NET_WM_WINDOW_TYPE_COMBO);
  const XAtom menu_atom = wm_->GetXAtom(ATOM_NET_WM_WINDOW_TYPE_MENU);
  const XAtom dropdown_atom = wm_->GetXAtom(
      ATOM_NET_WM_WINDOW_TYPE_DROPDOWN_MENU);
  const XAtom popup_atom = wm_->GetXAtom(ATOM_NET_WM_WINDOW_TYPE_POPUP_MENU);

  // Create an override redirect X window.
  XWindow xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),
      0, 0, 640, 480,
      true,   // override redirect
      false,  // input only
      0);     // event mask
  xconn_->SetIntProperty(xid,
                         wm_window_type_atom,  // atom
                         wm_window_type_atom,  // type
                         combo_atom);  // combo type

  // Attach our Window to the X window
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, true, geometry);
  EXPECT_TRUE(win.using_shadow());  // use shadow for combo

  xconn_->SetIntProperty(xid,
                         wm_window_type_atom,  // atom
                         wm_window_type_atom,  // type
                         menu_atom);  // menu type
  win.FetchAndApplyWmWindowType(true);
  EXPECT_TRUE(win.using_shadow());  // use shadow for menu

  xconn_->SetIntProperty(xid,
                         wm_window_type_atom,  // atom
                         wm_window_type_atom,  // type
                         dropdown_atom);  // dropdown menu type
  win.FetchAndApplyWmWindowType(true);
  EXPECT_TRUE(win.using_shadow());  // use shadow for dropdown menu

  xconn_->SetIntProperty(xid,
                         wm_window_type_atom,  // atom
                         wm_window_type_atom,  // type
                         popup_atom);  // popup menu type
  win.FetchAndApplyWmWindowType(true);
  EXPECT_TRUE(win.using_shadow());  // use shadow for popup menu

  XAtom normal_atom = 0;
  ASSERT_TRUE(xconn_->GetAtom("_NET_WM_WINDOW_TYPE_NORMAL", &normal_atom));

  xconn_->SetIntProperty(xid,
                         wm_window_type_atom,  // atom
                         wm_window_type_atom,  // type
                         normal_atom);  // normal type
  win.FetchAndApplyWmWindowType(true);
  EXPECT_FALSE(win.using_shadow());  // not use shadow for normal
}

TEST_F(WindowTest, ChromeState) {
  const XAtom state_atom = wm_->GetXAtom(ATOM_CHROME_STATE);
  const XAtom collapsed_atom = wm_->GetXAtom(ATOM_CHROME_STATE_COLLAPSED_PANEL);
  // This isn't an atom that we'd actually set in the _CHROME_STATE
  // property, but we need another atom besides the collapsed one for
  // testing.
  const XAtom other_atom = wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL);

  // Set the "collapsed" atom on a window.  The Window class should load
  // the initial property in its constructor.
  XWindow xid = CreateSimpleWindow();
  xconn_->SetIntProperty(xid,
                         state_atom,                // atom
                         wm_->GetXAtom(ATOM_ATOM),  // type
                         collapsed_atom);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);

  // Tell the window to set the other atom.
  map<XAtom, bool> states;
  states[other_atom] = true;
  EXPECT_TRUE(win.ChangeChromeState(states));

  // Check that both atoms are included in the property.
  vector<int> values;
  ASSERT_TRUE(xconn_->GetIntArrayProperty(xid, state_atom, &values));
  ASSERT_EQ(2, values.size());
  EXPECT_EQ(collapsed_atom, values[0]);
  EXPECT_EQ(other_atom, values[1]);

  // Now tell the window to unset the "collapsed" atom, and make sure that
  // only the other atom is present.
  states.clear();
  states[collapsed_atom] = false;
  EXPECT_TRUE(win.ChangeChromeState(states));
  values.clear();
  ASSERT_TRUE(xconn_->GetIntArrayProperty(xid, state_atom, &values));
  ASSERT_EQ(1, values.size());
  EXPECT_EQ(other_atom, values[0]);

  // If we also unset the other atom, the property should be removed.
  states.clear();
  states[other_atom] = false;
  EXPECT_TRUE(win.ChangeChromeState(states));
  EXPECT_FALSE(xconn_->GetIntArrayProperty(xid, state_atom, &values));
}

TEST_F(WindowTest, Shape) {
  // Create a shaped window.
  int width = 10;
  int height = 5;
  XWindow xid = CreateBasicWindow(10, 20, width, height);
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  info->shape.reset(new ByteMap(width, height));
  info->shape->Clear(0xff);
  info->shape->SetRectangle(0, 0, 3, 3, 0x0);

  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  EXPECT_TRUE(info->shape_events_selected);
  EXPECT_TRUE(win.shaped());
  EXPECT_FALSE(win.using_shadow());

  // Set the opacity for the window's shadow (even though it's not using a
  // shadow right now).
  double shadow_opacity = 0.5;
  win.SetShadowOpacity(shadow_opacity, 0);  // anim_ms

  // Check that the shape mask got applied to the compositing actor.
  MockCompositor::TexturePixmapActor* mock_actor = GetMockActorForWindow(&win);
  ASSERT_TRUE(mock_actor->alpha_mask_bytes() != NULL);
  EXPECT_PRED_FORMAT3(BytesAreEqual,
                      info->shape->bytes(),
                      mock_actor->alpha_mask_bytes(),
                      width * height);

  // Change the shape and check that the window updates its actor.
  info->shape->Clear(0xff);
  info->shape->SetRectangle(width - 3, height - 3, 3, 3, 0x0);
  win.FetchAndApplyShape(true);  // update_shadow
  EXPECT_TRUE(win.shaped());
  EXPECT_FALSE(win.using_shadow());
  ASSERT_TRUE(mock_actor->alpha_mask_bytes() != NULL);
  EXPECT_PRED_FORMAT3(BytesAreEqual,
                      info->shape->bytes(),
                      mock_actor->alpha_mask_bytes(),
                      width * height);

  // Now clear the shape and make sure that the mask is removed from the
  // actor.
  info->shape.reset();
  win.FetchAndApplyShape(true);  // update_shadow
  EXPECT_FALSE(win.shaped());
  EXPECT_TRUE(mock_actor->alpha_mask_bytes() == NULL);

  // The newly-created shadow should have the opacity that we set earlier.
  EXPECT_TRUE(win.using_shadow());
  ASSERT_TRUE(win.shadow() != NULL);
  EXPECT_EQ(shadow_opacity, win.shadow()->opacity());
}

TEST_F(WindowTest, OverrideRedirectForDestroyedWindow) {
  // Check that Window's c'tor uses the passed-in override-redirect value
  // instead of querying the server.  If an override-redirect window has
  // already been destroyed, we don't want to mistakenly think that it's
  // non-override-redirect.
  // TODO: Remove this once we're able to grab the server while
  // constructing Window objects (see comments in window_manager.cc).
  XConnection::WindowGeometry geometry;
  Window win(wm_.get(), 43241, true, geometry);
  EXPECT_TRUE(win.override_redirect());
}

// Test that we remove windows' borders.
TEST_F(WindowTest, RemoveBorder) {
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  info->border_width = 1;

  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  EXPECT_EQ(0, info->border_width);
}

// Test that we don't resize the composited window until we receive
// notification that the client window has been resized.  Otherwise, we can
// end up with the previous contents being scaled to fit the new size --
// see http://crosbug.com/1279.
TEST_F(WindowTest, DeferResizingActor) {
  const int orig_width = 300, orig_height = 200;
  XWindow xid = CreateToplevelWindow(2, 0, 0, 0, orig_width, orig_height);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  xconn_->MapWindow(xid);
  win.HandleMapNotify();

  // Check that the actor's initial dimensions match that of the client window.
  EXPECT_EQ(orig_width, win.actor()->GetWidth());
  EXPECT_EQ(orig_height, win.actor()->GetHeight());

  // After resizing the client window, the actor should still still be
  // using the original dimensions.
  const int new_width = 600, new_height = 400;
  win.ResizeClient(new_width, new_height, GRAVITY_NORTHWEST);
  EXPECT_EQ(orig_width, win.actor()->GetWidth());
  EXPECT_EQ(orig_height, win.actor()->GetHeight());

  // Now let the window know that we've seen a ConfigureNotify event with
  // the new dimensions and check that the actor is resized.
  win.HandleConfigureNotify(new_width, new_height);
  EXPECT_EQ(new_width, win.actor()->GetWidth());
  EXPECT_EQ(new_height, win.actor()->GetHeight());
}

// Test that pixmap actor and shadow sizes get updated correctly in
// response to ConfigureNotify events.
TEST_F(WindowTest, UpdatePixmapAndShadowSizes) {
  const int orig_width = 300, orig_height = 200;
  XWindow xid = CreateToplevelWindow(2, 0, 0, 0, orig_width, orig_height);
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);

  // Resize the window once before it gets mapped, to make sure that we get
  // the updated size later after the window is mapped.
  const int second_width = orig_width + 10, second_height = orig_height + 10;
  ASSERT_TRUE(xconn_->ResizeWindow(xid, second_width, second_height));
  win.HandleConfigureNotify(info->width, info->height);

  // Now map the window and check that everything starts out at the right size.
  ASSERT_TRUE(xconn_->MapWindow(xid));
  win.HandleMapNotify();
  MockCompositor::TexturePixmapActor* actor = GetMockActorForWindow(&win);
  EXPECT_EQ(second_width, actor->GetWidth());
  EXPECT_EQ(second_height, actor->GetHeight());
  EXPECT_EQ(second_width, win.shadow()->width());
  EXPECT_EQ(second_height, win.shadow()->height());

  // We shouldn't reload the pixmap in response to a non-resize
  // ConfigureNotify event (like what we'll receive whenever the window
  // gets moved).
  info->compositing_pixmap++;
  win.HandleConfigureNotify(info->width, info->height);
  EXPECT_EQ(info->compositing_pixmap - 1, actor->pixmap());
  info->compositing_pixmap--;

  // Now act as if the window gets resized two more times, but the second
  // resize has already happened in the X server by the time that the
  // window manager receives the ConfigureNotify for the first resize.
  const int third_width = second_width + 10, third_height = second_height + 10;
  const int fourth_width = third_width + 10, fourth_height = third_height + 10;
  xconn_->ResizeWindow(xid, fourth_width, fourth_height);
  win.HandleConfigureNotify(third_width, third_height);

  // We should load the pixmap now and resize the shadow to the dimensions
  // from the final pixmap instead of the ones supplied in the event.
  EXPECT_EQ(fourth_width, actor->GetWidth());
  EXPECT_EQ(fourth_height, actor->GetHeight());
  EXPECT_EQ(fourth_width, win.shadow()->width());
  EXPECT_EQ(fourth_height, win.shadow()->height());

  // Nothing should change after we get the second ConfigureNotify.
  win.HandleConfigureNotify(fourth_width, fourth_height);
  EXPECT_EQ(fourth_width, actor->GetWidth());
  EXPECT_EQ(fourth_height, actor->GetHeight());
  EXPECT_EQ(fourth_width, win.shadow()->width());
  EXPECT_EQ(fourth_height, win.shadow()->height());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
