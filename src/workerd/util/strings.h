// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/string.h>

namespace workerd {

inline kj::String toLowerCopy(kj::StringPtr ptr) {
  auto str = kj::str(ptr);
  for (char& c: str) {
    if ('A' <= c && c <= 'Z') c += 'a' - 'A';
  }
  return kj::mv(str);
}

inline kj::String toLowerCopy(kj::ArrayPtr<const char> ptr) {
  auto str = kj::str(ptr);
  for (char& c: str) {
    if ('A' <= c && c <= 'Z') c += 'a' - 'A';
  }
  return kj::mv(str);
}

constexpr kj::FixedArray<uint8_t, 256> kHexDigitTable = []() consteval {
  kj::FixedArray<uint8_t, 256> result{};
  for (uint8_t c: {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'}) {
    result[c] = true;
  }
  for (uint8_t c: {'A', 'B', 'C', 'D', 'E', 'F'}) {
    result[c] = true;         // Uppercase
    result[c + 0x20] = true;  // Lowercase
  }
  return result;
}();

// Check if `c` is the ASCII code of a hexadecimal digit.
constexpr bool isHexDigit(char c) {
  return kHexDigitTable[static_cast<int>(c)] == 1;
}

}  // namespace workerd
