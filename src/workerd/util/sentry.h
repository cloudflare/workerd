// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Some utilities related to logging, particularly with respect to how Cloudflare's edge runtime's
// Sentry integration will end up treating the logs.

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/string.h>
#include <kj/exception.h>
#include <kj/time.h>
#include <stdint.h>

namespace workerd {

// Log out an exception with context but without frills. This macro excludes any variadic arguments
// from the macro so that we do not accidentally make a more granular fingerprint. It also will only
// take a `context` argument that is known at compile time (via constexpr assignment).
#define LOG_EXCEPTION(context, exception) \
  [&](const kj::Exception& e) { \
    constexpr auto sentryErrorContext = context; \
    KJ_LOG(ERROR, e, sentryErrorContext); \
  }(exception)

#define ACTOR_STORAGE_OP_PREFIX "; actorStorageOp = "

inline bool isInterestingException(const kj::Exception& e) {
  return e.getType() != kj::Exception::Type::DISCONNECTED
      && e.getType() != kj::Exception::Type::OVERLOADED;
}

#define LOG_NOSENTRY(severity, ...) \
  KJ_LOG(severity, "NOSENTRY " __VA_ARGS__);

#define LOG_IF_INTERESTING(exception, severity, ...) \
  if (!::workerd::isInterestingException(exception)) { \
    LOG_NOSENTRY(severity, __VA_ARGS__); \
  } else { \
    KJ_LOG(severity, __VA_ARGS__); \
  }

// Log this to Sentry once ever per process. Typically will be better to use LOG_PERIODICALLY.
#define LOG_ONCE(severity, ...)                                                \
  do {                                                                         \
    static bool logOnce KJ_UNUSED = [&]() {                                    \
      KJ_LOG(severity, __VA_ARGS__);                                           \
      return true;                                                             \
    }();                                                                       \
  } while (0)

// Log this to Sentry once ever per process. Typically will be better to use LOG_WARNING_PERIODICALLY.
#define LOG_WARNING_ONCE(...)                                                  \
  LOG_ONCE(WARNING, __VA_ARGS__);

// Log this to Sentry once ever per process. Typically will be better to use LOG_ERROR_PERIODICALLY.
#define LOG_ERROR_ONCE(...)                                                    \
  LOG_ONCE(ERROR, __VA_ARGS__);

// Slightly more expensive than LOG_ONCE. Avoid putting into a hot path (e.g. within a loop)
// where an overhead of ~hundreds of nanoseconds per evaluation to retrieve the current time would
// be prohibitive.
#define LOG_PERIODICALLY(severity, ...)                                        \
  do {                                                                         \
    static kj::TimePoint KJ_UNIQUE_NAME(lastLogged) =                          \
        kj::origin<kj::TimePoint>() - 1 * kj::HOURS;                           \
    const auto now = kj::systemCoarseMonotonicClock().now();                   \
    const auto elapsed = now - KJ_UNIQUE_NAME(lastLogged);                     \
    if (KJ_UNLIKELY(elapsed >= 1 * kj::HOURS)) {                               \
      KJ_UNIQUE_NAME(lastLogged) = now;                                        \
      KJ_LOG(severity, __VA_ARGS__);                                           \
    }                                                                          \
  } while (0)

// Slightly more expensive than LOG_WARNING_ONCE. Avoid putting into a hot path (e.g. within a loop)
// where an overhead of ~hundreds of nanoseconds per evaluation to retrieve the current time would
// be prohibitive.
#define LOG_WARNING_PERIODICALLY(...)                                            \
  LOG_PERIODICALLY(WARNING, __VA_ARGS__);

// Slightly more expensive than LOG_ERROR_ONCE. Avoid putting into a hot path (e.g. within a loop)
// where an overhead of ~hundreds of nanoseconds per evaluation to retrieve the current time would
// be prohibitive.
#define LOG_ERROR_PERIODICALLY(...)                                            \
  LOG_PERIODICALLY(ERROR, __VA_ARGS__);

// The DEBUG_FATAL_RELEASE_LOG macros is for assertions that should definitely break in tests but
// are not worth breaking production over. Instead, it logs the assertion message to sentry so that
// we can notice the event. If your code requires that an assertion is true for safety (e.g.
// checking if a value is not null), this is not the macro for you.
#ifdef KJ_DEBUG
#define DEBUG_FATAL_RELEASE_LOG(severity, ...)                                 \
  ([&]() noexcept {                                                            \
    KJ_FAIL_ASSERT(__VA_ARGS__);                                               \
  })()
#else
#define DEBUG_FATAL_RELEASE_LOG(severity, ...)                                 \
  do {                                                                         \
    static bool logOnce KJ_UNUSED = [&]() {                                    \
      KJ_LOG(severity, __VA_ARGS__);                                           \
      return true;                                                             \
    }();                                                                       \
  } while (0)
#endif

} // namespace workerd
