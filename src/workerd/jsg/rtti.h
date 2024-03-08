// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Runtime type system for jsg.
// Produces capnp description (rtti.capnp) of jsg structs, resources and c++ types of their
// members.
// Can be used to generate typescript type, dynamically invoke methods, fuzz, check backward
// compatibility etc.

#include <kj/map.h>
#include <capnp/message.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/rtti.capnp.h>
#include <kj/map.h>

namespace workerd::jsg::rtti {

namespace impl {

// Struct for partial specialization.
template <typename Configuration, typename T, typename Enable = void>
struct BuildRtti;

} // namespace impl

// User's entry point into rtti.
// Builder owns capnp builder for all the objects it returns, so usual capnp builder
// rules apply.
// The rtti describes object structure and their types.
// All structure references in rtti are stored by name. The builder maintains a symbol table
// which can be used to resolve them. It is guaranteed that the table is full enough to
// interpret all types passed through a given builder.
template<typename MetaConfiguration>
class Builder {
public:
  const MetaConfiguration config;

  Builder(const MetaConfiguration& config) : config(config) {}

  template<typename T>
  Type::Reader type() {
    auto type = builder.initRoot<Type>();
    impl::BuildRtti<MetaConfiguration, T>::build(type, *this);
    return type;
  }

  template<typename T>
  Structure::Reader structure() {
    auto name = jsg::fullyQualifiedTypeName(typeid(T));
    KJ_IF_SOME(builder, symbols.find(name)) {
      return builder->template getRoot<Structure>();
    }

    auto& builder = symbols.insert(
          kj::str(name), kj::heap<capnp::MallocMessageBuilder>()).value;
    auto structure = builder->template initRoot<Structure>();
    impl::BuildRtti<MetaConfiguration, T>::build(structure, *this);
    return structure;
  }

  kj::Maybe<Structure::Reader> structure(kj::StringPtr name) {
    // lookup structure in the symbol table
    return symbols.find(name).map([](kj::Own<capnp::MallocMessageBuilder>& builder) {
      return builder->template getRoot<Structure>();
    });
  }

private:
  capnp::MallocMessageBuilder builder;
  kj::HashMap<kj::String, kj::Own<capnp::MallocMessageBuilder>> symbols;
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

template<typename Configuration, typename Tuple>
struct TupleRttiBuilder {
  static inline void build(capnp::List<Type>::Builder builder, Builder<Configuration>& rtti) {
    build(std::make_integer_sequence<size_t, std::tuple_size_v<Tuple>>{}, builder, rtti);
  }

private:
  template<size_t...Indexes>
  static inline void build(std::integer_sequence<size_t, Indexes...> seq,
                           capnp::List<Type>::Builder builder,
                           Builder<Configuration>& rtti) {
    ((buildIndex<Indexes>(builder, rtti)), ...);
  }

  template<size_t I>
  static inline void buildIndex(capnp::List<Type>::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, std::tuple_element_t<I, Tuple>>::build(builder[I], rtti);
  }
};


// Primitives

template<typename Configuration>
struct BuildRtti<Configuration, void> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.setVoidt(); }
};

template<typename Configuration>
struct BuildRtti<Configuration, bool> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.setBoolt(); }
};

template<typename Configuration>
struct BuildRtti<Configuration, jsg::JsBoolean> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.setBoolt(); }
};

template<typename Configuration>
struct BuildRtti<Configuration, v8::Value> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.setUnknown(); }
};

template<typename Configuration>
struct BuildRtti<Configuration, jsg::JsValue> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.setUnknown(); }
};

template<typename Configuration>
struct BuildRtti<Configuration, jsg::JsRegExp> {
  // This isn't really unknown but we currently do not expose these types at all, so
  // this is ok for now.
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.setUnknown(); }
};

template<typename Configuration>
struct BuildRtti<Configuration, jsg::JsMap> {
  // This isn't really unknown but we currently do not expose these types at all, so
  // this is ok for now.
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.setUnknown(); }
};

template<typename Configuration>
struct BuildRtti<Configuration, jsg::JsSet> {
  // This isn't really unknown but we currently do not expose these types at all, so
  // this is ok for now.
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.setUnknown(); }
};

template<typename Configuration>
struct BuildRtti<Configuration, jsg::JsSymbol> {
  // This isn't really unknown but we currently do not expose these types at all, so
  // this is ok for now.
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.setUnknown(); }
};


// Numbers

#define DECLARE_NUMBER_TYPE(T) \
template<typename Configuration> \
struct BuildRtti<Configuration, T> { \
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.initNumber().setName(#T); } \
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
  F(long long) \
  F(unsigned long long) \
  F(double) \
  F(jsg::JsNumber) \
  F(jsg::JsInt32) \
  F(jsg::JsUint32) \
  F(jsg::JsBigInt)

FOR_EACH_NUMBER_TYPE(DECLARE_NUMBER_TYPE)

#undef FOR_EACH_NUMBER_TYPE
#undef DECLARE_NUMBER_TYPE

// Strings

#define DECLARE_STRING_TYPE(T) \
template<typename Configuration> \
struct BuildRtti<Configuration, T> { \
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.initString().setName(#T); } \
};

#define FOR_EACH_STRING_TYPE(F) \
  F(kj::String) \
  F(kj::StringPtr) \
  F(v8::String) \
  F(ByteString) \
  F(jsg::JsString)

FOR_EACH_STRING_TYPE(DECLARE_STRING_TYPE)

#undef FOR_EACH_STRING_TYPE
#undef DECLARE_STRING_TYPE

// Object Types

template<typename Configuration>
struct BuildRtti<Configuration, v8::Object> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.setObject(); }
};

template<typename Configuration>
struct BuildRtti<Configuration, jsg::Object> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.setObject(); }
};

template<typename Configuration>
struct BuildRtti<Configuration, jsg::JsObject> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { builder.setObject(); }
};

// References

template<typename Configuration, typename T>
struct BuildRtti<Configuration, Ref<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

template<typename Configuration, typename T>
struct BuildRtti<Configuration, V8Ref<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

template<typename Configuration, typename T>
struct BuildRtti<Configuration, JsRef<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

template<typename Configuration, typename T>
struct BuildRtti<Configuration, HashableV8Ref<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

template<typename Configuration, typename T>
struct BuildRtti<Configuration, v8::Local<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

template<typename Configuration, typename T>
struct BuildRtti<Configuration, v8::Global<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

template<typename Configuration, typename T>
struct BuildRtti<Configuration, jsg::MemoizedIdentity<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

template<typename Configuration, typename T>
struct BuildRtti<Configuration, jsg::Identified<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

template<typename Configuration, typename T>
struct BuildRtti<Configuration, jsg::NonCoercible<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

// Maybe Types

#define DECLARE_MAYBE_TYPE(T) \
template<typename Configuration, typename V> \
struct BuildRtti<Configuration, T<V>> { \
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { \
    auto maybe = builder.initMaybe(); \
    BuildRtti<Configuration, V>::build(maybe.initValue(), rtti); \
    maybe.setName(#T); \
  } \
};

#define FOR_EACH_MAYBE_TYPE(F) \
  F(kj::Maybe) \
  F(jsg::Optional) \
  F(jsg::LenientOptional)

FOR_EACH_MAYBE_TYPE(DECLARE_MAYBE_TYPE)

#undef FOR_EACH_MAYBE_TYPE
#undef DECLARE_MAYBE_TYPE

// Array Types

#define DECLARE_ARRAY_TYPE(T) \
template<typename Configuration, typename V> \
struct BuildRtti<Configuration, T<V>> { \
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { \
    auto array = builder.initArray(); \
    BuildRtti<Configuration, V>::build(array.initElement(), rtti); \
    array.setName(#T); \
  } \
};

#define FOR_EACH_ARRAY_TYPE(F) \
  F(kj::Array) \
  F(kj::ArrayPtr) \
  F(jsg::Sequence)

template<typename Configuration> \
struct BuildRtti<Configuration, jsg::JsArray> { \
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    auto array = builder.initArray();
    BuildRtti<Configuration, JsValue>::build(array.initElement(), rtti);
    array.setName("jsg::JsArray");
  }
};

FOR_EACH_ARRAY_TYPE(DECLARE_ARRAY_TYPE)

#undef FOR_EACH_ARRAY_TYPE
#undef DECLARE_ARRAY_TYPE

// Misc Generic Types

template<typename Configuration, typename K, typename V>
struct BuildRtti<Configuration, jsg::Dict<V, K>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    auto dict = builder.initDict();
    BuildRtti<Configuration, K>::build(dict.initKey(), rtti);
    BuildRtti<Configuration, V>::build(dict.initValue(), rtti);
  }
};

template<typename Configuration, typename...Variants>
struct BuildRtti<Configuration, kj::OneOf<Variants...>> {
  using Seq = std::index_sequence_for<Variants...>;
  using Tuple = std::tuple<Variants...>;

  template<size_t I>
  static inline void buildVariant(capnp::List<Type>::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, std::tuple_element_t<I, Tuple>>::build(builder[I], rtti);
  }

  template<size_t...Indexes>
  static inline void buildVariants(std::integer_sequence<size_t, Indexes...> seq,
                                   capnp::List<Type>::Builder builder,
                                   Builder<Configuration>& rtti) {
    ((buildVariant<Indexes>(builder, rtti)), ...);
  }

  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    auto variants = builder.initOneOf().initVariants(Seq::size());
    buildVariants(Seq{}, variants, rtti);
  }
};

// Promises

template<typename Configuration, typename T>
struct BuildRtti<Configuration, kj::Promise<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder.initPromise().initValue(), rtti);
  }
};


template<typename Configuration, typename T>
struct BuildRtti<Configuration, jsg::Promise<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder.initPromise().initValue(), rtti);
  }
};

template<typename Configuration>
struct BuildRtti<Configuration, v8::Promise> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    builder.initPromise().initValue().setUnknown();
  }
};

// Builtins

#define DECLARE_BUILTIN_TYPE(T, V) \
template<typename Configuration> \
struct BuildRtti<Configuration, T> { \
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { \
    builder.initBuiltin().setType(V); \
  } \
};

#define FOR_EACH_BUILTIN_TYPE(F, ...) \
  F(jsg::BufferSource, BuiltinType::Type::JSG_BUFFER_SOURCE) \
  F(kj::Date, BuiltinType::Type::KJ_DATE) \
  F(v8::ArrayBufferView, BuiltinType::Type::V8_ARRAY_BUFFER_VIEW) \
  F(v8::ArrayBuffer, BuiltinType::Type::V8_ARRAY_BUFFER) \
  F(v8::Function, BuiltinType::Type::V8_FUNCTION) \
  F(v8::Uint8Array, BuiltinType::Type::V8_UINT8_ARRAY) \
  F(jsg::JsDate, BuiltinType::Type::KJ_DATE)

FOR_EACH_BUILTIN_TYPE(DECLARE_BUILTIN_TYPE)

#undef FOR_EACH_BUILTIN_TYPE
#undef DECLARE_BUILTIN_TYPE

// Jsg implementation types

#define DECLARE_JSG_IMPL_TYPE(T, V) \
template<typename Configuration> \
struct BuildRtti<Configuration, T> { \
  static void build(Type::Builder builder, Builder<Configuration>& rtti) { \
    builder.initJsgImpl().setType(V); \
  } \
};

#define FOR_EACH_JSG_IMPL_TYPE(F, ...) \
  F(jsg::Lock, JsgImplType::Type::JSG_LOCK) \
  F(jsg::Name, JsgImplType::Type::JSG_NAME) \
  F(jsg::SelfRef, JsgImplType::Type::JSG_SELF_REF) \
  F(jsg::Unimplemented, JsgImplType::Type::JSG_UNIMPLEMENTED) \
  F(jsg::Varargs, JsgImplType::Type::JSG_VARARGS) \
  F(v8::Isolate*, JsgImplType::Type::V8_ISOLATE) \
  F(v8::FunctionCallbackInfo<v8::Value>, JsgImplType::Type::V8_FUNCTION_CALLBACK_INFO) \
  F(v8::PropertyCallbackInfo<v8::Value>, JsgImplType::Type::V8_PROPERTY_CALLBACK_INFO)

FOR_EACH_JSG_IMPL_TYPE(DECLARE_JSG_IMPL_TYPE)

#undef FOR_EACH_JSG_IMPL_TYPE
#undef DECLARE_JSG_IMPL_TYPE

template<typename Configuration, typename T>
struct BuildRtti<Configuration, Arguments<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    // TODO(someday): Create a representation of Arguments<T> that actually encodes the type T.
    builder.initJsgImpl().setType(JsgImplType::Type::JSG_VARARGS);
  }
};

template<typename Configuration>
struct BuildRtti<Configuration, Configuration> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    builder.initJsgImpl().setType(JsgImplType::Type::CONFIGURATION);
  }
};

template<typename Configuration, typename T>
struct BuildRtti<Configuration, jsg::TypeHandler<T>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    builder.initJsgImpl().setType(JsgImplType::Type::JSG_TYPE_HANDLER);
  }
};


// Functions

template<typename Configuration, typename Fn>
struct BuildRtti<Configuration, jsg::Function<Fn>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    auto fn = builder.initFunction();
    using Traits = FunctionTraits<Fn>;
    BuildRtti<Configuration, typename Traits::ReturnType>::build(fn.initReturnType(), rtti);
    using Args = typename Traits::ArgsTuple;
    TupleRttiBuilder<Configuration, Args>::build(fn.initArgs(std::tuple_size_v<Args>), rtti);
  }
};

// C++ modifiers

template<typename Configuration, typename T>
struct BuildRtti<Configuration, const T> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

template<typename Configuration, typename T>
struct BuildRtti<Configuration, T&> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

template<typename Configuration, typename T>
struct BuildRtti<Configuration, T&&> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

template<typename Configuration, typename T>
struct BuildRtti<Configuration, const T&> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    BuildRtti<Configuration, T>::build(builder, rtti);
  }
};

// Structs

// count all members in the structure
struct MemberCounter {
  template <typename Type, typename GetNamedMethod, GetNamedMethod getNamedMethod>
  inline void registerWildcardProperty() { /* not a member */}

  template<const char* name, typename Method, Method method>
  inline void registerMethod() { ++members; }

  template<typename Method, Method method>
  inline void registerCallable() { /* not a member */ }

  template<typename Type>
  inline void registerInherit() { /* inherit is not a member */ }

  template<const char* name>
  inline void registerInheritIntrinsic(v8::Intrinsic intrinsic) { /* inherit is not a member */ }

  template<const char* name, typename Method, Method method>
  inline void registerIterable() { /* not a member */ }

  template<const char* name, typename Method, Method method>
  inline void registerAsyncIterable() { /* not a member */ }

  template<const char* name, typename Method, Method method>
  inline void registerDispose() { /* not a member */ }

  template<const char* name, typename Method, Method method>
  inline void registerAsyncDispose() { /* not a member */ }

  template<typename Type, const char* name>
  inline void registerNestedType() { ++members; }

  template<const char* name, typename Property, auto property>
  inline void registerStructProperty() { ++members; }

  template<const char* name, typename Getter, Getter getter>
  inline void registerReadonlyPrototypeProperty() { ++members; }

  template<const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerPrototypeProperty() { ++members; }

  template<const char* name, typename Getter, Getter getter>
  inline void registerReadonlyInstanceProperty() { ++members; }

  template<typename T>
  inline void registerReadonlyInstanceProperty(kj::StringPtr, T value) { ++members; }

  template<const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerInstanceProperty() { ++members; }

  template<const char* name, typename Getter, Getter getter, bool readOnly>
  inline void registerLazyInstanceProperty() { ++members; }

  template<const char* name, const char* moduleName, bool readOnly>
  inline void registerLazyJsInstanceProperty() { ++members; }

  template<const char* name, typename Getter, Getter getter>
  inline void registerInspectProperty() { /* not included */ }

  template<const char* name, typename T>
  inline void registerStaticConstant(T value) { ++members; }

  template<const char* name, typename Method, Method method>
  inline void registerStaticMethod() { ++members; }

  inline void registerTypeScriptRoot() { /* not a member */ }

  template<const char* tsOverride>
  inline void registerTypeScriptOverride() { /* not a member */ }

  template<const char* tsDefine>
  inline void registerTypeScriptDefine() { /* not a member */ }

  inline void registerJsBundle(Bundle::Reader bundle) {
    modules += bundle.getModules().size();
  }

  size_t members = 0;
  size_t modules = 0;
};

template<typename Self, typename Configuration>
struct MembersBuilder {
  Structure::Builder structure;
  capnp::List<Member>::Builder members;
  capnp::List<Module>::Builder modules;
  Builder<Configuration>& rtti;
  uint memberIndex = 0;
  uint moduleIndex = 0;

  MembersBuilder(Structure::Builder structure,
                 capnp::List<Member>::Builder members,
                 capnp::List<Module>::Builder modules,
                 Builder<Configuration>& rtti)
    : structure(structure), members(members), modules(modules), rtti(rtti) { }

  template<typename Type>
  inline void registerInherit() {
    BuildRtti<Configuration, Type>::build(structure.initExtends(), rtti);
  }

  template<const char* name>
  inline void registerInheritIntrinsic(v8::Intrinsic intrinsic) {
    structure.initExtends().initIntrinsic().setName(name);
  }

  template<typename Type, const char* name>
  inline void registerNestedType() {
    auto nested = members[memberIndex++].initNested();
    nested.setName(name);
    BuildRtti<Configuration, Type>::build(nested.initStructure(), rtti);
  }

  template<const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerInstanceProperty() {
    auto prop = members[memberIndex++].initProperty();
    prop.setName(name);
    using GetterTraits = FunctionTraits<Getter>;
    BuildRtti<Configuration, typename GetterTraits::ReturnType>::build(prop.initType(), rtti);
  }

  template<const char* name, typename Getter, Getter getter>
  inline void registerReadonlyInstanceProperty() {
    auto prop = members[memberIndex++].initProperty();
    prop.setName(name);
    prop.setReadonly(true);
    using GetterTraits = FunctionTraits<Getter>;
    BuildRtti<Configuration, typename GetterTraits::ReturnType>::build(prop.initType(), rtti);
  }

  template<typename T>
  inline void registerReadonlyInstanceProperty(kj::StringPtr name, T value) {
    auto prop = members[memberIndex++].initProperty();
    prop.setName(name);
    prop.setReadonly(true);
    BuildRtti<Configuration, T>::build(prop.initType(), rtti);
  }

  template<const char* name, typename Getter, Getter getter, bool readOnly>
  inline void registerLazyInstanceProperty() {
    auto prop = members[memberIndex++].initProperty();
    prop.setName(name);
    prop.setReadonly(readOnly);
    prop.setLazy(true);
    using GetterTraits = FunctionTraits<Getter>;
    BuildRtti<Configuration, typename GetterTraits::ReturnType>::build(prop.initType(), rtti);
  }


  template<const char* name, const char* moduleName, bool readOnly>
  inline void registerLazyJsInstanceProperty() {
    auto prop = members[memberIndex++].initProperty();
    prop.setName(name);
    prop.setReadonly(readOnly);
    prop.setLazy(true);
    auto jsBuiltin = prop.initType().initJsBuiltin();
    jsBuiltin.setModule(moduleName);
    jsBuiltin.setExport(name);
  }

  template<const char* name, typename Getter, Getter getter, typename Setter, Setter setter>
  inline void registerPrototypeProperty() {
    auto prop = members[memberIndex++].initProperty();
    prop.setName(name);
    prop.setPrototype(true);
    using GetterTraits = FunctionTraits<Getter>;
    BuildRtti<Configuration, typename GetterTraits::ReturnType>::build(prop.initType(), rtti);
  }

  template<const char* name, typename Getter, Getter getter>
  inline void registerReadonlyPrototypeProperty() {
    auto prop = members[memberIndex++].initProperty();
    prop.setName(name);
    prop.setPrototype(true);
    prop.setReadonly(true);
    using GetterTraits = FunctionTraits<Getter>;
    BuildRtti<Configuration, typename GetterTraits::ReturnType>::build(prop.initType(), rtti);
  }

  template<const char* name, typename Getter, Getter getter>
  inline void registerInspectProperty() { }

  template<const char* name, typename T>
  inline void registerStaticConstant(T value) {
    auto constant = members[memberIndex++].initConstant();
    constant.setName(name);
    constant.setValue(value);
    // BuildRtti<Configuration, T>::build(constant.initType());
  }

  template<const char* name, typename Property, Property Self::*property>
  void registerStructProperty() {
    auto prop = members[memberIndex++].initProperty();
    prop.setName(name);
    BuildRtti<Configuration, Property>::build(prop.initType(), rtti);
  }

  template<const char* name, typename Method, Method>
  inline void registerMethod() {
    auto method = members[memberIndex++].initMethod();

    method.setName(name);
    using Traits = FunctionTraits<Method>;
    BuildRtti<Configuration, typename Traits::ReturnType>::build(method.initReturnType(), rtti);
    using Args = typename Traits::ArgsTuple;
    TupleRttiBuilder<Configuration, Args>::build(method.initArgs(std::tuple_size_v<Args>), rtti);
  }

  template<typename Method, Method method>
  inline void registerCallable() {
    auto func = structure.initCallable();

    using Traits = FunctionTraits<Method>;
    BuildRtti<Configuration, typename Traits::ReturnType>::build(func.initReturnType(), rtti);
    using Args = typename Traits::ArgsTuple;
    TupleRttiBuilder<Configuration, Args>::build(func.initArgs(std::tuple_size_v<Args>), rtti);
  }

  template<const char* name, typename Method, Method>
  inline void registerStaticMethod() {
    auto method = members[memberIndex++].initMethod();

    method.setName(name);
    method.setStatic(true);
    using Traits = FunctionTraits<Method>;
    BuildRtti<Configuration, typename Traits::ReturnType>::build(method.initReturnType(), rtti);
    using Args = typename Traits::ArgsTuple;
    TupleRttiBuilder<Configuration, Args>::build(method.initArgs(std::tuple_size_v<Args>), rtti);
  }

  template<const char* name, typename Method, Method>
  inline void registerIterable() {
    structure.setIterable(true);

    auto method = structure.initIterator();
    method.setName(name);
    using Traits = FunctionTraits<Method>;
    BuildRtti<Configuration, typename Traits::ReturnType>::build(method.initReturnType(), rtti);
    using Args = typename Traits::ArgsTuple;
    TupleRttiBuilder<Configuration, Args>::build(method.initArgs(std::tuple_size_v<Args>), rtti);
  }

  template<const char* name, typename Method, Method>
  inline void registerAsyncIterable() {
    structure.setAsyncIterable(true);

    auto method = structure.initAsyncIterator();
    method.setName(name);
    using Traits = FunctionTraits<Method>;
    BuildRtti<Configuration, typename Traits::ReturnType>::build(method.initReturnType(), rtti);
    using Args = typename Traits::ArgsTuple;
    TupleRttiBuilder<Configuration, Args>::build(method.initArgs(std::tuple_size_v<Args>), rtti);
  }

  template<const char* name, typename Method, Method>
  inline void registerDispose() {
    structure.setDisposable(true);

    auto method = structure.initDispose();
    method.setName(name);
    using Traits = FunctionTraits<Method>;
    BuildRtti<Configuration, typename Traits::ReturnType>::build(method.initReturnType(), rtti);
    using Args = typename Traits::ArgsTuple;
    TupleRttiBuilder<Configuration, Args>::build(method.initArgs(std::tuple_size_v<Args>), rtti);
  }

  template<const char* name, typename Method, Method>
  inline void registerAsyncDispose() {
    structure.setAsyncDisposable(true);

    auto method = structure.initAsyncDispose();
    method.setName(name);
    using Traits = FunctionTraits<Method>;
    BuildRtti<Configuration, typename Traits::ReturnType>::build(method.initReturnType(), rtti);
    using Args = typename Traits::ArgsTuple;
    TupleRttiBuilder<Configuration, Args>::build(method.initArgs(std::tuple_size_v<Args>), rtti);
  }

  inline void registerTypeScriptRoot() {
    structure.setTsRoot(true);
  }

  template<const char* tsOverride>
  inline void registerTypeScriptOverride() {
    structure.setTsOverride(tsOverride);
  }

  template<const char* tsDefine>
  inline void registerTypeScriptDefine() {
    structure.setTsDefine(tsDefine);
  }

  inline void registerJsBundle(Bundle::Reader bundle) {
    for (auto module: bundle.getModules()) {
      auto m = modules[moduleIndex++];
      m.setSpecifier(module.getName());
      m.setTsDeclarations(module.getTsDeclaration());
    }
  }

  template <typename Type, typename GetNamedMethod, GetNamedMethod getNamedMethod>
  inline void registerWildcardProperty() {
    // Nothing to do in this case.
  }
};

// true when the T has registerMembers() function generated by JSG_RESOURCE/JSG_STRUCT
template <typename T, typename = int>
struct HasRegisterMembers : std::false_type {};

template <typename T>
struct HasRegisterMembers<T, decltype(T::template registerMembers<MemberCounter, T>, 0)> : std::true_type { };

// true when the T has constructor() function
template <typename T, typename = int>
struct HasConstructor : std::false_type {};

template <typename T>
struct HasConstructor<T, decltype(T::constructor, 0)> : std::true_type { };

template <typename Configuration, typename T>
struct BuildRtti<Configuration, T, std::enable_if_t<HasRegisterMembers<T>::value>> {
  static void build(Type::Builder builder, Builder<Configuration>& rtti) {
    auto structure = builder.initStructure();
    structure.setName(jsg::typeName(typeid(T)));
    structure.setFullyQualifiedName(jsg::fullyQualifiedTypeName(typeid(T)));
    rtti.template structure<T>();
  }

  static void build(Structure::Builder builder, Builder<Configuration>& rtti) {
    builder.setName(jsg::typeName(typeid(T)));
    builder.setFullyQualifiedName(jsg::fullyQualifiedTypeName(typeid(T)));

    MemberCounter counter;
    if constexpr (isDetected<GetConfiguration, T>()) {
      T::template registerMembers<decltype(counter), T>(counter, rtti.config);
    } else {
      T::template registerMembers<decltype(counter), T>(counter);
    }
    auto membersCount = counter.members;

    if constexpr (HasConstructor<T>::value) {
      membersCount++;
    }

    auto members = builder.initMembers(membersCount);
    auto modules = counter.modules > 0 ? builder.initBuiltinModules(counter.modules) : capnp::List<Module>::Builder();
    MembersBuilder<T, Configuration> membersBuilder(builder, members, modules, rtti);
    if constexpr (isDetected<GetConfiguration, T>()) {
      T::template registerMembers<decltype(membersBuilder), T>(membersBuilder, rtti.config);
    } else {
      T::template registerMembers<decltype(membersBuilder), T>(membersBuilder);
    }

    if constexpr (HasConstructor<T>::value) {
      auto constructor = members[membersBuilder.memberIndex++].initConstructor();
      using Traits = FunctionTraits<decltype(T::constructor)>;
      using Args = typename Traits::ArgsTuple;
      TupleRttiBuilder<Configuration, Args>::build(constructor.initArgs(std::tuple_size_v<Args>), rtti);
    }
  }
};


} // namespace impl

} // namespace workerd::jsg::rtti
