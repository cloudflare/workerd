// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// A "resource type" (in KJ parlance) is the opposite of a "value type". In JSG, a resource is a
// C++ class that will be wrapped and exposed to JavaScript by reference, such that JavaScript code
// can call back to the class's methods. This differs from, say, a struct type, which will be deeply
// converted into a JS object when passed into JS.

#include <workerd/jsg/fast-api.h>
#include <workerd/jsg/memory.h>
#include <workerd/jsg/meta.h>
#include <workerd/jsg/modules.capnp.h>
// JSG has very entrenched include cycles
// NOLINTNEXTLINE(misc-header-include-cycle)
#include <workerd/jsg/ser.h>
#include <workerd/jsg/util.h>
#include <workerd/jsg/wrappable.h>

#include <v8-template.h>

#include <kj/debug.h>
#include <kj/map.h>
#include <kj/tuple.h>

#include <type_traits>
#include <typeindex>

namespace std {
inline auto KJ_HASHCODE(const std::type_index& idx) {
  // Make std::type_index (which points to std::type_info) usable as a kj::HashMap key.
  // TODO(cleanup): This probably shouldn't live here, but where should it live? KJ?
  return idx.hash_code();
}
}  // namespace std

namespace workerd::jsg {

const auto kIllegalInvocation =
    "Illegal invocation: function called with incorrect `this` reference. "
    "See https://developers.cloudflare.com/workers/observability/errors/#illegal-invocation-errors for details."_kj;

class Serializer;
class Deserializer;

// Return true if the type requires GC visitation, which we assume is the case if the type or any
// superclass (other than Object) declares a `visitForGc()` method.
template <typename T>
constexpr bool resourceNeedsGcTracing() {
  return &T::visitForGc != &Object::visitForGc;
}
template <>
constexpr bool resourceNeedsGcTracing<Object>() {
  return false;
}

// Call obj->visitForGc() if and only if T defines its own `visitForGc()` method -- do not call
// the parent class's `visitForGc()`.
template <typename T>
inline void visitSubclassForGc(T* obj, GcVisitor& visitor) {
  if constexpr (&T::visitForGc != &T::jsgSuper::visitForGc) {
    obj->visitForGc(visitor);
  }
}

void throwIfConstructorCalledAsFunction(
    const v8::FunctionCallbackInfo<v8::Value>& args, const std::type_info& type);

// The scheduleUnimplemented* variants will schedule an exception on the isolate
// but do not throw a JsExceptionThrown.

// Called to throw errors about calling unimplemented functionality. It's assumed these are called
// directly from the V8 trampoline without liftKj, so they don't throw JsExceptionThrown.
void scheduleUnimplementedConstructorError(
    const v8::FunctionCallbackInfo<v8::Value>& args, const std::type_info& type);

// Called to throw errors about calling unimplemented functionality. It's assumed these are called
// directly from the V8 trampoline without liftKj, so they don't throw JsExceptionThrown.
void scheduleUnimplementedMethodError(const v8::FunctionCallbackInfo<v8::Value>& args,
    const std::type_info& type,
    const char* methodName);

// Called to throw errors about calling unimplemented functionality. It's assumed these are called
// directly from the V8 trampoline without liftKj, so they don't throw JsExceptionThrown.
void scheduleUnimplementedPropertyError(
    v8::Isolate* isolate, const std::type_info& type, const char* propertyName);

template <typename TypeWrapper, typename T>
class ResourceWrapper;

// Implements the V8 callback function for calling the static `constructor()` method of the C++
// class.
template <typename TypeWrapper,
    typename T,
    typename = decltype(T::constructor),
    typename = ArgumentIndexes<decltype(T::constructor)>>
struct ConstructorCallback;

template <typename TypeWrapper, typename T, typename... Args, size_t... indexes>
struct ConstructorCallback<TypeWrapper, T, Ref<T>(Args...), kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();

      throwIfConstructorCalledAsFunction(args, typeid(T));

      auto context = isolate->GetCurrentContext();
      auto& js = Lock::from(isolate);
      auto obj = args.This();
      KJ_ASSERT(obj->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);

      auto& wrapper = TypeWrapper::from(isolate);

      Ref<T> ptr = T::constructor(wrapper.template unwrap<Args>(js, context, args, indexes,
          TypeErrorContext::constructorArgument(typeid(T), indexes))...);
      if constexpr (T::jsgHasReflection) {
        ptr->jsgInitReflection(wrapper);
      }
      ptr.attachWrapper(isolate, obj);
    });
  }
};

// Specialization for constructors that take `Lock&` as their first parameter.
template <typename TypeWrapper, typename T, typename... Args, size_t... indexes>
struct ConstructorCallback<TypeWrapper, T, Ref<T>(Lock&, Args...), kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();

      throwIfConstructorCalledAsFunction(args, typeid(T));

      auto context = isolate->GetCurrentContext();
      auto& js = Lock::from(isolate);
      auto obj = args.This();
      KJ_ASSERT(obj->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);

      auto& wrapper = TypeWrapper::from(isolate);

      Ref<T> ptr = T::constructor(Lock::from(isolate),
          wrapper.template unwrap<Args>(js, context, args, indexes,
              TypeErrorContext::constructorArgument(typeid(T), indexes))...);
      if constexpr (T::jsgHasReflection) {
        ptr->jsgInitReflection(wrapper);
      }
      ptr.attachWrapper(isolate, obj);
    });
  }
};

// Specialization for constructors that take `const v8::FunctionCallbackInfo<v8::Value>&` as
// their first parameter.
template <typename TypeWrapper, typename T, typename... Args, size_t... indexes>
struct ConstructorCallback<TypeWrapper,
    T,
    Ref<T>(const v8::FunctionCallbackInfo<v8::Value>&, Args...),
    kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();

      throwIfConstructorCalledAsFunction(args, typeid(T));

      auto context = isolate->GetCurrentContext();
      auto& js = Lock::from(isolate);
      auto obj = args.This();
      KJ_ASSERT(obj->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);

      auto& wrapper = TypeWrapper::from(isolate);

      Ref<T> ptr = T::constructor(args,
          wrapper.template unwrap<Args>(js, context, args, indexes,
              TypeErrorContext::constructorArgument(typeid(T), indexes))...);
      if constexpr (T::jsgHasReflection) {
        ptr->jsgInitReflection(wrapper);
      }
      ptr.attachWrapper(isolate, obj);
    });
  }
};

template <typename TypeWrapper, typename T, typename... Args, size_t... indexes>
struct ConstructorCallback<TypeWrapper, T, Unimplemented(Args...), kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    scheduleUnimplementedConstructorError(args, typeid(T));
  }
};

// Implements the V8 callback function for calling a method of the C++ class.
template <typename TypeWrapper,
    const char* methodName,
    bool isContext,
    typename T,
    typename Method,
    Method method,
    typename Indexes>
struct MethodCallback;

template <typename TypeWrapper,
    const char* methodName,
    bool isContext,
    typename T,
    typename U,
    typename Ret,
    typename... Args,
    Ret (U::*method)(Args...),
    size_t... indexes>
struct MethodCallback<TypeWrapper,
    methodName,
    isContext,
    T,
    Ret (U::*)(Args...),
    method,
    kj::_::Indexes<indexes...>> {

  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if constexpr (TypeWrapper::trackCallCounts) {
      callCounter.slow++;
    }

    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = args.This();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& lock = Lock::from(isolate);
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      if constexpr (isVoid<Ret>()) {
        (self.*method)(wrapper.template unwrap<Args>(lock, context, args, indexes,
            TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
      } else {
        return wrapper.wrap(lock, context, obj,
            (self.*method)(wrapper.template unwrap<Args>(lock, context, args, indexes,
                TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...));
      }
    });
  }

  template <typename ReturnType = Ret>
  static ReturnType fastCallback(v8::Local<v8::Object> receiver,
      FastApiJSGToV8<Args>::value... fastArgs,
      v8::FastApiCallbackOptions& options) {
    if constexpr (TypeWrapper::trackCallCounts) {
      callCounter.fast++;
    }
    auto isolate = options.isolate;
    v8::HandleScope handleScope(isolate);
    auto context = isolate->GetCurrentContext();
    auto& js = Lock::from(isolate);
    auto& self = extractInternalPointer<T, isContext>(context, receiver);
    auto& wrapper = TypeWrapper::from(isolate);

    return liftKj<Ret>(isolate, [&]() {
      return (self.*method)(wrapper.template unwrapFastApi<Args>(js, context, fastArgs,
          TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
    });
  }
};

// Specialization for methods that take `Lock&` as their first parameter.
template <typename TypeWrapper,
    const char* methodName,
    bool isContext,
    typename T,
    typename U,
    typename Ret,
    typename... Args,
    Ret (U::*method)(Lock&, Args...),
    size_t... indexes>
struct MethodCallback<TypeWrapper,
    methodName,
    isContext,
    T,
    Ret (U::*)(Lock&, Args...),
    method,
    kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if constexpr (TypeWrapper::trackCallCounts) {
      callCounter.slow++;
    }
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = args.This();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      auto& lock = Lock::from(isolate);
      if constexpr (isVoid<Ret>()) {
        (self.*method)(lock,
            wrapper.template unwrap<Args>(lock, context, args, indexes,
                TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
      } else {
        return wrapper.wrap(lock, context, obj,
            (self.*method)(lock,
                wrapper.template unwrap<Args>(lock, context, args, indexes,
                    TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...));
      }
    });
  }

  template <typename ReturnType = Ret>
  static ReturnType fastCallback(v8::Local<v8::Object> receiver,
      FastApiJSGToV8<Args>::value... fastArgs,
      v8::FastApiCallbackOptions& options) {
    if constexpr (TypeWrapper::trackCallCounts) {
      callCounter.fast++;
    }
    auto isolate = options.isolate;
    v8::HandleScope handleScope(isolate);
    auto context = isolate->GetCurrentContext();
    auto& self = extractInternalPointer<T, isContext>(context, receiver);
    auto& lock = Lock::from(isolate);
    auto& wrapper = TypeWrapper::from(isolate);

    return liftKj<Ret>(isolate, [&]() {
      return (self.*method)(lock,
          wrapper.template unwrapFastApi<Args>(lock, context, fastArgs,
              TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
    });
  }
};

// Specialization for methods that take `const v8::FunctionCallbackInfo<v8::Value>&` as their
// first parameter.
template <typename TypeWrapper,
    const char* methodName,
    bool isContext,
    typename T,
    typename U,
    typename Ret,
    typename... Args,
    Ret (U::*method)(const v8::FunctionCallbackInfo<v8::Value>&, Args...),
    size_t... indexes>
struct MethodCallback<TypeWrapper,
    methodName,
    isContext,
    T,
    Ret (U::*)(const v8::FunctionCallbackInfo<v8::Value>&, Args...),
    method,
    kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = args.This();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& lock = Lock::from(isolate);
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      if constexpr (isVoid<Ret>()) {
        (self.*method)(args,
            wrapper.template unwrap<Args>(lock, context, args, indexes,
                TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
      } else {
        return wrapper.wrap(lock, context, obj,
            (self.*method)(args,
                wrapper.template unwrap<Args>(lock, context, args, indexes,
                    TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...));
      }
    });
  }
};

template <typename TypeWrapper,
    const char* methodName,
    bool isContext,
    typename T,
    typename U,
    typename... Args,
    Unimplemented (U::*method)(Args...),
    size_t... indexes>
struct MethodCallback<TypeWrapper,
    methodName,
    isContext,
    T,
    Unimplemented (U::*)(Args...),
    method,
    kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    scheduleUnimplementedMethodError(args, typeid(T), methodName);
  }
};

// Implements the V8 callback function for calling a static method of the C++ class.
//
// This is separate from MethodCallback<> because we need to know the interface type, T, and it
// won't be deducible from Method when Method is a C++ static member function.
//
// In the explicit specializations of this template, we use TypeErrorContext::methodArgument() for
// generating error messages, rather than concoct a new error message format specifically for static
// methods. This matches Chrome's behavior.

template <typename TypeWrapper,
    const char* methodName,
    typename T,
    typename Method,
    Method* method,
    typename Indexes>
struct StaticMethodCallback;

template <typename TypeWrapper,
    const char* methodName,
    typename T,
    typename Ret,
    typename... Args,
    Ret (*method)(Args...),
    size_t... indexes>
struct StaticMethodCallback<TypeWrapper,
    methodName,
    T,
    Ret(Args...),
    method,
    kj::_::Indexes<indexes...>> {

  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if constexpr (TypeWrapper::trackCallCounts) {
      callCounter.slow++;
    }
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& lock = Lock::from(isolate);
      if constexpr (isVoid<Ret>()) {
        (*method)(wrapper.template unwrap<Args>(lock, context, args, indexes,
            TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
      } else {
        return wrapper.wrap(lock, context, kj::none,
            (*method)(wrapper.template unwrap<Args>(lock, context, args, indexes,
                TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...));
      }
    });
  }

  template <typename ReturnType = Ret>
  static ReturnType fastCallback(v8::Local<v8::Object> receiver,
      FastApiJSGToV8<Args>::value... fastArgs,
      v8::FastApiCallbackOptions& options) {
    if constexpr (TypeWrapper::trackCallCounts) {
      callCounter.fast++;
    }
    auto isolate = options.isolate;
    v8::HandleScope handleScope(isolate);
    auto context = isolate->GetCurrentContext();
    auto& lock = Lock::from(isolate);
    auto& wrapper = TypeWrapper::from(isolate);

    return liftKj<Ret>(isolate, [&]() {
      return (*method)(wrapper.template unwrapFastApi<Args>(lock, context, fastArgs,
          TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
    });
  }
};

// Specialization for methods that take `Lock&` as their first parameter.
template <typename TypeWrapper,
    const char* methodName,
    typename T,
    typename Ret,
    typename... Args,
    Ret (*method)(Lock&, Args...),
    size_t... indexes>
struct StaticMethodCallback<TypeWrapper,
    methodName,
    T,
    Ret(Lock&, Args...),
    method,
    kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if constexpr (TypeWrapper::trackCallCounts) {
      callCounter.slow++;
    }

    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& lock = Lock::from(isolate);
      if constexpr (isVoid<Ret>()) {
        (*method)(lock,
            wrapper.template unwrap<Args>(lock, context, args, indexes,
                TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
      } else {
        return wrapper.wrap(lock, context, kj::none,
            (*method)(lock,
                wrapper.template unwrap<Args>(lock, context, args, indexes,
                    TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...));
      }
    });
  }

  template <typename ReturnType = Ret>
  static ReturnType fastCallback(v8::Local<v8::Object> receiver,
      FastApiJSGToV8<Args>::value... fastArgs,
      v8::FastApiCallbackOptions& options) {
    if constexpr (TypeWrapper::trackCallCounts) {
      callCounter.fast++;
    }
    auto isolate = options.isolate;
    v8::HandleScope handleScope(isolate);
    auto context = isolate->GetCurrentContext();
    auto& lock = Lock::from(isolate);
    auto& wrapper = TypeWrapper::from(isolate);

    return liftKj<Ret>(isolate, [&]() {
      return (*method)(lock,
          wrapper.template unwrapFastApi<Args>(lock, context, fastArgs,
              TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
    });
  }
};

// Specialization for methods that take `const v8::FunctionCallbackInfo<v8::Value>&` as their
// first parameter.
template <typename TypeWrapper,
    const char* methodName,
    typename T,
    typename Ret,
    typename... Args,
    Ret (*method)(const v8::FunctionCallbackInfo<v8::Value>&, Args...),
    size_t... indexes>
struct StaticMethodCallback<TypeWrapper,
    methodName,
    T,
    Ret(const v8::FunctionCallbackInfo<v8::Value>&, Args...),
    method,
    kj::_::Indexes<indexes...>> {

  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto& lock = Lock::from(isolate);
      auto& wrapper = TypeWrapper::from(isolate);
      if constexpr (isVoid<Ret>()) {
        (*method)(args,
            wrapper.template unwrap<Args>(lock, context, args, indexes,
                TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
      } else {
        return wrapper.wrap(lock, context, kj::none,
            (*method)(args,
                wrapper.template unwrap<Args>(lock, context, args, indexes,
                    TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...));
      }
    });
  }
};

template <typename TypeWrapper,
    const char* methodName,
    typename T,
    typename... Args,
    Unimplemented (*method)(Args...),
    size_t... indexes>
struct StaticMethodCallback<TypeWrapper,
    methodName,
    T,
    Unimplemented(Args...),
    method,
    kj::_::Indexes<indexes...>> {

  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    scheduleUnimplementedMethodError(args, typeid(T), methodName);
  }
};

// Implements the V8 callback function for static property getters.
template <typename TypeWrapper,
    const char* propertyName,
    typename T,
    typename Getter,
    Getter* getter>
struct StaticPropertyCallback;

template <typename TypeWrapper, const char* propertyName, typename T, typename Ret, Ret (*getter)()>
struct StaticPropertyCallback<TypeWrapper, propertyName, T, Ret(), getter> {

  static void callback(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& lock = Lock::from(isolate);

      if constexpr (isVoid<Ret>()) {
        (*getter)();
      } else {
        return wrapper.wrap(lock, context, kj::none, (*getter)());
      }
    });
  }
};

// Specialization for static property getter that takes Lock& as first parameter
template <typename TypeWrapper,
    const char* propertyName,
    typename T,
    typename Ret,
    Ret (*getter)(Lock&)>
struct StaticPropertyCallback<TypeWrapper, propertyName, T, Ret(Lock&), getter> {

  static void callback(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& lock = Lock::from(isolate);

      if constexpr (isVoid<Ret>()) {
        (*getter)(lock);
      } else {
        return wrapper.wrap(lock, context, kj::none, (*getter)(lock));
      }
    });
  }
};

// Implements the V8 callback function for calling a property getter method of a C++ class.
template <typename TypeWrapper,
    const char* methodName,
    typename Method,
    Method method,
    bool isContext>
struct GetterCallback;

#define JSG_DEFINE_GETTER_CALLBACK_STRUCTS(...)                                                    \
  template <typename TypeWrapper, const char* methodName, typename T, typename Ret,                \
      typename... Args, Ret (T::*method)(Args...) __VA_ARGS__, bool isContext>                     \
  struct GetterCallback<TypeWrapper, methodName, Ret (T::*)(Args...) __VA_ARGS__, method,          \
      isContext> {                                                                                 \
    static constexpr bool enumerable = true;                                                       \
    static void callback(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {   \
      if constexpr (TypeWrapper::trackCallCounts) {                                                \
        callCounter.slow++;                                                                        \
      }                                                                                            \
      liftKj(info, [&]() {                                                                         \
        auto isolate = info.GetIsolate();                                                          \
        auto context = isolate->GetCurrentContext();                                               \
        auto obj = info.HolderV2();                                                                \
        auto& js = Lock::from(isolate);                                                            \
        auto& wrapper = TypeWrapper::from(isolate);                                                \
        /* V8 no longer supports AccessorSignature, so we must manually verify `this`'s type. */   \
        if (!isContext &&                                                                          \
            !wrapper.getTemplate(isolate, static_cast<T*>(nullptr))->HasInstance(obj)) {           \
          throwTypeError(isolate, kIllegalInvocation);                                             \
        }                                                                                          \
        auto& self = extractInternalPointer<T, isContext>(context, obj);                           \
        return wrapper.wrap(js, context, obj,                                                      \
            (self.*method)(                                                                        \
                wrapper.unwrap(js, context, static_cast<kj::Decay<Args>*>(nullptr))...));          \
      });                                                                                          \
    }                                                                                              \
    template <typename ReturnType = Ret>                                                           \
    static ReturnType fastCallback(                                                                \
        v8::Local<v8::Object> receiver, v8::FastApiCallbackOptions& options) {                     \
      if constexpr (TypeWrapper::trackCallCounts) {                                                \
        callCounter.fast++;                                                                        \
      }                                                                                            \
      auto isolate = options.isolate;                                                              \
      v8::HandleScope handleScope(isolate);                                                        \
      auto context = isolate->GetCurrentContext();                                                 \
      auto& lock = Lock::from(isolate);                                                            \
      auto& self = extractInternalPointer<T, isContext>(context, receiver);                        \
      auto& wrapper = TypeWrapper::from(isolate);                                                  \
      return liftKj<ReturnType>(isolate, [&]() {                                                   \
        return (self.*method)(wrapper.template unwrapFastApi<Args>(                                \
            lock, context, static_cast<kj::Decay<Args>*>(nullptr))...);                            \
      });                                                                                          \
    }                                                                                              \
  };                                                                                               \
                                                                                                   \
  /* Specialization for methods that take `Lock&` as their first parameter. */                     \
  template <typename TypeWrapper, const char* methodName, typename T, typename Ret,                \
      typename... Args, Ret (T::*method)(Lock&, Args...) __VA_ARGS__, bool isContext>              \
  struct GetterCallback<TypeWrapper, methodName, Ret (T::*)(Lock&, Args...) __VA_ARGS__, method,   \
      isContext> {                                                                                 \
    static constexpr bool enumerable = true;                                                       \
    static void callback(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {   \
      if constexpr (TypeWrapper::trackCallCounts) {                                                \
        callCounter.slow++;                                                                        \
      }                                                                                            \
      liftKj(info, [&]() {                                                                         \
        auto isolate = info.GetIsolate();                                                          \
        auto context = isolate->GetCurrentContext();                                               \
        auto& js = Lock::from(isolate);                                                            \
        auto obj = info.HolderV2();                                                                \
        auto& wrapper = TypeWrapper::from(isolate);                                                \
        /* V8 no longer supports AccessorSignature, so we must manually verify `this`'s type. */   \
        if (!isContext &&                                                                          \
            !wrapper.getTemplate(isolate, static_cast<T*>(nullptr))->HasInstance(obj)) {           \
          throwTypeError(isolate, kIllegalInvocation);                                             \
        }                                                                                          \
        auto& self = extractInternalPointer<T, isContext>(context, obj);                           \
        return wrapper.wrap(js, context, obj,                                                      \
            (self.*method)(                                                                        \
                js, wrapper.unwrap(js, context, static_cast<kj::Decay<Args>*>(nullptr))...));      \
      });                                                                                          \
    }                                                                                              \
    template <typename ReturnType = Ret>                                                           \
    static ReturnType fastCallback(                                                                \
        v8::Local<v8::Object> receiver, v8::FastApiCallbackOptions& options) {                     \
      if constexpr (TypeWrapper::trackCallCounts) {                                                \
        callCounter.fast++;                                                                        \
      }                                                                                            \
      auto isolate = options.isolate;                                                              \
      v8::HandleScope handleScope(isolate);                                                        \
      auto context = isolate->GetCurrentContext();                                                 \
      auto& js = Lock::from(isolate);                                                              \
      auto& self = extractInternalPointer<T, isContext>(context, receiver);                        \
      auto& wrapper = TypeWrapper::from(isolate);                                                  \
      return liftKj<ReturnType>(isolate, [&]() {                                                   \
        return (self.*method)(js,                                                                  \
            wrapper.template unwrapFastApi<Args>(                                                  \
                js, context, static_cast<kj::Decay<Args>*>(nullptr))...);                          \
      });                                                                                          \
    }                                                                                              \
  };                                                                                               \
                                                                                                   \
  template <typename TypeWrapper, const char* propertyName, typename T,                            \
      Unimplemented (T::*method)() __VA_ARGS__, bool isContext>                                    \
  struct GetterCallback<TypeWrapper, propertyName, Unimplemented (T::*)() __VA_ARGS__, method,     \
      isContext> {                                                                                 \
    static constexpr bool enumerable = false;                                                      \
    static void callback(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {   \
      scheduleUnimplementedPropertyError(info.GetIsolate(), typeid(T), propertyName);              \
    }                                                                                              \
    template <typename ReturnType = Unimplemented>                                                 \
    static ReturnType fastCallback(                                                                \
        v8::Local<v8::Object> receiver, v8::FastApiCallbackOptions& options) {                     \
      scheduleUnimplementedPropertyError(options.isolate, typeid(T), propertyName);                \
      if constexpr (!isVoid<ReturnType>()) {                                                       \
        return ReturnType{};                                                                       \
      }                                                                                            \
    };                                                                                             \
  };

JSG_DEFINE_GETTER_CALLBACK_STRUCTS()
JSG_DEFINE_GETTER_CALLBACK_STRUCTS(const)

#undef JSG_DEFINE_GETTER_CALLBACK_STRUCTS

template <typename TypeWrapper,
    const char* methodName,
    typename Method,
    Method method,
    bool isContext>
struct PropertyGetterCallback;

#define JSG_DEFINE_PROPERTY_GETTER_CALLBACK_STRUCTS(...)                                           \
  template <typename TypeWrapper, const char* methodName, typename T, typename Ret,                \
      typename... Args, Ret (T::*method)(Args...) __VA_ARGS__, bool isContext>                     \
  struct PropertyGetterCallback<TypeWrapper, methodName, Ret (T::*)(Args...) __VA_ARGS__, method,  \
      isContext> {                                                                                 \
    static constexpr bool enumerable = true;                                                       \
    static void callback(const v8::FunctionCallbackInfo<v8::Value>& info) {                        \
      if constexpr (TypeWrapper::trackCallCounts) {                                                \
        callCounter.slow++;                                                                        \
      }                                                                                            \
      liftKj(info, [&]() {                                                                         \
        auto isolate = info.GetIsolate();                                                          \
        auto context = isolate->GetCurrentContext();                                               \
        auto& js = Lock::from(isolate);                                                            \
        auto obj = info.This();                                                                    \
        auto& wrapper = TypeWrapper::from(isolate);                                                \
        /* V8 no longer supports AccessorSignature, so we must manually verify `this`'s type. */   \
        if (!isContext &&                                                                          \
            !wrapper.getTemplate(isolate, static_cast<T*>(nullptr))->HasInstance(obj)) {           \
          throwTypeError(isolate, kIllegalInvocation);                                             \
        }                                                                                          \
        auto& self = extractInternalPointer<T, isContext>(context, obj);                           \
        return wrapper.wrap(js, context, obj,                                                      \
            (self.*method)(                                                                        \
                wrapper.unwrap(js, context, static_cast<kj::Decay<Args>*>(nullptr))...));          \
      });                                                                                          \
    }                                                                                              \
    template <typename ReturnType = Ret>                                                           \
    static ReturnType fastCallback(                                                                \
        v8::Local<v8::Object> receiver, v8::FastApiCallbackOptions& options) {                     \
      if constexpr (TypeWrapper::trackCallCounts) {                                                \
        callCounter.fast++;                                                                        \
      }                                                                                            \
      auto isolate = options.isolate;                                                              \
      v8::HandleScope handleScope(isolate);                                                        \
      auto context = isolate->GetCurrentContext();                                                 \
      auto& self = extractInternalPointer<T, isContext>(context, receiver);                        \
      auto& wrapper = TypeWrapper::from(isolate);                                                  \
      auto& js = Lock::from(isolate);                                                              \
                                                                                                   \
      return liftKj<ReturnType>(isolate, [&]() -> ReturnType {                                     \
        return (self.*method)(                                                                     \
            wrapper.unwrap(js, context, static_cast<kj::Decay<Args>*>(nullptr))...);               \
      });                                                                                          \
    }                                                                                              \
  };                                                                                               \
                                                                                                   \
  /* Specialization for methods that take `Lock&` as their first parameter. */                     \
  template <typename TypeWrapper, const char* methodName, typename T, typename Ret,                \
      typename... Args, Ret (T::*method)(Lock&, Args...) __VA_ARGS__, bool isContext>              \
  struct PropertyGetterCallback<TypeWrapper, methodName, Ret (T::*)(Lock&, Args...) __VA_ARGS__,   \
      method, isContext> {                                                                         \
    static constexpr bool enumerable = true;                                                       \
    static void callback(const v8::FunctionCallbackInfo<v8::Value>& info) {                        \
      if constexpr (TypeWrapper::trackCallCounts) {                                                \
        callCounter.slow++;                                                                        \
      }                                                                                            \
      liftKj(info, [&]() {                                                                         \
        auto isolate = info.GetIsolate();                                                          \
        auto context = isolate->GetCurrentContext();                                               \
        auto& js = Lock::from(isolate);                                                            \
        auto obj = info.This();                                                                    \
        auto& wrapper = TypeWrapper::from(isolate);                                                \
        /* V8 no longer supports AccessorSignature, so we must manually verify `this`'s type. */   \
        if (!isContext &&                                                                          \
            !wrapper.getTemplate(isolate, static_cast<T*>(nullptr))->HasInstance(obj)) {           \
          throwTypeError(isolate, kIllegalInvocation);                                             \
        }                                                                                          \
        auto& self = extractInternalPointer<T, isContext>(context, obj);                           \
        return wrapper.wrap(js, context, obj,                                                      \
            (self.*method)(                                                                        \
                js, wrapper.unwrap(js, context, static_cast<kj::Decay<Args>*>(nullptr))...));      \
      });                                                                                          \
    }                                                                                              \
    template <typename ReturnType = Ret>                                                           \
    static ReturnType fastCallback(                                                                \
        v8::Local<v8::Object> receiver, v8::FastApiCallbackOptions& options) {                     \
      if constexpr (TypeWrapper::trackCallCounts) {                                                \
        callCounter.fast++;                                                                        \
      }                                                                                            \
      auto isolate = options.isolate;                                                              \
      v8::HandleScope handleScope(isolate);                                                        \
      auto context = isolate->GetCurrentContext();                                                 \
      auto& self = extractInternalPointer<T, isContext>(context, receiver);                        \
      auto& js = Lock::from(isolate);                                                              \
      auto& wrapper = TypeWrapper::from(isolate);                                                  \
                                                                                                   \
      return liftKj<ReturnType>(isolate, [&]() -> ReturnType {                                     \
        return (self.*method)(                                                                     \
            js, wrapper.unwrap(js, context, static_cast<kj::Decay<Args>*>(nullptr))...);           \
      });                                                                                          \
    }                                                                                              \
  };                                                                                               \
  template <typename TypeWrapper, const char* propertyName, typename T,                            \
      Unimplemented (T::*method)() __VA_ARGS__, bool isContext>                                    \
  struct PropertyGetterCallback<TypeWrapper, propertyName, Unimplemented (T::*)() __VA_ARGS__,     \
      method, isContext> {                                                                         \
    static constexpr bool enumerable = false;                                                      \
    static void callback(const v8::FunctionCallbackInfo<v8::Value>& info) {                        \
      scheduleUnimplementedPropertyError(info.GetIsolate(), typeid(T), propertyName);              \
    }                                                                                              \
    template <typename ReturnType = Unimplemented>                                                 \
    static ReturnType fastCallback(                                                                \
        v8::Local<v8::Object> receiver, v8::FastApiCallbackOptions& options) {                     \
      scheduleUnimplementedPropertyError(options.isolate, typeid(T), propertyName);                \
      if constexpr (!isVoid<ReturnType>()) {                                                       \
        return ReturnType{};                                                                       \
      }                                                                                            \
    }                                                                                              \
  };

JSG_DEFINE_PROPERTY_GETTER_CALLBACK_STRUCTS()
JSG_DEFINE_PROPERTY_GETTER_CALLBACK_STRUCTS(const)

// Implements the V8 callback function for calling a property setter method of a C++ class.
template <typename TypeWrapper,
    const char* methodName,
    typename Method,
    Method method,
    bool isContext>
struct SetterCallback;

template <typename TypeWrapper,
    const char* methodName,
    typename T,
    typename Arg,
    void (T::*method)(Arg),
    bool isContext>
struct SetterCallback<TypeWrapper, methodName, void (T::*)(Arg), method, isContext> {
  static void callback(
      v8::Local<v8::Name>, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
    liftKj(info, [&]() {
      auto isolate = info.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto& js = Lock::from(isolate);
      auto obj = info.HolderV2();
      auto& wrapper = TypeWrapper::from(isolate);
      // V8 no longer supports AccessorSignature, so we must manually verify `this`'s type.
      if (!isContext && !wrapper.getTemplate(isolate, static_cast<T*>(nullptr))->HasInstance(obj)) {
        throwTypeError(isolate, kIllegalInvocation);
      }
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      (self.*method)(wrapper.template unwrap<Arg>(
          js, context, value, TypeErrorContext::setterArgument(typeid(T), methodName)));
    });
  }
};

// Specialization for methods that take `Lock&` as their first parameter.
template <typename TypeWrapper,
    const char* methodName,
    typename T,
    typename Arg,
    void (T::*method)(Lock&, Arg),
    bool isContext>
struct SetterCallback<TypeWrapper, methodName, void (T::*)(Lock&, Arg), method, isContext> {
  static void callback(
      v8::Local<v8::Name>, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
    liftKj(info, [&]() {
      auto isolate = info.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = info.HolderV2();
      auto& wrapper = TypeWrapper::from(isolate);
      // V8 no longer supports AccessorSignature, so we must manually verify `this`'s type.
      if (!isContext && !wrapper.getTemplate(isolate, static_cast<T*>(nullptr))->HasInstance(obj)) {
        throwTypeError(isolate, kIllegalInvocation);
      }
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      auto& js = Lock::from(isolate);
      (self.*method)(js,
          wrapper.template unwrap<Arg>(
              js, context, value, TypeErrorContext::setterArgument(typeid(T), methodName)));
    });
  }
};

template <typename TypeWrapper,
    const char* methodName,
    typename Method,
    Method method,
    bool isContext>
struct PropertySetterCallback;

template <typename TypeWrapper,
    const char* methodName,
    typename T,
    typename Arg,
    void (T::*method)(Arg),
    bool isContext>
struct PropertySetterCallback<TypeWrapper, methodName, void (T::*)(Arg), method, isContext> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if constexpr (TypeWrapper::trackCallCounts) {
      callCounter.slow++;
    }
    liftKj(info, [&]() {
      auto isolate = info.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto& js = Lock::from(isolate);
      auto obj = info.This();
      auto& wrapper = TypeWrapper::from(isolate);
      // V8 no longer supports AccessorSignature, so we must manually verify `this`'s type.
      if (!isContext && !wrapper.getTemplate(isolate, static_cast<T*>(nullptr))->HasInstance(obj)) {
        throwTypeError(isolate, kIllegalInvocation);
      }
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      (self.*method)(wrapper.template unwrap<Arg>(
          js, context, info[0], TypeErrorContext::setterArgument(typeid(T), methodName)));
    });
  }

  template <typename ReturnType = void>
  static ReturnType fastCallback(v8::Local<v8::Object> receiver,
      FastApiJSGToV8<Arg>::value fastArgs,
      v8::FastApiCallbackOptions& options) {
    if constexpr (TypeWrapper::trackCallCounts) {
      callCounter.fast++;
    }
    auto isolate = options.isolate;
    v8::HandleScope handleScope(isolate);
    auto context = isolate->GetCurrentContext();
    auto& js = Lock::from(isolate);
    auto& self = extractInternalPointer<T, isContext>(context, receiver);
    auto& wrapper = TypeWrapper::from(isolate);

    liftKj<void>(isolate, [&]() -> void {
      (self.*method)(wrapper.template unwrapFastApi<Arg>(
          js, context, fastArgs, TypeErrorContext::setterArgument(typeid(T), methodName)));
    });
  }
};

// Specialization for methods that take `Lock&` as their first parameter.
template <typename TypeWrapper,
    const char* methodName,
    typename T,
    typename Arg,
    void (T::*method)(Lock&, Arg),
    bool isContext>
struct PropertySetterCallback<TypeWrapper, methodName, void (T::*)(Lock&, Arg), method, isContext> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if constexpr (TypeWrapper::trackCallCounts) {
      callCounter.slow++;
    }
    liftKj(info, [&]() {
      auto isolate = info.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = info.This();
      auto& wrapper = TypeWrapper::from(isolate);
      // V8 no longer supports AccessorSignature, so we must manually verify `this`'s type.
      if (!isContext && !wrapper.getTemplate(isolate, static_cast<T*>(nullptr))->HasInstance(obj)) {
        throwTypeError(isolate, kIllegalInvocation);
      }
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      auto& js = Lock::from(isolate);
      (self.*method)(js,
          wrapper.template unwrap<Arg>(
              js, context, info[0], TypeErrorContext::setterArgument(typeid(T), methodName)));
    });
  }

  template <typename ReturnType = void>
  static ReturnType fastCallback(v8::Local<v8::Object> receiver,
      FastApiJSGToV8<Arg>::value fastArgs,
      v8::FastApiCallbackOptions& options) {
    if constexpr (TypeWrapper::trackCallCounts) {
      callCounter.fast++;
    }
    auto isolate = options.isolate;
    v8::HandleScope handleScope(isolate);
    auto context = isolate->GetCurrentContext();
    auto& self = extractInternalPointer<T, isContext>(context, receiver);
    auto& lock = Lock::from(isolate);
    auto& wrapper = TypeWrapper::from(isolate);

    liftKj<void>(isolate, [&]() -> void {
      (self.*method)(lock,
          wrapper.template unwrapFastApi<Arg>(
              lock, context, fastArgs, TypeErrorContext::setterArgument(typeid(T), methodName)));
    });
  }
};

template <typename T>
class TypeHandler;

// Helper to call T::serialize() and pass along any TypeHandlers it needs.
template <typename TypeWrapper, typename T, typename Method = decltype(&T::serialize)>
struct SerializeInvoker;
template <typename TypeWrapper, typename T, typename... Types>
struct SerializeInvoker<TypeWrapper,
    T,
    void (T::*)(Lock&, Serializer&, const TypeHandler<Types>&...)> {
  static void call(TypeWrapper& wrapper, T& target, Lock& js, Serializer& serializer) {
    target.serialize(js, serializer, TypeWrapper::template TYPE_HANDLER_INSTANCE<Types>...);
  }
};

// Helper to call T::deserialize() and pass along any TypeHandlers it needs, as well as wrap
// the result.
template <typename TypeWrapper, typename T, typename Method = decltype(T::deserialize)>
struct DeserializeInvoker;
template <typename TypeWrapper, typename T, typename Ret, typename Tag, typename... Types>
struct DeserializeInvoker<TypeWrapper,
    T,
    Ret(Lock&, Tag, Deserializer&, const TypeHandler<Types>&...)> {
  static v8::Local<v8::Object> call(
      TypeWrapper& wrapper, Lock& js, Tag tag, Deserializer& deserializer) {
    return wrapper.wrap(js, js.v8Context(), kj::none,
        T::deserialize(
            js, tag, deserializer, TypeWrapper::template TYPE_HANDLER_INSTANCE<Types>...));
  }
};

// SFINAE helper to detect if a type has a static method called `constructor`.
// Includes a static_assert to provide a clear error if the method exists but is non-static.
template <typename T, typename = void>
constexpr bool HasConstructorMethod = false;

template <typename T>
constexpr bool HasConstructorMethod<T, std::void_t<decltype(&T::constructor)>> = [] {
  static_assert(!std::is_member_function_pointer_v<decltype(&T::constructor)>,
      "JSG_RESOURCE_TYPE constructor must be a static method.");
  return true;
}();

// Expose the global scope type as a nested type under the global scope itself, such that for some
// global scope type `GlobalScope`, `this.GlobalScope === this.constructor` holds true. Note that
// this does not actually allow the user to construct a global scope object (unless it has a static
// C++ constructor member function, of course). It is primarily to allow code like
// `this instanceof ServiceWorkerGlobalScope` to work correctly.
//
// This is implemented manually because the obvious way of doing it causes a crash:
//
//     struct GlobalScope {
//       JSG_RESOURCE_TYPE {
//         JSG_NESTED_TYPE(GlobalScope);  // BAD!
//       }
//     };
void exposeGlobalScopeType(v8::Isolate* isolate, v8::Local<v8::Context> context);

v8::Local<v8::Symbol> getSymbolDispose(v8::Isolate* isolate);

// A configuration type that can be derived from any input type, because it contains nothing.
class NullConfiguration {
 public:
  template <typename T>
  NullConfiguration(T&&) {}
};

// TypeWrapper must list this type as its first superclass. The ResourceWrappers that it
// subclasses will then be able to register themselves in the map.
template <typename TypeWrapper>
class DynamicResourceTypeMap {
 private:
  using ReflectionInitializer = void(jsg::Object& object, TypeWrapper& wrapper);
  struct DynamicTypeInfo {
    v8::Local<v8::FunctionTemplate> tmpl;
    kj::Maybe<ReflectionInitializer&> reflectionInitializer;
  };

  DynamicTypeInfo getDynamicTypeInfo(v8::Isolate* isolate, const std::type_info& type) {
    KJ_IF_SOME(f, resourceTypeMap.find(std::type_index(type))) {
      return (*f)(static_cast<TypeWrapper&>(*this), isolate);
    } else {
      KJ_FAIL_REQUIRE(
          "cannot wrap object type that was not registered with JSG_DECLARE_ISOLATE_TYPE",
          typeName(type));
    }
  }

  using GetTypeInfoFunc = DynamicTypeInfo(TypeWrapper&, v8::Isolate*);

  // Maps type_info values to functions that can be used to get the associated template. Used by
  // ResourceWrapper.
  //
  // Fun fact: Comparing type_index on Linux is a simple pointer comparison. On Windows it is a
  // string comparison (of the type names). On Mac arm64 it could be either, there's a special bit
  // in the type_info that indicates if it is known to be unique. See
  // _LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION in <typeinfo> for more.
  kj::HashMap<std::type_index, GetTypeInfoFunc*> resourceTypeMap;

  // Map types to serializers. Given an `Object`, to serialize it, extract its typeinfo and look
  // it up in this table. See jsg::Serializer for more about serialization.
  //
  // "Why not just use a virtual function?" Virtual functions give the wrong semantics for
  // serialization, since it's essential that we use the serializer for the most-derived class,
  // not for some parent class. If Foo extends Bar, and Bar is serializable, but Foo does not
  // declare a serializer, then Foo is *not* serializable. Bar's serializer is not sufficient,
  // since it doesn't know about Foo's extensions. But to get the right behavior from virtual
  // calls, Foo would have to explicitly override Bar's serialize method to make it throw an
  // exception instead. This seems error-prone.
  //
  // It also just provides nice symmetry with `deserializerMap`.
  //
  // The SerializeFunc() must always start by writing a tag.
  using SerializeFunc = void(TypeWrapper&, Lock& js, jsg::Object& instance, Serializer& serializer);
  kj::HashMap<std::type_index, SerializeFunc*> serializerMap;

  // Map tag numbers to deserializer functions.
  using DeserializeFunc = v8::Local<v8::Object>(
      TypeWrapper&, Lock& js, uint tag, Deserializer& deserializer);
  kj::HashMap<uint, DeserializeFunc*> deserializerMap;

  template <typename, typename>
  friend class ResourceWrapper;
  template <typename>
  friend class ObjectWrapper;
  template <typename>
  friend class Isolate;
};

// ======================================================================================
// WildcardProperty implementation

class JsValue;

template <typename TypeWrapper, typename T, typename GetNamedMethod, GetNamedMethod getNamedMethod>
struct WildcardPropertyCallbacks;

template <typename TypeWrapper,
    typename T,
    typename U,
    typename Ret,
    kj::Maybe<Ret> (U::*getNamedMethod)(jsg::Lock&, kj::String)>
struct WildcardPropertyCallbacks<TypeWrapper,
    T,
    kj::Maybe<Ret> (U::*)(jsg::Lock&, kj::String),
    getNamedMethod>: public v8::NamedPropertyHandlerConfiguration {
  WildcardPropertyCallbacks()
      : v8::NamedPropertyHandlerConfiguration(getter,
            nullptr,
            query,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            v8::Local<v8::Value>(),
            static_cast<v8::PropertyHandlerFlags>(
                static_cast<int>(v8::PropertyHandlerFlags::kNonMasking) |
                static_cast<int>(v8::PropertyHandlerFlags::kHasNoSideEffect) |
                static_cast<int>(v8::PropertyHandlerFlags::kOnlyInterceptStrings))) {}

  // Query callback is needed for V8 to properly handle property creation with correct
  // enumerable attributes when the interceptor is on the instance template.
  static v8::Intercepted query(
      v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Integer>& info) {
    v8::Intercepted result = v8::Intercepted::kNo;
    liftKj(info, [&]() -> v8::Local<v8::Integer> {
      auto isolate = info.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = info.HolderV2();
      auto& wrapper = TypeWrapper::from(isolate);
      if (!wrapper.getTemplate(isolate, static_cast<T*>(nullptr))->HasInstance(obj)) {
        throwTypeError(isolate, kIllegalInvocation);
      }
      auto& self = extractInternalPointer<T, false>(context, obj);
      auto& lock = Lock::from(isolate);
      if ((self.*getNamedMethod)(lock, kj::str(name.As<v8::String>())) != kj::none) {
        result = v8::Intercepted::kYes;
      }
      return {};
    });
    return result;
  }

  static v8::Intercepted getter(
      v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Intercepted result = v8::Intercepted::kNo;
    liftKj(info, [&]() -> v8::Local<v8::Value> {
      auto isolate = info.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = info.HolderV2();
      auto& wrapper = TypeWrapper::from(isolate);
      if (!wrapper.getTemplate(isolate, static_cast<T*>(nullptr))->HasInstance(obj)) {
        throwTypeError(isolate, kIllegalInvocation);
      }
      auto& self = extractInternalPointer<T, false>(context, obj);
      auto& lock = Lock::from(isolate);
      KJ_IF_SOME(value, (self.*getNamedMethod)(lock, kj::str(name.As<v8::String>()))) {
        result = v8::Intercepted::kYes;
        return wrapper.wrap(lock, context, obj, kj::fwd<Ret>(value));
      } else {
        // Return an empty handle to indicate the member doesn't exist.
        return {};
      }
    });
    return result;
  }
};

// ======================================================================================

// Used by the JSG_METHOD macro to register a method on a resource type.
template <typename TypeWrapper, typename Self, bool isContext>
struct ResourceTypeBuilder {
  ResourceTypeBuilder(TypeWrapper& typeWrapper,
      v8::Isolate* isolate,
      v8::Local<v8::FunctionTemplate> constructor,
      v8::Local<v8::ObjectTemplate> instance,
      v8::Local<v8::ObjectTemplate> prototype,
      v8::Local<v8::Signature> signature)
      : typeWrapper(typeWrapper),
        isolate(isolate),
        constructor(constructor),
        instance(instance),
        prototype(prototype),
        signature(signature) {
    // Mark the prototype as belonging to a resource type. Calling `util.inspect()` will see this
    // symbol and trigger custom inspect handling. This value of this symbol property maps names
    // names of internal pseudo-properties to symbol-keyed-getters for accessing them, or false if
    // the property is unimplemented.
    // See `JSG_INSPECT_PROPERTY` for more details.
    auto symbol = v8::Symbol::ForApi(isolate, v8StrIntern(isolate, "kResourceTypeInspect"_kj));
    inspectProperties = v8::ObjectTemplate::New(isolate);
    prototype->Set(symbol, inspectProperties,
        static_cast<v8::PropertyAttribute>(
            v8::PropertyAttribute::ReadOnly | v8::PropertyAttribute::DontEnum));
  }

  template <typename Type, typename GetNamedMethod, GetNamedMethod getNamedMethod>
  inline void registerWildcardProperty() {
    auto& resourceWrapper = static_cast<ResourceWrapper<TypeWrapper, Type>&>(typeWrapper);
    KJ_ASSERT(
        resourceWrapper.wildcardHandler == kj::none, "only one wildcard per instance supported");
    resourceWrapper.wildcardHandler =
        WildcardPropertyCallbacks<TypeWrapper, Type, GetNamedMethod, getNamedMethod>{};
  }

  template <typename Type>
  inline void registerInherit() {
    constructor->Inherit(
        typeWrapper.template getTemplate<isContext>(isolate, static_cast<Type*>(nullptr)));
    // Propagate wildcard proxy to children. It's a data property, so it should be propagated, but v8 only handles normal data properties.
    auto& parentWrapper = static_cast<ResourceWrapper<TypeWrapper, Type>&>(typeWrapper);
    if (parentWrapper.wildcardHandler != kj::none) {
      auto& selfWrapper = static_cast<ResourceWrapper<TypeWrapper, Self>&>(typeWrapper);
      KJ_ASSERT(
          selfWrapper.wildcardHandler == kj::none, "only one wildcard per instance supported");
      selfWrapper.wildcardHandler = parentWrapper.wildcardHandler;
    }
  }

  template <const char* name>
  inline void registerInheritIntrinsic(v8::Intrinsic intrinsic) {
    auto intrinsicPrototype = v8::FunctionTemplate::New(isolate);
    intrinsicPrototype->RemovePrototype();
    auto prototypeString = ::workerd::jsg::v8StrIntern(isolate, "prototype");
    intrinsicPrototype->SetIntrinsicDataProperty(prototypeString, intrinsic);
    constructor->Inherit(intrinsicPrototype);
  }

  template <typename Method, Method method>
  inline void registerCallable() {
    // Note that we set the call handler on the instance and not the prototype.
    // TODO(cleanup): Specifying the name (for error messages) as "(called as function)" is a bit
    //   hacky but it's hard to do better while reusing `MethodCallback`.
    static const char NAME[] = "(called as function)";
    instance->SetCallAsFunctionHandler(&MethodCallback<TypeWrapper, NAME, isContext, Self, Method,
                                       method, ArgumentIndexes<Method>>::callback);
  }

  template <const char* name, auto method>
  inline void registerMethod() {
    if constexpr (isFastApiCompatible<decltype(method)>) {
      if (typeWrapper.isFastApiEnabled()) {
        auto cFunction = v8::CFunction::Make(MethodCallback<TypeWrapper, name, isContext, Self,
            decltype(method), method, ArgumentIndexes<decltype(method)>>::template fastCallback<>);
        auto functionTemplate = v8::FunctionTemplate::NewWithCFunctionOverloads(isolate,
            &MethodCallback<TypeWrapper, name, isContext, Self, decltype(method), method,
                ArgumentIndexes<decltype(method)>>::callback,
            v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow,
            v8::SideEffectType::kHasSideEffect, {&cFunction, 1});

        prototype->Set(isolate, name, functionTemplate);
        return;
      }
    }

    prototype->Set(isolate, name,
        v8::FunctionTemplate::New(isolate,
            &MethodCallback<TypeWrapper, name, isContext, Self, decltype(method), method,
                ArgumentIndexes<decltype(method)>>::callback,
            v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow));
  }

  template <const char* name, typename Method, Method method>
  inline void registerStaticMethod() {
    if constexpr (isFastApiCompatible<Method>) {
      if (typeWrapper.isFastApiEnabled()) {
        auto cFunction = v8::CFunction::Make(StaticMethodCallback<TypeWrapper, name, Self, Method,
            method, ArgumentIndexes<Method>>::template fastCallback<>);

        // Create a function template with both slow and fast paths
        // Notably, we specify an empty signature because a static method invocation will have no holder
        // object.
        auto functionTemplate = v8::FunctionTemplate::NewWithCFunctionOverloads(isolate,
            &StaticMethodCallback<TypeWrapper, name, Self, Method, method,
                ArgumentIndexes<Method>>::callback,
            v8::Local<v8::Value>(), v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
            v8::SideEffectType::kHasSideEffect, {&cFunction, 1});
        functionTemplate->RemovePrototype();
        constructor->Set(v8StrIntern(isolate, name), functionTemplate);
        return;
      }
    }

    // Notably, we specify an empty signature because a static method invocation will have no holder
    // object.
    auto functionTemplate = v8::FunctionTemplate::New(isolate,
        &StaticMethodCallback<TypeWrapper, name, Self, Method, method,
            ArgumentIndexes<Method>>::callback,
        v8::Local<v8::Value>(), v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow);
    functionTemplate->RemovePrototype();
    constructor->Set(v8StrIntern(isolate, name), functionTemplate);
  }

  template <const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerInstanceProperty() {
    auto v8Name = v8StrIntern(isolate, name);

    using Gcb = GetterCallback<TypeWrapper, name, Getter, getter, isContext>;
    if (!Gcb::enumerable) {
      // Mark as unimplemented if `Gcb::enumerable` is `false`. This is only the case when `Getter`
      // returns `Unimplemented`.
      inspectProperties->Set(v8Name, v8::False(isolate), v8::PropertyAttribute::ReadOnly);
    }

    instance->SetNativeDataProperty(v8Name, Gcb::callback,
        &SetterCallback<TypeWrapper, name, Setter, setter, isContext>::callback,
        v8::Local<v8::Value>(),
        Gcb::enumerable ? v8::PropertyAttribute::None : v8::PropertyAttribute::DontEnum);
  }

  template <const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerPrototypeProperty() {
    auto v8Name = v8StrIntern(isolate, name);
    v8::Local<v8::FunctionTemplate> getterFn;
    v8::Local<v8::FunctionTemplate> setterFn;

    using Gcb = PropertyGetterCallback<TypeWrapper, name, Getter, getter, isContext>;
    using Scb = PropertySetterCallback<TypeWrapper, name, Setter, setter, isContext>;
    if (!Gcb::enumerable) {
      inspectProperties->Set(v8Name, v8::False(isolate), v8::PropertyAttribute::ReadOnly);
    }

    bool useSlowApi = true;
    if constexpr (isFastApiCompatible<Getter> && isFastApiCompatible<Setter>) {
      if (typeWrapper.isFastApiEnabled()) {
        auto getterCFunction = v8::CFunction::Make(Gcb::template fastCallback<>);
        getterFn = v8::FunctionTemplate::NewWithCFunctionOverloads(isolate, &Gcb::callback,
            v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow,
            v8::SideEffectType::kHasSideEffect, {&getterCFunction, 1});

        auto setterCFunction = v8::CFunction::Make(Scb::template fastCallback<>);
        setterFn = v8::FunctionTemplate::NewWithCFunctionOverloads(isolate, &Scb::callback,
            v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow,
            v8::SideEffectType::kHasSideEffect, {&setterCFunction, 1});

        useSlowApi = false;
      }
    }

    if (useSlowApi) {
      // Note that we cannot use `SetNativeDataProperty(...)` here the way we do with other
      // properties because it will not properly handle the prototype chain when it comes
      // to using setters... which is annoying. It means we end up having to use FunctionTemplates
      // instead of the more convenient property callbacks.
      getterFn = v8::FunctionTemplate::New(isolate, Gcb::callback);
      setterFn = v8::FunctionTemplate::New(isolate, &Scb::callback);
    }

    prototype->SetAccessorProperty(v8Name, getterFn, setterFn,
        Gcb::enumerable ? v8::PropertyAttribute::None : v8::PropertyAttribute::DontEnum);
  }

  template <const char* name, typename Getter, Getter getter>
  inline void registerReadonlyInstanceProperty() {
    auto v8Name = v8StrIntern(isolate, name);

    using Gcb = GetterCallback<TypeWrapper, name, Getter, getter, isContext>;
    if (!Gcb::enumerable) {
      inspectProperties->Set(v8Name, v8::False(isolate), v8::PropertyAttribute::ReadOnly);
    }

    instance->SetNativeDataProperty(v8Name, &Gcb::callback, nullptr, v8::Local<v8::Value>(),
        Gcb::enumerable ? v8::PropertyAttribute::ReadOnly
                        : static_cast<v8::PropertyAttribute>(
                              v8::PropertyAttribute::ReadOnly | v8::PropertyAttribute::DontEnum));
  }

  template <typename T>
  inline void registerReadonlyInstanceProperty(kj::StringPtr name, T value) {
    auto v8Name = v8StrIntern(isolate, name);
    auto v8Value = typeWrapper.wrap(isolate, kj::none, kj::mv(value));
    instance->Set(v8Name, v8Value, v8::PropertyAttribute::ReadOnly);
  }

  template <const char* name, typename Getter, Getter getter>
  inline void registerReadonlyPrototypeProperty() {
    auto v8Name = v8StrIntern(isolate, name);

    using Gcb = PropertyGetterCallback<TypeWrapper, name, Getter, getter, isContext>;
    if (!Gcb::enumerable) {
      inspectProperties->Set(v8Name, v8::False(isolate), v8::PropertyAttribute::ReadOnly);
    }
    auto getterFn = v8::FunctionTemplate::New(isolate, Gcb::callback);

    prototype->SetAccessorProperty(v8Name, getterFn, {},
        Gcb::enumerable ? v8::PropertyAttribute::ReadOnly
                        : static_cast<v8::PropertyAttribute>(
                              v8::PropertyAttribute::DontEnum | v8::PropertyAttribute::ReadOnly));
  }

  template <const char* name, typename Getter, Getter getter, bool readOnly>
  inline void registerLazyInstanceProperty() {
    auto v8Name = v8StrIntern(isolate, name);

    using Gcb = GetterCallback<TypeWrapper, name, Getter, getter, isContext>;
    if (!Gcb::enumerable) {
      inspectProperties->Set(v8Name, v8::False(isolate), v8::PropertyAttribute::ReadOnly);
    }

    v8::PropertyAttribute attributes =
        Gcb::enumerable ? v8::PropertyAttribute::None : v8::PropertyAttribute::DontEnum;
    if (readOnly) {
      attributes = static_cast<v8::PropertyAttribute>(attributes | v8::PropertyAttribute::ReadOnly);
    }
    instance->SetLazyDataProperty(v8Name, &Gcb::callback, v8::Local<v8::Value>(), attributes);
  }

  template <const char* name, typename Getter, Getter getter>
  inline void registerInspectProperty() {
    using Gcb = PropertyGetterCallback<TypeWrapper, name, Getter, getter, isContext>;

    auto v8Name = v8StrIntern(isolate, name);

    // Create a new unique symbol so this property can only be accessed through `util.inspect()`
    auto symbol = v8::Symbol::New(isolate, v8Name);
    inspectProperties->Set(v8Name, symbol, v8::PropertyAttribute::ReadOnly);

    auto getterFn = v8::FunctionTemplate::New(isolate, &Gcb::callback);
    prototype->SetAccessorProperty(symbol, getterFn, {},
        static_cast<v8::PropertyAttribute>(
            v8::PropertyAttribute::ReadOnly | v8::PropertyAttribute::DontEnum));
  }

  template <const char* name, typename T>
  inline void registerStaticConstant(T value) {
    // The main difference between this and a read-only property is that a static constant has no
    // getter but is simply a primitive value set at constructor creation time.

    auto v8Name = v8StrIntern(isolate, name);
    auto v8Value = typeWrapper.wrap(isolate, kj::none, kj::mv(value));

    constructor->Set(v8Name, v8Value, v8::PropertyAttribute::ReadOnly);
    constructor->PrototypeTemplate()->Set(v8Name, v8Value, v8::PropertyAttribute::ReadOnly);
  }

  template <const char* name, typename Getter, Getter getter>
  inline void registerStaticProperty() {
    constructor->SetNativeDataProperty(v8StrIntern(isolate, name),
        &StaticPropertyCallback<TypeWrapper, name, Self, Getter, getter>::callback, nullptr,
        v8::Local<v8::Value>(), v8::PropertyAttribute::ReadOnly);
  }

  template <const char* name, typename Method, Method method>
  inline void registerIterable() {
    prototype->Set(v8::Symbol::GetIterator(isolate),
        v8::FunctionTemplate::New(isolate,
            &MethodCallback<TypeWrapper, name, isContext, Self, Method, method,
                ArgumentIndexes<Method>>::callback,
            v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow),
        v8::PropertyAttribute::DontEnum);
  }

  template <const char* name, typename Method, Method method>
  inline void registerAsyncIterable() {
    prototype->Set(v8::Symbol::GetAsyncIterator(isolate),
        v8::FunctionTemplate::New(isolate,
            &MethodCallback<TypeWrapper, name, isContext, Self, Method, method,
                ArgumentIndexes<Method>>::callback,
            v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow),
        v8::PropertyAttribute::DontEnum);
  }

  template <const char* name, typename Method, Method method>
  inline void registerDispose() {
    prototype->Set(v8::Symbol::GetDispose(isolate),
        v8::FunctionTemplate::New(isolate,
            &MethodCallback<TypeWrapper, name, isContext, Self, Method, method,
                ArgumentIndexes<Method>>::callback,
            v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow),
        v8::PropertyAttribute::DontEnum);
  }

  template <const char* name, typename Method, Method method>
  inline void registerAsyncDispose() {
    prototype->Set(v8::Symbol::GetAsyncDispose(isolate),
        v8::FunctionTemplate::New(isolate,
            &MethodCallback<TypeWrapper, name, isContext, Self, Method, method,
                ArgumentIndexes<Method>>::callback,
            v8::Local<v8::Value>(), signature, 0, v8::ConstructorBehavior::kThrow),
        v8::PropertyAttribute::DontEnum);
  }

  template <typename Type, const char* name>
  inline void registerNestedType() {
    static_assert(Type::JSG_KIND == ::workerd::jsg::JsgKind::RESOURCE,
        "Type is not a resource type, and therefore cannot be declared nested");

    constexpr auto hasGetTemplate =
        ::workerd::jsg::isDetected<::workerd::jsg::HasGetTemplateOverload, decltype(typeWrapper),
            Type>();
    static_assert(
        hasGetTemplate, "Type must be listed in JSG_DECLARE_ISOLATE_TYPE to be declared nested.");

    prototype->Set(isolate, name, typeWrapper.getTemplate(isolate, static_cast<Type*>(nullptr)));
  }

  inline void registerTypeScriptRoot() { /* only needed for RTTI */ }

  template <const char* tsOverride>
  inline void registerTypeScriptOverride() { /* only needed for RTTI */ }

  template <const char* tsDefine>
  inline void registerTypeScriptDefine() { /* only needed for RTTI */ }

  inline void registerJsBundle(Bundle::Reader bundle) { /* handled at the second stage */ }

 private:
  TypeWrapper& typeWrapper;
  v8::Isolate* isolate;
  v8::Local<v8::FunctionTemplate> constructor;
  v8::Local<v8::ObjectTemplate> instance;
  v8::Local<v8::ObjectTemplate> prototype;
  v8::Local<v8::ObjectTemplate> inspectProperties;
  v8::Local<v8::Signature> signature;
};

// initializes javascript parts of a context
template <typename TypeWrapper, typename Self>
struct JsSetup {
  KJ_DISALLOW_COPY_AND_MOVE(JsSetup);

  JsSetup(jsg::Lock& js, v8::Local<v8::Context> context): js(js), context(context) {}

  inline void registerJsBundle(Bundle::Reader bundle) {
    ModuleRegistryImpl<TypeWrapper>::from(js)->addBuiltinBundle(bundle);
  }

  // the rest of the callbacks are empty

  template <typename Type>
  inline void registerInherit() {}

  template <const char* name>
  inline void registerInheritIntrinsic(v8::Intrinsic intrinsic) {}

  template <typename Method, Method method>
  inline void registerCallable() {}

  template <const char* name, auto method>
  inline void registerMethod() {}

  template <const char* name, typename Method, Method method>
  inline void registerStaticMethod() {}

  template <const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerInstanceProperty() {}

  template <const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerPrototypeProperty() {}

  template <const char* name, typename Getter, Getter getter>
  inline void registerReadonlyInstanceProperty() {}

  template <typename T>
  inline void registerReadonlyInstanceProperty(kj::StringPtr name, T value) {}

  template <const char* name, typename Getter, Getter getter>
  inline void registerReadonlyPrototypeProperty() {}

  template <const char* name, typename Getter, Getter getter, bool readOnly>
  inline void registerLazyInstanceProperty() {}

  template <const char* name, typename Getter, Getter getter>
  inline void registerInspectProperty() {}

  template <const char* name, typename T>
  inline void registerStaticConstant(T value) {}

  template <const char* name, typename Getter, Getter getter>
  inline void registerStaticProperty() {}

  template <const char* name, typename Method, Method method>
  inline void registerIterable() {}

  template <const char* name, typename Method, Method method>
  inline void registerAsyncIterable() {}

  template <typename Type, const char* name>
  inline void registerNestedType() {}

  inline void registerTypeScriptRoot() {}

  template <const char* tsOverride>
  inline void registerTypeScriptOverride() {}

  template <const char* tsDefine>
  inline void registerTypeScriptDefine() {}

 private:
  jsg::Lock& js;
  v8::Local<v8::Context> context;
};

class ModuleRegistryBase {
 public:
  virtual ~ModuleRegistryBase() noexcept(false) {}
  virtual kj::Own<void> attachToIsolate(
      Lock& js, const CompilationObserver& observer) const KJ_WARN_UNUSED_RESULT = 0;

  virtual const capnp::SchemaLoader& getSchemaLoader() const = 0;
};

struct NewContextOptions {
  kj::Maybe<const ModuleRegistryBase&> newModuleRegistry = kj::none;
  kj::Maybe<const capnp::SchemaLoader&> schemaLoader = kj::none;
  bool enableWeakRef = false;
};

// TypeWrapper mixin for resource types (application-defined C++ classes declared with a
// JSG_RESOURCE_TYPE block).
template <typename TypeWrapper, typename T>
class ResourceWrapper {
 public:
  // If the JSG_RESOURCE_TYPE macro declared a configuration parameter, then `Configuration` will
  // be that type, otherwise NullConfiguration which accepts any configuration.
  using Configuration = DetectedOr<NullConfiguration, GetConfiguration, T>;

  template <typename MetaConfiguration>
  ResourceWrapper(MetaConfiguration&& configuration)
      : configuration(kj::fwd<MetaConfiguration>(configuration)) {}

  inline void initTypeWrapper() {
    TypeWrapper& wrapper = static_cast<TypeWrapper&>(*this);
    wrapper.resourceTypeMap.insert(typeid(T),
        [](TypeWrapper& wrapper,
            v8::Isolate* isolate) -> DynamicResourceTypeMap<TypeWrapper>::DynamicTypeInfo {
      kj::Maybe<typename DynamicResourceTypeMap<TypeWrapper>::ReflectionInitializer&> rinit;
      if constexpr (T::jsgHasReflection) {
        rinit = [](jsg::Object& object, TypeWrapper& wrapper) {
          static_cast<T&>(object).jsgInitReflection(wrapper);
        };
      }
      return {wrapper.getTemplate(isolate, static_cast<T*>(nullptr)), rinit};
    });

    if constexpr (static_cast<uint>(T::jsgSerializeTag) !=
        static_cast<uint>(T::jsgSuper::jsgSerializeTag)) {
      // This type is declared JSG_SERIALIZABLE.
      // HACK: The type of `serializer` should be `Serializer&`, not `auto&`, but Clang complains
      //   about the `writeRawUint32()` call being made on an incomplete type if `ser.h` hasn't been
      //   included -- *even if* T doesn't declare itself serializable and therefore this branch
      //   should not be compiled at all! Unsure if this is a compiler bug.
      wrapper.serializerMap.insert(
          typeid(T), [](TypeWrapper& wrapper, Lock& js, jsg::Object& instance, auto& serializer) {
        serializer.writeRawUint32(static_cast<uint>(T::jsgSerializeTag));
        SerializeInvoker<TypeWrapper, T>::call(wrapper, static_cast<T&>(instance), js, serializer);
      });

      if constexpr (!T::jsgSerializeOneway) {
        typename TypeWrapper::DeserializeFunc* deserializeFunc =
            [](TypeWrapper& wrapper, Lock& js, uint tag, Deserializer& deserializer) {
          // Cast the tag to the application's preferred tag type.
          auto typedTag = static_cast<decltype(T::jsgSerializeTag)>(tag);
          return DeserializeInvoker<TypeWrapper, T>::call(wrapper, js, typedTag, deserializer);
        };

        // We make duplicates here fatal because it's really hard to debug exceptions thrown during
        // isolate startup and frankly this is pretty fatal for the runtime anyway.
        auto reportDuplicate = [](auto&, auto&&) noexcept {
          KJ_FAIL_REQUIRE("JSG_SERIALIZABLE declaration tried to register a duplicate type tag");
        };

        wrapper.deserializerMap.upsert(
            static_cast<uint>(T::jsgSerializeTag), deserializeFunc, reportDuplicate);
        for (auto& oldTag: T::jsgSerializeOldTags) {
          wrapper.deserializerMap.upsert(
              static_cast<uint>(oldTag), deserializeFunc, reportDuplicate);
        }
      }
    }
  }

  static constexpr const std::type_info& getName(T*) {
    return typeid(T);
  }

  // `Ref<T>` is NOT a resource type -- TypeHandler<Ref<T>> should use the value-oriented
  // implementation.
  static constexpr const std::type_info& getName(Ref<T>*) {
    return typeid(T);
  }

  v8::Local<v8::Object> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      Ref<T>&& value) {
    // Wrap a value of type T.

    auto isolate = js.v8Isolate;

    KJ_IF_SOME(h, value->tryGetHandle(isolate)) {
      return h;
    } else {
      auto& type = typeid(*value);
      auto& wrapper = static_cast<TypeWrapper&>(*this);
      // Check if *value is actually a subclass of T. If so, we need to dynamically look up the
      // correct wrapper. But in the common case that it's exactly T, we can skip the lookup.
      v8::Local<v8::FunctionTemplate> tmpl;
      if (type == typeid(T)) {
        tmpl = getTemplate(isolate, nullptr);
        if constexpr (T::jsgHasReflection) {
          value->jsgInitReflection(wrapper);
        }
      } else {
        auto info = wrapper.getDynamicTypeInfo(isolate, type);
        tmpl = info.tmpl;
        KJ_IF_SOME(i, info.reflectionInitializer) {
          i(*value, wrapper);
        }
      }
      v8::Local<v8::Object> object = check(tmpl->InstanceTemplate()->NewInstance(context));
      value.attachWrapper(isolate, object);
      return object;
    }
  }

  template <typename... Args>
  JsContext<T> newContext(jsg::Lock& js,
      jsg::NewContextOptions options,
      CompilationObserver& compilationObserver,
      T*,
      Args&&... args) {
    // Construct an instance of this type to be used as the Javascript global object, creating
    // a new JavaScript context. Unfortunately, we have to do some things differently in this
    // case, because of quirks in how V8 handles the global object. There appear to be bugs
    // that prevent it from being treated uniformly for callback purposes. See:
    //
    //     https://groups.google.com/d/msg/v8-users/RET5b3KOa5E/3EvpRBzwAQAJ
    //
    // Because of this, our entire type registration system threads through an extra template
    // parameter `bool isContext`. When the application decides to create a context using this
    // type as the global, we instantiate this separate branch specifically for that type.
    // Fortunately, for types that are never used as the global object, we never have to
    // instantiate the `isContext = true` branch.

    auto isolate = js.v8Isolate;
    auto tmpl = getTemplate<true>(isolate, nullptr)->InstanceTemplate();
    v8::Local<v8::Context> context = v8::Context::New(isolate, nullptr, tmpl);
    auto global = context->Global();

    auto ptr = js.alloc<T>(kj::fwd<Args>(args)...);
    if constexpr (T::jsgHasReflection) {
      ptr->jsgInitReflection(static_cast<TypeWrapper&>(*this));
    }
    ptr.attachWrapper(isolate, global);

    // Disable `eval(code)` and `new Function(code)`. (Actually, setting this to `false` really
    // means "call the callback registered on the isolate to check" -- setting it to `true` means
    // "skip callback and just allow".)
    context->AllowCodeGenerationFromStrings(false);

    if (!options.enableWeakRef) {
      check(global->Delete(context, v8StrIntern(isolate, "WeakRef"_kj)));
      check(global->Delete(context, v8StrIntern(isolate, "FinalizationRegistry"_kj)));
    }

    // Store a pointer to this object in slot 1, to be extracted in callbacks.
    jsg::setAlignedPointerInEmbedderData(
        context, jsg::ContextPointerSlot::GLOBAL_WRAPPER, ptr.get());
    // We need to set the highest used index in every context we create to be a nullptr
    // This is because we might later on call GetAlignedPointerFromEmbedderData which fails with
    // a fatal error if the array is smaller than the given index.
    jsg::setAlignedPointerInEmbedderData(
        context, jsg::ContextPointerSlot::MAX_POINTER_SLOT, nullptr);

    // (Note: V8 docs say: "Note that index 0 currently has a special meaning for Chrome's
    // debugger." We aren't Chrome, but it does appear that some versions of V8 will mess with
    // slot 0, causing us to segfault if we try to put anything there. So we avoid it and use slot
    // 1, which seems to work just fine.)

    // Expose the type of the global scope in the global scope itself.
    exposeGlobalScopeType(isolate, context);

    ptr->setModuleRegistryBackingOwner(([&]() -> kj::Own<void> {
      KJ_IF_SOME(newModuleRegistry, options.newModuleRegistry) {
        return JSG_WITHIN_CONTEXT_SCOPE(js, context, [&](jsg::Lock& js) {
          // The context must be current for attachToIsolate to succeed.
          return newModuleRegistry.attachToIsolate(js, compilationObserver);
        });
      } else {
        return ModuleRegistryImpl<TypeWrapper>::install(isolate, context, compilationObserver);
      }
    })());

    KJ_IF_SOME(loader, options.schemaLoader) {
      ptr->setSchemaLoader(loader);
    }

    return JSG_WITHIN_CONTEXT_SCOPE(js, context, [&](jsg::Lock& js) {
      setupJavascript(js);
      return JsContext<T>(context, kj::mv(ptr));
    });
  }

  kj::Maybe<T&> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      T*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // Try to unwrap a value of type T.

    if (handle->IsObject()) {
      v8::Local<v8::Object> instance =
          v8::Local<v8::Object>::Cast(handle)->FindInstanceInPrototypeChain(
              getTemplate(js.v8Isolate, nullptr));
      if (!instance.IsEmpty()) {
        return extractInternalPointer<T, false>(context, instance);
      }
    }

    return kj::none;
  }

  kj::Maybe<Ref<T>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Ref<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // Try to unwrap a value of type Ref<T>.

    KJ_IF_SOME(p, tryUnwrap(js, context, handle, static_cast<T*>(nullptr), parentObject)) {
      return Ref<T>(kj::addRef(p));
    } else {
      return kj::none;
    }
  }

  template <bool isContext = false>
  v8::Local<v8::FunctionTemplate> getTemplate(v8::Isolate* isolate, T*) {
    v8::Global<v8::FunctionTemplate>& slot = isContext ? contextConstructor : memoizedConstructor;
    if (slot.IsEmpty()) {
      // Construct lazily.
      v8::EscapableHandleScope scope(isolate);

      v8::Local<v8::FunctionTemplate> constructor;
      if constexpr (!isContext && HasConstructorMethod<T>) {
        constructor =
            v8::FunctionTemplate::New(isolate, &ConstructorCallback<TypeWrapper, T>::callback);
      } else {
        constructor = v8::FunctionTemplate::New(isolate, &throwIllegalConstructor);
      }

      auto prototype = constructor->PrototypeTemplate();

      // Signatures protect our methods from being invoked with the wrong `this`.
      auto signature = v8::Signature::New(isolate, constructor);

      auto instance = constructor->InstanceTemplate();

      instance->SetInternalFieldCount(Wrappable::INTERNAL_FIELD_COUNT);

      auto classname = v8StrIntern(isolate, typeName(typeid(T)));

      if (getShouldSetToStringTag(isolate)) {
        prototype->Set(
            v8::Symbol::GetToStringTag(isolate), classname, v8::PropertyAttribute::DontEnum);
      }

      // Previously, miniflare would use the lack of a Symbol.toStringTag on a class to
      // detect a type that came from the runtime. That's obviously a bit problematic because
      // Symbol.toStringTag is required for full compliance on standard web platform APIs.
      // To help use cases where it is necessary to detect if a class is a runtime class, we
      // will add a special symbol to the prototype of the class to indicate. Note that
      // because this uses the global symbol registry user code could still mark their own
      // classes with this symbol but that's unlikely to be a problem in any practical case.
      auto internalMarker =
          v8::Symbol::For(isolate, v8StrIntern(isolate, "cloudflare:internal-class"));
      prototype->Set(internalMarker, internalMarker,
          static_cast<v8::PropertyAttribute>(v8::PropertyAttribute::DontEnum |
              v8::PropertyAttribute::DontDelete | v8::PropertyAttribute::ReadOnly));

      if (getShouldSetImmutablePrototype(isolate)) {
        constructor->ReadOnlyPrototype();
      }

      constructor->SetClassName(classname);

      static_assert(kj::isSameType<typename T::jsgThis, T>(),
          "Name passed to JSG_RESOURCE_TYPE() must be the class's own name.");

      auto& typeWrapper = static_cast<TypeWrapper&>(*this);

      ResourceTypeBuilder<TypeWrapper, T, isContext> builder(
          typeWrapper, isolate, constructor, instance, prototype, signature);

      if constexpr (isDetected<GetConfiguration, T>()) {
        T::template registerMembers<decltype(builder), T>(builder, configuration);
      } else {
        T::template registerMembers<decltype(builder), T>(builder);
      }

      KJ_IF_SOME(handler, wildcardHandler) {
        instance->SetHandler(handler);
      }

      auto result = scope.Escape(constructor);
      slot.Reset(isolate, result);
      return result;
    } else {
      return slot.Get(isolate);
    }
  }

  kj::Maybe<v8::NamedPropertyHandlerConfiguration> wildcardHandler;

 private:
  Configuration configuration;
  v8::Global<v8::FunctionTemplate> memoizedConstructor;
  v8::Global<v8::FunctionTemplate> contextConstructor;

  void setupJavascript(jsg::Lock& js) {
    JsSetup<TypeWrapper, T> setup(js, js.v8Context());

    if constexpr (isDetected<GetConfiguration, T>()) {
      T::template registerMembers<decltype(setup), T>(setup, configuration);
    } else {
      T::template registerMembers<decltype(setup), T>(setup);
    }
  }

  template <typename, typename, typename, typename>
  friend struct ConstructorCallback;
};

// Like ResourceWrapper for T = jsg::Object. We need some special-casing for this type.
template <typename TypeWrapper>
class ObjectWrapper {
 public:
  static constexpr const std::type_info& getName(Object*) {
    return typeid(Object);
  }

  static constexpr const std::type_info& getName(Ref<Object>*) {
    return typeid(Object);
  }

  // Wrap a value of type T.
  v8::Local<v8::Object> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      Ref<Object>&& value) {
    auto isolate = js.v8Isolate;

    KJ_IF_SOME(h, value->tryGetHandle(isolate)) {
      return h;
    } else {
      auto& valueRef = *value;  // avoid compiler warning about typeid(*value) having side effects
      auto& type = typeid(valueRef);
      auto& wrapper = static_cast<TypeWrapper&>(*this);

      // In ResourceWrapper::wrap() we check if the value is a subclass. Here, we assume it is
      // always a subclass, because `jsg::Object` cannot be constructed directly.
      auto info = wrapper.getDynamicTypeInfo(isolate, type);
      v8::Local<v8::FunctionTemplate> tmpl = info.tmpl;
      KJ_IF_SOME(i, info.reflectionInitializer) {
        i(*value, wrapper);
      }
      v8::Local<v8::Object> object = check(tmpl->InstanceTemplate()->NewInstance(context));
      value.attachWrapper(isolate, object);
      return object;
    }
  }

  // We do not support unwrapping Ref<Object>; use V8Ref<v8::Object> instead.
  kj::Maybe<Ref<Object>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Ref<Object>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) = delete;
};

}  // namespace workerd::jsg
