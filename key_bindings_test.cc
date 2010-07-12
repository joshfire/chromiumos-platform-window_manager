// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

extern "C" {
#include <X11/keysym.h>
}
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <vector>

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/stl_util-inl.h"
#include "base/string_util.h"
#include "window_manager/callback.h"
#include "window_manager/key_bindings.h"
#include "window_manager/mock_x_connection.h"
#include "window_manager/test_lib.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::string;
using std::vector;

namespace window_manager {

struct TestAction {
  TestAction(const string& name_param, KeyBindings* bindings_arg)
      : name(name_param),
        begin_call_count(0),
        repeat_call_count(0),
        end_call_count(0),
        last_event_time(0),
        bindings(bindings_arg) {
  }
  ~TestAction() {
  }

  void Reset() {
    begin_call_count = 0;
    repeat_call_count = 0;
    end_call_count = 0;
  }

  void inc_begin_call_count() {
    ++begin_call_count;
    last_event_time = bindings->current_event_time();
  }
  void inc_repeat_call_count() {
    ++repeat_call_count;
    last_event_time = bindings->current_event_time();
  }
  void inc_end_call_count() {
    ++end_call_count;
    last_event_time = bindings->current_event_time();
  }

  string name;
  int begin_call_count;
  int repeat_call_count;
  int end_call_count;
  XTime last_event_time;
  KeyBindings* bindings;
};

class KeyBindingsTest : public ::testing::Test {
 protected:
  virtual void SetUp() {
    xconn_.reset(new MockXConnection());
    bindings_.reset(new KeyBindings(xconn_.get()));
    for (int i = 0; i < kNumActions; ++i) {
      actions_.push_back(new TestAction(StringPrintf("action_%d", i),
                                        bindings_.get()));
    }
  }
  virtual void TearDown() {
    STLDeleteElements(&actions_);
  }

  void AddAction(int number,
                 bool use_begin_closure,
                 bool use_repeat_closure,
                 bool use_end_closure) {
    CHECK(number < kNumActions);
    TestAction* const action = actions_[number];
    bindings_->AddAction(
        action->name,
        use_begin_closure ?
        NewPermanentCallback(action, &TestAction::inc_begin_call_count) : NULL,
        use_repeat_closure ?
        NewPermanentCallback(action, &TestAction::inc_repeat_call_count) : NULL,
        use_end_closure ?
        NewPermanentCallback(action, &TestAction::inc_end_call_count) : NULL);
  }

  void AddAllActions() {
    for (int i = 0; i < kNumActions; ++i) {
      AddAction(i, true, true, true);
    }
  }

  scoped_ptr<window_manager::MockXConnection> xconn_;
  scoped_ptr<window_manager::KeyBindings> bindings_;
  vector<TestAction*> actions_;

  static const int kNumActions = 10;
};

TEST_F(KeyBindingsTest, Basic) {
  const KeyCode keycode_e = 1, keycode_t = 2;
  xconn_->AddKeyMapping(keycode_e, XK_e);
  xconn_->AddKeyMapping(keycode_t, XK_t);

  // Action 0: Requests begin, end, and repeat callbacks.
  AddAction(0, true, true, true);
  bindings_->AddBinding(KeyBindings::KeyCombo(XK_e, KeyBindings::kControlMask),
                        actions_[0]->name);

  // -- Combo press for action 0
  XTime event_time = 10;
  EXPECT_EQ(0, bindings_->current_event_time());
  EXPECT_TRUE(bindings_->HandleKeyPress(keycode_e, KeyBindings::kControlMask,
                                        event_time++));
  EXPECT_EQ(0, bindings_->current_event_time());
  EXPECT_EQ(10, actions_[0]->last_event_time);
  EXPECT_EQ(1, actions_[0]->begin_call_count);
  EXPECT_EQ(0, actions_[0]->repeat_call_count);
  EXPECT_EQ(0, actions_[0]->end_call_count);

  // -- Combo repeats for action 0
  EXPECT_TRUE(bindings_->HandleKeyPress(keycode_e, KeyBindings::kControlMask,
                                        event_time++));
  EXPECT_EQ(1, actions_[0]->begin_call_count);
  EXPECT_EQ(1, actions_[0]->repeat_call_count);
  EXPECT_EQ(0, actions_[0]->end_call_count);
  EXPECT_TRUE(bindings_->HandleKeyPress(keycode_e, KeyBindings::kControlMask,
                                        event_time++));
  EXPECT_EQ(1, actions_[0]->begin_call_count);
  EXPECT_EQ(2, actions_[0]->repeat_call_count);
  EXPECT_EQ(0, actions_[0]->end_call_count);

  // -- Combo release for action 0
  bindings_->HandleKeyRelease(
      keycode_e, KeyBindings::kControlMask, event_time++);
  EXPECT_EQ(1, actions_[0]->begin_call_count);
  EXPECT_EQ(2, actions_[0]->repeat_call_count);
  EXPECT_EQ(1, actions_[0]->end_call_count);

  // -- Unregistered combo presses.
  EXPECT_FALSE(bindings_->HandleKeyPress(keycode_t, KeyBindings::kControlMask,
                                         event_time++));
  EXPECT_FALSE(bindings_->HandleKeyRelease(keycode_t, KeyBindings::kControlMask,
                                           event_time++));
  EXPECT_FALSE(bindings_->HandleKeyPress(keycode_e, KeyBindings::kShiftMask,
                                         event_time++));
  EXPECT_FALSE(bindings_->HandleKeyRelease(keycode_e, KeyBindings::kShiftMask,
                                           event_time++));
  EXPECT_FALSE(bindings_->HandleKeyPress(keycode_e, 0, event_time++));
  EXPECT_FALSE(bindings_->HandleKeyRelease(keycode_e, 0, event_time++));
  EXPECT_EQ(1, actions_[0]->begin_call_count);
  EXPECT_EQ(2, actions_[0]->repeat_call_count);
  EXPECT_EQ(1, actions_[0]->end_call_count);
}

TEST_F(KeyBindingsTest, ModifierKey) {
  const KeyCode keycode = 1;
  xconn_->AddKeyMapping(keycode, XK_Alt_L);

  // Action 0: Requests begin and end callbacks.
  AddAction(0, true, true, true);

  // Bind a modifier as the main key. Upon release, the modifiers mask will
  // also contain the modifier, so make sure that doesn't mess things up.
  KeyBindings::KeyCombo combo(XK_Alt_L, KeyBindings::kControlMask);
  bindings_->AddBinding(combo, actions_[0]->name);

  // -- Combo press for action 0
  XTime event_time = 10;
  EXPECT_TRUE(bindings_->HandleKeyPress(keycode, KeyBindings::kControlMask,
                                        event_time++));
  EXPECT_EQ(1, actions_[0]->begin_call_count);
  EXPECT_EQ(0, actions_[0]->end_call_count);

  // -- Combo release for action 0
  // NOTE: We add in the modifier mask for the key itself.
  bindings_->HandleKeyRelease(
      keycode, KeyBindings::kControlMask | KeyBindings::kAltMask,
      event_time++);
  EXPECT_EQ(1, actions_[0]->begin_call_count);
  EXPECT_EQ(1, actions_[0]->end_call_count);
}

TEST_F(KeyBindingsTest, NullCallbacks) {
  const KeyCode keycode_e = 1, keycode_b = 2, keycode_r = 3;
  xconn_->AddKeyMapping(keycode_e, XK_e);
  xconn_->AddKeyMapping(keycode_b, XK_b);
  xconn_->AddKeyMapping(keycode_r, XK_r);

  // Action 0: Requests end callback only.
  AddAction(0, false, false, true);
  bindings_->AddBinding(KeyBindings::KeyCombo(XK_e, KeyBindings::kControlMask),
                        actions_[0]->name);

  // Action 1: Requests begin callback only.
  AddAction(1, true, false, false);
  bindings_->AddBinding(KeyBindings::KeyCombo(XK_b, KeyBindings::kControlMask),
                        actions_[1]->name);

  // Action 2: Requests repeat callback only.
  AddAction(2, false, true, false);
  bindings_->AddBinding(KeyBindings::KeyCombo(XK_r, KeyBindings::kControlMask),
                        actions_[2]->name);

  // -- Combo press for action 0
  XTime event_time = 10;
  EXPECT_FALSE(bindings_->HandleKeyPress(keycode_e, KeyBindings::kControlMask,
                                         event_time++));
  EXPECT_EQ(0, actions_[0]->begin_call_count);
  EXPECT_EQ(0, actions_[0]->repeat_call_count);
  EXPECT_EQ(0, actions_[0]->end_call_count);

  // -- Combo repeat for action 0
  EXPECT_FALSE(bindings_->HandleKeyPress(keycode_e, KeyBindings::kControlMask,
                                         event_time++));
  EXPECT_EQ(0, actions_[0]->begin_call_count);
  EXPECT_EQ(0, actions_[0]->repeat_call_count);
  EXPECT_EQ(0, actions_[0]->end_call_count);

  // -- Combo release for action 0
  EXPECT_TRUE(bindings_->HandleKeyRelease(keycode_e, KeyBindings::kControlMask,
                                          event_time++));
  EXPECT_EQ(0, actions_[0]->begin_call_count);
  EXPECT_EQ(0, actions_[0]->repeat_call_count);
  EXPECT_EQ(1, actions_[0]->end_call_count);

  // -- Combo press for action 1
  EXPECT_TRUE(bindings_->HandleKeyPress(keycode_b, KeyBindings::kControlMask,
                                        event_time++));
  EXPECT_EQ(1, actions_[1]->begin_call_count);
  EXPECT_EQ(0, actions_[1]->repeat_call_count);
  EXPECT_EQ(0, actions_[1]->end_call_count);

  // -- Combo repeat for action 1
  EXPECT_FALSE(bindings_->HandleKeyPress(keycode_b, KeyBindings::kControlMask,
                                         event_time++));
  EXPECT_EQ(1, actions_[1]->begin_call_count);
  EXPECT_EQ(0, actions_[1]->repeat_call_count);
  EXPECT_EQ(0, actions_[1]->end_call_count);

  // -- Combo release for action 1
  EXPECT_FALSE(bindings_->HandleKeyRelease(keycode_b, KeyBindings::kControlMask,
                                           event_time++));
  EXPECT_EQ(1, actions_[1]->begin_call_count);
  EXPECT_EQ(0, actions_[1]->repeat_call_count);
  EXPECT_EQ(0, actions_[1]->end_call_count);

  // -- Combo press for action 2
  EXPECT_FALSE(bindings_->HandleKeyPress(keycode_r, KeyBindings::kControlMask,
                                         event_time++));
  EXPECT_EQ(0, actions_[2]->begin_call_count);
  EXPECT_EQ(0, actions_[2]->repeat_call_count);
  EXPECT_EQ(0, actions_[2]->end_call_count);

  // -- Combo repeat for action 2
  EXPECT_TRUE(bindings_->HandleKeyPress(keycode_r, KeyBindings::kControlMask,
                                        event_time++));
  EXPECT_EQ(0, actions_[2]->begin_call_count);
  EXPECT_EQ(1, actions_[2]->repeat_call_count);
  EXPECT_EQ(0, actions_[2]->end_call_count);

  // -- Combo release for action 2
  EXPECT_FALSE(bindings_->HandleKeyRelease(keycode_r, KeyBindings::kControlMask,
                                           event_time++));
  EXPECT_EQ(0, actions_[2]->begin_call_count);
  EXPECT_EQ(1, actions_[2]->repeat_call_count);
  EXPECT_EQ(0, actions_[2]->end_call_count);
}

TEST_F(KeyBindingsTest, InvalidOperations) {
  xconn_->AddKeyMapping(1, XK_e);

  EXPECT_FALSE(bindings_->RemoveAction("nonexistant"));
  EXPECT_FALSE(bindings_->RemoveBinding(KeyBindings::KeyCombo(XK_e)));
  EXPECT_FALSE(bindings_->AddBinding(KeyBindings::KeyCombo(XK_e),
                                     "nonexistant"));

  EXPECT_TRUE(bindings_->AddAction("test", NULL, NULL, NULL));
  EXPECT_FALSE(bindings_->AddAction("test", NULL, NULL, NULL));  // Double add

  KeyBindings::KeyCombo combo(XK_e);
  EXPECT_TRUE(bindings_->AddBinding(combo, "test"));
  EXPECT_FALSE(bindings_->AddBinding(combo, "test"));  // Double add
}

TEST_F(KeyBindingsTest, ManyActionsAndBindings) {
  AddAllActions();

  // Add multiple key bindings for each action.
  const int kBindingsPerAction = 4;
  for (int i = 0; i < kBindingsPerAction; ++i) {
    for (int j = 0; j < kNumActions; ++j) {
      KeySym keysym = XK_a + (i * kNumActions) + j;
      xconn_->AddKeyMapping(i * kNumActions + j + 1, keysym);
      EXPECT_TRUE(
          bindings_->AddBinding(KeyBindings::KeyCombo(keysym),
                                actions_[j]->name));
    }
  }

  // Test key combos across all bindings.
  const int kNumActivates = 2;
  XTime event_time = 10;
  for (int i = 0; i < kBindingsPerAction; ++i) {
    for (int j = 0; j < kNumActions; ++j) {
      const KeyCode keycode = i * kNumActions + j + 1;
      for (int k = 0; k < kNumActivates; ++k) {
        const int count = i * kNumActivates + k;
        EXPECT_TRUE(bindings_->HandleKeyPress(keycode, 0,
                                              event_time++));    // Press
        EXPECT_EQ(count + 1, actions_[j]->begin_call_count);
        EXPECT_EQ(count, actions_[j]->repeat_call_count);
        EXPECT_EQ(count, actions_[j]->end_call_count);
        EXPECT_TRUE(bindings_->HandleKeyPress(keycode, 0,
                                              event_time++));    // Repeat
        EXPECT_EQ(count + 1, actions_[j]->begin_call_count);
        EXPECT_EQ(count + 1, actions_[j]->repeat_call_count);
        EXPECT_EQ(count, actions_[j]->end_call_count);
        EXPECT_TRUE(bindings_->HandleKeyRelease(keycode, 0,
                                                event_time++));  // Release
        EXPECT_EQ(count + 1, actions_[j]->begin_call_count);
        EXPECT_EQ(count + 1, actions_[j]->repeat_call_count);
        EXPECT_EQ(count + 1, actions_[j]->end_call_count);
      }
    }
  }

  // Remove half of the key bindings
  for (int i = 0; i < (kBindingsPerAction / 2); ++i) {
    for (int j = 0; j < kNumActions; ++j) {
      KeySym keysym = XK_a + (i * kNumActions) + j;
      EXPECT_TRUE(bindings_->RemoveBinding(KeyBindings::KeyCombo(keysym)));
    }
  }

  // Test all key combos again, but half the bindings have been removed.
  for (int i = 0; i < kNumActions; ++i) {
    actions_[i]->Reset();
  }
  for (int i = 0; i < kBindingsPerAction; ++i) {
    for (int j = 0; j < kNumActions; ++j) {
      const KeyCode keycode = i * kNumActions + j + 1;
      const bool has_binding = (i >= (kBindingsPerAction / 2));
      EXPECT_EQ(has_binding, bindings_->HandleKeyPress(keycode, 0,
                                                       event_time++));
      EXPECT_EQ(has_binding, bindings_->HandleKeyRelease(keycode, 0,
                                                         event_time++));
      if (has_binding) {
        EXPECT_GT(actions_[j]->begin_call_count, 0);
        EXPECT_GT(actions_[j]->end_call_count, 0);
      } else {
        // These key bindings were removed.
        EXPECT_EQ(0, actions_[j]->begin_call_count);
        EXPECT_EQ(0, actions_[j]->end_call_count);
      }
    }
  }

  // Remove all of the actions; key combos should be cleaned up automatically.
  for (int i = 0; i < kNumActions; ++i) {
    actions_[i]->Reset();
    EXPECT_TRUE(bindings_->RemoveAction(actions_[i]->name));
  }
  for (int i = 0; i < kBindingsPerAction; ++i) {
    for (int j = 0; j < kNumActions; ++j) {
      const KeyCode keycode = i * kNumActions + j + 1;
      EXPECT_FALSE(bindings_->HandleKeyPress(keycode, 0, event_time++));
      EXPECT_FALSE(bindings_->HandleKeyRelease(keycode, 0, event_time++));
      EXPECT_EQ(0, actions_[j]->begin_call_count);
      EXPECT_EQ(0, actions_[j]->end_call_count);
    }
  }

  // Attempts to remove bindings should fail.
  for (int i = 0; i < kBindingsPerAction; ++i) {
    for (int j = 0; j < kNumActions; ++j) {
      KeySym keysym = XK_a + (i * kNumActions) + j;
      EXPECT_FALSE(bindings_->RemoveBinding(KeyBindings::KeyCombo(keysym)));
    }
  }

  // Attempts to remove actions should fail (already removed).
  for (int i = 0; i < kNumActions; ++i) {
    EXPECT_FALSE(bindings_->RemoveAction(actions_[i]->name));
  }
}

// Test that we use the lowercase versions of keysyms.
TEST_F(KeyBindingsTest, Lowercase) {
  // Add the lowercase version first so that the keycode-to-keysym mappings
  // will return it instead of the uppercase one.
  const KeyCode keycode = 1;
  xconn_->AddKeyMapping(keycode, XK_e);
  xconn_->AddKeyMapping(keycode, XK_E);

  // Add a Ctrl+E (uppercase 'e') binding and check that it's activated by
  // the 'e' keycode.
  XTime event_time = 10;
  AddAction(0, true, false, true);
  ASSERT_TRUE(bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_E, KeyBindings::kControlMask),
      actions_[0]->name));
  EXPECT_TRUE(bindings_->HandleKeyPress(keycode, KeyBindings::kControlMask,
                                        event_time++));
  EXPECT_TRUE(bindings_->HandleKeyRelease(keycode, KeyBindings::kControlMask,
                                          event_time++));
  EXPECT_EQ(1, actions_[0]->begin_call_count);
  EXPECT_EQ(1, actions_[0]->end_call_count);
}

// Test that keysyms are still recognized when Caps Lock is on.
TEST_F(KeyBindingsTest, RemoveCapsLock) {
  const KeyCode keycode = 1;
  xconn_->AddKeyMapping(keycode, XK_E);
  xconn_->AddKeyMapping(keycode, XK_e);
  AddAction(0, true, false, true);
  ASSERT_TRUE(bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_e, KeyBindings::kControlMask),
      actions_[0]->name));

  const uint32_t kCapsLock = KeyBindings::kCapsLockMask;
  const uint32_t kNumLock = KeyBindings::kNumLockMask;

  // We need to grab Ctrl+e, Ctrl+CapsLock+e, Ctrl+NumLock+e, and
  // Ctrl+CapsLock+NumLock+e; we wouldn't get triggered when Caps Lock or
  // Num Lock is on otherwise.
  XTime event_time = 10;
  EXPECT_TRUE(xconn_->KeyIsGrabbed(keycode, KeyBindings::kControlMask));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(
                  keycode, KeyBindings::kControlMask | kCapsLock));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(
                  keycode, KeyBindings::kControlMask | kNumLock));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(
                  keycode, KeyBindings::kControlMask | kCapsLock | kNumLock));

  EXPECT_TRUE(bindings_->HandleKeyPress(
      keycode, KeyBindings::kControlMask | kCapsLock, event_time++));
  EXPECT_TRUE(bindings_->HandleKeyRelease(
      keycode, KeyBindings::kControlMask | kCapsLock, event_time++));
  EXPECT_EQ(1, actions_[0]->begin_call_count);
  EXPECT_EQ(0, actions_[0]->repeat_call_count);
  EXPECT_EQ(1, actions_[0]->end_call_count);

  EXPECT_TRUE(bindings_->HandleKeyPress(
      keycode, KeyBindings::kControlMask | kNumLock, event_time++));
  EXPECT_TRUE(bindings_->HandleKeyRelease(
      keycode, KeyBindings::kControlMask | kNumLock, event_time++));
  EXPECT_EQ(2, actions_[0]->begin_call_count);
  EXPECT_EQ(0, actions_[0]->repeat_call_count);
  EXPECT_EQ(2, actions_[0]->end_call_count);

  EXPECT_TRUE(bindings_->HandleKeyPress(
      keycode, KeyBindings::kControlMask | kCapsLock | kNumLock, event_time++));
  EXPECT_TRUE(bindings_->HandleKeyRelease(
      keycode, KeyBindings::kControlMask | kCapsLock | kNumLock, event_time++));
  EXPECT_EQ(3, actions_[0]->begin_call_count);
  EXPECT_EQ(0, actions_[0]->repeat_call_count);
  EXPECT_EQ(3, actions_[0]->end_call_count);
}

// Test that we terminate in-progress actions correctly when their modifier
// keys get released before the non-modifier key.
TEST_F(KeyBindingsTest, ModifierReleasedFirst) {
  XTime event_time = 10;
  const KeyCode keycode_k = 1;
  xconn_->AddKeyMapping(keycode_k, XK_k);
  const KeyCode keycode_control = 2;
  xconn_->AddKeyMapping(keycode_control, XK_Control_L);

  AddAction(0, true, false, true);
  ASSERT_TRUE(bindings_->AddBinding(
      KeyBindings::KeyCombo(XK_k, KeyBindings::kControlMask),
      actions_[0]->name));

  EXPECT_FALSE(bindings_->HandleKeyPress(keycode_control, 0, event_time++));
  EXPECT_TRUE(bindings_->HandleKeyPress(keycode_k, KeyBindings::kControlMask,
                                        event_time++));
  EXPECT_FALSE(bindings_->HandleKeyRelease(keycode_control,
                                           KeyBindings::kControlMask,
                                           event_time++));
  EXPECT_TRUE(bindings_->HandleKeyRelease(keycode_k, 0, event_time++));

  EXPECT_EQ(1, actions_[0]->begin_call_count);
  EXPECT_EQ(1, actions_[0]->end_call_count);
}

TEST_F(KeyBindingsTest, RefreshKeyMappings) {
  const KeySym keysym = XK_a;
  const uint32_t mods = KeyBindings::kControlMask;
  const KeyCode old_keycode = 1;
  const KeyCode new_keycode = 2;
  const KeyCode newest_keycode = 3;

  xconn_->AddKeyMapping(old_keycode, keysym);
  AddAction(0, true, false, false);
  ASSERT_TRUE(bindings_->AddBinding(KeyBindings::KeyCombo(keysym, mods),
                                    actions_[0]->name));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(old_keycode, mods));
  EXPECT_FALSE(xconn_->KeyIsGrabbed(new_keycode, mods));
  EXPECT_FALSE(xconn_->KeyIsGrabbed(newest_keycode, mods));

  // After we remap the 'a' keysym to a different keycode, KeyBindings
  // should update its grabs.
  xconn_->RemoveKeyMapping(old_keycode, keysym);
  xconn_->AddKeyMapping(new_keycode, keysym);
  bindings_->RefreshKeyMappings();
  EXPECT_FALSE(xconn_->KeyIsGrabbed(old_keycode, mods));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(new_keycode, mods));
  EXPECT_FALSE(xconn_->KeyIsGrabbed(newest_keycode, mods));

  // Remap it one more time.
  xconn_->RemoveKeyMapping(new_keycode, keysym);
  xconn_->AddKeyMapping(newest_keycode, keysym);
  bindings_->RefreshKeyMappings();
  EXPECT_FALSE(xconn_->KeyIsGrabbed(old_keycode, mods));
  EXPECT_FALSE(xconn_->KeyIsGrabbed(new_keycode, mods));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(newest_keycode, mods));
}

// Test that we correctly handle the case where a keyboard mapping change
// overlaps with the previous mapping.
TEST_F(KeyBindingsTest, OverlappingKeyMappings) {
  const KeySym keysym_a = XK_a;
  const KeySym keysym_b = XK_b;
  const uint32_t mods = KeyBindings::kControlMask;

  // Make keycode 1 map to 'a' and 2 map to 'b'.
  xconn_->AddKeyMapping(1, keysym_a);
  xconn_->AddKeyMapping(2, keysym_b);

  AddAction(0, true, false, false);
  ASSERT_TRUE(bindings_->AddBinding(KeyBindings::KeyCombo(keysym_a, mods),
                                    actions_[0]->name));
  AddAction(1, true, false, false);
  ASSERT_TRUE(bindings_->AddBinding(KeyBindings::KeyCombo(keysym_b, mods),
                                    actions_[1]->name));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(1, mods));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(2, mods));

  // Now swap things so that keycode 1 maps to 'b' and 2 maps to 'a', and
  // make sure that both keycodes are still grabbed.
  xconn_->RemoveKeyMapping(1, keysym_a);
  xconn_->RemoveKeyMapping(2, keysym_b);
  xconn_->AddKeyMapping(1, keysym_b);
  xconn_->AddKeyMapping(2, keysym_a);
  bindings_->RefreshKeyMappings();
  EXPECT_TRUE(xconn_->KeyIsGrabbed(1, mods));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(2, mods));
}

TEST_F(KeyBindingsTest, KeyBindingsGroup) {
  const KeySym keysym_a = XK_a;
  const KeySym keysym_b = XK_b;
  const uint32_t mods = KeyBindings::kControlMask;
  xconn_->AddKeyMapping(1, keysym_a);
  xconn_->AddKeyMapping(2, keysym_b);

  AddAction(0, true, false, false);
  scoped_ptr<KeyBindingsGroup> group(new KeyBindingsGroup(bindings_.get()));
  EXPECT_TRUE(group->enabled());
  group->AddBinding(KeyBindings::KeyCombo(keysym_a, mods), actions_[0]->name);
  EXPECT_TRUE(xconn_->KeyIsGrabbed(1, mods));

  group->Disable();
  EXPECT_FALSE(group->enabled());
  EXPECT_FALSE(xconn_->KeyIsGrabbed(1, mods));

  group->AddBinding(KeyBindings::KeyCombo(keysym_b, mods), actions_[0]->name);
  EXPECT_FALSE(xconn_->KeyIsGrabbed(1, mods));
  EXPECT_FALSE(xconn_->KeyIsGrabbed(2, mods));

  group->Enable();
  EXPECT_TRUE(xconn_->KeyIsGrabbed(1, mods));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(2, mods));

  group.reset();
  EXPECT_FALSE(xconn_->KeyIsGrabbed(1, mods));
  EXPECT_FALSE(xconn_->KeyIsGrabbed(2, mods));
}

// Check that we do something reasonable when handling bindings with
// keysyms that don't have keycodes.
TEST_F(KeyBindingsTest, MissingKeycodes) {
  // Add a mapping between 'a' and keycode 1.
  const KeySym keysym_a = XK_a;
  const KeySym keysym_b = XK_b;
  const uint32_t mods = KeyBindings::kControlMask;
  xconn_->AddKeyMapping(1, keysym_a);

  // Add bindings for both 'a' and 'b', and check that the keycode for 'a'
  // is grabbed (and that we don't do something stupid like trying to grab
  // keycode 0).
  AddAction(0, true, false, false);
  ASSERT_TRUE(bindings_->AddBinding(KeyBindings::KeyCombo(keysym_a, mods),
                                    actions_[0]->name));
  ASSERT_TRUE(bindings_->AddBinding(KeyBindings::KeyCombo(keysym_b, mods),
                                    actions_[0]->name));
  bindings_->RefreshKeyMappings();
  EXPECT_FALSE(xconn_->KeyIsGrabbed(0, mods));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(1, mods));

  // Now remove the keycode for 'a' and map 'b' to keycode 2.  When we
  // refresh the mappings, the grab for keycode 1 should be removed and a
  // grab should be added for keycode 2.
  xconn_->RemoveKeyMapping(1, keysym_a);
  xconn_->AddKeyMapping(2, keysym_b);
  bindings_->RefreshKeyMappings();
  EXPECT_FALSE(xconn_->KeyIsGrabbed(0, mods));
  EXPECT_FALSE(xconn_->KeyIsGrabbed(1, mods));
  EXPECT_TRUE(xconn_->KeyIsGrabbed(2, mods));

  // Everything should be ungrabbed after the KeyBindings object is destroyed.
  bindings_.reset();
  EXPECT_FALSE(xconn_->KeyIsGrabbed(0, mods));
  EXPECT_FALSE(xconn_->KeyIsGrabbed(1, mods));
  EXPECT_FALSE(xconn_->KeyIsGrabbed(2, mods));
}

// This just checks that we request detectable auto repeat from the X server.
TEST_F(KeyBindingsTest, EnableDetectableAutoRepeat) {
  EXPECT_TRUE(xconn_->using_detectable_keyboard_auto_repeat());
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
