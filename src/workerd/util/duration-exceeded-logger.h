// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/debug.h>
#include <kj/string.h>
#include <kj/time.h>

namespace workerd::util {

// This is a utility class for instantiating a timer, which will log if it's destructed after a specified time
// This works by relying on RAII (Scope-Bound Resource Management) to check how much time has elapsed
// when the object is destructed. This ensures that the time is checked when the timer object goes out of scope
// thereby timing everything after the initialization of the timer, until the end of the scope where it was
// instantiated.
class DurationExceededLogger {
public:
  DurationExceededLogger(
      const kj::MonotonicClock& clock, kj::Duration warningDuration, kj::StringPtr logMessage)
      : warningDuration(warningDuration),
        logMessage(logMessage),
        start(clock.now()),
        clock(clock) {}

  KJ_DISALLOW_COPY_AND_MOVE(DurationExceededLogger);

  ~DurationExceededLogger() noexcept(false) {
    kj::Duration actualDuration = clock.now() - start;
    if (actualDuration >= warningDuration) {
      KJ_LOG(WARNING, kj::str("NOSENTRY ", logMessage), warningDuration, actualDuration);
    }
  }

private:
  kj::Duration warningDuration;
  kj::StringPtr logMessage;
  kj::TimePoint start;
  const kj::MonotonicClock& clock;
};

}  // namespace workerd::util
