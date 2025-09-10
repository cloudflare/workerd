// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/string.h>

#include <compare>
#include <cstdint>

namespace workerd {

// WD_STRONG_BOOL(StrongBool) defines a class type, `StrongBool`, which acts like a boolean flag,
// but with greater type safety.
//
// `StrongBool` has the following restrictions:
// - No default constructor: you must explicitly initialize values
// - No possibility for uninitialized values
// - No implicit conversion to or from boolean values
//
// `StrongBool` supports the following explicit and contextual boolean conversions:
// - Explicit conversion from boolean values: `StrongBool(true)`, `StrongBool(false)`
// - Explicit conversion to boolean values: `bool(StrongBool::YES)`, `StrongBool::NO.toBool()`
// - Contextual boolean conversion: `if (strongBool)`, `while (strongBool)`, `!strongBool`
//
// The `.toBool()` function exists to make safe, explicit conversion to `bool` more convenient,
// avoiding the verbosity of `static_cast` and the risk of C-style/functional casts.
//
// `StrongBool` supports the full suite of comparison and logical operators. Note that ! is
// supported via contextual conversion (`explicit operator bool()`), rather than `operator!()`.
//
// Logical operators (&& and ||) preserve the type of their operands when the operands are the same
// `StrongBool` type. Otherwise, contextual boolean conversion applies, and the operands will be
// converted to `bool` before the operator is invoked.
#define WD_STRONG_BOOL(Type)                                                                       \
  class Type final {                                                                               \
   public:                                                                                         \
    static const Type NO;                                                                          \
    static const Type YES;                                                                         \
    constexpr explicit Type(bool booleanValue): value(booleanValue ? Value::YES : Value::NO) {}    \
    constexpr explicit operator bool() const {                                                     \
      return toBool();                                                                             \
    }                                                                                              \
    constexpr bool toBool() const {                                                                \
      return value == YES;                                                                         \
    }                                                                                              \
    constexpr auto operator<=>(const Type&) const = default;                                       \
    constexpr Type operator&&(const Type& other) const {                                           \
      return Type(value == YES && other.value == YES);                                             \
    }                                                                                              \
    constexpr Type operator||(const Type& other) const {                                           \
      return Type(value == YES || other.value == YES);                                             \
    }                                                                                              \
                                                                                                   \
   private:                                                                                        \
    enum class Value : std::uint8_t { NO, YES };                                                   \
    constexpr Type(Value value): value(value) {}                                                   \
    Value value;                                                                                   \
  };                                                                                               \
  constexpr inline kj::LiteralStringConst KJ_STRINGIFY(Type value) {                               \
    return value ? #Type "::YES"_kjc : #Type "::NO"_kjc;                                           \
  }                                                                                                \
  inline constexpr Type Type::NO{Type::Value::NO};                                                 \
  inline constexpr Type Type::YES {                                                                \
    Type::Value::YES                                                                               \
  }

}  // namespace workerd
