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

#include <kj/tuple.h>
#include <kj/debug.h>
#include <type_traits>
#include <kj/map.h>
#include "util.h"
#include "wrappable.h"
#include "jsg.h"
#include <typeindex>

namespace std {
  inline auto KJ_HASHCODE(const std::type_index& idx) {
    // Make std::type_index (which points to std::type_info) usable as a kj::HashMap key.
    // TODO(cleanup): This probably shouldn't live here, but where should it live? KJ?
    return idx.hash_code();
  }
}

namespace workerd::jsg {

template <typename T>
constexpr bool resourceNeedsGcTracing() {
  // Return true if the type requires GC visitation, which we assume is the case if the type or any
  // superclass (other than Object) declares a `visitForGc()` method.
  return &T::visitForGc != &Object::visitForGc;
}
template <>
constexpr bool resourceNeedsGcTracing<Object>() {
  return false;
}

template <typename T>
inline void visitSubclassForGc(T* obj, GcVisitor& visitor) {
  // Call obj->visitForGc() if and only if T defines its own `visitForGc()` method -- do not call
  // the parent class's `visitForGc()`.
  if constexpr (&T::visitForGc != &T::jsgSuper::visitForGc) {
    obj->visitForGc(visitor);
  }
}

template <typename T>
struct ArgumentIndexes_;
template <typename T, typename Ret, typename... Args>
struct ArgumentIndexes_<Ret (T::*)(Args...)> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename T, typename Ret, typename... Args>
struct ArgumentIndexes_<Ret (T::*)(Lock&, Args...)> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename T, typename Ret, typename... Args>
struct ArgumentIndexes_<Ret (T::*)(const v8::FunctionCallbackInfo<v8::Value>&, Args...)> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename T, typename Ret, typename... Args>
struct ArgumentIndexes_<Ret (T::*)(Args...) const> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename T, typename Ret, typename... Args>
struct ArgumentIndexes_<Ret (T::*)(Lock&, Args...) const> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename T, typename Ret, typename... Args>
struct ArgumentIndexes_<Ret (T::*)(const v8::FunctionCallbackInfo<v8::Value>&, Args...) const> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename Ret, typename... Args>
struct ArgumentIndexes_<Ret(Args...)> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename Ret, typename... Args>
struct ArgumentIndexes_<Ret(Lock&, Args...)> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename Ret, typename... Args>
struct ArgumentIndexes_<Ret(const v8::FunctionCallbackInfo<v8::Value>&, Args...)> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename T>
using ArgumentIndexes = typename ArgumentIndexes_<T>::Indexes;
// ArgumentIndexes<SomeMethodType> expands to kj::_::Indexes<0, 1, 2, 3, ..., n-1>, where n is the
// number of arguments to the method, not counting the magic Lock or FunctionCallbackInfo parameter
// (if any).

template <typename T, bool isContext>
T& extractInternalPointer(const v8::Local<v8::Context>& context,
                          const v8::Local<v8::Object>& object) {
  // Given a handle to a resource type, extract the raw C++ object pointer.
  //
  // Due to bugs in V8, we can't use internal fields on the global object:
  //   https://groups.google.com/d/msg/v8-users/RET5b3KOa5E/3EvpRBzwAQAJ
  //
  // So, when wrapping a global object, we store the pointer in the "embedder data" of the context
  // instead of the internal fields of the object.

  if constexpr (isContext) {
    // V8 docs say EmbedderData slot 0 is special, so we use slot 1. (See comments in newContext().)
    return *reinterpret_cast<T*>(context->GetAlignedPointerFromEmbedderData(1));
  } else {
    KJ_ASSERT(object->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);
    return *reinterpret_cast<T*>(object->GetAlignedPointerFromInternalField(
        Wrappable::WRAPPED_OBJECT_FIELD_INDEX));
  }
}

void throwIfConstructorCalledAsFunction(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    const std::type_info& type);

void scheduleUnimplementedConstructorError(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    const std::type_info& type);
void scheduleUnimplementedMethodError(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    const std::type_info& type, const char* methodName);
void scheduleUnimplementedPropertyError(
    const v8::PropertyCallbackInfo<v8::Value>& args,
    const std::type_info& type, const char* propertyName);
// The scheduleUnimplemented* variants will schedule an exception on the isolate
// but do not throw a JsExceptionThrown.

// Called to throw errors about calling unimplemented functionality. It's assumed these are called
// directly from the V8 trampoline without liftKj, so they don't throw JsExceptionThrown.

template <typename TypeWrapper, typename T,
          typename = decltype(T::constructor),
          typename = ArgumentIndexes<decltype(T::constructor)>>
struct ConstructorCallback;
// Implements the V8 callback function for calling the static `constructor()` method of the C++
// class.

template <typename TypeWrapper, typename T, typename... Args, size_t... indexes>
struct ConstructorCallback<TypeWrapper, T, Ref<T>(Args...), kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();

      throwIfConstructorCalledAsFunction(args, typeid(T));

      auto context = isolate->GetCurrentContext();
      auto obj = args.This();
      KJ_ASSERT(obj->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);

      auto& wrapper = TypeWrapper::from(isolate);

      Ref<T> ptr = T::constructor(
          wrapper.template unwrap<Args>(context, args, indexes,
              TypeErrorContext::constructorArgument(typeid(T), indexes))...);
      if constexpr (T::jsgHasReflection) {
        ptr->jsgInitReflection(wrapper);
      }
      ptr.attachWrapper(isolate, obj);
    });
  }
};

template <typename TypeWrapper, typename T, typename... Args, size_t... indexes>
struct ConstructorCallback<TypeWrapper, T,
    Ref<T>(Lock&, Args...), kj::_::Indexes<indexes...>> {
  // Specialization for constructors that take `Lock&` as their first parameter.

  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();

      throwIfConstructorCalledAsFunction(args, typeid(T));

      auto context = isolate->GetCurrentContext();
      auto obj = args.This();
      KJ_ASSERT(obj->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);

      auto& wrapper = TypeWrapper::from(isolate);

      Ref<T> ptr = T::constructor(Lock::from(isolate),
          wrapper.template unwrap<Args>(context, args, indexes,
              TypeErrorContext::constructorArgument(typeid(T), indexes))...);
      if constexpr (T::jsgHasReflection) {
        ptr->jsgInitReflection(wrapper);
      }
      ptr.attachWrapper(isolate, obj);
    });
  }
};

template <typename TypeWrapper, typename T, typename... Args, size_t... indexes>
struct ConstructorCallback<TypeWrapper, T,
    Ref<T>(const v8::FunctionCallbackInfo<v8::Value>&, Args...), kj::_::Indexes<indexes...>> {
  // Specialization for constructors that take `const v8::FunctionCallbackInfo<v8::Value>&` as
  // their first parameter.

  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();

      throwIfConstructorCalledAsFunction(args, typeid(T));

      auto context = isolate->GetCurrentContext();
      auto obj = args.This();
      KJ_ASSERT(obj->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);

      auto& wrapper = TypeWrapper::from(isolate);

      Ref<T> ptr = T::constructor(args,
          wrapper.template unwrap<Args>(context, args, indexes,
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

template <typename TypeWrapper, const char* methodName, bool isContext,
          typename T, typename Method, Method method, typename Indexes>
struct MethodCallback;
// Implements the V8 callback function for calling a method of the C++ class.

template <typename TypeWrapper, const char* methodName, bool isContext,
          typename T, typename U, typename Ret, typename... Args,
          Ret (U::*method)(Args...), size_t... indexes>
struct MethodCallback<TypeWrapper, methodName, isContext,
                      T, Ret (U::*)(Args...), method, kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = args.This();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      if constexpr(isVoid<Ret>()) {
        (self.*method)(wrapper.template unwrap<Args>(context, args, indexes,
            TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
      } else {
        return wrapper.wrap(context, obj, (self.*method)(
            wrapper.template unwrap<Args>(context, args, indexes,
                TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...));
      }
    });
  }
};

template <typename TypeWrapper, const char* methodName, bool isContext,
          typename T, typename U, typename Ret, typename... Args,
          Ret (U::*method)(Lock&, Args...), size_t... indexes>
struct MethodCallback<TypeWrapper, methodName, isContext,
                      T, Ret (U::*)(Lock&, Args...),
                      method, kj::_::Indexes<indexes...>> {
  // Specialization for methods that take `Lock&` as their first parameter.

  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = args.This();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      auto& lock = Lock::from(isolate);
      if constexpr(isVoid<Ret>()) {
        (self.*method)(lock, wrapper.template unwrap<Args>(context, args, indexes,
            TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
      } else {
        return wrapper.wrap(context, obj, (self.*method)(lock,
            wrapper.template unwrap<Args>(context, args, indexes,
                TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...));
      }
    });
  }
};

template <typename TypeWrapper, const char* methodName, bool isContext,
          typename T, typename U, typename Ret, typename... Args,
          Ret (U::*method)(const v8::FunctionCallbackInfo<v8::Value>&, Args...), size_t... indexes>
struct MethodCallback<TypeWrapper, methodName, isContext,
                      T, Ret (U::*)(const v8::FunctionCallbackInfo<v8::Value>&, Args...),
                      method, kj::_::Indexes<indexes...>> {
  // Specialization for methods that take `const v8::FunctionCallbackInfo<v8::Value>&` as their
  // first parameter.

  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = args.This();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      if constexpr(isVoid<Ret>()) {
        (self.*method)(args, wrapper.template unwrap<Args>(context, args, indexes,
            TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
      } else {
        return wrapper.wrap(context, obj, (self.*method)(args,
            wrapper.template unwrap<Args>(context, args, indexes,
                TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...));
      }
    });
  }
};

template <typename TypeWrapper, const char* methodName, bool isContext,
          typename T, typename U, typename... Args,
          Unimplemented (U::*method)(Args...), size_t... indexes>
struct MethodCallback<TypeWrapper, methodName, isContext,
                      T, Unimplemented (U::*)(Args...), method, kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    scheduleUnimplementedMethodError(args, typeid(T), methodName);
  }
};

template <typename TypeWrapper, const char* methodName,
          typename T, typename Method, Method* method, typename Indexes>
struct StaticMethodCallback;
// Implements the V8 callback function for calling a static method of the C++ class.
//
// This is separate from MethodCallback<> because we need to know the interface type, T, and it
// won't be deducible from Method when Method is a C++ static member function.
//
// In the explicit specializations of this template, we use TypeErrorContext::methodArgument() for
// generating error messages, rather than concoct a new error message format specifically for static
// methods. This matches Chrome's behavior.

template <typename TypeWrapper, const char* methodName,
          typename T, typename Ret, typename... Args,
          Ret (*method)(Args...), size_t... indexes>
struct StaticMethodCallback<TypeWrapper, methodName, T,
                            Ret(Args...), method, kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto& wrapper = TypeWrapper::from(isolate);
      if constexpr(isVoid<Ret>()) {
        (*method)(wrapper.template unwrap<Args>(context, args, indexes,
            TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
      } else {
        return wrapper.wrap(context, nullptr, (*method)(
            wrapper.template unwrap<Args>(context, args, indexes,
                TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...));
      }
    });
  }
};

template <typename TypeWrapper, const char* methodName,
          typename T, typename Ret, typename... Args,
          Ret (*method)(Lock&, Args...), size_t... indexes>
struct StaticMethodCallback<TypeWrapper, methodName, T,
                            Ret(Lock&, Args...),
                            method, kj::_::Indexes<indexes...>> {
  // Specialization for methods that take `Lock&` as their first parameter.

  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto& wrapper = TypeWrapper::from(isolate);
      auto& lock = Lock::from(isolate);
      if constexpr(isVoid<Ret>()) {
        (*method)(lock, wrapper.template unwrap<Args>(context, args, indexes,
            TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
      } else {
        return wrapper.wrap(context, nullptr, (*method)(lock,
            wrapper.template unwrap<Args>(context, args, indexes,
                TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...));
      }
    });
  }
};

template <typename TypeWrapper, const char* methodName,
          typename T, typename Ret, typename... Args,
          Ret (*method)(const v8::FunctionCallbackInfo<v8::Value>&, Args...), size_t... indexes>
struct StaticMethodCallback<TypeWrapper, methodName, T,
                            Ret(const v8::FunctionCallbackInfo<v8::Value>&, Args...),
                            method, kj::_::Indexes<indexes...>> {
  // Specialization for methods that take `const v8::FunctionCallbackInfo<v8::Value>&` as their
  // first parameter.

  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    liftKj(args, [&]() {
      auto isolate = args.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto& wrapper = TypeWrapper::from(isolate);
      if constexpr(isVoid<Ret>()) {
        (*method)(args, wrapper.template unwrap<Args>(context, args, indexes,
            TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...);
      } else {
        return wrapper.wrap(context, nullptr, (*method)(args,
            wrapper.template unwrap<Args>(context, args, indexes,
                TypeErrorContext::methodArgument(typeid(T), methodName, indexes))...));
      }
    });
  }
};

template <typename TypeWrapper, const char* methodName,
          typename T, typename... Args,
          Unimplemented (*method)(Args...), size_t... indexes>
struct StaticMethodCallback<TypeWrapper, methodName, T,
                            Unimplemented(Args...), method, kj::_::Indexes<indexes...>> {
  static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    scheduleUnimplementedMethodError(args, typeid(T), methodName);
  }
};

template <typename TypeWrapper, const char* methodName,
          typename Method, Method method, bool isContext>
struct GetterCallback;
// Implements the V8 callback function for calling a property getter method of a C++ class.

#define JSG_DEFINE_GETTER_CALLBACK_STRUCTS(...) \
    template <typename TypeWrapper, const char* methodName, typename T, typename Ret, \
              typename... Args, Ret (T::*method)(Args...) __VA_ARGS__, bool isContext> \
    struct GetterCallback<TypeWrapper, methodName, Ret (T::*)(Args...) __VA_ARGS__, method, \
                          isContext> { \
      static constexpr bool enumerable = true; \
      static void callback(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) { \
        liftKj(info, [&]() { \
          auto isolate = info.GetIsolate(); \
          auto context = isolate->GetCurrentContext(); \
          auto obj = info.This(); \
          auto& wrapper = TypeWrapper::from(isolate); \
          /* V8 no longer supports AccessorSignature, so we must manually verify `this`'s type. */\
          if (!isContext && !wrapper.template getTemplate(isolate, (T*)nullptr)->HasInstance(obj)) { \
            throwTypeError(isolate, "Illegal invocation"); \
          } \
          auto& self = extractInternalPointer<T, isContext>(context, obj); \
          return wrapper.wrap(context, obj, (self.*method)( \
              wrapper.unwrap(context, (kj::Decay<Args>*)nullptr)...)); \
        }); \
      } \
    }; \
    \
    template <typename TypeWrapper, const char* methodName, typename T, typename Ret, \
              typename... Args, \
              Ret (T::*method)(Lock&, Args...) __VA_ARGS__, \
              bool isContext> \
    struct GetterCallback<TypeWrapper, methodName, \
                          Ret (T::*)(Lock&, Args...) __VA_ARGS__, \
                          method, isContext> { \
      /* Specialization for methods that take `Lock&` as their first parameter. */ \
      static constexpr bool enumerable = true; \
      static void callback(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) { \
        liftKj(info, [&]() { \
          auto isolate = info.GetIsolate(); \
          auto context = isolate->GetCurrentContext(); \
          auto obj = info.This(); \
          auto& wrapper = TypeWrapper::from(isolate); \
          /* V8 no longer supports AccessorSignature, so we must manually verify `this`'s type. */\
          if (!isContext && !wrapper.template getTemplate(isolate, (T*)nullptr)->HasInstance(obj)) { \
            throwTypeError(isolate, "Illegal invocation"); \
          } \
          auto& self = extractInternalPointer<T, isContext>(context, obj); \
          return wrapper.wrap(context, obj, (self.*method)(Lock::from(isolate), \
              wrapper.unwrap(context, (kj::Decay<Args>*)nullptr)...)); \
        }); \
      } \
    }; \
    \
    template <typename TypeWrapper, const char* methodName, typename T, typename Ret, \
              typename... Args, \
              Ret (T::*method)(const v8::PropertyCallbackInfo<v8::Value>&, Args...) __VA_ARGS__, \
              bool isContext> \
    struct GetterCallback<TypeWrapper, methodName, \
                          Ret (T::*)(const v8::PropertyCallbackInfo<v8::Value>&, Args...) __VA_ARGS__, \
                          method, isContext> { \
      /* Specialization for methods that take `const v8::PropertyCallbackInfo<v8::Value>&` as \
       * their first parameter. */ \
      static constexpr bool enumerable = true; \
      static void callback(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) { \
        liftKj(info, [&]() { \
          auto isolate = info.GetIsolate(); \
          auto context = isolate->GetCurrentContext(); \
          auto obj = info.This(); \
          auto& wrapper = TypeWrapper::from(isolate); \
          /* V8 no longer supports AccessorSignature, so we must manually verify `this`'s type. */\
          if (!isContext && !wrapper.template getTemplate(isolate, (T*)nullptr)->HasInstance(obj)) { \
            throwTypeError(isolate, "Illegal invocation"); \
          } \
          auto& self = extractInternalPointer<T, isContext>(context, obj); \
          return wrapper.wrap(context, obj, (self.*method)(info, \
              wrapper.unwrap(context, (kj::Decay<Args>*)nullptr)...)); \
        }); \
      } \
    }; \
    \
    template <typename TypeWrapper, const char* propertyName, typename T, \
              Unimplemented (T::*method)() __VA_ARGS__, bool isContext> \
    struct GetterCallback<TypeWrapper, propertyName, Unimplemented (T::*)() __VA_ARGS__, method, \
                          isContext> { \
      static constexpr bool enumerable = false; \
      static void callback(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) { \
        scheduleUnimplementedPropertyError(info, typeid(T), propertyName); \
      } \
    };

JSG_DEFINE_GETTER_CALLBACK_STRUCTS()
JSG_DEFINE_GETTER_CALLBACK_STRUCTS(const)

#undef JSG_DEFINE_GETTER_CALLBACK_STRUCTS

template <typename TypeWrapper, const char* methodName,
          typename Method, Method method, bool isContext>
struct SetterCallback;
// Implements the V8 callback function for calling a property setter method of a C++ class.

template <typename TypeWrapper, const char* methodName, typename T, typename Arg,
          void (T::*method)(Arg), bool isContext>
struct SetterCallback<TypeWrapper, methodName, void (T::*)(Arg), method, isContext> {
  static void callback(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                       const v8::PropertyCallbackInfo<void>& info) {
    liftKj(info, [&]() {
      auto isolate = info.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = info.This();
      auto& wrapper = TypeWrapper::from(isolate);
      // V8 no longer supports AccessorSignature, so we must manually verify `this`'s type.
      if (!isContext && !wrapper.template getTemplate(isolate, (T*)nullptr)->HasInstance(obj)) {
        throwTypeError(isolate, "Illegal invocation");
      }
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      (self.*method)(wrapper.template unwrap<Arg>(context, value,
          TypeErrorContext::setterArgument(typeid(T), methodName)));
    });
  }
};

template <typename TypeWrapper, const char* methodName, typename T, typename Arg,
          void (T::*method)(Lock&, Arg), bool isContext>
struct SetterCallback<TypeWrapper, methodName,
                      void (T::*)(Lock&, Arg), method, isContext> {
  // Specialization for methods that take `Lock&` as their first parameter.

  static void callback(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                       const v8::PropertyCallbackInfo<void>& info) {
    liftKj(info, [&]() {
      auto isolate = info.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = info.This();
      auto& wrapper = TypeWrapper::from(isolate);
      // V8 no longer supports AccessorSignature, so we must manually verify `this`'s type.
      if (!isContext && !wrapper.template getTemplate(isolate, (T*)nullptr)->HasInstance(obj)) {
        throwTypeError(isolate, "Illegal invocation");
      }
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      (self.*method)(Lock::from(isolate), wrapper.template unwrap<Arg>(context, value,
          TypeErrorContext::setterArgument(typeid(T), methodName)));
    });
  }
};

template <typename TypeWrapper, const char* methodName, typename T, typename Arg,
          void (T::*method)(const v8::PropertyCallbackInfo<void>&, Arg), bool isContext>
struct SetterCallback<TypeWrapper, methodName,
                      void (T::*)(const v8::PropertyCallbackInfo<void>&, Arg), method, isContext> {
  // Specialization for methods that take `const v8::PropertyCallbackInfo<void>&` as their
  // first parameter.

  static void callback(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                       const v8::PropertyCallbackInfo<void>& info) {
    liftKj(info, [&]() {
      auto isolate = info.GetIsolate();
      auto context = isolate->GetCurrentContext();
      auto obj = info.This();
      auto& wrapper = TypeWrapper::from(isolate);
      // V8 no longer supports AccessorSignature, so we must manually verify `this`'s type.
      if (!isContext && !wrapper.template getTemplate(isolate, (T*)nullptr)->HasInstance(obj)) {
        throwTypeError(isolate, "Illegal invocation");
      }
      auto& self = extractInternalPointer<T, isContext>(context, obj);
      (self.*method)(info, wrapper.template unwrap<Arg>(context, value,
          TypeErrorContext::setterArgument(typeid(T), methodName)));
    });
  }
};

template <typename T, typename Constructor = decltype(&T::constructor)>
constexpr bool hasConstructorMethod(T*) {
  static_assert(!std::is_member_function_pointer<Constructor>::value,
      "JSG resource type `constructor` member functions must be static.");
  // TODO(cleanup): Write our own isMemberFunctionPointer and put it in KJ so we don't have to pull
  //   in <type_traits>. (See the "Motivation" section of Boost.CallableTraits for why I didn't just
  //   dive in and do it.)
  return true;
}
constexpr bool hasConstructorMethod(...) { return false; }
// SFINAE to detect if a type has a static method called `constructor`.

void exposeGlobalScopeType(v8::Isolate* isolate, v8::Local<v8::Context> context);
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

class NullConfiguration {
  // A configuration type that can be derived from any input type, because it contains nothing.
public:
  template <typename T>
  NullConfiguration(T&&) {}
};

template <typename TypeWrapper>
class DynamicResourceTypeMap {
  // TypeWrapper must list this type as its first superclass. The ResourceWrappers that it
  // subclasses will then be able to regsiter themselves in the map.

private:
  typedef void ReflectionInitializer(jsg::Object& object, TypeWrapper& wrapper);
  struct DynamicTypeInfo {
    v8::Local<v8::FunctionTemplate> tmpl;
    kj::Maybe<ReflectionInitializer&> reflectionInitializer;
  };

  DynamicTypeInfo getDynamicTypeInfo(
      v8::Isolate* isolate, const std::type_info& type) {
    KJ_IF_MAYBE(f, resourceTypeMap.find(std::type_index(type))) {
      return (**f)(static_cast<TypeWrapper&>(*this), isolate);
    } else {
      KJ_FAIL_REQUIRE(
          "cannot wrap object type that was not registered with JSG_DECLARE_ISOLATE_TYPE",
          typeName(type));
    }
  }

  typedef DynamicTypeInfo GetTypeInfoFunc(TypeWrapper&, v8::Isolate*);
  kj::HashMap<std::type_index, GetTypeInfoFunc*> resourceTypeMap;
  // Maps type_info values to functions that can be used to get the associated template. Used by
  // ResourceWrapper.

  template <typename, typename>
  friend class ResourceWrapper;
  template <typename>
  friend class ObjectWrapper;
};


template <typename TypeWrapper>
class ObjectWrapper {
  // Like ResourceWrapper for T = jsg::Object. We need some special-casing for this type.

public:
  static constexpr const std::type_info& getName(Object*) { return typeid(Object); }

  static constexpr const std::type_info& getName(Ref<Object>*) { return typeid(Object); }

  v8::Local<v8::Object> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator,
      Ref<Object>&& value) {
    // Wrap a value of type T.

    auto isolate = context->GetIsolate();

    KJ_IF_MAYBE(h, value->tryGetHandle(isolate)) {
      return *h;
    } else {
      auto& valueRef = *value;  // avoid compiler warning about typeid(*value) having side effects
      auto& type = typeid(valueRef);
      auto& wrapper = static_cast<TypeWrapper&>(*this);

      // In ResourceWrapper::wrap() we check if the value is a subclass. Here, we assume it is
      // always a subclass, because `jsg::Object` cannot be constructed directly.
      auto info = wrapper.getDynamicTypeInfo(isolate, type);
      v8::Local<v8::FunctionTemplate> tmpl = info.tmpl;
      KJ_IF_MAYBE(i, info.reflectionInitializer) {
        (*i)(*value, wrapper);
      }
      v8::Local<v8::Object> object = check(tmpl->InstanceTemplate()->NewInstance(context));
      value.attachWrapper(isolate, object);
      return object;
    }
  }

  // We do not support unwrapping Ref<Object>; use V8Ref<v8::Object> instead.
  kj::Maybe<Ref<Object>> tryUnwrap(
      v8::Local<v8::Context> context, v8::Local<v8::Value> handle, Ref<Object>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) = delete;
};

}  // namespace workerd::jsg
