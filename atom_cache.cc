// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/atom_cache.h"

#include <vector>

#include "base/logging.h"
#include "window_manager/util.h"
#include "window_manager/x11/x_connection.h"

using base::hash_map;
using std::string;
using std::vector;
using window_manager::util::FindWithDefault;
using window_manager::util::XidStr;

namespace window_manager {

// A value from the Atom enum and the actual name that should be used to
// look up its ID on the X server.
struct AtomInfo {
  Atom atom;
  const char* name;
};

// Each value from the Atom enum must be present here.
static const AtomInfo kAtomInfos[] = {
  { ATOM_ATOM,                         "ATOM" },
  { ATOM_CARDINAL,                     "CARDINAL" },
  { ATOM_CHROME_GET_SERVER_TIME,       "_CHROME_GET_SERVER_TIME" },
  { ATOM_CHROME_FREEZE_UPDATES,        "_CHROME_FREEZE_UPDATES" },
  { ATOM_CHROME_LOGGED_IN,             "_CHROME_LOGGED_IN" },
  { ATOM_CHROME_STATE,                 "_CHROME_STATE" },
  { ATOM_CHROME_STATE_COLLAPSED_PANEL, "_CHROME_STATE_COLLAPSED_PANEL" },
  { ATOM_CHROME_VIDEO_TIME,            "_CHROME_VIDEO_TIME" },
  { ATOM_CHROME_WINDOW_TYPE,           "_CHROME_WINDOW_TYPE" },
  { ATOM_CHROME_WM_MESSAGE,            "_CHROME_WM_MESSAGE" },
  { ATOM_MANAGER,                      "MANAGER" },
  { ATOM_NET_ACTIVE_WINDOW,            "_NET_ACTIVE_WINDOW" },
  { ATOM_NET_CLIENT_LIST,              "_NET_CLIENT_LIST" },
  { ATOM_NET_CLIENT_LIST_STACKING,     "_NET_CLIENT_LIST_STACKING" },
  { ATOM_NET_CURRENT_DESKTOP,          "_NET_CURRENT_DESKTOP" },
  { ATOM_NET_DESKTOP_GEOMETRY,         "_NET_DESKTOP_GEOMETRY" },
  { ATOM_NET_DESKTOP_VIEWPORT,         "_NET_DESKTOP_VIEWPORT" },
  { ATOM_NET_NUMBER_OF_DESKTOPS,       "_NET_NUMBER_OF_DESKTOPS" },
  { ATOM_NET_SUPPORTED,                "_NET_SUPPORTED" },
  { ATOM_NET_SUPPORTING_WM_CHECK,      "_NET_SUPPORTING_WM_CHECK" },
  { ATOM_NET_WM_CM_S0,                 "_NET_WM_CM_S0" },
  { ATOM_NET_WM_MOVERESIZE,            "_NET_WM_MOVERESIZE" },
  { ATOM_NET_WM_NAME,                  "_NET_WM_NAME" },
  { ATOM_NET_WM_PID,                   "_NET_WM_PID" },
  { ATOM_NET_WM_PING,                  "_NET_WM_PING" },
  { ATOM_NET_WM_STATE,                 "_NET_WM_STATE" },
  { ATOM_NET_WM_STATE_FULLSCREEN,      "_NET_WM_STATE_FULLSCREEN" },
  { ATOM_NET_WM_STATE_MAXIMIZED_HORZ,  "_NET_WM_STATE_MAXIMIZED_HORZ" },
  { ATOM_NET_WM_STATE_MAXIMIZED_VERT,  "_NET_WM_STATE_MAXIMIZED_VERT" },
  { ATOM_NET_WM_STATE_MODAL,           "_NET_WM_STATE_MODAL" },
  { ATOM_NET_WM_SYNC_REQUEST,          "_NET_WM_SYNC_REQUEST" },
  { ATOM_NET_WM_SYNC_REQUEST_COUNTER,  "_NET_WM_SYNC_REQUEST_COUNTER" },
  { ATOM_NET_WM_USER_TIME,             "_NET_WM_USER_TIME" },
  { ATOM_NET_WM_WINDOW_OPACITY,        "_NET_WM_WINDOW_OPACITY" },
  { ATOM_NET_WM_WINDOW_TYPE,           "_NET_WM_WINDOW_TYPE" },
  { ATOM_NET_WM_WINDOW_TYPE_COMBO,     "_NET_WM_WINDOW_TYPE_COMBO" },
  { ATOM_NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
                                       "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU" },
  { ATOM_NET_WM_WINDOW_TYPE_MENU,      "_NET_WM_WINDOW_TYPE_MENU" },
  { ATOM_NET_WM_WINDOW_TYPE_POPUP_MENU,
                                       "_NET_WM_WINDOW_TYPE_POPUP_MENU" },
  { ATOM_NET_WM_WINDOW_TYPE_TOOLTIP,   "_NET_WM_WINDOW_TYPE_TOOLTIP" },
  { ATOM_NET_WORKAREA,                 "_NET_WORKAREA" },
  { ATOM_PRIMARY,                      "PRIMARY" },
  { ATOM_WM_CLIENT_MACHINE,            "WM_CLIENT_MACHINE" },
  { ATOM_WM_DELETE_WINDOW,             "WM_DELETE_WINDOW" },
  { ATOM_WM_HINTS,                     "WM_HINTS" },
  { ATOM_WM_NAME,                      "WM_NAME" },
  { ATOM_WM_NORMAL_HINTS,              "WM_NORMAL_HINTS" },
  { ATOM_WM_PROTOCOLS,                 "WM_PROTOCOLS" },
  { ATOM_WM_S0,                        "WM_S0" },
  { ATOM_WM_STATE,                     "WM_STATE" },
  { ATOM_WM_TAKE_FOCUS,                "WM_TAKE_FOCUS" },
  { ATOM_WM_TRANSIENT_FOR,             "WM_TRANSIENT_FOR" },
};

AtomCache::AtomCache(XConnection* xconn)
    : xconn_(xconn) {
  CHECK(xconn_);

  CHECK(sizeof(kAtomInfos) / sizeof(AtomInfo) == kNumAtoms)
      << "Each value in the Atom enum in atom_cache.h must have "
      << "a mapping in kAtomInfos in atom_cache.cc";
  vector<string> names;
  vector<XAtom> xatoms;

  for (int i = 0; i < kNumAtoms; ++i)
    names.push_back(kAtomInfos[i].name);

  CHECK(xconn_->GetAtoms(names, &xatoms));
  CHECK(xatoms.size() == kNumAtoms);

  for (size_t i = 0; i < kNumAtoms; ++i) {
    LOG(INFO) << "Registering atom " << XidStr(xatoms[i])
              << " (" << kAtomInfos[i].name << ")";
    atom_to_xatom_[kAtomInfos[i].atom] = xatoms[i];
    xatom_to_string_[xatoms[i]] = kAtomInfos[i].name;
  }
}

XAtom AtomCache::GetXAtom(Atom atom) const {
  XAtom xatom = FindWithDefault(atom_to_xatom_,
                                static_cast<int>(atom),
                                static_cast<XAtom>(0));
  CHECK(xatom) << "Couldn't find X atom for Atom " << XidStr(atom);
  return xatom;
}

const string& AtomCache::GetName(XAtom xatom) {
  hash_map<XAtom, string>::const_iterator it = xatom_to_string_.find(xatom);
  if (it != xatom_to_string_.end())
    return it->second;

  string name;
  if (!xconn_->GetAtomName(xatom, &name)) {
    LOG(ERROR) << "Unable to look up name for atom " << XidStr(xatom);
    static const string kEmptyName = "";
    return kEmptyName;
  }
  return xatom_to_string_.insert(make_pair(xatom, name)).first->second;
}

}  // namespace window_manager
