// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/string.h>

namespace workerd::jsg {

#define JSG_EXCEPTION(jsErrorType) JSG_ERROR_ ## jsErrorType
#define JSG_DOM_EXCEPTION(name) "jsg.DOMException(" name ")"
#define JSG_INTERNAL_DOM_EXCEPTION(name) "jsg-internal.DOMException(" name ")"

#define JSG_ERROR_DOMOperationError JSG_DOM_EXCEPTION("OperationError")
#define JSG_ERROR_DOMDataError JSG_DOM_EXCEPTION("DataError")
#define JSG_ERROR_DOMDataCloneError JSG_DOM_EXCEPTION("DataCloneError")
#define JSG_ERROR_DOMInvalidAccessError JSG_DOM_EXCEPTION("InvalidAccessError")
#define JSG_ERROR_DOMInvalidStateError JSG_DOM_EXCEPTION("InvalidStateError")
#define JSG_ERROR_DOMInvalidCharacterError JSG_DOM_EXCEPTION("InvalidCharacterError")
#define JSG_ERROR_DOMNotSupportedError JSG_DOM_EXCEPTION("NotSupportedError")
#define JSG_ERROR_DOMSyntaxError JSG_DOM_EXCEPTION("SyntaxError")
#define JSG_ERROR_DOMTimeoutError JSG_DOM_EXCEPTION("TimeoutError")
#define JSG_ERROR_DOMTypeMismatchError JSG_DOM_EXCEPTION("TypeMismatchError")
#define JSG_ERROR_DOMQuotaExceededError JSG_DOM_EXCEPTION("QuotaExceededError")
#define JSG_ERROR_DOMAbortError JSG_DOM_EXCEPTION("AbortError")
#define JSG_ERROR_DOMSyntaxError JSG_DOM_EXCEPTION("SyntaxError")

#define JSG_ERROR_TypeError "jsg.TypeError"
#define JSG_ERROR_Error "jsg.Error"
#define JSG_ERROR_RangeError "jsg.RangeError"

#define JSG_ERROR_InternalDOMOperationError JSG_INTERNAL_DOM_EXCEPTION("OperationError")

#define JSG_KJ_EXCEPTION(type, jsErrorType, ...)                                        \
  kj::Exception(kj::Exception::Type::type, __FILE__, __LINE__,                           \
                kj::str(JSG_EXCEPTION(jsErrorType) ": ", __VA_ARGS__))

#define JSG_ASSERT(cond, jsErrorType, ...)                                              \
  KJ_ASSERT(cond, kj::str(JSG_EXCEPTION(jsErrorType) ": ", ##__VA_ARGS__))

#define JSG_REQUIRE(cond, jsErrorType, ...)                                             \
  KJ_REQUIRE(cond, kj::str(JSG_EXCEPTION(jsErrorType) ": ", ##__VA_ARGS__))
// Unlike KJ_REQUIRE, JSG_REQUIRE passes all message arguments through kj::str which makes it
// "prettier". This does have some implications like if there's only string literal arguments then
// there's an unnecessary heap copy. More importantly none of the expressions you pass in end up in
// the resultant string AND you are responsible for formatting the resultant string. For example,
// KJ_REQUIRE(false, "some message", x) formats it like "some message; x = 5". The "equivalent" via
// this macro would be JSG_REQUIRE(false, "some message ", x); which would yield a string like
// "some message 5" (or JSG_REQUIRE(false, "some message; x = ", x) if you wanted identical output,
// but then why not use KJ_REQUIRE).

#define JSG_REQUIRE_NONNULL(value, jsErrorType, ...)                                    \
  KJ_REQUIRE_NONNULL(value, kj::str(JSG_EXCEPTION(jsErrorType) ": ", ##__VA_ARGS__))
// JSG_REQUIRE + KJ_REQUIRE_NONNULL.

#define JSG_FAIL_REQUIRE(jsErrorType, ...)                                              \
  KJ_FAIL_REQUIRE(kj::str(JSG_EXCEPTION(jsErrorType) ": ", ##__VA_ARGS__))
// JSG_REQUIRE + KJ_FAIL_REQUIRE

#define JSG_WARN_ONCE(msg, ...) \
    static bool logOnce KJ_UNUSED = ([&] { \
      KJ_LOG(WARNING, msg, ##__VA_ARGS__); \
      return true; \
    })() \

// Conditionally log a warning, at most once. Useful for determining if code changes would break
// any existing scripts.
#define JSG_WARN_ONCE_IF(cond, msg, ...) \
  if (cond) { \
    JSG_WARN_ONCE(msg, ##__VA_ARGS__); \
  }

// These are passthrough functions to KJ. We expect the error string to be
// surfaced to the application.

#define _JSG_INTERNAL_REQUIRE(cond, jsErrorType, ...)                                             \
  do {                                                                                             \
    try {                                                                                          \
      KJ_REQUIRE(cond, jsErrorType ": Cloudflare internal error.");                                \
    } catch (const kj::Exception& e) {                                                             \
      KJ_LOG(ERROR, e, ##__VA_ARGS__);                                                             \
      throw e;                                                                                     \
    }                                                                                              \
  } while (0)

#define _JSG_INTERNAL_REQUIRE_NONNULL(value, jsErrorType, ...)                                    \
  ([&]() -> decltype(auto) {                                                                       \
    try {                                                                                          \
      return KJ_REQUIRE_NONNULL(value, jsErrorType ": Cloudflare internal error.");                \
    } catch (const kj::Exception& e) {                                                             \
      KJ_LOG(ERROR, e, ##__VA_ARGS__);                                                             \
      throw e;                                                                                     \
    }                                                                                              \
  }())

#define _JSG_INTERNAL_FAIL_REQUIRE(jsErrorType, ...)                                              \
  do {                                                                                             \
    try {                                                                                          \
      KJ_FAIL_REQUIRE(jsErrorType ": Cloudflare internal error.");                                 \
    } catch (const kj::Exception& e) {                                                             \
      KJ_LOG(ERROR, e, ##__VA_ARGS__);                                                             \
      throw e;                                                                                     \
    }                                                                                              \
  } while (0)

// Given a KJ exception's description, strips any leading "remote exception: " prefixes.
kj::StringPtr stripRemoteExceptionPrefix(kj::StringPtr internalMessage);

// Given a KJ exception's description, returns whether it contains a tunneled exception that could
// be converted back to JavaScript via makeInternalError().
bool isTunneledException(kj::StringPtr internalMessage);

// Given a KJ exception's description, returns whether it contains the magic constant that indicates
// the exception is the script's fault and isn't worth logging.
bool isDoNotLogException(kj::StringPtr internalMessage);

// Log an exception ala LOG_EXCEPTION, but only if it is worth logging and not a tunneled exception.
#define LOG_EXCEPTION_IF_INTERNAL(context, exception) \
  if (!jsg::isTunneledException(exception.getDescription()) && \
      !jsg::isDoNotLogException(exception.getDescription())) { \
    LOG_EXCEPTION(context, exception); \
  }


struct TunneledErrorType {
  // The original error message stripped of prefixes.
  kj::StringPtr message;

  // Was this error prefixed by JSG already?
  bool isJsgError;

  // Is this error internal? If so, the error message should be logged to syslog and hidden from
  // the app.
  bool isInternal;

  // Was the error tunneled from either a worker or an actor?
  bool isFromRemote;

  // Was the error created because a durable object is broken?
  bool isDurableObjectReset;
};

TunneledErrorType tunneledErrorType(kj::StringPtr internalMessage);

// Annotate an internal message with the corresponding brokenness reason.
kj::String annotateBroken(kj::StringPtr internalMessage, kj::StringPtr brokennessReason);

}  // namespace workerd::jsg
