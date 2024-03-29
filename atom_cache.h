// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_ATOM_CACHE_H_
#define WINDOW_MANAGER_ATOM_CACHE_H_

#include <string>

#include "base/basictypes.h"
#include "base/hash_tables.h"
#include "window_manager/x11/x_types.h"

namespace window_manager {

class XConnection;  // from x_connection.h

// Atom names with "_" prefixes (if any) stripped.
//
// When adding a new value, also insert a mapping to its actual name in
// kAtomInfos in atom_cache.cc.
enum Atom {
  ATOM_ATOM = 0,
  ATOM_CARDINAL,
  ATOM_CHROME_FREEZE_UPDATES,
  ATOM_CHROME_GET_SERVER_TIME,
  ATOM_CHROME_LOGGED_IN,
  ATOM_CHROME_STATE,
  ATOM_CHROME_STATE_COLLAPSED_PANEL,
  ATOM_CHROME_VIDEO_TIME,
  ATOM_CHROME_WINDOW_TYPE,
  ATOM_CHROME_WM_MESSAGE,
  ATOM_MANAGER,
  ATOM_NET_ACTIVE_WINDOW,
  ATOM_NET_CLIENT_LIST,
  ATOM_NET_CLIENT_LIST_STACKING,
  ATOM_NET_CURRENT_DESKTOP,
  ATOM_NET_DESKTOP_GEOMETRY,
  ATOM_NET_DESKTOP_VIEWPORT,
  ATOM_NET_NUMBER_OF_DESKTOPS,
  ATOM_NET_SUPPORTED,
  ATOM_NET_SUPPORTING_WM_CHECK,
  ATOM_NET_WM_CM_S0,
  ATOM_NET_WM_MOVERESIZE,
  ATOM_NET_WM_NAME,
  ATOM_NET_WM_PID,
  ATOM_NET_WM_PING,
  ATOM_NET_WM_STATE,
  ATOM_NET_WM_STATE_FULLSCREEN,
  ATOM_NET_WM_STATE_MAXIMIZED_HORZ,
  ATOM_NET_WM_STATE_MAXIMIZED_VERT,
  ATOM_NET_WM_STATE_MODAL,
  ATOM_NET_WM_SYNC_REQUEST,
  ATOM_NET_WM_SYNC_REQUEST_COUNTER,
  ATOM_NET_WM_USER_TIME,
  ATOM_NET_WM_WINDOW_OPACITY,
  ATOM_NET_WM_WINDOW_TYPE,
  ATOM_NET_WM_WINDOW_TYPE_COMBO,
  ATOM_NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
  ATOM_NET_WM_WINDOW_TYPE_MENU,
  ATOM_NET_WM_WINDOW_TYPE_POPUP_MENU,
  ATOM_NET_WM_WINDOW_TYPE_TOOLTIP,
  ATOM_NET_WORKAREA,
  ATOM_PRIMARY,
  ATOM_WM_CLIENT_MACHINE,
  ATOM_WM_DELETE_WINDOW,
  ATOM_WM_HINTS,
  ATOM_WM_NAME,
  ATOM_WM_NORMAL_HINTS,
  ATOM_WM_PROTOCOLS,
  ATOM_WM_S0,
  ATOM_WM_STATE,
  ATOM_WM_TAKE_FOCUS,
  ATOM_WM_TRANSIENT_FOR,
  kNumAtoms,
};

// A simple class for looking up X atoms.  Using XInternAtom() to find the
// X atom for a given string requires a round trip to the X server; we
// avoid that by keeping a static map here.  To add some compile-time
// safety against typos in atom strings, values from the above Atom enum
// (rather than strings) are used to look up the X server's IDs for atoms.
// All atoms are fetched from the server just once, in the constructor.
class AtomCache {
 public:
  explicit AtomCache(XConnection* xconn);

  // Get the X server's ID for a value in our Atom enum.
  XAtom GetXAtom(Atom atom) const;

  // Debugging method to get the string value of an atom ID returned from
  // the X server.  Looks up the atom using XGetAtomName() if it's not
  // already present in the cache.  Only pass atoms that were received from
  // the X server (empty strings will be returned for invalid atoms).
  const std::string& GetName(XAtom xatom);

 private:
  XConnection* xconn_;  // not owned

  // Maps from our Atom enum to the X server's atom IDs and from the
  // server's IDs to atoms' string names.  These maps aren't necessarily in
  // sync; |atom_to_xatom_| is constant after the constructor finishes but
  // GetName() caches additional string mappings in |xatom_to_string_|.
  base::hash_map<int, XAtom> atom_to_xatom_;
  base::hash_map<XAtom, std::string> xatom_to_string_;

  DISALLOW_COPY_AND_ASSIGN(AtomCache);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_ATOM_CACHE_H_
