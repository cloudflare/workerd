// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/string.h>

#include <cstdint>

namespace workerd {

enum CharAttributeFlag : uint8_t {
  NONE = 0,
  ALPHA = 1 << 0,
  DIGIT = 1 << 1,
  HEX = 1 << 2,
  ASCII = 1 << 3,
  ASCII_WHITESPACE = 1 << 4,
  UPPER_CASE = 1 << 5,
  LOWER_CASE = 1 << 6,
  SEPARATOR = 1 << 7,
};

// Construct a lookup table for various interesting character properties.
constexpr kj::FixedArray<uint8_t, 256> kCharLookupTable = []() consteval {
  kj::FixedArray<uint8_t, 256> result{};
  for (uint8_t c = 'A'; c <= 'Z'; c++) {
    if (c <= 'F') {
      result[c] |= CharAttributeFlag::HEX;
      result[c + 0x20] |= CharAttributeFlag::HEX;
    }
    result[c] |= CharAttributeFlag::ALPHA | CharAttributeFlag::UPPER_CASE;
    result[c + 0x20] |= CharAttributeFlag::ALPHA | CharAttributeFlag::LOWER_CASE;
  }
  for (uint8_t c = '0'; c <= '9'; c++) {
    result[c] |= CharAttributeFlag::DIGIT | CharAttributeFlag::HEX;
  }
  for (uint8_t c = 0; c <= 0x7f; c++) {
    result[c] |= CharAttributeFlag::ASCII;
  }
  for (uint8_t c: {0x09, 0x0a, 0x0c, 0x0d, 0x20}) {
    result[c] |= CharAttributeFlag::ASCII_WHITESPACE;
  }
  result['+'] |= CharAttributeFlag::SEPARATOR;
  result['-'] |= CharAttributeFlag::SEPARATOR;
  result['_'] |= CharAttributeFlag::SEPARATOR;
  return result;
}();

constexpr bool isAlpha(const kj::byte c) noexcept {
  return kCharLookupTable[c] & CharAttributeFlag::ALPHA;
}

constexpr bool isDigit(const kj::byte c) noexcept {
  return kCharLookupTable[c] & CharAttributeFlag::DIGIT;
}

// Check if `c` is the ASCII code of a hexadecimal digit.
constexpr bool isHexDigit(const kj::byte c) noexcept {
  return kCharLookupTable[c] & CharAttributeFlag::HEX;
}

constexpr bool isAscii(const kj::byte c) noexcept {
  return kCharLookupTable[c] & CharAttributeFlag::ASCII;
}

constexpr bool isAsciiWhitespace(const kj::byte c) noexcept {
  return kCharLookupTable[c] & CharAttributeFlag::ASCII_WHITESPACE;
}

constexpr bool isAlphaUpper(const kj::byte c) noexcept {
  return kCharLookupTable[c] & CharAttributeFlag::UPPER_CASE;
}

constexpr bool isAlphaLower(const kj::byte c) noexcept {
  return kCharLookupTable[c] & CharAttributeFlag::LOWER_CASE;
}

// Convert ASCII alpha characters in the given string to lowercase in place.
kj::String toLower(kj::String&& str);

// Convert ASCII alpha characters in the given string to uppercase in place.
kj::String toUpper(kj::String&& str);

// Copy the input and convert ASCII alpha characters in the given string to lowercase.
kj::String toLower(kj::ArrayPtr<const char> ptr);
kj::String toUpper(kj::ArrayPtr<const char> ptr);

kj::ArrayPtr<const char> trimLeadingAndTrailingWhitespace(kj::ArrayPtr<const char> ptr);
kj::ArrayPtr<const char> trimTailingWhitespace(kj::ArrayPtr<const char> ptr);
kj::Array<kj::byte> stripInnerWhitespace(kj::ArrayPtr<kj::byte> input);

}  // namespace workerd
