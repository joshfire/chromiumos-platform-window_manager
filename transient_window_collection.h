// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_TRANSIENT_WINDOW_COLLECTION_H_
#define WINDOW_MANAGER_TRANSIENT_WINDOW_COLLECTION_H_

#include <map>
#include <tr1/memory>

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "window_manager/geometry.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/x_types.h"

namespace window_manager {

class EventConsumer;
class WindowManager;

// TransientWindowCollection stores information like stacking, position,
// and focus about a set of transient windows belonging to a specific
// "owner" window.
class TransientWindowCollection {
 public:
  // |owner_win| is the window owning the transients in this collection.
  // If |win_to_stack_above| is non-NULL, transients are stacked above it
  // instead of above |owner_win| (this is used for panels, which have titlebar
  // windows that are stacked above their content windows -- we want the
  // transient to be above the titlebar in addition to the content).
  // |event_consumer| is used to register interest in events concerning the
  // windows.
  TransientWindowCollection(Window* owner_win,
                            Window* win_to_stack_above,
                            EventConsumer* event_consumer);
  ~TransientWindowCollection();

  bool is_hidden() const { return is_hidden_; }

  // Do we contain the passed-in window?
  bool ContainsWindow(const Window& win) const;

  // Does one of our transient windows currently have the input focus?
  bool HasFocusedWindow() const;

  // Focus a transient window if possible.  Returns true if successful and
  // false if no window was available to be focused.
  bool TakeFocus(XTime timestamp);

  // Set the window to be focused the next time that TakeFocus() is called.
  // Note that this request may be ignored if a modal transient window
  // already has the focus.
  void SetPreferredWindowToFocus(Window* transient_win);

  // Add a transient window.  This should be called in response to the
  // window being mapped.  The transient will typically be stacked above
  // any other existing transients (unless an existing transient is modal),
  // but if this is the only transient, it will be stacked above the owner
  // if |stack_directly_above_owner| is true and in
  // StackingManager::LAYER_ACTIVE_TRANSIENT_WINDOW otherwise.
  void AddWindow(Window* transient_win,
                 bool stack_directly_above_owner);

  // Remove a transient window.
  // This should be called in response to the window being unmapped.
  void RemoveWindow(Window* transient_win);

  // Update all transient windows' positions and scales based on the owner
  // window's position and scale.
  void ConfigureAllWindowsRelativeToOwner(int anim_ms);

  // Stack all transient windows' composited and client windows in the
  // order dictated by |stacked_transients_|.  If
  // |stack_directly_above_owner| is false, then we stack the transients at
  // StackingManager::LAYER_ACTIVE_TRANSIENT_WINDOW instead of directly
  // above |owner_win_|.
  void ApplyStackingForAllWindows(bool stack_directly_above_owner);

  // Handle a ConfigureRequest event about one of our transient windows.
  void HandleConfigureRequest(Window* transient_win,
                              int req_x, int req_y,
                              int req_width, int req_height);

  // Close all transient windows (which should eventually result in the
  // owner receiving a bunch of UnmapNotify events and calling
  // RemoveWindow() for each transient).
  void CloseAllWindows();

  // Hide all transient windows by moving their client windows offscreen
  // and hiding their composited windows.
  void Hide();

  // Restore previously-hidden transient windows by moving their client
  // windows back onscreen and showing the composited windows.
  void Restore();

  // Ensure that client windows remain offscreen even if the screen got
  // larger.
  void HandleScreenResize();

 private:
  // Information about a transient window.
  struct TransientWindow {
    explicit TransientWindow(Window* win)
        : win(win),
          x_offset(0),
          y_offset(0),
          centered(false) {
    }
    ~TransientWindow() {
      win = NULL;
    }

    // Save the transient window's current offset from another window
    // (typically its owner).
    void SaveOffsetsRelativeToWindow(Window* base_win) {
      x_offset = win->client_x() - base_win->client_x();
      y_offset = win->client_y() - base_win->client_y();
    }

    // Update offsets so the transient will be centered over the passed-in
    // window.  If |bounding_rect| has a positive width and height, the
    // transient window's position will be constrained within it if possible.
    void UpdateOffsetsToCenterOverWindow(Window* base_win,
                                         const Rect& bounding_rect);

    // The transient window itself.  Not owned by us.
    Window* win;

    // Transient window's position's offset from its owner's origin.
    int x_offset;
    int y_offset;

    // Is the transient window centered over its owner?  We set this when
    // we first center a transient window but remove it if the client
    // ever moves the transient itself.
    bool centered;
  };

  typedef std::map<XWindow, std::tr1::shared_ptr<TransientWindow> >
      TransientWindowMap;

  WindowManager* wm() { return owner_win_->wm(); }

  // Get the TransientWindow struct representing the passed-in window.
  TransientWindow* GetTransientWindow(const Window& win);

  // Update the passed-in transient window's client and composited windows
  // appropriately for the owner window's current configuration.  If the
  // collection is currently hidden, we do not move the client window
  // (since it should remain offscreen).
  void ConfigureTransientWindow(TransientWindow* transient, int anim_ms);

  // Stack a transient window's composited and client windows.  If
  // |other_win| is non-NULL, we stack |transient| above it; otherwise,
  // we stack |transient| at the top of
  // StackingManager::LAYER_ACTIVE_TRANSIENT_WINDOW.
  void ApplyStackingForTransientWindow(
      TransientWindow* transient, Window* other_win);

  // Choose a new transient window to focus.  We choose the topmost modal
  // window if there is one; otherwise we just return the topmost
  // transient, or NULL if there aren't any transients.
  TransientWindow* FindTransientWindowToFocus() const;

  // Move a transient window to the top of the collection's stacking order,
  // if it's not already there.  Updates the transient's position in
  // |stacked_transients_| and also restacks its composited and client
  // windows.
  void RestackTransientWindowOnTop(TransientWindow* transient);

  // Window owning this collection.  Not owned by us.
  Window* owner_win_;

  // Window above which all transients should be stacked.  Typically
  // |owner_win_|, but see the comment in the constructor.
  Window* win_to_stack_above_;

  // Event consumer that we register as being interested in events about
  // our transient windows.  The consumer should pass ConfigureRequest
  // notify events about the windows to us using HandleConfigureRequest().
  EventConsumer* event_consumer_;

  // Transient windows, keyed by XID.
  TransientWindowMap transients_;

  // Transient windows in top-to-bottom stacking order.
  scoped_ptr<Stacker<TransientWindow*> > stacked_transients_;

  // Transient window that should be focused when TakeFocus() is called,
  // or NULL if we should avoid focusing any transients (indicating that
  // the owner should be focused instead).
  TransientWindow* transient_to_focus_;

  // Was Hide() called?
  bool is_hidden_;

  DISALLOW_COPY_AND_ASSIGN(TransientWindowCollection);
};

}  // end namespace window_manager

#endif  // WINDOW_MANAGER_TRANSIENT_WINDOW_COLLECTION_H_
