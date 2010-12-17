// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_MOCK_DBUS_INTERFACE_H_
#define WINDOW_MANAGER_MOCK_DBUS_INTERFACE_H_

#include <string>
#include <vector>

#include "window_manager/dbus_interface.h"

namespace window_manager {

// Mock implementation of DBusInterface for use by tests.
class MockDBusInterface : public DBusInterface {
 public:
  // Simple struct representing a D-Bus message with no parameters.
  struct Message {
    Message(const std::string& target,
            const std::string& object,
            const std::string& interface,
            const std::string& method)
        : target(target),
          object(object),
          interface(interface),
          method(method) {
    }
    std::string target;
    std::string object;
    std::string interface;
    std::string method;
  };

  MockDBusInterface();
  virtual ~MockDBusInterface() {}

  const std::vector<Message>& sent_messages() const { return sent_messages_; }

  // DBusInterface implementation.
  virtual bool Init();
  virtual bool CallMethod(const std::string& target,
                          const std::string& object,
                          const std::string& interface,
                          const std::string& method);

 private:
  // Has Init() been called?
  bool connected_;

  // Messages that have been sent.
  std::vector<Message> sent_messages_;

  DISALLOW_COPY_AND_ASSIGN(MockDBusInterface);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_MOCK_DBUS_INTERFACE_H_
