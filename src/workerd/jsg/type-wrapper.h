// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// The TypeWrapper knows how to convert a variety of types between C++ and JavaScript.

#include "buffersource.h"
#include "function.h"
#include "jsvalue.h"
#include "resource.h"
#include "struct.h"
#include "util.h"
#include "value.h"
#include "web-idl.h"
#include "wrappable.h"

namespace workerd::jsg {

// True if there is an unwrap() overload which does *not* take a v8::Value to unwrap for this
// parameter type T. This is useful to identify types like TypeHandlers and v8::Isolate* which
// functions can declare they accept at the end of their parameter list, but which are not created
// from any particular JS value.
template <typename TypeWrapper, typename T, typename = void>
constexpr bool isValueLessParameter = false;
template <typename TypeWrapper, typename T>
constexpr bool isValueLessParameter<TypeWrapper,
    T,
    kj::VoidSfinae<decltype(kj::instance<TypeWrapper>().unwrap(
        kj::instance<v8::Local<v8::Context>>(), kj::instance<T*>()))>> = true;

// TypeWrapper mixin for V8 handles.
//
// This is just a trivial pass-through.
class V8HandleWrapper {
public:
  template <typename T, typename = kj::EnableIf<kj::canConvert<T, v8::Value>()>>
  static constexpr const std::type_info& getName(v8::Local<T>*) {
    return typeid(T);
  }

  template <typename T, typename = kj::EnableIf<kj::canConvert<T, v8::Value>()>>
  v8::Local<T> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      v8::Local<T> value) {
    return value;
  }

  kj::Maybe<v8::Local<v8::Value>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      v8::Local<v8::Value>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return handle;
  }

#define JSG_FOR_EACH_V8_VALUE_SUBCLASS(f)                                                          \
  f(ArrayBuffer) f(ArrayBufferView) f(TypedArray) f(DataView) f(Int8Array) f(Uint8Array)           \
      f(Uint8ClampedArray) f(Int16Array) f(Uint16Array) f(Int32Array) f(Uint32Array)               \
          f(Float32Array) f(Float64Array) f(Object) f(String) f(Function) f(WasmMemoryObject)      \
              f(BigInt)

  // Define a tryUnwrap() overload for each interesting subclass of v8::Value.
#define JSG_DEFINE_TRY_UNWRAP(type)                                                                \
  kj::Maybe<v8::Local<v8::type>> tryUnwrap(v8::Local<v8::Context> context,                         \
      v8::Local<v8::Value> handle, v8::Local<v8::type>*,                                           \
      kj::Maybe<v8::Local<v8::Object>> parentObject) {                                             \
    if (handle->Is##type()) {                                                                      \
      return handle.As<v8::type>();                                                                \
    }                                                                                              \
    return kj::none;                                                                               \
  }                                                                                                \
                                                                                                   \
  kj::Maybe<v8::Global<v8::type>> tryUnwrap(v8::Local<v8::Context> context,                        \
      v8::Local<v8::Value> handle, v8::Global<v8::type>*,                                          \
      kj::Maybe<v8::Local<v8::Object>> parentObject) {                                             \
    if (handle->Is##type()) {                                                                      \
      return v8::Global<v8::type>(context->GetIsolate(), handle.As<v8::type>());                   \
    }                                                                                              \
    return kj::none;                                                                               \
  }                                                                                                \
                                                                                                   \
  kj::Maybe<V8Ref<v8::type>> tryUnwrap(v8::Local<v8::Context> context,                             \
      v8::Local<v8::Value> handle, V8Ref<v8::type>*,                                               \
      kj::Maybe<v8::Local<v8::Object>> parentObject) {                                             \
    if (handle->Is##type()) {                                                                      \
      return V8Ref<v8::type>(context->GetIsolate(), handle.As<v8::type>());                        \
    }                                                                                              \
    return kj::none;                                                                               \
  }                                                                                                \
  template <typename T = v8::type, typename = decltype(&T::GetIdentityHash)>                       \
  kj::Maybe<HashableV8Ref<T>> tryUnwrap(v8::Local<v8::Context> context,                            \
      v8::Local<v8::Value> handle, HashableV8Ref<v8::type>*,                                       \
      kj::Maybe<v8::Local<v8::Object>> parentObject) {                                             \
    if (handle->Is##type()) {                                                                      \
      return HashableV8Ref<v8::type>(context->GetIsolate(), handle.As<v8::type>());                \
    }                                                                                              \
    return kj::none;                                                                               \
  }

  JSG_FOR_EACH_V8_VALUE_SUBCLASS(JSG_DEFINE_TRY_UNWRAP)

#undef JSG_DEFINE_TRY_UNWRAP
#undef JSG_FOR_EACH_V8_VALUE_SUBCLASS

  template <typename T, typename = kj::EnableIf<kj::canConvert<T, v8::Value>()>>
  static constexpr const std::type_info& getName(v8::Global<T>*) {
    return typeid(T);
  }

  template <typename T, typename = kj::EnableIf<kj::canConvert<T, v8::Value>()>>
  v8::Local<T> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      v8::Global<T> value) {
    return value.Get(context->GetIsolate());
  }

  kj::Maybe<v8::Global<v8::Value>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      v8::Global<v8::Value>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return v8::Global<v8::Value>(context->GetIsolate(), handle);
  }

  template <typename T, typename = kj::EnableIf<kj::canConvert<T, v8::Value>()>>
  static constexpr const std::type_info& getName(V8Ref<T>*) {
    return typeid(T);
  }

  template <typename T, typename = kj::EnableIf<kj::canConvert<T, v8::Value>()>>
  v8::Local<T> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, V8Ref<T> value) {
    return value.getHandle(context->GetIsolate());
  }

  kj::Maybe<V8Ref<v8::Value>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      V8Ref<v8::Value>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return V8Ref<v8::Value>(context->GetIsolate(), handle);
  }
};

class UnimplementedWrapper {
public:
  static constexpr const std::type_info& getName(Unimplemented*) {
    return typeid(Unimplemented);
  }

  v8::Local<v8::Value> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      Unimplemented value) = delete;
  kj::Maybe<Unimplemented> tryUnwrap(v8::Local<v8::Context> context,
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
//     v8::Local<v8::Value> wrap(v8::Local<v8::Context> jsContext,
//                               kj::Maybe<v8::Local<v8::Object>> creator,
//                               T cppValue);
//     kj::Maybe<T> tryUnwrap(v8::Local<v8::Context> jsContext, v8::Local<v8::Value> jsHandle,
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
    : public StructWrapper<Self, T, typename T::template JsgFieldWrappers<Self, T>> {
public:
  template <typename MetaConfiguration>
  TypeWrapperBase(MetaConfiguration& config) {}

  inline void initTypeWrapper() {}
  template <typename MetaConfiguration>
  void updateConfiguration(MetaConfiguration&& configuration) {}

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
      typename = kj::EnableIf<!sfinae<MetaConfiguration>((Extension<Self>*)nullptr)>>
  TypeWrapperBase(MetaConfiguration& config) {}

  template <typename MetaConfiguration,
      typename = kj::EnableIf<sfinae<MetaConfiguration>((Extension<Self>*)nullptr)>>
  TypeWrapperBase(MetaConfiguration& config, bool = false): Extension<Self>(config) {}

  void unwrap() = delete;  // extensions only implement tryUnwrap(), not unwrap()

  inline void initTypeWrapper() {}
  template <typename MetaConfiguration>
  void updateConfiguration(MetaConfiguration&& configuration) {}
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

  Configuration unwrap(v8::Local<v8::Context> context, Configuration*) {
    return configuration;
  }

  void tryUnwrap() = delete;
  void wrap() = delete;
  void newContext() = delete;
  void getTemplate() = delete;

  inline void initTypeWrapper() {}
  template <typename MetaConfiguration>
  void updateConfiguration(MetaConfiguration&& config) {
    configuration = kj::fwd<MetaConfiguration>(config);
  }

private:
  Configuration configuration;
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
//     kj::Maybe<T> tryUnwrap(v8::Local<v8::Context> jsContext, v8::Local<v8::Value> jsHandle,
//                            T* dummy, kj::Maybe<v8::Local<v8::Object>> parentObject);
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
                   public TypeWrapperBase<Self, T>...,
                   public PrimitiveWrapper,
                   public NameWrapper<Self>,
                   public StringWrapper,
                   public OptionalWrapper<Self>,
                   public LenientOptionalWrapper<Self>,
                   public MaybeWrapper<Self>,
                   public OneOfWrapper<Self>,
                   public ArrayWrapper<Self>,
                   public SequenceWrapper<Self>,
                   public GeneratorWrapper<Self>,
                   public ArrayBufferWrapper<Self>,
                   public DictWrapper<Self>,
                   public DateWrapper<Self>,
                   public BufferSourceWrapper<Self>,
                   public FunctionWrapper<Self>,
                   public PromiseWrapper<Self>,
                   public NonCoercibleWrapper<Self>,
                   public MemoizedIdentityWrapper<Self>,
                   public IdentifiedWrapper<Self>,
                   public SelfRefWrapper<Self>,
                   public ExceptionWrapper<Self>,
                   public ObjectWrapper<Self>,
                   public V8HandleWrapper,
                   public UnimplementedWrapper,
                   public JsValueWrapper<Self> {
  // TODO(soon): Should the TypeWrapper object be stored on the isolate rather than the context?

public:
  template <typename MetaConfiguration>
  TypeWrapper(v8::Isolate* isolate, MetaConfiguration&& configuration)
      : TypeWrapperBase<Self, T>(configuration)...,
        MaybeWrapper<Self>(configuration),
        PromiseWrapper<Self>(configuration) {
    isolate->SetData(1, this);
  }
  KJ_DISALLOW_COPY_AND_MOVE(TypeWrapper);

  void initTypeWrapper() {
    (TypeWrapperBase<Self, T>::initTypeWrapper(), ...);
  }

  template <typename MetaConfiguration>
  void updateConfiguration(MetaConfiguration&& configuration) {
    (TypeWrapperBase<Self, T>::updateConfiguration(kj::fwd<MetaConfiguration>(configuration)), ...);
    MaybeWrapper<Self>::updateConfiguration(kj::fwd<MetaConfiguration>(configuration));
    PromiseWrapper<Self>::updateConfiguration(kj::fwd<MetaConfiguration>(configuration));
  }

  static TypeWrapper& from(v8::Isolate* isolate) {
    return *reinterpret_cast<TypeWrapper*>(isolate->GetData(1));
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
  USING_WRAPPER(NameWrapper<Self>);
  USING_WRAPPER(StringWrapper);
  USING_WRAPPER(OptionalWrapper<Self>);
  USING_WRAPPER(LenientOptionalWrapper<Self>);
  USING_WRAPPER(MaybeWrapper<Self>);
  USING_WRAPPER(OneOfWrapper<Self>);
  USING_WRAPPER(ArrayWrapper<Self>);
  USING_WRAPPER(SequenceWrapper<Self>);
  USING_WRAPPER(GeneratorWrapper<Self>);
  USING_WRAPPER(ArrayBufferWrapper<Self>);
  USING_WRAPPER(DictWrapper<Self>);
  USING_WRAPPER(DateWrapper<Self>);
  USING_WRAPPER(BufferSourceWrapper<Self>);
  USING_WRAPPER(FunctionWrapper<Self>);
  USING_WRAPPER(PromiseWrapper<Self>);
  USING_WRAPPER(NonCoercibleWrapper<Self>);
  USING_WRAPPER(MemoizedIdentityWrapper<Self>);
  USING_WRAPPER(IdentifiedWrapper<Self>);
  USING_WRAPPER(SelfRefWrapper<Self>);
  USING_WRAPPER(ExceptionWrapper<Self>);
  USING_WRAPPER(ObjectWrapper<Self>);
  USING_WRAPPER(V8HandleWrapper);
  USING_WRAPPER(UnimplementedWrapper);
  USING_WRAPPER(JsValueWrapper<Self>);
#undef USING_WRAPPER

  template <typename U>
  class TypeHandlerImpl;

  template <typename U>
  static constexpr TypeHandlerImpl<U> TYPE_HANDLER_INSTANCE = TypeHandlerImpl<U>();

  template <typename U>
  static constexpr const char* getName(TypeHandler<U>*) {
    return "TypeHandler";
  }

  template <typename U>
  const TypeHandler<U>& unwrap(v8::Local<v8::Context>, TypeHandler<U>*) {
    // if you're here because of compiler error template garbage, you forgot to register
    // a type with JSG_DECLARE_ISOLATE_TYPE
    return TYPE_HANDLER_INSTANCE<U>;
  }

  static constexpr const char* getName(v8::Isolate**) {
    return "Isolate";
  }

  v8::Isolate* unwrap(v8::Local<v8::Context> context, v8::Isolate**) {
    return context->GetIsolate();
  }

  template <typename U>
  auto unwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      TypeErrorContext errorContext,
      kj::Maybe<v8::Local<v8::Object>> parentObject = kj::none) -> RemoveRvalueRef<U> {
    auto maybe = this->tryUnwrap(context, handle, (kj::Decay<U>*)nullptr, parentObject);
    KJ_IF_SOME(result, maybe) {
      return kj::fwd<RemoveMaybe<decltype(maybe)>>(result);
    } else {
      throwTypeError(
          context->GetIsolate(), errorContext, TypeWrapper::getName((kj::Decay<U>*)nullptr));
    }
  }

  // Helper for unwrapping function/method arguments correctly. Specifically, we need logic to
  // handle the case where the user passes in fewer arguments than the function has parameters.
  template <typename U>
  auto unwrap(v8::Local<v8::Context> context,
      const v8::FunctionCallbackInfo<v8::Value>& args,
      size_t parameterIndex,
      TypeErrorContext errorContext) -> RemoveRvalueRef<U> {
    using V = kj::Decay<U>;

    if constexpr (kj::isSameType<V, Varargs>()) {
      return Varargs(parameterIndex, args);
    } else if constexpr (isArguments<V>()) {
      using E = typename V::ElementType;
      size_t size = args.Length() >= parameterIndex ? args.Length() - parameterIndex : 0;
      auto builder = kj::heapArrayBuilder<E>(size);
      for (size_t i = parameterIndex; i < args.Length(); i++) {
        builder.add(unwrap<E>(context, args[i], errorContext));
      }
      return builder.finish();
    } else if constexpr (isValueLessParameter<Self, V>) {
      // C++ parameters which don't unwrap JS values, like v8::Isolate* and TypeHandlers.
      return unwrap(context, (V*)nullptr);
    } else {
      if constexpr (!webidl::isOptional<V> && !kj::isSameType<V, Unimplemented>()) {
        // TODO(perf): Better to perform this parameter index check once, at the unwrap<U>() call
        //   site. We'll need function length properties implemented correctly for that, most
        //   likely -- see EW-386.
        if (parameterIndex >= args.Length()) {
          // We're unwrapping a nonexistent argument into a required parameter. Since Web IDL
          // nullable types (Maybe<T>) can be initialized from `undefined`, we need to explicitly
          // throw here, or else `f(Maybe<T>)` could be called like `f()`.
          throwTypeError(context->GetIsolate(), errorContext, TypeWrapper::getName((V*)nullptr));
        }
      }

      // If we get here, we're either unwrapping into an optional or unimplemented parameter, in
      // which cases we're fine with nonexistent arguments implying `undefined`, or we have an
      // argument at this parameter index.
      return unwrap<U>(context, args[parameterIndex], errorContext);
    }
  }

  template <typename Holder, typename U>
  void initReflection(Holder* holder, PropertyReflection<U>& reflection) {
    reflection.self = holder;
    reflection.unwrapper = [](v8::Isolate* isolate, v8::Local<v8::Object> object,
                               kj::StringPtr name) -> kj::Maybe<U> {
      auto context = isolate->GetCurrentContext();
      auto value = jsg::check(object->Get(context, v8StrIntern(isolate, name)));
      if (value->IsUndefined()) {
        return kj::none;
      } else {
        // TypeErrorContext::structField() produces a pretty good error message for this case.
        return from(isolate).template unwrap<U>(
            context, value, TypeErrorContext::structField(typeid(Holder), name.cStr()), object);
      }
    };
  }

  template <typename Holder, typename... U>
  void initReflection(Holder* holder, PropertyReflection<U>&... reflections) {
    (initReflection(holder, reflections), ...);
  }
};

template <typename Self, typename... Types>
template <typename T>
class TypeWrapper<Self, Types...>::TypeHandlerImpl final: public TypeHandler<T> {
public:
  v8::Local<v8::Value> wrap(Lock& js, T value) const override {
    auto isolate = js.v8Isolate;
    auto context = js.v8Context();
    return TypeWrapper::from(isolate).wrap(context, kj::none, kj::mv(value));
  }

  kj::Maybe<T> tryUnwrap(Lock& js, v8::Local<v8::Value> handle) const override {
    auto isolate = js.v8Isolate;
    auto context = js.v8Context();
    return TypeWrapper::from(isolate).tryUnwrap(context, handle, (T*)nullptr, kj::none);
  }
};

}  // namespace workerd::jsg
