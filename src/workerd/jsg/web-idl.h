// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// Type traits to help us map between C++ and Web IDL types/concepts.

#include <workerd/jsg/jsg.h>

#include <kj/array.h>
#include <kj/common.h>
#include <kj/one-of.h>

namespace workerd::jsg::webidl {

template <typename T>
constexpr bool isOptional = false;
template <typename T>
constexpr bool isOptional<Optional<T>> = true;
template <typename T>
constexpr bool isOptional<LenientOptional<T>> = true;

// Counts the number of Web IDL nullable types (modeled with kj::Maybe in JSG) that exist in
// `T...`. This variable template is designed to accept unflattened OneOfs -- it will recurse
// manually through the OneOfs, meaning `nullableTypeCount<Maybe<OneOf<Maybe<U>>>> == 2`.
//
// Implements the "number of nullable member types" algorithm defined here:
// https://heycam.github.io/webidl/#dfn-number-of-nullable-member-types
template <typename... T>
constexpr size_t nullableTypeCount = 0;

template <typename T, typename... U>
constexpr size_t nullableTypeCount<T, U...> = nullableTypeCount<U...>;
template <typename T, typename... U>
constexpr size_t nullableTypeCount<kj::Maybe<T>, U...> =
    1 + nullableTypeCount<T> + nullableTypeCount<U...>;
template <typename... T, typename... U>
constexpr size_t nullableTypeCount<kj::OneOf<T...>, U...> =
    nullableTypeCount<T...> + nullableTypeCount<U...>;
// TODO(soon): What to do with Optional? Unwrap? Hard error? It's not nullable.

// =======================================================================================
// Distinguishable type categories
//
// Web IDL defines nine different categories of distinguishable types, which are used to validate
// union types. For a basic example, consider `kj::OneOf<double, int>`. From Web IDL's perspective,
// these are both numeric types, thus the union is invalid.
//
// Note that these categories do not cover all Web IDL types, like Promises. Such types are not
// allowed in unions under any circumstances.
//
// TODO(cleanup): Making these traits free variable templates is probably a mistake. Perhaps these
//   categories should be added to JsgKind and we should replace this:
//       `constexpr bool isResourcetype(T*)`
//   with:
//       `constexpr JsgKind getKind(T*)`

// True if T is a Web IDL dictionary type (modeled with JSG_STRUCT), false otherwise.
template <typename T, typename = void>
constexpr bool isDictionaryType = false;

// True if T is a Web IDL dictionary type (modeled with JSG_STRUCT), false otherwise.
template <typename T>
constexpr bool isDictionaryType<T, kj::VoidSfinae<decltype(T::JSG_KIND)>> =
    T::JSG_KIND == JsgKind::STRUCT;

// True if T is a Web IDL non-callback interface type (modeled with JSG_RESOURCE), false otherwise.
//
// Note: This covers Web IDL exception types as well. This doesn't seem to be a problem in practice,
//   but it's worth knowing that the Web IDL spec considers the two categories distinct.
template <typename T, typename = void>
constexpr bool isNonCallbackInterfaceType = false;

// True if T is a Web IDL non-callback interface type (modeled with JSG_RESOURCE), false otherwise.
//
// Note: This covers Web IDL exception types as well. This doesn't seem to be a problem in practice,
//   but it's worth knowing that the Web IDL spec considers the two categories distinct.
template <typename T>
constexpr bool isNonCallbackInterfaceType<T, kj::VoidSfinae<decltype(T::JSG_KIND)>> =
    T::JSG_KIND == JsgKind::RESOURCE;

// True if T is a Web IDL non-callback interface type (modeled with JSG_RESOURCE), false otherwise.
//
// Note: This covers Web IDL exception types as well. This doesn't seem to be a problem in practice,
//   but it's worth knowing that the Web IDL spec considers the two categories distinct.
template <typename T>
constexpr bool isNonCallbackInterfaceType<Ref<T>, kj::VoidSfinae<decltype(T::JSG_KIND)>> =
    T::JSG_KIND == JsgKind::RESOURCE;

template <typename T>
constexpr bool isBufferSourceType = kj::isSameType<T, kj::Array<kj::byte>>() ||
    kj::isSameType<T, kj::ArrayPtr<kj::byte>>() || kj::isSameType<T, kj::Array<const kj::byte>>() ||
    kj::isSameType<T, kj::ArrayPtr<const kj::byte>>() || kj::isSameType<T, jsg::BufferSource>();

template <typename T>
constexpr bool isRecordType = false;
template <typename K, typename V>
constexpr bool isRecordType<Dict<V, K>> = true;

template <typename T>
constexpr bool isBooleanType = kj::isSameType<T, bool>() || kj::isSameType<T, NonCoercible<bool>>();

template <typename T>
constexpr bool isIntegerType = kj::isSameType<T, int8_t>() || kj::isSameType<T, int16_t>() ||
    kj::isSameType<T, int>() || kj::isSameType<T, int64_t>() || kj::isSameType<T, uint8_t>() ||
    kj::isSameType<T, uint16_t>() || kj::isSameType<T, uint32_t>() ||
    kj::isSameType<T, uint64_t>() || kj::isSameType<T, v8::Local<v8::BigInt>>();

template <typename T>
constexpr bool isNumericType =
    isIntegerType<T> || kj::isSameType<T, double>() || kj::isSameType<T, NonCoercible<double>>();

template <typename T>
constexpr bool isStringType = kj::isSameType<T, kj::String>() || kj::isSameType<T, ByteString>() ||
    kj::isSameType<T, USVString>() || kj::isSameType<T, DOMString>() ||
    kj::isSameType<T, v8::Local<v8::String>>() || kj::isSameType<T, jsg::V8Ref<v8::String>>() ||
    kj::isSameType<T, NonCoercible<kj::String>>() || kj::isSameType<T, NonCoercible<USVString>>() ||
    kj::isSameType<T, NonCoercible<DOMString>>() || kj::isSameType<T, jsg::JsString>();

template <typename T>
constexpr bool isObjectType =
    kj::isSameType<T, v8::Local<v8::Object>>() || kj::isSameType<T, v8::Global<v8::Object>>();

template <typename T>
constexpr bool isSymbolType = false;
// TODO(soon): isSameType<T, v8::Local<v8::Symbol>>()?

template <typename T>
constexpr bool isCallbackFunctionType = false;
template <typename T>
constexpr bool isCallbackFunctionType<kj::Function<T>> = true;
template <typename T>
constexpr bool isCallbackFunctionType<Constructor<T>> = true;

// True if T is a Web IDL buffer source type, exception type, or non-callback interface type. The
// latter two cases are both modeled with JSG_RESOURCE_TYPE, which is why this trait only has two
// predicates, rather than three.
template <typename T, typename = void>
constexpr bool isInterfaceLikeType = isBufferSourceType<T> || isNonCallbackInterfaceType<T>;

template <typename T>
constexpr bool isDictionaryLikeType = isDictionaryType<T> || isRecordType<T>;
// TODO(someday): Or callback interface types. Callback interface types seem to be going the way of the
//   dodo -- fingers crossed that we won't have to implement them.

template <typename T>
constexpr bool isSequenceLikeType = false;
template <typename T>
constexpr bool isSequenceLikeType<kj::Array<T>> =
    !kj::isSameType<T, kj::byte>() && !kj::isSameType<T, const kj::byte>();
template <typename T>
constexpr bool isSequenceLikeType<Sequence<T>> = true;
// TODO(soon): And frozen array types.

// True if T is not listed in the table in Web IDL's distinguishable type algorithm:
// https://heycam.github.io/webidl/#dfn-distinguishable, step 4.
template <typename T>
constexpr bool isIndistinguishableType = !(isBooleanType<T> || isNumericType<T> ||
    isStringType<T> || isObjectType<T> || isSymbolType<T> || isInterfaceLikeType<T> ||
    isCallbackFunctionType<T> || isDictionaryLikeType<T> || isSequenceLikeType<T>);

template <typename... T>
constexpr bool hasDuplicateTypes = false;
template <typename T, typename U, typename... V>
constexpr bool hasDuplicateTypes<T, U, V...> =
    kj::isSameType<T, U>() || hasDuplicateTypes<T, V...> || hasDuplicateTypes<U, V...>;

template <typename... T>
struct FlattenedTypeTraits_ {
  static constexpr size_t dictionaryTypeCount = ((size_t)isDictionaryType<T> + ...);

  static constexpr size_t booleanTypeCount = ((size_t)isBooleanType<T> + ...);
  static constexpr size_t numericTypeCount = ((size_t)isNumericType<T> + ...);
  static constexpr size_t stringTypeCount = ((size_t)isStringType<T> + ...);
  static constexpr size_t objectTypeCount = ((size_t)isObjectType<T> + ...);
  static constexpr size_t symbolTypeCount = ((size_t)isSymbolType<T> + ...);
  static constexpr size_t interfaceLikeTypeCount = ((size_t)isInterfaceLikeType<T> + ...);
  static constexpr size_t callbackFunctionTypeCount = ((size_t)isCallbackFunctionType<T> + ...);
  static constexpr size_t dictionaryLikeTypeCount = ((size_t)isDictionaryLikeType<T> + ...);
  static constexpr size_t sequenceLikeTypeCount = ((size_t)isSequenceLikeType<T> + ...);

  static constexpr size_t hasDuplicateTypes = webidl::hasDuplicateTypes<T...>;
  static constexpr size_t hasIndistinguishableTypes = (isIndistinguishableType<T> || ...);
  static constexpr size_t hasOptionalTypes = (isOptional<T> || ...);
};

template <typename Traits, typename... T>
struct Flatten;
template <typename Traits>
struct Flatten<Traits>: Traits {};
template <template <typename...> class Traits, typename... T, typename U, typename... V>
struct Flatten<Traits<T...>, U, V...>: Flatten<Traits<T..., U>, V...> {};
template <template <typename...> class Traits, typename... T, typename U, typename... V>
struct Flatten<Traits<T...>, Ref<U>, V...>: Flatten<Traits<T...>, U, V...> {};
template <template <typename...> class Traits, typename... T, typename U, typename... V>
struct Flatten<Traits<T...>, kj::Maybe<U>, V...>: Flatten<Traits<T...>, U, V...> {};
template <template <typename...> class Traits, typename... T, typename... U, typename... V>
struct Flatten<Traits<T...>, kj::OneOf<U...>, V...>: Flatten<Traits<T...>, U..., V...> {};

// Flattens a list of types (recursively unwraps Maybes and OneOfs) and exposes some data about
// those types: number of dictionary types, whether or not there are duplicate types, presence of
// indistinguishable types, etc.
//
// Note: Web IDL dictates that we flatten nullables (Maybe) and unions (OneOf). We add one more
//   flattening: Ref<T> -> T. We do this because JSG has two models for non-callback interface
//   types: Ref<T> (unwrapped by reference) and T (unwrapped by copy/move). We need to be able to
//   catch ambiguous OneOfs like `kj::OneOf<Interface, Ref<Interface>>`.
template <typename... T>
using FlattenedTypeTraits = Flatten<FlattenedTypeTraits_<>, T...>;

// Instantiate to check that the type T satisfies the constraints on union types prescribed by
// Web IDL spec: https://heycam.github.io/webidl/#idl-union
template <typename T>
struct UnionTypeValidator {
  using Traits = FlattenedTypeTraits<T>;

  static_assert(nullableTypeCount<T> + Traits::dictionaryTypeCount <= 1,
      "A Web IDL union (OneOf) may contain at most one nullable or dictionary type.");

  static_assert(Traits::booleanTypeCount <= 1,
      "A Web IDL union (OneOf) may contain at most one boolean type.");
  static_assert(Traits::numericTypeCount <= 1,
      "A Web IDL union (OneOf) may contain at most one numeric type.");
  static_assert(
      Traits::stringTypeCount <= 1, "A Web IDL union (OneOf) may contain at most one string type.");
  static_assert(
      Traits::objectTypeCount <= 1, "A Web IDL union (OneOf) may contain at most one object type.");
  static_assert(Traits::objectTypeCount == 0 ||
          Traits::interfaceLikeTypeCount + Traits::callbackFunctionTypeCount +
                  Traits::dictionaryLikeTypeCount + Traits::sequenceLikeTypeCount ==
              0,
      "A Web IDL union (OneOf) may contain an object type only if it also contains no "
      "interface-like, callback function, dictionary-like, or sequence-like types.");
  static_assert(
      Traits::symbolTypeCount <= 1, "A Web IDL union (OneOf) may contain at most one symbol type.");
  static_assert(Traits::callbackFunctionTypeCount <= 1,
      "A Web IDL union (OneOf) may contain at most one callback function type.");
  // TODO(cleanup): This next check made it impossible to define a type for named top-level module
  //   exports, which are allowed to be objects or classes. I don't understand why this restriction
  //   existed since it's definitely possible to distinguish a function from a non-function. Do
  //   we really need to be enforcing WebIDL rules to the letter even when our type system is more
  //   expressive?
  //   static_assert(Traits::callbackFunctionTypeCount == 0 || Traits::dictionaryLikeTypeCount == 0,
  //       "A Web IDL union (OneOf) may contain a callback function type only if it also contains no "
  //       "dictionary-like types.");
  static_assert(Traits::dictionaryLikeTypeCount <= 1,
      "A Web IDL union (OneOf) may contain at most one dictionary-like type.");
  static_assert(Traits::sequenceLikeTypeCount <= 1,
      "A Web IDL union (OneOf) may contain at most one sequence-like type.");

  // There is no `Traits::interfaceLikeTypeCount <= 1` check because Web IDL unions can have
  // multiple interface-like types as long as:
  //
  //   1. They are not the same type.
  //   2. No single platform object implements more than one of the interfaces in question.
  //
  // Condition (1) will be taken care of with the `hasDuplicateTypes` check below (and is why
  // `FlattenedTypeTraits` unwraps `Ref`s). Condition (2) is difficult to guarantee, but unless
  // we start using multiple-inheritance in our API implementation types, we should be safe.

  static_assert(
      !Traits::hasDuplicateTypes, "A Web IDL union (OneOf) may not contain duplicate types.");
  // TODO(cleanup): This rule is incompatible with addEventListener(), whose second argument is
  //   allowed to be either a function or an object with a `handleEvent()` method. If such a
  //   fundamental web interface violates this rule, should we really be enforcing it?
  //   static_assert(!Traits::hasIndistinguishableTypes,
  //       "A Web IDL union (OneOf) may only contain distinguishable types, i.e., types which fall "
  //       "into one of the following categories: boolean, numeric, string, object, symbol, "
  //       "interface-like, callback function, dictionary-like, or sequence-like. See the definition "
  //       "of 'distinguishable' in the Web IDL spec for details.");
  static_assert(!Traits::hasOptionalTypes,
      "A Web IDL union (OneOf) may not contain any Optional<T> types. Optional<T> must only be "
      "used to mark optional function/method parameters and non-required members of a "
      "dictionary. Use Maybe<T> to represent nullable types.");
};

}  // namespace workerd::jsg::webidl
