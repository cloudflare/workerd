// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>

namespace workerd {

namespace _ {

template<typename Functor>
struct CaptureForCoroutine {
  kj::Maybe<Functor> maybeFunctor;

  explicit CaptureForCoroutine(Functor&& f) : maybeFunctor(kj::mv(f)) {}

  template<typename ...Args>
  static auto coInvoke(Functor functor, Args&&... args)
      -> decltype(functor(kj::fwd<Args>(args)...)) {
    // Since the functor is now in the local scope and no longer a member variable, it will be
    // persisted in the coroutine state.

    // Note that `co_await functor(...)` can still return `void`. It just happens that
    // `co_return voidReturn();` is explicitly allowed.
    co_return co_await functor(kj::fwd<Args>(args)...);
  }

  template<typename ...Args>
  auto operator()(Args&&... args) {
    KJ_IREQUIRE(maybeFunctor != nullptr,
        "Attempted to invoke CaptureForCoroutine functor multiple times");
    auto localFunctor = kj::mv(*kj::_::readMaybe(maybeFunctor));
    maybeFunctor = nullptr;
    return coInvoke(kj::mv(localFunctor), kj::fwd<Args>(args)...);
  }
};

}  // namespace _

template <typename Functor>
auto coCapture(Functor&& f) {
  // Assuming `f()` returns a Promise<T> `p`, wrap `f` in such a way that it will outlive its
  // returned Promise. Note that the returned object may only be invoked once.
  //
  // This function is meant to help address this pain point with functors that return a coroutine:
  // https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rcoro-capture
  //
  // The two most common patterns where this may be useful look like so:
  // ```
  // void addTask(Value myValue) {
  //   auto myFun = [myValue]() -> kj::Promise<void> {
  //     ...
  //     co_return;
  //   };
  //   tasks.add(myFun());
  // }
  // ```
  // and
  // ```
  // kj::Promise<void> afterPromise(kj::Promise<void> promise, Value myValue) {
  //   auto myFun = [myValue]() -> kj::Promise<void> {
  //     ...
  //     co_return;
  //   };
  //   return promise.then(kj::mv(myFun));
  // }
  // ```
  //
  // Note that there are potentially more optimal alternatives to both of these patterns:
  // ```
  // void addTask(Value myValue) {
  //   auto myFun = [](auto myValue) -> kj::Promise<void> {
  //     ...
  //     co_return;
  //   };
  //   tasks.add(myFun(myValue));
  // }
  // ```
  // and
  // ```
  // kj::Promise<void> afterPromise(kj::Promise<void> promise, Value myValue) {
  //   auto myFun = [&]() -> kj::Promise<void> {
  //     ...
  //     co_return;
  //   };
  //   co_await promise;
  //   co_await myFun();
  //   co_return;
  // }
  // ```
  //
  // For situations where you are trying to capture a specific local variable, kj::mvCapture() can
  // also be useful:
  // ```
  // kj::Promise<void> reactToPromise(kj::Promise<MyType> promise) {
  //   BigA a;
  //   TinyB b;
  //
  //   doSomething(a, b);
  //   return promise.then(kj::mvCapture(b, [](TinyB b, MyType type) -> kj::Promise<void> {
  //     ...
  //     co_return;
  //   });
  // }
  // ```

  return _::CaptureForCoroutine(kj::mv(f));
}

}  // namespace workerd
