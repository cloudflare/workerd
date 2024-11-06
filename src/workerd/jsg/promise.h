// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "jsg.h"
#include "util.h"
#include "wrappable.h"

#include <kj/async.h>
#include <kj/table.h>

namespace workerd::jsg {

// =======================================================================================
// Utilities for wrapping arbitrary C++ in an opaque way. See wrapOpaque().
//
// At present this is used privately in the Promise implementation, but we could consider making
// wrapOpaque() more public if it is useful.

template <typename T, bool = isGcVisitable<T>()>
struct OpaqueWrappable;

template <typename T>
struct OpaqueWrappable<T, false>: public Wrappable {
  // Used to implement wrapOpaque().

  OpaqueWrappable(T&& value): value(kj::mv(value)) {}

  T value;
  bool movedAway = false;

  kj::StringPtr jsgGetMemoryName() const override {
    return "OpaqueWrappable"_kjc;
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(OpaqueWrappable);
  }
  void jsgGetMemoryInfo(MemoryTracker& tracker) const override {
    Wrappable::jsgGetMemoryInfo(tracker);
  }
};

template <typename T>
struct OpaqueWrappable<T, true>: public OpaqueWrappable<T, false> {
  // When T is GC-visitable, make sure to implement visitation.

  using OpaqueWrappable<T, false>::OpaqueWrappable;

  kj::StringPtr jsgGetMemoryName() const override {
    return "OpaqueWrappable"_kjc;
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(OpaqueWrappable);
  }
  void jsgGetMemoryInfo(MemoryTracker& tracker) const override {
    Wrappable::jsgGetMemoryInfo(tracker);
  }

  void jsgVisitForGc(GcVisitor& visitor) override {
    if (!this->movedAway) {
      visitor.visit(this->value);
    }
  }
};

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
template <typename T>
v8::Local<v8::Value> wrapOpaque(v8::Local<v8::Context> context, T&& t) {
  static_assert(!kj::isReference<T>());
  static_assert(!isV8Ref<T>(), "no need to opaque-wrap regular JavaScript values");
  static_assert(!isV8Local<T>(), "can't opaque-wrap non-persistent handles");

  auto wrapped = kj::refcounted<OpaqueWrappable<T>>(kj::mv(t));
  return wrapped->attachOpaqueWrapper(context, isGcVisitable<T>());
}

// Unwraps a handle created using `wrapOpaque()`. This consumes (moves away) the underlying
// value, so can only be called once. Throws if the handle is the wrong type or has already been
// consumed previously.
template <typename T>
T unwrapOpaque(v8::Isolate* isolate, v8::Local<v8::Value> handle) {
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

// Unwraps a handle created using `wrapOpaque()`, without consuming the value.  Throws if the
// handle is the wrong type or has already been consumed previously.
template <typename T>
T& unwrapOpaqueRef(v8::Isolate* isolate, v8::Local<v8::Value> handle) {
  static_assert(!kj::isReference<T>());
  static_assert(!isV8Ref<T>(), "no need to opaque-wrap regular JavaScript values");
  static_assert(!isV8Local<T>(), "can't opaque-wrap non-persistent handles");

  Wrappable& wrappable = KJ_ASSERT_NONNULL(Wrappable::tryUnwrapOpaque(isolate, handle));
  OpaqueWrappable<T>* holder = dynamic_cast<OpaqueWrappable<T>*>(&wrappable);
  KJ_ASSERT(holder != nullptr);
  KJ_ASSERT(!holder->movedAway);
  return holder->value;
}

// Destroys the value contained by an opaque handle, without returning it. This is equivalent
// to calling unwrapOpaque<T>() and dropping the result, except that if the handle is the wrong
// type, this function silently does nothing rather than throw.
template <typename T>
void dropOpaque(v8::Isolate* isolate, v8::Local<v8::Value> handle) {
  static_assert(!kj::isReference<T>());
  static_assert(!isV8Ref<T>());

  KJ_IF_SOME(wrappable, Wrappable::tryUnwrapOpaque(isolate, handle)) {
    OpaqueWrappable<T>* holder = dynamic_cast<OpaqueWrappable<T>*>(&wrappable);
    if (holder != nullptr) {
      holder->movedAway = true;
      auto drop KJ_UNUSED = kj::mv(holder->value);
    }
  }
}

// =======================================================================================
// Promise implementation

// This type (opaque-wrapped) is the type of the "data" for a continuation callback. We have both
// the success and error callbacks share the same "data" object so that both underlying C++
// callbacks are proactively destroyed after one of the runs. Otherwise, we'd only destroy the
// function that was called, while the other one would have to wait for GC, which may mean
// keeping around C++ resources longer than necessary.
template <typename ThenFunc, typename CatchFunc>
struct ThenCatchPair {
  ThenFunc thenFunc;
  CatchFunc catchFunc;
};

// FunctionCallback implementing a C++ .then() continuation on a JS promise.
//
// We expect the input is already an opaque-wrapped value, args.Data() is an opaque-wrapped C++
// function to execute, and we want to produce an opaque-wrapped output or Promise.
template <typename FuncPairType, bool isCatch, typename Input, typename Output>
void promiseContinuation(const v8::FunctionCallbackInfo<v8::Value>& args) {
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
      return v8::Local<v8::Value>(callFunc().consumeHandle(Lock::from(isolate)));
    } else if constexpr (isV8Ref<Output>()) {
      return callFunc().getHandle(isolate);
    } else {
      return wrapOpaque(isolate->GetCurrentContext(), callFunc());
    }
  });
}

// Promise continuation that propagates the value or exception unmodified, but makes sure to
// proactively destroy the ThenCatchPair.
template <typename FuncPairType, bool isCatch>
void identityPromiseContinuation(const v8::FunctionCallbackInfo<v8::Value>& args) {
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
      : v8Promise(V8Ref<v8::Promise>(isolate, v8Promise)) {}

  Promise(decltype(nullptr)): v8Promise(kj::none) {}
  // For use when you're declaring a local variable that will be initialized later.

  void markAsHandled(Lock& js) {
    auto promise = getInner(js);
    promise->MarkAsHandled();
    markedAsHandled = true;
  }

  // Attach a continuation function and error handler to be called when this promise
  // is fulfilled. It is important to remember that then(...) can synchronously throw
  // a JavaScript exception (and jsg::JsExceptionThrown) in certain cases.
  template <typename Func, typename ErrorFunc>
  PromiseForResult<Func, T, true> then(Lock& js, Func&& func, ErrorFunc&& errorFunc) {
    typedef ReturnType<Func, T, true> Output;
    static_assert(kj::isSameType<Output, ReturnType<ErrorFunc, Value, true>>(),
        "functions passed to .then() must return exactly the same type");

    typedef ThenCatchPair<Func, ErrorFunc> FuncPair;
    return thenImpl<Output>(js, FuncPair{kj::fwd<Func>(func), kj::fwd<ErrorFunc>(errorFunc)},
        &promiseContinuation<FuncPair, false, T, Output>,
        &promiseContinuation<FuncPair, true, Value, Output>);
  }

  // Attach a continuation function to be called when this promise is fulfilled.
  // It is important to remember that then(...) can synchronously throw
  // a JavaScript exception (and jsg::JsExceptionThrown) in certain cases.
  template <typename Func>
  PromiseForResult<Func, T, true> then(Lock& js, Func&& func) {
    typedef ReturnType<Func, T, true> Output;

    // HACK: The error function is never called, so it need not actually be a functor.
    typedef ThenCatchPair<Func, bool> FuncPair;
    return thenImpl<Output>(js, FuncPair{kj::fwd<Func>(func), false},
        &promiseContinuation<FuncPair, false, T, Output>,
        &identityPromiseContinuation<FuncPair, true>);
  }

  template <typename ErrorFunc>
  Promise<T> catch_(Lock& js, ErrorFunc&& errorFunc) {
    static_assert(kj::isSameType<T, ReturnType<ErrorFunc, Value, true>>(),
        "function passed to .catch_() must return exactly the promise's type");

    // HACK: The non-error function is never called, so it need not actually be a functor.
    typedef ThenCatchPair<bool, ErrorFunc> FuncPair;
    return thenImpl<T>(js, FuncPair{false, kj::fwd<ErrorFunc>(errorFunc)},
        &identityPromiseContinuation<FuncPair, false>,
        &promiseContinuation<FuncPair, true, Value, T>);
  }

  // whenResolved returns a new Promise<void> that resolves when this promise resolves,
  // stopping the propagation of the resolved value. Unlike then(), calling whenResolved()
  // does not consume the promise, and whenResolved() can be called multiple times,
  // with each call creating a new branch off the original promise. Another key difference
  // with whenResolved() is that the markAsHandled status will propagate to the new Promise<void>
  // returned by whenResolved().
  Promise<void> whenResolved(Lock& js) {
    auto promise = Promise<void>(js.v8Isolate, getInner(js));
    if (markedAsHandled) {
      promise.markAsHandled(js);
    }
    return kj::mv(promise);
  }

  v8::Local<v8::Promise> consumeHandle(Lock& js) {
    auto result = getInner(js);
    v8Promise = kj::none;
    return result;
  }

  // If the promise is resolved, return the result, consuming the Promise. If it is pending
  // or rejected, returns null. This can be used as an optimization or in tests, but you must
  // never rely on it for correctness.
  kj::Maybe<T> tryConsumeResolved(Lock& js) {
    return js.withinHandleScope([&]() -> kj::Maybe<T> {
      auto handle =
          KJ_REQUIRE_NONNULL(v8Promise, "jsg::Promise can only be used once").getHandle(js);
      switch (handle->State()) {
        case v8::Promise::kPending:
        case v8::Promise::kRejected:
          return kj::none;
        case v8::Promise::kFulfilled:
          v8Promise = kj::none;
          return unwrapOpaque<T>(js.v8Isolate, handle->Result());
      }
    });
  }

  class Resolver {
  public:
    Resolver(v8::Isolate* isolate, v8::Local<v8::Promise::Resolver> v8Resolver)
        : v8Resolver(isolate, kj::mv(v8Resolver)) {}

    template <typename U = T, typename = kj::EnableIf<!isVoid<U>()>>
    void resolve(Lock& js, kj::NoInfer<U>&& value) {
      js.withinHandleScope([&] {
        auto context = js.v8Context();
        v8::Local<v8::Value> handle;
        if constexpr (isV8Ref<U>()) {
          handle = value.getHandle(js);
        } else {
          handle = wrapOpaque(context, kj::mv(value));
        }
        check(v8Resolver.getHandle(js)->Resolve(context, handle));
      });
    }

    template <typename U = T, typename = kj::EnableIf<isVoid<U>()>>
    void resolve(Lock& js) {
      js.withinHandleScope(
          [&] { check(v8Resolver.getHandle(js)->Resolve(js.v8Context(), js.v8Undefined())); });
    }

    void resolve(Lock& js, Promise&& promise) {
      // Resolve to another Promise.
      check(v8Resolver.getHandle(js)->Resolve(js.v8Context(), promise.consumeHandle(js)));
    }

    void reject(Lock& js, v8::Local<v8::Value> exception) {
      js.withinHandleScope(
          [&] { check(v8Resolver.getHandle(js)->Reject(js.v8Context(), exception)); });
    }

    void reject(Lock& js, kj::Exception exception) {
      reject(js, makeInternalError(js.v8Isolate, kj::mv(exception)));
    }

    Resolver addRef(Lock& js) {
      return {js.v8Isolate, v8Resolver.getHandle(js)};
    }
    void visitForGc(GcVisitor& visitor) {
      visitor.visit(v8Resolver);
    }

    JSG_MEMORY_INFO(Resolver) {
      tracker.trackField("resolver", v8Resolver);
    }

  private:
    V8Ref<v8::Promise::Resolver> v8Resolver;
    friend class MemoryTracker;
  };

  void visitForGc(GcVisitor& visitor) {
    visitor.visit(v8Promise);
  }

  JSG_MEMORY_INFO(Promise) {
    KJ_IF_SOME(promise, v8Promise) {
      tracker.trackField("promise", promise);
    }
  }

private:
  kj::Maybe<V8Ref<v8::Promise>> v8Promise;
  bool markedAsHandled = false;

  v8::Local<v8::Promise> getInner(Lock& js) {
    return KJ_REQUIRE_NONNULL(v8Promise, "jsg::Promise can only be used once").getHandle(js);
  }

  template <typename U = T, typename = kj::EnableIf<!isVoid<U>()>()>
  Promise(Lock& js, kj::NoInfer<U>&& value) {
    js.withinHandleScope([&] {
      auto context = js.v8Context();
      auto resolver = check(v8::Promise::Resolver::New(context));
      v8::Local<v8::Value> handle;
      if constexpr (isV8Ref<U>()) {
        handle = value.getHandle(js);
      } else {
        handle = wrapOpaque(context, kj::mv(value));
      };
      check(resolver->Resolve(context, handle));
      v8Promise.emplace(js.v8Isolate, resolver->GetPromise());
    });
  }

  template <typename U = T, typename = kj::EnableIf<isVoid<U>()>()>
  explicit Promise(Lock& js) {
    js.withinHandleScope([&] {
      auto context = js.v8Context();
      auto resolver = check(v8::Promise::Resolver::New(context));
      check(resolver->Resolve(context, js.v8Undefined()));
      v8Promise.emplace(js.v8Isolate, resolver->GetPromise());
    });
  }

  template <typename Result, typename FuncPair>
  Promise<RemovePromise<Result>> thenImpl(Lock& js,
      FuncPair&& funcPair,
      v8::FunctionCallback thenCallback,
      v8::FunctionCallback errCallback) {
    return js.withinHandleScope([&] {
      auto context = js.v8Context();

      auto funcPairHandle = wrapOpaque(context, kj::mv(funcPair));

      auto then = check(v8::Function::New(
          context, thenCallback, funcPairHandle, 1, v8::ConstructorBehavior::kThrow));

      auto errThen = check(v8::Function::New(
          context, errCallback, funcPairHandle, 1, v8::ConstructorBehavior::kThrow));

      using Type = RemovePromise<Result>;

      return Promise<Type>(js.v8Isolate, check(consumeHandle(js)->Then(context, then, errThen)));
    });
  }

  friend class Lock;
  template <typename TypeWrapper>
  friend class PromiseWrapper;
  friend class MemoryTracker;
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

  JSG_MEMORY_INFO(PromiseResolverPair) {
    tracker.trackField("promise", promise);
    tracker.trackField("resolver", resolver);
  }
};

template <typename T>
PromiseResolverPair<T> Lock::newPromiseAndResolver() {
  return withinHandleScope([&]() -> PromiseResolverPair<T> {
    auto resolver = check(v8::Promise::Resolver::New(v8Context()));
    auto promise = resolver->GetPromise();
    return {{v8Isolate, promise}, {v8Isolate, resolver}};
  });
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
  auto [promise, resolver] = newPromiseAndResolver<T>();
  resolver.reject(*this, exception);
  return kj::mv(promise);
}

template <typename T>
Promise<T> Lock::rejectedPromise(jsg::Value exception) {
  return withinHandleScope([&] { return rejectedPromise<T>(exception.getHandle(*this)); });
}

template <typename T>
Promise<T> Lock::rejectedPromise(kj::Exception&& exception) {
  return withinHandleScope(
      [&] { return rejectedPromise<T>(makeInternalError(v8Isolate, kj::mv(exception))); });
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
      // Probably TerminateExecution() called.
      tryCatch.ReThrow();
      throw;
    }
  } catch (kj::Exception& e) {
    return rejectedPromise<Result>(kj::mv(e));
  } catch (std::exception& exception) {
    return rejectedPromise<Result>(makeInternalError(v8Isolate, exception.what()));
  } catch (...) {
    return rejectedPromise<Result>(makeInternalError(
        v8Isolate, kj::str("caught unknown exception of type: ", kj::getCaughtExceptionType())));
  }
}

// -----------------------------------------------------------------------------

// Continuation function that converts a promised C++ value into a JavaScript value.
template <typename TypeWrapper, typename Input>
void thenWrap(const v8::FunctionCallbackInfo<v8::Value>& args) {
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
      return wrapper.wrap(context, kj::none, unwrapOpaque<Input>(isolate, args[0]));
    });
  }
}

// Continuation function that converts a promised JavaScript value into a C++ value.
template <typename TypeWrapper, typename Output>
void thenUnwrap(const v8::FunctionCallbackInfo<v8::Value>& args) {
  liftKj(args, [&]() {
    v8::Isolate* isolate = args.GetIsolate();
    auto& wrapper = TypeWrapper::from(isolate);
    auto context = isolate->GetCurrentContext();
    return wrapOpaque(context,
        wrapper.template unwrap<Output>(context, args[0], TypeErrorContext::promiseResolution()));
  });
}

// TypeWrapper mixin for Promise.
template <typename TypeWrapper>
class PromiseWrapper {
public:
  // The constructor here is a bit of a hack. The config is optional and might not be a JsgConfig
  // object (or convertible to a JsgConfig) if is provided. However, because of the way TypeWrapper
  // inherits PromiseWrapper, we always end up passing a config option (which might be
  // std::nullptr_t). The getConfig allows us to handle any case using reasonable defaults.
  PromiseWrapper(const auto& config): config(getConfig(config)) {}

  template <typename MetaConfiguration>
  void updateConfiguration(MetaConfiguration&& configuration) {
    config = getConfig(kj::fwd<MetaConfiguration>(configuration));
  }

  template <typename T>
  static constexpr const char* getName(Promise<T>*) {
    return "Promise";
  }

  template <typename T>
  v8::Local<v8::Promise> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      Promise<T>&& promise) {
    // Add a .then() to unwrap the value (i.e. convert C++ value to JavaScript).
    //
    // We use `creator` as the `data` value for this continuation so that the creator object
    // cannot be GC'd while the callback still exists. This gives us the KJ-style guarantee that
    // the object whose method returned the promise will not be destroyed while the promise is
    // still executing.
    auto markedAsHandled = promise.markedAsHandled;
    auto then = check(v8::Function::New(context, &thenWrap<TypeWrapper, T>, creator.orDefault({}),
        1, v8::ConstructorBehavior::kThrow));

    auto& js = jsg::Lock::from(context->GetIsolate());
    auto ret = check(promise.consumeHandle(js)->Then(context, then));
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
  kj::Maybe<Promise<T>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Promise<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
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
        auto then = check(v8::Function::New(
            context, &thenUnwrap<TypeWrapper, T>, {}, 1, v8::ConstructorBehavior::kThrow));
        promise = check(promise->Then(context, then));
      }
      return Promise<T>(context->GetIsolate(), promise);
    } else {
      // Input is a resolved value (not a promise). Try to unwrap it now.

      // If the input is an object that is not a promise, there's a chance it is a custom
      // thenable (and object with a then method intended to be used as a promise). If that
      // is the case, then we can handle the thenable by resolving it to a promise then
      // unwrapping that promise.
      // Unfortunately this needs to be gated by a compatibility flag because there are
      // existing workers that appear to rely on the old behavior -- although it's not clear
      // if those workers actually work the way they were intended to.
      if (config.unwrapCustomThenables && isThenable(context, handle)) {
        auto paf = check(v8::Promise::Resolver::New(context));
        check(paf->Resolve(context, handle));
        return tryUnwrap(context, paf->GetPromise(), (Promise<T>*)nullptr, parentObject);
      }

      auto& js = Lock::from(context->GetIsolate());
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
        // optionally return Promise<void> -- it seems like the callback isn't actually intending
        // to return the result of `someExpression()` but does so by accident since the braces
        // are missing. This is probably common in user code, too.
        return js.resolvedPromise();
      } else {
        auto& wrapper = *static_cast<TypeWrapper*>(this);
        KJ_IF_SOME(value, wrapper.tryUnwrap(context, handle, (T*)nullptr, parentObject)) {
          return js.resolvedPromise(kj::mv(value));
        } else {
          // Wrong type.
          return kj::none;
        }
      }
    }
  }

private:
  JsgConfig config;

  static bool isThenable(v8::Local<v8::Context> context, v8::Local<v8::Value> handle) {
    if (handle->IsObject()) {
      auto obj = handle.As<v8::Object>();
      return check(obj->Has(context, v8StrIntern(context->GetIsolate(), "then")));
    }
    return false;
  }
};

// -----------------------------------------------------------------------------

// A utility used internally by ServiceWorkerGlobalScope to perform the book keeping
// for unhandled promise rejection notifications. The handler maintains a table of
// weak references to rejected promises that have not been handled and will handle
// emitting events and console warnings as appropriate.
class UnhandledRejectionHandler {
public:
  using Handler = void(jsg::Lock& js,
      v8::PromiseRejectEvent event,
      jsg::V8Ref<v8::Promise> promise,
      jsg::Value value);

  explicit UnhandledRejectionHandler(kj::Function<Handler> handler): handler(kj::mv(handler)) {}

  void report(jsg::Lock& js,
      v8::PromiseRejectEvent event,
      jsg::V8Ref<v8::Promise> promise,
      jsg::Value value);

  void clear();

  JSG_MEMORY_INFO(UnhandledRejectionHandler) {
    // TODO (soon): Can we reasonably measure the function handler?
    tracker.trackField("unhandledRejections", unhandledRejections);
    tracker.trackField("warnedRejections", warnedRejections);
  }

private:
  // Used as part of the book keeping for unhandled rejections. When an
  // unhandled rejection occurs, the unhandledRejections Table will be updated.
  // If the rejection is later handled asynchronously, then the item will be
  // removed from the table. When the unhandled rejection table is processed
  // later in the event loop tick, any remaining rejections will generate a
  // warning to the inspector console (if enabled);
  struct UnhandledRejection {
    explicit UnhandledRejection(jsg::Lock& js,
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

    inline bool isAlive() {
      return !promise.IsEmpty() && !value.IsEmpty();
    }

    size_t rejectionNumber;

    uint hashCode() const {
      return hash;
    }

    JSG_MEMORY_INFO(UnhandledRejection) {
      tracker.trackField("promise", promise);
      tracker.trackField("value", value);
      visitForMemoryInfo(tracker);
    }
    void visitForMemoryInfo(MemoryTracker& tracker) const;
  };

  // A v8::Promise with memoized hash code.
  struct HashedPromise {
    v8::Local<v8::Promise> promise;
    uint hash;

    HashedPromise(v8::Local<v8::Promise> promise)
        : promise(promise),
          hash(kj::hashCode(promise->GetIdentityHash())) {}

    JSG_MEMORY_INFO(HashedPromise) {
      tracker.trackField("promise", promise);
    }
  };

  struct UnhandledRejectionCallbacks {
    inline const UnhandledRejection& keyForRow(const UnhandledRejection& row) const {
      return row;
    }
    inline bool matches(const UnhandledRejection& a, const UnhandledRejection& b) const {
      return a.promise == b.promise;
    }
    inline bool matches(const UnhandledRejection& a, const HashedPromise& b) const {
      return a.promise == b.promise;
    }
    inline uint hashCode(const UnhandledRejection& row) const {
      return row.hashCode();
    }
    inline uint hashCode(const HashedPromise& key) const {
      return key.hash;
    }
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
