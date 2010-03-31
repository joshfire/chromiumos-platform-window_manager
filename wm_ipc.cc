// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/wm_ipc.h"

#include <cstring>

#include "window_manager/atom_cache.h"
#include "window_manager/util.h"
#include "window_manager/x_connection.h"

using std::string;
using std::vector;

namespace window_manager {

WmIpc::WmIpc(XConnection* xconn, AtomCache* cache)
    : xconn_(xconn),
      atom_cache_(cache),
      wm_window_(xconn_->GetSelectionOwner(atom_cache_->GetXAtom(ATOM_WM_S0))) {
  LOG(INFO) << "Window manager window is " << XidStr(wm_window_);
}

bool WmIpc::GetWindowType(XWindow xid, WindowType* type, vector<int>* params) {
  CHECK(type);
  CHECK(params);

  params->clear();
  vector<int> values;
  if (!xconn_->GetIntArrayProperty(
          xid, atom_cache_->GetXAtom(ATOM_CHROME_WINDOW_TYPE), &values)) {
    return false;
  }
  CHECK(!values.empty());
  *type = static_cast<WindowType>(values[0]);
  for (size_t i = 1; i < values.size(); ++i) {
    params->push_back(values[i]);
  }
  return true;
}

bool WmIpc::SetWindowType(
    XWindow xid, WindowType type, const vector<int>* params) {
  CHECK(type >= 0);
  CHECK(type < kNumWindowTypes);

  vector<int> values;
  values.push_back(type);
  if (params) {
    for (size_t i = 0; i < params->size(); ++i) {
      values.push_back((*params)[i]);
    }
  }
  return xconn_->SetIntArrayProperty(
      xid, atom_cache_->GetXAtom(ATOM_CHROME_WINDOW_TYPE),
      atom_cache_->GetXAtom(ATOM_CARDINAL), values);
}

bool WmIpc::SetSystemMetricsProperty(XWindow xid, const string& metrics) {
  return xconn_->SetStringProperty(
      xid, atom_cache_->GetXAtom(ATOM_WM_SYSTEM_METRICS), metrics);
}

bool WmIpc::GetMessage(XWindow xid,
                       XAtom message_type,
                       int format,
                       const long data[5],
                       Message* msg_out) {
  CHECK(msg_out);

  // Skip other types of client messages.
  if (message_type != atom_cache_->GetXAtom(ATOM_CHROME_WM_MESSAGE)) {
    return false;
  }

  if (format != XConnection::kLongFormat) {
    LOG(WARNING) << "Ignoring Chrome OS ClientEvent message with invalid bit "
                 << "format " << format << " (expected 32-bit values)";
    return false;
  }

  msg_out->set_type(static_cast<Message::Type>(data[0]));
  if (msg_out->type() < 0 || msg_out->type() >= Message::kNumTypes) {
    LOG(WARNING) << "Ignoring Chrome OS ClientEventMessage with invalid "
                 << "message type " << msg_out->type();
    return false;
  }

  msg_out->set_xid(xid);

  // ClientMessage events only have five 32-bit items, and we're using the
  // first one for our message type.
  CHECK(msg_out->max_params() <= 4);
  for (int i = 0; i < msg_out->max_params(); ++i) {
    msg_out->set_param(i, data[i+1]);  // l[0] contains message type
  }
  return true;
}

bool WmIpc::SendMessage(XWindow xid, const Message& msg) {
  DLOG(INFO) << "Sending message of type " << msg.type() << " to "
             << XidStr(xid);

  long data[5];
  memset(data, 0, sizeof(data));
  data[0] = msg.type();
  // XClientMessageEvent only gives us five 32-bit items, and we're using
  // the first one for our message type.
  CHECK(msg.max_params() <= 4);
  for (int i = 0; i < msg.max_params(); ++i)
    data[i+1] = msg.param(i);

  return xconn_->SendClientMessageEvent(
             xid,  // destination window
             xid,  // window field in event
             atom_cache_->GetXAtom(ATOM_CHROME_WM_MESSAGE),
             data,
             0);   // event_mask
}

}  // namespace window_manager
