// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/debug.h>
#include <kj/function.h>
#include <kj/string.h>
#include <kj/time.h>

namespace workerd::util {

// This is a utility class for instantiating a timer, which will run a lambda if it's destructed
// after a specified time. This works by relying on RAII (Scope-Bound Resource Management) to check
// how much time has elapsed when the object is destructed. This ensures that the time is checked
// when the timer object goes out of scope thereby timing everything after the initialization of the
// timer, until the end of the scope where it was instantiated.
//
// The labmda that is passed in, and the values it captures needs to be valid for the lifetime of
// the DurationExceededLambda object.
//
// If you just want to emit a log when a certain duration has been exceeded, use the
// DurationExceededLogger instead.
template <typename Func>
class DurationExceededLambda {
 public:
  // The lambda takes a kj::Duration as an argument, which is the actual duration of when this
  // object was destoyed or when end() was called. The lambda should return void.
  DurationExceededLambda(
      const kj::MonotonicClock& clock, kj::Duration thresholdDuration, Func lambda)
      : thresholdDuration(thresholdDuration),
        lambda(lambda),
        start(clock.now()),
        clock(clock) {}

  DurationExceededLambda(DurationExceededLambda&& other)
      : finished(other.finished),
        thresholdDuration(other.thresholdDuration),
        lambda(kj::mv(other.lambda)),
        start(other.start),
        clock(other.clock) {
    other.finished = true;
  }

  ~DurationExceededLambda() noexcept(false) {
    end();
  }

  void end() {
    if (finished) {
      return;
    }
    finished = true;

    kj::Duration actualDuration = clock.now() - start;
    if (actualDuration >= thresholdDuration) {
      lambda(actualDuration);
    }
  }

 private:
  bool finished;
  kj::Duration thresholdDuration;
  Func lambda;
  kj::TimePoint start;
  const kj::MonotonicClock& clock;
};

}  // namespace workerd::util
