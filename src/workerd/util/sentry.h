// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Some utilities related to logging, particularly with respect to how Cloudflare's edge runtime's
// Sentry integration will end up treating the logs.

#include <kj/common.h>
#include <kj/string.h>
#include <kj/exception.h>
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

}  // namespace workerd
