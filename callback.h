// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_CALLBACK_H_
#define WINDOW_MANAGER_CALLBACK_H_

#include <google/protobuf/stubs/common.h>

// We currently use the callbacks from the protobuf code.
// TODO: Better callbacks.

namespace window_manager {

using google::protobuf::Closure;
using google::protobuf::NewPermanentCallback;
using google::protobuf::NewCallback;

}  // namespace window_manager

#endif  // WINDOW_MANAGER_CALLBACK_H_
