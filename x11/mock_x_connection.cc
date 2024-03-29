// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/x11/mock_x_connection.h"

#include <fcntl.h>
#include <unistd.h>

#include <list>

extern "C" {
#include <X11/extensions/sync.h>
#include <X11/extensions/Xdamage.h>
}

#include "base/logging.h"
#include "base/eintr_wrapper.h"
#include "window_manager/image_enums.h"
#include "window_manager/util.h"
#include "window_manager/x11/x_connection_internal.h"

using std::list;
using std::make_pair;
using std::map;
using std::pair;
using std::string;
using std::tr1::shared_ptr;
using std::vector;
using window_manager::util::FindWithDefault;
using window_manager::util::XidStr;

namespace window_manager {

const int MockXConnection::kDisplayWidth = 1024;
const int MockXConnection::kDisplayHeight = 768;
const XID MockXConnection::kTransparentCursor = 1000;  // arbitrary

MockXConnection::MockXConnection()
    : windows_(),
      stacked_xids_(new Stacker<XWindow>),
      next_xid_(1),
      root_(CreateWindow(0,      // parent
                         Rect(0, 0, kDisplayWidth, kDisplayHeight),
                         true,   // override_redirect
                         false,  // input_only
                         0,      // event_mask
                         0)),    // visual
      overlay_(CreateWindow(root_,  // parent
                            Rect(0, 0, kDisplayWidth, kDisplayHeight),
                            true,   // override_redirect
                            false,  // input_only
                            0,      // event_mask
                            0)),    // visual
      next_atom_(1000),
      focused_xid_(None),
      last_focus_timestamp_(0),
      current_time_(0),
      pointer_grab_xid_(None),
      keyboard_grab_xid_(None),
      num_keymap_refreshes_(0),
      pointer_pos_(kDisplayWidth / 2, kDisplayHeight / 2),
      cursor_shown_(true),
      using_detectable_keyboard_auto_repeat_(false),
      connection_pipe_has_data_(false),
      num_pointer_ungrabs_with_replayed_events_(0) {
  PCHECK(HANDLE_EINTR(pipe(connection_pipe_fds_)) != -1);
  PCHECK(HANDLE_EINTR(
             fcntl(connection_pipe_fds_[0], F_SETFL, O_NONBLOCK)) != -1);
  PCHECK(HANDLE_EINTR(
             fcntl(connection_pipe_fds_[1], F_SETFL, O_NONBLOCK)) != -1);
  // Arbitrary large numbers unlikely to be used by other events.
  damage_event_base_ = 10000;
  shape_event_base_  = 10010;
  randr_event_base_  = 10020;
  sync_event_base_   = 10030;
}

MockXConnection::~MockXConnection() {
  PCHECK(HANDLE_EINTR(close(connection_pipe_fds_[0])) != -1);
  PCHECK(HANDLE_EINTR(close(connection_pipe_fds_[1])) != -1);
}

bool MockXConnection::GetWindowGeometry(XWindow xid, WindowGeometry* geom_out) {
  CHECK(geom_out);
  if (WindowInfo* window_info = GetWindowInfo(xid)) {
    geom_out->bounds = window_info->bounds;
    geom_out->border_width = window_info->border_width;
    geom_out->depth = window_info->depth;
    return true;
  }

  if (PixmapInfo* pixmap_info = GetPixmapInfo(xid)) {
    geom_out->bounds.reset(Point(), pixmap_info->size);
    geom_out->border_width = 0;
    geom_out->depth = pixmap_info->depth;
    return true;
  }

  return false;
}

bool MockXConnection::MapWindow(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->mapped = true;
  info->changed = true;
  return true;
}

bool MockXConnection::UnmapWindow(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->mapped = false;
  if (focused_xid_ == xid)
    focused_xid_ = None;
  info->changed = true;
  return true;
}

bool MockXConnection::MoveWindow(XWindow xid, const Point& pos) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->bounds.move(pos);
  info->changed = true;
  info->num_configures++;
  return true;
}

bool MockXConnection::ResizeWindow(XWindow xid, const Size& size) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->bounds.resize(size, GRAVITY_NORTHWEST);
  info->changed = true;
  info->num_configures++;
  return true;
}

bool MockXConnection::RaiseWindow(XWindow xid) {
  if (!stacked_xids_->Contains(xid))
    return false;
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  stacked_xids_->Remove(xid);
  stacked_xids_->AddOnTop(xid);
  info->num_configures++;
  return true;
}

bool MockXConnection::FocusWindow(XWindow xid, XTime event_time) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;

  // The X server ignores requests with old timestamps.
  if (event_time < last_focus_timestamp_)
    return true;

  focused_xid_ = xid;
  last_focus_timestamp_ = event_time;
  return true;
}

bool MockXConnection::StackWindow(XWindow xid, XWindow other, bool above) {
  if (!stacked_xids_->Contains(xid) || !stacked_xids_->Contains(other))
    return false;
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  stacked_xids_->Remove(xid);
  if (above)
    stacked_xids_->AddAbove(xid, other);
  else
    stacked_xids_->AddBelow(xid, other);
  info->num_configures++;
  return true;
}

bool MockXConnection::AddButtonGrabOnWindow(
    XWindow xid, int button, int event_mask, bool synchronous) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->button_grabs[button] =
      WindowInfo::ButtonGrabInfo(event_mask, synchronous);
  return true;
}

bool MockXConnection::SetWindowBorderWidth(XWindow xid, int width) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->border_width = width;
  info->num_configures++;
  return true;
}

bool MockXConnection::SelectInputOnWindow(
    XWindow xid, int event_mask, bool preserve_existing) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->event_mask = preserve_existing ?
      (info->event_mask | event_mask) : event_mask;
  return true;
}

bool MockXConnection::DeselectInputOnWindow(XWindow xid, int event_mask) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->event_mask &= ~event_mask;
  return true;
}

bool MockXConnection::RemoveButtonGrabOnWindow(XWindow xid, int button) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->button_grabs.erase(button);
  return true;
}

bool MockXConnection::GrabPointer(XWindow xid,
                                  int event_mask,
                                  XTime timestamp,
                                  XID cursor) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  if (pointer_grab_xid_ != None) {
    LOG(ERROR) << "Pointer is already grabbed for " << XidStr(pointer_grab_xid_)
               << "; ignoring request to grab it for " << XidStr(xid);
    return false;
  }
  pointer_grab_xid_ = xid;
  return true;
}

bool MockXConnection::UngrabPointer(bool replay_events, XTime timestamp) {
  pointer_grab_xid_ = None;
  if (replay_events)
    num_pointer_ungrabs_with_replayed_events_++;
  return true;
}

bool MockXConnection::GrabKeyboard(XWindow xid, XTime timestamp) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  if (keyboard_grab_xid_ != None) {
    LOG(ERROR) << "Keyeboard is already grabbed for "
               << XidStr(keyboard_grab_xid_)
               << "; ignoring request to grab it for " << XidStr(xid);
    return false;
  }
  keyboard_grab_xid_ = xid;
  return true;
}

bool MockXConnection::GetSizeHintsForWindow(XWindow xid, SizeHints* hints_out) {
  CHECK(hints_out);
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  *hints_out = info->size_hints;
  return true;
}

bool MockXConnection::GetTransientHintForWindow(XWindow xid,
                                                XWindow* owner_out) {
  CHECK(owner_out);
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  *owner_out = info->transient_for;
  return true;
}

bool MockXConnection::GetWindowAttributes(XWindow xid,
                                          WindowAttributes* attr_out) {
  CHECK(attr_out);
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;

  attr_out->window_class = info->input_only ?
      WindowAttributes::WINDOW_CLASS_INPUT_ONLY :
      WindowAttributes::WINDOW_CLASS_INPUT_OUTPUT;
  attr_out->map_state = info->mapped ?
      WindowAttributes::MAP_STATE_VIEWABLE :
      WindowAttributes::MAP_STATE_UNMAPPED;
  attr_out->override_redirect = info->override_redirect;
  return true;
}

bool MockXConnection::RedirectSubwindowsForCompositing(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->redirect_subwindows = true;

  for (map<XWindow, shared_ptr<WindowInfo> >::iterator it = windows_.begin();
       it != windows_.end(); ++it) {
    WindowInfo* other_win_info = it->second.get();
    if (other_win_info->parent == xid)
      other_win_info->redirected = true;
  }

  return true;
}

bool MockXConnection::RedirectWindowForCompositing(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->redirected = true;
  return true;
}

bool MockXConnection::UnredirectWindowForCompositing(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->redirected = false;
  return true;
}

XPixmap MockXConnection::CreatePixmap(XDrawable drawable,
                                      const Size& size,
                                      int depth) {
  XID xid = next_xid_++;
  shared_ptr<PixmapInfo> info(new PixmapInfo(xid, size, depth));
  pixmaps_[xid] = info;
  return xid;
}

XPixmap MockXConnection::GetCompositingPixmapForWindow(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return 0;
  return CreatePixmap(xid, info->bounds.size(), info->depth);
}

bool MockXConnection::FreePixmap(XPixmap pixmap) {
  map<XID, shared_ptr<PixmapInfo> >::iterator it = pixmaps_.find(pixmap);
  if (it == pixmaps_.end())
    return false;
  pixmaps_.erase(it);
  return true;
}

XWindow MockXConnection::CreateWindow(
    XWindow parent,
    const Rect& bounds,
    bool override_redirect,
    bool input_only,
    int event_mask,
    XVisualID visual) {
  XWindow xid = next_xid_++;
  shared_ptr<WindowInfo> info(new WindowInfo(xid, parent));
  info->bounds = bounds;
  info->override_redirect = override_redirect;
  info->input_only = input_only;
  info->event_mask = event_mask;
  info->visual = visual;

  windows_[xid] = info;
  stacked_xids_->AddOnTop(xid);

  const WindowInfo* parent_info = GetWindowInfo(parent);
  if (parent_info && parent_info->redirect_subwindows)
    info->redirected = true;

  return xid;
}

bool MockXConnection::DestroyWindow(XWindow xid) {
  map<XWindow, shared_ptr<WindowInfo> >::iterator it = windows_.find(xid);
  if (it == windows_.end())
    return false;
  windows_.erase(it);
  stacked_xids_->Remove(xid);
  if (focused_xid_ == xid)
    focused_xid_ = None;

  // Release any selections held by this window.
  vector<XAtom> orphaned_selections;
  for (map<XAtom, XWindow>::const_iterator it = selection_owners_.begin();
       it != selection_owners_.end(); ++it) {
    if (it->second == xid)
      orphaned_selections.push_back(it->first);
  }
  for (vector<XAtom>::const_iterator it = orphaned_selections.begin();
       it != orphaned_selections.end(); ++it) {
    selection_owners_.erase(*it);
  }

  return true;
}

bool MockXConnection::IsWindowShaped(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  return (info->shape.get() != NULL);
}

bool MockXConnection::SelectShapeEventsOnWindow(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->shape_events_selected = true;
  return true;
}

bool MockXConnection::GetWindowBoundingRegion(XWindow xid, ByteMap* bytemap) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;

  bytemap->Resize(info->bounds.size());
  bytemap->Clear(0);
  if (info->shape.get())
    bytemap->Copy(*(info->shape.get()));
  else
    bytemap->Clear(0xff);
  return true;
}

bool MockXConnection::SetWindowBoundingRegionToRect(XWindow xid,
                                                    const Rect& region) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;

  info->shape.reset(
      new ByteMap(Size(region.x + region.width, region.y + region.height)));
  info->shape->Clear(0);
  info->shape->SetRectangle(region, 0xff);
  return true;
}

bool MockXConnection::ResetWindowBoundingRegionToDefault(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->shape.reset();
  return true;
}

bool MockXConnection::SelectRandREventsOnWindow(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->randr_events_selected = true;
  return true;
}

bool MockXConnection::GetAtoms(const vector<string>& names,
                               vector<XAtom>* atoms_out) {
  CHECK(atoms_out);
  atoms_out->clear();
  for (vector<string>::const_iterator name_it = names.begin();
       name_it != names.end(); ++name_it) {
    map<string, XAtom>::const_iterator find_it = name_to_atom_.find(*name_it);
    if (find_it != name_to_atom_.end()) {
      atoms_out->push_back(find_it->second);
    } else {
      XAtom atom = next_atom_;
      atoms_out->push_back(atom);
      name_to_atom_[*name_it] = atom;
      atom_to_name_[atom] = *name_it;
      next_atom_++;
    }
  }
  return true;
}

bool MockXConnection::GetIntArrayProperty(XWindow xid,
                                          XAtom xatom,
                                          vector<int>* values) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  map<XAtom, vector<int> >::const_iterator it =
      info->int_properties.find(xatom);
  if (it == info->int_properties.end())
    return false;
  *values = it->second;
  return true;
}

bool MockXConnection::SetIntArrayProperty(XWindow xid,
                                          XAtom xatom,
                                          XAtom type,
                                          const vector<int>& values) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->int_properties[xatom] = values;
  // TODO: Also save type.
  Closure* cb =
      FindWithDefault(property_callbacks_,
                      make_pair(xid, xatom),
                      shared_ptr<Closure>(static_cast<Closure*>(NULL))).get();
  if (cb)
    cb->Run();
  return true;
}

bool MockXConnection::GetStringProperty(XWindow xid, XAtom xatom, string* out) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  map<XAtom, string>::const_iterator it = info->string_properties.find(xatom);
  if (it == info->string_properties.end())
    return false;
  *out = it->second;
  return true;
}

bool MockXConnection::SetStringProperty(XWindow xid,
                                        XAtom xatom,
                                        const string& value) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->string_properties[xatom] = value;
  Closure* cb =
      FindWithDefault(property_callbacks_,
                      make_pair(xid, xatom),
                      shared_ptr<Closure>(static_cast<Closure*>(NULL))).get();
  if (cb)
    cb->Run();
  return true;
}

bool MockXConnection::DeletePropertyIfExists(XWindow xid, XAtom xatom) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->int_properties.erase(xatom);
  info->string_properties.erase(xatom);
  return true;
}

bool MockXConnection::IsEventPending() {
  return !queued_events_.empty();
}

bool MockXConnection::WaitForPropertyChange(XWindow xid, XTime* timestamp_out) {
  if (timestamp_out) {
    current_time_ += 10;
    *timestamp_out = current_time_;
  }
  return true;
}

bool MockXConnection::SendClientMessageEvent(XWindow dest_xid,
                                             XWindow xid,
                                             XAtom message_type,
                                             long data[5],
                                             int event_mask) {
  WindowInfo* info = GetWindowInfo(dest_xid);
  if (!info)
    return false;

  XEvent event;
  x_connection_internal::InitXClientMessageEvent(
      &event, xid, message_type, data);
  info->client_messages.push_back(event.xclient);
  return true;
}

bool MockXConnection::SendConfigureNotifyEvent(XWindow xid,
                                               const Rect& bounds,
                                               int border_width,
                                               XWindow above_xid,
                                               bool override_redirect) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;

  XEvent event;
  x_connection_internal::InitXConfigureEvent(
      &event, xid, bounds, border_width, above_xid, override_redirect);

  info->configure_notify_events.push_back(event.xconfigure);
  return true;
}

bool MockXConnection::GetAtomName(XAtom atom, string* name) {
  CHECK(name);
  map<XAtom, string>::const_iterator it = atom_to_name_.find(atom);
  if (it == atom_to_name_.end())
    return false;
  *name = it->second;
  return true;
}

XWindow MockXConnection::GetSelectionOwner(XAtom atom) {
  map<XAtom, XWindow>::const_iterator it = selection_owners_.find(atom);
  return (it == selection_owners_.end()) ? None : it->second;
}

bool MockXConnection::SetSelectionOwner(
    XAtom atom, XWindow xid, XTime timestamp) {
  selection_owners_[atom] = xid;
  return true;
}

bool MockXConnection::GetImage(XID drawable,
                               const Rect& bounds,
                               int drawable_depth,
                               scoped_ptr_malloc<uint8_t>* data_out,
                               ImageFormat* format_out) {
  CHECK(data_out);
  CHECK(format_out);
  WindowInfo* info = GetWindowInfo(drawable);
  if (!info)
    return false;

  // TODO: Make data settable in the WindowInfo so it can be tested.
  data_out->reset(NULL);
  *format_out = (drawable_depth == 32) ?
                IMAGE_FORMAT_RGBA_32 :
                IMAGE_FORMAT_RGBX_32;
  return true;
}

bool MockXConnection::SetWindowCursor(XWindow xid, XID cursor) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->cursor = cursor;
  return true;
}

bool MockXConnection::GrabKey(KeyCode keycode, uint32 modifiers) {
  grabbed_keys_.insert(make_pair(keycode, modifiers));
  return true;
}

bool MockXConnection::UngrabKey(KeyCode keycode, uint32 modifiers) {
  grabbed_keys_.erase(make_pair(keycode, modifiers));
  return true;
}

void MockXConnection::SetSyncCounter(XID counter_id, int64_t value) {
  sync_counters_[counter_id] = value;
}

XID MockXConnection::CreateSyncCounterAlarm(XID counter_id,
                                            int64_t initial_trigger_value) {
  if (sync_counters_.count(counter_id) == 0)
    sync_counters_[counter_id] = 0;

  XID alarm_id = next_xid_++;
  sync_counter_alarms_[alarm_id] =
      shared_ptr<SyncCounterAlarmInfo>(
          new SyncCounterAlarmInfo(counter_id, initial_trigger_value));
  return alarm_id;
}

void MockXConnection::DestroySyncCounterAlarm(XID alarm_id) {
  CHECK(sync_counter_alarms_.erase(alarm_id))
      << "Sync counter alarm " << XidStr(alarm_id) << " not registered";
}

bool MockXConnection::QueryPointerPosition(Point* absolute_pos_out) {
  *absolute_pos_out = pointer_pos_;
  return true;
}

bool MockXConnection::SetWindowBackgroundPixmap(XWindow xid, XPixmap pixmap) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->background_pixmap = pixmap;
  return true;
}

bool MockXConnection::GetParentWindow(XWindow xid, XWindow* parent_out) {
  DCHECK(parent_out);
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  *parent_out = info->parent;
  return true;
}

bool MockXConnection::GetChildWindows(XWindow xid,
                                      vector<XWindow>* children_out) {
  CHECK(children_out);
  children_out->clear();

  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;

  // Add the children in bottom-to-top order to match XQueryTree().
  for (list<XWindow>::const_reverse_iterator it =
         stacked_xids_->items().rbegin();
       it != stacked_xids_->items().rend(); ++it) {
    const WindowInfo* child_info = GetWindowInfo(*it);
    CHECK(child_info) << "No info found for window " << *it;
    if (child_info->parent == xid)
      children_out->push_back(*it);
  }
  return true;
}

KeySym MockXConnection::GetKeySymFromKeyCode(KeyCode keycode) {
  map<KeyCode, vector<KeySym> >::iterator it =
      keycodes_to_keysyms_.find(keycode);
  if (it == keycodes_to_keysyms_.end() || it->second.empty())
    return 0;
  return it->second.front();
}

KeyCode MockXConnection::GetKeyCodeFromKeySym(KeySym keysym) {
  return FindWithDefault(keysyms_to_keycodes_, keysym, static_cast<KeyCode>(0));
}

MockXConnection::WindowInfo::WindowInfo(XWindow xid, XWindow parent)
    : xid(xid),
      parent(parent),
      bounds(-1, -1, 1, 1),
      border_width(0),
      depth(24),
      mapped(false),
      override_redirect(false),
      input_only(false),
      redirect_subwindows(false),
      redirected(false),
      event_mask(0),
      transient_for(None),
      cursor(0),
      shape(NULL),
      shape_events_selected(false),
      randr_events_selected(false),
      changed(false),
      num_configures(0),
      background_pixmap(0) {
}

MockXConnection::PixmapInfo::PixmapInfo(XWindow xid,
                                        const Size& size,
                                        int depth)
    : xid(xid),
      size(size),
      depth(depth) {
}

MockXConnection::WindowInfo* MockXConnection::GetWindowInfo(XWindow xid) const {
  map<XWindow, shared_ptr<WindowInfo> >::const_iterator it = windows_.find(xid);
  return (it != windows_.end()) ? it->second.get() : NULL;
}

MockXConnection::PixmapInfo* MockXConnection::GetPixmapInfo(XPixmap xid) const {
  map<XPixmap, shared_ptr<PixmapInfo> >::const_iterator it = pixmaps_.find(xid);
  return (it != pixmaps_.end()) ? it->second.get() : NULL;
}

MockXConnection::SyncCounterAlarmInfo* MockXConnection::GetSyncCounterAlarmInfo(
    XID xid) const {
  map<XID, shared_ptr<SyncCounterAlarmInfo> >::const_iterator it =
      sync_counter_alarms_.find(xid);
  return (it != sync_counter_alarms_.end()) ? it->second.get() : NULL;
}

int64_t MockXConnection::GetSyncCounterValueOrDie(XID counter_id) const {
  map<XID, int64_t>::const_iterator it = sync_counters_.find(counter_id);
  CHECK(it != sync_counters_.end());
  return it->second;
}

void MockXConnection::AddKeyMapping(KeyCode keycode, KeySym keysym) {
  keycodes_to_keysyms_[keycode].push_back(keysym);
  CHECK(keysyms_to_keycodes_.insert(make_pair(keysym, keycode)).second)
      << "Keysym " << keysym << " is already mapped to a keycode";
}

void MockXConnection::RemoveKeyMapping(KeyCode keycode, KeySym keysym) {
  map<KeyCode, vector<KeySym> >::iterator keycode_it =
      keycodes_to_keysyms_.find(keycode);
  CHECK(keycode_it != keycodes_to_keysyms_.end())
      << "Keycode " << keycode << " isn't mapped to anything";
  vector<KeySym>::iterator keysym_vector_it =
      find(keycode_it->second.begin(), keycode_it->second.end(), keysym);
  CHECK(keysym_vector_it != keycode_it->second.end())
      << "Keycode " << keycode << " isn't mapped to keysym " << keysym;
  keycode_it->second.erase(keysym_vector_it);

  map<KeySym, KeyCode>::iterator keysym_it =
      keysyms_to_keycodes_.find(keysym);
  CHECK(keysym_it != keysyms_to_keycodes_.end())
      << "Keysym " << keysym << " isn't mapped";
  CHECK(keysym_it->second == keycode)
      << "Keysym " << keysym << " is mapped to keycode " << keysym_it->second
      << " rather than " << keycode;
  keysyms_to_keycodes_.erase(keysym_it);
}

XWindow MockXConnection::GetWindowBelowWindow(XWindow xid) const {
  const XWindow* other_xid = stacked_xids_->GetUnder(xid);
  return other_xid ? *other_xid : 0;
}

void MockXConnection::AppendEventToQueue(const XEvent& event,
                                         bool write_to_fd) {
  queued_events_.push(event);
  if (write_to_fd && !connection_pipe_has_data_) {
    unsigned char data = 1;
    PCHECK(HANDLE_EINTR(write(connection_pipe_fds_[1], &data, 1)) == 1);
    connection_pipe_has_data_ = true;
  }
}

void MockXConnection::RegisterPropertyCallback(
    XWindow xid, XAtom xatom, Closure* cb) {
  CHECK(cb);
  CHECK(property_callbacks_.insert(
            make_pair(make_pair(xid, xatom), shared_ptr<Closure>(cb))).second);
}

void MockXConnection::InitButtonEvent(XEvent* event,
                                      XWindow xid,
                                      const Point& pos,
                                      int button,
                                      bool press) const {
  CHECK(event);
  const WindowInfo* info = GetWindowInfoOrDie(xid);
  XButtonEvent* button_event = &(event->xbutton);
  memset(button_event, 0, sizeof(*button_event));
  button_event->type = press ? ButtonPress : ButtonRelease;
  button_event->window = info->xid;
  button_event->x = pos.x;
  button_event->y = pos.y;
  button_event->x_root = info->bounds.x + pos.x;
  button_event->y_root = info->bounds.y + pos.y;
  button_event->button = button;
}

void MockXConnection::InitKeyEvent(XEvent* event,
                                   XWindow xid,
                                   KeyCode key_code,
                                   uint32_t modifiers,
                                   XTime time,
                                   bool press) const {
  CHECK(event);
  XKeyEvent* key_event = &(event->xkey);
  memset(key_event, 0, sizeof(*key_event));
  key_event->type = press ? KeyPress : KeyRelease;
  key_event->window = xid;
  key_event->state = modifiers;
  key_event->keycode = key_code;
  key_event->time = time;
}

void MockXConnection::InitClientMessageEvent(XEvent* event,
                                             XWindow xid,
                                             XAtom type,
                                             long arg1,
                                             long arg2,
                                             long arg3,
                                             long arg4,
                                             long arg5) const {
  CHECK(event);
  XClientMessageEvent* client_event = &(event->xclient);
  memset(client_event, 0, sizeof(*client_event));
  client_event->type = ClientMessage;
  client_event->window = xid;
  client_event->message_type = type;
  client_event->format = kLongFormat;
  client_event->data.l[0] = arg1;
  client_event->data.l[1] = arg2;
  client_event->data.l[2] = arg3;
  client_event->data.l[3] = arg4;
  client_event->data.l[4] = arg5;
}

void MockXConnection::InitConfigureNotifyEvent(XEvent* event,
                                               XWindow xid) const {
  CHECK(event);
  const WindowInfo* info = GetWindowInfoOrDie(xid);
  XConfigureEvent* conf_event = &(event->xconfigure);
  memset(conf_event, 0, sizeof(*conf_event));
  conf_event->type = ConfigureNotify;
  conf_event->window = info->xid;
  conf_event->above = GetWindowBelowWindow(xid);
  conf_event->override_redirect = info->override_redirect;
  conf_event->x = info->bounds.x;
  conf_event->y = info->bounds.y;
  conf_event->width = info->bounds.width;
  conf_event->height = info->bounds.height;
}

void MockXConnection::InitConfigureRequestEvent(XEvent* event,
                                                XWindow xid,
                                                const Rect& bounds) const {
  CHECK(event);
  XConfigureRequestEvent* conf_event = &(event->xconfigurerequest);
  memset(conf_event, 0, sizeof(*conf_event));
  conf_event->type = ConfigureRequest;
  conf_event->window = xid;
  conf_event->x = bounds.x;
  conf_event->y = bounds.y;
  conf_event->width = bounds.width;
  conf_event->height = bounds.height;
  conf_event->value_mask = CWX | CWY | CWWidth | CWHeight;
}

void MockXConnection::InitCreateWindowEvent(XEvent* event, XWindow xid) const {
  CHECK(event);
  const WindowInfo* info = GetWindowInfoOrDie(xid);
  XCreateWindowEvent* create_event = &(event->xcreatewindow);
  memset(create_event, 0, sizeof(*create_event));
  create_event->type = CreateNotify;
  create_event->parent = info->parent;
  create_event->window = info->xid;
  create_event->x = info->bounds.x;
  create_event->y = info->bounds.y;
  create_event->width = info->bounds.width;
  create_event->height = info->bounds.height;
  create_event->border_width = info->border_width;
  create_event->override_redirect = info->override_redirect ? True : False;
}

void MockXConnection::InitDamageNotifyEvent(XEvent* event,
                                            XWindow drawable,
                                            const Rect& bounds) const {
  CHECK(event);
  XDamageNotifyEvent* damage_event =
      reinterpret_cast<XDamageNotifyEvent*>(event);
  memset(damage_event, 0, sizeof(*damage_event));
  damage_event->type = damage_event_base_ + XDamageNotify;
  damage_event->drawable = drawable;
  damage_event->area.x = bounds.x;
  damage_event->area.y = bounds.y;
  damage_event->area.width = bounds.width;
  damage_event->area.height = bounds.height;
}

void MockXConnection::InitDestroyWindowEvent(XEvent* event, XWindow xid) const {
  CHECK(event);
  XDestroyWindowEvent* destroy_event = &(event->xdestroywindow);
  memset(destroy_event, 0, sizeof(*destroy_event));
  destroy_event->type = DestroyNotify;
  destroy_event->window = xid;
}

void MockXConnection::InitEnterOrLeaveWindowEvent(XEvent* event,
                                                  XWindow xid,
                                                  const Point& pos,
                                                  bool enter) const {
  CHECK(event);
  const WindowInfo* info = GetWindowInfoOrDie(xid);
  XEnterWindowEvent* enter_event = &(event->xcrossing);
  memset(enter_event, 0, sizeof(*enter_event));
  enter_event->type = enter ? EnterNotify : LeaveNotify;
  enter_event->window = info->xid;
  enter_event->x = pos.x;
  enter_event->y = pos.y;
  enter_event->x_root = info->bounds.x + pos.x;
  enter_event->y_root = info->bounds.y + pos.y;
  // Leave everything else blank for now; we don't use it.
}

void MockXConnection::InitMapEvent(XEvent* event, XWindow xid) const {
  CHECK(event);
  XMapEvent* map_event = &(event->xmap);
  memset(map_event, 0, sizeof(*map_event));
  map_event->type = MapNotify;
  map_event->window = xid;
}

void MockXConnection::InitMapRequestEvent(XEvent* event, XWindow xid) const {
  CHECK(event);
  const WindowInfo* info = GetWindowInfoOrDie(xid);
  XMapRequestEvent* req_event = &(event->xmaprequest);
  memset(req_event, 0, sizeof(*req_event));
  req_event->type = MapRequest;
  req_event->window = info->xid;
  req_event->parent = info->parent;
}

void MockXConnection::InitMotionNotifyEvent(XEvent* event,
                                            XWindow xid,
                                            const Point& pos) const {
  CHECK(event);
  const WindowInfo* info = GetWindowInfoOrDie(xid);
  XMotionEvent* motion_event = &(event->xmotion);
  memset(motion_event, 0, sizeof(*motion_event));
  motion_event->type = MotionNotify;
  motion_event->window = info->xid;
  motion_event->x = pos.x;
  motion_event->y = pos.y;
  motion_event->x_root = info->bounds.x + pos.x;
  motion_event->y_root = info->bounds.y + pos.y;
  // Leave everything else blank for now; we don't use it.
}

void MockXConnection::InitPropertyNotifyEvent(XEvent* event,
                                              XWindow xid,
                                              XAtom xatom) const {
  CHECK(event);
  XPropertyEvent* property_event = &(event->xproperty);
  memset(property_event, 0, sizeof(*property_event));
  property_event->type = PropertyNotify;
  property_event->window = xid;
  property_event->atom = xatom;
  property_event->state = PropertyNewValue;
}

void MockXConnection::InitSyncAlarmNotifyEvent(XEvent* event,
                                               XID alarm_xid,
                                               int64_t value) const {
  CHECK(event);
  XSyncAlarmNotifyEvent* alarm_event =
      reinterpret_cast<XSyncAlarmNotifyEvent*>(event);
  memset(alarm_event, 0, sizeof(*alarm_event));
  alarm_event->type = sync_event_base_ + XSyncAlarmNotify;
  alarm_event->alarm = alarm_xid;
  x_connection_internal::StoreInt64InXSyncValue(
      value, &(alarm_event->counter_value));
}

void MockXConnection::InitUnmapEvent(XEvent* event, XWindow xid) const {
  CHECK(event);
  XUnmapEvent* unmap_event = &(event->xunmap);
  memset(unmap_event, 0, sizeof(*unmap_event));
  unmap_event->type = UnmapNotify;
  unmap_event->window = xid;
}

void MockXConnection::GetEventInternal(XEvent* event, bool remove_from_queue) {
  CHECK(event);
  CHECK(!queued_events_.empty())
      << "GetEventInternal() called while no events are queued in "
      << "single-threaded testing code -- we would block forever";
  *event = queued_events_.front();
  if (remove_from_queue)
    queued_events_.pop();

  if (connection_pipe_has_data_) {
    unsigned char data = 0;
    PCHECK(HANDLE_EINTR(read(connection_pipe_fds_[0], &data, 1)) == 1);
    connection_pipe_has_data_ = false;
  }
}

}  // namespace window_manager
