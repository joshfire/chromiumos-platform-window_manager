// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/mock_x_connection.h"

#include <fcntl.h>
#include <unistd.h>

#include <list>

#include "base/logging.h"
#include "base/eintr_wrapper.h"
#include "window_manager/util.h"

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

MockXConnection::MockXConnection()
    : windows_(),
      stacked_xids_(new Stacker<XWindow>),
      next_window_(1),
      next_pixmap_(100000),
      root_(CreateWindow(None, 0, 0,
                         kDisplayWidth, kDisplayHeight, true, false, 0)),
      overlay_(CreateWindow(root_, 0, 0,
                            kDisplayWidth, kDisplayHeight, true, false, 0)),
      next_atom_(1000),
      focused_xid_(None),
      last_focus_timestamp_(0),
      current_time_(0),
      pointer_grab_xid_(None),
      num_keymap_refreshes_(0),
      pointer_x_(0),
      pointer_y_(0),
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
}

MockXConnection::~MockXConnection() {
  PCHECK(HANDLE_EINTR(close(connection_pipe_fds_[0])) != -1);
  PCHECK(HANDLE_EINTR(close(connection_pipe_fds_[1])) != -1);
}

bool MockXConnection::GetWindowGeometry(XWindow xid, WindowGeometry* geom_out) {
  CHECK(geom_out);
  WindowInfo* info = GetWindowInfo(xid);
  if (!info) {
    // Maybe this is a compositing pixmap for a window.  If so, just use
    // the window's geometry instead.
    XWindow window_xid = FindWithDefault(
        pixmap_to_window_, static_cast<XID>(xid), static_cast<XWindow>(0));
    info = GetWindowInfo(window_xid);
    if (!info)
      return false;
  }
  geom_out->x = info->x;
  geom_out->y = info->y;
  geom_out->width = info->width;
  geom_out->height = info->height;
  geom_out->border_width = info->border_width;
  geom_out->depth = info->depth;
  return true;
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

bool MockXConnection::MoveWindow(XWindow xid, int x, int y) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->x = x;
  info->y = y;
  info->changed = true;
  info->num_configures++;
  return true;
}

bool MockXConnection::ResizeWindow(XWindow xid, int width, int height) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->width = width;
  info->height = height;
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

bool MockXConnection::AddPointerGrabForWindow(
    XWindow xid, int event_mask, XTime timestamp) {
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

bool MockXConnection::RemovePointerGrab(bool replay_events, XTime timestamp) {
  pointer_grab_xid_ = None;
  if (replay_events)
    num_pointer_ungrabs_with_replayed_events_++;
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

bool MockXConnection::GetTransientHintForWindow(
    XWindow xid, XWindow* owner_out) {
  CHECK(owner_out);
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  *owner_out = info->transient_for;
  return true;
}

bool MockXConnection::GetWindowAttributes(
    XWindow xid, WindowAttributes* attr_out) {
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

bool MockXConnection::UnredirectWindowForCompositing(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->redirected = false;
  return true;
}

XPixmap MockXConnection::GetCompositingPixmapForWindow(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return 0;
  return info->compositing_pixmap;
}

XWindow MockXConnection::CreateWindow(
    XWindow parent,
    int x, int y,
    int width, int height,
    bool override_redirect,
    bool input_only,
    int event_mask) {
  XWindow xid = next_window_;
  next_window_++;
  shared_ptr<WindowInfo> info(new WindowInfo(xid, parent));
  info->x = x;
  info->y = y;
  info->width = width;
  info->height = height;
  info->override_redirect = override_redirect;
  info->input_only = input_only;
  info->event_mask = event_mask;
  info->compositing_pixmap = next_pixmap_++;
  CHECK(!GetWindowInfo(info->compositing_pixmap));

  windows_[xid] = info;
  pixmap_to_window_[info->compositing_pixmap] = xid;
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
  pixmap_to_window_.erase(it->second->compositing_pixmap);
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
  if (info->shape.get())
    bytemap->Copy(*(info->shape.get()));
  else
    bytemap->SetRectangle(0, 0, info->width, info->height, 0xff);
  return true;
}

bool MockXConnection::SelectRandREventsOnWindow(XWindow xid) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->randr_events_selected = true;
  return true;
}

bool MockXConnection::GetAtoms(
    const vector<string>& names, vector<XAtom>* atoms_out) {
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

bool MockXConnection::GetIntArrayProperty(
    XWindow xid, XAtom xatom, vector<int>* values) {
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

bool MockXConnection::SetIntArrayProperty(
    XWindow xid, XAtom xatom, XAtom type, const vector<int>& values) {
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

bool MockXConnection::SetStringProperty(
    XWindow xid, XAtom xatom, const string& value) {
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
  XClientMessageEvent* client_event = &(event.xclient);
  client_event->type = ClientMessage;
  client_event->window = xid;
  client_event->message_type = message_type;
  client_event->format = XConnection::kLongFormat;
  memcpy(client_event->data.l, data, sizeof(client_event->data.l));
  info->client_messages.push_back(event.xclient);
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

bool MockXConnection::SetWindowCursor(XWindow xid, uint32 shape) {
  WindowInfo* info = GetWindowInfo(xid);
  if (!info)
    return false;
  info->cursor = shape;
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

bool MockXConnection::QueryPointerPosition(int* x_root, int* y_root) {
  if (x_root)
    *x_root = pointer_x_;
  if (y_root)
    *y_root = pointer_y_;
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
      x(-1),
      y(-1),
      width(1),
      height(1),
      border_width(0),
      depth(32),
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
      compositing_pixmap(None) {
}

MockXConnection::WindowInfo::~WindowInfo() {}

MockXConnection::WindowInfo* MockXConnection::GetWindowInfo(XWindow xid) const {
  map<XWindow, shared_ptr<WindowInfo> >::const_iterator it = windows_.find(xid);
  return (it != windows_.end()) ? it->second.get() : NULL;
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

void MockXConnection::InitButtonEvent(
    XEvent* event, XWindow xid, int x, int y, int button, bool press) const {
  CHECK(event);
  const WindowInfo* info = GetWindowInfoOrDie(xid);
  XButtonEvent* button_event = &(event->xbutton);
  memset(button_event, 0, sizeof(*button_event));
  button_event->type = press ? ButtonPress : ButtonRelease;
  button_event->window = info->xid;
  button_event->x = x;
  button_event->y = y;
  button_event->x_root = info->x + x;
  button_event->y_root = info->y + y;
  button_event->button = button;
}

void MockXConnection::InitKeyEvent(XEvent* event, XWindow xid,
                                   unsigned int keycode,
                                   unsigned int key_mask,
                                   XTime time,
                                   bool press) const {
  CHECK(event);
  XKeyEvent* key_event = &(event->xkey);
  memset(key_event, 0, sizeof(*key_event));
  key_event->type = press ? KeyPress : KeyRelease;
  key_event->window = xid;
  key_event->state = key_mask;
  key_event->keycode = keycode;
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
  conf_event->x = info->x;
  conf_event->y = info->y;
  conf_event->width = info->width;
  conf_event->height = info->height;
}

void MockXConnection::InitConfigureRequestEvent(
    XEvent* event, XWindow xid, int x, int y, int width, int height) const {
  CHECK(event);
  XConfigureRequestEvent* conf_event = &(event->xconfigurerequest);
  memset(conf_event, 0, sizeof(*conf_event));
  conf_event->type = ConfigureRequest;
  conf_event->window = xid;
  conf_event->x = x;
  conf_event->y = y;
  conf_event->width = width;
  conf_event->height = height;
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
  create_event->x = info->x;
  create_event->y = info->y;
  create_event->width = info->width;
  create_event->height = info->height;
  create_event->border_width = info->border_width;
  create_event->override_redirect = info->override_redirect ? True : False;
}

void MockXConnection::InitDamageNotifyEvent(XEvent* event, XWindow drawable,
                                            int x, int y,
                                            int width, int height) const {
  CHECK(event);
  XDamageNotifyEvent* damage_event =
      reinterpret_cast<XDamageNotifyEvent*>(event);
  memset(damage_event, 0, sizeof(*damage_event));
  damage_event->type = damage_event_base_ + XDamageNotify;
  damage_event->drawable = drawable;
  damage_event->area.x = x;
  damage_event->area.y = y;
  damage_event->area.width = width;
  damage_event->area.height = height;
}

void MockXConnection::InitDestroyWindowEvent(XEvent* event, XWindow xid) const {
  CHECK(event);
  XDestroyWindowEvent* destroy_event = &(event->xdestroywindow);
  memset(destroy_event, 0, sizeof(*destroy_event));
  destroy_event->type = DestroyNotify;
  destroy_event->window = xid;
}

void MockXConnection::InitEnterOrLeaveWindowEvent(
    XEvent* event, XWindow xid, int x, int y, bool enter) const {
  CHECK(event);
  const WindowInfo* info = GetWindowInfoOrDie(xid);
  XEnterWindowEvent* enter_event = &(event->xcrossing);
  memset(enter_event, 0, sizeof(*enter_event));
  enter_event->type = enter ? EnterNotify : LeaveNotify;
  enter_event->window = info->xid;
  enter_event->x = x;
  enter_event->y = y;
  enter_event->x_root = info->x + x;
  enter_event->y_root = info->y + y;
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

void MockXConnection::InitMotionNotifyEvent(XEvent* event, XWindow xid,
                                            int x, int y) const {
  CHECK(event);
  const WindowInfo* info = GetWindowInfoOrDie(xid);
  XMotionEvent* motion_event = &(event->xmotion);
  memset(motion_event, 0, sizeof(*motion_event));
  motion_event->type = MotionNotify;
  motion_event->window = info->xid;
  motion_event->x = x;
  motion_event->y = y;
  motion_event->x_root = info->x + x;
  motion_event->y_root = info->y + y;
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
