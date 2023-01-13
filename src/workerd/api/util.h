// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async-io.h>
#include <kj/compat/url.h>
#include <kj/encoding.h>
#include <workerd/jsg/jsg.h>
#include <v8.h>
#include <queue>

namespace workerd::api {

jsg::ByteString toLower(kj::StringPtr str);
// Convert `str` to lower-case (e.g. to canonicalize a header name).

// =======================================================================================

struct CiLess {
  // Case-insensitive comparator for use with std::set/map.

  bool operator()(kj::StringPtr lhs, kj::StringPtr rhs) const {
    return strcasecmp(lhs.begin(), rhs.begin()) < 0;
  }
};

kj::String toLower(kj::String&& str);
// Mutate `str` with all alphabetic ASCII characters lowercased. Returns `str`.
kj::String toUpper(kj::String&& str);
// Mutate `str` with all alphabetic ASCII characters uppercased. Returns `str`.

inline bool isHexDigit(uint32_t c) {
  // Check if `c` is the ASCII code of a hexadecimal digit.
  return ('0' <= c && c <= '9') ||
         ('a' <= c && c <= 'f') ||
         ('A' <= c && c <= 'F');
}

void parseQueryString(kj::Vector<kj::Url::QueryParam>& query, kj::ArrayPtr<const char> rawText,
                      bool skipLeadingQuestionMark = false);
// Parse `rawText` as application/x-www-form-urlencoded name/value pairs and store in `query`. If
// `skipLeadingQuestionMark` is true, any initial '?' will be ignored. Otherwise, it will be
// interpreted as part of the first URL-encoded field.
//
// TODO(cleanup): Would be really nice to move this to kj-url.

kj::Maybe<kj::ArrayPtr<const char>> readContentTypeParameter(kj::StringPtr contentType,
                                                             kj::StringPtr param);
// Given the value of a Content-Type header, returns the value of a single expected parameter.
// For example:
//
//   readContentTypeParameter("application/x-www-form-urlencoded; charset=\"foobar\"", "charset")
//
// would return "foobar" (without the quotes).
//
// Assumptions:
//   - `contentType` has a semi-colon followed by OWS before the parameters.
//   - If the wanted parameter uses quoted-string values, the correct
//     value may not be returned.
//
// TODO(cleanup): Replace this function with a full kj::MimeType parser.

// =======================================================================================

struct ErrorTranslation {
  kj::StringPtr kjDescription;
  // A snippet of a KJ API exception description to be searched for.

  kj::StringPtr jsDescription;
  // A cleaned up exception description suitable for exposing to JavaScript. There is no need to
  // prefix it with jsg.TypeError.
};

kj::Maybe<kj::Exception> translateKjException(
    const kj::Exception& exception,
    std::initializer_list<ErrorTranslation> translations);
// HACK: In some cases, KJ APIs throw exceptions with essential details that we want to expose to
// the user, but also sensitive details or poor formatting which we'd prefer not to expose to the
// user. While crude, we can string match to provide cleaned up exception messages. This O(n)
// function helps you do that.

// =======================================================================================

kj::Own<kj::AsyncInputStream> newTeeErrorAdapter(kj::Own<kj::AsyncInputStream> inner);
// Wrap the given stream in an adapter which translates kj::newTee()-specific exceptions into
// JS-visible exceptions.

kj::String redactUrl(kj::StringPtr url);
// Redacts potential secret keys from a given URL using a couple heuristics:
//   - Any run of hex characters of 32 or more digits, ignoring potential "+-_" separators
//   - Any run of base64 characters of 21 or more digits, including at least
//     two each of digits, capital letters, and lowercase letters.
// Such ids are replaced with the text "REDACTED".

// =======================================================================================

double dateNow();
// Returns exactly what Date.now() would return.

// =======================================================================================

template <typename T>
struct DeferredProxy {
  // Some API methods return Promise<DeferredProxy<T>> when the task can be separated into two
  // parts: some work that must be done with the IoContext still live, and some part that
  // can occur after the IoContext completes, but which should still be performed before
  // the overall task is "done".
  //
  // In particular, when an HTTP event ends up proxying the response body stream (or WebSocket
  // stream) directly to/from origin, then that streaming can take place without pinning the
  // isolate in memory, and without holding the IoContext open. So,
  // `ServiceWorkerGlobalScope::request()` returns `Promise<DeferredProxy<void>>`. The outer
  // Promise waits for the JavaScript work to be done, and the inner DeferredProxy<void> represents
  // the proxying step.
  //
  // Note that if you're performing a task that resolves to DeferredProxy but JavaScript is
  // actually waiting for the result of the task, then it's your responsibility to call
  // IoContext::current().registerPendingEvent() and attach it to `proxyTask`, otherwise
  // the request might be canceled as the proxy task won't be recognized as something that the
  // request is waiting on.
  //
  // TODO(cleanup): Now that we have jsg::Promise, it might make sense for deferred proxying to
  //    be represented as `jsg::Promise<api::DeferredProxy<T>>`, since the outer promise is
  //    intended to represent activity that happens in JavaScript while the inner one represents
  //    pure I/O. This will require some refactoring, though.

  kj::Promise<T> proxyTask;
};

template <typename T>
inline kj::Promise<DeferredProxy<T>> addNoopDeferredProxy(kj::Promise<T> promise) {
  // Helper method to use when you need to return `Promise<DeferredProxy<T>>` but no part of the
  // operation you are returning is eligible to be deferred past the IoContext lifetime.

  return promise.then([](T&& value) { return DeferredProxy<T> { kj::mv(value) }; });
}
inline kj::Promise<DeferredProxy<void>> addNoopDeferredProxy(kj::Promise<void> promise) {
  return promise.then([]() { return DeferredProxy<void> { kj::READY_NOW }; });
}

}  // namespace workerd::api
