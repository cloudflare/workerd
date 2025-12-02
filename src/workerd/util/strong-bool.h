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
//
// This macro can be used both at namespace scope and inside class definitions.
#define WD_STRONG_BOOL(Type)                                                                       \
  class Type final {                                                                               \
    enum class InitValue : bool { NO = false, YES = true };                                        \
                                                                                                   \
   public:                                                                                         \
    static constexpr auto NO = InitValue::NO;                                                      \
    static constexpr auto YES = InitValue::YES;                                                    \
    constexpr Type(const InitValue& initValue): value(initValue == InitValue::YES) {}              \
    constexpr explicit Type(bool booleanValue): value(booleanValue) {}                             \
    constexpr explicit operator bool() const {                                                     \
      return toBool();                                                                             \
    }                                                                                              \
    constexpr bool toBool() const {                                                                \
      return value;                                                                                \
    }                                                                                              \
    constexpr auto operator<=>(const Type&) const = default;                                       \
    constexpr Type operator&&(const Type& other) const {                                           \
      return Type(value && other.value);                                                           \
    }                                                                                              \
    constexpr Type operator||(const Type& other) const {                                           \
      return Type(value || other.value);                                                           \
    }                                                                                              \
    friend constexpr kj::LiteralStringConst KJ_STRINGIFY(const Type& val) {                        \
      return val ? #Type "::YES"_kjc : #Type "::NO"_kjc;                                           \
    }                                                                                              \
                                                                                                   \
   private:                                                                                        \
    bool value;                                                                                    \
  }

}  // namespace workerd
