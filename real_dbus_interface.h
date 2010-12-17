// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_REAL_DBUS_INTERFACE_H_
#define WINDOW_MANAGER_REAL_DBUS_INTERFACE_H_

#include <string>

#include "window_manager/dbus_interface.h"

struct DBusConnection;

namespace window_manager {

// Real implementation of DBusInterface.
class RealDBusInterface : public DBusInterface {
 public:
  RealDBusInterface();
  virtual ~RealDBusInterface();

  // DBusInterface implementation.
  virtual bool Init();
  virtual bool CallMethod(const std::string& target,
                          const std::string& object,
                          const std::string& interface,
                          const std::string& method);

 private:
  // Connection to the system bus.
  DBusConnection* connection_;

  DISALLOW_COPY_AND_ASSIGN(RealDBusInterface);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_REAL_DBUS_INTERFACE_H_
