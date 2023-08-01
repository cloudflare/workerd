// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async-io.h>
#include <kj/compat/url.h>
#include <kj/string.h>
#include <workerd/jsg/jsg.h>
#include <v8.h>

namespace workerd::api {

jsg::ByteString toLower(kj::StringPtr str);
// Convert `str` to lower-case (e.g. to canonicalize a header name).

// =======================================================================================

#if _MSC_VER
#define strcasecmp _stricmp
#endif

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

kj::Maybe<kj::String> readContentTypeParameter(kj::StringPtr contentType,
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

inline DeferredProxy<void> newNoopDeferredProxy() {
  return DeferredProxy<void> { kj::READY_NOW };
}

template <typename T>
inline DeferredProxy<T> newNoopDeferredProxy(T&& value) {
  return DeferredProxy<T> { kj::mv(value) };
}

template <typename T>
inline kj::Promise<DeferredProxy<T>> addNoopDeferredProxy(kj::Promise<T> promise) {
  // Helper method to use when you need to return `Promise<DeferredProxy<T>>` but no part of the
  // operation you are returning is eligible to be deferred past the IoContext lifetime.
  co_return newNoopDeferredProxy(co_await promise);
}
inline kj::Promise<DeferredProxy<void>> addNoopDeferredProxy(kj::Promise<void> promise) {
  co_await promise;
  co_return newNoopDeferredProxy();
}

// ---------------------------------------------------------
// Deferred proxy coroutine integration

class BeginDeferredProxyingConstant final {};
constexpr BeginDeferredProxyingConstant BEGIN_DEFERRED_PROXYING {};
// A magic constant which a DeferredProxyPromise<T> coroutine can `co_yield` to indicate that the
// deferred proxying phase of its operation has begun.

template <typename T>
class DeferredProxyPromise: public kj::Promise<DeferredProxy<T>> {
  // A "strong typedef" for a kj::Promise<DeferredProxy<T>>. DeferredProxyPromise<T> is intended to
  // be used as the return type for coroutines, in which case the coroutine implementation gains the
  // following features:
  //
  // - `co_yield BEGIN_DEFERRED_PROXYING` fulfills the outer kj::Promise<DeferredProxy<T>>. The
  //   resulting DeferredProxy<T> object contains a `proxyTask` Promise which owns the coroutine.
  //
  // - `co_return` implicitly fulfills the outer Promise for the DeferredProxy<T> (if it has not
  //   already been fulfilled by the magic `co_yield` described above), then fulfills the inner
  //   `proxyTask`.
  //
  // - Unhandled exceptions reject the outer kj::Promise<DeferredProxy<T>> (if it has not already
  //   been fulfilled by the magic `co_yield` described above), then reject the inner `proxyTask`.

public:
  DeferredProxyPromise(kj::Promise<DeferredProxy<T>> promise)
      : kj::Promise<DeferredProxy<T>>(kj::mv(promise)) {}
  // Allow conversion from a regular Promise. This allows our `promise_type::get_return_object()`
  // implementation to be implemented as a regular Promise-returning coroutine.

  class Coroutine;
  using promise_type = Coroutine;
  // The coroutine adapter class, required for the compiler to know how to create coroutines
  // returning DeferredProxyPromise<T>. Since the whole point of DeferredProxyPromise<T> is to serve
  // as a coroutine return type, there's not really any point hiding promise_type inside of a
  // coroutine_traits specialization, like kj::Promise<T> does.
};

template <typename T>
class DeferredProxyPromise<T>::Coroutine:
    public kj::_::CoroutineMixin<DeferredProxyPromise<T>::Coroutine, T> {
  // The coroutine adapter type for DeferredProxyPromise<T>. Most of the work is forwarded to the
  // regular kj::Promise<T> coroutine adapter.

public:
  using Handle = kj::_::stdcoro::coroutine_handle<Coroutine>;

  Coroutine(kj::SourceLocation location = {}): inner(Handle::from_promise(*this), location) {}

  kj::Promise<DeferredProxy<T>> get_return_object() {
    // We need to return a RAII object which will destroy this (as in, `this`) coroutine adapter.
    // The logic which calls `coroutine_handle<>::destroy()` is tucked away in our inner coroutine
    // adapter, however, leading to the weird situation where the `inner.get_return_object()`
    // Promise owns `this`. Thus, we cannot store the inner promise in our own coroutine adapter
    // class, because that would cause a reference cycle. Fortunately, we can implement our own
    // `get_return_object()` as a regular Promise-returning coroutine and keep the inner Promise in
    // our coroutine frame, giving the caller transitive ownership of the DeferredProxyPromise<T>
    // coroutine by way of the kj::Promise<DeferredProxy<T>> coroutine. Later on, when the outer
    // Promise is fulfilled, the caller will gain direct ownership of the DeferredProxyPromise<T>
    // coroutine via the `proxyTask` promise.
    auto proxyTask = inner.get_return_object();
    co_await beginDeferredProxying.promise;
    co_return DeferredProxy<T> { kj::mv(proxyTask) };
  }

  auto initial_suspend() { return inner.initial_suspend(); }
  auto final_suspend() noexcept { return inner.final_suspend(); }
  // Just trivially forward these.

  void unhandled_exception() {
    // If the outer promise hasn't yet been fulfilled, it needs to be rejected now.
    if (beginDeferredProxying.fulfiller->isWaiting()) {
      beginDeferredProxying.fulfiller->reject(kj::getCaughtExceptionAsKj());
    }

    inner.unhandled_exception();
  }

  kj::_::stdcoro::suspend_never yield_value(decltype(BEGIN_DEFERRED_PROXYING)) {
    // This allows us to write `co_yield` within a DeferredProxyPromise<T> coroutine to fulfill
    // the coroutine's outer promise with a DeferredProxy<T>.
    // This could alternatively be an await_transform() with a magic parameter type.
    if (beginDeferredProxying.fulfiller->isWaiting()) {
      beginDeferredProxying.fulfiller->fulfill();
    }

    return {};
  }

  void fulfill(kj::_::FixVoid<T>&& value) {
    // Required by CoroutineMixin implementation to implement `co_return`.

    // Fulfill the outer promise if it hasn't already been fulfilled.
    if (beginDeferredProxying.fulfiller->isWaiting()) {
      beginDeferredProxying.fulfiller->fulfill();
    }

    inner.fulfill(kj::mv(value));
  }

  template <typename U>
  auto await_transform(U&& awaitable) {
    // Trivially forward everything, so we can await anything a kj::Promise<T> can.
    return inner.await_transform(kj::fwd<U>(awaitable));
  }

  operator kj::_::CoroutineBase&() { return inner; }
  // Required by Awaiter<T>::await_suspend() to support awaiting Promises.

private:
  typename kj::_::stdcoro::coroutine_traits<kj::Promise<T>>::promise_type inner;
  // We defer the majority of the implementation to the regular kj::Promise<T> coroutine adapter.

  kj::PromiseFulfillerPair<void> beginDeferredProxying = kj::newPromiseAndFulfiller<void>();
  // Our `get_return_object()` function returns a kj::Promise<DeferredProxy<T>>, waits on this
  // `beginDeferredProxying.promise`, then fulfills its Promise with the result of
  // `inner.get_return_object()`.
};

// =======================================================================================

kj::Maybe<jsg::V8Ref<v8::Object>> cloneRequestCf(
    jsg::Lock& js, kj::Maybe<jsg::V8Ref<v8::Object>> maybeCf);

void maybeWarnIfNotText(kj::StringPtr str);

}  // namespace workerd::api
