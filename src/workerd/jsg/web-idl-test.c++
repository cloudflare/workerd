// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

// Static unit tests for certain Web IDL traits.
static_assert(webidl::isDictionaryType<TestStruct> == true);
static_assert(webidl::isDictionaryType<NumberBox> == false);
static_assert(webidl::isDictionaryType<kj::Maybe<TestStruct>> == false);

static_assert(webidl::isNonCallbackInterfaceType<TestStruct> == false);
static_assert(webidl::isNonCallbackInterfaceType<NumberBox> == true);
static_assert(webidl::isNonCallbackInterfaceType<kj::Maybe<NumberBox>> == false);

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

static_assert(webidl::FlattenedTypeTraits<kj::String, ByteString>::stringTypeCount == 2);

KJ_TEST("web-idl meta") {
  // Nothing to actually do here; tests are compile-time
}

}  // namespace
}  // namespace workerd::jsg::test
