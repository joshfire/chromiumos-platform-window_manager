// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_COMPOSITOR_ANIMATION_H_
#define WINDOW_MANAGER_COMPOSITOR_ANIMATION_H_

#include <vector>

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "base/time.h"

namespace window_manager {

// The Animation class takes a sequence of keyframes and computes the
// appropriate value at a given time.  It's used by Compositor to animate
// Actors.
class Animation {
 public:
  // The animation starts at |start_time| with |start_value|.
  Animation(float start_value, const base::TimeTicks& start_time);
  ~Animation();

  // Is the animation active at a particular time?
  bool IsDone(const base::TimeTicks& now) const;

  // Gets the value of the animation at a particular time.
  float GetValue(const base::TimeTicks& now) const;

  // Return the value at the end of the animation.
  float GetEndValue() const;

  // Record a new value for the animation to reach at a specified amount of time
  // from the last frame (or the initial frame if none have been added yet).
  void AppendKeyframe(float value,
                      const base::TimeDelta& delay_from_last_keyframe);

 private:
  struct Keyframe {
    Keyframe(float value, const base::TimeTicks& timestamp)
        : value(value), timestamp(timestamp) {
    }

    // Value when this keyframe is shown.
    float value;

    // Time at which this keyframe is shown.
    base::TimeTicks timestamp;
  };

  // Starting and ending points for the animation.
  // We optimize for the common case where we're just animating between two
  // keyframes.
  Keyframe start_keyframe_, end_keyframe_;

  // Frames falling between the starting and ending points.
  // NULL until we have more keyframes than the starting and ending one.
  scoped_ptr<std::vector<Keyframe> > keyframes_;

  DISALLOW_COPY_AND_ASSIGN(Animation);
};

class AnimationPair {
 public:
  // Takes ownership of the animations.
  AnimationPair(Animation* first_animation,
                Animation* second_animation);
  ~AnimationPair();

  const Animation& first_animation() { return *(first_animation_.get()); }
  const Animation& second_animation() { return *(second_animation_.get()); }

  Animation* release_first_animation() { return first_animation_.release(); }
  Animation* release_second_animation() { return second_animation_.release(); }

  // Add new keyframes to both animations, scheduled to appear at the same time.
  void AppendKeyframe(float first_value,
                      float second_value,
                      const base::TimeDelta& delay_from_last_keyframe);

 private:
  scoped_ptr<Animation> first_animation_;
  scoped_ptr<Animation> second_animation_;

  DISALLOW_COPY_AND_ASSIGN(AnimationPair);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_COMPOSITOR_ANIMATION_H_
