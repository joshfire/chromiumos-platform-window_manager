// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/modality_handler.h"
#include "window_manager/test_lib.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/wm_ipc.h"
#include "window_manager/x11/mock_x_connection.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::vector;

namespace window_manager {

class ModalityHandlerTest : public BasicWindowManagerTest {
 protected:
  virtual void SetUp() {
    BasicWindowManagerTest::SetUp();
    handler_ = wm_->modality_handler_.get();
  }

  ModalityHandler* handler_;
};

TEST_F(ModalityHandlerTest, Basic) {
  const XAtom kStateAtom = xconn_->GetAtomOrDie("_NET_WM_STATE");
  const XAtom kModalAtom = xconn_->GetAtomOrDie("_NET_WM_STATE_MODAL");

  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  MockCompositor::ColoredBoxActor* dimming_actor =
      dynamic_cast<MockCompositor::ColoredBoxActor*>(
          handler_->dimming_actor_.get());
  CHECK(dimming_actor);

  EXPECT_FALSE(handler_->modal_window_is_focused());
  EXPECT_TRUE(dimming_actor->is_shown());
  EXPECT_DOUBLE_EQ(0, dimming_actor->opacity());

  // Create a regular toplevel window.
  XWindow toplevel_xid = CreateSimpleWindow();
  SendInitialEventsForWindow(toplevel_xid);

  // Create and map a modal transient window.  LayoutManager should focus it.
  XWindow transient_xid = CreateSimpleWindow();
  xconn_->GetWindowInfoOrDie(transient_xid)->transient_for = toplevel_xid;
  AppendAtomToProperty(transient_xid, kStateAtom, kModalAtom);
  SendInitialEventsForWindow(transient_xid);
  ASSERT_EQ(transient_xid, xconn_->focused_xid());

  // The handler should claim that a modal dialog is focused now and the
  // dimming actor should be stacked directly under the transient.
  EXPECT_TRUE(handler_->modal_window_is_focused());
  EXPECT_GT(dimming_actor->opacity(), 0);
  EXPECT_EQ(stage->GetStackingIndex(
                wm_->GetWindowOrDie(transient_xid)->GetBottomActor()) + 1,
            stage->GetStackingIndex(dimming_actor));

  // Make the transient window non-modal and notify the window manager that the
  // property changed.
  XEvent event;
  xconn_->InitClientMessageEvent(
      &event, transient_xid, kStateAtom, 0, kModalAtom, None, None, None);
  wm_->HandleEvent(&event);
  ASSERT_FALSE(wm_->GetWindowOrDie(transient_xid)->wm_state_modal());
  xconn_->InitPropertyNotifyEvent(&event, transient_xid, kStateAtom);
  wm_->HandleEvent(&event);

  // The handler should set its modal-is-focused member to false and make the
  // dimming actor invisible.
  EXPECT_FALSE(handler_->modal_window_is_focused());
  EXPECT_DOUBLE_EQ(0, dimming_actor->opacity());

  // Make the window modal again.
  xconn_->InitClientMessageEvent(
      &event, transient_xid, kStateAtom, 1, kModalAtom, None, None, None);
  wm_->HandleEvent(&event);
  ASSERT_TRUE(wm_->GetWindowOrDie(transient_xid)->wm_state_modal());
  xconn_->InitPropertyNotifyEvent(&event, transient_xid, kStateAtom);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(handler_->modal_window_is_focused());
  EXPECT_GT(dimming_actor->opacity(), 0);

  // Unmap it and check that we reset everything.
  xconn_->InitUnmapEvent(&event, transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_FALSE(handler_->modal_window_is_focused());
  EXPECT_DOUBLE_EQ(0, dimming_actor->opacity());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
