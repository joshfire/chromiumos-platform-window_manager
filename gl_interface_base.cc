// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/gl_interface_base.h"

#include <string>
#include <vector>

using std::string;
using std::vector;

namespace window_manager {

void GLInterfaceBase::ParseExtensionString(vector<string>* out,
                                           const char* extensions) {
  string ext(extensions);
  for (string::size_type pos = 0; pos != string::npos;) {
    string::size_type last_pos = ext.find_first_of(" ", pos);
    out->push_back(ext.substr(pos, last_pos - pos));
    pos = ext.find_first_not_of(" ", last_pos);
  }
}

bool GLInterfaceBase::HasExtension(const vector<string>& extensions,
                                   const char* extension) {
  for (vector<string>::const_iterator i = extensions.begin();
       i != extensions.end(); ++i) {
    if (*i == extension)
      return true;
  }
  return false;
}

}  // namespace window_manager
