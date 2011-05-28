// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <string>
#include <vector>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/geometry.h"
#include "window_manager/shadow.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x11/mock_x_connection.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

DECLARE_bool(load_window_shapes);  // from window.cc

using std::map;
using std::string;
using std::vector;

namespace window_manager {

class WindowTest : public BasicWindowManagerTest {};

// Test that we load a window's title when it's first created (instead of
// waiting until we get a PropertyNotify event to load it).
TEST_F(WindowTest, Title) {
  XWindow xid = CreateSimpleWindow();
  const string kTitle = "foo";
  const XAtom kAtom = xconn_->GetAtomOrDie("_NET_WM_NAME");
  xconn_->SetStringProperty(xid, kAtom, kTitle);

  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  EXPECT_EQ(kTitle, win.title());

  const string kNewTitle = "bar";
  xconn_->SetStringProperty(xid, kAtom, kNewTitle);
  win.FetchAndApplyTitle();
  EXPECT_EQ(kNewTitle, win.title());

  xconn_->DeletePropertyIfExists(xid, kAtom);
  win.FetchAndApplyTitle();
  EXPECT_EQ("", win.title());
}

TEST_F(WindowTest, WindowType) {
  XWindow xid = CreateSimpleWindow();
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  EXPECT_EQ(chromeos::WM_IPC_WINDOW_UNKNOWN, win.type());

  ASSERT_TRUE(wm_->wm_ipc()->SetWindowType(
                  xid, chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL, NULL));
  EXPECT_TRUE(win.FetchAndApplyWindowType());
  EXPECT_EQ(chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL, win.type());

  ASSERT_TRUE(wm_->wm_ipc()->SetWindowType(
                  xid, chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE, NULL));
  EXPECT_TRUE(win.FetchAndApplyWindowType());
  EXPECT_EQ(chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE, win.type());
}

TEST_F(WindowTest, ChangeClient) {
  XWindow xid = CreateBasicWindow(Rect(10, 20, 30, 40));
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
  EXPECT_EQ(100, info->bounds.x);
  EXPECT_EQ(200, info->bounds.y);
  EXPECT_EQ(100, window.client_x());
  EXPECT_EQ(200, window.client_y());

  // Resize the window.
  EXPECT_TRUE(window.Resize(Size(300, 400), GRAVITY_NORTHWEST));
  EXPECT_EQ(300, info->bounds.width);
  EXPECT_EQ(400, info->bounds.height);
  EXPECT_EQ(300, window.client_width());
  EXPECT_EQ(400, window.client_height());
}

TEST_F(WindowTest, ChangeComposited) {
  XWindow xid = CreateBasicWindow(Rect(10, 20, 30, 40));
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
  XWindow xid = CreateBasicWindow(Rect(10, 20, 30, 40));

  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  info->size_hints.min_size.reset(400, 300);
  info->size_hints.max_size.reset(800, 600);
  info->size_hints.size_increment.reset(10, 5);
  info->size_hints.base_size.reset(40, 30);

  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  ASSERT_TRUE(win.FetchAndApplySizeHints());
  Size size;

  // We should get the minimum size if we request a size smaller than it.
  win.GetMaxSize(Size(300, 200), &size);
  EXPECT_EQ(Size(400, 300), size);

  // And the maximum size if we request one larger than it.
  win.GetMaxSize(Size(1000, 800), &size);
  EXPECT_EQ(Size(800, 600), size);

  // Test that the size increment hints are honored when we request a size
  // that's not equal to the base size plus some multiple of the size
  // increments.
  win.GetMaxSize(Size(609, 409), &size);
  EXPECT_EQ(Size(600, 405), size);
}

// Test WM_DELETE_WINDOW and WM_TAKE_FOCUS from ICCCM's WM_PROTOCOLS.
TEST_F(WindowTest, WmProtocols) {
  // Create a window.
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);

  // Set its WM_PROTOCOLS property to indicate that it supports both
  // message types.
  vector<int> values;
  values.push_back(static_cast<int>(xconn_->GetAtomOrDie("WM_DELETE_WINDOW")));
  values.push_back(static_cast<int>(xconn_->GetAtomOrDie("WM_TAKE_FOCUS")));
  xconn_->SetIntArrayProperty(xid,
                              xconn_->GetAtomOrDie("WM_PROTOCOLS"),  // atom
                              xconn_->GetAtomOrDie("ATOM"),          // type
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
  EXPECT_EQ(xconn_->GetAtomOrDie("WM_PROTOCOLS"), delete_msg.message_type);
  EXPECT_EQ(XConnection::kLongFormat, delete_msg.format);
  EXPECT_EQ(xconn_->GetAtomOrDie("WM_DELETE_WINDOW"), delete_msg.data.l[0]);
  EXPECT_EQ(timestamp, delete_msg.data.l[1]);

  // Now do the same thing with WM_TAKE_FOCUS.
  timestamp = 98;  // arbitrary
  info->client_messages.clear();
  EXPECT_TRUE(win.TakeFocus(timestamp));
  ASSERT_EQ(1, info->client_messages.size());
  const XClientMessageEvent& focus_msg = info->client_messages[0];
  EXPECT_EQ(xconn_->GetAtomOrDie("WM_PROTOCOLS"), focus_msg.message_type);
  EXPECT_EQ(XConnection::kLongFormat, focus_msg.format);
  EXPECT_EQ(xconn_->GetAtomOrDie("WM_TAKE_FOCUS"), focus_msg.data.l[0]);
  EXPECT_EQ(timestamp, focus_msg.data.l[1]);

  // Get rid of the window's WM_PROTOCOLS support.
  xconn_->DeletePropertyIfExists(xid, xconn_->GetAtomOrDie("WM_PROTOCOLS"));
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
  const XAtom wm_hints_atom = xconn_->GetAtomOrDie("WM_HINTS");
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
  XAtom wm_state_atom = xconn_->GetAtomOrDie("_NET_WM_STATE");
  XAtom fullscreen_atom = xconn_->GetAtomOrDie("_NET_WM_STATE_FULLSCREEN");
  XAtom max_horz_atom = xconn_->GetAtomOrDie("_NET_WM_STATE_MAXIMIZED_HORZ");
  XAtom max_vert_atom = xconn_->GetAtomOrDie("_NET_WM_STATE_MAXIMIZED_VERT");
  XAtom modal_atom = xconn_->GetAtomOrDie("_NET_WM_STATE_MODAL");

  // Create a window with its _NET_WM_STATE property set to only
  // _NET_WM_STATE_MODAL and make sure that it's correctly loaded in the
  // constructor.
  XWindow xid = CreateSimpleWindow();
  xconn_->SetIntProperty(xid,
                         wm_state_atom,                 // atom
                         xconn_->GetAtomOrDie("ATOM"),  // type
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

TEST_F(WindowTest, ChromeState) {
  const XAtom state_atom = xconn_->GetAtomOrDie("_CHROME_STATE");
  const XAtom collapsed_atom =
      xconn_->GetAtomOrDie("_CHROME_STATE_COLLAPSED_PANEL");
  // This isn't an atom that we'd actually set in the _CHROME_STATE
  // property, but we need another atom besides the collapsed one for
  // testing.
  const XAtom other_atom = xconn_->GetAtomOrDie("_NET_WM_STATE_MODAL");

  // Set the "collapsed" atom on a window.  The Window class should load
  // the initial property in its constructor.
  XWindow xid = CreateSimpleWindow();
  xconn_->SetIntProperty(xid,
                         state_atom,                    // atom
                         xconn_->GetAtomOrDie("ATOM"),  // type
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
  // Loading windows' regions is turned off by default, since it can cause
  // a pretty big memory allocation for new windows and the compositor
  // doesn't currently even support using these regions as masks, but we
  // need to enable it to test this code.
  AutoReset<bool> flag_resetter(&FLAGS_load_window_shapes, true);

  // Create a shaped window.
  int width = 10;
  int height = 5;
  XWindow xid = CreateBasicWindow(Rect(10, 20, width, height));
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  info->shape.reset(new ByteMap(Size(width, height)));
  info->shape->Clear(0xff);
  info->shape->SetRectangle(Rect(0, 0, 3, 3), 0x0);

  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  win.SetShadowType(Shadow::TYPE_RECTANGULAR);
  EXPECT_TRUE(info->shape_events_selected);
  EXPECT_TRUE(win.shaped());
  win.HandleMapNotify();
  win.ShowComposited();

  // We should have created a shadow (since SetShadowType() was called),
  // but we shouldn't be showing it (since the window is shaped).
  ASSERT_TRUE(win.shadow() != NULL);
  EXPECT_FALSE(win.shadow()->is_shown());

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
  info->shape->SetRectangle(Rect(width - 3, height - 3, 3, 3), 0x0);
  win.FetchAndApplyShape();
  EXPECT_TRUE(win.shaped());
  EXPECT_FALSE(win.shadow()->is_shown());
  ASSERT_TRUE(mock_actor->alpha_mask_bytes() != NULL);
  EXPECT_PRED_FORMAT3(BytesAreEqual,
                      info->shape->bytes(),
                      mock_actor->alpha_mask_bytes(),
                      width * height);

  // Now clear the shape and make sure that the mask is removed from the
  // actor.
  info->shape.reset();
  win.FetchAndApplyShape();
  EXPECT_FALSE(win.shaped());
  EXPECT_TRUE(mock_actor->alpha_mask_bytes() == NULL);

  // Since the shape is gone, the shadow should now be shown using the
  // opacity that was specified earlier.
  EXPECT_TRUE(win.shadow()->is_shown());
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
  const Rect kOrigBounds(0, 0, 300, 200);
  XWindow xid = CreateToplevelWindow(2, 0, kOrigBounds);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  xconn_->MapWindow(xid);
  win.HandleMapNotify();

  // Check that the actor's initial dimensions match that of the client window.
  EXPECT_EQ(kOrigBounds.size(), win.actor()->GetBounds().size());

  // After resizing the client window, the actor should still still be
  // using the original dimensions.
  const Rect kNewBounds(0, 0, 600, 400);
  win.Resize(kNewBounds.size(), GRAVITY_NORTHWEST);
  EXPECT_EQ(kOrigBounds.size(), win.actor()->GetBounds().size());

  // Now let the window know that we've seen a ConfigureNotify event with
  // the new dimensions and check that the actor is resized.
  win.HandleConfigureNotify(kNewBounds, 0);
  EXPECT_EQ(kNewBounds.size(), win.actor()->GetBounds().size());
}

// Test that pixmap actor and shadow sizes get updated correctly in
// response to ConfigureNotify events.
TEST_F(WindowTest, UpdatePixmapAndShadowSizes) {
  const int orig_width = 300, orig_height = 200;
  XWindow xid = CreateToplevelWindow(2, 0, Rect(0, 0, orig_width, orig_height));
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  win.SetShadowType(Shadow::TYPE_RECTANGULAR);

  // Resize the window once before it gets mapped, to make sure that we get
  // the updated size later after the window is mapped.
  const int second_width = orig_width + 10, second_height = orig_height + 10;
  ASSERT_TRUE(xconn_->ResizeWindow(xid, Size(second_width, second_height)));
  win.HandleConfigureNotify(info->bounds, 0);

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
  XID prev_pixmap = actor->pixmap();
  win.HandleConfigureNotify(info->bounds, 0);
  EXPECT_EQ(prev_pixmap, actor->pixmap());

  // Now act as if the window gets resized two more times, but the second
  // resize has already happened in the X server by the time that the
  // window manager receives the ConfigureNotify for the first resize.
  const int third_width = second_width + 10, third_height = second_height + 10;
  const int fourth_width = third_width + 10, fourth_height = third_height + 10;
  xconn_->ResizeWindow(xid, Size(fourth_width, fourth_height));
  win.HandleConfigureNotify(
      Rect(info->bounds.x, info->bounds.y, third_width, third_height), 0);

  // We should load the pixmap now and resize the shadow to the dimensions
  // from the final pixmap instead of the ones supplied in the event.
  EXPECT_EQ(fourth_width, actor->GetWidth());
  EXPECT_EQ(fourth_height, actor->GetHeight());
  EXPECT_EQ(fourth_width, win.shadow()->width());
  EXPECT_EQ(fourth_height, win.shadow()->height());

  // Nothing should change after we get the second ConfigureNotify.
  win.HandleConfigureNotify(info->bounds, 0);
  EXPECT_EQ(fourth_width, actor->GetWidth());
  EXPECT_EQ(fourth_height, actor->GetHeight());
  EXPECT_EQ(fourth_width, win.shadow()->width());
  EXPECT_EQ(fourth_height, win.shadow()->height());
}

// Test that we show and hide shadows under the proper conditions (note
// that a portion of this is covered by the Shape test).
TEST_F(WindowTest, ShadowVisibility) {
  XWindow xid = CreateSimpleWindow();
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);

  // First, turn on the window's shadow before it's been mapped.  Since we
  // can't draw the window yet, we shouldn't draw its shadow either.
  win.SetShadowType(Shadow::TYPE_RECTANGULAR);
  win.ShowComposited();
  ASSERT_TRUE(win.shadow() != NULL);
  EXPECT_FALSE(win.shadow()->is_shown());

  // After the window gets mapped, we should show the shadow.
  win.HandleMapNotify();
  EXPECT_TRUE(win.shadow()->is_shown());

  // If we hide the window, the shadow should also be hidden.
  win.HideComposited();
  EXPECT_FALSE(win.shadow()->is_shown());

  // We should show the shadow again after the window is shown.
  win.ShowComposited();
  EXPECT_TRUE(win.shadow()->is_shown());

  // We should destroy the Shadow object when requested.
  win.DisableShadow();
  EXPECT_TRUE(win.shadow() == NULL);
}

// Check our implementation of the _NET_WM_SYNC_REQUEST protocol defined in
// EWMH, used for synchronizing redraws by the client when the window
// manager resizes a window.
TEST_F(WindowTest, SyncRequest) {
  XWindow xid = CreateSimpleWindow();
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  xconn_->MapWindow(xid);
  win.HandleMapRequested();
  win.HandleMapNotify();
  MockCompositor::TexturePixmapActor* actor = GetMockActorForWindow(&win);

  EXPECT_TRUE(win.client_has_redrawn_after_last_resize_);
  EXPECT_EQ(geometry.bounds.size(), actor->GetBounds().size());

  // If the client doesn't support the sync request protocol, we should
  // just pretend like it's always redrawn the window immediately after a
  // resize.
  Size kFirstSize(500, 500);
  win.Resize(kFirstSize, GRAVITY_NORTHWEST);
  EXPECT_TRUE(win.client_has_redrawn_after_last_resize_);
  win.HandleConfigureNotify(Rect(geometry.bounds.position(), kFirstSize), 0);
  EXPECT_EQ(kFirstSize, actor->GetBounds().size());

  // Add the hint saying that the window supports the sync request
  // protocol, but don't actually set the property saying which counter
  // it's using.  The hint should be ignored.
  ASSERT_TRUE(
      xconn_->SetIntProperty(
          xid,
          xconn_->GetAtomOrDie("WM_PROTOCOLS"),  // atom
          xconn_->GetAtomOrDie("ATOM"),          // type
          xconn_->GetAtomOrDie("_NET_WM_SYNC_REQUEST")));
  win.FetchAndApplyWmProtocols();
  EXPECT_EQ(0, win.wm_sync_request_alarm_);

  // Now set the property and check that an alarm gets created to watch it.
  const XID counter_xid = 45;  // arbitrary
  ASSERT_TRUE(
      xconn_->SetIntProperty(
          xid,
          xconn_->GetAtomOrDie("_NET_WM_SYNC_REQUEST_COUNTER"),  // atom
          xconn_->GetAtomOrDie("CARDINAL"),                      // type
          counter_xid));
  win.FetchAndApplyWmProtocols();
  EXPECT_NE(0, win.wm_sync_request_alarm_);
  const MockXConnection::SyncCounterAlarmInfo* alarm_info =
      xconn_->GetSyncCounterAlarmInfoOrDie(win.wm_sync_request_alarm_);
  EXPECT_EQ(counter_xid, alarm_info->counter_id);

  // We should initialize the counter to a nonzero value and set the
  // alarm's trigger at the next-greatest value.
  int64_t initial_counter_value = xconn_->GetSyncCounterValueOrDie(counter_xid);
  EXPECT_NE(static_cast<int64_t>(0), initial_counter_value);
  int64_t next_counter_value = initial_counter_value + 1;
  EXPECT_EQ(next_counter_value, alarm_info->initial_trigger_value);

  // When we resize the window, we should consider the window as needing to
  // be redrawn.
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  info->client_messages.clear();
  Size kSecondSize(600, 600);
  win.Resize(kSecondSize, GRAVITY_NORTHWEST);
  EXPECT_FALSE(win.client_has_redrawn_after_last_resize_);

  // We should also abstain from getting a new pixmap in response to
  // ConfigureNotify events...
  win.HandleConfigureNotify(Rect(info->bounds.position(), kSecondSize), 0);
  EXPECT_EQ(kFirstSize, actor->GetBounds().size());

  // ... and we should send the client a message telling it to increment the
  // counter when it's done redrawing.
  ASSERT_EQ(1, info->client_messages.size());
  const XClientMessageEvent& msg = info->client_messages[0];
  EXPECT_EQ(xconn_->GetAtomOrDie("WM_PROTOCOLS"), msg.message_type);
  EXPECT_EQ(XConnection::kLongFormat, msg.format);
  EXPECT_EQ(xconn_->GetAtomOrDie("_NET_WM_SYNC_REQUEST"), msg.data.l[0]);
  // TODO: Check timestamp in l[1]?
  EXPECT_EQ(static_cast<uint32_t>(next_counter_value & 0xffffffff),
            msg.data.l[2]);
  EXPECT_EQ(static_cast<uint32_t>((next_counter_value >> 32) & 0xffffffff),
            msg.data.l[3]);

  // If we get notified that the counter is at the previous value, we
  // should ignore it.
  win.HandleSyncAlarmNotify(win.wm_sync_request_alarm_, initial_counter_value);
  EXPECT_FALSE(win.client_has_redrawn_after_last_resize_);

  // Ditto if we get notified about some alarm that we don't know about
  // (this shouldn't happen in practice).
  win.HandleSyncAlarmNotify(0, next_counter_value);
  EXPECT_FALSE(win.client_has_redrawn_after_last_resize_);

  // When we get notified that the counter has increased to the next value,
  // we should consider the window to be redrawn and fetch an updated pixmap.
  win.HandleSyncAlarmNotify(win.wm_sync_request_alarm_, next_counter_value);
  EXPECT_TRUE(win.client_has_redrawn_after_last_resize_);
  EXPECT_EQ(kSecondSize, actor->GetBounds().size());

  // If we somehow get notified that the window has been redrawn before we
  // get the ConfigureNotify, reset the pixmap immediately.
  Size kThirdSize(700, 700);
  win.Resize(kThirdSize, GRAVITY_NORTHWEST);
  win.HandleSyncAlarmNotify(win.wm_sync_request_alarm_,
                            win.current_wm_sync_num_);
  EXPECT_EQ(kThirdSize, actor->GetBounds().size());
}

// Test that we wait to fetch pixmaps for newly-created windows until the
// client tells us that they've been painted.
TEST_F(WindowTest, DeferFetchingPixmapUntilPainted) {
  // Create a window and configure it to use _NET_WM_SYNC_REQUEST.
  XWindow xid = CreateSimpleWindow();
  ConfigureWindowForSyncRequestProtocol(xid);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  xconn_->MapWindow(xid);
  win.HandleMapRequested();

  // Window::HandleMapRequested() should send a message to the client
  // asking it to sync after painting the window, along with a synthetic
  // ConfigureNotify event.
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);

  ASSERT_EQ(1, info->client_messages.size());
  const XClientMessageEvent& msg = info->client_messages[0];
  EXPECT_EQ(xconn_->GetAtomOrDie("WM_PROTOCOLS"), msg.message_type);
  EXPECT_EQ(XConnection::kLongFormat, msg.format);
  EXPECT_EQ(xconn_->GetAtomOrDie("_NET_WM_SYNC_REQUEST"), msg.data.l[0]);

  ASSERT_EQ(1, info->configure_notify_events.size());
  const XConfigureEvent& conf_notify = info->configure_notify_events[0];
  EXPECT_EQ(info->bounds.x, conf_notify.x);
  EXPECT_EQ(info->bounds.y, conf_notify.y);
  EXPECT_EQ(info->bounds.width, conf_notify.width);
  EXPECT_EQ(info->bounds.height, conf_notify.height);
  EXPECT_EQ(info->border_width, conf_notify.border_width);
  // Don't bother checking the stacking here.  We never registered this
  // window with WindowManager (we don't want event consumers messing
  // around with it), so the Window class won't be able to query the
  // correct stacking position from WindowManager when it sends the
  // synthetic event.
  EXPECT_EQ(0, conf_notify.override_redirect);

  // We should hold off on fetching the pixmap in response to a MapNotify
  // event if we haven't received notice that the window has been painted.
  win.HandleMapNotify();
  EXPECT_EQ(0, win.pixmap_);
  EXPECT_FALSE(win.has_initial_pixmap());

  // After getting notice, we should fetch the pixmap.
  win.HandleSyncAlarmNotify(win.wm_sync_request_alarm_,
                            win.current_wm_sync_num_);
  EXPECT_NE(0, win.pixmap_);
  EXPECT_TRUE(win.has_initial_pixmap());
}

// Test that we load the WM_CLIENT_MACHINE property, containing the
// hostname of the machine where the client is running.
TEST_F(WindowTest, ClientHostname) {
  const XAtom client_machine_atom = xconn_->GetAtomOrDie("WM_CLIENT_MACHINE");

  string hostname = "a.example.com";
  XWindow xid = CreateSimpleWindow();
  xconn_->SetStringProperty(xid, client_machine_atom, hostname);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  EXPECT_EQ(hostname, win.client_hostname());

  hostname = "b.example.com";
  xconn_->SetStringProperty(xid, client_machine_atom, hostname);
  win.FetchAndApplyWmClientMachine();
  EXPECT_EQ(hostname, win.client_hostname());

  xconn_->DeletePropertyIfExists(xid, client_machine_atom);
  win.FetchAndApplyWmClientMachine();
  EXPECT_EQ("", win.client_hostname());
}

// Test that we load the _NET_WM_PID property, containing the client's PID.
TEST_F(WindowTest, ClientPid) {
  const XAtom pid_atom = xconn_->GetAtomOrDie("_NET_WM_PID");
  const XAtom cardinal_atom = xconn_->GetAtomOrDie("CARDINAL");

  int pid = 123;
  XWindow xid = CreateSimpleWindow();
  xconn_->SetIntProperty(xid, pid_atom, cardinal_atom, pid);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  EXPECT_EQ(pid, win.client_pid());

  pid = 5436;
  xconn_->SetIntProperty(xid, pid_atom, cardinal_atom, pid);
  win.FetchAndApplyWmPid();
  EXPECT_EQ(pid, win.client_pid());

  xconn_->DeletePropertyIfExists(xid, pid_atom);
  win.FetchAndApplyWmPid();
  EXPECT_EQ(-1, win.client_pid());
}

// Test that we're able to send messages per the _NET_WM_PING protocol.
TEST_F(WindowTest, SendPingMessage) {
  XWindow xid = CreateSimpleWindow();
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);

  // SendPing() should just fail without doing anything if the window
  // hasn't told us that it supports the protocol.
  XTime timestamp = 123;
  info->client_messages.clear();
  EXPECT_FALSE(win.SendPing(timestamp));
  EXPECT_TRUE(info->client_messages.empty());

  // Otherwise, we should send a client message as described in the spec.
  AppendAtomToProperty(xid,
                       xconn_->GetAtomOrDie("WM_PROTOCOLS"),
                       xconn_->GetAtomOrDie("_NET_WM_PING"));
  info->client_messages.clear();
  win.FetchAndApplyWmProtocols();
  EXPECT_TRUE(win.SendPing(timestamp));

  ASSERT_EQ(1, info->client_messages.size());
  const XClientMessageEvent& msg = info->client_messages[0];
  EXPECT_EQ(xconn_->GetAtomOrDie("WM_PROTOCOLS"), msg.message_type);
  EXPECT_EQ(XConnection::kLongFormat, msg.format);
  EXPECT_EQ(xconn_->GetAtomOrDie("_NET_WM_PING"), msg.data.l[0]);
  EXPECT_EQ(timestamp, msg.data.l[1]);
  EXPECT_EQ(xid, msg.data.l[2]);
  EXPECT_EQ(0, msg.data.l[3]);
  EXPECT_EQ(0, msg.data.l[4]);
}

// Check that we avoid a race that used to result in us displaying an
// incorrectly-sized shadow when an override-redirect window would be
// mapped and then immediately resized around the same time that we were
// enabling its shadow.  See http://crosbug.com/7227.
TEST_F(WindowTest, ShadowSizeRace) {
  // Create a 1x1 override-redirect window.
  const Size kOrigSize(1, 1);
  XWindow xid = xconn_->CreateWindow(xconn_->GetRootWindow(),
                                     Rect(Point(0, 0), kOrigSize),
                                     true,   // override_redirect
                                     false,  // input_only
                                     0,      // event_mask
                                     0);     // visual
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, true, geometry);

  // Map the window and then resize it to 200x400.
  xconn_->MapWindow(xid);
  const Size kNewSize(200, 400);
  xconn_->ResizeWindow(xid, kNewSize);

  // Let the Window object know about the MapNotify.  Since the window has
  // already been resized in the X server at this point, the actor should
  // get the 200x400 pixmap.
  win.HandleMapNotify();
  EXPECT_EQ(kNewSize, win.actor()->GetBounds().size());

  // Turn on the shadow while we're in this brief state where we have a
  // 200x400 actor but have only heard about the 1x1 size from the X
  // server.  The shadow should take the actor's size.
  win.SetShadowType(Shadow::TYPE_RECTANGULAR);
  EXPECT_EQ(kNewSize, win.shadow()->bounds().size());

  // Now send the ConfigureNotify and check that nothing changes.
  win.HandleConfigureNotify(Rect(Point(0, 0), kNewSize), 0);
  EXPECT_EQ(kNewSize, win.actor()->GetBounds().size());
}

// Test that when we ask a window to simultaneously move and resize itself (that
// is, we request a resize with non-northwest gravity), the actor's position and
// size are updated atomically, rather than its position getting changed
// immediately and the resize only happening after we fetch the new pixmap.
TEST_F(WindowTest, SimultaneousMoveAndResize) {
  // Create and map a window.
  const Rect kOrigBounds(100, 150, 300, 250);
  XWindow xid = xconn_->CreateWindow(xconn_->GetRootWindow(),
                                     kOrigBounds,
                                     false,  // override_redirect
                                     false,  // input_only
                                     0,      // event_mask
                                     0);     // visual
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  ASSERT_TRUE(xconn_->MapWindow(xid));
  win.HandleMapNotify();
  win.ShowComposited();

  // The client window and the actor should both have the requested bounds.
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  EXPECT_EQ(kOrigBounds, info->bounds);
  MockCompositor::TexturePixmapActor* actor = GetMockActorForWindow(&win);
  EXPECT_EQ(kOrigBounds, actor->GetBounds());
  EXPECT_EQ(kOrigBounds.x, win.composited_x());
  EXPECT_EQ(kOrigBounds.y, win.composited_y());

  // Now make the window 50 pixels wider and taller with southeast gravity.
  // In other words, its origin should also move 50 pixels up and to the left.
  const Rect kNewBounds(50, 100, 350, 300);
  win.Resize(kNewBounds.size(), GRAVITY_SOUTHEAST);

  // A request should've been sent to the X server asking for the new bounds, so
  // the client window should be resized.  The actor should still be at the old
  // size (since we can't fetch its bitmap yet) and also at the old position (so
  // we can make the move and resize happen atomically onscreen later).
  EXPECT_EQ(kNewBounds, info->bounds);
  EXPECT_EQ(kOrigBounds, actor->GetBounds());
  EXPECT_EQ(kNewBounds.x, win.composited_x());
  EXPECT_EQ(kNewBounds.y, win.composited_y());

  // After we've received notification that the new pixmap is available, the
  // actor should be both resized and moved to the requested position.
  win.HandleConfigureNotify(kNewBounds, 0);
  EXPECT_EQ(kNewBounds, actor->GetBounds());
  EXPECT_EQ(kNewBounds.x, win.composited_x());
  EXPECT_EQ(kNewBounds.y, win.composited_y());

  // Move the actor to a completely different position.
  const Point kCompositedPosition(500, 600);
  win.MoveComposited(kCompositedPosition.x, kCompositedPosition.y, 0);
  EXPECT_EQ(Rect(kCompositedPosition, kNewBounds.size()), actor->GetBounds());
  EXPECT_EQ(kCompositedPosition.x, win.composited_x());
  EXPECT_EQ(kCompositedPosition.y, win.composited_y());

  // Now resize the window back to its old size, again with southeast gravity.
  // The actor shouldn't move, but we should update the |composited_x| and
  // |composited_y| fields.
  win.Resize(kOrigBounds.size(), GRAVITY_SOUTHEAST);
  EXPECT_EQ(kOrigBounds, info->bounds);
  EXPECT_EQ(Rect(kCompositedPosition, kNewBounds.size()), actor->GetBounds());
  const Point kOffsetCompositedPosition(
      kCompositedPosition.x + (kNewBounds.width - kOrigBounds.width),
      kCompositedPosition.y + (kNewBounds.height - kOrigBounds.height));
  EXPECT_EQ(kOffsetCompositedPosition.x, win.composited_x());
  EXPECT_EQ(kOffsetCompositedPosition.y, win.composited_y());

  // After getting notification about the pixmap, the actor should be resized
  // and moved to the new position.
  win.HandleConfigureNotify(kOrigBounds, 0);
  EXPECT_EQ(Rect(kOffsetCompositedPosition, kOrigBounds.size()),
            actor->GetBounds());

  // Move the composited window back to the client window's position and scale
  // it to 50% of its original size.
  win.MoveComposited(kOrigBounds.x, kOrigBounds.y, 0);
  const double kCompositedScale = 0.5;
  win.ScaleComposited(kCompositedScale, kCompositedScale, 0);

  // Resize the client again.  The amount that the composited window is moved
  // should be scaled by its scaling factor.
  win.Resize(kNewBounds.size(), GRAVITY_SOUTHEAST);
  const Point kScaledCompositedPosition(
      kOrigBounds.x + kCompositedScale * (kNewBounds.x - kOrigBounds.x),
      kOrigBounds.y + kCompositedScale * (kNewBounds.y - kOrigBounds.y));
  EXPECT_EQ(kScaledCompositedPosition.x, win.composited_x());
  EXPECT_EQ(kScaledCompositedPosition.y, win.composited_y());

  win.HandleConfigureNotify(kNewBounds, 0);
  EXPECT_EQ(Rect(kScaledCompositedPosition, kNewBounds.size()),
            actor->GetBounds());
}

// Exercises the new interface for managing both X and composited windows
// simultaneously (SetVisibility() and Move()).
TEST_F(WindowTest, SetVisibility) {
  // Create and map a window.
  const Rect kOrigBounds(100, 150, 300, 250);
  XWindow xid = xconn_->CreateWindow(xconn_->GetRootWindow(),
                                     kOrigBounds,
                                     false,  // override_redirect
                                     false,  // input_only
                                     0,      // event_mask
                                     0);     // visual
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  ASSERT_TRUE(xconn_->MapWindow(xid));
  win.HandleMapNotify();

  // In the default state, we should leave the X window at its original
  // position and hide the composited window.
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  EXPECT_EQ(kOrigBounds, info->bounds);
  MockCompositor::TexturePixmapActor* actor = GetMockActorForWindow(&win);
  EXPECT_EQ(kOrigBounds, actor->GetBounds());
  EXPECT_FALSE(actor->is_shown());

  // With VISIBILITY_SHOWN, the X and composited windows should be in the same
  // place and the composited window should be shown.
  win.SetVisibility(Window::VISIBILITY_SHOWN);
  EXPECT_EQ(kOrigBounds, info->bounds);
  EXPECT_EQ(kOrigBounds, actor->GetBounds());
  EXPECT_TRUE(actor->is_shown());

  // When we call Move(), both windows should be moved.
  const Point kNewPosition(200, 300);
  win.Move(kNewPosition, 0);
  EXPECT_EQ(kNewPosition, info->bounds.position());
  EXPECT_EQ(kNewPosition, actor->GetBounds().position());
  EXPECT_TRUE(actor->is_shown());

  // With VISIBILITY_SHOWN_NO_INPUT, the X window should be moved offscreen.
  const Point kOffscreenPosition(Window::kOffscreenX, Window::kOffscreenY);
  win.SetVisibility(Window::VISIBILITY_SHOWN_NO_INPUT);
  EXPECT_EQ(kOffscreenPosition, info->bounds.position());
  EXPECT_EQ(kNewPosition, actor->GetBounds().position());
  EXPECT_TRUE(actor->is_shown());

  // The X window should stay offscreen when we call Move().
  win.Move(kOrigBounds.position(), 0);
  EXPECT_EQ(kOffscreenPosition, info->bounds.position());
  EXPECT_EQ(kOrigBounds.position(), actor->GetBounds().position());
  EXPECT_TRUE(actor->is_shown());

  // With VISIBILITY_HIDDEN, the composited window should additionally be
  // hidden.
  win.SetVisibility(Window::VISIBILITY_HIDDEN);
  EXPECT_EQ(kOffscreenPosition, info->bounds.position());
  EXPECT_EQ(kOrigBounds.position(), actor->GetBounds().position());
  EXPECT_FALSE(actor->is_shown());

  // The composited window should get moved but stay hidden when we call Move().
  win.Move(kNewPosition, 0);
  EXPECT_EQ(kOffscreenPosition, info->bounds.position());
  EXPECT_EQ(kNewPosition, actor->GetBounds().position());
  EXPECT_FALSE(actor->is_shown());

  // After setting the visibility to VISIBILITY_SHOWN, the X window should be
  // moved back to the same position as the composited window.
  win.SetVisibility(Window::VISIBILITY_SHOWN);
  EXPECT_EQ(kNewPosition, info->bounds.position());
  EXPECT_EQ(kNewPosition, actor->GetBounds().position());
  EXPECT_TRUE(actor->is_shown());

  // Scaling the composited window should automatically move the X window
  // offscreen, since mouse events wouldn't get transformed correctly if it
  // stayed onscreen.
  win.ScaleComposited(0.5, 1.0, 0);
  EXPECT_EQ(kOffscreenPosition, info->bounds.position());
  EXPECT_EQ(kNewPosition, actor->GetBounds().position());

  // Check that the X window gets moved back when we restore the scale.
  win.ScaleComposited(1.0, 1.0, 0);
  EXPECT_EQ(kNewPosition, info->bounds.position());
  EXPECT_EQ(kNewPosition, actor->GetBounds().position());

  // Similarly, setting the opacity to 0 should move the X window offscreen.
  win.SetCompositedOpacity(0.0, 0);
  EXPECT_EQ(kOffscreenPosition, info->bounds.position());
  EXPECT_EQ(kNewPosition, actor->GetBounds().position());

  // The X window should get moved back when we make the window partially
  // visible.
  win.SetCompositedOpacity(0.5, 0);
  EXPECT_EQ(kNewPosition, info->bounds.position());
  EXPECT_EQ(kNewPosition, actor->GetBounds().position());
}

TEST_F(WindowTest, SetUpdateClientPositionForMoves) {
  // Create and map a window.
  const Rect kOrigBounds(100, 150, 300, 250);
  XWindow xid = xconn_->CreateWindow(xconn_->GetRootWindow(),
                                     kOrigBounds,
                                     false,  // override_redirect
                                     false,  // input_only
                                     0,      // event_mask
                                     0);     // visual
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);
  win.SetVisibility(Window::VISIBILITY_SHOWN);
  ASSERT_TRUE(xconn_->MapWindow(xid));
  win.HandleMapNotify();

  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);
  EXPECT_EQ(kOrigBounds, info->bounds);
  MockCompositor::TexturePixmapActor* actor = GetMockActorForWindow(&win);
  EXPECT_EQ(kOrigBounds, actor->GetBounds());

  const Point kNewPosition(200, 300);
  win.SetUpdateClientPositionForMoves(false);
  win.Move(kNewPosition, 0);
  EXPECT_EQ(kOrigBounds.position(), info->bounds.position());
  EXPECT_EQ(kNewPosition, actor->GetBounds().position());

  win.SetUpdateClientPositionForMoves(true);
  EXPECT_EQ(kNewPosition, info->bounds.position());
  EXPECT_EQ(kNewPosition, actor->GetBounds().position());
}

TEST_F(WindowTest, FreezeUpdates) {
  XWindow xid = CreateSimpleWindow();
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid, &geometry));
  Window win(wm_.get(), xid, false, geometry);

  // Set the _CHROME_FREEZE_UPDATES property on the window before mapping it.
  // We should avoid fetching its pixmap.
  const XAtom kAtom = xconn_->GetAtomOrDie("_CHROME_FREEZE_UPDATES");
  ASSERT_TRUE(xconn_->SetIntProperty(xid, kAtom, kAtom, 1));
  win.HandleFreezeUpdatesPropertyChange(true);
  win.HandleMapRequested();
  xconn_->MapWindow(xid);
  win.HandleMapNotify();
  EXPECT_EQ(0, win.pixmap_);
  EXPECT_FALSE(win.has_initial_pixmap());

  // After the property is removed, we should fetch the pixmap.
  ASSERT_TRUE(xconn_->DeletePropertyIfExists(xid, kAtom));
  win.HandleFreezeUpdatesPropertyChange(false);
  EXPECT_NE(0, win.pixmap_);
  EXPECT_TRUE(win.has_initial_pixmap());

  // Create a second window.  Configure it for _NET_WM_SYNC_REQUEST and set the
  // _CHROME_FREEZE_UPDATES property before the Window class hears about it.
  XWindow xid2 = CreateSimpleWindow();
  ConfigureWindowForSyncRequestProtocol(xid2);
  ASSERT_TRUE(xconn_->SetIntProperty(xid2, kAtom, kAtom, 1));
  ASSERT_TRUE(xconn_->GetWindowGeometry(xid2, &geometry));
  Window win2(wm_.get(), xid2, false, geometry);

  // Map the window and check that we don't load its pixmap.
  win2.HandleMapRequested();
  xconn_->MapWindow(xid2);
  win2.HandleMapNotify();
  EXPECT_EQ(0, win2.pixmap_);
  EXPECT_FALSE(win2.has_initial_pixmap());

  // Update the sync counter.  We should still avoid loading the pixmap, since
  // the freeze-updates property is still set.
  win2.HandleSyncAlarmNotify(win2.wm_sync_request_alarm_,
                             win2.current_wm_sync_num_);
  EXPECT_EQ(0, win2.pixmap_);
  EXPECT_FALSE(win2.has_initial_pixmap());

  // After the property is removed, the pixmap should finally be loaded.
  ASSERT_TRUE(xconn_->DeletePropertyIfExists(xid2, kAtom));
  win2.HandleFreezeUpdatesPropertyChange(false);
  EXPECT_NE(0, win2.pixmap_);
  EXPECT_TRUE(win2.has_initial_pixmap());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
