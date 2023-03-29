// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Some utilities related to logging, particularly with respect to how Cloudflare's edge runtime's
// Sentry integration will end up treating the logs.

#include <kj/common.h>
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

inline kj::StringPtr maybeOmitColoFromSentry(uint32_t coloId) {
  // Avoid logging about problems talking to DOG. It's not great to hard-code this, but it'll help
  // avoid sentry spam and is only used in deciding whether to log to sentry, not to change behavior
  // at all.
  const uint32_t dogColoId = 131;
  return coloId == dogColoId ? "NOSENTRY"_kj : ""_kj;
}

#define LOG_NOSENTRY(severity, message, ...) \
  KJ_LOG(severity,  "NOSENTRY " message, __VA_ARGS__);

#define LOG_IF_INTERESTING(exception, severity, ...) \
  if (!::workerd::isInterestingException(exception)) { \
    LOG_NOSENTRY(severity, __VA_ARGS__); \
  } else { \
    KJ_LOG(severity,  __VA_ARGS__); \
  }

// Log this to Sentry once ever per process. Typically will be better to use LOG_ERROR_PERIODICALLY.
#define LOG_ERROR_ONCE(msg, ...)                                               \
  do {                                                                         \
    static bool logOnce KJ_UNUSED = [&]() {                                    \
      KJ_LOG(ERROR, msg, ##__VA_ARGS__);                                       \
      return true;                                                             \
    }();                                                                       \
  } while (0)

#define LOG_ERROR_ONCE_IF(cond, msg, ...)                                      \
  do {                                                                         \
    if (cond) {                                                                \
      LOG_ERROR_ONCE(msg, ##__VA_ARGS__);                                      \
    }                                                                          \
  } while (0)

// Slightly more expensive than LOG_ERROR_ONCE. Avoid putting into a hot path (e.g. within a loop)
// where an overhead of ~hundreds of nanoseconds per evaluation to retrieve the current time would
// be prohibitive.
#define LOG_ERROR_PERIODICALLY(msg, ...)                                       \
  do {                                                                         \
    static kj::TimePoint KJ_UNIQUE_NAME(lastLogged) =                          \
        kj::origin<kj::TimePoint>();                                           \
    const auto now = kj::systemCoarseMonotonicClock().now();                   \
    const auto elapsed = now - KJ_UNIQUE_NAME(lastLogged);                     \
    if (KJ_UNLIKELY(elapsed >= 1 * kj::HOURS)) {                               \
      KJ_UNIQUE_NAME(lastLogged) = now;                                        \
      KJ_LOG(ERROR, msg, ##__VA_ARGS__);                                       \
    }                                                                          \
  } while (0)

#define LOG_ERROR_PERIODICALLY_IF(cond, msg, ...)                              \
  do {                                                                         \
    if (cond) {                                                                \
      LOG_ERROR_PERIODICALLY(msg, ##__VA_ARGS__);                              \
    }                                                                          \
  } while (0)

} // namespace workerd
