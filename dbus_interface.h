// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_DBUS_INTERFACE_H_
#define WINDOW_MANAGER_DBUS_INTERFACE_H_

#include <string>

#include "base/basictypes.h"

namespace window_manager {

// Interface for communication via D-Bus.
//
// This class currently only supports sending simple messages.
class DBusInterface {
 public:
  DBusInterface() {}
  virtual ~DBusInterface() {}

  // Open a connection to the system bus, returning false on failure.
  virtual bool Init() = 0;

  // Invoke a method.
  // |target| will be similar to "org.chromium.SessionManager",
  // |object| will be similar to "/org/chromium/SessionManager",
  // |interface| will be similar to "org.chromium.SessionManagerInterface", and
  // |method| will be similar to "EmitLoginPromptVisible".
  // Returns true on success and false otherwise.
  virtual bool CallMethod(const std::string& target,
                          const std::string& object,
                          const std::string& interface,
                          const std::string& method) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(DBusInterface);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_DBUS_INTERFACE_H_
