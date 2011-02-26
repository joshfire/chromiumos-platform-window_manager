// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor/animation.h"

#include <cmath>

#include "base/logging.h"

using base::TimeDelta;
using base::TimeTicks;
using std::vector;

namespace window_manager {

Animation::Animation(float start_value, const TimeTicks& start_time)
    : start_keyframe_(start_value, start_time),
      end_keyframe_(start_value, start_time) {
}

Animation::~Animation() {
}

bool Animation::IsDone(const TimeTicks& now) const {
  return (now >= end_keyframe_.timestamp);
}

float Animation::GetValue(const TimeTicks& now) const {
  if (now <= start_keyframe_.timestamp)
    return start_keyframe_.value;
  if (now >= end_keyframe_.timestamp)
    return end_keyframe_.value;

  const int num_keyframes_in_vector =
      (keyframes_.get() ? static_cast<int>(keyframes_->size()) : 0);

  const Keyframe* prev_keyframe = &start_keyframe_;
  const Keyframe* next_keyframe =
      (num_keyframes_in_vector ? &((*keyframes_)[0]) : &end_keyframe_);
  int next_keyframe_index = 1;

  // Walk through the keyframes in-order until we find one that starts later
  // than |now|.  Our current position falls between that keyframe
  // (|next_keyframe|) and the one before it (|prev_keyframe|).
  while (now > next_keyframe->timestamp) {
    prev_keyframe = next_keyframe;
    next_keyframe_index++;
    next_keyframe =
        (next_keyframe_index <= num_keyframes_in_vector) ?
        &((*keyframes_)[next_keyframe_index - 1]) :
        &end_keyframe_;
    DCHECK_NE(prev_keyframe, next_keyframe);
  }

  TimeDelta time_between_keyframes =
      next_keyframe->timestamp - prev_keyframe->timestamp;
  TimeDelta time_since_prev_keyframe = now - prev_keyframe->timestamp;

  float ease_factor = M_PI / time_between_keyframes.InMilliseconds();
  float fraction =
      (1.0f - cosf(ease_factor * time_since_prev_keyframe.InMilliseconds())) /
      2.0f;
  return prev_keyframe->value +
      fraction * (next_keyframe->value - prev_keyframe->value);
}

float Animation::GetEndValue() const {
  return end_keyframe_.value;
}

void Animation::AppendKeyframe(float value,
                               const TimeDelta& delay_from_last_keyframe) {
  DCHECK_GT(delay_from_last_keyframe.InMilliseconds(), 0LL);

  // |start_keyframe_| and |end_keyframe_| initially both contain the starting
  // keyframe.  If a keyframe has already been added, then we need to insert a
  // new one and copy the previous ending keyframe to it.
  if (end_keyframe_.timestamp > start_keyframe_.timestamp) {
    if (!keyframes_.get())
      keyframes_.reset(new vector<Keyframe>);
    keyframes_->push_back(end_keyframe_);
  }

  // End at the new value.
  end_keyframe_.value = value;
  end_keyframe_.timestamp += delay_from_last_keyframe;
}

AnimationPair::AnimationPair(Animation* first_animation,
                             Animation* second_animation)
    : first_animation_(first_animation),
      second_animation_(second_animation) {
}

AnimationPair::~AnimationPair() {
}

void AnimationPair::AppendKeyframe(float first_value,
                                   float second_value,
                                   const TimeDelta& delay_from_last_keyframe) {
  first_animation_->AppendKeyframe(first_value, delay_from_last_keyframe);
  second_animation_->AppendKeyframe(second_value, delay_from_last_keyframe);
}

}  // namespace window_manager
