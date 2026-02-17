// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/parse/char.h>
#include <kj/string.h>

namespace workerd::util {

inline bool isValidHeaderValue(kj::ArrayPtr<const char> value) {
  if (value == nullptr) return true;
  for (auto c: value) {
    if (c == '\0' || c == '\r' || c == '\n') {
      return false;
    }
  }
  return true;
}

constexpr auto HTTP_SEPARATOR_CHARS = kj::parse::anyOfChars("()<>@,;:\\\"/[]?={} \t");
// RFC2616 section 2.2: https://www.w3.org/Protocols/rfc2616/rfc2616-sec2.html#sec2.2

constexpr auto HTTP_TOKEN_CHARS = kj::parse::controlChar.orChar('\x7f')
                                      .orGroup(kj::parse::whitespaceChar)
                                      .orGroup(HTTP_SEPARATOR_CHARS)
                                      .invert();
// RFC2616 section 2.2: https://www.w3.org/Protocols/rfc2616/rfc2616-sec2.html#sec2.2
// RFC2616 section 4.2: https://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.2

inline constexpr bool isHttpWhitespace(char c) {
  return c == '\t' || c == '\r' || c == '\n' || c == ' ';
}
static_assert(isHttpWhitespace(' '));
static_assert(!isHttpWhitespace('A'));
inline constexpr bool isHttpTokenChar(char c) {
  return HTTP_TOKEN_CHARS.contains(c);
}
static_assert(isHttpTokenChar('A'));
static_assert(!isHttpTokenChar(' '));
}  // namespace workerd::util
