// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "strong-bool.h"

#include <kj/test.h>

namespace workerd {

WD_STRONG_BOOL(Strongbad);
WD_STRONG_BOOL(Burninator);

constexpr Strongbad giveStrongbad() {
  return Strongbad::NO;
}
constexpr Burninator giveBurninator() {
  return Burninator::YES;
}
constexpr void takeStrongbad(Strongbad strongbadValue) {}
constexpr void takeBurninator(Burninator burninatorValue) {}

// TODO(soon): We don't have negative compile tests that I know of :(
KJ_TEST("WD_STRONG_BOOL compile failures") {
  [[maybe_unused]] Strongbad strongbadValue{Strongbad::NO};
  [[maybe_unused]] Burninator burninatorValue{Burninator::YES};
  [[maybe_unused]] bool booleanValue = false;
  [[maybe_unused]] int integerValue = 123;

  // No uninitialized values
  //Strongbad uninitialized;

  // No implicit conversion from bool or int
  //strongbad = false;
  //strongbad = 123;
  //strongbad = booleanValue;
  //strongbad = integerValue;
  //Strongbad s1 = false;
  //Strongbad s2 = 123;

  // No implicit conversion to bool or int
  //booleanValue = strongbadValue;
  //integerValue = strongbadValue;
  //bool b = strongbadValue;
  //int i = strongbadValue;
  //int i = Strongbad::NO;
  //bool b = Strongbad::YES;

  // No implicit conversion between strong bools.
  //strongbadValue = burninatorValue;
  //if (strongbadValue == burninatorValue) {}

  //takeBurninator(giveStrongbad());
  //takeStrongbad(giveBurninator());
}

KJ_TEST("WD_STRONG_BOOL can be explicitly converted to and from `bool`") {
  Strongbad strongbadNo{Strongbad::NO};
  Strongbad strongbadYes{Strongbad::YES};

  auto booleanValue = bool(strongbadNo);
  static_assert(kj::isSameType<decltype(booleanValue), bool>());

  auto strongbadNo2 = Strongbad(booleanValue);
  static_assert(kj::isSameType<decltype(strongbadNo2), Strongbad>());

  // Literals can be explicitly converted in both directions, too.
  static_assert(kj::isSameType<decltype(bool(Strongbad::NO)), bool>());
  static_assert(kj::isSameType<decltype(Strongbad(false)), Strongbad>());

  // Can't use static_assert because they're not constexpr. We'll test constexpr elsewhere.
  KJ_EXPECT(!bool(strongbadNo));
  KJ_EXPECT(bool(strongbadYes));
  KJ_EXPECT(Strongbad(false) == Strongbad::NO);
  KJ_EXPECT(Strongbad(true) == Strongbad::YES);
}

KJ_TEST("WD_STRONG_BOOL can be contextually converted to `bool`") {
  Strongbad strongbadNo{Strongbad::NO};
  Burninator burninatorYes{Burninator::YES};

  // It's a Strongbad ...
  static_assert(kj::isSameType<decltype(strongbadNo), Strongbad>());
  static_assert(kj::isSameType<decltype(Strongbad::NO), const Strongbad>());
  static_assert(kj::isSameType<decltype(Strongbad::YES), const Strongbad>());

  // ... until you use it in an explicitly boolean context.
  static_assert(kj::isSameType<decltype(!strongbadNo), bool>());
  static_assert(kj::isSameType<decltype(strongbadNo && burninatorYes), bool>());
  static_assert(kj::isSameType<decltype(strongbadNo || burninatorYes), bool>());

  // These are all contextually converted to `bool`s.
  if (strongbadNo) {}
  if (strongbadNo && burninatorYes) {}
  if (strongbadNo || burninatorYes) {}

  // Can't use static_assert because they're not constexpr. We'll test constexpr elsewhere.
  KJ_EXPECT(!strongbadNo);
  KJ_EXPECT(!Strongbad::NO);

  // TODO(someday): KJ magic asserts are not a boolean context :(
  KJ_EXPECT(!!burninatorYes);
  KJ_EXPECT(!!Strongbad::YES);
}

KJ_TEST("WD_STRONG_BOOL is constexpr") {
  if constexpr (constexpr auto s = giveStrongbad()) {
    static_assert(kj::isSameType<decltype(s), const Strongbad>());
  }

  constexpr Strongbad strongbadValue{Strongbad::NO};
  if constexpr (strongbadValue) {}
  if constexpr (Strongbad(true)) {}
  if constexpr (bool(Strongbad::NO)) {}
  if constexpr (Strongbad::NO || Strongbad::YES) {}
  if constexpr (Strongbad::NO && Strongbad::YES) {}
  [[maybe_unused]] constexpr auto order = Strongbad::YES <=> Strongbad::NO;

  static_assert(!strongbadValue);
}

KJ_TEST("WD_STRONG_BOOL comparison operators") {
  constexpr Strongbad strongbadNo{Strongbad::NO};
  constexpr Strongbad strongbadYes{Strongbad::YES};

  if constexpr (strongbadNo == strongbadYes) {}
  if constexpr (strongbadNo != strongbadYes) {}
  if constexpr (strongbadNo < strongbadYes) {}
  if constexpr (strongbadNo > strongbadYes) {}
  if constexpr (strongbadNo <= strongbadYes) {}
  if constexpr (strongbadNo >= strongbadYes) {}

  static_assert(strongbadNo == strongbadNo);
  static_assert(strongbadYes == strongbadYes);
  static_assert(!(strongbadNo != strongbadNo));
  static_assert(!(strongbadYes != strongbadYes));
  static_assert(!(strongbadNo < strongbadNo));
  static_assert(!(strongbadYes < strongbadYes));
  static_assert(strongbadNo < strongbadYes);
  static_assert(strongbadYes > strongbadNo);
  static_assert(strongbadYes >= strongbadNo);
  static_assert(strongbadNo <= strongbadYes);
  static_assert(!(strongbadYes <= strongbadNo));
  static_assert(!(strongbadNo >= strongbadYes));
}

KJ_TEST("WD_STRONG_BOOL logical operators") {
  constexpr Strongbad strongbadNo{Strongbad::NO};
  constexpr Strongbad strongbadYes{Strongbad::YES};

  static_assert(kj::isSameType<decltype(strongbadNo && strongbadYes), Strongbad>());
  static_assert(kj::isSameType<decltype(strongbadNo || strongbadYes), Strongbad>());

  // Logical operators with contextual conversion
  if (strongbadNo && strongbadYes) {}
  if (strongbadNo || strongbadYes) {}

  static_assert((strongbadNo && strongbadNo) == Strongbad::NO);
  static_assert((strongbadNo && strongbadYes) == Strongbad::NO);
  static_assert((strongbadYes && strongbadNo) == Strongbad::NO);
  static_assert((strongbadYes && strongbadYes) == Strongbad::YES);
  static_assert((strongbadNo || strongbadNo) == Strongbad::NO);
  static_assert((strongbadNo || strongbadYes) == Strongbad::YES);
  static_assert((strongbadYes || strongbadNo) == Strongbad::YES);
  static_assert((strongbadYes || strongbadYes) == Strongbad::YES);
}

KJ_TEST("WD_STRONG_BOOL can be stringified") {
  constexpr Strongbad strongbadNo{Strongbad::NO};
  constexpr Strongbad strongbadYes{Strongbad::YES};
  constexpr auto stringNo = kj::_::STR * strongbadNo;
  constexpr auto stringYes = kj::_::STR * strongbadYes;
  static_assert(stringNo == "Strongbad::NO"_kjc);
  static_assert(stringYes == "Strongbad::YES"_kjc);

  static_assert(kj::_::STR * Burninator::NO == "Burninator::NO"_kjc);
  static_assert(kj::_::STR * Burninator::YES == "Burninator::YES"_kjc);
}

}  // namespace workerd
