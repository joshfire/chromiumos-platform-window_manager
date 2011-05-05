// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_UTIL_H_
#define WINDOW_MANAGER_UTIL_H_

#include <algorithm>
#include <ctime>
#include <list>
#include <map>
#include <string>

#include "base/basictypes.h"
#include "base/hash_tables.h"
#include "base/logging.h"
#include "base/time.h"
#include "window_manager/geometry.h"

namespace window_manager {

// Stacker maintains an ordering of objects (e.g. windows) in which changes
// can be made in faster-than-linear time.
template<class T>
class Stacker {
 public:
  Stacker() {}

  // Get the (top-to-bottom) ordered list of items.
  const std::list<T>& items() const { return items_; }

  // Has a particular item been registered?
  bool Contains(T item) const {
    return (index_.find(item) != index_.end());
  }

  // Get an item's 0-based position in the stack, or -1 if it isn't
  // present.  Slow but useful for testing.
  int GetIndex(T item) const {
    int i = 0;
    for (typename std::list<T>::const_iterator it = items_.begin();
         it != items_.end(); ++it, ++i) {
      if (*it == item)
        return i;
    }
    return -1;
  }

  // Get the item under |item| on the stack, or NULL if |item| is on the
  // bottom of the stack.
  const T* GetUnder(T item) const {
    typename IteratorMap::const_iterator map_it = index_.find(item);
    if (map_it == index_.end()) {
      LOG(WARNING) << "Got request for item under not-present item " << item;
      return NULL;
    }
    typename std::list<T>::iterator list_it = map_it->second;
    list_it++;
    if (list_it == items_.end()) {
      return NULL;
    }
    return &(*list_it);
  }

  // Add an item on the top of the stack.
  void AddOnTop(T item) {
    if (Contains(item)) {
      LOG(WARNING) << "Ignoring request to add already-present item "
                   << item << " on top";
      return;
    }
    items_.push_front(item);
    index_.insert(make_pair(item, items_.begin()));
  }

  // Add an item on the bottom of the stack.
  void AddOnBottom(T item) {
    if (Contains(item)) {
      LOG(WARNING) << "Ignoring request to add already-present item "
                   << item << " on bottom";
      return;
    }
    items_.push_back(item);
    index_.insert(make_pair(item, --(items_.end())));
  }

  // Add |item| above |other_item|.  |other_item| must already exist on the
  // stack.
  void AddAbove(T item, T other_item) {
    if (Contains(item)) {
      LOG(WARNING) << "Ignoring request to add already-present item "
                   << item << " above item " << other_item;
      return;
    }
    typename IteratorMap::iterator other_it = index_.find(other_item);
    if (other_it == index_.end()) {
      LOG(WARNING) << "Ignoring request to add item " << item
                   << " above not-present item " << other_item;
      return;
    }
    typename std::list<T>::iterator new_it = items_.insert(other_it->second,
                                                           item);
    index_.insert(make_pair(item, new_it));
  }

  // Add |item| below |other_item|.  |other_item| must already exist on the
  // stack.
  void AddBelow(T item, T other_item) {
    if (Contains(item)) {
      LOG(WARNING) << "Ignoring request to add already-present item "
                   << item << " below item " << other_item;
      return;
    }
    typename IteratorMap::iterator other_it = index_.find(other_item);
    if (other_it == index_.end()) {
      LOG(WARNING) << "Ignoring request to add item " << item
                   << " below not-present item " << other_item;
      return;
    }
    // Lists don't support operator+ or operator-, so we need to use ++.
    // Make a copy of the iterator before doing this so that we don't screw
    // up the previous value in the map.
    typename std::list<T>::iterator new_it = other_it->second;
    typename std::list<T>::iterator it = items_.insert(++new_it, item);
    index_.insert(make_pair(item, it));
  }

  // Remove an item from the stack.
  void Remove(T item) {
    typename IteratorMap::iterator it = index_.find(item);
    if (it == index_.end()) {
      LOG(WARNING) << "Ignoring request to remove not-present item " << item;
      return;
    }
    items_.erase(it->second);
    index_.erase(it);
  }

 private:
  // Items stacked from top to bottom.
  std::list<T> items_;

  typedef std::map<T, typename std::list<T>::iterator> IteratorMap;

  // Index into |items_|.
  IteratorMap index_;

  DISALLOW_COPY_AND_ASSIGN(Stacker);
};


// ByteMap unions rectangles into a 2-D array of bytes.  That's it. :-P
class ByteMap {
 public:
  ByteMap(const Size& size);
  ~ByteMap();

  const Size& size() const { return size_; }
  const unsigned char* bytes() const { return bytes_; }

  // Resize this bytemap.  Its contents are cleared to 0.
  void Resize(const Size& new_size);

  // Copy the bytes from |other|, which need not have the same dimensions as
  // this map.
  void Copy(const ByteMap& other);

  // Set every byte to |value|.
  void Clear(unsigned char value);

  // Set the bytes covered by the passed-in rectangle.
  void SetRectangle(const Rect& rect, unsigned char value);

  // Check if the bytes from |other| match the bytes from this.
  bool operator==(const ByteMap& other);

 private:
  Size size_;
  unsigned char* bytes_;

  DISALLOW_COPY_AND_ASSIGN(ByteMap);
};


// Sets a variable to a value within a particular scope and resets it when
// the scope is exited.
// TODO: This is just a templatized version of Chrome's base/auto_reset.h.
// Use that instead when/if it's templatized.
template<class T>
class AutoReset {
 public:
  AutoReset(T* scoped_variable, T new_value)
      : scoped_variable_(scoped_variable),
        original_value_(*scoped_variable) {
    *scoped_variable_ = new_value;
  }
  ~AutoReset() { *scoped_variable_ = original_value_; }

 private:
  T* scoped_variable_;
  T original_value_;

  DISALLOW_COPY_AND_ASSIGN(AutoReset);
};


namespace util {

// Look up a value in a map given the corresponding key, returning a
// default value if the key isn't present.
template<class K, class V>
V FindWithDefault(const std::map<K, V>& the_map, const K& key, const V& def) {
  typename std::map<K, V>::const_iterator it = the_map.find(key);
  if (it == the_map.end()) {
    return def;
  }
  return it->second;
}

template<class K, class V>
V FindWithDefault(const base::hash_map<K, V>& the_map,
                  const K& key,
                  const V& def) {
  typename base::hash_map<K, V>::const_iterator it =
      the_map.find(key);
  if (it == the_map.end()) {
    return def;
  }
  return it->second;
}

// Move an iterator to another position.
template<class ForwardIterator>
void ReorderIterator(ForwardIterator src_it, ForwardIterator dest_it) {
  if (dest_it > src_it)
    std::rotate(src_it, src_it + 1, dest_it + 1);
  else
    std::rotate(dest_it, src_it, src_it + 1);
}


// Helper method to convert an XID into a hex string.
std::string XidStr(unsigned long xid);

// Convert the passed-in time (containing seconds since the epoch) to a
// string of the form "YYYYMMDD-HHMMSS" in the local time zone.
std::string GetTimeAsString(time_t utime);

// Get the number of seconds since the epoch.
// The values returned by successive calls can decrease if the system clock
// is set to an earlier time.
time_t GetCurrentTimeSec();

// Get the number of milliseconds since the epoch.
// The values returned by successive calls can decrease if the system clock
// is set to an earlier time.
int64_t GetCurrentTimeMs();

// Set the time returned by GetCurrentTimeSecs() and GetCurrentTimeMs().
// A negative |sec| value makes us revert to the real time.  Used by tests.
void SetCurrentTimeForTest(time_t sec, int ms);

// Get a monotonically-increasing time.
// The values returned are not affected by changes to the system clock.
base::TimeTicks GetMonotonicTime();

// Set the time to be returned by GetMonototonicTime().  Used by tests.
void SetMonotonicTimeForTest(const base::TimeTicks& now);

// TimeTicks has a protected constructor for passing in your own time, but this
// can be used to get around it.  Used by tests.
base::TimeTicks CreateTimeTicksFromMs(int64_t time_ms);

// Helper function to create a symlink pointing from |symlink_path| (a full
// path) to |log_basename| (the name of a file that should be in the same
// directory as the symlink).  Removes |symlink_path| if it already exists.
// Returns true on success.
bool SetUpLogSymlink(const std::string& symlink_path,
                     const std::string& log_basename);

// Get the machine's hostname, as returned by gethostname().
std::string GetHostname();

// Run a command using system().  '&' is appended.
// Ideally we'd just use LaunchApp() from Chrome's process_util.h instead,
// but that method expects a pre-parsed argv and we're running commands
// specified on the command line (and don't want to do shell-style
// quote-handling ourselves).
//
// The argument is not a reference because the callback code that we're
// using from protobuf has trouble with references.
void RunCommandInBackground(std::string command);

}  // namespace util

}  // namespace window_manager

#endif  // WINDOW_MANAGER_UTIL_H_
