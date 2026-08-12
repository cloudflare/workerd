// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// The TypeWrapper knows how to convert a variety of types between C++ and JavaScript.

#include <workerd/jsg/dom-exception.h>
#include <workerd/jsg/function.h>
#include <workerd/jsg/iterator.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/jsvalue.h>
#include <workerd/jsg/resource.h>
#include <workerd/jsg/struct.h>
#include <workerd/jsg/util.h>
#include <workerd/jsg/value.h>
#include <workerd/jsg/web-idl.h>
#include <workerd/jsg/wrappable-tag.h>
#include <workerd/jsg/wrappable.h>

#include <v8-wasm.h>

namespace workerd::jsg {

// True if there is an unwrap() overload which does *not* take a v8::Value to unwrap for this
// parameter type T. This is useful to identify types like TypeHandlers and v8::Isolate* which
// functions can declare they accept at the end of their parameter list, but which are not created
// from any particular JS value.
// A concept that identifies types that can be unwrapped without needing a JS value
template <typename TypeWrapper, typename T>
concept ValueLessParameter =
    requires(TypeWrapper wrapper, Lock& js, v8::Local<v8::Context> context, T* ptr) {
      wrapper.unwrap(js, context, ptr);
    };

// =======================================================================================
// RequiredArgCount_ specialization — counts leading required JS-visible arguments.
//
// This completes the definition of requiredArgumentCount<TypeWrapper, T> declared in meta.h.
// It lives here (rather than meta.h or web-idl.h) because it needs the ValueLessParameter
// concept above to automatically detect ALL injected parameter types — TypeHandler<T>,
// InjectConfiguration<T> (e.g. CompatibilityFlags::Reader), and any future valueless types.
//
// Arguments<T> (variadic rest args) is detected separately via isArguments<>() because it
// does not satisfy ValueLessParameter — it consumes remaining JS arguments rather than being
// injected by the runtime.
//
// Template instantiation of requiredArgumentCount happens from resource.h templates, which
// are only instantiated after this header is fully parsed, so these specializations are
// guaranteed to be visible at the point of use.
namespace detail {

template <typename TypeWrapper>
struct RequiredArgCount_<TypeWrapper, TypeList<>> {
  static constexpr int value = 0;
};

template <typename TypeWrapper, typename Head, typename... Tail>
struct RequiredArgCount_<TypeWrapper, TypeList<Head, Tail...>> {
  using D = kj::Decay<Head>;
  static constexpr int value = (isArguments<D>() || ValueLessParameter<TypeWrapper, D>)
      ? RequiredArgCount_<TypeWrapper, TypeList<Tail...>>::value  // skip injected args
      : (webidl::isOptional<D> ? 0  // optional arg — stop counting required
                               : 1 + RequiredArgCount_<TypeWrapper, TypeList<Tail...>>::value);
};

}  // namespace detail

// TypeWrapper mixin for V8 handles.
//
// This is just a trivial pass-through.
class V8HandleWrapper {
 public:
  template <V8Value T>
  static constexpr const std::type_info& getName(v8::Local<T>*) {
    return typeid(T);
  }

  template <V8Value T>
  v8::Local<T> wrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      v8::Local<T> value) {
    return value;
  }

  kj::Maybe<v8::Local<v8::Value>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      v8::Local<v8::Value>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return handle;
  }

#define JSG_FOR_EACH_V8_VALUE_SUBCLASS(f)                                                          \
  f(ArrayBuffer) f(ArrayBufferView) f(TypedArray) f(DataView) f(Int8Array) f(Uint8Array)           \
      f(Uint8ClampedArray) f(Int16Array) f(Uint16Array) f(Int32Array) f(Uint32Array)               \
          f(Float16Array) f(Float32Array) f(Float64Array) f(Object) f(String) f(Function)          \
              f(WasmMemoryObject) f(WasmModuleObject) f(BigInt)

  // Define a tryUnwrap() overload for each interesting subclass of v8::Value.
#define JSG_DEFINE_TRY_UNWRAP(type)                                                                \
  kj::Maybe<v8::Local<v8::type>> tryUnwrap(jsg::Lock& js, v8::Local<v8::Context> context,          \
      v8::Local<v8::Value> handle, v8::Local<v8::type>*,                                           \
      kj::Maybe<v8::Local<v8::Object>> parentObject) {                                             \
    if (handle->Is##type()) {                                                                      \
      return handle.As<v8::type>();                                                                \
    }                                                                                              \
    return kj::none;                                                                               \
  }                                                                                                \
                                                                                                   \
  kj::Maybe<v8::Global<v8::type>> tryUnwrap(jsg::Lock& js, v8::Local<v8::Context> context,         \
      v8::Local<v8::Value> handle, v8::Global<v8::type>*,                                          \
      kj::Maybe<v8::Local<v8::Object>> parentObject) {                                             \
    if (handle->Is##type()) {                                                                      \
      return v8::Global<v8::type>(js.v8Isolate, handle.As<v8::type>());                            \
    }                                                                                              \
    return kj::none;                                                                               \
  }                                                                                                \
                                                                                                   \
  kj::Maybe<V8Ref<v8::type>> tryUnwrap(jsg::Lock& js, v8::Local<v8::Context> context,              \
      v8::Local<v8::Value> handle, V8Ref<v8::type>*,                                               \
      kj::Maybe<v8::Local<v8::Object>> parentObject) {                                             \
    if (handle->Is##type()) {                                                                      \
      return V8Ref<v8::type>(js.v8Isolate, handle.As<v8::type>());                                 \
    }                                                                                              \
    return kj::none;                                                                               \
  }                                                                                                \
  template <typename T = v8::type, typename = decltype(&T::GetIdentityHash)>                       \
  kj::Maybe<HashableV8Ref<T>> tryUnwrap(jsg::Lock& js, v8::Local<v8::Context> context,             \
      v8::Local<v8::Value> handle, HashableV8Ref<v8::type>*,                                       \
      kj::Maybe<v8::Local<v8::Object>> parentObject) {                                             \
    if (handle->Is##type()) {                                                                      \
      return HashableV8Ref<v8::type>(js.v8Isolate, handle.As<v8::type>());                         \
    }                                                                                              \
    return kj::none;                                                                               \
  }

  JSG_FOR_EACH_V8_VALUE_SUBCLASS(JSG_DEFINE_TRY_UNWRAP)

#undef JSG_DEFINE_TRY_UNWRAP
#undef JSG_FOR_EACH_V8_VALUE_SUBCLASS

  template <V8Value T>
  static constexpr const std::type_info& getName(v8::Global<T>*) {
    return typeid(T);
  }

  template <V8Value T>
  v8::Local<T> wrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      v8::Global<T> value) {
    return value.Get(js.v8Isolate);
  }

  kj::Maybe<v8::Global<v8::Value>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      v8::Global<v8::Value>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return v8::Global<v8::Value>(js.v8Isolate, handle);
  }

  template <V8Value T>
  static constexpr const std::type_info& getName(V8Ref<T>*) {
    return typeid(T);
  }

  template <V8Value T>
  v8::Local<T> wrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      V8Ref<T> value) {
    return value.getHandle(js.v8Isolate);
  }

  kj::Maybe<V8Ref<v8::Value>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      V8Ref<v8::Value>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return V8Ref<v8::Value>(js.v8Isolate, handle);
  }
};

class UnimplementedWrapper {
 public:
  static constexpr const std::type_info& getName(Unimplemented*) {
    return typeid(Unimplemented);
  }

  v8::Local<v8::Value> wrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      Unimplemented value) = delete;
  kj::Maybe<Unimplemented> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Unimplemented*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // Can only be `undefined`.
    if (handle->IsUndefined()) {
      return Unimplemented();
    } else {
      return kj::none;
    }
  }
};

// A type T is SelfConvertible if it defines static wrap() and tryUnwrap() methods that handle
// its own conversion to/from JavaScript values. This lets value types opt into JSG type
// conversion without being registered in JSG_DECLARE_ISOLATE_TYPE or needing a dedicated
// wrapper mixin.
//
// The static methods receive the TypeWrapper instance (as auto&) so they can recursively
// convert inner types if needed.
//
// To opt in, define:
//
//     struct MyType {
//       static v8::Local<v8::Value> jsgWrap(auto& typeWrapper, Lock& js,
//           v8::Local<v8::Context> context,
//           kj::Maybe<v8::Local<v8::Object>> creator, MyType value);
//       static kj::Maybe<MyType> jsgTryUnwrap(auto& typeWrapper, Lock& js,
//           v8::Local<v8::Context> context,
//           v8::Local<v8::Value> handle,
//           kj::Maybe<v8::Local<v8::Object>> parentObject);
//     };
//
// If a SelfConvertible type appears in JSG-visible signatures (method parameters or return
// types, JSG_STRUCT fields, etc.), it also needs an RTTI representation for TypeScript type
// generation. Declare `using JsgRttiDelegate = ...;` to describe the type to RTTI as some
// existing type; see the delegated-RTTI support in rtti.h.
template <typename T>
concept SelfConvertible = requires(Lock& js,
    v8::Local<v8::Context> ctx,
    kj::Maybe<v8::Local<v8::Object>> creator,
    T value,
    v8::Local<v8::Value> handle,
    kj::Maybe<v8::Local<v8::Object>> parent,
    int& dummyWrapper) {
  {
    T::jsgWrap(dummyWrapper, js, ctx, creator, kj::mv(value))
  } -> std::convertible_to<v8::Local<v8::Value>>;
  { T::jsgTryUnwrap(dummyWrapper, js, ctx, handle, parent) } -> std::same_as<kj::Maybe<T>>;
};

// TypeWrapper mixin for SelfConvertible types.
//
// Detects types that define their own static jsgWrap()/jsgTryUnwrap() methods and delegates to
// them. Uses CRTP to pass the full TypeWrapper as the first argument, giving the type access to
// the complete overload set for recursive conversion of inner types.
template <typename TypeWrapper>
class SelfUnwrap {
 public:
  template <SelfConvertible T>
  static constexpr const std::type_info& getName(T*) {
    return typeid(T);
  }

  template <SelfConvertible T>
  v8::Local<v8::Value> wrap(
      Lock& js, v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, T value) {
    return T::jsgWrap(static_cast<TypeWrapper&>(*this), js, context, creator, kj::mv(value));
  }

  template <SelfConvertible T>
  kj::Maybe<T> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      T*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return T::jsgTryUnwrap(static_cast<TypeWrapper&>(*this), js, context, handle, parentObject);
  }
};

// The application can use this type to extend TypeWrapper with its own custom mixins. The
// template `Extension` is a mixin which will be inherited by the TypeWrapper. It will be passed
// the full TypeWrapper specialization as a type parameter. See TypeWrapper, below, for an
// explanation of the mixin design and use of CRTP.
//
// Specify `TypeWrapperExtension` in the same list as your API types. Example:
//
//     template <typename TypeWrapper>
//     class MyMixin {
//     public:
//       // ... implementation ...
//     };
//
//     JSG_DECLARE_ISOLATE_TYPE(MyIsolate, MyApiType1, MyApiType2,
//         jsg::TypeWrapperExtension<MyMixin>, ...)
//
// The extension mixin must declare the following methods:
//
//     static constexpr const char* getName(T* dummy);
//     v8::Local<v8::Value> wrap(jsg::Lock& js, v8::Local<v8::Context> jsContext,
//                               kj::Maybe<v8::Local<v8::Object>> creator,
//                               T cppValue);
//     kj::Maybe<T> tryUnwrap(Lock& js, v8::Local<v8::Context> jsContext, v8::Local<v8::Value> jsHandle,
//                            T* dummy, kj::Maybe<v8::Local<v8::Object>> parentObject);
//
//     Ref<T, v8::Context> newContext(v8::Isolate* isolate, T* dummy, Args&&... args);
//     template <bool isContext = false>
//     v8::Local<v8::FunctionTemplate> getTemplate(v8::Isolate* isolate, T*)
//
// Note that most mixins do not actually need the last two methods. Unfortunately, due to
// limitation of the C++ `using` directive, we can't easily make these optional. You can,
// however, declare them deleted, like:
//
//     void newContext() = delete;
//     void getTemplate() = delete;
//
// The mixin's constructor can optionally accept a configuration value as its parameter, which
// works the same way as the second parameter to `JSG_RESOURCE_TYPE`.
template <template <typename TypeWrapper> typename Extension>
class TypeWrapperExtension {
 public:
  static const JsgKind JSG_KIND = JsgKind::EXTENSION;
};

// Include this type in the FFI type list to implement auto-injection of a parameter type based
// on configuration. `Configuration` must be a type that can be constructed from the isolate's
// meta configuration object. Wrapped functions will be able to accept `Configuration` as a
// parameter type, and instead of being converted from a JavaScript parameter, it will instead
// receive the isolate-global configuration.
//
// `Configuration` can be a reference type.
template <typename Configuration>
class InjectConfiguration {
 public:
  static const JsgKind JSG_KIND = JsgKind::EXTENSION;
};

// Selects the appropriate mixin to support wrapping/unwrapping type T, which is one of the API
// types passed to JSG_DECLARE_ISOLATE_TYPE() by the application.
template <typename Self, typename T, JsgKind kind = T::JSG_KIND>
class TypeWrapperBase;

// Specialization of TypeWrapperBase for types that have a JSG_RESOURCE_TYPE block.
template <typename Self, typename T>
class TypeWrapperBase<Self, T, JsgKind::RESOURCE>: public ResourceWrapper<Self, T> {
 public:
  template <typename MetaConfiguration>
  TypeWrapperBase(MetaConfiguration& config): ResourceWrapper<Self, T>(config) {}

  void unwrap() = delete;  // ResourceWrapper only implements tryUnwrap(), not unwrap()
};

// Specialization of TypeWrapperBase for types that have a JSG_STRUCT block.
template <typename Self, typename T>
class TypeWrapperBase<Self, T, JsgKind::STRUCT>
    : public StructWrapper<Self, T, typename T::template JsgFields<T>> {
 public:
  template <typename MetaConfiguration>
  TypeWrapperBase(MetaConfiguration& config) {}

  inline void initTypeWrapper() {}

  void unwrap() = delete;  // StructWrapper only implements tryUnwrap(), not unwrap()
};

// Specialization of TypeWrapperBase for TypeWrapperExtension.
template <typename Self, template <typename> typename Extension>
class TypeWrapperBase<Self, TypeWrapperExtension<Extension>, JsgKind::EXTENSION>
    : public Extension<Self> {
  template <typename MetaConfiguration>
  static constexpr bool sfinae(decltype(Extension<Self>(kj::instance<MetaConfiguration&>()))*) {
    return true;  // extension constructor takes configuration argument
  }
  template <typename MetaConfiguration>
  static constexpr bool sfinae(...) {
    return false;  // extension constructor does not take arguments
  }

 public:
  template <typename MetaConfiguration,
      typename = kj::EnableIf<!sfinae<MetaConfiguration>(static_cast<Extension<Self>*>(nullptr))>>
  TypeWrapperBase(MetaConfiguration& config) {}

  template <typename MetaConfiguration,
      typename = kj::EnableIf<sfinae<MetaConfiguration>(static_cast<Extension<Self>*>(nullptr))>>
  TypeWrapperBase(MetaConfiguration& config, bool = false): Extension<Self>(config) {}

  void unwrap() = delete;  // extensions only implement tryUnwrap(), not unwrap()

  inline void initTypeWrapper() {}
};

// Specialization of TypeWrapperBase for InjectConfiguration.
template <typename Self, typename Configuration>
class TypeWrapperBase<Self, InjectConfiguration<Configuration>, JsgKind::EXTENSION> {
 public:
  template <typename MetaConfiguration>
  TypeWrapperBase(MetaConfiguration& config): configuration(kj::fwd<MetaConfiguration>(config)) {}

  static constexpr const char* getName(kj::Decay<Configuration>*) {
    return "Configuration";
  }

  Configuration unwrap(Lock& js, v8::Local<v8::Context> context, Configuration*) {
    return configuration;
  }

  void tryUnwrap() = delete;
  void wrap() = delete;
  void newContext() = delete;
  void getTemplate() = delete;

  inline void initTypeWrapper() {}

 private:
  Configuration configuration;
};

// Base class of `TypeWrapper` holding the members that don't depend on the list of registered
// API types.
// These could all live directly in `TypeWrapper`, but `TypeWrapper`'s own template argument list
// can be quite long since it names every API type registered with the isolate. That list ends up in
// the mangled name and the DWARF linkage name of every member instantiation, which can result in
// tens of megabytes of symbol names and debug info. Hoisting them into a base parameterized only on
// the most-derived type keeps those names small. See also the comment on `TypeWrapper::from()`.
// `TypeWrapper` re-exports these with using-declarations, so callers don't see the difference.
template <typename Self>
class TypeWrapperOps {
 public:
  template <typename T>
  class TypeHandlerImpl;

  // The `TypeHandler` singleton for each type. These have static storage duration, so pointers
  // to them remain valid forever.
  template <typename T>
  static constexpr TypeHandlerImpl<T> TYPE_HANDLER_INSTANCE = TypeHandlerImpl<T>();

  template <typename U, typename Func>
  static void forEachTypeHandlerImpl(Func& func) {
    if constexpr (U::JSG_KIND == JsgKind::RESOURCE) {
      func(typeid(TypeHandler<Ref<U>>),
          static_cast<const TypeHandler<Ref<U>>*>(&TYPE_HANDLER_INSTANCE<Ref<U>>));
    }
  }

  template <typename U>
  static constexpr const char* getName(TypeHandler<U>*) {
    return "TypeHandler";
  }

  template <typename U>
  const TypeHandler<U>& unwrap(Lock& js, v8::Local<v8::Context>, TypeHandler<U>*) {
    // if you're here because of compiler error template garbage, you forgot to register
    // a type with JSG_DECLARE_ISOLATE_TYPE
    return TYPE_HANDLER_INSTANCE<U>;
  }

  template <typename U>
  kj::Maybe<const TypeHandler<U>&> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      TypeHandler<U>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // TypeHandler is not a value that needs to be unwrapped from JS
    return TYPE_HANDLER_INSTANCE<U>;
  }

  template <typename U>
  auto unwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      TypeErrorContext errorContext,
      kj::Maybe<v8::Local<v8::Object>> parentObject = kj::none) -> RemoveRvalueRef<U> {
    // Dispatch through `Self&` (not the `TypeWrapper<Self, ...>` base) so that the `self` parameter
    // of the wrapper methods will be the short-named final type. See the comment on
    // `TypeWrapper::from()` for why this matters to debug info size.
    auto& self = static_cast<Self&>(*this);
    auto maybe =
        self.tryUnwrap(js, context, handle, static_cast<kj::Decay<U>*>(nullptr), parentObject);
    KJ_IF_SOME(result, maybe) {
      return kj::fwd<RemoveMaybe<decltype(maybe)>>(result);
    } else {
      throwTypeError(
          js.v8Isolate, errorContext, Self::getName(static_cast<kj::Decay<U>*>(nullptr)));
    }
  }

  template <typename U, FastApiPrimitive A>
  auto unwrapFastApi(
      jsg::Lock& js, v8::Local<v8::Context> context, A& arg, TypeErrorContext errorContext) -> A {
    return arg;
  }

  template <typename U>
  auto unwrapFastApi(jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value>& arg,
      TypeErrorContext errorContext) -> RemoveRvalueRef<U> {
    return static_cast<Self&>(*this).template unwrap<U>(js, context, arg, errorContext);
  }

  // Helper for unwrapping function/method arguments correctly. Specifically, we need logic to
  // handle the case where the user passes in fewer arguments than the function has parameters.
  template <typename U>
  auto unwrap(Lock& js,
      v8::Local<v8::Context> context,
      const v8::FunctionCallbackInfo<v8::Value>& args,
      size_t parameterIndex,
      TypeErrorContext errorContext) -> RemoveRvalueRef<U> {
    using V = kj::Decay<U>;

    // Dispatch through `Self&` so that wrapper methods see the short-named final type; see the
    // comment on `TypeWrapper::from()`.
    auto& self = static_cast<Self&>(*this);

    if constexpr (isArguments<V>()) {
      using E = V::ElementType;
      size_t size = args.Length() >= parameterIndex ? args.Length() - parameterIndex : 0;
      auto builder = kj::heapArrayBuilder<E>(size);
      for (size_t i = parameterIndex; i < args.Length(); i++) {
        builder.add(self.template unwrap<E>(js, context, args[i], errorContext));
      }
      return builder.finish();
    } else if constexpr (ValueLessParameter<Self, V>) {
      // C++ parameters which don't unwrap JS values, like TypeHandlers or v8::FunctionCallbackInfo.
      return self.unwrap(js, context, static_cast<V*>(nullptr));
    } else {
      if constexpr (!webidl::OptionalType<V> && !kj::isSameType<V, Unimplemented>()) {
        // TODO(perf): Better to perform this parameter index check once, at the unwrap<U>() call
        //   site. We'll need function length properties implemented correctly for that, most
        //   likely -- see EW-386.
        if (parameterIndex >= args.Length()) {
          // We're unwrapping a nonexistent argument into a required parameter. Since Web IDL
          // nullable types (Maybe<T>) can be initialized from `undefined`, we need to explicitly
          // throw here, or else `f(Maybe<T>)` could be called like `f()`.
          throwTypeError(js.v8Isolate, errorContext, Self::getName(static_cast<V*>(nullptr)));
        }
      }

      // If we get here, we're either unwrapping into an optional or unimplemented parameter, in
      // which cases we're fine with nonexistent arguments implying `undefined`, or we have an
      // argument at this parameter index.
      return self.template unwrap<U>(js, context, args[parameterIndex], errorContext);
    }
  }

  template <typename Holder, typename U>
  void initReflection(Holder* holder, PropertyReflection<U>& reflection) {
    reflection.self = holder;
    reflection.unwrapper = [](v8::Isolate* isolate, v8::Local<v8::Object> object,
                               kj::StringPtr name) -> kj::Maybe<U> {
      auto context = isolate->GetCurrentContext();
      auto& js = Lock::from(isolate);
      auto value = jsg::check(object->Get(context, v8StrIntern(isolate, name)));
      if (value->IsUndefined()) {
        return kj::none;
      } else {
        // TypeErrorContext::structField() produces a pretty good error message for this case.
        return Self::from(isolate).template unwrap<U>(
            js, context, value, TypeErrorContext::structField(typeid(Holder), name.cStr()), object);
      }
    };
  }

  template <typename Holder, typename... U>
  void initReflection(Holder* holder, PropertyReflection<U>&... reflections) {
    (initReflection(holder, reflections), ...);
  }
};

// The TypeWrapper class aggregates functionality to convert between C++ values and JavaScript
// values. It primarily implements two methods:
//
//     v8::Local<v8::Value> wrap(v8::Local<v8::Context> jsContext,
//                               kj::Maybe<v8::Local<v8::Object>> creator
//                               T cppValue);
//     // Converts cppValue to JavaScript.
//     //
//     // `creator` is non-null when converting the return value of a method; in this case,
//     // `creator` is the object on which the method was called. This is useful for some types
//     // (like Promises) where the KJ convention is to assume that the creator must outlive the
//     // returned object.
//
//     T unwrap<T>(v8::Local<v8::Context> jsContext, v8::Local<v8::Value> jsHandle);
//     // Converts jsValue to C++, expecting type T.
//
// The design is based on mixins: TypeWrapper derives from classes that handle each individual
// type. Each mixin is expected to implement the following methods:
//
//     static constexpr const char* getName(T* dummy);
//     // Return the name of the type for the purpose of TypeError exception messages. Note that
//     // you can also return `const std::type_info&` here, in which case the type name will
//     // be derived by stripping off the namespace from the C++ type name.
//
//     v8::Local<v8::Value> wrap(v8::Local<v8::Context> jsContext,
//                               kj::Maybe<v8::Local<v8::Object>> creator,
//                               T cppValue);
//     // Converts cppValue to JavaScript.
//
//     kj::Maybe<T> tryUnwrap(Lock& js, v8::Local<v8::Context> jsContext,
//                            v8::Local<v8::Value> jsHandle, T* dummy,
//                            kj::Maybe<v8::Local<v8::Object>> parentObject);
//     // Converts jsValue to C++, expecting type T. If the input is not of type T, returns
//     // null. If we're unwrapping a field of an object, then `parentObject` is the handle to
//     // the object; this is useful when unwrapping a function, to bind `this`.
//     //
//     // Note that only a shallow type check is performed. E.g. if a struct type is expected,
//     // tryUnwrap() will only return null if the input is not a JS Object. If it is an object,
//     // but one of its fields is the wrong type, tryUnwrap() will throw a TypeError. The idea
//     // here is that `tryUnwrap()` should only do the amount of type checking that one would
//     // typically do in JavaScript to distinguish a variant type (e.g. "string or number").
//     // Typically this is limited to what you can do with the `typeof` and `instanceof`
//     // keywords on the top-level value.
//
// Note the `dummy` parameters of type T*. These will always be passed `nullptr`. The purpose of
// these parameters is to select the correct overload for the desired type. Normally, one would
// use an explicit template parameter for this, but that only works if all the methods are
// actually specializations of the same template method declaration. That's not the case here,
// because we're inheriting totally independent method declarations from all our mixins. So, we
// have to slum it by passing `(T*)nullptr` as an argument purely for overload selection.
//
// Note that many of these mixins need to call back to the TypeWrapper recursively. For example,
// OptionalWrapper (for Optional<T>) will need to call back to unwrap the inner T. To that end,
// we use the Curiously Recurring Template Pattern, passing the TypeWrapper type itself to its
// superclasses, so that they can cast themselves back to the subclass type and call it
// recursively. See:
//
//     https://en.wikipedia.org/wiki/Curiously_recurring_template_pattern
//
// Actually, TypeWrapper itself *also* takes itself as a template parameter called `Self`. This
// is primarily done as a trick in order to make compiler error messages less difficult to read.
// The `Self` parameter to `TypeWrapper` is actually a specific subclass of TypeWrapper. See
// JSG_DECLARE_ISOLATE_TYPE in setup.h.
//
// Note that a pointer to the TypeWrapper object is stored in the V8 context's "embedder data",
// in slot 1, so that we can get back to it from V8 callbacks.
template <typename Self, typename... T>
class TypeWrapper: public DynamicResourceTypeMap<Self>,
                   public TypeWrapperOps<Self>,
                   public TypeWrapperBase<Self, T>...,
                   public PrimitiveWrapper,
                   public NameWrapper,
                   public StringWrapper,
                   public OptionalWrapper<Self>,
                   public LenientOptionalWrapper<Self>,
                   public MaybeWrapper<Self>,
                   public OneOfWrapper<Self>,
                   public ArrayWrapper<Self>,
                   public SetWrapper<Self>,
                   public SequenceWrapper,
                   public GeneratorWrapper<Self>,
                   public ArrayBufferWrapper,
                   public DictWrapper,
                   public DateWrapper,
                   public FunctionWrapper<Self>,
                   public PromiseWrapper<Self>,
                   public NonCoercibleWrapper<Self>,
                   public MemoizedIdentityWrapper<Self>,
                   public IdentifiedWrapper<Self>,
                   public SelfRefWrapper,
                   public ExceptionWrapper<Self>,
                   public ObjectWrapper<Self>,
                   public SelfUnwrap<Self>,
                   public V8HandleWrapper,
                   public UnimplementedWrapper,
                   public JsValueWrapper {
  // TODO(soon): Should the TypeWrapper object be stored on the isolate rather than the context?
 public:
  template <typename MetaConfiguration>
  TypeWrapper(v8::Isolate* isolate, MetaConfiguration&& configuration)
      : TypeWrapperBase<Self, T>(configuration)...,
        MaybeWrapper<Self>(configuration),
        GeneratorWrapper<Self>(configuration),
        PromiseWrapper<Self>(configuration),
        config(getConfig(configuration)) {
    isolate->SetData(SET_DATA_TYPE_WRAPPER, this);
  }
  KJ_DISALLOW_COPY_AND_MOVE(TypeWrapper);

  void initTypeWrapper() {
    (TypeWrapperBase<Self, T>::initTypeWrapper(), ...);
  }

  static Self& from(v8::Isolate* isolate) {
    // Return a reference typed as the most-derived `Self` (e.g. `Foo_TypeWrapper`) rather than the
    // `TypeWrapper<Self, ...>` base. Both refer to the same object -- the `TypeWrapper` base is at
    // offset 0 within `Self` -- but the spelling matters enormously for debug info size.
    //
    // The type used for various wrapper mixins when calling unwrap is whatever the static type of
    // the object expression is at the call site. If callers hold a `TypeWrapper<Self, ...>&`, then
    // the fully-spelled base type -- which lists every API type registered with the isolate -- gets
    // baked into the DWARF name (`DW_AT_name`/`DW_AT_linkage_name`) of every wrapper method
    // instantiation. That string is ~20KB and is repeated across thousands of instantiations,
    // adding tens of MB to `.debug_str`. Using the short-named `Self` keeps those names tiny.
    return *reinterpret_cast<Self*>(isolate->GetData(SET_DATA_TYPE_WRAPPER));
  }

  bool isFastApiEnabled() const {
    return config.fastApiEnabled;
  }

  using TypeWrapperBase<Self, T>::getName...;
  using TypeWrapperBase<Self, T>::wrap...;
  using TypeWrapperBase<Self, T>::newContext...;
  using TypeWrapperBase<Self, T>::unwrap...;
  using TypeWrapperBase<Self, T>::tryUnwrap...;
  using TypeWrapperBase<Self, T>::getTemplate...;

#define USING_WRAPPER(Name)                                                                        \
  using Name::getName;                                                                             \
  using Name::wrap;                                                                                \
  using Name::tryUnwrap

  USING_WRAPPER(PrimitiveWrapper);
  USING_WRAPPER(NameWrapper);
  USING_WRAPPER(StringWrapper);
  USING_WRAPPER(OptionalWrapper<Self>);
  USING_WRAPPER(LenientOptionalWrapper<Self>);
  USING_WRAPPER(MaybeWrapper<Self>);
  USING_WRAPPER(OneOfWrapper<Self>);
  USING_WRAPPER(ArrayWrapper<Self>);
  USING_WRAPPER(SetWrapper<Self>);
  USING_WRAPPER(SequenceWrapper);
  USING_WRAPPER(GeneratorWrapper<Self>);
  USING_WRAPPER(ArrayBufferWrapper);
  USING_WRAPPER(DictWrapper);
  USING_WRAPPER(DateWrapper);
  USING_WRAPPER(FunctionWrapper<Self>);
  USING_WRAPPER(PromiseWrapper<Self>);
  USING_WRAPPER(NonCoercibleWrapper<Self>);
  USING_WRAPPER(MemoizedIdentityWrapper<Self>);
  USING_WRAPPER(IdentifiedWrapper<Self>);
  USING_WRAPPER(SelfRefWrapper);
  USING_WRAPPER(SelfUnwrap<Self>);
  USING_WRAPPER(ExceptionWrapper<Self>);
  USING_WRAPPER(ObjectWrapper<Self>);
  USING_WRAPPER(V8HandleWrapper);
  USING_WRAPPER(UnimplementedWrapper);
  USING_WRAPPER(JsValueWrapper);
#undef USING_WRAPPER

  using TypeWrapperOps<Self>::getName;
  using TypeWrapperOps<Self>::unwrap;
  using TypeWrapperOps<Self>::tryUnwrap;
  using TypeWrapperOps<Self>::unwrapFastApi;
  using TypeWrapperOps<Self>::initReflection;
  using TypeWrapperOps<Self>::TYPE_HANDLER_INSTANCE;
  using TypeWrapperOps<Self>::forEachTypeHandlerImpl;

  // Invokes func(const std::type_info&, const TypeHandler<Ref<T>>*) for every resource
  // type registered with this TypeWrapper. The handler instances are the constexpr
  // TYPE_HANDLER_INSTANCE singletons, so the pointers remain valid forever.
  //
  // Only resource types participate: their handlers always support both wrap and
  // tryUnwrap. Struct (and other value) types cannot be registered eagerly because
  // instantiating a TypeHandler requires BOTH directions to compile, and some registered
  // structs are deliberately one-directional (e.g. output-only structs with no unwrap
  // path, or input-only structs with Unimplemented members whose wrap is deleted).
  // Value types continue to obtain handlers via TypeHandler<T> parameter injection,
  // which only instantiates handlers for types that actually support it. Extension and
  // configuration entries are likewise skipped (they are not themselves wrappable).
  //
  // Used by jsg::Isolate to populate the isolate-wide registry backing
  // jsg::Lock::tryGetTypeHandler(); see setup.h.
  template <typename Func>
  static void forEachTypeHandler(Func&& func) {
    (TypeWrapperOps<Self>::template forEachTypeHandlerImpl<T>(func), ...);
  }

  // === Per-type CppHeapPointerTag numbering ===================================================
  //
  // Assign every resource type registered with this isolate a dense id via DFS pre-order over the
  // JSG_INHERIT forest, so that a type and all its subclasses occupy a contiguous id interval.
  // `wrappableTag<U>()` is U's own tag (used at Wrap time); `wrappableTagRange<U>()` is the
  // [U, last-subclass-of-U] interval (used at Unwrap time to accept U or any subclass).
  //
  // Only resource types (JSG_KIND == RESOURCE) are numbered. Non-resource wrappables share
  // kNonResourceWrappableTag and are handled separately by their unwrap sites.
  //
  // The whole assignment is computed in a single consteval pass over a per-type metadata table,
  // rather than as a web of recursive templates: extracting each type's parent and kind is O(N)
  // instantiations, and all the DFS arithmetic then runs inside one interpreted constexpr loop with
  // no further instantiation (we have to be careful about workers-api.c++ compile times).

  static constexpr size_t kNumTypes = sizeof...(T);

  // Position of U in the pack T..., or kNumTypes if U is not in the pack (e.g. Object, which is the
  // forest root and is never itself wrapped).
  template <typename U>
  static consteval size_t typeIndex() {
    size_t index = kNumTypes;
    size_t i = 0;
    ((kj::isSameType<U, T>() ? (index = i, void()) : void(), ++i), ...);
    return index;
  }

  // Per-type metadata, one entry per pack member, used by the numbering pass.
  struct TypeMeta {
    // Index into the pack of this type's JSG superclass, or kNumTypes if the parent is Object (a
    // forest root) or this type is not a resource.
    size_t parentIndex = 0;
    bool isResource = false;
  };

  // Metadata for every pack member. Building this is the only per-type template work: each entry
  // reads the type's JSG_KIND and, for resources, locates its jsgSuper in the pack.
  static constexpr auto kTypeMeta = []() {
    kj::FixedArray<TypeMeta, kNumTypes> meta{};
    size_t i = 0;
    ([&]<typename U>() {
      if constexpr (U::JSG_KIND == JsgKind::RESOURCE) {
        using Parent = U::jsgSuper;
        if constexpr (kj::isSameType<Parent, Object>()) {
          // A forest root: Object is never itself wrapped, so it has no index in the pack.
          meta[i] = TypeMeta{.parentIndex = kNumTypes, .isResource = true};
        } else {
          // typeIndex() also returns kNumTypes for a type that is simply absent from the pack, so
          // without this check an unregistered JSG superclass would be indistinguishable from
          // Object and silently make this type a forest root. Its tag would then fall outside the
          // subtree range of every registered ancestor, and the first inherited method dispatched
          // through one of them would abort at runtime.
          static_assert(typeIndex<Parent>() < kNumTypes,
              "this JSG resource type's superclass (its jsgSuper, i.e. the JSG_RESOURCE_TYPE of "
              "its C++ base class) is not registered in this isolate's JSG_DECLARE_ISOLATE_TYPE "
              "list; register it, or correct the type's inheritance");
          meta[i] = TypeMeta{.parentIndex = typeIndex<Parent>(), .isResource = true};
        }
      } else {
        meta[i] = TypeMeta{.parentIndex = kNumTypes, .isResource = false};
      }
      ++i;
    }.template operator()<T>(), ...);
    return meta;
  }();

  // DFS pre-order ids and subtree sizes for every resource type, computed in one pass over
  // kTypeMeta. Non-resource entries are left at zero and never consulted.
  struct Numbering {
    kj::FixedArray<uint16_t, kNumTypes> preorderId;
    kj::FixedArray<uint16_t, kNumTypes> subtreeSize;
  };

  static constexpr Numbering kNumbering = []() {
    Numbering n{};

    // The pack order (JSG_DECLARE_ISOLATE_TYPE registration order) is arbitrary: a resource's JSG
    // superclass may appear either before or after it. So neither pass below may assume a parent
    // precedes its children in index order. Both instead process resources in order of increasing
    // depth in the jsgSuper forest, which guarantees every parent is handled before its children
    // regardless of pack order. Depth is bounded by the inheritance depth (small), so the passes
    // are O(kNumTypes * maxDepth) with no fixed-point iteration.
    kj::FixedArray<uint16_t, kNumTypes> depth{};
    uint16_t maxDepth = 0;
    for (size_t i = 0; i < kNumTypes; ++i) {
      if (!kTypeMeta[i].isResource) continue;
      uint16_t d = 0;
      for (size_t p = kTypeMeta[i].parentIndex; p != kNumTypes; p = kTypeMeta[p].parentIndex) {
        ++d;
      }
      depth[i] = d;
      if (d > maxDepth) maxDepth = d;
    }

    // subtreeSize(U) = 1 + sum of subtreeSize over direct resource children of U. Fold deepest
    // resources into their parents first, so a parent's total is complete before it is itself folded
    // into its own parent.
    for (size_t i = 0; i < kNumTypes; ++i) {
      n.subtreeSize[i] = kTypeMeta[i].isResource ? 1 : 0;
    }
    for (size_t level = maxDepth; level-- > 0;) {
      for (size_t i = 0; i < kNumTypes; ++i) {
        if (!kTypeMeta[i].isResource || depth[i] != level + 1) continue;
        size_t parent = kTypeMeta[i].parentIndex;
        n.subtreeSize[parent] = static_cast<uint16_t>(n.subtreeSize[parent] + n.subtreeSize[i]);
      }
    }

    // preorder(U) = preorder(parent) + 1 + (subtree space of earlier siblings), where siblings are
    // resources with the same parent; roots (parent == Object) start at base 0. Assigning by
    // increasing depth ensures preorder(parent) is set before any child reads it. Sibling order is
    // pack order among nodes sharing a parent, tracked by the per-parent cursor `nextChildOffset`.
    kj::FixedArray<uint16_t, kNumTypes> nextChildOffset{};  // per-parent accumulated sibling space
    uint16_t nextRootOffset = 0;                            // sibling space among forest roots
    for (size_t level = 0; level <= maxDepth; ++level) {
      for (size_t i = 0; i < kNumTypes; ++i) {
        if (!kTypeMeta[i].isResource || depth[i] != level) continue;
        size_t parent = kTypeMeta[i].parentIndex;
        if (parent == kNumTypes) {
          n.preorderId[i] = nextRootOffset;
          nextRootOffset = static_cast<uint16_t>(nextRootOffset + n.subtreeSize[i]);
        } else {
          n.preorderId[i] =
              static_cast<uint16_t>(n.preorderId[parent] + 1 + nextChildOffset[parent]);
          nextChildOffset[parent] =
              static_cast<uint16_t>(nextChildOffset[parent] + n.subtreeSize[i]);
        }
      }
    }

    return n;
  }();

  // U's own tag: its pre-order id offset past the reserved non-resource tag.
  template <typename U>
  static consteval v8::CppHeapPointerTag wrappableTag() {
    static_assert(U::JSG_KIND == JsgKind::RESOURCE);
    constexpr uint16_t tag = kFirstResourceTag + kNumbering.preorderId[typeIndex<U>()];
    // If this fires, this isolate registers more wrappable types than the freelist bucket array in
    // HeapTracer can index. Raise kMaxWrappableTags in wrappable-tag.h; see the comment there.
    static_assert(wrappableTagBucketIndex(tag) < kMaxWrappableTags,
        "too many JSG resource types for this isolate; raise kMaxWrappableTags");
    return static_cast<v8::CppHeapPointerTag>(tag);
  }

  // The tag range accepted when unwrapping into a receiver of static type U: U itself through the
  // last id in U's subtree. subtreeSize includes U, so the last id is preorder(U) +
  // subtreeSize(U) - 1.
  template <typename U>
  static consteval v8::CppHeapPointerTagRange wrappableTagRange() {
    static_assert(U::JSG_KIND == JsgKind::RESOURCE);
    constexpr size_t idx = typeIndex<U>();
    uint16_t first = kFirstResourceTag + kNumbering.preorderId[idx];
    uint16_t last = first + kNumbering.subtreeSize[idx] - 1;
    return v8::CppHeapPointerTagRange(
        static_cast<v8::CppHeapPointerTag>(first), static_cast<v8::CppHeapPointerTag>(last));
  }

 private:
  const JsgConfig config;
};

// Implementation of the abstract `TypeHandler<T>` interface, which converts between `T` and
// JavaScript by dispatching back through the TypeWrapper.
template <typename Self>
template <typename T>
class TypeWrapperOps<Self>::TypeHandlerImpl final: public TypeHandler<T> {
 public:
  v8::Local<v8::Value> wrap(Lock& js, T value) const override {
    return Self::from(js.v8Isolate).wrap(js, js.v8Context(), kj::none, kj::mv(value));
  }

  kj::Maybe<T> tryUnwrap(Lock& js, v8::Local<v8::Value> handle) const override {
    return Self::from(js.v8Isolate)
        .tryUnwrap(js, js.v8Context(), handle, static_cast<T*>(nullptr), kj::none);
  }
};

// This macro helps cut down on template spam in error messages. Instead of instantiating Isolate
// directly, do:
//
//     JSG_DECLARE_ISOLATE_TYPE(MyIsolate, SomeApiType, AnotherApiType, ...);
//
// `MyIsolate` becomes your custom Isolate type, which will support wrapping all of the listed
// API types.
#define JSG_DECLARE_ISOLATE_TYPE(Type, ...)                                                        \
  class Type##_TypeWrapper;                                                                        \
  using Type##_TypeWrapperBase =                                                                   \
      ::workerd::jsg::TypeWrapper<Type##_TypeWrapper, jsg::DOMException, ##__VA_ARGS__>;           \
  class Type##_TypeWrapper final: public Type##_TypeWrapperBase {                                  \
   public:                                                                                         \
    [[maybe_unused]] static constexpr bool trackCallCounts = false;                                \
    using Type##_TypeWrapperBase::TypeWrapper;                                                     \
    /* Re-export the wrapper overload sets into the most-derived class. `TypeWrapper::from()` */   \
    /* returns `Self&` (this class) so that the TypeWrapper type used in wrapper mixins maps to */ \
    /* this short-named type rather than the full `TypeWrapper<Self, ...all types...>` base */     \
    /* (which would bloat DWARF .debug_str). These using-declarations re-home the base's using- */ \
    /* declared, non-deducing-this overloads onto this class, giving them an identity object- */   \
    /* argument match. Without this, those overloads would lose to deducing-this overloads */      \
    /* (which are not yet used in JSG yet but will be introduced soon and always deduce an */      \
    /* identity `self`) and overload resolution would differ from calling through the base. */     \
    using Type##_TypeWrapperBase::wrap;                                                            \
    using Type##_TypeWrapperBase::tryUnwrap;                                                       \
    using Type##_TypeWrapperBase::getName;                                                         \
    using Type##_TypeWrapperBase::unwrap;                                                          \
  };                                                                                               \
  class Type final: public ::workerd::jsg::Isolate<Type##_TypeWrapper> {                           \
   public:                                                                                         \
    using ::workerd::jsg::Isolate<Type##_TypeWrapper>::Isolate;                                    \
  }

#define JSG_DECLARE_DEBUG_ISOLATE_TYPE(Type, ...)                                                  \
  class Type##_TypeWrapper;                                                                        \
  using Type##_TypeWrapperBase =                                                                   \
      ::workerd::jsg::TypeWrapper<Type##_TypeWrapper, jsg::DOMException, ##__VA_ARGS__>;           \
  class Type##_TypeWrapper final: public Type##_TypeWrapperBase {                                  \
   public:                                                                                         \
    [[maybe_unused]] static constexpr bool trackCallCounts = true;                                 \
    using Type##_TypeWrapperBase::TypeWrapper;                                                     \
    /* See the comment in JSG_DECLARE_ISOLATE_TYPE for why these are re-exported. */               \
    using Type##_TypeWrapperBase::wrap;                                                            \
    using Type##_TypeWrapperBase::tryUnwrap;                                                       \
    using Type##_TypeWrapperBase::getName;                                                         \
    using Type##_TypeWrapperBase::unwrap;                                                          \
  };                                                                                               \
  class Type final: public ::workerd::jsg::Isolate<Type##_TypeWrapper> {                           \
   public:                                                                                         \
    using ::workerd::jsg::Isolate<Type##_TypeWrapper>::Isolate;                                    \
  }

}  // namespace workerd::jsg
