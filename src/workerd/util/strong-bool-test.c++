// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "strong-bool.h"

#include <kj/test.h>

#include <type_traits>

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

#define CAN_DECLARE(DECLARATION, TYPE)                                                             \
  {                                                                                                \
    /* wrap it in a lambda to make it embeddable inside functions */                               \
    constexpr auto can_declare = [&]<typename T>() -> bool {                                       \
      /* Using a template to delay compilation into the requires */                                \
      static_assert(requires(T t) { DECLARATION; }, "Can declare");                                \
      return true;                                                                                 \
    };                                                                                             \
    KJ_ASSERT(can_declare.template operator()<TYPE>(), "Can declare"); /* Easier to debug */       \
  }

#define CAN_NOT_DECLARE(DECLARATION, TYPE)                                                         \
  {                                                                                                \
    /* wrap it in a lambda to make it embeddable inside functions */                               \
    constexpr auto can_not_declare = [&]<typename T>() -> bool {                                   \
      /* Using a template to delay compilation into the requires */                                \
      static_assert(!requires(T t) { DECLARATION; }, "Can not declare");                           \
      return false;                                                                                \
    };                                                                                             \
    KJ_ASSERT(!can_not_declare.template operator()<TYPE>(), "Can not declare");                    \
  }

KJ_TEST("WD_STRONG_BOOL compile failures") {
  [[maybe_unused]] Strongbad strongbadValue{Strongbad::NO};
  [[maybe_unused]] Burninator burninatorValue{Burninator::YES};
  [[maybe_unused]] bool booleanValue = false;
  [[maybe_unused]] int integerValue = 123;

  // No uninitialized values
  //Strongbad uninitialized;
  CAN_DECLARE(T{}, bool);
  CAN_NOT_DECLARE(T{}, Strongbad);
  static_assert(!std::is_default_constructible_v<Strongbad>, "Should fail");

  // No implicit conversion from bool or int
  //strongbad = false;
  CAN_DECLARE(t = false, bool);
  CAN_NOT_DECLARE(t = false, Strongbad);
  //strongbad = 123;
  CAN_DECLARE(t = 123, bool);
  CAN_NOT_DECLARE(t = 123, Strongbad);
  //strongbad = booleanValue;
  CAN_DECLARE(t = booleanValue, int);
  CAN_NOT_DECLARE(t = 123, Strongbad);
  //strongbad = integerValue;
  CAN_DECLARE(t = integerValue, bool);
  CAN_NOT_DECLARE(t = integerValue, Strongbad);
  //Strongbad s1 = false;
  static_assert(
      !std::is_convertible_v<Strongbad, bool>, "Should not be implicitly convertible from bool");
  //Strongbad s2 = 123;
  static_assert(
      !std::is_convertible_v<Strongbad, int>, "Should not be implicitly convertible from int");

  // No implicit conversion to bool or int
  //booleanValue = strongbadValue;
  CAN_NOT_DECLARE(t = strongbadValue, bool);
  CAN_NOT_DECLARE(t = Strongbad::YES, bool);
  //integerValue = strongbadValue;
  CAN_NOT_DECLARE(t = strongbadValue, int);
  CAN_NOT_DECLARE(t = Strongbad::YES, int);
  //bool b = strongbadValue;
  //bool b = Strongbad::YES;
  static_assert(
      !std::is_convertible_v<bool, Strongbad>, "Should not be implicitly convertible to Strongbad");
  //int i = strongbadValue;
  //int i = Strongbad::NO;
  static_assert(
      !std::is_convertible_v<int, Strongbad>, "Should not be implicitly convertible to Strongbad");

  // No implicit conversion between strong bools.
  //strongbadValue = burninatorValue;
  CAN_NOT_DECLARE(t = burninatorValue, Strongbad);
  //if (strongbadValue == burninatorValue) {}
  CAN_NOT_DECLARE(t == burninatorValue, Strongbad);

  //takeBurninator(giveStrongbad());
  CAN_NOT_DECLARE(takeBurninator(t), Strongbad);
  //takeStrongbad(giveBurninator());
  CAN_NOT_DECLARE(takeStrongbad(t), Burninator);
}

KJ_TEST("WD_STRONG_BOOL can be explicitly converted to and from `bool`") {
  Strongbad strongbadNo{Strongbad::NO};
  Strongbad strongbadYes{Strongbad::YES};

  auto booleanValue = strongbadNo.toBool();
  static_assert(kj::isSameType<decltype(booleanValue), bool>());

  auto strongbadNo2 = Strongbad(booleanValue);
  static_assert(kj::isSameType<decltype(strongbadNo2), Strongbad>());

  // Literals can be explicitly converted in both directions, too.
  static_assert(kj::isSameType<decltype(Strongbad::NO.toBool()), bool>());
  static_assert(kj::isSameType<decltype(Strongbad(false)), Strongbad>());

  // Can't use static_assert because they're not constexpr. We'll test constexpr elsewhere.
  KJ_EXPECT(!strongbadNo.toBool());
  KJ_EXPECT(strongbadYes.toBool());
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
  if constexpr (Strongbad::NO.toBool()) {}
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

  // NOLINTBEGIN(misc-redundant-expression)
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
  // NOLINTEND(misc-redundant-expression)
}

KJ_TEST("WD_STRONG_BOOL logical operators") {
  constexpr Strongbad strongbadNo{Strongbad::NO};
  constexpr Strongbad strongbadYes{Strongbad::YES};

  static_assert(kj::isSameType<decltype(strongbadNo && strongbadYes), Strongbad>());
  static_assert(kj::isSameType<decltype(strongbadNo || strongbadYes), Strongbad>());

  // Logical operators with contextual conversion
  if (strongbadNo && strongbadYes) {}
  if (strongbadNo || strongbadYes) {}

  // NOLINTBEGIN(misc-redundant-expression)
  static_assert((strongbadNo && strongbadNo) == Strongbad::NO);
  static_assert((strongbadNo && strongbadYes) == Strongbad::NO);
  static_assert((strongbadYes && strongbadNo) == Strongbad::NO);
  static_assert((strongbadYes && strongbadYes) == Strongbad::YES);
  static_assert((strongbadNo || strongbadNo) == Strongbad::NO);
  static_assert((strongbadNo || strongbadYes) == Strongbad::YES);
  static_assert((strongbadYes || strongbadNo) == Strongbad::YES);
  static_assert((strongbadYes || strongbadYes) == Strongbad::YES);
  // NOLINTEND(misc-redundant-expression)
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
