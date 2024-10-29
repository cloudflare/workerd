// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// Handling of various basic value types: numbers, booleans, strings, optionals, maybes, variants,
// arrays, buffers, dicts.

#include "simdutf.h"
#include "util.h"
#include "web-idl.h"
#include "wrappable.h"

#include <kj/debug.h>
#include <kj/one-of.h>
#include <kj/time.h>

namespace workerd::jsg {

// =======================================================================================
// Primitives (numbers, booleans)

// TypeWrapper mixin for numbers and booleans.
//
// This wrapper has extra wrap() overloads that take an isolate instead of a context. This is used
// to implement static constants in JavaScript: we need to be able to wrap C++ constants in V8
// values before a context has been entered.
//
// Note that we can't generally change the wrap(context, ...) functions to wrap(isolate, ...)
// because ResourceWrapper<TW, T>::wrap() needs the context to create new object instances.
class PrimitiveWrapper {
public:
  static constexpr const char* getName(double*) {
    return "number";
  }

  v8::Local<v8::Number> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, double value) {
    return wrap(context->GetIsolate(), creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, double value) {
    return v8::Number::New(isolate, value);
  }

  kj::Maybe<double> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      double*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return check(handle->ToNumber(context))->Value();
  }

  static constexpr const char* getName(int8_t*) {
    return "byte";
  }

  v8::Local<v8::Number> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, int8_t value) {
    return wrap(context->GetIsolate(), creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, int8_t value) {
    return v8::Integer::New(isolate, value);
  }

  kj::Maybe<int8_t> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      int8_t*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto value = check(handle->ToNumber(context))->Value();

    JSG_REQUIRE(
        isFinite(value), TypeError, "The value cannot be converted because it is not an integer.");

    JSG_REQUIRE(value <= int8_t(kj::maxValue) && value >= int8_t(kj::minValue), TypeError,
        kj::str("Value out of range. Must be between ", int8_t(kj::minValue), " and ",
            int8_t(kj::maxValue), " (inclusive)."));

    return int8_t(value);
  }

  static constexpr const char* getName(uint8_t*) {
    return "octet";
  }

  v8::Local<v8::Number> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, uint8_t value) {
    return wrap(context->GetIsolate(), creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, uint8_t value) {
    return v8::Integer::NewFromUnsigned(isolate, value);
  }

  kj::Maybe<uint8_t> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      uint8_t*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto value = check(handle->ToNumber(context))->Value();
    JSG_REQUIRE(
        isFinite(value), TypeError, "The value cannot be converted because it is not an integer.");

    JSG_REQUIRE(value >= 0, TypeError,
        "The value cannot be converted because it is negative and this "
        "API expects a positive number.");

    JSG_REQUIRE(value <= uint8_t(kj::maxValue), TypeError,
        kj::str("Value out of range. Must be less than or equal to ", uint8_t(kj::maxValue), "."));

    return uint8_t(value);
  }

  static constexpr const char* getName(int16_t*) {
    return "short integer";
  }

  v8::Local<v8::Number> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, int16_t value) {
    return wrap(context->GetIsolate(), creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, int16_t value) {
    return v8::Number::New(isolate, value);
  }

  kj::Maybe<int16_t> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      int16_t*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto value = check(handle->ToNumber(context))->Value();

    JSG_REQUIRE(
        isFinite(value), TypeError, "The value cannot be converted because it is not an integer.");

    JSG_REQUIRE(value <= int16_t(kj::maxValue) && value >= int16_t(kj::minValue), TypeError,
        kj::str("Value out of range. Must be between ", int16_t(kj::minValue), " and ",
            int16_t(kj::maxValue), " (inclusive)."));

    return int16_t(value);
  }

  static constexpr const char* getName(uint16_t*) {
    return "unsigned short integer";
  }

  v8::Local<v8::Number> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, uint16_t value) {
    return wrap(context->GetIsolate(), creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, uint16_t value) {
    return v8::Integer::NewFromUnsigned(isolate, value);
  }

  kj::Maybe<uint16_t> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      uint16_t*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto value = check(handle->ToNumber(context))->Value();
    JSG_REQUIRE(
        isFinite(value), TypeError, "The value cannot be converted because it is not an integer.");

    JSG_REQUIRE(value >= 0, TypeError,
        "The value cannot be converted because it is negative and this "
        "API expects a positive number.");

    JSG_REQUIRE(value <= uint16_t(kj::maxValue), TypeError,
        kj::str("Value out of range. Must be less than or equal to ", uint16_t(kj::maxValue), "."));

    return uint16_t(value);
  }

  static constexpr const char* getName(int*) {
    return "integer";
  }

  v8::Local<v8::Number> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, int value) {
    return wrap(context->GetIsolate(), creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, int value) {
    return v8::Number::New(isolate, value);
  }

  kj::Maybe<int> tryUnwrap(v8::Local<v8::Context> context,
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
    JSG_REQUIRE(value <= int(kj::maxValue) && value >= int(kj::minValue), TypeError,
        kj::str("Value out of range. Must be between ", int(kj::minValue), " and ",
            int(kj::maxValue), " (inclusive)."));

    return int(value);
  }

  static constexpr const char* getName(uint32_t*) {
    return "unsigned integer";
  }

  v8::Local<v8::Number> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, uint32_t value) {
    return wrap(context->GetIsolate(), creator, value);
  }

  v8::Local<v8::Number> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, uint32_t value) {
    return v8::Integer::NewFromUnsigned(isolate, value);
  }

  kj::Maybe<uint32_t> tryUnwrap(v8::Local<v8::Context> context,
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

    JSG_REQUIRE(value <= uint32_t(kj::maxValue), TypeError,
        kj::str("Value out of range. Must be less than or equal to ", uint32_t(kj::maxValue), "."));

    return uint32_t(value);
  }

  static constexpr const char* getName(uint64_t*) {
    return "bigint";
  }

  v8::Local<v8::BigInt> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, uint64_t value) {
    return wrap(context->GetIsolate(), creator, value);
  }

  v8::Local<v8::BigInt> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, uint64_t value) {
    return v8::BigInt::New(isolate, value);
  }

  kj::Maybe<uint64_t> tryUnwrap(v8::Local<v8::Context> context,
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

    JSG_REQUIRE(value <= uint64_t(kj::maxValue), TypeError,
        kj::str("Value out of range. Must be less than or equal to ", uint64_t(kj::maxValue), "."));

    return uint64_t(value);
  }

  static constexpr const char* getName(int64_t*) {
    return "bigint";
  }

  v8::Local<v8::BigInt> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, int64_t value) {
    return wrap(context->GetIsolate(), creator, value);
  }

  v8::Local<v8::BigInt> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, int64_t value) {
    return v8::BigInt::New(isolate, value);
  }

  kj::Maybe<int64_t> tryUnwrap(v8::Local<v8::Context> context,
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

    JSG_REQUIRE(value <= int64_t(kj::maxValue) && value >= int64_t(kj::minValue), TypeError,
        kj::str("Value out of range. Must be between ", int64_t(kj::minValue), " and ",
            int64_t(kj::maxValue), " (inclusive)."));

    return int64_t(value);
  }

  static constexpr const char* getName(bool*) {
    return "boolean";
  }

  template <typename T, typename = kj::EnableIf<kj::isSameType<T, bool>()>>
  v8::Local<v8::Boolean> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, T value) {
    // The template is needed to prevent this overload from being chosen for arbitrary types that
    // can convert to bool, such as pointers.
    return wrap(context->GetIsolate(), creator, value);
  }

  template <typename T, typename = kj::EnableIf<kj::isSameType<T, bool>()>>
  v8::Local<v8::Boolean> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, T value) {
    // The template is needed to prevent this overload from being chosen for arbitrary types that
    // can convert to bool, such as pointers.
    return v8::Boolean::New(isolate, value);
  }

  kj::Maybe<bool> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      bool*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return handle->ToBoolean(context->GetIsolate())->Value();
  }
};

// =======================================================================================
// Name
template <typename TypeWrapper>
class NameWrapper {
public:
  static constexpr const char* getName(Name*) {
    return "string or Symbol";
  }

  v8::Local<v8::Value> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, Name value) {
    auto isolate = context->GetIsolate();
    KJ_SWITCH_ONEOF(value.getUnwrapped(isolate)) {
      KJ_CASE_ONEOF(string, kj::StringPtr) {
        auto& wrapper = static_cast<TypeWrapper&>(*this);
        return wrapper.wrap(isolate, creator, kj::str(string));
      }
      KJ_CASE_ONEOF(symbol, v8::Local<v8::Symbol>) {
        return symbol;
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Maybe<Name> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Name*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsSymbol()) {
      return Name(Lock::from(context->GetIsolate()), handle.As<v8::Symbol>());
    }

    // Since most things are coercible to a string, this ought to catch pretty much
    // any value other than symbol
    auto& wrapper = static_cast<TypeWrapper&>(*this);
    KJ_IF_SOME(string, wrapper.tryUnwrap(context, handle, (kj::String*)nullptr, parentObject)) {
      return Name(kj::mv(string));
    }

    return kj::none;
  }
};

// =======================================================================================
// Strings

// TypeWrapper mixin for strings.
//
// This wrapper has an extra wrap() overload that takes an isolate instead of a context, for the
// same reason discussed in PrimitiveWrapper.
class StringWrapper {
public:
  static constexpr const char* getName(kj::String*) {
    return "string";
  }
  // TODO(someday): This conflates USVStrings, which must have valid code points, with DOMStrings,
  //   which needn't have valid code points.

  static constexpr const char* getName(kj::ArrayPtr<const char>*) {
    return "string";
  }
  static constexpr const char* getName(kj::Array<const char>*) {
    return "string";
  }

  static constexpr const char* getName(ByteString*) {
    return "ByteString";
  }
  // TODO(cleanup): Move to a HeaderStringWrapper in the api directory.

  v8::Local<v8::String> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      kj::ArrayPtr<const char> value) {
    return v8Str(context->GetIsolate(), value);
  }

  v8::Local<v8::String> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      kj::Array<const char> value) {
    return wrap(context, creator, value.asPtr());
  }

  v8::Local<v8::String> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, kj::StringPtr value) {
    return v8Str(isolate, value);
  }

  v8::Local<v8::String> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      const ByteString& value) {
    // TODO(cleanup): Move to a HeaderStringWrapper in the api directory.
    return wrap(context, creator, value.asPtr());
  }

  kj::Maybe<kj::String> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::String*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    v8::Local<v8::String> str = check(handle->ToString(context));
    v8::Isolate* isolate = context->GetIsolate();
    auto buf = kj::heapArray<char>(str->Utf8Length(isolate) + 1);
    str->WriteUtf8(isolate, buf.begin(), buf.size());
    buf[buf.size() - 1] = 0;
    return kj::String(kj::mv(buf));
  }

  kj::Maybe<ByteString> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      ByteString*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // TODO(cleanup): Move to a HeaderStringWrapper in the api directory.
    v8::Local<v8::String> str = check(handle->ToString(context));
    auto result = ByteString(KJ_ASSERT_NONNULL(
        tryUnwrap(context, str, static_cast<kj::String*>(nullptr), parentObject)));

    if (!simdutf::validate_ascii(result.begin(), result.size())) {
      // If storage is one-byte or the string contains only one-byte
      // characters, we know that it contains extended ASCII characters.
      //
      // The order of execution matters, since ContainsOnlyOneByte()
      // will scan the whole string for two-byte storage.
      if (str->ContainsOnlyOneByte()) {
        result.warning = ByteString::Warning::CONTAINS_EXTENDED_ASCII;
      } else {
        // Storage is two-bytes and it contains two-byte characters.
        result.warning = ByteString::Warning::CONTAINS_UNICODE;
      }
    }

    return kj::mv(result);
  }
};

// =======================================================================================
// Optional (value or undefined) and Maybe (value or null)

template <typename... T>
constexpr bool isUnionType(kj::OneOf<T...>*) {
  return true;
}

template <typename T>
constexpr bool isUnionType(T*) {
  return false;
}

// TypeWrapper mixin for optionals.
template <typename TypeWrapper>
class OptionalWrapper {
public:
  template <typename U>
  static constexpr decltype(auto) getName(Optional<U>*) {
    return TypeWrapper::getName((kj::Decay<U>*)nullptr);
  }

  template <typename U>
  v8::Local<v8::Value> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, Optional<U> ptr) {
    KJ_IF_SOME(p, ptr) {
      return static_cast<TypeWrapper*>(this)->wrap(context, creator, kj::fwd<U>(p));
    } else {
      return v8::Undefined(context->GetIsolate());
    }
  }

  template <typename U>
  kj::Maybe<Optional<U>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Optional<U>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsUndefined()) {
      return Optional<U>(kj::none);
    } else {
      return static_cast<TypeWrapper*>(this)
          ->tryUnwrap(context, handle, (kj::Decay<U>*)nullptr, parentObject)
          .map([](auto&& value) -> Optional<U> { return kj::fwd<decltype(value)>(value); });
    }
  }
};

// TypeWrapper mixin for lenient optionals.
template <typename TypeWrapper>
class LenientOptionalWrapper {
public:
  template <typename U>
  static constexpr decltype(auto) getName(LenientOptional<U>*) {
    return TypeWrapper::getName((kj::Decay<U>*)nullptr);
  }

  template <typename U>
  v8::Local<v8::Value> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      LenientOptional<U> ptr) {
    KJ_IF_SOME(p, ptr) {
      return static_cast<TypeWrapper*>(this)->wrap(context, creator, kj::fwd<U>(p));
    } else {
      return v8::Undefined(context->GetIsolate());
    }
  }

  template <typename U>
  kj::Maybe<LenientOptional<U>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      LenientOptional<U>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsUndefined()) {
      return LenientOptional<U>(kj::none);
    } else {
      KJ_IF_SOME(unwrapped,
          static_cast<TypeWrapper*>(this)->tryUnwrap(
              context, handle, (kj::Decay<U>*)nullptr, parentObject)) {
        return LenientOptional<U>(kj::mv(unwrapped));
      } else {
        return LenientOptional<U>(kj::none);
      }
    }
  }
};

// TypeWrapper mixin for maybes.
template <typename TypeWrapper>
class MaybeWrapper {

public:
  // The constructor here is a bit of a hack. The config is optional and might not be a JsgConfig
  // object (or convertible to a JsgConfig) if is provided. However, because of the way TypeWrapper
  // inherits MaybeWrapper, we always end up passing a config option (which might be std::nullptr_t)
  // The getConfig allows us to handle any case using reasonable defaults.
  MaybeWrapper(const auto& config): config(getConfig(config)) {}

  template <typename MetaConfiguration>
  void updateConfiguration(MetaConfiguration&& configuration) {
    config = getConfig(kj::fwd<MetaConfiguration>(configuration));
  }
  template <typename U>
  static constexpr decltype(auto) getName(kj::Maybe<U>*) {
    return TypeWrapper::getName((kj::Decay<U>*)nullptr);
  }

  template <typename U>
  v8::Local<v8::Value> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, kj::Maybe<U> ptr) {
    KJ_IF_SOME(p, ptr) {
      return static_cast<TypeWrapper*>(this)->wrap(context, creator, kj::fwd<U>(p));
    } else {
      return v8::Null(context->GetIsolate());
    }
  }

  template <typename U>
  kj::Maybe<kj::Maybe<U>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<U>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsNullOrUndefined()) {
      return kj::Maybe<U>(kj::none);
    } else if (config.noSubstituteNull) {
      // There was a bug in the initial version of this method that failed to correctly handle
      // the following tryUnwrap returning a nullptr because of an incorrect type. The
      // noSubstituteNull compatibility flag is needed to fix that.
      return static_cast<TypeWrapper*>(this)
          ->tryUnwrap(context, handle, (kj::Decay<U>*)nullptr, parentObject)
          .map([](auto&& value) -> kj::Maybe<U> { return kj::fwd<decltype(value)>(value); });
    } else {
      return static_cast<TypeWrapper*>(this)->tryUnwrap(
          context, handle, (kj::Decay<U>*)nullptr, parentObject);
    }
  }

private:
  JsgConfig config;
};

// =======================================================================================
// OneOf / variants

template <typename T>
constexpr bool isOneOf = false;
template <typename... T>
constexpr bool isOneOf<kj::OneOf<T...>> = true;
// TODO(cleanup): Move to kj/one-of.h?

// TypeWrapper mixin for variants.
template <typename TypeWrapper>
class OneOfWrapper {
public:
  template <typename... U>
  static kj::String getName(kj::OneOf<U...>*) {
    const auto getNameStr = [](auto u) {
      if constexpr (kj::isSameType<const std::type_info&, decltype(TypeWrapper::getName(u))>()) {
        return typeName(TypeWrapper::getName(u));
      } else {
        return kj::str(TypeWrapper::getName(u));
      }
    };

    return kj::strArray(kj::arr(getNameStr((U*)nullptr)...), " or ");
  }

  template <typename U, typename... V>
  bool wrapHelper(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      kj::OneOf<V...>& in,
      v8::Local<v8::Value>& out) {
    if (in.template is<U>()) {
      out = static_cast<TypeWrapper*>(this)->wrap(context, creator, kj::mv(in.template get<U>()));
      return true;
    } else {
      return false;
    }
  }

  template <typename... U>
  v8::Local<v8::Value> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      kj::OneOf<U...> value) {
    v8::Local<v8::Value> result;
    if (!(wrapHelper<U>(context, creator, value, result) || ...)) {
      result = v8::Undefined(context->GetIsolate());
    }
    return result;
  }

  template <template <typename> class Predicate, typename U, typename... V>
  bool unwrapHelperRecursive(
      v8::Local<v8::Context> context, v8::Local<v8::Value> in, kj::OneOf<V...>& out) {
    if constexpr (isOneOf<U>) {
      // Ugh, a nested OneOf. We can't just call tryUnwrap(), because then our string/numeric
      // coercion might trigger early.
      U val;
      if (unwrapHelper<Predicate>(context, in, val)) {
        out.template init<U>(kj::mv(val));
        return true;
      }
    } else if constexpr (Predicate<kj::Decay<U>>::value) {
      KJ_IF_SOME(val,
          static_cast<TypeWrapper*>(this)->tryUnwrap(context, in, (U*)nullptr, kj::none)) {
        out.template init<U>(kj::mv(val));
        return true;
      }
    }
    return false;
  }

  template <template <typename> class Predicate, typename... U>
  bool unwrapHelper(v8::Local<v8::Context> context, v8::Local<v8::Value> in, kj::OneOf<U...>& out) {
    return (unwrapHelperRecursive<Predicate, U>(context, in, out) || ...);
  }

  // Predicates for helping implement nested OneOf unwrapping. These must be struct templates
  // because we can't pass variable templates as template template parameters.

  template <typename T>
  struct IsResourceType {
    static constexpr bool value = webidl::isNonCallbackInterfaceType<T>;
  };
  template <typename T>
  struct IsFallibleType {
    static constexpr bool value =
        !(webidl::isStringType<T> || webidl::isNumericType<T> || webidl::isBooleanType<T>);
  };
  template <typename T>
  struct IsStringType {
    static constexpr bool value = webidl::isStringType<T>;
  };
  template <typename T>
  struct IsNumericType {
    static constexpr bool value = webidl::isNumericType<T>;
  };
  template <typename T>
  struct IsBooleanType {
    static constexpr bool value = webidl::isBooleanType<T>;
  };

  template <typename... U>
  kj::Maybe<kj::OneOf<U...>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::OneOf<U...>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    (void)webidl::UnionTypeValidator<kj::OneOf<U...>>();
    // Just need to instantiate this, static_asserts will do the rest.

    // In order for string, numeric, and boolean coercion to function as expected, we need to follow
    // the algorithm defined by Web IDL section 3.2.22 to convert JS values to OneOfs. That
    // algorithm is written in a terribly wonky way, of course, but it appears we can restate it
    // like so:
    //
    //   - Perform a series of breadth-first-searches on the OneOf, filtering out certain categories
    //     of types on each run. For the types which are not filtered out, perform a tryUnwrap() on
    //     that type, and succeed if that call succeeds (i.e., short-circuit). The filters used for
    //     each pass are the following:
    //       a. Consider only fallible (uncoercible) types.
    //       b. If the JS value is a boolean, consider only boolean types.
    //       c. If the JS value is a number, consider only numeric types.
    //       d. Consider only string types.
    //       e. Consider only numeric types.
    //       f. Consider only boolean types.
    //
    // Note the symmetry across steps b-f. This way, strings only get coerced to numbers if the
    // OneOf doesn't contain a string type, numbers only get coerced to strings if the OneOf doesn't
    // contain a numeric type, objects only get coerced to a coercible type if there's no matching
    // object type, null and undefined only get coerced to a coercible type if there's no nullable
    // type, etc.
    //
    // TODO(soon): Hacked this by unwrapping into resource types first, so that we can unwrap
    //   Requests and Responses into Initializers without them being interpreted as dictionaries. I
    //   believe this is actually what the Web IDL spec prescribes anyway, but verify.
    //
    // TODO(someday): Prove that this is the same algorithm as the one defined by Web IDL.
    kj::OneOf<U...> result;
    if (unwrapHelper<IsResourceType>(context, handle, result) ||
        unwrapHelper<IsFallibleType>(context, handle, result) ||
        (handle->IsBoolean() && unwrapHelper<IsBooleanType>(context, handle, result)) ||
        (handle->IsNumber() && unwrapHelper<IsNumericType>(context, handle, result)) ||
        (handle->IsBigInt() && unwrapHelper<IsNumericType>(context, handle, result)) ||
        (unwrapHelper<IsStringType>(context, handle, result)) ||
        (unwrapHelper<IsNumericType>(context, handle, result)) ||
        (unwrapHelper<IsBooleanType>(context, handle, result))) {
      return kj::mv(result);
    }
    return kj::none;
  }
};

// =======================================================================================
// Arrays

// TypeWrapper mixin for arrays.
template <typename TypeWrapper>
class ArrayWrapper {
public:
  static auto constexpr MAX_STACK = 64;
  template <typename U>
  static constexpr const char* getName(kj::Array<U>*) {
    return "Array";
  }

  template <typename U>
  v8::Local<v8::Value> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      kj::Array<U> array) {
    v8::Isolate* isolate = context->GetIsolate();
    v8::EscapableHandleScope handleScope(isolate);

    v8::LocalVector<v8::Value> items(isolate, array.size());
    for (auto n = 0; n < items.size(); n++) {
      items[n] = static_cast<TypeWrapper*>(this)
                     ->wrap(context, creator, kj::mv(array[n]))
                     .template As<v8::Value>();
    }
    auto out = v8::Array::New(isolate, items.data(), items.size());

    return handleScope.Escape(out);
  }
  template <typename U>
  v8::Local<v8::Value> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      kj::ArrayPtr<U> array) {
    v8::Isolate* isolate = context->GetIsolate();
    v8::EscapableHandleScope handleScope(isolate);

    v8::LocalVector<v8::Value> items(isolate, array.size());
    for (auto n = 0; n < items.size(); n++) {
      items[n] = static_cast<TypeWrapper*>(this)
                     ->wrap(context, creator, kj::mv(array[n]))
                     .template As<v8::Value>();
    }
    auto out = v8::Array::New(isolate, items.data(), items.size());

    return handleScope.Escape(out);
  }
  template <typename U>
  v8::Local<v8::Value> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      kj::Array<U>& array) {
    return static_cast<TypeWrapper*>(this)->wrap(context, creator, array.asPtr());
  }

  template <typename U>
  kj::Maybe<kj::Array<U>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Array<U>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (!handle->IsArray()) {
      return kj::none;
    }

    auto array = handle.As<v8::Array>();
    auto length = array->Length();
    auto builder = kj::heapArrayBuilder<U>(length);
    for (auto i: kj::zeroTo(length)) {
      v8::Local<v8::Value> element = check(array->Get(context, i));
      builder.add(static_cast<TypeWrapper*>(this)->template unwrap<U>(
          context, element, TypeErrorContext::arrayElement(i)));
    }
    return builder.finish();
  }
};

// =======================================================================================
// ArrayBuffers / ArrayBufferViews
//
// This wrapper implements the following wrapping conversions:
//  - kj::Array<kj::byte> -> ArrayBuffer
//
// And the following unwrapping conversions:
//   - ArrayBuffer -> kj::Array<kj::byte>
//     (the kj::Array object holds a Global to the unwrapped ArrayBuffer)
//   - ArrayBufferView -> kj::Array<kj::byte>
//     (the kj::Array object holds a Global to the unwrapped ArrayBufferView's backing buffer)
//
// Note that there are no conversions for kj::ArrayPtr<kj::byte>, since it does not own its own
// buffer -- fine in C++, but problematic in a GC language like JS. Restricting the interface to
// only operate on owned arrays makes memory management simpler and safer in both directions.
//
// Logically a kj::Array<byte> could be considered analogous to a Uint8Array in JS, and for a time
// that was the wrapping conversion implemented by this wrapper. However, the most common use cases
// in web platform APIs involve accepting BufferSources for processing as immutable input and
// returning ArrayBuffers. Since a kj::byte does not map to any JavaScript primitive, establishing
// a mapping between ArrayBuffer/ArrayBufferView and Array<byte> is unambiguous and
// convenient. The few places where a specific TypedArray is expected (e.g. Uint8Array) can be
// handled explicitly with a v8::Local<v8::Uint8Array> (or other appropriate TypedArray type).
//
// BufferSource arguments to web platform API methods are typically expected to be processed but not
// mutated, such as the input parameter to TextDecoder.decode(). This processing might happen
// asynchronously, such as the plaintext parameter to SubtleCrypto.encrypt(). I am unaware of any
// use of BufferSources which involve mutating the underlying ArrayBuffer -- typically an explicit
// ArrayBufferView is expected for this case, such as the parameters to crypto.getRandomValues() or
// the Streams spec's BYOB reader's read() method.
//
// This suggests the following rules of thumb:
//
// 1. If a BufferSource parameter is used as input to a:
//   - synchronous method: accept a `kj::Array<const kj::byte>`.
//   - asynchronous method (user is allowed to re-use the buffer during processing): accept a
//     `kj::Array<const kj::byte>` and explicitly copy its bytes.
//
// 2. If a method accepts an ArrayBufferView that it is expected to mutate:
//   - accept a `v8::Local<v8::ArrayBufferView>` explicitly (handled by V8HandleWrapper in
//     type-wrapper.h) rather than a `kj::Array<kj::byte>` -- otherwise your method's contract
//     will be wider than intended.
//   - use `jsg::asBytes()` as a quick way to get a `kj::ArrayPtr<kj::byte>` view onto it.
//
// 3. If a method returns an ArrayBuffer, create and return a `kj::Array<kj::byte>`.
template <typename TypeWrapper>
class ArrayBufferWrapper {
public:
  static constexpr const char* getName(kj::ArrayPtr<byte>*) {
    return "ArrayBuffer or ArrayBufferView";
  }
  static constexpr const char* getName(kj::ArrayPtr<const byte>*) {
    return "ArrayBuffer or ArrayBufferView";
  }
  static constexpr const char* getName(kj::Array<byte>*) {
    return "ArrayBuffer or ArrayBufferView";
  }

  v8::Local<v8::ArrayBuffer> wrap(
      v8::Isolate* isolate, kj::Maybe<v8::Local<v8::Object>> creator, kj::Array<byte> value) {
    // We need to construct a BackingStore that owns the byte array. We use the version of
    // v8::ArrayBuffer::NewBackingStore() that accepts a deleter callback, and arrange for it to
    // delete an Array<byte> placed on the heap.
    //
    // TODO(perf): We could avoid an allocation here, perhaps, by decomposing the kj::Array<byte>
    //   into its component pointer and disposer, and then pass the disposer pointer as the
    //   "deleter_data" for NewBackingStore. However, KJ doesn't give us any way to decompose an
    //   Array<T> this way, and it might not want to, as this could make it impossible to support
    //   unifying Array<T> and Vector<T> in the future (i.e. making all Array<T>s growable). So
    //   it may be best to stick with allocating an Array<byte> on the heap after all...
    size_t size = value.size();
    if (size == 0) {
      // BackingStore doesn't call custom deleter if begin is null, which it often is for empty
      // arrays.
      return v8::ArrayBuffer::New(isolate, 0);
    }
    byte* begin = value.begin();

    auto ownerPtr = new kj::Array<byte>(kj::mv(value));

    std::unique_ptr<v8::BackingStore> backing =
        v8::ArrayBuffer::NewBackingStore(begin, size, [](void* begin, size_t size, void* ownerPtr) {
      delete reinterpret_cast<kj::Array<byte>*>(ownerPtr);
    }, ownerPtr);

    return v8::ArrayBuffer::New(isolate, kj::mv(backing));
  }

  v8::Local<v8::ArrayBuffer> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      kj::Array<byte> value) {
    return wrap(context->GetIsolate(), creator, kj::mv(value));
  }

  kj::Maybe<kj::Array<byte>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Array<byte>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsArrayBufferView()) {
      return asBytes(handle.As<v8::ArrayBufferView>());
    } else if (handle->IsArrayBuffer()) {
      return asBytes(handle.As<v8::ArrayBuffer>());
    }
    return kj::none;
  }

  kj::Maybe<kj::Array<const byte>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Array<const byte>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return tryUnwrap(context, handle, (kj::Array<byte>*)nullptr, parentObject);
  }
};

// =======================================================================================
// Dicts

// TypeWrapper mixin for dictionaries (objects used as string -> value maps).
template <typename TypeWrapper>
class DictWrapper {
public:
  template <typename K, typename V>
  static constexpr const char* getName(Dict<V, K>*) {
    return "object";
  }

  template <typename K, typename V>
  v8::Local<v8::Value> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, Dict<V, K> dict) {
    static_assert(webidl::isStringType<K>, "Dicts must be keyed on a string type.");

    v8::Isolate* isolate = context->GetIsolate();
    v8::EscapableHandleScope handleScope(isolate);
    auto out = v8::Object::New(isolate);
    for (auto& field: dict.fields) {
      // Set() returns Maybe<bool>. As usual, if the Maybe is null, then there was an exception,
      // but I have no idea what it means if the Maybe was filled in with the boolean value false...
      KJ_ASSERT(check(out->Set(context,
          static_cast<TypeWrapper*>(this)->wrap(context, creator, kj::mv(field.name)),
          static_cast<TypeWrapper*>(this)->wrap(context, creator, kj::mv(field.value)))));
    }
    return handleScope.Escape(out);
  }

  template <typename K, typename V>
  kj::Maybe<Dict<V, K>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Dict<V, K>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    static_assert(webidl::isStringType<K>, "Dicts must be keyed on a string type.");

    auto& wrapper = static_cast<TypeWrapper&>(*this);

    // Currently the same as wrapper.unwrap<kj::String>(), but this allows us not to bother with the
    // TypeErrorContext, or worrying about whether the tryUnwrap(kj::String*) version will ever be
    // modified to return nullptr in the future.
    const auto convertToUtf8 = [isolate = context->GetIsolate()](v8::Local<v8::String> v8String) {
      auto buf = kj::heapArray<char>(v8String->Utf8Length(isolate) + 1);
      v8String->WriteUtf8(isolate, buf.begin(), buf.size());
      buf[buf.size() - 1] = 0;
      return kj::String(kj::mv(buf));
    };

    if (!handle->IsObject() || handle->IsArray()) {
      return kj::none;
    }

    auto object = handle.As<v8::Object>();
    v8::Local<v8::Array> names = check(object->GetOwnPropertyNames(context));
    auto length = names->Length();
    auto builder = kj::heapArrayBuilder<typename Dict<V, K>::Field>(length);
    for (auto i: kj::zeroTo(length)) {
      v8::Local<v8::String> name = check(check(names->Get(context, i))->ToString(context));
      v8::Local<v8::Value> value = check(object->Get(context, name));

      if constexpr (kj::isSameType<K, kj::String>()) {
        auto strName = convertToUtf8(name);
        const char* cstrName = strName.cStr();
        builder.add(typename Dict<V, K>::Field{kj::mv(strName),
          wrapper.template unwrap<V>(
              context, value, TypeErrorContext::dictField(cstrName), object)});
      } else {
        // Here we have to be a bit more careful than for the kj::String case. The unwrap<K>() call
        // may throw, but we need the name in UTF-8 for the very exception that it needs to throw.
        // Thus, we do the unwrapping manually and UTF-8-convert the name only if it's needed.
        auto unwrappedName = wrapper.tryUnwrap(context, name, (K*)nullptr, object);
        if (unwrappedName == kj::none) {
          auto strName = convertToUtf8(name);
          throwTypeError(context->GetIsolate(), TypeErrorContext::dictKey(strName.cStr()),
              TypeWrapper::getName((K*)nullptr));
        }
        auto unwrappedValue = wrapper.tryUnwrap(context, value, (V*)nullptr, object);
        if (unwrappedValue == kj::none) {
          auto strName = convertToUtf8(name);
          throwTypeError(context->GetIsolate(), TypeErrorContext::dictField(strName.cStr()),
              TypeWrapper::getName((V*)nullptr));
        }
        builder.add(typename Dict<V, K>::Field{
          KJ_ASSERT_NONNULL(kj::mv(unwrappedName)), KJ_ASSERT_NONNULL(kj::mv(unwrappedValue))});
      }
    }
    return Dict<V, K>{builder.finish()};
  }
};

// =======================================================================================
// Dates

template <typename TypeWrapper>
class DateWrapper {
public:
  static constexpr const char* getName(kj::Date*) {
    return "date";
  }

  v8::Local<v8::Value> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator, kj::Date date) {
    return check(v8::Date::New(context, (date - kj::UNIX_EPOCH) / kj::MILLISECONDS));
  }

  kj::Maybe<kj::Date> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Date*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsDate()) {
      double millis = handle.template As<v8::Date>()->ValueOf();
      return toKjDate(millis);
    } else if (handle->IsNumber()) {
      double millis = handle.template As<v8::Number>()->Value();
      return toKjDate(millis);
    } else {
      return kj::none;
    }
  }

private:
  kj::Date toKjDate(double millis) {
    JSG_REQUIRE(isFinite(millis), TypeError,
        "The value cannot be converted because it is not a valid Date.");

    // JS Date uses milliseconds stored as a double-precision float to represent times
    // KJ uses nanoseconds stored as an int64_t, which is significantly smaller but larger
    // than my lifetime.
    //
    // For most use-cases, throwing when we encounter a date outside of KJ's supported range is OK.
    // API's that need to support time-travelers or historians may need to consider using the
    // V8 Date type directly.
    constexpr double millisToNanos = kj::MILLISECONDS / kj::NANOSECONDS;
    double nanos = millis * millisToNanos;
    JSG_REQUIRE(
        nanos < int64_t(kj::maxValue), TypeError, "This API doesn't support dates after 2189.");
    JSG_REQUIRE(
        nanos > int64_t(kj::minValue), TypeError, "This API doesn't support dates before 1687.");
    return kj::UNIX_EPOCH + int64_t(millis) * kj::MILLISECONDS;
  };
};

// =======================================================================================
// NonCoercible<T>

template <typename TypeWrapper>
class NonCoercibleWrapper {
public:
  template <CoercibleType T>
  static auto getName(NonCoercible<T>*) {
    return TypeWrapper::getName((T*)nullptr);
  }

  template <CoercibleType T>
  v8::Local<v8::Value> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      NonCoercible<T>) = delete;

  template <CoercibleType T>
  kj::Maybe<NonCoercible<T>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      NonCoercible<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto& wrapper = static_cast<TypeWrapper&>(*this);
    if constexpr (kj::isSameType<kj::String, T>()) {
      if (!handle->IsString()) return kj::none;
      KJ_IF_SOME(value, wrapper.tryUnwrap(context, handle, (T*)nullptr, parentObject)) {
        return NonCoercible<T>{
          .value = kj::mv(value),
        };
      }
      return kj::none;
    } else if constexpr (kj::isSameType<bool, T>()) {
      if (!handle->IsBoolean()) return kj::none;
      return wrapper.tryUnwrap(context, handle, (T*)nullptr, parentObject).map([](auto& value) {
        return NonCoercible<T>{
          .value = value,
        };
      });
    } else if constexpr (kj::isSameType<double, T>()) {
      if (!handle->IsNumber()) return kj::none;
      return wrapper.tryUnwrap(context, handle, (T*)nullptr, parentObject).map([](auto& value) {
        return NonCoercible<T>{
          .value = value,
        };
      });
    } else {
      return nullptr;
    }
  }
};

// =======================================================================================
// MemoizedIdentity<T>

template <typename T>
void MemoizedIdentity<T>::visitForGc(GcVisitor& visitor) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(raw, T) {
      if constexpr (isGcVisitable<T>()) {
        visitor.visit(raw);
      }
    }
    KJ_CASE_ONEOF(handle, Value) {
      return visitor.visit(handle);
    }
  }
}

template <typename TypeWrapper>
class MemoizedIdentityWrapper {
public:
  template <typename T>
  static auto getName(MemoizedIdentity<T>*) {
    return TypeWrapper::getName((T*)nullptr);
  }

  template <typename T>
  v8::Local<v8::Value> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      MemoizedIdentity<T>& value) {
    auto& wrapper = static_cast<TypeWrapper&>(*this);
    KJ_SWITCH_ONEOF(value.value) {
      KJ_CASE_ONEOF(raw, T) {
        auto handle = wrapper.wrap(context, creator, kj::mv(raw));
        value.value.template init<Value>(context->GetIsolate(), handle);
        return handle;
      }
      KJ_CASE_ONEOF(handle, Value) {
        return handle.getHandle(context->GetIsolate());
      }
    }
    __builtin_unreachable();
  }

  template <typename T>
  kj::Maybe<MemoizedIdentity<T>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      MemoizedIdentity<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) = delete;
};

// =======================================================================================
// Identified<T>

template <typename TypeWrapper>
class IdentifiedWrapper {
public:
  template <typename T>
  static auto getName(Identified<T>*) {
    return TypeWrapper::getName((T*)nullptr);
  }

  template <typename T>
  v8::Local<v8::Value> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      Identified<T>& value) = delete;

  template <typename T>
  kj::Maybe<Identified<T>> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Identified<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (!handle->IsObject()) {
      return kj::none;
    }

    auto& wrapper = static_cast<TypeWrapper&>(*this);
    return wrapper.tryUnwrap(context, handle, (T*)nullptr, parentObject)
        .map([&](T&& value) -> Identified<T> {
      auto isolate = context->GetIsolate();
      auto obj = handle.As<v8::Object>();
      return {.identity = {isolate, obj}, .unwrapped = kj::mv(value)};
    });
  }
};

// =======================================================================================
// SelfRef

template <typename TypeWrapper>
class SelfRefWrapper {
public:
  static auto getName(SelfRef*) {
    return "SelfRef";
  }

  v8::Local<v8::Value> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      const SelfRef& value) = delete;

  kj::Maybe<SelfRef> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      SelfRef*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // I'm sticking this here because it's related and I'm lazy.
    return SelfRef(context->GetIsolate(),
        KJ_ASSERT_NONNULL(
            parentObject, "SelfRef cannot only be used as a member of a JSG_STRUCT."));
  }
};

// =======================================================================================
// kj::Exception
//
// The kj::Exception wrapper handles the translation of so-called "tunneled" exceptions
// between KJ and JavaScript. The wrapper is capable of turning any JavaScript value into
// a kj::Exception with the caveat that the kj::Exception is not guaranteed to retain all
// of the detail. Likewise, it can turn a kj::Exception with the correct metadata into a
// reasonable JavaScript exception.

class DOMException;

template <typename TypeWrapper>
class ExceptionWrapper {
public:
  static constexpr const char* getName(kj::Exception*) {
    return "Exception";
  }

  v8::Local<v8::Value> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      kj::Exception exception) {
    return makeInternalError(context->GetIsolate(), kj::mv(exception));
  }

  kj::Maybe<kj::Exception> tryUnwrap(v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Exception*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {

    // If handle is a DOMException, then createTunneledException will not work
    // here. We have to manually handle the DOMException case.
    //
    // Note that this is a general issue with any JSG_RESOURCE_TYPE that we
    // happen to use as Errors. The createTunneledException() method uses V8's
    // ToDetailString() to extract the detail about the error in a manner that
    // is safe and side-effect free. Unfortunately, that mechanism does not
    // work for JSG_RESOURCE_TYPE objects that are used as errors. For those,
    // we need to drop down to the C++ interface and generate the kj::Exception
    // ourselves. If any additional JSG_RESOURCE_TYPE error-like things are
    // introduced, they'll need to be handled explicitly here also.
    auto& js = Lock::from(context->GetIsolate());
    auto& wrapper = TypeWrapper::from(js.v8Isolate);
    kj::Exception result = [&]() {
      kj::Exception::Type excType = [&]() {
        // Use .retryable and .overloaded properties as hints for what kj exception type to use.
        if (handle->IsObject()) {
          auto object = handle.As<v8::Object>();

          if (js.toBool(check(object->Get(context, v8StrIntern(js.v8Isolate, "overloaded"_kj))))) {
            return kj::Exception::Type::OVERLOADED;
          }
          if (js.toBool(check(object->Get(context, v8StrIntern(js.v8Isolate, "retryable"_kj))))) {
            return kj::Exception::Type::DISCONNECTED;
          }
        }
        return kj::Exception::Type::FAILED;
      }();

      KJ_IF_SOME(domException,
          wrapper.tryUnwrap(context, handle, (DOMException*)nullptr, parentObject)) {
        return KJ_EXCEPTION(FAILED,
            kj::str("jsg.DOMException(", domException.getName(), "): ", domException.getMessage()));
      } else {

        static const constexpr kj::StringPtr PREFIXES[] = {
          // JavaScript intrinsic Error Types
          "Error"_kj,
          "RangeError"_kj,
          "TypeError"_kj,
          "SyntaxError"_kj,
          "ReferenceError"_kj,
          // WASM Error Types
          "CompileError"_kj,
          "LinkError"_kj,
          "RuntimeError"_kj,
          // JSG_RESOURCE_TYPE Error Types
          "DOMException"_kj,
        };

        kj::String reason;
        if (!handle->IsObject()) {
          // if the argument isn't an object, it couldn't possibly be an Error.
          reason = kj::str(JSG_EXCEPTION(Error) ": ", handle);
        } else {
          reason = kj::str(handle);
          bool found = false;
          // If the error message starts with a platform error type that we tunnel,
          // prefix it with "jsg."
          for (auto name: PREFIXES) {
            if (reason.startsWith(name)) {
              reason = kj::str("jsg.", reason);
              found = true;
              break;
            }
          }
          // Everything else should just come through as a normal error.
          if (!found) {
            reason = kj::str(JSG_EXCEPTION(Error) ": ", reason);
          }
        }
        return kj::Exception(excType, __FILE__, __LINE__, kj::mv(reason));
      }
    }();

    addExceptionDetail(js, result, handle);
    return result;
  }
};

}  // namespace workerd::jsg
