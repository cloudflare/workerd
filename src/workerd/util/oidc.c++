// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "oidc.h"

#include <kj/encoding.h>
#include <kj/parse/char.h>

namespace workerd {
namespace {

constexpr auto ALPHA = kj::parse::charRange('a', 'z').orRange('A', 'Z');
constexpr auto DIGIT = kj::parse::charRange('0', '9');
constexpr auto HEX = DIGIT.orRange('a', 'f').orRange('A', 'F');
constexpr auto UNRESERVED = ALPHA.orGroup(DIGIT).orAny("-._~");
constexpr auto REG_NAME = UNRESERVED.orAny("!$&'()*+,;=");
constexpr auto USER_INFO = REG_NAME.orChar(':');
constexpr auto PATH = USER_INFO.orAny("@/");
constexpr auto QUERY_FRAGMENT = PATH.orChar('?');

// Validate percent escapes without decoding them: the original spelling defines the audience.
bool validComponent(kj::ArrayPtr<const char> text, const kj::parse::CharGroup_& allowed) {
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%') {
      if (text.size() - i < 3 || !HEX.contains(text[i + 1]) || !HEX.contains(text[i + 2])) {
        return false;
      }
      i += 2;
    } else if (!allowed.contains(text[i])) {
      return false;
    }
  }
  return true;
}

bool validIpv4(kj::ArrayPtr<const char> text) {
  for (uint32_t part = 0; part < 4; ++part) {
    auto end = text.findFirst('.').orDefault(text.size());
    auto octet = text.first(end);
    if (octet.size() == 0 || octet.size() > 3 || !DIGIT.containsAll(octet) ||
        (octet.size() > 1 && octet[0] == '0'))
      return false;
    uint32_t value = 0;
    for (auto c: octet) value = value * 10 + c - '0';
    if (value > 255) return false;
    if (part == 3) return end == text.size();
    if (end == text.size()) return false;
    text = text.slice(end + 1);
  }
  return false;
}

bool validIpv6(kj::ArrayPtr<const char> text) {
  uint32_t groups = 0;
  bool compressed = false;
  if (text.size() >= 2 && text[0] == ':' && text[1] == ':') {
    compressed = true;
    text = text.slice(2);
  }
  while (text.size() > 0) {
    auto end = text.findFirst(':').orDefault(text.size());
    auto group = text.first(end);
    if (group.findFirst('.') != kj::none) {
      return end == text.size() && validIpv4(group) && (compressed ? groups < 6 : groups == 6);
    }
    if (group.size() == 0 || group.size() > 4 || !HEX.containsAll(group) || ++groups > 8) {
      return false;
    }
    if (end == text.size()) break;
    text = text.slice(end + 1);
    if (text.size() > 0 && text[0] == ':') {
      if (compressed) return false;
      compressed = true;
      text = text.slice(1);
    } else if (text.size() == 0) {
      return false;
    }
  }
  return compressed ? groups < 8 : groups == 8;
}

bool validIpLiteral(kj::ArrayPtr<const char> text) {
  if (text.size() == 0 || (text[0] != 'v' && text[0] != 'V')) return validIpv6(text);
  // RFC 3986 IPvFuture: "v" 1*HEXDIG "." 1*(unreserved / sub-delims / ":").
  auto dot = text.findFirst('.').orDefault(text.size());
  return dot > 1 && dot + 1 < text.size() && HEX.containsAll(text.slice(1, dot)) &&
      USER_INFO.containsAll(text.slice(dot + 1));
}

bool validAuthority(kj::ArrayPtr<const char> text) {
  KJ_IF_SOME(at, text.findFirst('@')) {
    if (!validComponent(text.first(at), USER_INFO)) return false;
    text = text.slice(at + 1);
  }
  if (text.size() > 0 && text[0] == '[') {
    auto end = text.findFirst(']').orDefault(text.size());
    if (end == text.size() || !validIpLiteral(text.slice(1, end))) return false;
    text = text.slice(end + 1);
    if (text.size() == 0) return true;
    if (text[0] != ':') return false;
  } else {
    auto end = text.findFirst(':').orDefault(text.size());
    if (!validComponent(text.first(end), REG_NAME)) return false;
    text = text.slice(end);
    if (text.size() == 0) return true;
  }
  return DIGIT.containsAll(text.slice(1));
}

bool validUri(kj::ArrayPtr<const char> text, size_t colon) {
  if (colon == 0 || !ALPHA.contains(text[0]) ||
      !ALPHA.orGroup(DIGIT).orAny("+-.").containsAll(text.slice(1, colon)))
    return false;
  text = text.slice(colon + 1);
  KJ_IF_SOME(hash, text.findFirst('#')) {
    if (!validComponent(text.slice(hash + 1), QUERY_FRAGMENT)) return false;
    text = text.first(hash);
  }
  KJ_IF_SOME(question, text.findFirst('?')) {
    if (!validComponent(text.slice(question + 1), QUERY_FRAGMENT)) return false;
    text = text.first(question);
  }
  if (text.size() >= 2 && text[0] == '/' && text[1] == '/') {
    text = text.slice(2);
    auto slash = text.findFirst('/').orDefault(text.size());
    if (!validAuthority(text.first(slash))) return false;
    text = text.slice(slash);
  }
  return validComponent(text, PATH);
}

bool isWhitespace(uint32_t c) {
  return (c >= 0x09 && c <= 0x0d) || c == 0x20 || c == 0x85 || c == 0xa0 || c == 0x1680 ||
      (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
      c == 0x3000 || c == 0xfeff;
}

}  // namespace

bool isValidOidcAudience(kj::StringPtr value) {
  if (value.size() == 0 || value.size() > 2048) return false;
  auto unicode = kj::encodeUtf32(value);
  if (unicode.hadErrors) return false;
  bool hasNonWhitespace = false;
  for (auto c: unicode) {
    if (c >= 0xd800 && c <= 0xdfff) return false;
    hasNonWhitespace |= !isWhitespace(c);
  }
  if (!hasNonWhitespace) return false;
  KJ_IF_SOME(colon, value.findFirst(':')) {
    return validUri(value, colon);
  }
  return true;
}

}  // namespace workerd
