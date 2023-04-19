// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// Handles wrapping a C++ function so that it can be called from JavaScript, and vice versa.

#include "wrappable.h"
#include "resource.h"  // for ArgumentIndexes
#include <kj/function.h>

namespace workerd::jsg {

template <typename Signature> class WrappableFunction;

template <typename Ret, typename...Args>
class WrappableFunction<Ret(Args...)>: public Wrappable {
public:
  WrappableFunction(bool needsGcTracing): needsGcTracing(needsGcTracing) {}
  virtual Ret operator()(Lock& js, Args&&... args) = 0;

  const bool needsGcTracing;
};

template <typename Signature, typename Impl, bool = hasPublicVisitForGc<Impl>()>
class WrappableFunctionImpl;

template <typename Ret, typename... Args, typename Impl>
class WrappableFunctionImpl<Ret(Args...), Impl, false>: public WrappableFunction<Ret(Args...)> {
public:
  WrappableFunctionImpl(Impl&& func)
      : WrappableFunction<Ret(Args...)>(false),
        func(kj::fwd<Impl>(func)) {}

  Ret operator()(Lock& js, Args&&... args) override {
    return func(js, kj::fwd<Args>(args)...);
  }

private:
  Impl func;
};

template <typename Ret, typename... Args, typename Impl>
class WrappableFunctionImpl<Ret(Args...), Impl, true>: public WrappableFunction<Ret(Args...)> {
public:
  WrappableFunctionImpl(Impl&& func)
      : WrappableFunction<Ret(Args...)>(true),
        func(kj::fwd<Impl>(func)) {}

  Ret operator()(Lock& js, Args&&... args) override {
    return func(js, kj::fwd<Args>(args)...);
  }
  void jsgVisitForGc(GcVisitor& visitor) override {
    visitor.visit(func);
  }

private:
  Impl func;
};

template <typename TypeWrapper, typename Signature, typename = ArgumentIndexes<Signature>>
struct FunctorCallback;

template <typename TypeWrapper, typename Ret, typename... Args, size_t... indexes>
struct FunctorCallback<TypeWrapper, Ret(Args...), kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& func = extractInternalPointer<WrappableFunction<Ret(Args...)>, false>(
          context, args.Data().As<v8::Object>());

      if constexpr (isVoid<Ret>()) {
        func(Lock::from(isolate), wrapper.template unwrap<Args>(context, args, indexes,
            TypeErrorContext::callbackArgument(indexes))...);
      } else {
        return wrapper.wrap(context, args.This(), func(Lock::from(isolate),
            wrapper.template unwrap<Args>(context, args, indexes,
                TypeErrorContext::callbackArgument(indexes))...));
      }
    });
  }
};

template <typename TypeWrapper, typename Ret, typename... Args, size_t... indexes>
struct FunctorCallback<TypeWrapper,
                       Ret(const v8::FunctionCallbackInfo<v8::Value>&, Args...),
                       kj::_::Indexes<indexes...>> {
  // Specialization for functions that take `const v8::FunctionCallbackInfo<v8::Value>&` as their
  // second parameter (after Lock&).

  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& func = extractInternalPointer<
          WrappableFunction<Ret(const v8::FunctionCallbackInfo<v8::Value>&, Args...)>, false>(
              context, args.Data().As<v8::Object>());

      if constexpr (isVoid<Ret>()) {
        func(Lock::from(isolate), args, wrapper.template unwrap<Args>(context, args, indexes,
            TypeErrorContext::callbackArgument(indexes))...);
      } else {
        return wrapper.wrap(context, args.This(), func(Lock::from(isolate), args,
            wrapper.template unwrap<Args>(context, args, indexes,
                TypeErrorContext::callbackArgument(indexes))...));
      }
    });
  }
};

template <typename Ret, typename...Args>
class Function<Ret(Args...)> {
  // (Docs are in jsg.h, the public header.)

public:
  using NativeFunction = jsg::WrappableFunction<Ret(Args...)>;

  typedef Ret Wrapper(
      jsg::Lock& js,
      v8::Local<v8::Object> receiver,  // the `this` value in the function
      v8::Local<v8::Function> fn,
      Args...);
  // When holding a JavaScript function, `Wrapper` is a C++ function that will handle converting
  // C++ arguments into JavaScript values and then call the JS function.

  Function(Wrapper* wrapper, V8Ref<v8::Object> receiver, V8Ref<v8::Function> function)
      : impl(JsImpl {
          .wrapper = kj::mv(wrapper),
          .receiver = kj::mv(receiver),
          .handle = kj::mv(function)
        }) {}
  // Construct jsg::Function wrapping a JavaScript function.

  template <typename Func, typename =
      decltype(kj::instance<Func>()(kj::instance<Lock&>(), kj::instance<Args>()...))>
  Function(Func&& func)
      : impl(Ref<NativeFunction>(
          alloc<WrappableFunctionImpl<Ret(Args...), Func>>(kj::fwd<Func>(func)))) {}
  // Construct jsg::Function wrapping a C++ function. The parameter can be a lambda or anything
  // else with operator() with a compatible signature. If the parameter has a visitForGc(GcVisitor&)
  // method, then GC visitation will be arranged.

  Function(Function&&) = default;
  Function& operator=(Function&&) = default;
  KJ_DISALLOW_COPY(Function);

  Ret operator()(jsg::Lock& jsl, Args... args) {
    KJ_SWITCH_ONEOF(impl) {
      KJ_CASE_ONEOF(native, Ref<NativeFunction>) {
        return (*native)(jsl, kj::fwd<Args>(args)...);
      }
      KJ_CASE_ONEOF(js, JsImpl) {
        return (*js.wrapper)(jsl, js.receiver.getHandle(jsl.v8Isolate),
                             js.handle.getHandle(jsl.v8Isolate), kj::fwd<Args>(args)...);
      }
    }
    __builtin_unreachable();
  }

  template <typename MakeNativeWrapperFunc>
  v8::Local<v8::Function> getOrCreateHandle(v8::Isolate* isolate,
      MakeNativeWrapperFunc&& makeNativeWrapper) {
    // Get a handle to the underlying function. If this is a native funciton,
    // `makeNativeWrapper(Ref<Func>&)` is called to create the wrapper.
    //
    // Only the `FunctionWrapper` TypeWrapper mixin should call this. Anyone else needs to call
    // `tryGetHandle()`.

    KJ_SWITCH_ONEOF(impl) {
      KJ_CASE_ONEOF(native, Ref<NativeFunction>) {
        return makeNativeWrapper(native);
      }
      KJ_CASE_ONEOF(js, JsImpl) {
        return js.handle.getHandle(isolate);
      }
    }
    __builtin_unreachable();
  }

  kj::Maybe<v8::Local<v8::Function>> tryGetHandle(v8::Isolate* isolate) {
    // Like getHandle() but if there's no wrapper yet, returns null.

    KJ_SWITCH_ONEOF(impl) {
      KJ_CASE_ONEOF(native, Ref<NativeFunction>) {
        return nullptr;
      }
      KJ_CASE_ONEOF(js, JsImpl) {
        return js.handle.getHandle(isolate);
      }
    }
    __builtin_unreachable();
  }

  inline void visitForGc(GcVisitor& visitor) {
    KJ_SWITCH_ONEOF(impl) {
      KJ_CASE_ONEOF(native, Ref<NativeFunction>) {
        visitor.visit(native);
      }
      KJ_CASE_ONEOF(js, JsImpl) {
        visitor.visit(js.receiver, js.handle);
      }
    }
  }

  inline Function<Ret(Args...)> addRef(v8::Isolate* isolate) {
    KJ_SWITCH_ONEOF(impl) {
      KJ_CASE_ONEOF(native, Ref<NativeFunction>) {
        return Function<Ret(Args...)>(native.addRef());
      }
      KJ_CASE_ONEOF(js, JsImpl) {
        return Function<Ret(Args...)>(js.wrapper, js.receiver.addRef(isolate), js.handle.addRef(isolate));
      }
    }
    __builtin_unreachable();
  }

  inline Function<Ret(Args...)> addRef(Lock& js) {
    KJ_SWITCH_ONEOF(impl) {
      KJ_CASE_ONEOF(native, Ref<NativeFunction>) {
        return Function<Ret(Args...)>(native.addRef());
      }
      KJ_CASE_ONEOF(jsi, JsImpl) {
        return Function<Ret(Args...)>(jsi.wrapper, jsi.receiver.addRef(js), jsi.handle.addRef(js));
      }
    }
    __builtin_unreachable();
  }

private:
  Function(Ref<NativeFunction>&& func) : impl(kj::mv(func)) {}

  struct JsImpl {
    Wrapper* wrapper;
    V8Ref<v8::Object> receiver;
    V8Ref<v8::Function> handle;
  };

  kj::OneOf<Ref<NativeFunction>, JsImpl> impl;
};

template <typename T>
class Constructor: public jsg::Function<T> {
public:
  using jsg::Function<T>::Function;
};

template <typename Method>
struct MethodSignature_;
template <typename T, typename Ret, typename... Args>
struct MethodSignature_<Ret (T::*)(Lock&, Args...)> {
  using Type = Ret(Args...);
};
template <typename T, typename Ret, typename... Args>
struct MethodSignature_<Ret (T::*)(Lock&, Args...) const> {
  using Type = Ret(Args...);
};

template <typename Method>
using MethodSignature = typename MethodSignature_<Method>::Type;
// Extracts a function signature from a method type.

template <typename TypeWrapper>
class FunctionWrapper {
  // TypeWrapper mixin for functions / lambdas.

public:
  template <typename Func, typename = decltype(&kj::Decay<Func>::operator())>
  static constexpr const char* getName(Func*) { return "function"; }

  template <typename Func, typename Signature =
      MethodSignature<decltype(&kj::Decay<Func>::operator())>>
  v8::Local<v8::Function> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, Func&& func) {
    return wrap(context, creator, jsg::Function<Signature>(kj::mv(func)));
  }

  template <typename Signature>
  v8::Local<v8::Function> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator,
      Function<Signature>&& func) {
    v8::Isolate* isolate = context->GetIsolate();
    return func.getOrCreateHandle(isolate, [&](Ref<WrappableFunction<Signature>>& ref) {
      v8::Local<v8::Object> data;
      KJ_IF_MAYBE(h, ref->tryGetHandle(isolate)) {
        // Apparently, this function has been wrapped before and already has an opaque handle.
        // That's interesting. However, unfortunately, we don't have a handle to the v8::Function
        // that was created last time, so we can't return the same function instance. This is
        // arguably incorrect; what if the application added properties to it or somtehing?
        //
        // Unfortunately, it is exceedingly difficult for us to store the function handle for
        // reuse without introducing performance problems.
        // - Ideally, we'd use the v8::Function itself as the object's wrapper, rather than an
        //   "opaque" wrapper. However, this doesn't work, because we can't attach internal fields
        //   to it. v8::Function::New() does not let us specify an internal field count. We can
        //   specify internal fields if we create a FunctionTemplate and then create the Function
        //   from that, but a FunctionTemplate only instantiates one Function (per Context). We
        //   need a separate Function instance for object we want to wrap. So... this doesn't work.
        //   (Note that V8's heap tracing API deeply depends on wrapper objects having two internal
        //   fields, so using other schemes like v8::External doesn't help either.)
        // - Another approach might be to store the v8::Function on the WrappableFunction, once
        //   it's created. This is a cyclic reference, but we could rely on GC visitation to
        //   collect it. The problem is, cyclic references can only be collected by tracing, not
        //   by scavenging. Tracing runs much less often than scavenging. So we'd be forcingc every
        //   function object to live on the heap longer than otherwise necessary.
        //
        // In practice, it probably never matters that returning the same jsg::Function twice
        // produces exactly the same JavaScript handle. So... screw it.
        data = *h;
      } else {
        data = ref->attachOpaqueWrapper(context, ref->needsGcTracing);
      }

      // TODO(conform): Correctly set `length` on all functions. Probably doesn't need a compat flag
      //   but I'd like to do it as a separate commit which can be reverted. We also currently fail
      //   to set this on constructors and methods (see resource.h). Remember not to count
      //   injected parameters!
      return check(v8::Function::New(
          context, &FunctorCallback<TypeWrapper, Signature>::callback, data));
    });
  }

  template <typename Ret, typename... Args>
  kj::Maybe<Constructor<Ret(Args...)>> tryUnwrap(
        v8::Local<v8::Context> context, v8::Local<v8::Value> handle, Constructor<Ret(Args...)>*,
        kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (!handle->IsFunction()) {
      return nullptr;
    }

    auto isolate = context->GetIsolate();

    auto wrapperFn = [](Lock& js,
                        v8::Local<v8::Object> receiver,
                        v8::Local<v8::Function> func,
                        Args... args) -> Ret {
      auto isolate = js.v8Isolate;
      auto& typeWrapper = TypeWrapper::from(isolate);

      v8::HandleScope scope(isolate);
      auto context = js.v8Context();
      v8::Local<v8::Value> argv[sizeof...(Args)] {
        typeWrapper.wrap(context, nullptr, kj::fwd<Args>(args))...
      };

      v8::Local<v8::Object> result = check(func->NewInstance(context, sizeof...(Args), argv));
      return typeWrapper.template unwrap<Ret>(
          context, result, TypeErrorContext::callbackReturn());
    };

    return Constructor<Ret(Args...)>(
        wrapperFn,
        V8Ref(isolate, parentObject.orDefault(context->Global())),
        V8Ref(isolate, handle.As<v8::Function>()));
  }

  template <typename Ret, typename... Args>
  kj::Maybe<Function<Ret(Args...)>> tryUnwrap(
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Function<Ret(Args...)>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (!handle->IsFunction()) {
      return nullptr;
    }

    auto isolate = context->GetIsolate();

    auto wrapperFn = [](Lock& js,
                        v8::Local<v8::Object> receiver,
                        v8::Local<v8::Function> func,
                        Args... args) -> Ret {
      auto isolate = js.v8Isolate;
      auto& typeWrapper = TypeWrapper::from(isolate);

      v8::HandleScope scope(isolate);
      auto context = js.v8Context();
      v8::Local<v8::Value> argv[sizeof...(Args)] {
        typeWrapper.wrap(context, nullptr, kj::fwd<Args>(args))...
      };

      auto result = check(func->Call(context, receiver, sizeof...(Args), argv));
      if constexpr(!isVoid<Ret>()) {
        return typeWrapper.template unwrap<Ret>(
            context, result, TypeErrorContext::callbackReturn());
      } else {
        return;
      }
    };

    return Function<Ret(Args...)>(
        wrapperFn,
        V8Ref(isolate, parentObject.orDefault(context->Global())),
        V8Ref(isolate, handle.As<v8::Function>()));
  }
};

template <typename Func>
class VisitableLambda {
public:
  VisitableLambda(Func&& func): func(kj::fwd<Func>(func)) {}

  template <typename... Params>
  auto operator()(Params&&... params) {
    return func(kj::fwd<Params>(params)...);
  }

  void visitForGc(GcVisitor& visitor) {
    func(visitor);
  }

private:
  Func func;
};

template <typename... Params> constexpr bool isGcVisitor() { return false; }
template <> constexpr bool isGcVisitor<GcVisitor&>() { return true; }

#define JSG_VISITABLE_LAMBDA(CAPTURES, VISITS, ...) \
  ::workerd::jsg::VisitableLambda([JSG_EXPAND CAPTURES](auto&&... params) mutable { \
    if constexpr (::workerd::jsg::isGcVisitor<decltype(params)...>()) { \
      (params.visit VISITS, ...); \
    } else { \
      return ([&]__VA_ARGS__)(kj::fwd<decltype(params)>(params)...); \
    } \
  })

}  // namespace workerd::jsg
