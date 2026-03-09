// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

// Static unit tests for Web IDL type concepts.

// DictionaryType concept tests
static_assert(webidl::DictionaryType<TestStruct>);
static_assert(!webidl::DictionaryType<NumberBox>);
static_assert(!webidl::DictionaryType<kj::Maybe<TestStruct>>);

// NonCallbackInterfaceType concept tests
static_assert(!webidl::NonCallbackInterfaceType<TestStruct>);
static_assert(webidl::NonCallbackInterfaceType<NumberBox>);
static_assert(!webidl::NonCallbackInterfaceType<kj::Maybe<NumberBox>>);
static_assert(webidl::NonCallbackInterfaceType<Ref<NumberBox>>);

// Backward-compatible variable template tests (these delegate to concepts)
static_assert(webidl::isNonCallbackInterfaceType<TestStruct> == false);
static_assert(webidl::isNonCallbackInterfaceType<NumberBox> == true);
static_assert(webidl::isNonCallbackInterfaceType<kj::Maybe<NumberBox>> == false);

// Additional type category concept tests
static_assert(webidl::StringType<kj::String>);
static_assert(webidl::StringType<USVString>);
static_assert(webidl::StringType<DOMString>);
static_assert(!webidl::StringType<int>);

static_assert(webidl::NumericType<int>);
static_assert(webidl::NumericType<double>);
static_assert(!webidl::NumericType<kj::String>);

static_assert(webidl::BooleanType<bool>);
static_assert(!webidl::BooleanType<int>);

static_assert(webidl::InterfaceLikeType<NumberBox>);
static_assert(webidl::InterfaceLikeType<kj::Array<kj::byte>>);
static_assert(!webidl::InterfaceLikeType<kj::String>);

static_assert(webidl::DistinguishableType<kj::String>);
static_assert(webidl::DistinguishableType<int>);
static_assert(webidl::DistinguishableType<bool>);
static_assert(webidl::DistinguishableType<NumberBox>);

static_assert(webidl::nullableTypeCount<int> == 0);
static_assert(webidl::nullableTypeCount<kj::Maybe<int>> == 1);
static_assert(webidl::nullableTypeCount<kj::Maybe<int>, kj::Maybe<kj::String>> == 2);
static_assert(webidl::nullableTypeCount<kj::OneOf<kj::Maybe<int>, kj::Maybe<kj::String>>> == 2);
static_assert(
    webidl::nullableTypeCount<kj::Maybe<kj::OneOf<kj::Maybe<int>, kj::Maybe<kj::String>>>> == 3);
static_assert(webidl::nullableTypeCount<kj::OneOf<kj::Maybe<int>, kj::Maybe<kj::String>>,
                  kj::OneOf<kj::Maybe<bool>, kj::Maybe<char>>> == 4);
static_assert(webidl::nullableTypeCount<kj::Maybe<kj::OneOf<kj::Maybe<int>>>> == 2);

static_assert(webidl::hasDuplicateTypes<int> == false);
static_assert(webidl::hasDuplicateTypes<int, int> == true);
static_assert(webidl::hasDuplicateTypes<int, bool> == false);
static_assert(webidl::hasDuplicateTypes<bool, int, int> == true);
static_assert(webidl::hasDuplicateTypes<int, bool, int> == true);
static_assert(webidl::hasDuplicateTypes<int, int, bool> == true);
static_assert(webidl::hasDuplicateTypes<int, int, bool, char> == true);
static_assert(webidl::hasDuplicateTypes<int, bool, int, char> == true);
static_assert(webidl::hasDuplicateTypes<int, bool, char, int> == true);
static_assert(webidl::hasDuplicateTypes<bool, int, char, int> == true);
static_assert(webidl::hasDuplicateTypes<bool, char, int, int> == true);

static_assert(webidl::FlattenedTypeTraits<kj::String, USVString>::stringTypeCount == 2);
static_assert(webidl::FlattenedTypeTraits<kj::String, DOMString>::stringTypeCount == 2);

// =====================================================================================
// ArgumentIndexes tests (meta.h)

// Member function - no magic param.
struct Dummy {
  int noMagic(int, double, bool);
  int withLock(Lock&, int, double);
  int withInfo(const v8::FunctionCallbackInfo<v8::Value>&, int);
  int constNoMagic(int) const;
  int constWithLock(Lock&, int, double) const;
  int constWithInfo(const v8::FunctionCallbackInfo<v8::Value>&) const;
};

static_assert(
    kj::isSameType<ArgumentIndexes<decltype(&Dummy::noMagic)>, kj::_::Indexes<0, 1, 2>>());
static_assert(kj::isSameType<ArgumentIndexes<decltype(&Dummy::withLock)>, kj::_::Indexes<0, 1>>());
static_assert(kj::isSameType<ArgumentIndexes<decltype(&Dummy::withInfo)>, kj::_::Indexes<0>>());
static_assert(kj::isSameType<ArgumentIndexes<decltype(&Dummy::constNoMagic)>, kj::_::Indexes<0>>());
static_assert(
    kj::isSameType<ArgumentIndexes<decltype(&Dummy::constWithLock)>, kj::_::Indexes<0, 1>>());
static_assert(kj::isSameType<ArgumentIndexes<decltype(&Dummy::constWithInfo)>, kj::_::Indexes<>>());

// Free functions.
static_assert(kj::isSameType<ArgumentIndexes<int(int, int)>, kj::_::Indexes<0, 1>>());
static_assert(kj::isSameType<ArgumentIndexes<void(Lock&, int)>, kj::_::Indexes<0>>());
static_assert(kj::isSameType<ArgumentIndexes<void()>, kj::_::Indexes<>>());

// =====================================================================================
// requiredArgumentCount tests (meta.h + web-idl.h)

// All required - count equals total visible args.
static_assert(requiredArgumentCount<int(int, double, bool)> == 3);
static_assert(requiredArgumentCount<int(Lock&, int, double, bool)> == 3);

// No args �� length is 0.
static_assert(requiredArgumentCount<void()> == 0);
static_assert(requiredArgumentCount<void(Lock&)> == 0);

// Optional args stop the count.
static_assert(requiredArgumentCount<void(int, Optional<int>)> == 1);
static_assert(requiredArgumentCount<void(int, double, Optional<int>)> == 2);
static_assert(requiredArgumentCount<void(Optional<int>)> == 0);
static_assert(requiredArgumentCount<void(Lock&, int, Optional<int>)> == 1);

// LenientOptional also stops the count.
static_assert(requiredArgumentCount<void(int, LenientOptional<int>)> == 1);

// TypeHandler<T> is invisible - does not count and does not stop.
static_assert(requiredArgumentCount<void(TypeHandler<int>&, int, double)> == 2);
static_assert(requiredArgumentCount<void(int, TypeHandler<int>&, double)> == 2);
static_assert(requiredArgumentCount<void(int, TypeHandler<int>&, Optional<double>)> == 1);
// Arguments following optionals are not counted
static_assert(requiredArgumentCount<void(int, TypeHandler<int>&, Optional<double>, int)> == 1);

// Arguments<T> is invisible - does not count and does not stop.
static_assert(requiredArgumentCount<void(int, Arguments<int>)> == 1);

// Member functions.
static_assert(requiredArgumentCount<decltype(&Dummy::noMagic)> == 3);
static_assert(requiredArgumentCount<decltype(&Dummy::withLock)> == 2);
static_assert(requiredArgumentCount<decltype(&Dummy::withInfo)> == 1);
static_assert(requiredArgumentCount<decltype(&Dummy::constNoMagic)> == 1);
static_assert(requiredArgumentCount<decltype(&Dummy::constWithLock)> == 2);
static_assert(requiredArgumentCount<decltype(&Dummy::constWithInfo)> == 0);

// =====================================================================================
// isValuelessArg tests (web-idl.h)

static_assert(detail::isValuelessArg<TypeHandler<int>> == true);
static_assert(detail::isValuelessArg<Arguments<int>> == true);
static_assert(detail::isValuelessArg<int> == false);
static_assert(detail::isValuelessArg<Optional<int>> == false);
static_assert(detail::isValuelessArg<kj::String> == false);

KJ_TEST("web-idl meta") {
  // Nothing to actually do here; tests are compile-time
}

}  // namespace
}  // namespace workerd::jsg::test
