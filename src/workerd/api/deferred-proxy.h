// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async.h>
#include <kj/debug.h>

namespace workerd::api {

// =======================================================================================

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
template <typename T>
struct DeferredProxy {
  // TODO(cleanup): Now that we have jsg::Promise, it might make sense for deferred proxying to
  //    be represented as `jsg::Promise<api::DeferredProxy<T>>`, since the outer promise is
  //    intended to represent activity that happens in JavaScript while the inner one represents
  //    pure I/O. This will require some refactoring, though.

  kj::Promise<T> proxyTask;
};

inline DeferredProxy<void> newNoopDeferredProxy() {
  return DeferredProxy<void>{kj::READY_NOW};
}

template <typename T>
inline DeferredProxy<T> newNoopDeferredProxy(T&& value) {
  return DeferredProxy<T>{kj::mv(value)};
}

// Helper method to use when you need to return `Promise<DeferredProxy<T>>` but no part of the
// operation you are returning is eligible to be deferred past the IoContext lifetime.
template <typename T>
inline kj::Promise<DeferredProxy<T>> addNoopDeferredProxy(kj::Promise<T> promise) {
  co_return newNoopDeferredProxy(co_await promise);
}
inline kj::Promise<DeferredProxy<void>> addNoopDeferredProxy(kj::Promise<void> promise) {
  co_await promise;
  co_return newNoopDeferredProxy();
}

// ---------------------------------------------------------
// Deferred proxy coroutine integration

// If a coroutine returns a kj::Promise<DeferredProxy<T>>, the coroutine implementation gains the
// following features:
//
// - `KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING` fulfills the outer kj::Promise<DeferredProxy<T>>. The
//   resulting DeferredProxy<T> object contains a `proxyTask` Promise which owns the coroutine.
//
// - `co_return` implicitly fulfills the outer Promise for the DeferredProxy<T> (if it has not
//   already been fulfilled by the magic `KJ_CO_MAGIC` described above), then fulfills the inner
//   `proxyTask`.
//
// - Unhandled exceptions reject the outer kj::Promise<DeferredProxy<T>> (if it has not already
//   been fulfilled by the magic `KJ_CO_MAGIC` described above), then reject the inner `proxyTask`.
//
// It is not possible to write a "regular" coroutine which returns kj::Promise<DeferredProxy<T>>;
// that is, `co_return DeferredProxy<T> { ... }` is a compile error. You must initiate deferred
// proxying using `KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING`.

// The coroutine adapter class, required for the compiler to know how to create coroutines
// returning kj::Promise<DeferredProxy<T>>. We declare it here so we can name it in our
// `coroutine_traits` specialization below.
template <typename T, typename... Args>
class DeferredProxyCoroutine;
}  // namespace workerd::api

namespace KJ_COROUTINE_STD_NAMESPACE {
// Enter the `std` or `std::experimental` namespace, depending on whether we're using C++20
// coroutines or the Coroutines TS.

template <class T, class... Args>
struct coroutine_traits<kj::Promise<workerd::api::DeferredProxy<T>>, Args...> {
  using promise_type = workerd::api::DeferredProxyCoroutine<T, Args...>;
};

}  // namespace KJ_COROUTINE_STD_NAMESPACE

namespace workerd::api {

class BeginDeferredProxyingConstant final {};
// A magic constant which a DeferredProxyPromise<T> coroutine can `KJ_CO_MAGIC` to indicate that the
// deferred proxying phase of its operation has begun.
constexpr BeginDeferredProxyingConstant BEGIN_DEFERRED_PROXYING{};

// A concept which is true if C is a coroutine adapter which supports the `co_yield` operator for
// type T. We could also check that the expression results in an awaitable, but that is already a
// compile error in other ways.
template <typename T, typename C>
concept CoroutineYieldValue = requires(T&& v, C coroutineAdapter) {
  { coroutineAdapter.yield_value(kj::fwd<T>(v)) };
};

// The coroutine adapter type for DeferredProxyPromise<T>. Most of the work is forwarded to the
// regular kj::Promise<T> coroutine adapter.
template <typename T, typename... Args>
class DeferredProxyCoroutine: public kj::_::PromiseNode,
                              public kj::_::CoroutineMixin<DeferredProxyCoroutine<T, Args...>, T> {
  using InnerCoroutineAdapter =
      typename kj::_::stdcoro::coroutine_traits<kj::Promise<T>, Args...>::promise_type;

 public:
  using Handle = kj::_::stdcoro::coroutine_handle<DeferredProxyCoroutine>;

  DeferredProxyCoroutine(kj::SourceLocation location = {})
      : inner(Handle::from_promise(*this), location) {}

  kj::Promise<DeferredProxy<T>> get_return_object() {
    // We need to return a RAII object which will destroy this (as in, `this`) coroutine adapter.
    // The logic which calls `coroutine_handle<>::destroy()` is tucked away in our inner coroutine
    // adapter, however, leading to the weird situation where the `inner.get_return_object()`
    // Promise owns `this`. And `this` owns `inner.get_return_object()` transitively via `result`!
    //
    // Fortunately, DeferredProxyCoroutine implements the PromiseNode interface, meaning when our
    // returned Promise is eventually dropped, our `PromiseNode::destroy()` implementation will be
    // called. This gives us the opportunity (that is, in `destroy()`) to destroy our
    // `inner.get_return_object()` Promise, breaking the ownership cycle and destroying `this`.

    result.value = DeferredProxy<T>{inner.get_return_object()};
    return kj::_::PromiseNode::to<kj::Promise<DeferredProxy<T>>>(kj::_::OwnPromiseNode(this));
  }

  auto initial_suspend() {
    return inner.initial_suspend();
  }
  auto final_suspend() noexcept {
    return inner.final_suspend();
  }
  // Just trivially forward these.

  void unhandled_exception() {
    // Reject our outer promise if it hasn't yet been fulfilled, then forward to the inner
    // implementation.

    rejectOuterPromise();
    inner.unhandled_exception();
  }

  kj::_::stdcoro::suspend_never yield_value(decltype(BEGIN_DEFERRED_PROXYING)) {
    // This allows us to write `KJ_CO_MAGIC` within a DeferredProxyPromise<T> coroutine to fulfill
    // the coroutine's outer promise with a DeferredProxy<T>.
    //
    // This could alternatively be implemented as an await_transform() with a magic parameter type.

    fulfillOuterPromise();
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

    fulfillOuterPromise();
    inner.fulfill(kj::mv(value));
  }

  template <typename U>
  decltype(auto) await_transform(U&& awaitable) {
    // Trivially forward everything, so we can await anything a kj::Promise<T> can.
    return inner.await_transform(kj::fwd<U>(awaitable));
  }

  operator kj::_::CoroutineBase&() {
    return inner;
  }
  // Required by Awaiter<T>::await_suspend() to support awaiting Promises.

 private:
  void fulfillOuterPromise() {
    // Fulfill the outer promise if it hasn't already settled.

    if (!deferredProxyingHasBegun) {
      // Our `result` is put in place already by `get_return_object()`, so all we have to do is arm
      // the event.
      onReadyEvent.arm();
      deferredProxyingHasBegun = true;
    }
  }

  void rejectOuterPromise() {
    // Reject the outer promise if it hasn't already settled.

    if (!deferredProxyingHasBegun) {
      result.addException(kj::getCaughtExceptionAsKj());
      onReadyEvent.arm();
      deferredProxyingHasBegun = true;
    }
  }

  // PromiseNode implementation

  void setSelfPointer(kj::_::OwnPromiseNode* selfPtr) noexcept override {
    this->selfPtr = selfPtr;
  }

  void destroy() override {
    // The promise returned by `inner.get_return_object()` is what actually owns this coroutine
    // frame. We temporarily store that in `result` until our outer promise is fulfilled. So, to
    // destroy ourselves, we must manually drop `result`.
    //
    // On the other hand, if our outer promise has already been fulfilled, then `result` has already
    // been delivered to wherever it is going, and someone else directly owns the coroutine now, not
    // us. In this case, this `destroy()` override will have already been called (and it will have
    // been a no-op), because our own OwnPromiseNode will have already been dropped in `get()`.

    auto drop = kj::mv(result);
  }

  void onReady(kj::_::Event* event) noexcept override {
    onReadyEvent.init(event);
  }

  void get(kj::_::ExceptionOrValue& output) noexcept override {
    // Make sure that the outer PromiseNode (`this` one) is destroyed before the inner PromiseNode.
    // kj-async should already provide us this guarantee, but since incorrect destruction order
    // would cause invalid memory access, we provide a stronger guarantee. Also see the comment for
    // the `result` data member.
    KJ_ASSERT(selfPtr != nullptr);
    KJ_DEFER(*selfPtr = nullptr);

    static_cast<decltype(result)&>(output) = kj::mv(result);
  }

  void tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) override {
    // The PromiseNode we're waiting on is whatever the coroutine is waiting on.
    static_cast<kj::_::PromiseNode&>(inner).tracePromise(builder, stopAtNextEvent);

    // Maybe returning the address of get() will give us a function name with meaningful type
    // information.
    builder.add(getMethodStartAddress(implicitCast<PromiseNode&>(*this), &PromiseNode::get));
  }

  // We defer the majority of the implementation to the regular kj::Promise<T> coroutine adapter.
  InnerCoroutineAdapter inner;

  // Helper to arm the event which fires when the outer promise (that is, `this` PromiseNode) for
  // the DeferredProxy<T> is ready.
  OnReadyEvent onReadyEvent;

  // Stores the result for the outer promise.
  //
  // WARNING: This object owns `this` PromiseNode! If `result` is ever moved away, as is done in
  // `get()`, we must arrange to make sure that no one ever tries to use `this` PromiseNode again.
  // Stated another way, we must guarantee that the outer PromiseNode (for `DeferredProxy<T>`) is
  // always destroyed before the inner PromiseNode (for `T`). kj-async always does this anyway, but
  // we implement an additional safeguard by immediately destroying our own `OwnPromiseNode` (which
  // we have access to via `setSelfPointer()`) when we move `result` away in `get()`.
  kj::_::ExceptionOr<DeferredProxy<T>> result;

  // Used to drop ourselves in `get()` -- see comment for `result`.
  kj::_::OwnPromiseNode* selfPtr = nullptr;

  // Set to true when deferred proxying has begun -- that is, when the outer DeferredProxy<T>
  // promise is fulfilled by calling `onReadyEvent.arm()`.
  bool deferredProxyingHasBegun = false;
};

}  // namespace workerd::api
