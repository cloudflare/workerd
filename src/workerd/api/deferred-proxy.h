// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async.h>

namespace workerd::api {

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

  using InnerCoroutineAdapter =
      typename kj::_::stdcoro::coroutine_traits<kj::Promise<T>, Args...>::promise_type;

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
    // This allows us to write `KJ_CO_MAGIC` within a DeferredProxyPromise<T> coroutine to fulfill
    // the coroutine's outer promise with a DeferredProxy<T>.
    // This could alternatively be an await_transform() with a magic parameter type.
    if (beginDeferredProxying.fulfiller->isWaiting()) {
      beginDeferredProxying.fulfiller->fulfill();
    }

    return {};
  }

  template <CoroutineYieldValue<InnerCoroutineAdapter> U>
  auto yield_value(U&& value) {
    // Forward all other `co_yield`s to the inner coroutine, if it has a `yield_value()`
    // implementation -- it might implement some magic, too.
    return inner.yield_value(kj::fwd<U>(value));
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

}  // namespace workerd::api
