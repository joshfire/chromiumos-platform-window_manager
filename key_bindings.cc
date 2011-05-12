// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/key_bindings.h"

#include <utility>
#include <vector>

extern "C" {
#include <X11/X.h>
#include <X11/Xutil.h>
}
#include <gflags/gflags.h>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "window_manager/util.h"
#include "window_manager/x11/x_connection.h"

using std::make_pair;
using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;
using window_manager::util::FindWithDefault;

namespace window_manager {

const uint32_t KeyBindings::kShiftMask    = ShiftMask;
const uint32_t KeyBindings::kCapsLockMask = LockMask;
const uint32_t KeyBindings::kControlMask  = ControlMask;
const uint32_t KeyBindings::kAltMask      = Mod1Mask;
const uint32_t KeyBindings::kNumLockMask  = Mod2Mask;


KeyBindings::KeyCombo::KeyCombo(KeySym key_param, uint32_t modifiers_param) {
  KeySym upper_keysym = None, lower_keysym = None;
  XConvertCase(key_param, &lower_keysym, &upper_keysym);
  keysym = lower_keysym;
  modifiers = (modifiers_param & (~kCapsLockMask & ~kNumLockMask));
}

bool KeyBindings::KeyCombo::operator<(const KeyCombo& o) const {
  return (keysym < o.keysym) ||
         ((keysym == o.keysym) && (modifiers < o.modifiers));
}

struct Action {
  Action(Closure* begin_closure_param,
         Closure* repeat_closure_param,
         Closure* end_closure_param)
      : running(false),
        begin_closure(begin_closure_param),
        repeat_closure(repeat_closure_param),
        end_closure(end_closure_param) {}
  ~Action() {
    CHECK(bindings.empty());
  }

  // Is this action currently "running"? For certain key combinations, the
  // X server will keep sending key presses while the key is held down. For
  // any such sequence, the action is "running" after the first combo press
  // until a combo release is seen.
  bool running;

  // Closure to run when the action begins (i.e. key combo press)
  scoped_ptr<Closure> begin_closure;

  // Closure to run on action repeat while running (i.e. key combo repeat)
  scoped_ptr<Closure> repeat_closure;

  // Closure to run when the action ends (i.e. key combo release)
  scoped_ptr<Closure> end_closure;

  // The set of key combinations currently bound to this action.
  set<KeyBindings::KeyCombo> bindings;

  DISALLOW_COPY_AND_ASSIGN(Action);
};

KeyBindings::KeyBindings(XConnection* xconn)
  : xconn_(xconn),
    current_event_time_(0),
    current_key_combo_(0, 0) {
  CHECK(xconn_);
  if (!xconn_->SetDetectableKeyboardAutoRepeat(true)) {
    LOG(WARNING) << "Unable to enable detectable keyboard autorepeat";
  }
}

KeyBindings::~KeyBindings() {
  while (!actions_.empty()) {
    RemoveAction(actions_.begin()->first);
  }

  // Removing all actions should have also removed all bindings.
  CHECK(bindings_.size() == 0);
}

bool KeyBindings::AddAction(const string& action_name,
                            Closure* begin_closure,
                            Closure* repeat_closure,
                            Closure* end_closure) {
  CHECK(!action_name.empty());
  if (actions_.find(action_name) != actions_.end()) {
    LOG(WARNING) << "Attempting to add action that already exists: "
                 << action_name;
    return false;
  }
  Action* const action = new Action(begin_closure, repeat_closure, end_closure);
  CHECK(actions_.insert(make_pair(action_name, action)).second);
  return true;
}

bool KeyBindings::RemoveAction(const string& action_name) {
  ActionMap::iterator iter = actions_.find(action_name);
  if (iter == actions_.end()) {
    LOG(WARNING) << "Attempting to remove non-existant action: " << action_name;
    return false;
  }
  Action* const action = iter->second;
  while (!action->bindings.empty()) {
    CHECK(RemoveBinding(*(action->bindings.begin())));
  }
  delete action;
  actions_.erase(iter);

  return true;
}

bool KeyBindings::AddBinding(const KeyCombo& combo, const string& action_name) {
  if (bindings_.find(combo) != bindings_.end()) {
    LOG(WARNING) << "Attempt to overwrite existing key binding for action: "
                 << action_name;
    return false;
  }
  ActionMap::iterator iter = actions_.find(action_name);
  if (iter == actions_.end()) {
    LOG(WARNING) << "Attempt to add key binding for missing action: "
                 << action_name;
    return false;
  }

  Action* const action = iter->second;
  CHECK(action->bindings.insert(combo).second);
  CHECK(bindings_.insert(make_pair(combo, action_name)).second);
  action_names_by_keysym_[combo.keysym][action_name]++;

  KeyCode keycode = FindWithDefault(
      keysyms_to_grabbed_keycodes_, combo.keysym, static_cast<KeyCode>(0));
  if (keycode == 0) {
    keycode = xconn_->GetKeyCodeFromKeySym(combo.keysym);
    if (keycode != 0)
      keysyms_to_grabbed_keycodes_[combo.keysym] = keycode;
  }

  if (keycode == 0) {
    // We'll try again if the keymap changes.
    LOG(WARNING) << "Unable to look up keycode for keysym " << combo.keysym
                 << "; not grabbing key";
  } else {
    GrabKey(keycode, combo.modifiers);
  }
  return true;
}

bool KeyBindings::RemoveBinding(const KeyCombo& combo) {
  BindingsMap::iterator bindings_iter = bindings_.find(combo);
  if (bindings_iter == bindings_.end())
    return false;

  ActionMap::iterator action_iter = actions_.find(bindings_iter->second);
  CHECK(action_iter != actions_.end());
  Action* action = action_iter->second;
  CHECK(action->bindings.erase(combo) == 1);

  // Decrement the count of bindings for this action in the
  // keysym-to-action map, and remove the entry if it was the only one.
  KeySymMap::iterator keysym_iter = action_names_by_keysym_.find(combo.keysym);
  CHECK(keysym_iter != action_names_by_keysym_.end());
  map<string, int>::iterator count_iter =
      keysym_iter->second.find(bindings_iter->second);
  CHECK(count_iter != keysym_iter->second.end());
  count_iter->second--;
  DCHECK_GE(count_iter->second, 0);
  if (count_iter->second == 0) {
    keysym_iter->second.erase(count_iter);
    if (keysym_iter->second.empty())
      action_names_by_keysym_.erase(keysym_iter);
  }

  bindings_.erase(bindings_iter);

  // If this action triggered its own binding's removal we won't know what
  // to do with the corresponding release, so go ahead and mark the action
  // as not running here.
  action->running = false;

  KeyCode keycode = FindWithDefault(
      keysyms_to_grabbed_keycodes_, combo.keysym, static_cast<KeyCode>(0));
  if (keycode != 0)
    UngrabKey(keycode, combo.modifiers);
  return true;
}

void KeyBindings::RefreshKeyMappings() {
  map<KeySym, KeyCode> new_keysyms_to_grabbed_keycodes_;

  vector<pair<KeyCode, uint32_t> > grabs_to_remove;
  vector<pair<KeyCode, uint32_t> > grabs_to_add;

  // Go through all of our combos, looking up the old keycodes and the new
  // ones and keeping track of things that've changed.
  for (BindingsMap::const_iterator it = bindings_.begin();
       it != bindings_.end(); ++it) {
    const KeyCombo& combo = it->first;

    KeyCode old_keycode = FindWithDefault(
        keysyms_to_grabbed_keycodes_, combo.keysym, static_cast<KeyCode>(0));

    KeyCode new_keycode = FindWithDefault(new_keysyms_to_grabbed_keycodes_,
                                          combo.keysym,
                                          static_cast<KeyCode>(0));
    if (new_keycode == 0) {
      new_keycode = xconn_->GetKeyCodeFromKeySym(combo.keysym);
      if (new_keycode != 0)
        new_keysyms_to_grabbed_keycodes_[combo.keysym] = new_keycode;
    }

    if (new_keycode != old_keycode) {
      if (old_keycode != 0)
        grabs_to_remove.push_back(make_pair(old_keycode, combo.modifiers));
      if (new_keycode != 0) {
        grabs_to_add.push_back(make_pair(new_keycode, combo.modifiers));
      } else {
        LOG(WARNING) << "Unable to look up new keycode for keysym "
                     << combo.keysym << "; not grabbing key";
      }
    }
  }

  // Now actually ungrab and regrab things as needed (this is done in a
  // separate step in case there's overlap between the old and new mappings).
  for (vector<pair<KeyCode, uint32_t> >::const_iterator it =
         grabs_to_remove.begin(); it != grabs_to_remove.end(); ++it) {
    UngrabKey(it->first, it->second);
  }
  for (vector<pair<KeyCode, uint32_t> >::const_iterator it =
         grabs_to_add.begin(); it != grabs_to_add.end(); ++it) {
    GrabKey(it->first, it->second);
  }
  keysyms_to_grabbed_keycodes_.swap(new_keysyms_to_grabbed_keycodes_);
}

bool KeyBindings::HandleKeyPress(KeyCode keycode,
                                 uint32_t modifiers,
                                 XTime event_time) {
  const KeySym keysym = xconn_->GetKeySymFromKeyCode(keycode);
  AutoReset<XTime> time_reset(&current_event_time_, event_time);
  KeyCombo combo(keysym, modifiers);
  BindingsMap::const_iterator bindings_iter = bindings_.find(combo);
  if (bindings_iter == bindings_.end())
    return false;

  AutoReset<KeyCombo> combo_reset(&current_key_combo_, combo);
  ActionMap::iterator action_iter = actions_.find(bindings_iter->second);
  CHECK(action_iter != actions_.end());
  Action* const action = action_iter->second;
  if (action->running) {
    if (action->repeat_closure.get()) {
      action->repeat_closure->Run();
      return true;
    }
  } else {
    action->running = true;
    if (action->begin_closure.get()) {
      action->begin_closure->Run();
      return true;
    }
  }
  return false;
}

bool KeyBindings::HandleKeyRelease(KeyCode keycode,
                                   uint32_t modifiers,
                                   XTime event_time) {
  const KeySym keysym = xconn_->GetKeySymFromKeyCode(keycode);
  AutoReset<XTime> time_reset(&current_event_time_, event_time);
  KeyCombo combo(keysym, modifiers);

  // It's possible that a combo's modifier key(s) will get released before
  // its non-modifier key: for an Alt+Tab combo, imagine seeing Alt press,
  // Tab press, Alt release, and then Tab release.  In this case, kAltMask
  // won't be present in the Tab release event's modifier bitmap.  We still
  // want to run the end closure for the in-progress action when we receive
  // the Tab release, so we check all of the non-modifier key's actions
  // here to see if any of them are active.
  KeySymMap::const_iterator keysym_iter =
      action_names_by_keysym_.find(combo.keysym);
  if (keysym_iter == action_names_by_keysym_.end())
    return false;

  AutoReset<KeyCombo> combo_reset(&current_key_combo_, combo);
  bool ran_end_closure = false;
  for (map<string, int>::const_iterator action_name_iter =
         keysym_iter->second.begin();
       action_name_iter != keysym_iter->second.end(); ++action_name_iter) {
    ActionMap::iterator action_iter = actions_.find(action_name_iter->first);
    CHECK(action_iter != actions_.end());
    Action* const action = action_iter->second;
    if (action->running) {
      action->running = false;
      if (action->end_closure.get()) {
        action->end_closure->Run();
        ran_end_closure = true;
      }
    }
  }
  return ran_end_closure;
}

void KeyBindings::GrabKey(KeyCode keycode, uint32_t modifiers) {
  xconn_->GrabKey(keycode, modifiers);
  xconn_->GrabKey(keycode, modifiers | kCapsLockMask);
  xconn_->GrabKey(keycode, modifiers | kNumLockMask);
  xconn_->GrabKey(keycode, modifiers | kCapsLockMask | kNumLockMask);
}

void KeyBindings::UngrabKey(KeyCode keycode, uint32_t modifiers) {
  xconn_->UngrabKey(keycode, modifiers);
  xconn_->UngrabKey(keycode, modifiers | kCapsLockMask);
  xconn_->UngrabKey(keycode, modifiers | kNumLockMask);
  xconn_->UngrabKey(keycode, modifiers | kCapsLockMask | kNumLockMask);
}


KeyBindingsActionRegistrar::~KeyBindingsActionRegistrar() {
  for (set<string>::const_iterator it = action_names_.begin();
       it != action_names_.end(); ++it) {
    bindings_->RemoveAction(*it);
  }
}

bool KeyBindingsActionRegistrar::AddAction(const std::string& action_name,
                                           Closure* begin_closure,
                                           Closure* repeat_closure,
                                           Closure* end_closure) {
  bool result = bindings_->AddAction(action_name,
                                     begin_closure,
                                     repeat_closure,
                                     end_closure);
  if (result) {
    CHECK(action_names_.insert(action_name).second)
        << "Action " << action_name << " has already been registered";
  }
  return result;
}


KeyBindingsGroup::KeyBindingsGroup(KeyBindings* bindings)
    : bindings_(bindings),
      enabled_(true) {
  DCHECK(bindings);
}

KeyBindingsGroup::~KeyBindingsGroup() {
  Disable();
}

void KeyBindingsGroup::AddBinding(const KeyBindings::KeyCombo& combo,
                                  const string& action_name) {
  combos_to_action_names_.insert(make_pair(combo, action_name));
  if (enabled_)
    bindings_->AddBinding(combo, action_name);
}

void KeyBindingsGroup::Enable() {
  if (enabled_)
    return;

  for (map<KeyBindings::KeyCombo, string>::const_iterator it =
         combos_to_action_names_.begin();
       it != combos_to_action_names_.end(); ++it) {
    bindings_->AddBinding(it->first, it->second);
  }
  enabled_ = true;
}

void KeyBindingsGroup::Disable() {
  if (!enabled_)
    return;

  for (map<KeyBindings::KeyCombo, string>::const_iterator it =
         combos_to_action_names_.begin();
       it != combos_to_action_names_.end(); ++it) {
    bindings_->RemoveBinding(it->first);
  }
  enabled_ = false;
}

}  // namespace window_manager
