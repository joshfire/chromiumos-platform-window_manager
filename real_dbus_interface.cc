// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/real_dbus_interface.h"

#include <dbus/dbus.h>

#include "base/logging.h"

using std::string;

namespace window_manager {

RealDBusInterface::RealDBusInterface() : connection_(NULL) {
}

RealDBusInterface::~RealDBusInterface() {
  if (connection_) {
    dbus_connection_unref(connection_);
    connection_ = NULL;
  }
}

bool RealDBusInterface::Init() {
  DCHECK(!connection_) << "Already connected";
  DBusError error;
  dbus_error_init(&error);

  LOG(INFO) << "Connecting to D-Bus system bus";
  connection_ = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
  if (dbus_error_is_set(&error)) {
    LOG(ERROR) << "Got connection error: " << error.message;
    dbus_error_free(&error);
  }
  if (!connection_) {
    LOG(ERROR) << "Unable to connect";
    return false;
  }
  LOG(INFO) << "Connection established";
  return true;
}

bool RealDBusInterface::CallMethod(const string& target,
                                   const string& object,
                                   const string& interface,
                                   const string& method) {
  if (!connection_) {
    LOG(WARNING) << "Ignoring request to call method "
                 << interface << "." << method << " while disconnected";
    return false;
  }

  DLOG(INFO) << "Calling " << interface << "." << method;
  DBusMessage* msg = dbus_message_new_method_call(target.c_str(),
                                                  object.c_str(),
                                                  interface.c_str(),
                                                  method.c_str());
  if (!msg) {
    LOG(ERROR) << "Creation of " << interface << "." << method
               << " message failed";
    return false;
  }

  dbus_uint32_t serial = 0;
  if (!dbus_connection_send(connection_, msg, &serial)) {
    LOG(ERROR) << "Calling " << interface << "." << method << " failed";
    dbus_message_unref(msg);
    return false;
  }

  dbus_connection_flush(connection_);
  dbus_message_unref(msg);
  DLOG(INFO) << "Finished sending message";
  return true;
}

}  // namespace window_manager
