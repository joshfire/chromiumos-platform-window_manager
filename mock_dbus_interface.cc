// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/mock_dbus_interface.h"

#include "base/logging.h"

using std::string;

namespace window_manager {

MockDBusInterface::MockDBusInterface() : connected_(false) {
}

bool MockDBusInterface::Init() {
  DCHECK(!connected_);
  connected_ = true;
  return true;
}

bool MockDBusInterface::CallMethod(const string& target,
                                   const string& object,
                                   const string& interface,
                                   const string& method) {
  DCHECK(connected_);
  sent_messages_.push_back(Message(target, object, interface, method));
  return true;
}

}  // namespace window_manager
