// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Runtime type system for jsg.
// Produces capnp description (rtti.capnp) of jsg structs, resources and c++ types of their
// members.
// Can be used to generate typescript type, dynamically invoke methods, fuzz, check backward
// compatibility etc.

#include <capnp/message.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/rtti.capnp.h>

namespace workerd::jsg::rtti {

namespace impl {

template <typename T, typename Enable = void>
struct BuildRtti;
// Struct for partial specialization.

} // namespace impl

class Builder {
  // User's entry point into rtti.
  // Builder owns capnp builder for all the objects it returns, so usual capnp builder
  // rules apply.

public:
  template<typename T>
  Type::Reader type() {
    auto type = builder.initRoot<Type>();
    impl::BuildRtti<T>::build(type);
    return type;
  }

  template<typename T, typename MetaConfiguration>
  Structure::Reader structure(MetaConfiguration&& config) {
    auto structure = builder.initRoot<Structure>();
    impl::BuildRtti<T>::build(structure, kj::fwd<MetaConfiguration>(config));
    return structure;
  }

private:
  capnp::MallocMessageBuilder builder;
};

namespace impl {

// Implementation tools

template <typename Function, typename = void>
struct FunctionTraits;

template <typename R, typename... Args>
struct FunctionTraits<R(Args...)> {
  using ReturnType = R;
  using ArgsTuple = std::tuple<Args...>;
};

template <typename R, typename... Args>
struct FunctionTraits<R(*)(Args...)> {
  using ReturnType = R;
  using ArgsTuple = std::tuple<Args...>;
};

template <typename This, typename R, typename... Args>
struct FunctionTraits<R(This::*)(Args...)> {
  using ReturnType = R;
  using ArgsTuple = std::tuple<Args...>;
};

template <typename T>
struct FunctionTraits<T, std::void_t<decltype(&T::operator())>>
: public FunctionTraits<decltype(&T::operator())> { };

template <typename This, typename R, typename... Args>
struct FunctionTraits<R(This::*)(Args...) const> {
  using ReturnType = R;
  using ArgsTuple = std::tuple<Args...>;
};

template<typename Tuple>
struct TupleRttiBuilder {
  static inline void build(capnp::List<Type>::Builder builder) {
    build(std::make_integer_sequence<size_t, std::tuple_size_v<Tuple>>{}, builder);
  }

private:
  template<size_t...Indexes>
  static inline void build(std::integer_sequence<size_t, Indexes...> seq,
                        capnp::List<Type>::Builder builder) {
    ((buildIndex<Indexes>(builder)), ...);
  }

  template<size_t I>
  static inline void buildIndex(capnp::List<Type>::Builder builder) {
    BuildRtti<std::tuple_element_t<I, Tuple>>::build(builder[I]);
  }
};


// Primitives

template<>
struct BuildRtti<void> {
  static void build(Type::Builder builder) { builder.setVoidt(); }
};

template<>
struct BuildRtti<bool> {
  static void build(Type::Builder builder) { builder.setBoolt(); }
};

template<>
struct BuildRtti<v8::Value> {
  static void build(Type::Builder builder) { builder.setUnknown(); }
};

// Numbers

#define DECLARE_NUMBER_TYPE(T) \
template<> \
struct BuildRtti<T> { \
  static void build(Type::Builder builder) { builder.initNumber().setName(#T); } \
};

#define FOR_EACH_NUMBER_TYPE(F) \
  F(char) \
  F(signed char) \
  F(unsigned char) \
  F(short) \
  F(unsigned short) \
  F(int) \
  F(unsigned int) \
  F(long) \
  F(unsigned long) \
  F(double)

FOR_EACH_NUMBER_TYPE(DECLARE_NUMBER_TYPE)

#undef FOR_EACH_NUMBER_TYPE
#undef DECLARE_NUMBER_TYPE

// Strings

#define DECLARE_STRING_TYPE(T) \
template<> \
struct BuildRtti<T> { \
  static void build(Type::Builder builder) { builder.initString().setName(#T); } \
};

#define FOR_EACH_STRING_TYPE(F) \
  F(kj::String) \
  F(kj::StringPtr) \
  F(v8::String) \
  F(ByteString) \
  F(UsvString) \
  F(UsvStringPtr)

FOR_EACH_STRING_TYPE(DECLARE_STRING_TYPE)

#undef FOR_EACH_STRING_TYPE
#undef DECLARE_STRING_TYPE

// Object Types

template<>
struct BuildRtti<v8::Object> {
  static void build(Type::Builder builder) { builder.setObject(); }
};

// References

template<typename T>
struct BuildRtti<Ref<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder); }
};

template<typename T>
struct BuildRtti<V8Ref<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder); }
};

template<typename T>
struct BuildRtti<HashableV8Ref<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder); }
};

template<typename T>
struct BuildRtti<v8::Local<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder); }
};

template<typename T>
struct BuildRtti<v8::Global<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder); }
};

template<typename T>
struct BuildRtti<jsg::MemoizedIdentity<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder); }
};

template<typename T>
struct BuildRtti<jsg::Identified<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder); }
};

// Generic Types

template<typename T>
struct BuildRtti<kj::Maybe<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder.initMaybe().initValue()); }
};

template<typename T>
struct BuildRtti<jsg::Optional<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder.initMaybe().initValue()); }
};
template<typename T>
struct BuildRtti<kj::Array<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder.initArray().initElement()); }
};

template<typename T>
struct BuildRtti<kj::ArrayPtr<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder.initArray().initElement()); }
};

template<typename T>
struct BuildRtti<jsg::Sequence<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder.initArray().initElement()); }
};

template<typename K, typename V>
struct BuildRtti<jsg::Dict<V, K>> {
  static void build(Type::Builder builder) {
    auto dict = builder.initDict();
    BuildRtti<K>::build(dict.initKey());
    BuildRtti<V>::build(dict.initValue());
  }
};

template<typename...Variants>
struct BuildRtti<kj::OneOf<Variants...>> {
  using Seq = std::index_sequence_for<Variants...>;
  using Tuple = std::tuple<Variants...>;

  template<size_t I>
  static inline void buildVariant(capnp::List<Type>::Builder builder) {
    BuildRtti<std::tuple_element_t<I, Tuple>>::build(builder[I]);
  }

  template<size_t...Indexes>
  static inline void buildVariants(std::integer_sequence<size_t, Indexes...> seq,
                                   capnp::List<Type>::Builder builder) {
    ((buildVariant<Indexes>(builder)), ...);
  }

  static void build(Type::Builder builder) {
    auto variants = builder.initOneOf().initVariants(Seq::size());
    buildVariants(Seq{}, variants);
  }
};

// Promises

template<typename T>
struct BuildRtti<kj::Promise<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder.initPromise().initValue()); }
};


template<typename T>
struct BuildRtti<jsg::Promise<T>> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder.initPromise().initValue()); }
};

template<>
struct BuildRtti<v8::Promise> {
  static void build(Type::Builder builder) { builder.initPromise().initValue().setUnknown(); }
};

// Builtins

#define DECLARE_BUILTIN_TYPE(T, V) \
template<> \
struct BuildRtti<T> { \
  static void build(Type::Builder builder) { \
    builder.initBuiltin().setType(V); \
  } \
};

#define FOR_EACH_BUILTIN_TYPE(F, ...) \
  F(jsg::BufferSource, BuiltinType::Type::JSG_BUFFER_SOURCE) \
  F(jsg::Lock, BuiltinType::Type::JSG_LOCK) \
  F(jsg::Unimplemented, BuiltinType::Type::JSG_UNIMPLEMENTED) \
  F(jsg::Varargs, BuiltinType::Type::JSG_VARARGS) \
  F(kj::Date, BuiltinType::Type::KJ_DATE) \
  F(v8::ArrayBufferView, BuiltinType::Type::V8_ARRAY_BUFFER_VIEW) \
  F(v8::Function, BuiltinType::Type::V8_FUNCTION) \
  F(v8::Isolate*, BuiltinType::Type::V8_ISOLATE) \
  F(v8::Uint8Array, BuiltinType::Type::V8_UINT8_ARRAY)

FOR_EACH_BUILTIN_TYPE(DECLARE_BUILTIN_TYPE)

#undef FOR_EACH_BUILTIN_TYPE
#undef DECLARE_BUILTIN_TYPE

#define JSG_RTTI_DECLARE_CONFIGURATION_TYPE(T) \
  namespace workerd::jsg::rtti::impl { \
    template<> \
    struct BuildRtti<T> { \
      static void build(Type::Builder builder) { \
        builder.initBuiltin().setType(BuiltinType::Type::FLAGS); \
      } \
    }; \
  }
// Use this at the global scope to declare a configuration type before invoking RTTI. E.g.:
//
//   JSG_RTTI_DECLARE_CONFIGURATION_TYPE(workerd::CompatibilityFlags::Reader)
//
// TODO(cleanup): This should probably be done instead through a template parameter to
//   `RttiBuilder` that gets passed down, but that's a deeper change.

template<typename T>
struct BuildRtti<jsg::TypeHandler<T>> {
  static void build(Type::Builder builder) {
    builder.initBuiltin().setType(BuiltinType::Type::JSG_TYPE_HANDLER);
  }
};


// Functions

template<typename Fn>
struct BuildRtti<jsg::Function<Fn>> {
  static void build(Type::Builder builder) {
    auto fn = builder.initFunction();
    using Traits = FunctionTraits<Fn>;
    BuildRtti<typename Traits::ReturnType>::build(fn.initReturnType());
    using Args = typename Traits::ArgsTuple;
    TupleRttiBuilder<Args>::build(fn.initArgs(std::tuple_size_v<Args>));
  }
};

// C++ modifiers

template<typename T>
struct BuildRtti<const T> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder); }
};

template<typename T>
struct BuildRtti<T&> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder); }
};

template<typename T>
struct BuildRtti<T&&> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder); }
};

template<typename T>
struct BuildRtti<const T&> {
  static void build(Type::Builder builder) { BuildRtti<T>::build(builder); }
};

// Structs

struct MemberCounter {
  // count all members in the structure

  template<const char* name, typename Method, Method method>
  inline void registerMethod() { ++count; }

  template<typename Type>
  inline void registerInherit() { /* inherit is not a member */ }

  template<const char* name>
  inline void registerInheritIntrinsic(v8::Intrinsic intrinsic) { /* inherit is not a member */ }

  template<const char* name, typename Method, Method method>
  inline void registerIterable() { /* not a member */ }

  template<const char* name, typename Method, Method method>
  inline void registerAsyncIterable() { /* not a member */ }

  template<typename Type, const char* name>
  inline void registerNestedType() { ++count; }

  template<const char* name, typename Property, auto property>
  inline void registerStructProperty() { ++count; }

  template<const char* name, typename Getter, Getter getter>
  inline void registerReadonlyPrototypeProperty() { ++count; }

  template<const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerPrototypeProperty() { ++count; }

  template<const char* name, typename Getter, Getter getter>
  inline void registerReadonlyInstanceProperty() { ++count; }

  template<const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerInstanceProperty() { ++count; }

  template<const char* name, typename Getter, Getter getter, bool readOnly>
  inline void registerLazyInstanceProperty() { ++count; }

  template<const char* name, typename T>
  inline void registerStaticConstant(T value) { ++count; }

  template<const char* name, typename Method, Method method>
  inline void registerStaticMethod() { ++count; }

  size_t count = 0;
};

template<typename Self, typename Configuration>
struct MembersBuilder {
  Configuration& configuration;
  Structure::Builder structure;
  capnp::List<Member>::Builder members;
  int index = 0;

  MembersBuilder(Configuration& configuration, Structure::Builder structure,
      capnp::List<Member>::Builder members)
    : configuration(configuration), structure(structure), members(members) { }

  template<typename Type>
  inline void registerInherit() {
    BuildRtti<Type>::build(structure.initExtends());
  }

  template<const char* name>
  inline void registerInheritIntrinsic(v8::Intrinsic intrinsic) {
    structure.initExtends().initIntrinsic().setName(name);
  }

  template<typename Type, const char* name>
  inline void registerNestedType() {
    auto nested = members[index++].initNested();
    BuildRtti<Type>::build(nested, configuration);
  }

  template<const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerInstanceProperty() {
    auto prop = members[index++].initProperty();
    prop.setName(name);
    using GetterTraits = FunctionTraits<Getter>;
    BuildRtti<typename GetterTraits::ReturnType>::build(prop.initType());
  }

  template<const char* name, typename Getter, Getter getter>
  inline void registerReadonlyInstanceProperty() {
    auto prop = members[index++].initProperty();
    prop.setName(name);
    prop.setReadonly(true);
    using GetterTraits = FunctionTraits<Getter>;
    BuildRtti<typename GetterTraits::ReturnType>::build(prop.initType());
  }

  template<const char* name, typename Getter, Getter getter, bool readOnly>
  inline void registerLazyInstanceProperty() {
    auto prop = members[index++].initProperty();
    prop.setName(name);
    prop.setReadonly(readOnly);
    prop.setLazy(true);
    using GetterTraits = FunctionTraits<Getter>;
    BuildRtti<typename GetterTraits::ReturnType>::build(prop.initType());
  }

  template<const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerPrototypeProperty() {
    auto prop = members[index++].initProperty();
    prop.setName(name);
    prop.setPrototype(true);
    using GetterTraits = FunctionTraits<Getter>;
    BuildRtti<typename GetterTraits::ReturnType>::build(prop.initType());
  }

  template<const char* name, typename Getter, Getter getter>
  inline void registerReadonlyPrototypeProperty() {
    auto prop = members[index++].initProperty();
    prop.setName(name);
    prop.setPrototype(true);
    prop.setReadonly(true);
    using GetterTraits = FunctionTraits<Getter>;
    BuildRtti<typename GetterTraits::ReturnType>::build(prop.initType());
  }

  template<const char* name, typename T>
  inline void registerStaticConstant(T value) {
    auto constant = members[index++].initConstant();
    constant.setName(name);
    constant.setValue(value);
    // BuildRtti<T>::build(constant.initType());
  }

  template<const char* name, typename Property, Property Self::*property>
  void registerStructProperty() {
    auto prop = members[index++].initProperty();
    prop.setName(name);
    BuildRtti<Property>::build(prop.initType());
  }

  template<const char* name, typename Method, Method>
  inline void registerMethod() {
    auto method = members[index++].initMethod();

    method.setName(name);
    using Traits = FunctionTraits<Method>;
    BuildRtti<typename Traits::ReturnType>::build(method.initReturnType());
    using Args = typename Traits::ArgsTuple;
    TupleRttiBuilder<Args>::build(method.initArgs(std::tuple_size_v<Args>));
  }

  template<const char* name, typename Method, Method>
  inline void registerStaticMethod() {
    auto method = members[index++].initMethod();

    method.setName(name);
    method.setStatic(true);
    using Traits = FunctionTraits<Method>;
    BuildRtti<typename Traits::ReturnType>::build(method.initReturnType());
    using Args = typename Traits::ArgsTuple;
    TupleRttiBuilder<Args>::build(method.initArgs(std::tuple_size_v<Args>));
  }

  template<const char* name, typename Method, Method method>
  inline void registerIterable() { structure.setIterable(true); }

  template<const char* name, typename Method, Method method>
  inline void registerAsyncIterable() { structure.setAsyncIterable(true); }
};

template <typename T, typename = int>
struct HasRegisterMembers : std::false_type {};
// true when the T has registerMembers() function generated by JSG_RESOURCE/JSG_STRUCT

template <typename T>
struct HasRegisterMembers<T, decltype(T::template registerMembers<MemberCounter, T>, 0)> : std::true_type { };


template <typename T, typename = int>
struct HasConstructor : std::false_type {};
// true when the T has constructor() function

template <typename T>
struct HasConstructor<T, decltype(T::constructor, 0)> : std::true_type { };

template <typename T>
struct BuildRtti<T, std::enable_if_t<HasRegisterMembers<T>::value>> {
  using Configuration = DetectedOr<NullConfiguration, GetConfiguration, T>;

  static void build(Type::Builder builder) {
    builder.initStructure().setName(jsg::typeName(typeid(T)));
  }

  static void build(Structure::Builder builder, Configuration configuration) {
    builder.setName(jsg::typeName(typeid(T)));

    MemberCounter counter;
    if constexpr (isDetected<GetConfiguration, T>()) {
      T::template registerMembers<decltype(counter), T>(counter, configuration);
    } else {
      T::template registerMembers<decltype(counter), T>(counter);
    }
    auto count = counter.count;

    if constexpr (HasConstructor<T>::value) {
      count++;
    }

    auto members = builder.initMembers(count);
    MembersBuilder<T, Configuration> membersBuilder(configuration, builder, members);
    if constexpr (isDetected<GetConfiguration, T>()) {
      T::template registerMembers<decltype(membersBuilder), T>(membersBuilder, configuration);
    } else {
      T::template registerMembers<decltype(membersBuilder), T>(membersBuilder);
    }

    if constexpr (HasConstructor<T>::value) {
      auto constructor = members[membersBuilder.index++].initConstructor();
      using Traits = FunctionTraits<decltype(T::constructor)>;
      using Args = typename Traits::ArgsTuple;
      TupleRttiBuilder<Args>::build(constructor.initArgs(std::tuple_size_v<Args>));
    }
  }
};


} // namespace impl

} // namespace workerd::jsg::rtti
