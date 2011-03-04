// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/x11/x_connection.h"

#include "window_manager/util.h"

using base::TimeDelta;
using std::string;
using std::vector;
using window_manager::util::GetMonotonicTime;

namespace window_manager {

const int XConnection::kByteFormat = 8;
const int XConnection::kLongFormat = 32;
const int XConnection::kMaxPosition = 32767;

XAtom XConnection::GetAtomOrDie(const std::string& name) {
  XAtom atom = 0;
  CHECK(GetAtom(name, &atom));
  return atom;
}

bool XConnection::GetIntProperty(XWindow xid, XAtom xatom, int* value) {
  CHECK(value);
  vector<int> values;
  if (!GetIntArrayProperty(xid, xatom, &values)) {
    return false;
  }

  CHECK(!values.empty());  // guaranteed by GetIntArrayProperty()
  if (values.size() > 1) {
    LOG(WARNING) << "GetIntProperty() called for property " << xatom
                 << " with " << values.size() << " values; just returning "
                 << "the first";
  }
  *value = values[0];
  return true;
}

bool XConnection::GrabServer() {
  DCHECK(!server_grabbed_) << "Attempting to grab already-grabbed server";
  if (GrabServerImpl()) {
    server_grabbed_ = true;
    server_grab_time_ = GetMonotonicTime();
    return true;
  }
  return false;
}

bool XConnection::UngrabServer() {
  DCHECK(server_grabbed_) << "Attempting to ungrab not-grabbed server";
  if (UngrabServerImpl()) {
    server_grabbed_ = false;
    TimeDelta delta = GetMonotonicTime() - server_grab_time_;
    DLOG(INFO) << "Server ungrabbed; duration was "
               << delta.InMilliseconds() << " ms";
    return true;
  }
  return false;
}

XConnection::ScopedServerGrab* XConnection::CreateScopedServerGrab() {
  return new ScopedServerGrab(this);
}

bool XConnection::GetAtom(const string& name, XAtom* atom_out) {
  vector<string> names;
  names.push_back(name);
  vector<XAtom> atoms;
  if (!GetAtoms(names, &atoms))
    return false;

  CHECK(atoms.size() == 1);
  *atom_out = atoms[0];
  return true;
}

bool XConnection::SetIntProperty(
    XWindow xid, XAtom xatom, XAtom type, int value) {
  vector<int> values(1, value);
  return SetIntArrayProperty(xid, xatom, type, values);
}

}  // namespace window_manager
