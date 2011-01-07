// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_KEY_BINDINGS_H_
#define WINDOW_MANAGER_KEY_BINDINGS_H_

// KeyBindings
//
// The KeyBindings class supports installing named actions and keyboard
// combos that trigger an installed action.
//
// A named action can have begin, repeat, and end callbacks associated with it
// which correspond to key down, key repeat, and key release respectively.
// Any of these callbacks may be NULL. Any number of KeyCombo's can be bound
// to a given action. A KeyCombo is a keysym and modifier combination such as
// (XK_Tab, kAltMask). For example, to install a "switch-window" action with
// the alt-tab key combo and have SwitchWindowCallback called on combo press:
//
//   KeyBindings bindings;
//   bindings.AddAction("switch-window",
//                      NewPermanentCallback(SwitchWindowCallback),
//                      NULL,    // No repeat callback
//                      NULL);   // No end callback
//   bindings.AddBinding(
//       KeyBindings::KeyCombo(XK_Tab, kAltMask), "switch-window");

#include <map>
#include <set>
#include <string>

#include <stdint.h>

#include "base/basictypes.h"
#include "window_manager/callback.h"
#include "window_manager/x_types.h"

namespace window_manager {

struct Action;
class XConnection;

class KeyBindings {
 public:
  // Set of possible modifer mask bits. OR these together to create a KeyCombo
  // modifiers value.
  static const uint32_t kShiftMask;
  static const uint32_t kCapsLockMask;
  static const uint32_t kControlMask;
  static const uint32_t kAltMask;
  static const uint32_t kNumLockMask;

  // A key and modifier combination, such as (XK_Tab, kAltMask) for alt-tab.
  struct KeyCombo {
    // We lowercase keysyms (the uppercase distinction when Shift is down
    // or Caps Lock is on isn't useful for us) and mask kCapsLockMask and
    // kNumLockMask out of the modifier (so that bindings will still be
    // recognized if Caps Lock or Num Lock are enabled).
    explicit KeyCombo(KeySym keysym_param, uint32_t modifiers_param);

    bool operator<(const KeyCombo& o) const;

    KeySym keysym;
    uint32_t modifiers;
  };

  explicit KeyBindings(XConnection* xconn);
  ~KeyBindings();

  XTime current_event_time() const { return current_event_time_; }
  const KeyCombo& current_key_combo() const { return current_key_combo_; }

  // Add a new action. This will fail if the action already exists.
  // NOTE: The KeyBindings class will take ownership of passed-in
  // callbacks, any of which may be NULL.
  bool AddAction(const std::string& action_name,
                 Closure* begin_closure,   // On combo press
                 Closure* repeat_closure,  // On combo auto-repeat
                 Closure* end_closure);    // On combo release

  // Removes an action. Any key bindings to this action will also be removed.
  bool RemoveAction(const std::string& action_name);

  // Add a binding from the given KeyCombo to the action. KeyCombo's must be
  // unique, but it is fine to have more than one combo map to a given action.
  bool AddBinding(const KeyCombo& combo,
                  const std::string& action_name);

  // Remove the KeyCombo. This may fail if the action to which the combo was
  // bound has been removed, in which case the combo was already cleaned up.
  bool RemoveBinding(const KeyCombo& combo);

  // Called after the X server's keymap changes to regrab updated keycodes
  // if needed.
  void RefreshKeyMappings();

  // These should be called by the window manager when keys are pressed or
  // released.  These methods return true if an action is invoked and false
  // otherwise.
  bool HandleKeyPress(KeyCode keycode, uint32_t modifiers, XTime event_time);
  bool HandleKeyRelease(KeyCode keycode, uint32_t modifiers, XTime event_time);

 private:
  // Grab or ungrab a combination of a key and some modifiers.  We also
  // install grabs for the combination plus Caps Lock and Num Lock.
  void GrabKey(KeyCode keycode, uint32_t modifiers);
  void UngrabKey(KeyCode keycode, uint32_t modifiers);

  XConnection* xconn_;  // not owned

  // Non-zero when we are within a call to HandleKeyPress or
  // HandleKeyRelease.  This allows the action closures to access the event
  // time if they need it.
  XTime current_event_time_;

  // The latest key combo associated with an action that we received.
  // When |current_event_time_| is non-zero, this contains the combo
  // corresponding to the action that is currently being executed.
  KeyCombo current_key_combo_;

  typedef std::map<std::string, Action*> ActionMap;
  ActionMap actions_;

  typedef std::map<KeyCombo, std::string> BindingsMap;
  BindingsMap bindings_;

  // Map from a keysym to the names of all of the actions that use it as
  // their non-modifier key and the number of combos triggering them (e.g.
  // if Alt-Tab and Ctrl-Tab both trigger "cycle-window", then the map will
  // contain { XK_Tab: { "cycle-window": 2 } }.
  typedef std::map<KeySym, std::map<std::string, int> > KeySymMap;
  KeySymMap action_names_by_keysym_;

  // Map from keysyms that we need to watch for to the corresponding
  // keycodes that we've grabbed (note that the keycodes can be out-of-date
  // if the X server's keymap has changed; HandleKeyMapChange() will
  // rectify this).
  std::map<KeySym, KeyCode> keysyms_to_grabbed_keycodes_;

  DISALLOW_COPY_AND_ASSIGN(KeyBindings);
};

// This RAII class can be used to track key binding actions.  When the
// class is destroyed, all of the actions that were registered through it
// are removed.
class KeyBindingsActionRegistrar {
 public:
  explicit KeyBindingsActionRegistrar(KeyBindings* bindings)
      : bindings_(bindings) {}
  ~KeyBindingsActionRegistrar();

  // Register an action.  See KeyBindings::AddAction().
  bool AddAction(const std::string& action_name,
                 Closure* begin_closure,
                 Closure* repeat_closure,
                 Closure* end_closure);

 private:
  KeyBindings* bindings_;  // not owned

  // Names of actions that have been registered.
  std::set<std::string> action_names_;

  DISALLOW_COPY_AND_ASSIGN(KeyBindingsActionRegistrar);
};

// This helper class can be used to easily enable or disable a group of key
// bindings.
class KeyBindingsGroup {
 public:
  // The group is initially enabled.
  explicit KeyBindingsGroup(KeyBindings* bindings);
  ~KeyBindingsGroup();

  bool enabled() const { return enabled_; }

  // Add a binding to the group.
  void AddBinding(const KeyBindings::KeyCombo& combo,
                  const std::string& action_name);

  // Enable or disable all bindings in this group.
  void Enable();
  void Disable();

 private:
  KeyBindings* bindings_;  // not owned

  // Are this group's bindings active?
  bool enabled_;

  // Bindings under this group's control.
  std::map<KeyBindings::KeyCombo, std::string> combos_to_action_names_;

  DISALLOW_COPY_AND_ASSIGN(KeyBindingsGroup);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_KEY_BINDINGS_H_
