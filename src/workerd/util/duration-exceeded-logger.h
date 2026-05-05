// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/debug.h>
#include <kj/string.h>
#include <kj/time.h>

namespace workerd::util {

// This is a utility class for instantiating a timer, which will log if it's destructed after a
// specified time. This works by relying on RAII (Scope-Bound Resource Management) to check how
// much time has elapsed when the object is destructed. This ensures that the time is checked when
// the timer object goes out of scope, thereby timing everything after the initialization of the
// timer until the end of the scope where it was instantiated.
//
// For the simple case (no extra parameters), construct directly:
//
//   DurationExceededLogger logger(clock, 5 * kj::SECONDS, "operation slow");
//
// To include lazily-evaluated extra parameters (avoiding heap allocation when the duration
// threshold is not exceeded), use the DURATION_EXCEEDED_LOG macro instead:
//
//   DURATION_EXCEEDED_LOG(logger, clock, 5 * kj::SECONDS, "operation slow", requestId, size);
//
// The extra parameters are formatted in the same style as KJ_LOG (name = value), and are only
// stringified if the duration threshold is actually exceeded.

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

// Template variant of DurationExceededLogger that supports lazily-evaluated extra parameters.
// Do not use this class directly; use the DURATION_EXCEEDED_LOG macro which creates the
// appropriate lambda and template instantiation.
template <typename ExtraArgsFunc>
class DurationExceededLoggerWithExtras {
 public:
  DurationExceededLoggerWithExtras(const kj::MonotonicClock& clock,
      kj::Duration warningDuration,
      kj::StringPtr logMessage,
      ExtraArgsFunc& extraArgsFunc)
      : warningDuration(warningDuration),
        logMessage(logMessage),
        start(clock.now()),
        clock(clock),
        extraArgsFunc(extraArgsFunc) {}

  KJ_DISALLOW_COPY_AND_MOVE(DurationExceededLoggerWithExtras);

  ~DurationExceededLoggerWithExtras() noexcept(false) {
    kj::Duration actualDuration = clock.now() - start;
    if (actualDuration >= warningDuration) {
      auto extra = extraArgsFunc();
      if (extra.size() > 0) {
        KJ_LOG(WARNING, kj::str("NOSENTRY ", logMessage, "; ", extra), warningDuration,
            actualDuration);
      } else {
        KJ_LOG(WARNING, kj::str("NOSENTRY ", logMessage), warningDuration, actualDuration);
      }
    }
  }

 private:
  kj::Duration warningDuration;
  kj::StringPtr logMessage;
  kj::TimePoint start;
  const kj::MonotonicClock& clock;
  ExtraArgsFunc& extraArgsFunc;
};

// Macro for creating a DurationExceededLogger with lazily-evaluated extra parameters.
// Extra parameters are only stringified if the duration threshold is actually exceeded,
// avoiding unnecessary heap allocations in the common case where the operation completes quickly.
//
// Parameters are formatted in the same style as KJ_LOG: name = value; name2 = value2
//
// Example:
//   DURATION_EXCEEDED_LOG(logger, clock, 5 * kj::SECONDS, "operation slow", requestId, size);
//
// If triggered, logs something like:
//   NOSENTRY operation slow; requestId = abc; size = 4096; warningDuration = 5s; actualDuration = 12s
//
// The macro also works without extra parameters, behaving identically to DurationExceededLogger:
//   DURATION_EXCEEDED_LOG(logger, clock, 5 * kj::SECONDS, "operation slow");
#if KJ_MSVC_TRADITIONAL_CPP
#define DURATION_EXCEEDED_LOG(name, clock, warningDuration, logMessage, ...)                       \
  auto KJ_UNIQUE_NAME(_delExtraArgsFunc) = [&]() -> kj::String {                                   \
    return ::kj::_::Debug::makeDescription("" #__VA_ARGS__, __VA_ARGS__);                          \
  };                                                                                               \
  ::workerd::util::DurationExceededLoggerWithExtras<decltype(KJ_UNIQUE_NAME(_delExtraArgsFunc))>   \
  name(clock, warningDuration, logMessage, KJ_UNIQUE_NAME(_delExtraArgsFunc))
#else
#define DURATION_EXCEEDED_LOG(name, clock, warningDuration, logMessage, ...)                       \
  auto KJ_UNIQUE_NAME(_delExtraArgsFunc) = [&]() -> kj::String {                                   \
    return ::kj::_::Debug::makeDescription(#__VA_ARGS__, ##__VA_ARGS__);                           \
  };                                                                                               \
  ::workerd::util::DurationExceededLoggerWithExtras<decltype(KJ_UNIQUE_NAME(_delExtraArgsFunc))>   \
  name(clock, warningDuration, logMessage, KJ_UNIQUE_NAME(_delExtraArgsFunc))
#endif

}  // namespace workerd::util
