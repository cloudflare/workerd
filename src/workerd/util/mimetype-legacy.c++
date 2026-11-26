// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "mimetype-legacy.h"

#include "strings.h"

#include <workerd/util/string-buffer.h>

#include <kj/debug.h>

namespace workerd {

namespace legacy {

// Reaches the parameters of a MimeType being built, as MimeType::addParam() does but with the
// C++ rules below.
struct MimeTypeAccess {
  static bool addParam(
      MimeType& mimeType, kj::ArrayPtr<const char> name, kj::ArrayPtr<const char> value);
};

namespace {

constexpr bool isWhitespace(const char c) noexcept {
  return (c == '\r' || c == '\n' || c == '\t' || c == ' ');
}

static constexpr kj::FixedArray<uint8_t, 256> token_table = []() consteval {
  kj::FixedArray<uint8_t, 256> result{};

  for (uint8_t c:
      {'!', '#', '$', '%', '&', '\'', '*', '+', '\\', '-', '.', '^', '_', '`', '|', '~'}) {
    result[c] = true;
  }

  // (c >= 'A' && c <= 'Z')
  for (uint8_t c = 'A'; c <= 'Z'; c++) {
    result[c] = true;
  }

  // (c >= 'a' && c <= 'z')
  for (uint8_t c = 'a'; c <= 'z'; c++) {
    result[c] = true;
  }

  // (c >= '0' && c <= '9')
  for (uint8_t c = '0'; c <= '9'; c++) {
    result[c] = true;
  }

  return result;
}();

constexpr bool isTokenChar(const uint8_t c) noexcept {
  return token_table[c];
}

static constexpr kj::FixedArray<uint8_t, 256> quoted_string_token_table = []() consteval {
  kj::FixedArray<uint8_t, 256> result{};
  result['\t'] = true;

  for (uint8_t c = 0x20; c <= 0x7e; c++) {
    result[c] = true;
  }

  for (uint8_t c = 0x80; c < 255; c++) {
    result[c] = true;
  }

  return result;
}();

constexpr bool isQuotedStringTokenChar(const uint8_t c) noexcept {
  return quoted_string_token_table[c];
}

kj::ArrayPtr<const char> skipWhitespace(kj::ArrayPtr<const char> str) {
  auto ptr = str.begin();
  auto end = str.end();
  while (ptr != end && isWhitespace(*ptr)) {
    ptr++;
  }
  return str.slice(ptr - str.begin());
}

kj::ArrayPtr<const char> trimWhitespace(kj::ArrayPtr<const char> str) {
  auto ptr = str.end();
  while (ptr > str.begin() && isWhitespace(*(ptr - 1))) --ptr;
  return str.first(ptr - str.begin());
}

constexpr bool hasInvalidCodepoints(kj::ArrayPtr<const char> str, auto predicate) {
  bool has_invalid_codepoints = false;
  for (const char c: str) {
    has_invalid_codepoints |= !predicate(static_cast<uint8_t>(c));
  }
  return has_invalid_codepoints;
}

kj::Maybe<size_t> findParamDelimiter(kj::ArrayPtr<const char> str) {
  auto ptr = str.begin();
  while (ptr != str.end()) {
    if (*ptr == ';' || *ptr == '=') return ptr - str.begin();
    ++ptr;
  }
  return kj::none;
}

}  // namespace

kj::Maybe<MimeType> tryParseMimeType(
    kj::ArrayPtr<const char> input, MimeType::ParseOptions options) {
  // Skip leading whitespace from start
  input = skipWhitespace(input);
  if (input.size() == 0) return kj::none;

  kj::Maybe<kj::String> maybeType;
  // Let's try to find the solidus that separates the type and subtype
  KJ_IF_SOME(n, input.findFirst('/')) {
    auto typeCandidate = input.first(n);
    if (typeCandidate.size() == 0 || hasInvalidCodepoints(typeCandidate, isTokenChar)) {
      return kj::none;
    }
    maybeType = toLower(typeCandidate);
    input = input.slice(n + 1);
  } else {
    // If the solidus is not found, then it's not a valid mime type
    return kj::none;
  }

  // If there's nothing else to parse at this point, it's not a valid mime type.
  if (input.size() == 0) return kj::none;

  kj::Maybe<kj::String> maybeSubtype;
  KJ_IF_SOME(n, input.findFirst(';')) {
    // If a semi-colon is found, the subtype is everything up to that point
    // minus trailing whitespace.
    auto subtypeCandidate = trimWhitespace(input.first(n));
    if (subtypeCandidate.size() == 0 || hasInvalidCodepoints(subtypeCandidate, isTokenChar)) {
      return kj::none;
    }
    maybeSubtype = toLower(subtypeCandidate);
    input = input.slice(n + 1);
  } else {
    auto subtypeCandidate = trimWhitespace(input);
    if (subtypeCandidate.size() == 0 || hasInvalidCodepoints(subtypeCandidate, isTokenChar)) {
      return kj::none;
    }
    maybeSubtype = toLower(subtypeCandidate);
    input = {};
  }

  MimeType result(kj::mv(KJ_ASSERT_NONNULL(maybeType)), kj::mv(KJ_ASSERT_NONNULL(maybeSubtype)));

  if (!(options & MimeType::IGNORE_PARAMS)) {
    // Parse the parameters...
    while (input.size() > 0) {
      input = skipWhitespace(input);
      if (input.size() == 0) break;
      KJ_IF_SOME(n, findParamDelimiter(input)) {
        // If the delimiter found is a ; then the parameter is invalid here, and
        // we will ignore it.
        if (input[n] == ';') {
          input = input.slice(n + 1);
          continue;
        }
        KJ_ASSERT(input[n] == '=');
        auto nameCandidate = input.first(n);
        input = input.slice(n + 1);
        if (nameCandidate.size() == 0 || hasInvalidCodepoints(nameCandidate, isTokenChar)) {
          // The name is invalid, try skipping to the next...
          KJ_IF_SOME(p, input.findFirst(';')) {
            input = input.slice(p + 1);
            continue;
          } else {
            break;
          }
        }
        if (input.size() == 0) break;

        // Check to see if the value starts off quoted or not.
        if (*input.begin() == '"') {
          // Collect an HTTP quoted string per Fetch spec §2.6, with extract-value=true.
          // Process character-by-character to correctly handle backslash escapes.
          input = input.slice(1);  // Skip opening quote
          auto valueBuf = kj::heapString(input.size());
          char* out = valueBuf.begin();
          while (input.size() > 0) {
            char c = input[0];
            if (c == '"') {
              // Closing quote found
              input = input.slice(1);
              break;
            } else if (c == '\\') {
              input = input.slice(1);
              if (input.size() == 0) {
                // Trailing backslash at end of input — append literal backslash per spec
                *out++ = '\\';
                break;
              }
              *out++ = input[0];
              input = input.slice(1);
            } else {
              *out++ = c;
              input = input.slice(1);
            }
          }
          auto valueCandidate =
              kj::heapString(kj::arrayPtr(valueBuf.begin(), out - valueBuf.begin()));
          // Spec step 11.8.2: skip any trailing content to the next ';'
          KJ_IF_SOME(p, input.findFirst(';')) {
            MimeTypeAccess::addParam(result, nameCandidate, valueCandidate);
            input = input.slice(p + 1);
            continue;
          }
          MimeTypeAccess::addParam(result, nameCandidate, valueCandidate);
          break;
        } else {
          // The parameter is not quoted. Let's scan ahead for the next semi-colon.
          KJ_IF_SOME(p, input.findFirst(';')) {
            auto valueCandidate = trimWhitespace(input.first(p));
            input = input.slice(p + 1);
            if (valueCandidate.size() > 0 &&
                !hasInvalidCodepoints(valueCandidate, isQuotedStringTokenChar)) {
              MimeTypeAccess::addParam(result, nameCandidate, valueCandidate);
            }
            continue;
          } else {
            auto valueCandidate = trimWhitespace(input);
            if (valueCandidate.size() > 0 &&
                !hasInvalidCodepoints(valueCandidate, isQuotedStringTokenChar)) {
              MimeTypeAccess::addParam(result, nameCandidate, valueCandidate);
            }
          }
          break;
        }
      } else {
        // If we got here, we scanned input and did not find a semi-colon or equal
        // sign before hitting the end of the input. We treat the remaining bits as
        // invalid and ignore them.
        break;
      }
    }
  }

  return kj::mv(result);
}

bool isMimeToken(kj::ArrayPtr<const char> s) {
  return s.size() > 0 && !hasInvalidCodepoints(s, isTokenChar);
}

bool isMimeParamValue(kj::ArrayPtr<const char> s) {
  return !hasInvalidCodepoints(s, isQuotedStringTokenChar);
}

bool MimeTypeAccess::addParam(
    MimeType& mimeType, kj::ArrayPtr<const char> name, kj::ArrayPtr<const char> value) {
  if (!isMimeToken(name) || !isMimeParamValue(value)) {
    return false;
  }
  mimeType.params_.upsert(toLower(name), kj::str(value), [](auto&, auto&&) {});
  return true;
}

namespace {

// 128 bytes will keep all reasonable mimetypes on the stack.
using ToStringBuffer = StringBuffer<128>;

void paramsToString(const MimeType& mimeType, ToStringBuffer& buffer) {
  bool first = true;
  for (auto& param: mimeType.params()) {
    buffer.append(first ? "" : ";");
    if (param.key == "boundary") {
      // This is to pass a pedantic WPT test that expects a space before only the boundary parameter
      // [1]: https://html.spec.whatwg.org/#submit-body
      // [2]: https://github.com/web-platform-tests/wpt/pull/29554
      buffer.append(" ");
    }

    buffer.append(param.key, "=");
    first = false;
    if (param.value.size() == 0) {
      buffer.append("\"\"");
    } else if (hasInvalidCodepoints(param.value, isTokenChar)) {
      auto view = param.value.asPtr();
      buffer.append("\"");
      while (view.size() > 0) {
        // Find the next character that needs escaping (per MIME Sniffing §4.1 step 4.4.1:
        // precede each occurrence of U+0022 (") or U+005C (\) with U+005C (\)).
        size_t i = 0;
        while (i < view.size() && view[i] != '"' && view[i] != '\\') ++i;
        buffer.append(view.first(i));
        if (i < view.size()) {
          buffer.append("\\", view.slice(i, i + 1));
          view = view.slice(i + 1);
        } else {
          break;
        }
      }
      buffer.append("\"");
    } else {
      buffer.append(param.value);
    }
  }
}

}  // namespace

kj::String mimeTypeParamsToString(const MimeType& mimeType) {
  ToStringBuffer buffer(512);
  paramsToString(mimeType, buffer);
  return buffer.toString();
}

kj::String mimeTypeToString(const MimeType& mimeType) {
  ToStringBuffer buffer(512);
  buffer.append(mimeType.type(), "/", mimeType.subtype());
  if (mimeType.params().size() > 0) {
    buffer.append(";");
    paramsToString(mimeType, buffer);
  }
  return buffer.toString();
}

kj::Maybe<MimeType> extractMimeType(kj::StringPtr input) {
  kj::Maybe<MimeType> mimeType;

  constexpr static auto findNextSeparator = [](auto& input) -> kj::Maybe<size_t> {
    // Scans input to find the next comma (,) that is not contained within
    // a quoted section, returning the position of the comma or kj::none
    // if not found.
    for (size_t i = 0; i < input.size(); ++i) {
      if (input[i] == '"' && (i == 0 || input[i - 1] != '\\')) {
        // Skip to the end of the quoted section
        while (++i < input.size() && (input[i] != '"' || input[i - 1] == '\\')) {}
      } else if (input[i] == ',' && (i == 0 || input[i - 1] != '\\')) {
        return i;
      }
    }
    return kj::none;
  };

  constexpr static auto processPart = [](auto& mimeType, auto& part) -> kj::Maybe<MimeType> {
    KJ_IF_SOME(parsed, tryParseMimeType(part)) {
      if (parsed == MimeType::WILDCARD) return kj::none;

      KJ_IF_SOME(current, mimeType) {
        if (current == parsed) {
          // mimeType will be set to parsed, but if parsed does not
          // have a charset, we will set the charset from current, if any.
          if (parsed.params().find("charset"_kj) == kj::none) {
            KJ_IF_SOME(charset, current.params().find("charset"_kj)) {
              MimeTypeAccess::addParam(parsed, "charset"_kj, charset);
            }
          }
        }
      }
      return kj::mv(parsed);
    }

    return kj::none;
  };

  while (input.size() > 0) {
    KJ_IF_SOME(pos, findNextSeparator(input)) {
      auto part = input.first(pos);
      input = input.slice(pos + 1);
      KJ_IF_SOME(parsed, processPart(mimeType, part)) {
        mimeType = kj::mv(parsed);
      } else {
        continue;
      }
    } else {
      KJ_IF_SOME(parsed, processPart(mimeType, input)) {
        mimeType = kj::mv(parsed);
      }
      break;
    }
  }

  return kj::mv(mimeType);
}

}  // namespace legacy
}  // namespace workerd
