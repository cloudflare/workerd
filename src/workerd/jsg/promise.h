// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async.h>
#include <kj/table.h>
#include "jsg.h"
#include "util.h"
#include "wrappable.h"

namespace workerd::jsg {

// =======================================================================================
// Utilities for wrapping arbitrary C++ in an opaque way. See wrapOpaque().
//
// At present this is used privately in the Promise implementation, but we could consider making
// wrapOpaque() more public if it is useful.

template <typename T>
constexpr bool isV8Ref(T*) { return false; }
template <typename T>
constexpr bool isV8Ref(V8Ref<T>*) { return true; }

template <typename T>
constexpr bool isV8Ref() { return isV8Ref((T*)nullptr); }

template <typename T>
constexpr bool isV8Local(T*) { return false; }
template <typename T>
constexpr bool isV8Local(v8::Local<T>*) { return true; }

template <typename T>
constexpr bool isV8Local() { return isV8Local((T*)nullptr); }

template <typename T, bool = isGcVisitable<T>()>
struct OpaqueWrappable;

template <typename T>
struct OpaqueWrappable<T, false>: public Wrappable {
  // Used to implement wrapOpaque().

  OpaqueWrappable(T&& value)
      : value(kj::mv(value)) {}

  T value;
  bool movedAway = false;
};

template <typename T>
struct OpaqueWrappable<T, true>: public OpaqueWrappable<T, false> {
  // When T is GC-visitable, make sure to implement visitation.

  using OpaqueWrappable<T, false>::OpaqueWrappable;

  void jsgVisitForGc(GcVisitor& visitor) override {
    if (!this->movedAway) {
      visitor.visit(this->value);
    }
  }
};

template <typename T>
v8::Local<v8::Value> wrapOpaque(v8::Local<v8::Context> context, T&& t) {
  // Create a JavaScript value that wraps `t` in an opaque way. JS code will see this as an empty
  // object, as if created by `{}`, but C++ code can unwrap the handle with `unwrapOpaque()`.
  //
  // If `T` is a type that can be passed to GcVisitor::visit(), then it will be visited whenever
  // the opaque handle is found to be reachable.
  //
  // Generally, the opaque handle should not actually be passed to the application at all. This
  // is useful in cases where the producer and consumer are both C++ code, but V8 requires that
  // a handle be used for some reason. For example, this is used to pass C++ values through V8
  // Promises.
  //
  // Opaque-wrapping of `V8Ref<T>` is explicitly disallowed to avoid waste. Just use the handle
  // directly in this case. If you really want to wrap a V8Ref opaquely, wrap it in a struct of
  // your own first. (Don't forget to implement `visitForGc()`.)

  static_assert(!kj::isReference<T>());
  static_assert(!isV8Ref<T>(), "no need to opaque-wrap regular JavaScript values");
  static_assert(!isV8Local<T>(), "can't opaque-wrap non-persistent handles");

  auto wrapped = kj::refcounted<OpaqueWrappable<T>>(kj::mv(t));
  return wrapped->attachOpaqueWrapper(context, isGcVisitable<T>());
}

template <typename T>
T unwrapOpaque(v8::Isolate* isolate, v8::Local<v8::Value> handle) {
  // Unwraps a handle created using `wrapOpaque()`. This consumes (moves away) the underlying
  // value, so can only be called once. Throws if the handle is the wrong type or has already been
  // consumed previously.

  static_assert(!kj::isReference<T>());
  static_assert(!isV8Ref<T>(), "no need to opaque-wrap regular JavaScript values");
  static_assert(!isV8Local<T>(), "can't opaque-wrap non-persistent handles");

  Wrappable& wrappable = KJ_ASSERT_NONNULL(Wrappable::tryUnwrapOpaque(isolate, handle));
  OpaqueWrappable<T>* holder = dynamic_cast<OpaqueWrappable<T>*>(&wrappable);
  KJ_ASSERT(holder != nullptr);
  KJ_ASSERT(!holder->movedAway);
  holder->movedAway = true;
  return kj::mv(holder->value);
}

template <typename T>
T& unwrapOpaqueRef(v8::Isolate* isolate, v8::Local<v8::Value> handle) {
  // Unwraps a handle created using `wrapOpaque()`, without consuming the value.  Throws if the
  // handle is the wrong type or has already been consumed previously.

  static_assert(!kj::isReference<T>());
  static_assert(!isV8Ref<T>(), "no need to opaque-wrap regular JavaScript values");
  static_assert(!isV8Local<T>(), "can't opaque-wrap non-persistent handles");

  Wrappable& wrappable = KJ_ASSERT_NONNULL(Wrappable::tryUnwrapOpaque(isolate, handle));
  OpaqueWrappable<T>* holder = dynamic_cast<OpaqueWrappable<T>*>(&wrappable);
  KJ_ASSERT(holder != nullptr);
  KJ_ASSERT(!holder->movedAway);
  return holder->value;
}

template <typename T>
void dropOpaque(v8::Isolate* isolate, v8::Local<v8::Value> handle) {
  // Destroys the value contained by an opaque handle, without returning it. This is equivalent
  // to calling unwrapOpaque<T>() and dropping the result, except that if the handle is the wrong
  // type, this function silently does nothing rather than throw.

  static_assert(!kj::isReference<T>());
  static_assert(!isV8Ref<T>());

  KJ_IF_MAYBE(wrappable, Wrappable::tryUnwrapOpaque(isolate, handle)) {
    OpaqueWrappable<T>* holder = dynamic_cast<OpaqueWrappable<T>*>(wrappable);
    if (holder != nullptr) {
      holder->movedAway = true;
      auto drop KJ_UNUSED = kj::mv(holder->value);
    }
  }
}

// =======================================================================================
// Promise implementation

template <typename ThenFunc, typename CatchFunc>
struct ThenCatchPair {
  // This type (opaque-wrapped) is the type of the "data" for a continuation callback. We have both
  // the success and error callbacks share the same "data" object so that both underlying C++
  // callbacks are proactively destroyed after one of the runs. Otherwise, we'd only destroy the
  // function that was called, while the other one would have to wait for GC, which may mean
  // keeping around C++ resources longer than necessary.

  ThenFunc thenFunc;
  CatchFunc catchFunc;
};

template <typename FuncPairType, bool passLock, bool isCatch, typename Input, typename Output>
void promiseContinuation(const v8::FunctionCallbackInfo<v8::Value>& args) {
  // FunctionCallback implementing a C++ .then() continuation on a JS promise.
  //
  // We expect the input is already an opaque-wrapped value, args.Data() is an opaque-wrapped C++
  // function to eoxecute, and we want to produce an opaque-wrapped output or Promise.
  liftKj(args, [&]() {
    auto isolate = args.GetIsolate();
#ifdef KJ_DEBUG
    // In debug mode only, we verify that the function hasn't captured any KJ heap objects without
    // a IoOwn. We don't bother with this check in release mode because it's pretty deterministic,
    // so it's likely to be caught in debug, and we'd like to avoid the extra overhead in releases.
    DISALLOW_KJ_IO_DESTRUCTORS_SCOPE;
#endif
    auto funcPair = unwrapOpaque<FuncPairType>(isolate, args.Data());
#ifdef KJ_DEBUG
    kj::AllowAsyncDestructorsScope allowAsyncDestructors;
#endif
    auto callFunc = [&]() -> Output {
      if constexpr (passLock) {
        auto& js = Lock::from(isolate);
        if constexpr (isCatch) {
          // Exception from V8 is not expected to be opaque-wrapped. It's just a Value.
          return funcPair.catchFunc(js, Value(isolate, args[0]));
        } else if constexpr (isVoid<Input>()) {
          return funcPair.thenFunc(js);
        } else if constexpr (isV8Ref<Input>()) {
          return funcPair.thenFunc(js, Input(isolate, args[0]));
        } else {
          return funcPair.thenFunc(js, unwrapOpaque<Input>(isolate, args[0]));
        }
      } else {
        // DEPRECATED: Callbacks that don't take a Lock parameter.
        if constexpr (isCatch) {
          // Exception from V8 is not expected to be opaque-wrapped. It's just a Value.
          return funcPair.catchFunc(Value(isolate, args[0]));
        } else if constexpr (isVoid<Input>()) {
          return funcPair.thenFunc();
        } else if constexpr (isV8Ref<Input>()) {
          return funcPair.thenFunc(Input(isolate, args[0]));
        } else {
          return funcPair.thenFunc(unwrapOpaque<Input>(isolate, args[0]));
        }
      }
    };
    if constexpr (isVoid<Output>()) {
      callFunc();
    } else if constexpr (isPromise<Output>()) {
      // Continuation returns Promise. We don't want to opaque-wrap that, we want to return it
      // raw, so that the V8 Promise machinery will chain it.

      // We cast the return value to v8::Local<v8::Value> so that it doesn't trigger liftKj()'s
      // special handling of promises, where it tries to catch exceptions and merge them into the
      // promise. We don't need to do this, because this is being called as a .then() which already
      // catches exceptions and does the right thing.
      return v8::Local<v8::Value>(callFunc().consumeHandle(isolate));
    } else if constexpr (isV8Ref<Output>()) {
      return callFunc().getHandle(isolate);
    } else {
      return wrapOpaque(isolate->GetCurrentContext(), callFunc());
    }
  });
}

template <typename FuncPairType, bool isCatch>
void identityPromiseContinuation(const v8::FunctionCallbackInfo<v8::Value>& args) {
  // Promise continuation that propagates the value or exception unmodified, but makes sure to
  // proactively destroy the ThenCatchPair.
  auto isolate = args.GetIsolate();
  dropOpaque<FuncPairType>(isolate, args.Data());
  if constexpr (isCatch) {
    isolate->ThrowException(args[0]);
  } else {
    args.GetReturnValue().Set(args[0]);
  }
}

template <typename TypeWrapper>
class PromiseWrapper;

template <typename T>
class Promise {
public:
  static_assert(!kj::canConvert<T*, v8::Data*>(),
      "jsg::Promise<T> expects T to be an instantiable C++ type, not a JS heap type; use "
      "jsg::Promise<jsg::V8Ref<T>> to represent a promise for a JavaScript heap object.");

  Promise(v8::Isolate* isolate, v8::Local<v8::Promise> v8Promise)
      : deprecatedIsolate(isolate), v8Promise(V8Ref<v8::Promise>(isolate, v8Promise)) {}

  Promise(decltype(nullptr)): deprecatedIsolate(nullptr), v8Promise(nullptr) {}
  // For use when you're declaring a local variable that will be initialized later.

  void markAsHandled(Lock& js) {
    auto promise = getInner(js);
    promise->MarkAsHandled();
    markedAsHandled = true;
  }

  template <bool passLock = true, typename Func, typename ErrorFunc>
  PromiseForResult<Func, T, passLock> then(Lock& js, Func&& func, ErrorFunc&& errorFunc) {
    typedef ReturnType<Func, T, passLock> Output;
    static_assert(kj::isSameType<Output, ReturnType<ErrorFunc, Value, passLock>>(),
        "functions passed to .then() must return exactly the same type");

    typedef ThenCatchPair<Func, ErrorFunc> FuncPair;
    return thenImpl<Output>(js,
        FuncPair { kj::fwd<Func>(func), kj::fwd<ErrorFunc>(errorFunc) },
        &promiseContinuation<FuncPair, passLock, false, T, Output>,
        &promiseContinuation<FuncPair, passLock, true, Value, Output>);
  }

  template <bool passLock = true, typename Func>
  PromiseForResult<Func, T, passLock> then(Lock& js, Func&& func) {
    typedef ReturnType<Func, T, passLock> Output;

    // HACK: The error function is never called, so it need not actually be a functor.
    typedef ThenCatchPair<Func, bool> FuncPair;
    return thenImpl<Output>(js,
        FuncPair { kj::fwd<Func>(func), false },
        &promiseContinuation<FuncPair, passLock, false, T, Output>,
        &identityPromiseContinuation<FuncPair, true>);
  }

  template <bool passLock = true, typename ErrorFunc>
  Promise<T> catch_(Lock& js, ErrorFunc&& errorFunc) {
    static_assert(kj::isSameType<T, ReturnType<ErrorFunc, Value, passLock>>(),
        "function passed to .catch_() must return exactly the promise's type");

    // HACK: The non-error function is never called, so it need not actually be a functor.
    typedef ThenCatchPair<bool, ErrorFunc> FuncPair;
    return thenImpl<T>(js,
        FuncPair { false, kj::fwd<ErrorFunc>(errorFunc) },
        &identityPromiseContinuation<FuncPair, false>,
        &promiseContinuation<FuncPair, passLock, true, Value, T>);
  }

  Promise<void> whenResolved(Lock& js) {
    // whenResolved returns a new Promise<void> that resolves when this promise resolves,
    // stopping the propagation of the resolved value. Unlike then(), calling whenResolved()
    // does not consume the promise, and whenResolved() can be called multiple times,
    // with each call creating a new branch off the original promise. Another key difference
    // with whenResolved() is that the markAsHandled status will propagate to the new Promise<void>
    // returned by whenResolved().
    auto promise = Promise<void>(js.v8Isolate, getInner(js));
    if (markedAsHandled) {
      promise.markAsHandled(js);
    }
    return kj::mv(promise);
  }

  v8::Local<v8::Promise> consumeHandle(Lock& js) {
    auto result = getInner(js);
    v8Promise = nullptr;
    return result;
  }

  kj::Maybe<T> tryConsumeResolved(Lock& js) {
    // If the promise is resolved, return the result, consuming the Promise. If it is pending
    // or rejected, returns null. This can be used as an optimization or in tests, but you must
    // never rely on it for correctness.

    v8::HandleScope scope(js.v8Isolate);

    auto handle = KJ_REQUIRE_NONNULL(v8Promise, "jsg::Promise can only be used once")
        .getHandle(js.v8Isolate);
    switch (handle->State()) {
      case v8::Promise::kPending:
      case v8::Promise::kRejected:
        return nullptr;
      case v8::Promise::kFulfilled:
        v8Promise = nullptr;
        return unwrapOpaque<T>(js.v8Isolate, handle->Result());
    }
  }

  class Resolver {
  public:
    Resolver(v8::Isolate* isolate, v8::Local<v8::Promise::Resolver> v8Resolver)
        : deprecatedIsolate(isolate), v8Resolver(isolate, kj::mv(v8Resolver)) {}

    template <typename U = T, typename = kj::EnableIf<!isVoid<U>()>>
    void resolve(Lock& js, kj::NoInfer<U>&& value) {
      auto isolate = js.v8Isolate;
      v8::HandleScope scope(isolate);
      auto context = js.v8Context();
      v8::Local<v8::Value> handle;
      if constexpr (isV8Ref<U>()) {
        handle = value.getHandle(isolate);
      } else {
        handle = wrapOpaque(context, kj::mv(value));
      }
      check(v8Resolver.getHandle(isolate)->Resolve(context, handle));
    }

    template <typename U = T, typename = kj::EnableIf<isVoid<U>()>>
    void resolve(Lock& js) {
      auto isolate = js.v8Isolate;
      v8::HandleScope scope(isolate);
      check(v8Resolver.getHandle(isolate)->Resolve(
          js.v8Context(), v8::Undefined(isolate)));
    }

    void resolve(Lock& js, Promise&& promise) {
      // Resolve to another Promise.
      auto isolate = js.v8Isolate;
      check(v8Resolver.getHandle(isolate)->Resolve(
          js.v8Context(), promise.consumeHandle(isolate)));
    }

    void reject(Lock& js, v8::Local<v8::Value> exception) {
      auto isolate = js.v8Isolate;
      v8::HandleScope scope(isolate);
      check(v8Resolver.getHandle(isolate)->Reject(js.v8Context(), exception));
    }

    Resolver addRef(Lock& js) {
      return { js.v8Isolate, v8Resolver.getHandle(js.v8Isolate) };
    }

    void visitForGc(GcVisitor& visitor) {
      visitor.visit(v8Resolver);
    }

    // DEPRECATED: Versions that don't take `Lock`, same as with Promise.
    template <typename U = T, typename = kj::EnableIf<!isVoid<U>()>>
    void resolve(kj::NoInfer<U>&& value) {
      resolve(Lock::from(deprecatedIsolate), kj::mv(value));
    }
    template <typename U = T, typename = kj::EnableIf<isVoid<U>()>>
    void resolve() {
      resolve(Lock::from(deprecatedIsolate));
    }
    void resolve(Promise&& promise) {
      resolve(Lock::from(deprecatedIsolate), kj::mv(promise));
    }
    void reject(v8::Local<v8::Value> exception) {
      reject(Lock::from(deprecatedIsolate), kj::mv(exception));
    }
    void reject(Lock& js, kj::Exception exception) {
      reject(js, makeInternalError(deprecatedIsolate, kj::mv(exception)));
    }
    Resolver addRef() {
      return addRef(Lock::from(deprecatedIsolate));
    }

  private:
    v8::Isolate* deprecatedIsolate;
    V8Ref<v8::Promise::Resolver> v8Resolver;
  };

  void visitForGc(GcVisitor& visitor) {
    visitor.visit(v8Promise);
  }

  // DEPRECATED: The versions below do not take a `Lock` as the first param, but they do actually
  //   require a lock. These versions also do not pass a `Lock` to the callback.
  // TODO(clenaup): Update all call sites to the version that passes locks. Then, remove these and
  //   also remove the `isolate` parameter from this class.

  template <typename Func, typename ErrorFunc>
  auto then(Func&& func, ErrorFunc&& errorFunc)
      KJ_DEPRECATED("Use variant that takes Lock as the first param") {
    return then<false>(Lock::from(deprecatedIsolate),
        kj::fwd<Func>(func), kj::fwd<ErrorFunc>(errorFunc));
  }
  template <typename Func>
  auto then(Func&& func)
      KJ_DEPRECATED("Use variant that takes Lock as the first param") {
    return then<false>(Lock::from(deprecatedIsolate), kj::fwd<Func>(func));
  }
  template <typename ErrorFunc>
  auto catch_(ErrorFunc&& errorFunc)
      KJ_DEPRECATED("Use variant that takes Lock as the first param") {
    return catch_<false>(Lock::from(deprecatedIsolate), kj::fwd<ErrorFunc>(errorFunc));
  }
  Promise<void> whenResolved()
      KJ_DEPRECATED("Use variant that takes Lock as the first param") {
    return whenResolved(Lock::from(deprecatedIsolate));
  }
  v8::Local<v8::Promise> consumeHandle(v8::Isolate* isolate)
      KJ_DEPRECATED("Use variant that takes Lock as the first param") {
    return consumeHandle(Lock::from(isolate));
  }
  kj::Maybe<T> tryConsumeResolved()
      KJ_DEPRECATED("Use variant that takes Lock as the first param") {
    return tryConsumeResolved(Lock::from(deprecatedIsolate));
  }

private:
  v8::Isolate* deprecatedIsolate;
  // We store a copy of the isolate pointer so that `.then()` can be called without passing in
  // the isolate pointer every time.

  kj::Maybe<V8Ref<v8::Promise>> v8Promise;
  bool markedAsHandled = false;

  v8::Local<v8::Promise> getInner(Lock& js) {
    return KJ_REQUIRE_NONNULL(v8Promise, "jsg::Promise can only be used once")
        .getHandle(js.v8Isolate);
  }

  template <typename U = T, typename = kj::EnableIf<!isVoid<U>()>()>
  Promise(Lock& js, kj::NoInfer<U>&& value)
      : deprecatedIsolate(js.v8Isolate) {
    auto isolate = js.v8Isolate;
    v8::HandleScope scope(isolate);
    auto context = js.v8Context();
    auto resolver = check(v8::Promise::Resolver::New(context));
    v8::Local<v8::Value> handle;
    if constexpr (isV8Ref<U>()) {
      handle = value.getHandle(isolate);
    } else {
      handle = wrapOpaque(context, kj::mv(value));
    };
    check(resolver->Resolve(context, handle));
    v8Promise.emplace(isolate, resolver->GetPromise());
  }

  template <typename U = T, typename = kj::EnableIf<isVoid<U>()>()>
  explicit Promise(Lock& js)
      : deprecatedIsolate(js.v8Isolate) {
    auto isolate = js.v8Isolate;
    v8::HandleScope scope(isolate);
    auto context = js.v8Context();
    auto resolver = check(v8::Promise::Resolver::New(context));
    check(resolver->Resolve(context, v8::Undefined(isolate)));
    v8Promise.emplace(isolate, resolver->GetPromise());
  }

  template <typename Result, typename FuncPair>
  Promise<RemovePromise<Result>> thenImpl(
      Lock& js, FuncPair&& funcPair,
      v8::FunctionCallback thenCallback,
      v8::FunctionCallback errCallback) {
    v8::HandleScope scope(js.v8Isolate);
    auto context = js.v8Context();

    auto funcPairHandle = wrapOpaque(context, kj::mv(funcPair));

    auto then = check(v8::Function::New(
        context, thenCallback, funcPairHandle, 1, v8::ConstructorBehavior::kThrow));

    auto errThen = check(v8::Function::New(
        context, errCallback, funcPairHandle, 1, v8::ConstructorBehavior::kThrow));

    using Type = RemovePromise<Result>;

    return Promise<Type>(js.v8Isolate,
        check(consumeHandle(js)->Then(context, then, errThen)));
  }

  friend class Lock;
  template <typename TypeWrapper>
  friend class PromiseWrapper;
};

template <typename T>
class Promise<Promise<T>> {
  static_assert(sizeof(T*) == 0, "Promise<Promise<T>> is invalid; use Promise<T> instead");
};

template <typename T>
class Promise<kj::Promise<T>> {
  static_assert(sizeof(T*) == 0, "jsg::Promise<kj::Promise<T>> is illegal; you need a IoOwn!");
};

template <typename T>
struct PromiseResolverPair {
  Promise<T> promise;
  typename Promise<T>::Resolver resolver;
};

template <typename T>
PromiseResolverPair<T> Lock::newPromiseAndResolver() {
  v8::HandleScope scope(v8Isolate);
  auto context = v8Context();
  auto resolver = check(v8::Promise::Resolver::New(context));
  auto promise = resolver->GetPromise();
  return {
    { v8Isolate, promise },
    { v8Isolate, resolver }
  };
}

template <typename T>
inline Promise<T> Lock::resolvedPromise(T&& value) {
  return Promise<T>(*this, kj::fwd<T>(value));
}
inline Promise<void> Lock::resolvedPromise() {
  return Promise<void>(*this);
}

template <typename T>
Promise<T> Lock::rejectedPromise(v8::Local<v8::Value> exception) {
  auto [ promise, resolver ] = newPromiseAndResolver<T>();
  resolver.reject(exception);
  return kj::mv(promise);
}

template <typename T>
Promise<T> Lock::rejectedPromise(jsg::Value exception) {
  v8::HandleScope scope(v8Isolate);
  return rejectedPromise<T>(exception.getHandle(v8Isolate));
}

template <typename T>
Promise<T> Lock::rejectedPromise(kj::Exception&& exception) {
  v8::HandleScope scope(v8Isolate);
  return rejectedPromise<T>(makeInternalError(v8Isolate, kj::mv(exception)));
}

template <class Func>
PromiseForResult<Func, void, false> Lock::evalNow(Func&& func) {
  typedef RemovePromise<ReturnType<Func, void>> Result;
  v8::TryCatch tryCatch(v8Isolate);
  try {
    if constexpr (isPromise<ReturnType<Func, void>>()) {
      return func();
    } else {
      return resolvedPromise<Result>(func());
    }
  } catch (jsg::JsExceptionThrown&) {
    if (tryCatch.HasCaught() && tryCatch.CanContinue()) {
      return rejectedPromise<Result>(tryCatch.Exception());
    } else {
      // probably TerminateExecution() called
      if (tryCatch.CanContinue()) tryCatch.ReThrow();
      throw;
    }
  } catch (kj::Exception& e) {
    return rejectedPromise<Result>(kj::mv(e));
  } catch (std::exception& exception) {
    return rejectedPromise<Result>(makeInternalError(v8Isolate, exception.what()));
  } catch (...) {
    return rejectedPromise<Result>(makeInternalError(v8Isolate,
        kj::str("caught unknown exception of type: ", kj::getCaughtExceptionType())));
  }
}

// DEPRECATED: These global functions should be replaced with the equivalent methods of `Lock`.
template <typename T>
inline PromiseResolverPair<T> newPromiseAndResolver(v8::Isolate* isolate) {
  return Lock::from(isolate).newPromiseAndResolver<T>();
}
template <typename T>
inline Promise<T> resolvedPromise(v8::Isolate* isolate, T&& value) {
  return Lock::from(isolate).resolvedPromise(kj::fwd<T>(value));
}
inline Promise<void> resolvedPromise(v8::Isolate* isolate) {
  return Lock::from(isolate).resolvedPromise();
}
template <typename T>
inline Promise<T> rejectedPromise(v8::Isolate* isolate, v8::Local<v8::Value> exception) {
  return Lock::from(isolate).rejectedPromise<T>(exception);
}
template <typename T>
inline Promise<T> rejectedPromise(v8::Isolate* isolate, jsg::Value exception) {
  return Lock::from(isolate).rejectedPromise<T>(kj::mv(exception));
}
template <typename T>
inline Promise<T> rejectedPromise(v8::Isolate* isolate, kj::Exception&& exception) {
  return Lock::from(isolate).rejectedPromise<T>(kj::mv(exception));
}
template <class Func>
PromiseForResult<Func, void, false> evalNow(v8::Isolate* isolate, Func&& func) {
  return Lock::from(isolate).evalNow(kj::fwd<Func>(func));
}

// -----------------------------------------------------------------------------

template <typename TypeWrapper, typename Input>
void thenWrap(const v8::FunctionCallbackInfo<v8::Value>& args) {
  // Continuation function that converts a promised C++ value into a JavaScript value.

  if constexpr (isVoid<Input>()) {
    // No wrapping needed. Note that we still attach `thenWrap` to the promise chain only because
    // we use `args.data` to prevent the object from being GC'd while the promise is still
    // executing.
    args.GetReturnValue().SetUndefined();
  } else if constexpr (isV8Ref<Input>()) {
    // Similarly, no unwrapping needed.
    args.GetReturnValue().Set(args[0]);
  } else {
    liftKj(args, [&]() {
      v8::Isolate* isolate = args.GetIsolate();
      auto& wrapper = TypeWrapper::from(isolate);
      auto context = isolate->GetCurrentContext();
      return wrapper.wrap(context, nullptr, unwrapOpaque<Input>(isolate, args[0]));
    });
  }
}

template <typename TypeWrapper, typename Output>
void thenUnwrap(const v8::FunctionCallbackInfo<v8::Value>& args) {
  // Continuation function that converts a promised JavaScript value into a C++ value.
  liftKj(args, [&]() {
    v8::Isolate* isolate = args.GetIsolate();
    auto& wrapper = TypeWrapper::from(isolate);
    auto context = isolate->GetCurrentContext();
    return wrapOpaque(context, wrapper.template unwrap<Output>(context, args[0],
        TypeErrorContext::promiseResolution()));
  });
}

template <typename TypeWrapper>
class PromiseWrapper {
  // TypeWrapper mixin for Promise.

public:
  template <typename T>
  static constexpr const char* getName(Promise<T>*) { return "Promise"; }

  template <typename T>
  v8::Local<v8::Promise> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator,
      Promise<T>&& promise) {
    // Add a .then() to unwrap the value (i.e. convert C++ value to JavaScript).
    //
    // We use `creator` as the `data` value for this continuation so that the creator object
    // cannot be GC'd while the callback still exists. This gives us the KJ-style guarantee that
    // the object whose method returned the promise will not be destroyed while the promise is
    // still executing.
    auto markedAsHandled = promise.markedAsHandled;
    auto then = check(v8::Function::New(context,
        &thenWrap<TypeWrapper, T>, creator.orDefault({}), 1, v8::ConstructorBehavior::kThrow));

    auto ret = check(promise.consumeHandle(context->GetIsolate())->Then(context, then));
    // Although we added a .then() to the promise to translate the value to JavaScript, we would
    // like things to behave as if the C++ code returned this Promise directly to JavaScript. In
    // particular, if the C++ code marked the Promise handled, then the derived JavaScript promise
    // ought to be marked as handled as well.
    if (markedAsHandled) {
      ret->MarkAsHandled();
    }

    return ret;
  }

  template <typename T>
  kj::Maybe<Promise<T>> tryUnwrap(
      v8::Local<v8::Context> context, v8::Local<v8::Value> handle,
      Promise<T>*, kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsPromise()) {
      auto promise = handle.As<v8::Promise>();
      if constexpr (!isVoid<T>() && !isV8Ref<T>()) {
        // Add a .then() to unwrap the promise's resolution (i.e. convert it from JS to C++).
        // Note that we don't need to handle the rejection case here as there is no wrapping
        // applied to exception values, so we just let it propagate through.
        //
        // TODO(perf): We could in theory check if promise->State() is kFulfilled and, in that
        //   case, pull out promise->Result(), unwrap it, and make a new immediate promise.
        //   Similarly in `wrap()`. Not clear if the added complexity is worth it, though.
        auto then = check(v8::Function::New(context,
            &thenUnwrap<TypeWrapper, T>, {}, 1, v8::ConstructorBehavior::kThrow));
        promise = check(promise->Then(context, then));
      }
      return Promise<T>(context->GetIsolate(), promise);
    } else {
      // Input is a resolved value (not a promise). Try to unwrap it now.
      if constexpr (isVoid<T>()) {
        // When expecting Promise<void>, we treat absolutely any non-promise value as being
        // an immediately-resolved promise. This is consistent with JavaScript where you'd
        // commonly use `Promise.resolve(param).then(() => {...})` in order to coerce the param
        // to a promise... normally you wouldn't bother checking that the param specifically
        // resolved to `undefined`, you'd just throw away whatever it resolved to.
        //
        // It's possible to argue that we should actually allow only `undefined` here but
        // changing it now could break existing users, e.g. html-rewriter.ew-test is broken
        // because it writes `() => someExpression()` for a callback that's supposed to
        // optionally return Promise<void> -- it seems like the callback isn't acutally intending
        // to return the result of `someExpression()` but does so by accident since the braces
        // are missing. This is probably common in user code, too.
        return resolvedPromise(context->GetIsolate());
      } else {
        auto& wrapper = *static_cast<TypeWrapper*>(this);
        KJ_IF_MAYBE(value,
            wrapper.tryUnwrap(context, handle, (T*)nullptr, parentObject)) {
          return resolvedPromise<T>(context->GetIsolate(), kj::mv(*value));
        } else {
          // Wrong type.
          return nullptr;
        }
      }
    }
  }
};

// -----------------------------------------------------------------------------

class UnhandledRejectionHandler {
  // A utility used internally by ServiceWorkerGlobalScope to perform the book keeping
  // for unhandled promise rejection notifications. The handler maintains a table of
  // weak references to rejected promises that have not been handled and will handle
  // emitting events and console warnings as appropriate.
public:
  using Handler = void(jsg::Lock& js,
                       v8::PromiseRejectEvent event,
                       jsg::V8Ref<v8::Promise> promise,
                       jsg::Value value);

  explicit UnhandledRejectionHandler(kj::Function<Handler> handler)
      : handler(kj::mv(handler)) {}

  void report(jsg::Lock& js,
              v8::PromiseRejectEvent event,
              jsg::V8Ref<v8::Promise> promise,
              jsg::Value value);

  void clear();

private:
  struct UnhandledRejection {
    // Used as part of the book keeping for unhandled rejections. When an
    // unhandled rejection occurs, the unhandledRejections Table will be updated.
    // If the rejection is later handled asynchronously, then the item will be
    // removed from the table. When the unhandled rejection table is processed
    // later in the event loop tick, any remaining rejections will generate a
    // warning to the inspector console (if enabled);

    explicit UnhandledRejection(
        jsg::Lock& js,
        jsg::V8Ref<v8::Promise> promise,
        jsg::Value value,
        v8::Local<v8::Message> message,
        size_t rejectionNumber);

    ~UnhandledRejection();

    UnhandledRejection(UnhandledRejection&& other) = default;
    UnhandledRejection& operator=(UnhandledRejection&& other) = default;

    // TODO(cleanup): It would be better to use a jsg::HashableV8Ref or
    // jsg::Identity here but we need the Globals to always be weak so
    // that the book keeping doesn't end up being a memory leak.

    uint hash;
    // We use v8::Globals directly here because these references are going to
    // be made weak and could be garbage collected and cleared while the items
    // are still in the unhandledRejections or warnedRejections tables.
    v8::Global<v8::Promise> promise;
    v8::Global<v8::Value> value;
    v8::Global<v8::Message> message;
    kj::Maybe<Ref<AsyncContextFrame>> asyncContextFrame;

    inline bool isAlive() { return !promise.IsEmpty() && !value.IsEmpty(); }

    size_t rejectionNumber;

    uint hashCode() const { return hash; }
  };

  struct HashedPromise {
    // A v8::Promise with memoized hash code.
    v8::Local<v8::Promise> promise;
    uint hash;

    HashedPromise(v8::Local<v8::Promise> promise)
        : promise(promise), hash(promise->GetIdentityHash()) {}
  };

  struct UnhandledRejectionCallbacks {
    inline const UnhandledRejection& keyForRow(const UnhandledRejection& row) const { return row; }
    inline bool matches(const UnhandledRejection& a, const UnhandledRejection& b) const {
      return a.promise == b.promise;
    }
    inline bool matches(const UnhandledRejection& a, const HashedPromise& b) const {
      return a.promise == b.promise;
    }
    inline uint hashCode(const UnhandledRejection& row) const { return row.hashCode(); }
    inline uint hashCode(const HashedPromise& key) const { return key.hash; }
  };

  kj::Function<Handler> handler;
  bool scheduled = false;
  size_t rejectionCount = 0;

  using UnhandledRejectionsTable =
      kj::Table<UnhandledRejection, kj::HashIndex<UnhandledRejectionCallbacks>>;

  UnhandledRejectionsTable unhandledRejections;
  UnhandledRejectionsTable warnedRejections;

  void rejectedWithNoHandler(jsg::Lock& js, jsg::V8Ref<v8::Promise> promise, jsg::Value value);
  void handledAfterRejection(jsg::Lock& js, jsg::V8Ref<v8::Promise> promise);
  void ensureProcessingWarnings(jsg::Lock& js);
};

}  // namespace workerd::jsg
