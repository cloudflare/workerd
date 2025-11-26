// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// The TypeWrapper knows how to convert a variety of types between C++ and JavaScript.

#include <workerd/jsg/buffersource.h>
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
#include <workerd/jsg/wrappable.h>
#include <workerd/util/autogate.h>

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
    : public StructWrapper<Self, T, typename T::template JsgFieldWrappers<Self, T>> {
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
                   public TypeWrapperBase<Self, T>...,
                   public NameWrapper<Self>,
                   public OptionalWrapper<Self>,
                   public LenientOptionalWrapper<Self>,
                   public MaybeWrapper<Self>,
                   public OneOfWrapper<Self>,
                   public ArrayWrapper<Self>,
                   public SetWrapper<Self>,
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
                   public JsValueWrapper<Self> {
  // TODO(soon): Should the TypeWrapper object be stored on the isolate rather than the context?
  bool fastApiEnabled = false;

 public:
  template <typename MetaConfiguration>
  TypeWrapper(v8::Isolate* isolate, MetaConfiguration&& configuration)
      : TypeWrapperBase<Self, T>(configuration)...,
        MaybeWrapper<Self>(configuration),
        GeneratorWrapper<Self>(configuration),
        PromiseWrapper<Self>(configuration) {
    isolate->SetData(SET_DATA_TYPE_WRAPPER, this);
    fastApiEnabled = util::Autogate::isEnabled(util::AutogateKey::V8_FAST_API);
  }
  KJ_DISALLOW_COPY_AND_MOVE(TypeWrapper);

  void initTypeWrapper() {
    (TypeWrapperBase<Self, T>::initTypeWrapper(), ...);
  }

  static TypeWrapper& from(v8::Isolate* isolate) {
    return *reinterpret_cast<TypeWrapper*>(isolate->GetData(SET_DATA_TYPE_WRAPPER));
  }

  bool isFastApiEnabled() const {
    return fastApiEnabled;
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

  USING_WRAPPER(NameWrapper<Self>);
  USING_WRAPPER(OptionalWrapper<Self>);
  USING_WRAPPER(LenientOptionalWrapper<Self>);
  USING_WRAPPER(MaybeWrapper<Self>);
  USING_WRAPPER(OneOfWrapper<Self>);
  USING_WRAPPER(ArrayWrapper<Self>);
  USING_WRAPPER(SetWrapper<Self>);
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
    auto maybe =
        this->tryUnwrap(js, context, handle, static_cast<kj::Decay<U>*>(nullptr), parentObject);
    KJ_IF_SOME(result, maybe) {
      return kj::fwd<RemoveMaybe<decltype(maybe)>>(result);
    } else {
      throwTypeError(
          js.v8Isolate, errorContext, TypeWrapper::getName(static_cast<kj::Decay<U>*>(nullptr)));
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
    return unwrap<U>(js, context, arg, errorContext);
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

    if constexpr (isArguments<V>()) {
      using E = typename V::ElementType;
      size_t size = args.Length() >= parameterIndex ? args.Length() - parameterIndex : 0;
      auto builder = kj::heapArrayBuilder<E>(size);
      for (size_t i = parameterIndex; i < args.Length(); i++) {
        builder.add(unwrap<E>(js, context, args[i], errorContext));
      }
      return builder.finish();
    } else if constexpr (ValueLessParameter<Self, V>) {
      // C++ parameters which don't unwrap JS values, like TypeHandlers or v8::FunctionCallbackInfo.
      return unwrap(js, context, static_cast<V*>(nullptr));
    } else {
      if constexpr (!webidl::isOptional<V> && !kj::isSameType<V, Unimplemented>()) {
        // TODO(perf): Better to perform this parameter index check once, at the unwrap<U>() call
        //   site. We'll need function length properties implemented correctly for that, most
        //   likely -- see EW-386.
        if (parameterIndex >= args.Length()) {
          // We're unwrapping a nonexistent argument into a required parameter. Since Web IDL
          // nullable types (Maybe<T>) can be initialized from `undefined`, we need to explicitly
          // throw here, or else `f(Maybe<T>)` could be called like `f()`.
          throwTypeError(
              js.v8Isolate, errorContext, TypeWrapper::getName(static_cast<V*>(nullptr)));
        }
      }

      // If we get here, we're either unwrapping into an optional or unimplemented parameter, in
      // which cases we're fine with nonexistent arguments implying `undefined`, or we have an
      // argument at this parameter index.
      return unwrap<U>(js, context, args[parameterIndex], errorContext);
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
        return from(isolate).template unwrap<U>(
            js, context, value, TypeErrorContext::structField(typeid(Holder), name.cStr()), object);
      }
    };
  }

  template <typename Holder, typename... U>
  void initReflection(Holder* holder, PropertyReflection<U>&... reflections) {
    (initReflection(holder, reflections), ...);
  }

  // ====================================================================================
  // Primitives
  static constexpr const char* getName(double*) {
    return "number";
  }

  v8::Local<v8::Number> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      double value) {
    return wrap(js.v8Isolate, creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, double value) {
    return v8::Number::New(isolate, value);
  }

  kj::Maybe<double> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      double*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return check(handle->ToNumber(context))->Value();
  }

  static constexpr const char* getName(int8_t*) {
    return "byte";
  }

  v8::Local<v8::Number> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      int8_t value) {
    return wrap(js.v8Isolate, creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, int8_t value) {
    return v8::Integer::New(isolate, value);
  }

  kj::Maybe<int8_t> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      int8_t*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto value = check(handle->ToNumber(context))->Value();

    JSG_REQUIRE(
        isFinite(value), TypeError, "The value cannot be converted because it is not an integer.");

    JSG_REQUIRE(
        value <= static_cast<int8_t>(kj::maxValue) && value >= static_cast<int8_t>(kj::minValue),
        TypeError,
        kj::str("Value out of range. Must be between ", static_cast<int8_t>(kj::minValue), " and ",
            static_cast<int8_t>(kj::maxValue), " (inclusive)."));

    return static_cast<int8_t>(value);
  }

  static constexpr const char* getName(uint8_t*) {
    return "octet";
  }

  v8::Local<v8::Number> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      uint8_t value) {
    return wrap(js.v8Isolate, creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, uint8_t value) {
    return v8::Integer::NewFromUnsigned(isolate, value);
  }

  kj::Maybe<uint8_t> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      uint8_t*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto value = check(handle->ToNumber(context))->Value();
    JSG_REQUIRE(
        isFinite(value), TypeError, "The value cannot be converted because it is not an integer.");

    JSG_REQUIRE(value >= 0, TypeError,
        "The value cannot be converted because it is negative and this "
        "API expects a positive number.");

    JSG_REQUIRE(value <= static_cast<uint8_t>(kj::maxValue), TypeError,
        kj::str("Value out of range. Must be less than or equal to ",
            static_cast<uint8_t>(kj::maxValue), "."));

    return static_cast<uint8_t>(value);
  }

  static constexpr const char* getName(int16_t*) {
    return "short integer";
  }

  v8::Local<v8::Number> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      int16_t value) {
    return wrap(js.v8Isolate, creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, int16_t value) {
    return v8::Number::New(isolate, value);
  }

  kj::Maybe<int16_t> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      int16_t*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto value = check(handle->ToNumber(context))->Value();

    JSG_REQUIRE(
        isFinite(value), TypeError, "The value cannot be converted because it is not an integer.");

    JSG_REQUIRE(
        value <= static_cast<int16_t>(kj::maxValue) && value >= static_cast<int16_t>(kj::minValue),
        TypeError,
        kj::str("Value out of range. Must be between ", static_cast<int16_t>(kj::minValue), " and ",
            static_cast<int16_t>(kj::maxValue), " (inclusive)."));

    return static_cast<int16_t>(value);
  }

  static constexpr const char* getName(uint16_t*) {
    return "unsigned short integer";
  }

  v8::Local<v8::Number> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      uint16_t value) {
    return wrap(js.v8Isolate, creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, uint16_t value) {
    return v8::Integer::NewFromUnsigned(isolate, value);
  }

  kj::Maybe<uint16_t> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      uint16_t*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto value = check(handle->ToNumber(context))->Value();
    JSG_REQUIRE(
        isFinite(value), TypeError, "The value cannot be converted because it is not an integer.");

    JSG_REQUIRE(value >= 0, TypeError,
        "The value cannot be converted because it is negative and this "
        "API expects a positive number.");

    JSG_REQUIRE(value <= static_cast<uint16_t>(kj::maxValue), TypeError,
        kj::str("Value out of range. Must be less than or equal to ",
            static_cast<uint16_t>(kj::maxValue), "."));

    return static_cast<uint16_t>(value);
  }

  static constexpr const char* getName(int*) {
    return "integer";
  }

  v8::Local<v8::Number> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      int value) {
    return wrap(js.v8Isolate, creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, int value) {
    return v8::Number::New(isolate, value);
  }

  kj::Maybe<int> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      int*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (int num; handle->IsInt32() && handle->Int32Value(context).To(&num)) {
      return num;
    }

    auto value = check(handle->ToNumber(context))->Value();
    if (!isFinite(value)) {
      return 0;
    }

    // One would think that RangeError is more appropriate than TypeError,
    // but WebIDL says it should be TypeError.
    JSG_REQUIRE(value <= static_cast<int>(kj::maxValue) && value >= static_cast<int>(kj::minValue),
        TypeError,
        kj::str("Value out of range. Must be between ", static_cast<int>(kj::minValue), " and ",
            static_cast<int>(kj::maxValue), " (inclusive)."));

    return static_cast<int>(value);
  }

  static constexpr const char* getName(uint32_t*) {
    return "unsigned integer";
  }

  v8::Local<v8::Number> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      uint32_t value) {
    return wrap(js.v8Isolate, creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, uint32_t value) {
    return v8::Integer::NewFromUnsigned(isolate, value);
  }

  kj::Maybe<uint32_t> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      uint32_t*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (uint32_t num; handle->IsUint32() && handle->Uint32Value(context).To(&num)) {
      return num;
    }

    auto value = check(handle->ToNumber(context))->Value();
    JSG_REQUIRE(
        isFinite(value), TypeError, "The value cannot be converted because it is not an integer.");

    JSG_REQUIRE(value >= 0, TypeError,
        "The value cannot be converted because it is negative and this "
        "API expects a positive number.");

    JSG_REQUIRE(value <= static_cast<uint32_t>(kj::maxValue), TypeError,
        kj::str("Value out of range. Must be less than or equal to ",
            static_cast<uint32_t>(kj::maxValue), "."));

    return static_cast<uint32_t>(value);
  }

  static constexpr const char* getName(uint64_t*) {
    return "bigint";
  }

  v8::Local<v8::BigInt> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      uint64_t value) {
    return wrap(js.v8Isolate, creator, value);
  }

  v8::Local<v8::BigInt> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, uint64_t value) {
    return v8::BigInt::New(isolate, value);
  }

  kj::Maybe<uint64_t> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      uint64_t*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (v8::Local<v8::BigInt> bigint;
        handle->IsBigInt() && handle->ToBigInt(context).ToLocal(&bigint)) {
      bool lossless;
      auto value = bigint->Uint64Value(&lossless);
      JSG_REQUIRE(lossless, TypeError,
          "The value cannot be converted because it is either negative and this "
          "API expects a positive bigint, or the value would be truncated.");
      return value;
    }

    auto value = check(handle->ToNumber(context))->Value();
    JSG_REQUIRE(
        isFinite(value), TypeError, "The value cannot be converted because it is not an integer.");

    JSG_REQUIRE(value >= 0, TypeError,
        "The value cannot be converted because it is negative and this "
        "API expects a positive bigint.");

    JSG_REQUIRE(value <= static_cast<uint64_t>(kj::maxValue), TypeError,
        kj::str("Value out of range. Must be less than or equal to ",
            static_cast<uint64_t>(kj::maxValue), "."));

    return static_cast<uint64_t>(value);
  }

  static constexpr const char* getName(int64_t*) {
    return "bigint";
  }

  v8::Local<v8::BigInt> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      int64_t value) {
    return wrap(js.v8Isolate, creator, value);
  }

  v8::Local<v8::BigInt> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, int64_t value) {
    return v8::BigInt::New(isolate, value);
  }

  kj::Maybe<int64_t> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      int64_t*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (v8::Local<v8::BigInt> bigint;
        handle->IsBigInt() && handle->ToBigInt(context).ToLocal(&bigint)) {
      bool lossless;
      auto value = bigint->Int64Value(&lossless);
      JSG_REQUIRE(
          lossless, TypeError, "The value cannot be converted because it would be truncated.");
      return value;
    }

    auto value = check(handle->ToNumber(context))->Value();
    JSG_REQUIRE(
        isFinite(value), TypeError, "The value cannot be converted because it is not an integer.");

    JSG_REQUIRE(
        value <= static_cast<int64_t>(kj::maxValue) && value >= static_cast<int64_t>(kj::minValue),
        TypeError,
        kj::str("Value out of range. Must be between ", static_cast<int64_t>(kj::minValue), " and ",
            static_cast<int64_t>(kj::maxValue), " (inclusive)."));

    return static_cast<int64_t>(value);
  }

  static constexpr const char* getName(bool*) {
    return "boolean";
  }

  template <typename U, typename = kj::EnableIf<kj::isSameType<U, bool>()>>
  v8::Local<v8::Boolean> wrap(
      Lock& js, v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, U value) {
    // The template is needed to prevent this overload from being chosen for arbitrary types that
    // can convert to bool, such as pointers.
    return wrap(js.v8Isolate, creator, value);
  }

  template <typename U, typename = kj::EnableIf<kj::isSameType<U, bool>()>>
  v8::Local<v8::Boolean> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, U value) {
    // The template is needed to prevent this overload from being chosen for arbitrary types that
    // can convert to bool, such as pointers.
    return v8::Boolean::New(isolate, value);
  }

  kj::Maybe<bool> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      bool*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return handle->ToBoolean(js.v8Isolate)->Value();
  }

  // ====================================================================================
  // Strings
  // TODO(someday): The conversion to kj::String doesn't explicitly consider the distinction
  // between DOMString (~ WTF-8; could contain invalid code points) and USVString (invalid code
  // points are always replaced with U+FFFD). Code should make an explict choice between the two.

  template <typename U>
    requires(kj::isSameType<U, kj::ArrayPtr<const char>>() ||
        kj::isSameType<U, kj::Array<const char>>() || kj::isSameType<U, kj::String>())
  static constexpr const char* getName(U*) {
    return "string";
  }

  template <typename U>
    requires(kj::isSameType<U, ByteString>() || kj::isSameType<U, USVString>() ||
        kj::isSameType<U, DOMString>())
  static constexpr const char* getName(U*) {
    if constexpr (kj::isSameType<U, ByteString>()) {
      return "ByteString";
    } else if constexpr (kj::isSameType<U, USVString>()) {
      return "USVString";
    } else if constexpr (kj::isSameType<U, DOMString>()) {
      return "DOMString";
    }
    KJ_UNREACHABLE;
  }

  v8::Local<v8::String> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, kj::StringPtr value) {
    return v8Str(isolate, value);
  }

  v8::Local<v8::String> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      kj::ArrayPtr<const char> value) {
    return v8Str(js.v8Isolate, value);
  }

  v8::Local<v8::String> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      kj::Array<const char> value) {
    return wrap(js, context, creator, value.asPtr());
  }

  template <typename U>
    requires(kj::isSameType<U, const ByteString&>() || kj::isSameType<U, const USVString&>() ||
        kj::isSameType<U, const DOMString&>())
  v8::Local<v8::String> wrap(
      Lock& js, v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, U value) {
    // TODO(cleanup): Move to a HeaderStringWrapper in the api directory.
    return v8Str(js.v8Isolate, value.asPtr());
  }

  template <typename U>
    requires(kj::isSameType<U, kj::String>() || kj::isSameType<U, ByteString>() ||
        kj::isSameType<U, USVString>() || kj::isSameType<U, DOMString>())
  kj::Maybe<U> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      U*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // Note that if handle is already a string, calling ToString will just
    // return handle without any further coercion. For any other type of
    // value, v8 will try to coerce it into a string. So there is no need
    // for us to check if handle is a string here or not, ToString does
    // that for us.
    JsString str(check(handle->ToString(context)));
    if constexpr (kj::isSameType<U, kj::String>()) {
      return str.toString(js);
    } else if constexpr (kj::isSameType<U, ByteString>()) {
      return str.toByteString(js);
    } else if constexpr (kj::isSameType<U, USVString>()) {
      return str.toUSVString(js);
    } else if constexpr (kj::isSameType<U, DOMString>()) {
      return str.toDOMString(js);
    }

    KJ_UNREACHABLE;
  }

  // ====================================================================================
  // Unimplemented
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

  // ====================================================================================
  // V8 Handles
  template <V8Value U>
  static constexpr const std::type_info& getName(v8::Local<U>*) {
    return typeid(U);
  }

  template <V8Value U>
  v8::Local<U> wrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      v8::Local<U> value) {
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
              f(WasmMemoryObject) f(BigInt)

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
  template <typename U = v8::type, typename = decltype(&U::GetIdentityHash)>                       \
  kj::Maybe<HashableV8Ref<U>> tryUnwrap(jsg::Lock& js, v8::Local<v8::Context> context,             \
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

  template <V8Value U>
  static constexpr const std::type_info& getName(v8::Global<U>*) {
    return typeid(U);
  }

  template <V8Value U>
  v8::Local<U> wrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      v8::Global<U> value) {
    return value.Get(js.v8Isolate);
  }

  kj::Maybe<v8::Global<v8::Value>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      v8::Global<v8::Value>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return v8::Global<v8::Value>(js.v8Isolate, handle);
  }

  template <V8Value U>
  static constexpr const std::type_info& getName(V8Ref<U>*) {
    return typeid(U);
  }

  template <V8Value U>
  v8::Local<U> wrap(jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      V8Ref<U> value) {
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

template <typename Self, typename... Types>
template <typename T>
class TypeWrapper<Self, Types...>::TypeHandlerImpl final: public TypeHandler<T> {
 public:
  v8::Local<v8::Value> wrap(Lock& js, T value) const override {
    auto isolate = js.v8Isolate;
    auto context = js.v8Context();
    return TypeWrapper::from(isolate).wrap(js, context, kj::none, kj::mv(value));
  }

  kj::Maybe<T> tryUnwrap(Lock& js, v8::Local<v8::Value> handle) const override {
    auto isolate = js.v8Isolate;
    auto context = js.v8Context();
    return TypeWrapper::from(isolate).tryUnwrap(
        js, context, handle, static_cast<T*>(nullptr), kj::none);
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
  };                                                                                               \
  class Type final: public ::workerd::jsg::Isolate<Type##_TypeWrapper> {                           \
   public:                                                                                         \
    using ::workerd::jsg::Isolate<Type##_TypeWrapper>::Isolate;                                    \
  }

}  // namespace workerd::jsg
