// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "url-standard.h"
#include "blob.h"
#include "util.h"
#include <kj/array.h>
#include <cmath>
#include <map>
#include <string.h>
#include <unicode/ustring.h>
#include <unicode/uchar.h>
#include <unicode/uidna.h>
#include <unicode/utf8.h>
#include <arpa/inet.h>
#include <algorithm>
#include <numeric>

namespace workerd::api::url {

namespace {

struct Common {
  jsg::UsvString EMPTY_STRING = jsg::usv();
  jsg::UsvString SCHEME_BLOB = jsg::usv('b', 'l', 'o', 'b');
  jsg::UsvString SCHEME_FILE = jsg::usv('f', 'i', 'l', 'e');
  jsg::UsvString SCHEME_FTP = jsg::usv('f', 't', 'p');
  jsg::UsvString SCHEME_HTTP = jsg::usv('h', 't', 't', 'p');
  jsg::UsvString SCHEME_HTTPS = jsg::usv('h', 't', 't', 'p', 's');
  jsg::UsvString SCHEME_WS = jsg::usv('w', 's');
  jsg::UsvString SCHEME_WSS = jsg::usv('w', 's', 's');
  jsg::UsvString LOCALHOST = jsg::usv('l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't');
  jsg::UsvString NULL_ = jsg::usv('n', 'u', 'l', 'l');
};

Common& getCommonStrings() {
  static Common common;
  return common;
}

bool isSpecialScheme(jsg::UsvStringPtr scheme) {
  if (scheme == getCommonStrings().SCHEME_FILE) return true;
  if (scheme == getCommonStrings().SCHEME_FTP) return true;
  if (scheme == getCommonStrings().SCHEME_HTTP) return true;
  if (scheme == getCommonStrings().SCHEME_HTTPS) return true;
  if (scheme == getCommonStrings().SCHEME_WS) return true;
  if (scheme == getCommonStrings().SCHEME_WSS) return true;
  return false;
}

kj::Maybe<uint16_t> defaultPortForScheme(jsg::UsvStringPtr scheme) {
  if (scheme == getCommonStrings().SCHEME_HTTP) return 80;
  if (scheme == getCommonStrings().SCHEME_HTTPS) return 443;
  if (scheme == getCommonStrings().SCHEME_FTP) return 21;
  if (scheme == getCommonStrings().SCHEME_WS) return 80;
  if (scheme == getCommonStrings().SCHEME_WSS) return 443;
  return nullptr;
}

static kj::Maybe<jsg::UsvString> domainToAscii(jsg::UsvStringPtr input) {
  UErrorCode status = U_ZERO_ERROR;
  auto options =
      UIDNA_CHECK_BIDI |
      UIDNA_CHECK_CONTEXTJ |
      UIDNA_NONTRANSITIONAL_TO_ASCII;

  UIDNA* uidna = uidna_openUTS46(options, &status);
  if (U_FAILURE(status)) return nullptr;
  KJ_DEFER(uidna_close(uidna));
  UIDNAInfo info = UIDNA_INFO_INITIALIZER;

  const auto storage = input.toUtf16();

  // We're going to pre-flight to get the size of the array
  // we need to allocate.
  auto len = uidna_nameToASCII(
      uidna,
      reinterpret_cast<const UChar*>(storage.begin()),
      storage.size(),
      nullptr, 0, &info, &status);

  if (len == 0) return nullptr;

  if (status == U_BUFFER_OVERFLOW_ERROR) {
    status = U_ZERO_ERROR;
    auto dest = kj::heapArray<uint16_t>(len);
    len = uidna_nameToASCII(
        uidna,
        reinterpret_cast<const UChar*>(storage.begin()),
        storage.size(),
        reinterpret_cast<UChar*>(dest.begin()),
        len, &info, &status);

    // Note: The following snippet is adapted directly from Node.js' implementation.

    // In UTS #46 which specifies ToASCII, certain error conditions are
    // configurable through options, and the WHATWG URL Standard promptly elects
    // to disable some of them to accommodate for real-world use cases.
    // Unfortunately, ICU4C's IDNA module does not support disabling some of
    // these options through `options` above, and thus continues throwing
    // unnecessary errors. To counter this situation, we just filter out the
    // errors that may have happened afterwards, before deciding whether to
    // return an error from this function.

    // CheckHyphens = false
    // (Specified in the current UTS #46 draft rev. 18.)
    // Refs:
    // - https://github.com/whatwg/url/issues/53
    // - https://github.com/whatwg/url/pull/309
    // - http://www.unicode.org/review/pri317/
    // - http://www.unicode.org/reports/tr46/tr46-18.html
    // - https://www.icann.org/news/announcement-2000-01-07-en
    info.errors &= ~UIDNA_ERROR_HYPHEN_3_4;
    info.errors &= ~UIDNA_ERROR_LEADING_HYPHEN;
    info.errors &= ~UIDNA_ERROR_TRAILING_HYPHEN;
    info.errors &= ~UIDNA_ERROR_EMPTY_LABEL;
    info.errors &= ~UIDNA_ERROR_LABEL_TOO_LONG;
    info.errors &= ~UIDNA_ERROR_DOMAIN_NAME_TOO_LONG;

    if (U_FAILURE(status) || info.errors != 0) {
      // Failed!
      return nullptr;
    }

    return jsg::usv(kj::mv(dest));
  }

  // If we get here, then we errored during pre-flight.
  KJ_ASSERT(U_FAILURE(status));
  return nullptr;
}

struct UrlRecordBuilder {
  using Path = kj::OneOf<jsg::UsvStringBuilder, kj::Vector<jsg::UsvString>>;

  jsg::UsvString scheme;
  jsg::UsvStringBuilder username;
  jsg::UsvStringBuilder password;
  kj::Maybe<jsg::UsvString> host;
  kj::Maybe<uint16_t> port;
  Path path = kj::Vector<jsg::UsvString>();
  kj::Maybe<jsg::UsvStringBuilder> query;
  kj::Maybe<jsg::UsvStringBuilder> fragment;

  bool special = false;

  operator UrlRecord() { return finish(); }

  UrlRecordBuilder(UrlRecord& record)
    : scheme(jsg::usv(record.scheme)),
      host(record.host.map([](jsg::UsvString& string) { return jsg::usv(string); })),
      port(record.port),
      path(copyPath(record.path)),
      query(record.query.map([](jsg::UsvString& string) {
        jsg::UsvStringBuilder builder(string.size());
        builder.addAll(string);
        return kj::mv(builder);
      })),
      fragment(record.fragment.map([](jsg::UsvString& string) {
        jsg::UsvStringBuilder builder(string.size());
        builder.addAll(string);
        return kj::mv(builder);
      })),
      special(record.special) {
    username.addAll(record.username);
    password.addAll(record.password);
  }

  static Path copyPath(UrlRecord::Path& base) {
    KJ_SWITCH_ONEOF(base) {
      KJ_CASE_ONEOF(string, jsg::UsvString) {
        jsg::UsvStringBuilder builder(string.size());
        builder.addAll(string);
        return kj::mv(builder);
      }
      KJ_CASE_ONEOF(array, kj::Array<jsg::UsvString>) {
        kj::Vector<jsg::UsvString> strings = KJ_MAP(string, array) {
          return jsg::usv(string);
        };
        return kj::mv(strings);
      }
    }
    KJ_UNREACHABLE;
  }

  UrlRecord::Path finishPath() {
    KJ_SWITCH_ONEOF(path) {
      KJ_CASE_ONEOF(string, jsg::UsvStringBuilder) {
        return string.finish();
      }
      KJ_CASE_ONEOF(strings, kj::Vector<jsg::UsvString>) {
        return strings.releaseAsArray();
      }
    }
    KJ_UNREACHABLE;
  }

  UrlRecord finish() {
    return {
      .scheme = kj::mv(scheme),
      .username = username.finish(),
      .password = password.finish(),
      .host = kj::mv(host),
      .port = kj::mv(port),
      .path = finishPath(),
      .query = query.map([](jsg::UsvStringBuilder& str) { return str.finish(); }),
      .fragment = fragment.map([](jsg::UsvStringBuilder& str) { return str.finish(); }),
      .special = special,
    };
  }
};

bool isControlCodepoint(uint32_t codepoint) {
  return codepoint >= 0x00 && codepoint <= 0x1f;
}

bool isControlOrSpaceCodepoint(uint32_t codepoint) {
  return isControlCodepoint(codepoint) || codepoint == 0x20;
}

bool isAsciiDigitCodepoint(uint32_t codepoint) {
  return codepoint >= '0' && codepoint <= '9';
}

bool isAsciiUpperAlphaCodepoint(uint32_t codepoint) {
  return codepoint >= 'A' && codepoint <= 'Z';
}

bool isAsciiLowerAlphaCodepoint(uint32_t codepoint) {
  return codepoint >= 'a' && codepoint <= 'z';
}

bool isAsciiAlphaCodepoint(uint32_t codepoint) {
  return isAsciiUpperAlphaCodepoint(codepoint) ||
         isAsciiLowerAlphaCodepoint(codepoint);
}

bool isAsciiAlphaNumCodepoint(uint32_t codepoint) {
  return isAsciiDigitCodepoint(codepoint) ||
         isAsciiAlphaCodepoint(codepoint);
}

bool isForbiddenHostCodepoint(uint32_t c, bool excludePercent = false) {
  if (excludePercent && c == '%') return false;
  return c == 0x00 || c == 0x09 /* Tab */ || c == 0x0a /* LF */ || c == 0x0d /* CR */ ||
         c == ' ' || c == '#' || c == '%' || c == '/' || c == ':' ||
         c == '<' || c == '>' || c == '?' || c == '@' || c == '[' || c == '\\' || c == ']' ||
         c == '^' || c == '|';
}

bool controlPercentEncodeSet(uint32_t c) { return isControlCodepoint(c) || c > 0x7e; }

bool fragmentPercentEncodeSet(uint32_t c) {
  return controlPercentEncodeSet(c) || c == ' ' || c == '"' || c == '<' || c == '>' || c == '`';
}

bool queryPercentEncodeSet(uint32_t c) {
  return controlPercentEncodeSet(c) || c == ' ' || c == '"' || c == '#' || c == '<' || c == '>';
}

bool specialQueryPercentEncodeSet(uint32_t c) { return queryPercentEncodeSet(c) || c == '\''; }

bool pathPercentEncodeSet(uint32_t c) {
  return queryPercentEncodeSet(c) || c == '?' || c == '`' || c == '{' || c == '}';
}

bool userInfoPercentEncodeSet(uint32_t c) {
  return pathPercentEncodeSet(c) || c == '/' || c == ':' || c == ';' || c == '=' || c == '@' ||
                                    (c >= '[' && c <= '^') || c == '|';
}

bool componentPercentEncodeSet(uint32_t c) {
  return userInfoPercentEncodeSet(c) || (c >= '$' && c <= '&') || c == '+' || c == ',';
}

bool urlEncodedPercentEncodeSet(uint32_t c) {
  return componentPercentEncodeSet(c) || c == '!' || (c >= '\'' && c <= ')') || c == '~';
}

size_t codepointToUtf8(kj::byte buf[4], uint32_t codepoint) {
  auto len = U8_LENGTH(codepoint);
  KJ_ASSERT(len <= 4);
  auto pos = 0;
  U8_APPEND_UNSAFE(buf, pos, codepoint);
  return len;
}

enum class HexEncodeOption {
  NONE,
  LOWER,
  SHORTEST,
};
inline constexpr HexEncodeOption operator|(HexEncodeOption a, HexEncodeOption b) {
  return static_cast<HexEncodeOption>(static_cast<uint>(a) | static_cast<uint>(b));
}
inline constexpr HexEncodeOption operator&(HexEncodeOption a, HexEncodeOption b) {
  return static_cast<HexEncodeOption>(static_cast<uint>(a) & static_cast<uint>(b));
}
inline constexpr HexEncodeOption operator~(HexEncodeOption a) {
  return static_cast<HexEncodeOption>(~static_cast<uint>(a));
}

void hexEncode(
    jsg::UsvStringBuilder& builder,
    auto value,
    HexEncodeOption option = HexEncodeOption::NONE) {
  // This hexEncode differs from kj::hex in that it supports encoding
  // individual bytes or uint16_t. It also supports the ability to
  // selectively encode using uppercase or lowercase hex values, and
  // encoding using the shortest sequence of hex digits necessary
  // for a value. The options are particularly important for the URLs
  // rules for encoding normalized IPv6 address, which must use the lower
  // case and shortest-sequence options.
  static uint32_t HEX[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                           '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  auto lower = (option & HexEncodeOption::LOWER) == HexEncodeOption::LOWER;
  auto maybeLower = [&lower](uint32_t cp) { return lower ? cp | 0x20 : cp; };

  const auto encodeNibble = [&option, &builder, &maybeLower](kj::byte byte) {
    auto shortest = (option & HexEncodeOption::SHORTEST) == HexEncodeOption::SHORTEST;
    if (shortest && byte == 0) return;
    auto first = byte >> 4;
    if (!shortest || (shortest && first > 0)) {
      builder.add(maybeLower(HEX[first]));
    }
    builder.add(maybeLower(HEX[byte & ~0xf0]));
  };

  if constexpr (kj::isSameType<decltype(value), kj::byte>()) {
    encodeNibble(value);
  } else if constexpr (kj::isSameType<decltype(value), uint16_t>()) {
    auto first = kj::byte(value >> 8);
    encodeNibble(first);
    if (first > 0) {
      // If first is greater than 0, don't use shortest for the next byte.
      option = option & ~HexEncodeOption::SHORTEST;
    }
    encodeNibble(kj::byte(value & ~0xFF00));
  }
}

kj::byte fromHexDigit(uint32_t c) {
  KJ_ASSERT(isHexDigit(c));
  if ('0' <= c && c <= '9') {
    return c - '0';
  } else if ('a' <= c && c <= 'f') {
    return c - ('a' - 10);
  } else if ('A' <= c && c <= 'F') {
    return c - ('A' - 10);
  }
  KJ_UNREACHABLE;
};

void percentEncodeCodepoint(
    jsg::UsvStringBuilder& builder,
    uint32_t codepoint,
    auto predicate,
    bool spaceAsPlus = false) {
  kj::byte buf[4];
  if (predicate(codepoint)) {
    // In the worst case we will need to percent encode four
    // utf8 bytes, which translates into 12 separate codepoints.
    if (spaceAsPlus && codepoint == ' ') {
      builder.add('+');
      return;
    }
    auto len = codepointToUtf8(buf, codepoint);
    for (auto n = 0; n < len; n++) {
      builder.add('%');
      hexEncode(builder, buf[n]);
    }
  } else {
    builder.add(codepoint);
  }
}

bool codepointIs(auto c) { return false; }

bool codepointIs(auto c, auto codepoint, auto... rest) {
  return c == codepoint || codepointIs(c, kj::fwd<decltype(rest)>(rest)...);
}

bool nextCodepointIs(auto it, auto... codepoints) {
  KJ_ASSERT(bool(it));
  auto next = it + 1;
  return next && codepointIs(*next, kj::fwd<decltype(codepoints)>(codepoints)...);
};

jsg::UsvString percentDecode(jsg::UsvStringPtr input) {
  // This is essentially a more lenient alternative to kj::decodeUriComponent
  // that follows the guidelines of the URL standard spec. Invalid sequences
  // are simply ignored and passed through as-is to the result.

  // At the worst case, the result is as long as the input. That said, the
  // input is user defined, so let's cap how much we reserve to something
  // reasonable.
  kj::Vector<char> result(kj::min(input.size(), 32));
  auto it = input.begin();

  const auto appendAsUtf8 = [&result](auto codepoint) {
    kj::byte buf[4];
    auto len = codepointToUtf8(buf, codepoint);
    for (auto n = 0; n < len; n++) {
      result.add(buf[n]);
    }
  };

  while (it) {
    auto c = *it;
    if (c != '%') {
      appendAsUtf8(c);
      ++it;
      continue;
    }
    auto next = it + 1;
    if (!next) {
      result.add(c);
      ++it;
      continue;
    }
    auto digit1 = *next;
    if (!isHexDigit(digit1)) {
      result.add(c);
      ++it;
      continue;
    }
    if (!(++next)) {
      result.add(c);
      ++it;
      continue;
    }
    auto digit2 = *next;
    if (!isHexDigit(digit2)) {
      result.add(c);
      ++it;
      continue;
    }

    result.add(fromHexDigit(digit1) << 4 | fromHexDigit(digit2));
    it += 3;
  }

  return jsg::usv(result.releaseAsArray());
}

UrlRecord handleConstructorParse(
    jsg::UsvStringPtr url,
    jsg::Optional<jsg::UsvStringPtr> maybeBase) {
  KJ_IF_MAYBE(base, maybeBase) {
    UrlRecord baseRecord =
        JSG_REQUIRE_NONNULL(URL::parse(*base), TypeError, "Invalid base URL string.");
    return JSG_REQUIRE_NONNULL(URL::parse(url, baseRecord), TypeError, "Invalid URL string.");
  }
  return JSG_REQUIRE_NONNULL(URL::parse(url), TypeError, "Invalid URL string.");
}

bool cannotHaveUsernamePasswordOrPort(UrlRecord& record) {
  if (record.scheme == getCommonStrings().SCHEME_FILE) return true;
  KJ_IF_MAYBE(host, record.host) {
    return *host == getCommonStrings().EMPTY_STRING;
  }
  return true;
}

kj::Maybe<jsg::UsvStringPtr> toMaybePtr(kj::Maybe<jsg::UsvString>& str) {
  return str.map([](jsg::UsvString& str) { return str.asPtr(); });
}

jsg::UsvStringIterator seek(jsg::UsvStringIterator& it, uint32_t delimiter) {
  while (it) {
    if (*it == delimiter) {
      return it;
    }
    ++it;
  }
  return it;
}

}  // namespace

Origin UrlRecord::getOrigin() {
  if (special && scheme != getCommonStrings().SCHEME_FILE) {
    // Covers http, http2, ftp, ws, and wss
    return TupleOrigin {
      .scheme = scheme,
      .host = KJ_ASSERT_NONNULL(host),
      .port = port,
    };
  }
  // TODO (later): Support Blob Origins
  return OpaqueOrigin {};
}

jsg::UsvString UrlRecord::getPathname() {
  KJ_SWITCH_ONEOF(path) {
    KJ_CASE_ONEOF(string, jsg::UsvString) {
      return jsg::usv(string);
    }
    KJ_CASE_ONEOF(strings, kj::Array<jsg::UsvString>) {
      // The initial reserved capacity here is just a guess since we don't
      // know exactly how much we'll actually need.
      auto size = std::accumulate(
          strings.begin(), strings.end(),
          strings.size(),  // One for each '/' prefix.
          [](uint z, jsg::UsvStringPtr s) { return z + s.size(); });
      jsg::UsvStringBuilder builder(size);
      for (auto& segment : strings) {
        builder.add('/');
        builder.addAll(segment);
      }
      return builder.finish();
    }
  }
  KJ_UNREACHABLE
}

jsg::UsvString UrlRecord::getHref(GetHrefOption option) {
  // The reservation size here is fairly arbitrary.
  jsg::UsvStringBuilder builder(255);
  builder.addAll(scheme);
  builder.add(':');
  KJ_IF_MAYBE(h, host) {
    builder.add('/', '/');
    if (!username.empty() || !password.empty()) {
      builder.addAll(username);
      if (!password.empty()) {
        builder.add(':');
        builder.addAll(password);
      }
      builder.add('@');
    }
    builder.addAll(*h);
    KJ_IF_MAYBE(p, port) {
      builder.add(':');
      builder.addAll(kj::toCharSequence(*p));
    }
  } else {
    KJ_IF_MAYBE(segments, path.tryGet<kj::Array<jsg::UsvString>>()) {
      if (segments->size() > 1 && (*segments)[0].empty()) {
        builder.add('/', '.');
      }
    }
  }
  builder.addAll(getPathname());
  KJ_IF_MAYBE(q, query) {
    builder.add('?');
    builder.addAll(*q);
  }
  if (option != GetHrefOption::EXCLUDE_FRAGMENT) {
    KJ_IF_MAYBE(f, fragment) {
      builder.add('#');
      builder.addAll(*f);
    }
  }
  return builder.finish();
}

void UrlRecord::setUsername(jsg::UsvStringPtr value) {
  if (value.empty()) {
    username = jsg::usv();
    return;
  }

  jsg::UsvStringBuilder builder;
  auto it = value.begin();
  while (it) {
    percentEncodeCodepoint(builder, *it, userInfoPercentEncodeSet);
    ++it;
  }
  username = builder.finish();
}

void UrlRecord::setPassword(jsg::UsvStringPtr value) {
  if (value.empty()) {
    password = jsg::usv();
    return;
  }

  jsg::UsvStringBuilder builder;
  auto it = value.begin();
  while (it) {
    percentEncodeCodepoint(builder, *it, userInfoPercentEncodeSet);
    ++it;
  }
  password = builder.finish();
}

bool UrlRecord::operator==(UrlRecord& other) {
  return equivalentTo(other);
}

bool UrlRecord::equivalentTo(UrlRecord& other, GetHrefOption option) {
  auto href = getHref(option);
  return href == other.getHref(option);
}

kj::Maybe<UrlRecord> URL::parse(
    jsg::UsvStringPtr input,
    jsg::Optional<UrlRecord&> maybeBase,
    kj::Maybe<UrlRecord&> maybeRecord,
    kj::Maybe<ParseState> maybeStateOverride) {
  static UrlRecord EMPTY_RECORD = UrlRecord {};
  UrlRecordBuilder record(maybeRecord.orDefault(EMPTY_RECORD));
  ParseState state = maybeStateOverride.orDefault(ParseState::SCHEME_START);
  // Worst case is that buffer will be the size of input, but that's unlikely,
  // and since user input is user controled, let's cap it at something reasonable.
  jsg::UsvStringBuilder buffer(kj::min(input.size(), 64));
  bool atSignSeen = false;
  bool insideBrackets = false;
  bool passwordTokenSeen = false;

  const auto trimControlOrSpace = [&maybeRecord](jsg::UsvStringPtr& input) {
    if (input.empty()) return jsg::usv();
    auto start = input.begin();
    auto end = input.end() - 1;

    // If the existing URL record is not provided, we trim off leading and
    // trailing whitespace... otherwise we leave it.
    if (maybeRecord == nullptr) {
      while (start && isControlOrSpaceCodepoint(*start)) { ++start; }
      while (end && end > start && isControlOrSpaceCodepoint(*end)) { --end; }

      // Now go through the remaining and strip tabs and newlines.
      input = input.slice(start, end + 1);
    }

    if (input.empty()) return jsg::usv();

    // But we always strip tabs and new-lines in the input.
    jsg::UsvStringBuilder res(input.size());
    auto it = input.begin();
    while (it) {
      auto c = *it;
      if (c != 0x09 /* tab */ && c != 0x0a /* lf */ && c != 0x0d /* cr */) {
        res.add(c);
      }
      ++it;
    }

    return res.finish();
  };

  // Per the spec, we have to trim leading control and space characters.
  auto processed = trimControlOrSpace(input);

  auto it = processed.begin();

  const auto isWindowsDriveLetter = [](jsg::UsvStringPtr str, bool normalized = false) {
    if (str.size() != 2) return false;
    auto it = str.begin();
    auto c = *it;
    if (!isAsciiAlphaCodepoint(c)) return false;
    ++it;
    c = *it;
    return c == ':' || (!normalized && c == '|');
  };

  const auto isWindowsDriveLetterFileQuirk = [](const kj::Vector<uint32_t>& storage) {
    if (storage.size() != 2) return false;
    auto c = storage[0];
    if (!isAsciiAlphaCodepoint(c)) return false;
    c = storage[1];
    return c == ':' || c == '|';
  };

  const auto startsWithWindowsDriveLetter =
      [&isWindowsDriveLetter](jsg::UsvStringPtr str, bool normalized = false) {
    auto size = str.size();
    if (size < 2) return false;
    if (!isWindowsDriveLetter(str.slice(0, 2))) return false;
    if (size == 2) return true;
    auto c = str.getCodepointAt(2);
    return c == '/' || c == '\\' || c == '?' || c == '#';
  };

  const auto shortenPath = [&startsWithWindowsDriveLetter](UrlRecordBuilder& record) {
    auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Vector<jsg::UsvString>>());
    if (!(record.scheme == getCommonStrings().SCHEME_FILE &&
          path.size() == 1 &&
          startsWithWindowsDriveLetter(path[0], true /* Normalized */))) {
      if (!path.empty()) path.removeLast();
    }
  };

  const auto findIpv6CompressIndex = [](const auto& pieces) -> kj::Maybe<uint> {
    // Given a sequence of uint16_t values, find the index at which the longest
    // contiguous sequence of two or more zero values begins. For instance, given
    // the address [ABCD:0:1234:0:0:2:0:0], the index returned would be 3.
    kj::Maybe<uint> maybeIndex;
    auto currentIndex = 0;
    auto prevCount = 0;
    auto count = 0;
    auto prevWasZero = false;
    for (int n = 0; n < 8; n++) {
      if (pieces[n] == 0) {
        if (!prevWasZero) {
          // We're starting a new span.
          if (maybeIndex == nullptr) maybeIndex = n;
          prevWasZero = true;
          currentIndex = n;
          count = 1;
          continue;
        }
        KJ_ASSERT(prevWasZero);
        count++;
      } else {
        // We finished a span!
        prevWasZero = false;
        if (count > prevCount) {
          // We found a longer segment.
          maybeIndex = currentIndex;
          prevCount = count;
          count = 0;
        }
      }
    }
    if (count > prevCount) {
      maybeIndex = currentIndex;
      prevCount = count;
    }
    return prevCount > 1 ? maybeIndex : nullptr;
  };

  const auto ipv6Parse =
      [&findIpv6CompressIndex](jsg::UsvStringPtr input) -> kj::Maybe<jsg::UsvString> {
    // We're going to cheat here a little. The URL spec describes an algorithm
    // for parsing IPv6 addresses but we're just going to use inet_pton to do
    // it for us since that produces spec compliant results.
    uint16_t pieces[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    kj::byte buf[sizeof(struct in6_addr)];
    auto str = input.toStr();

    auto ret = inet_pton(AF_INET6, str.begin(), buf);
    if (ret != 1) return nullptr;
    for (int i = 0; i < 16; i += 2) {
      pieces[i >> 1] = (buf[i] << 8) | buf[i + 1];
    }

    jsg::UsvStringBuilder builder(41);
    builder.add('[');
    auto maybeCompress = findIpv6CompressIndex(pieces);
    bool ignore0 = false;
    for (int n = 0; n <= 7; n++) {
      auto piece = pieces[n];
      if (ignore0 && piece == 0) {
        continue;
      } else if (ignore0) {
        ignore0 = false;
      }
      KJ_IF_MAYBE(compress, maybeCompress) {
        if (*compress == n) {
          if (n == 0) {
            builder.add(':', ':');
          } else {
            builder.add(':');
          }
          ignore0 = true;
          continue;
        }
      }
      hexEncode(builder, piece, HexEncodeOption::LOWER | HexEncodeOption::SHORTEST);
      if (n < 7) {
        builder.add(':');
      }
    }
    builder.add(']');
    return builder.finish();
  };

  const auto parseNumber = [](jsg::UsvStringPtr input) -> kj::Maybe<uint64_t> {
    // Parses base 8, 10, and 16 numbers
    auto size = input.size();
    KJ_ASSERT(size > 0);
    auto r = 10;
    auto it = input.begin();
    auto start = it;
    while (it) {
      auto c = *it;
      // If the first digit is a zero, then we're using hex or octal notation.
      if (it.position() == 0 && c == '0') {
        if (nextCodepointIs(it, 'x', 'X')) {
          r = 16;
          it += 2;
          start = it;
          continue;
        }
        if (r == 10 && size > 1) {
          r = 8;
          ++it;
          start = it;
          continue;
        }
      }
      switch (r) {
        case 8: {
          if (c < '0' || c > '7') return nullptr;
          break;
        }
        case 10: {
          if (!isAsciiDigitCodepoint(c)) return nullptr;
          break;
        }
        case 16:
          if (!isHexDigit(c)) {
            return nullptr;
          }
          break;
      }
      ++it;
    }
    auto str = kj::str(input.slice(start));
    return strtoll(str.begin(), nullptr, r);
  };

  const auto endsWithNumber = [&parseNumber](jsg::UsvStringPtr input) {
    kj::Vector<jsg::UsvStringPtr> parts(4);
    auto it = input.begin();
    auto start = it;
    while (it) {
      auto end = seek(it, '.');
      // Ignore the last empty segment if any.
      if (end == start && !it) break;
      parts.add(input.slice(start, end));
      if (!it) break; // Reached the end.
      start = ++it;
    }

    if (parts.empty()) return false;
    auto& last = parts.back();
    if (last.empty()) return false;
    // If every codepoint in the last part is an ascii digit,
    // return true. Otherwise, try parsing the number and if it
    // comes back as null, return false.
    it = last.begin();
    while (it) {
      if (!isAsciiDigitCodepoint(*it)) {
        return parseNumber(last) != nullptr;
      }
      ++it;
    }
    return true;
  };

  const auto ipv4Parse = [&parseNumber](jsg::UsvStringPtr input) -> kj::Maybe<jsg::UsvString> {
    kj::Vector<uint64_t> numbers(4);
    auto it = input.begin();
    auto start = it;
    auto tooBig = 0;

    // What is this doing? I'm glad you asked. We should all be familiar with
    // the standard dot notation format for ipv4 (e.g. 123.123.123.123). But,
    // what you may not know is that there are other ways of representing ipv4
    // addresses that are far less common but still supported by the WHATWG
    // URL standard. This algorithm parses the various possible formats into
    // a single number, then generates the standard encoded from that number
    // so that no matter what we take in, we're generating a consistent,
    // normalized result.

    while (it) {
      auto end = seek(it, '.');
      if (end == start) {
        // If the empty segment is the last segment, ignore it.
        if (!it) break;
        // Otherwise fail the parse.
        return nullptr;
      }

      KJ_IF_MAYBE(number, parseNumber(input.slice(start, end))) {
        if (*number > 255) tooBig++;
        numbers.add(*number);
      } else {
        return nullptr;
      }

      if (!it) break; // Reached the end.
      start = ++it;
    }

    if (numbers.size() > 4) return nullptr;

    uint64_t ipv4 = numbers.back();

    if (tooBig > 1 ||
        (tooBig == 1 && ipv4 <= 255) ||
        ipv4 >= std::pow(256, 5 - numbers.size())) {
      return nullptr;
    }

    numbers.removeLast();
    auto term = 1 << 24;
    for (auto number : numbers) {
      ipv4 += number * term;
      term >>= 8;
    }

    jsg::UsvStringBuilder builder(15);
    for (auto i = 4; i > 0; --i) {
      kj::byte octet = 0xff & (ipv4 >> (8 * (i - 1)));
      builder.addAll(kj::toCharSequence(octet));
      if (i != 1) builder.add('.');
    }

    return builder.finish();
  };

  const auto containsForbiddenHostCodePoint = [](jsg::UsvStringPtr input,
                                                 bool excludePercent = false) {
    auto it = input.begin();
    while (it) {
      if (isForbiddenHostCodepoint(*it, excludePercent)) return true;
      ++it;
    }
    return false;
  };

  const auto opaqueHostParse = [](jsg::UsvStringPtr input) -> kj::Maybe<jsg::UsvString> {
    jsg::UsvStringBuilder builder;
    auto it = input.begin();
    while (it) {
      auto c = *it;
      if (isForbiddenHostCodepoint(c, true /* Ignore % */)) return nullptr;
      percentEncodeCodepoint(builder, c, &controlPercentEncodeSet);
      ++it;
    }
    return builder.finish();
  };

  const auto hostParseBuffer =
      [&ipv4Parse, &ipv6Parse, &opaqueHostParse, &containsForbiddenHostCodePoint,
       &endsWithNumber, &buffer](bool notSpecial) -> kj::Maybe<jsg::UsvString> {
    KJ_DEFER(buffer.clear());
    auto string = buffer.asPtr();
    if (!string.empty() && string.first() == '[') {
      if (string.last() != ']') return nullptr;
      return ipv6Parse(string.slice(1, string.size() - 1));
    }
    if (notSpecial) return opaqueHostParse(string);
    if (string.empty()) return nullptr;
    KJ_IF_MAYBE(asciiDomain, domainToAscii(percentDecode(string))) {
      // Can't be zero-length or contain invalid codepoints
      if (!asciiDomain->empty() && !containsForbiddenHostCodePoint(*asciiDomain)) {
        return endsWithNumber(*asciiDomain) ? ipv4Parse(*asciiDomain) : kj::mv(*asciiDomain);
      }
    }
    // Failed!
    return nullptr;
  };

  const auto appendToPath = [&record](jsg::UsvString str = jsg::usv()) {
    // The appendToPath should only be called when record.path is a vector.
    KJ_ASSERT_NONNULL(record.path.tryGet<kj::Vector<jsg::UsvString>>()).add(kj::mv(str));
  };

  const auto countOnlyDots = [](jsg::UsvStringPtr string) -> kj::Maybe<uint> {
    auto count = 0;
    auto it = string.begin();
    while (it) {
      switch (*it) {
        case '.':
          ++count;
          ++it;
          continue;
        case '%':
          if (nextCodepointIs(it, '2') && nextCodepointIs(it + 1, 'e', 'E')) {
            ++count;
            it += 3;
            continue;
          }
          break;
      }
      return nullptr;
    }
    return count;
  };

  const auto isDoubleDotSegment = [&countOnlyDots](jsg::UsvStringPtr string) {
    auto size = string.size();
    if (size < 2 || size > 6) return false;
    KJ_IF_MAYBE(count, countOnlyDots(string)) {
      return *count == 2;
    }
    return false;
  };

  const auto isSingleDotSegment = [&countOnlyDots](jsg::UsvStringPtr string) {
    auto size = string.size();
    if (size < 1 || size > 3) return false;
    KJ_IF_MAYBE(count, countOnlyDots(string)) {
      return *count == 1;
    }
    return false;
  };

  const auto pathIsEmpty = [](UrlRecordBuilder& record) {
    KJ_SWITCH_ONEOF(record.path) {
      KJ_CASE_ONEOF(string, jsg::UsvStringBuilder) {
        return string.empty();
      }
      KJ_CASE_ONEOF(strings, kj::Vector<jsg::UsvString>) {
        return strings.empty();
      }
    }
    KJ_UNREACHABLE;
  };

  const auto currentCodepoint = [&it] { return !it ? 0 : *it; };

  while (true) {
    auto c = currentCodepoint();

    switch (state) {
      case ParseState::SCHEME_START: {
        if (isAsciiAlphaCodepoint(c)) {
          buffer.add(c | 0x20);  // Append the lower-case
          state = ParseState::SCHEME;
          break;
        } else if (maybeStateOverride == nullptr) {
          state = ParseState::NO_SCHEME;
          it = processed.begin();  // Start over!
          continue;
        }
        return nullptr;
      }
      case ParseState::SCHEME: {
        if (isAsciiAlphaNumCodepoint(c) || c == '+' || c == '-' || c == '.') {
          buffer.add(u_tolower(c));
          break;
        } else if (c == ':') {
          KJ_DEFER(buffer.clear());
          auto temp = buffer.asPtr();
          auto tempSpecial = isSpecialScheme(temp);
          if (maybeStateOverride != nullptr) {
            if (record.special != tempSpecial) {
              return UrlRecord(record);
            }
            if ((!record.username.empty() ||
                 !record.password.empty() ||
                 record.port != nullptr) &&
                 temp == getCommonStrings().SCHEME_FILE) {
              return UrlRecord(record);
            }
            if (record.scheme == getCommonStrings().SCHEME_FILE &&
                record.host.orDefault(jsg::usv()).empty()) {
              return UrlRecord(record);
            }
          }
          record.scheme = jsg::usv(temp);
          record.special = tempSpecial;
          if (maybeStateOverride != nullptr) {
            KJ_IF_MAYBE(port, record.port) {
              if (defaultPortForScheme(record.scheme) == *port) {
                record.port = nullptr;
              }
            }
            return UrlRecord(record);
          }
          if (record.scheme == getCommonStrings().SCHEME_FILE) {
            // If remaining does not start with //, it's a validation error.
            // But the spec doesn't require us to fail. So let's ignore.
            // Here's what the spec says about validation errors:
            //   A validation error does not mean that the parser terminates...
            //
            //   It is useful to signal validation errors as error-handling can
            //   be non-intuitive, legacy user agents might not implement correct
            //   error-handling, and the intent of what is written might be unclear
            //   to other developers.
            //
            // The URL API does not provide any way of communicating validation
            // errors and there's not a lot of reason for us to log them. The parsing
            // algorithm tolerates these so we will too, silently.
            state = ParseState::FILE;
            break;
          }
          if (record.special) {
            KJ_IF_MAYBE(baseRecord, maybeBase) {
              if (!baseRecord->special)
                return nullptr;
              if (baseRecord->scheme == record.scheme) {
                state = ParseState::SPECIAL_RELATIVE_OR_AUTHORITY;
                break;
              }
            }
            state = ParseState::SPECIAL_AUTHORITY_SLASHES;
            break;
          }
          KJ_ASSERT(!record.special);
          if (!nextCodepointIs(it, '/')) {
            record.path = jsg::UsvStringBuilder();
            state = ParseState::OPAQUE_PATH;
            break;
          }
          state = ParseState::PATH_OR_AUTHORITY;
          ++it;
          break;
        }
        if (maybeStateOverride == nullptr) {
          buffer.clear();
          state = ParseState::NO_SCHEME;
          it = processed.begin();  // Start over!
          continue;
        }
        return nullptr;
      }
      case ParseState::NO_SCHEME: {
        KJ_IF_MAYBE(baseRecord, maybeBase) {
          KJ_IF_MAYBE(opaquePath, baseRecord->path.tryGet<jsg::UsvString>()) {
            if (c != '#') return nullptr;
            record.scheme = jsg::usv(baseRecord->scheme);
            record.special = isSpecialScheme(record.scheme);
            record.path = UrlRecordBuilder::copyPath(baseRecord->path);
            KJ_IF_MAYBE(q, baseRecord->query) {
              jsg::UsvStringBuilder query;
              KJ_IF_MAYBE(q, baseRecord->query) { query.addAll(*q); }
              record.query = kj::mv(query);
            }
            record.fragment = jsg::UsvStringBuilder();
            state = ParseState::FRAGMENT;
            break;
          }
          state = baseRecord->scheme == getCommonStrings().SCHEME_FILE ?
              ParseState::FILE :
              ParseState::RELATIVE;
          continue;  // Continue without incrementing the iterator.
        }
        return nullptr;
      }
      case ParseState::SPECIAL_RELATIVE_OR_AUTHORITY: {
        if (c == '/' && nextCodepointIs(it, '/')) {
          state = ParseState::SPECIAL_AUTHORITY_IGNORE_SLASHES;
          it++;
          break;
        }
        // Validation error, but we're not reporting it.
        state = ParseState::RELATIVE;
        continue; // Continue without incrementing the iterator.
      }
      case ParseState::PATH_OR_AUTHORITY: {
        if (c == '/') {
          state = ParseState::AUTHORITY;
          break;
        }
        state = ParseState::PATH;
        continue; // Continue without incrementing the iterator.
      }
      case ParseState::RELATIVE: {
        auto& baseRecord = KJ_ASSERT_NONNULL(maybeBase);
        KJ_ASSERT(baseRecord.scheme != getCommonStrings().SCHEME_FILE);
        record.scheme = jsg::usv(baseRecord.scheme);
        record.special = isSpecialScheme(record.scheme);
        if (c == '/') {
          state = ParseState::RELATIVE_SLASH;
          break;
        } else if (record.special && c == '\\') {
          // Validation error, but we're ignoring it.
          state = ParseState::RELATIVE_SLASH;
          break;
        }
        record.username.clear();
        record.password.clear();
        record.username.addAll(baseRecord.username);
        record.password.addAll(baseRecord.password);
        record.host = baseRecord.host.map([](jsg::UsvString& str) { return jsg::usv(str); });
        record.port = baseRecord.port;
        record.path = UrlRecordBuilder::copyPath(baseRecord.path);
        KJ_IF_MAYBE(q, baseRecord.query) {
          if (c != '?') {
            auto query = jsg::UsvStringBuilder(q->size());
            query.addAll(*q);
            record.query = kj::mv(query);
          }
        }
        if (c == '?') {
          KJ_IF_MAYBE(q, record.query) {
            q->clear();
          } else {
            record.query = jsg::UsvStringBuilder();
          }
          state = ParseState::QUERY;
          break;
        }
        if (c == '#') {
          record.fragment = jsg::UsvStringBuilder();
          state = ParseState::FRAGMENT;
          break;
        }
        if (it) {
          record.query = nullptr;
          shortenPath(record);
          state = ParseState::PATH;
          continue; // Continue without incrementing the iterator.
        }
        // Reached the end of the input!
        return UrlRecord(record);
      }
      case ParseState::RELATIVE_SLASH: {
        if (record.special && (c == '/' || c == '\\')) {
          // If c is '\', then it's a validation error, but we're ignoring those.
          state = ParseState::SPECIAL_AUTHORITY_IGNORE_SLASHES;
          break;
        } else if (c == '/') {
          state = ParseState::AUTHORITY;
          break;
        }
        record.username.clear();
        record.password.clear();
        record.host = nullptr;
        record.port = nullptr;
        KJ_IF_MAYBE(baseRecord, maybeBase) {
          record.username.addAll(baseRecord->username);
          record.password.addAll(baseRecord->password);
          record.host = baseRecord->host.map([](jsg::UsvString& str) { return jsg::usv(str); });
          record.port = baseRecord->port;
        }
        state = ParseState::PATH;
        continue; // Continue without incrementing the iterator.
      }
      case ParseState::SPECIAL_AUTHORITY_SLASHES: {
        state = ParseState::SPECIAL_AUTHORITY_IGNORE_SLASHES;
        if (c == '/' && nextCodepointIs(it, '/')) {
          ++it;
          break;
        }
        continue;
      }
      case ParseState::SPECIAL_AUTHORITY_IGNORE_SLASHES: {
        if (c != '/' && c != '\\') {
          state = ParseState::AUTHORITY;
          continue;  // Continue without incrementing the iterator.
        }
        break; // Increment the iterator.
      }
      case ParseState::AUTHORITY: {
        if (c == '@') {
          // Validation error, but we're ignoring it.
          if (atSignSeen) {
            jsg::UsvStringBuilder result(kj::max(buffer.capacity(), buffer.size() + 3));
            result.add('%', '4', '0');
            result.addAll(buffer.asPtr());
            buffer = kj::mv(result);
          }
          atSignSeen = true;
          auto temp = buffer.asPtr();
          auto iter = temp.begin();
          while (iter) {
            auto cp = *iter;
            if (cp == ':' && !passwordTokenSeen) {
              passwordTokenSeen = true;
              ++iter;
              continue;
            }
            if (passwordTokenSeen) {
              percentEncodeCodepoint(record.password, cp, &userInfoPercentEncodeSet);
            } else {
              percentEncodeCodepoint(record.username, cp, &userInfoPercentEncodeSet);
            }
            iter++;
          }
          buffer.clear();
          break;
        }
        if ((!it || c == '/' || c == '?' || c == '#') || (record.special && c == '\\')) {
          if (atSignSeen && buffer.empty()) {
            return nullptr;
          }
          it -= buffer.size() + 1;
          buffer.clear();
          state = ParseState::HOST;
          break;
        }
        // Reached the end of the input unexpectedly.
        if (!it) return nullptr;
        buffer.add(c);
        break;
      }
      case ParseState::HOST: {
        KJ_FALLTHROUGH;
      }
      case ParseState::HOSTNAME: {
        if (maybeStateOverride != nullptr && record.scheme == getCommonStrings().SCHEME_FILE) {
          state = ParseState::FILE_HOST;
          continue;  // Continue without incrementing the iterator.
        }
        if (c == ':' && !insideBrackets) {
          if (buffer.empty()) {
            // Validation error and failure.
            return nullptr;
          }
          KJ_IF_MAYBE(stateOverride, maybeStateOverride) {
            if (state == ParseState::HOSTNAME) {
              return UrlRecord(record);
            }
          }
          KJ_IF_MAYBE(host, hostParseBuffer(!record.special)) {
            record.host = kj::mv(*host);
          } else {
            return nullptr;
          }
          state = ParseState::PORT;
          break;
        } else if ((!it || c == '/' || c == '?' || c == '#') || (record.special && c == '\\')) {
          if (record.special && buffer.empty()) {
            return nullptr;
          }
          if (maybeStateOverride != nullptr &&
              buffer.empty() &&
              ((!record.username.empty() || !record.password.empty()) ||
               record.port != nullptr)) {
            return UrlRecord(record);
          }
          // There's a subtle detail here that appears to be omitted from the URL spec.
          // If parsing with the HOSTNAME state override, the scheme might not be set
          // and might be an empty string. In that case, we can't really determine if
          // the URL is special or not but the behavior of hostParseBuffer depends on
          // us knowing. The URLPattern spec assumes that no scheme specified == special,
          // but that's kind of stretching assumptions a bit. To handle both cases, if
          // state override is given and the scheme is an empty string, we assume that
          // isNotSpecial is false.
          auto isNotSpecial = (maybeStateOverride != nullptr && record.scheme.empty()) ?
              false : !record.special;
          KJ_IF_MAYBE(host, hostParseBuffer(isNotSpecial)) {
            record.host = kj::mv(*host);
          } else {
            return nullptr;
          }
          if (maybeStateOverride != nullptr) {
            return UrlRecord(record);
          }
          state = ParseState::PATH_START;
          continue; // Continue without incrementing the iterator.
        }
        if (c == '[') insideBrackets = true;
        if (c == ']') insideBrackets = false;
        buffer.add(c);
        break;
      }
      case ParseState::PORT: {
        if (isAsciiDigitCodepoint(c)) {
          buffer.add(c);
          break;
        } else if ((!it || c == '/' || c == '?' || c == '#') ||
                   (record.special && c == '\\') ||
                   maybeStateOverride != nullptr) {
          if (!buffer.empty()) {
            uint64_t port = 0;
            auto temp = buffer.asPtr();
            auto iter = temp.begin();
            while (iter && port <= 0xffff) {
              port = port * 10 + *iter - '0';
              ++iter;
            }
            buffer.clear();
            if (port > 0xffff) {
              KJ_IF_MAYBE(override, maybeStateOverride) {
                if (*override == ParseState::HOST) {
                  return UrlRecord(record);
                }
              }
              return nullptr;
            }
            if (defaultPortForScheme(record.scheme) == port) {
              record.port = nullptr;
            } else {
              record.port = port;
            }
          }
          if (maybeStateOverride != nullptr) {
            return UrlRecord(record);
          }
          state = ParseState::PATH_START;
          continue; // Continue without incrementing the iterator.
        }
        return nullptr;
      }
      case ParseState::FILE: {
        record.scheme = jsg::usv(getCommonStrings().SCHEME_FILE);
        record.special = true;  // File is special
        record.host = jsg::usv();
        if (c == '/' || c == '\\') {
          state = ParseState::FILE_SLASH;
          break;
        } else KJ_IF_MAYBE(baseRecord, maybeBase) {
          if (baseRecord->scheme == getCommonStrings().SCHEME_FILE) {
            record.host = baseRecord->host.map([](jsg::UsvString& str) { return jsg::usv(str); });
            record.path = UrlRecordBuilder::copyPath(baseRecord->path);
            jsg::UsvStringBuilder query;
            if (c != '?') {
              KJ_IF_MAYBE(q, baseRecord->query) {
                query.addAll(*q);
              }
            }
            record.query = kj::mv(query);

            if (c == '?') {
              state = ParseState::QUERY;
              break;
            }
            if (c == '#') {
              record.fragment = jsg::UsvStringBuilder();
              state = ParseState::FRAGMENT;
              break;
            }
            if (!it) {
              // Reached the end!
              return UrlRecord(record);
            }
            record.query = nullptr;
            auto slice = processed.slice(it, processed.end());
            if (!startsWithWindowsDriveLetter(slice, false)) {
              shortenPath(record);
            } else {
              record.path = kj::Vector<jsg::UsvString>();
            }
          }
        }
        state = ParseState::PATH;
        continue;  // Continue without incrementing the iterator.
      }
      case ParseState::FILE_SLASH: {
        if (c == '/' || c == '\\') {
          state = ParseState::FILE_HOST;
          break;
        }
        KJ_IF_MAYBE(baseRecord, maybeBase) {
          if (baseRecord->scheme == getCommonStrings().SCHEME_FILE) {
            record.host = baseRecord->host.map([](jsg::UsvString& str) { return jsg::usv(str); });
            auto slice = processed.slice(it, processed.end());
            if (!startsWithWindowsDriveLetter(slice, false)) {
              KJ_SWITCH_ONEOF(baseRecord->path) {
                KJ_CASE_ONEOF(string, jsg::UsvString) {
                  if (isWindowsDriveLetter(string, true)) {
                    appendToPath(jsg::usv(string));
                  }
                }
                KJ_CASE_ONEOF(strings, kj::Array<jsg::UsvString>) {
                  if (isWindowsDriveLetter(strings[0], true)) {
                    appendToPath(jsg::usv(strings[0]));
                  }
                }
              }
            }
          }
        }
        state = ParseState::PATH;
        continue;  // Continue without incrementing the iterator.
      }
      case ParseState::FILE_HOST: {
        if (!it || c == '/' || c == '\\' || c == '?' || c == '#') {
          if (maybeStateOverride == nullptr &&
              isWindowsDriveLetterFileQuirk(buffer.storage())) {
            state = ParseState::PATH;
            continue;
          }
          if (buffer.empty()) {
            record.host = jsg::usv();
            if (maybeStateOverride != nullptr) {
              return UrlRecord(record);
            }
            state = ParseState::PATH_START;
            continue;
          }
          KJ_IF_MAYBE(host, hostParseBuffer(!record.special)) {
            record.host = (*host == getCommonStrings().LOCALHOST) ? jsg::usv() : kj::mv(*host);
          } else {
            return nullptr;
          }
          if (maybeStateOverride != nullptr) {
            return UrlRecord(record);
          }
          state = ParseState::PATH_START;
          continue; // Continue without incrementing the iterator
        }
        buffer.add(c);
        break;
      }
      case ParseState::PATH_START: {
        if (record.special) {
          state = ParseState::PATH;
          if (c != '/' && c != '\\') {
            continue; // Continue without incrementing iterator
          }
          break;  // Increment the iterator.
        }
        if (maybeStateOverride == nullptr && c == '?') {
          record.query = jsg::UsvStringBuilder();
          state = ParseState::QUERY;
          break;
        }
        if (maybeStateOverride == nullptr && c == '#') {
          record.fragment = jsg::UsvStringBuilder();
          state = ParseState::FRAGMENT;
          break;
        }
        if (it) {
          state = ParseState::PATH;
          if (c != '/') {
            continue;  // Continue without incrementing iterator
          }
          break;  // Increment the iterator and continue.
        }
        KJ_IF_MAYBE(o, maybeStateOverride) {
          if (record.host == nullptr) {
            appendToPath();
          }
        }
        if (!it) {
          // Reached the end of input! Nothing left to do.
          return UrlRecord(record);
        }
        break;
      }
      case ParseState::PATH: {
        auto specialBackSlash = record.special && c == '\\';
        // If we're at the end of input and c == /, or
        // If scheme is special and c == \, or
        // state override is not given and c is either ? or #.
        if (!it || c == '/' || specialBackSlash ||
            (maybeStateOverride == nullptr && (c == '?' || c == '#'))) {
          // if special and c == \, validation error
          KJ_DEFER(buffer.clear());
          auto temp = buffer.asPtr();
          auto isDoubleDot = isDoubleDotSegment(temp);
          auto isSingleDot = isSingleDotSegment(temp);
          if (isDoubleDot) {
            shortenPath(record);
            if (c != '/' && !specialBackSlash) {
              appendToPath();
            }
          } else if (isSingleDot && c != '/' && !specialBackSlash) {
            appendToPath();
          } else if (!isSingleDot) {
            if (record.scheme == getCommonStrings().SCHEME_FILE &&
                pathIsEmpty(record) &&
                isWindowsDriveLetter(temp, false)) {
              temp.storage()[1] = ':';
            }
            appendToPath(jsg::usv(temp));
          }
          if (c == '?') {
            record.query = jsg::UsvStringBuilder();
            state = ParseState::QUERY;
            break;
          }
          if (c == '#') {
            record.fragment = jsg::UsvStringBuilder();
            state = ParseState::FRAGMENT;
          }
          if (!it) {
            // We're at the end of input! Nothing left to do!
            return UrlRecord(record);
          }
          break;
        }
        if (!it) {
          // We're at the end of input! Nothing left to do!
          return UrlRecord(record);
        }
        // if c is not a URL codepoint, validation error.
        // if c is % and not followed by hex digits, validation error.
        percentEncodeCodepoint(buffer, c, &pathPercentEncodeSet);
        break;
      }
      case ParseState::OPAQUE_PATH: {
        if (!it) {
          // We hit the end! Nothing left to do.
          return UrlRecord(record);
        }
        if (c == '?') {
          record.query = jsg::UsvStringBuilder();
          state = ParseState::QUERY;
          break;
        }
        if (c == '#') {
          record.fragment = jsg::UsvStringBuilder();
          state = ParseState::FRAGMENT;
          break;
        }
        // The record.path must be a UsvStringBuilder here.
        auto& builder = KJ_ASSERT_NONNULL(record.path.tryGet<jsg::UsvStringBuilder>());
        percentEncodeCodepoint(builder, c, &controlPercentEncodeSet);
        break;
      }
      case ParseState::QUERY: {
        if ((maybeStateOverride == nullptr && c == '#') || !it) {
          // Either state override is not provided and we hit a hash character
          // or we hit the end of the input string...

          // Process the current buffer and append it to record.query
          if (!buffer.empty()) {
            auto percentEncodeSet = record.special ?
                &specialQueryPercentEncodeSet :
                &queryPercentEncodeSet;
            auto temp = buffer.asPtr();
            auto iter = temp.begin();
            auto& builder = KJ_ASSERT_NONNULL(record.query);
            while (iter) {
              auto ic = *iter;
              percentEncodeCodepoint(builder, ic, percentEncodeSet);
              ++iter;
            }
            buffer.clear();
          }
          if (!it) {
            // Reached the end! Nothing left to do!
            return UrlRecord(record);
          }
          if (c == '#') {
            record.fragment = jsg::UsvStringBuilder();
            state = ParseState::FRAGMENT;
          }
          break;
        }
        buffer.add(c);
        break;
      }
      case ParseState::FRAGMENT: {
        if (!it) {
          // Reached the end! Nothing else to do!
          return UrlRecord(record);
        }
        auto& builder = KJ_ASSERT_NONNULL(record.fragment);
        percentEncodeCodepoint(builder, c, &fragmentPercentEncodeSet);
        break;
      }
    }
    KJ_ASSERT(bool(it)); // We're not at the end of input.
    ++it;
  }

  KJ_UNREACHABLE;
}

URL::URL(jsg::UsvStringPtr url, jsg::Optional<jsg::UsvStringPtr> base)
    : inner(handleConstructorParse(url, kj::mv(base))) {}

URL::~URL() noexcept(false) {
  KJ_IF_MAYBE(searchParams, maybeSearchParams) {
    (*searchParams)->maybeUrl = nullptr;
  }
}

bool URL::canParse(jsg::UsvString url, jsg::Optional<jsg::UsvString> maybeBase) {
  KJ_IF_MAYBE(base, maybeBase) {
    KJ_IF_MAYBE(parsedBase, URL::parse(*base)) {
      KJ_IF_MAYBE(parsed, URL::parse(url, *parsedBase)) {
        return true;
      }
    }
  } else {
    KJ_IF_MAYBE(parsed, URL::parse(url)) {
      return true;
    }
  }
  return false;
}

jsg::UsvString URL::getOrigin() {
  KJ_SWITCH_ONEOF(inner.getOrigin()) {
    KJ_CASE_ONEOF(opaque, OpaqueOrigin) {
      return jsg::usv(getCommonStrings().NULL_);
    }
    KJ_CASE_ONEOF(tuple, TupleOrigin) {
      // The additional 9 codepoints here is for the :// and possible port prefix + port.
      jsg::UsvStringBuilder builder(tuple.scheme.size() + tuple.host.size() + 9);
      builder.addAll(tuple.scheme);
      builder.add(':', '/', '/');
      builder.addAll(tuple.host);
      KJ_IF_MAYBE(port, tuple.port) {
        builder.add(':');
        builder.addAll(kj::toCharSequence(*port));
      }
      return builder.finish();
    }
  }
  KJ_UNREACHABLE
}

jsg::UsvString URL::getHref() { return inner.getHref(); }

void URL::setHref(jsg::UsvString value) {
  inner = JSG_REQUIRE_NONNULL(parse(value), TypeError, "Invalid URL string.");
  KJ_IF_MAYBE(searchParams, maybeSearchParams) {
    (*searchParams)->reset(toMaybePtr(inner.query));
  }
}

jsg::UsvString URL::getProtocol() {
  jsg::UsvStringBuilder builder(inner.scheme.size() + 1);
  builder.addAll(inner.scheme);
  builder.add(':');
  return builder.finish();
}

void URL::setProtocol(jsg::UsvString value) {
  if (value.empty()) return;
  jsg::UsvStringBuilder builder(value.size() + 1);
  builder.addAll(value);
  builder.add(':');
  KJ_IF_MAYBE(record, parse(builder.finish(), nullptr, inner, ParseState::SCHEME_START)) {
    inner = kj::mv(*record);
  }
}

jsg::UsvStringPtr URL::getUsername() {
  return inner.username;
}

void URL::setUsername(jsg::UsvString value) {
  if (cannotHaveUsernamePasswordOrPort(inner)) return;
  inner.setUsername(value);
}

jsg::UsvStringPtr URL::getPassword() {
  return inner.password;
}

void URL::setPassword(jsg::UsvString value) {
  if (cannotHaveUsernamePasswordOrPort(inner)) return;
  inner.setPassword(value);
}

jsg::UsvString URL::getHost() {
  KJ_IF_MAYBE(host, inner.host) {
    KJ_IF_MAYBE(port, inner.port) {
      // The additional 6 here is for the port prefix and possible port
      jsg::UsvStringBuilder builder(host->size() + 6);
      builder.addAll(*host);
      builder.add(':');
      builder.addAll(kj::toCharSequence(*port));
      return builder.finish();
    }
    return jsg::usv(*host);
  }
  return jsg::usv();
}

void URL::setHost(jsg::UsvString value) {
  if (inner.path.is<jsg::UsvString>()) return;
  KJ_IF_MAYBE(record, parse(value, nullptr, inner, ParseState::HOST)) {
    inner = kj::mv(*record);
  }
}

jsg::UsvStringPtr URL::getHostname() {
  KJ_IF_MAYBE(host, inner.host) {
    return *host;
  }
  return getCommonStrings().EMPTY_STRING;
}

void URL::setHostname(jsg::UsvString value) {
  if (inner.path.is<jsg::UsvString>()) return;
  KJ_IF_MAYBE(record, parse(value, nullptr, inner, ParseState::HOSTNAME)) {
    inner = kj::mv(*record);
  }
}

jsg::UsvString URL::getPort() {
  KJ_IF_MAYBE(port, inner.port) {
    return jsg::usv(kj::toCharSequence(*port));
  }
  return jsg::usv();
}

void URL::setPort(jsg::UsvString port) {
  if (cannotHaveUsernamePasswordOrPort(inner)) return;
  if (port == getCommonStrings().EMPTY_STRING) {
    inner.port = nullptr;
    return;
  }
  KJ_IF_MAYBE(record, parse(port, nullptr, inner, ParseState::PORT)) {
    inner = kj::mv(*record);
  }
}

jsg::UsvString URL::getPathname() { return inner.getPathname(); }

void URL::setPathname(jsg::UsvString value) {
  if (inner.path.is<jsg::UsvString>()) return;
  inner.path = kj::Array<jsg::UsvString>();
  KJ_IF_MAYBE(record, parse(value, nullptr, inner, ParseState::PATH_START)) {
    inner = kj::mv(*record);
  }
}

jsg::UsvString URL::getSearch() {
  KJ_IF_MAYBE(query, inner.query) {
    if (!query->empty()) {
      jsg::UsvStringBuilder builder(query->size() + 1);
      builder.add('?');
      builder.addAll(*query);
      return builder.finish();
    }
  }
  return jsg::usv();
}

void URL::setSearch(jsg::UsvString query) {
  if (query == getCommonStrings().EMPTY_STRING) {
    inner.query = nullptr;
    KJ_IF_MAYBE(searchParams, maybeSearchParams) {
      (*searchParams)->reset();
    }
    return;
  }
  auto sliced = query.first() == '?' ? query.slice(1) : query.asPtr();
  inner.query = jsg::usv();
  KJ_IF_MAYBE(record, parse(sliced, nullptr, inner, ParseState::QUERY)) {
    inner = kj::mv(*record);
    KJ_IF_MAYBE(searchParams, maybeSearchParams) {
      (*searchParams)->reset(toMaybePtr(inner.query));
    }
  }
}

jsg::UsvString URL::getHash() {
  KJ_IF_MAYBE(fragment, inner.fragment) {
    if (!fragment->empty()) {
      jsg::UsvStringBuilder builder(fragment->size() + 1);
      builder.add('#');
      builder.addAll(*fragment);
      return builder.finish();
    }
  }
  return jsg::usv();
}

void URL::setHash(jsg::UsvString hash) {
  if (hash == getCommonStrings().EMPTY_STRING) {
    inner.fragment = nullptr;
    return;
  }
  auto sliced = hash.first() == '#' ? hash.slice(1) : hash.asPtr();
  inner.fragment = jsg::usv();
  KJ_IF_MAYBE(record, parse(sliced, nullptr, inner, ParseState::FRAGMENT)) {
    inner = kj::mv(*record);
  }
}

bool URL::isSpecialScheme(jsg::UsvStringPtr scheme) {
  return url::isSpecialScheme(scheme);
}

kj::Maybe<uint16_t> URL::defaultPortForScheme(jsg::UsvStringPtr scheme) {
  return url::defaultPortForScheme(scheme);
}

void URLSearchParams::init(Initializer init) {
  list.clear();
  KJ_SWITCH_ONEOF(init) {
    KJ_CASE_ONEOF(pairs, UsvStringPairs) {
      list = KJ_MAP(pair, pairs) {
        JSG_REQUIRE(pair.size() == 2, TypeError,
              "Sequence initializer must only contain pair elements.");
        return Entry(kj::mv(pair[0]), kj::mv(pair[1]));
      };
    }
    KJ_CASE_ONEOF(dict, jsg::Dict<jsg::UsvString, jsg::UsvString>) {
      for (auto& pair : dict.fields) {
        set(kj::mv(pair.name), kj::mv(pair.value));
      }
    }
    KJ_CASE_ONEOF(ptr, jsg::UsvString) {
      if (ptr.empty()) return;
      parse(ptr.first() == '?' ? ptr.slice(1) : ptr);
    }
  }
}

void URLSearchParams::parse(jsg::UsvStringPtr input) {
  list.clear();
  if (input.empty()) return;
  const auto process = [](jsg::UsvStringPtr input) {
    jsg::UsvStringBuilder builder(input.size());
    auto it = input.begin();
    while (it) {
      auto c = *it;
      builder.add(c == '+' ? ' ' : c);
      ++it;
    }
    auto res = percentDecode(builder.finish());
    return kj::mv(res);
  };

  auto it = input.begin();
  auto start = it;
  while (it) {
    auto end = seek(it, '&');
    if (end != start) {
      auto segment = input.slice(start, end);
      auto iter = segment.begin();
      auto nameStart = iter;
      auto nameEnd = seek(iter, '=');
      auto name = segment.slice(nameStart, nameEnd);
      auto value = iter != segment.end() ?
          segment.slice(++nameEnd) :
          getCommonStrings().EMPTY_STRING.asPtr();
      list.add(Entry(process(name), process(value)));
    }
    if (!it) {
      // Reached the end.
      break;
    }
    start = ++it;
  }
}

URLSearchParams::URLSearchParams(Initializer initializer) {
  init(kj::mv(initializer));
}

URLSearchParams::URLSearchParams(kj::Maybe<jsg::UsvString>& maybeQuery, URL& url) : maybeUrl(url) {
  KJ_IF_MAYBE(query, maybeQuery) { parse(*query); }
}

void URLSearchParams::update() {
  KJ_IF_MAYBE(url, maybeUrl) {
    auto serialized = toString();
    if (serialized == getCommonStrings().EMPTY_STRING) {
      url->inner.query = nullptr;
    } else {
      url->inner.query = kj::mv(serialized);
    }
  }
}

void URLSearchParams::reset(kj::Maybe<jsg::UsvStringPtr> value) {
  KJ_IF_MAYBE(val, value) {
    parse(*val);
  } else {
    parse(getCommonStrings().EMPTY_STRING);
  }
}

void URLSearchParams::append(jsg::UsvString name, jsg::UsvString value) {
  list.add(Entry(kj::mv(name), kj::mv(value)));
  update();
}

void URLSearchParams::delete_(jsg::UsvString name) {
  auto pivot = std::remove_if(list.begin(), list.end(),
                              [&name](auto& kv) { return kv.name == name; });
  list.truncate(pivot - list.begin());
  update();
}

kj::Maybe<jsg::UsvStringPtr> URLSearchParams::get(jsg::UsvString name) {
  for (auto& entry : list) {
    if (entry.name == name) return entry.value.asPtr();
  }
  return nullptr;
}

kj::Array<jsg::UsvStringPtr> URLSearchParams::getAll(jsg::UsvString name) {
  kj::Vector<jsg::UsvStringPtr> result(list.size());
  for (auto& entry : list) {
    if (entry.name == name) result.add(entry.value);
  }
  return result.releaseAsArray();
}

bool URLSearchParams::has(jsg::UsvString name) {
  for (auto& entry : list) {
    if (entry.name == name) return true;
  }
  return false;
}

void URLSearchParams::set(jsg::UsvString name, jsg::UsvString value) {
  // Set the first element named `name` to `value`, then remove all the rest matching that name.
  const auto predicate = [&name](auto& kv) { return kv.name == name; };
  auto firstFound = std::find_if(list.begin(), list.end(), predicate);
  if (firstFound != list.end()) {
    firstFound->value = kj::mv(value);
    auto pivot = std::remove_if(++firstFound, list.end(), predicate);
    list.truncate(pivot - list.begin());
  } else {
    list.add(Entry(kj::mv(name), kj::mv(value)));
  }
  update();
}

void URLSearchParams::sort() {
  // The sort operation here is fairly expensive. The storage for a UsvString is by
  // codepoint (uint32_t). The URLSearchParams specification, however, requires that
  // sorting be based on uint16_t code unit order. This means that to perform the
  // comparison correctly, we need to the the utf16 version of the data. Because
  // that is not a cheap operation, we don't want to regenerate the utf16 encoding
  // on every comparison operation. Instead, we memoize the entries in a hash map.
  // It's still fairly expensive, but much less so.
  //
  // Why not simply store the data in utf16 format to begin with? Well, most of the
  // operations on UsvString operate on the codepoint level. The sort() operation
  // here is an exception to the rule. We're optimizing for performance everywhere
  // else and taking a moderate performance hit here.
  kj::HashMap<const Entry&, kj::Array<const uint16_t>> memo;
  const auto upsert = [&memo](const Entry& entry) {
    return memo.findOrCreate(entry, [&entry]() {
      return decltype(memo)::Entry {
        .key = entry,
        .value = entry.name.toUtf16(),
      };
    }).asPtr();
  };

  std::stable_sort(list.begin(), list.end(), [&upsert](const Entry& a, const Entry& b) {
    if (a.name == b.name) return false;
    if (a.name.empty() && !b.name.empty()) return true;

    auto ptr1 = upsert(a);
    auto ptr2 = upsert(b);

    return u_strCompare(
        reinterpret_cast<const UChar*>(ptr1.begin()),
        ptr1.size(),
        reinterpret_cast<const UChar*>(ptr2.begin()),
        ptr2.size(),
        false
    ) < 0;
  });
  update();
}

jsg::Ref<URLSearchParams::EntryIterator> URLSearchParams::entries(
    jsg::Lock&,
    CompatibilityFlags::Reader featureFlags) {
  return jsg::alloc<URLSearchParams::EntryIterator>(IteratorState { JSG_THIS });
}

jsg::Ref<URLSearchParams::KeyIterator> URLSearchParams::keys(
    jsg::Lock&,
    CompatibilityFlags::Reader featureFlags) {
  return jsg::alloc<URLSearchParams::KeyIterator>(IteratorState { JSG_THIS });
}

jsg::Ref<URLSearchParams::ValueIterator> URLSearchParams::values(
    jsg::Lock&,
    CompatibilityFlags::Reader featureFlags) {
  return jsg::alloc<URLSearchParams::ValueIterator>(IteratorState { JSG_THIS });
}

void URLSearchParams::forEach(
      jsg::V8Ref<v8::Function> callback,
      jsg::Optional<jsg::Value> thisArg,
      v8::Isolate* isolate) {
  auto cb = callback.getHandle(isolate);
  auto this_ = thisArg.map([&isolate](jsg::Value& v) { return v.getHandle(isolate); })
      .orDefault(v8::Undefined(isolate));
  auto query = KJ_ASSERT_NONNULL(JSG_THIS.tryGetHandle(isolate));
  // On each iteration of the for loop, a JavaScript callback is invokved. If a new
  // item is appended to the URLSearchParams within that function, the loop must pick
  // it up. Using the classic for (;;) syntax here allows for that. However, this does
  // mean that it's possible for a user to trigger an infinite loop here if new items
  // are added to the search params unconditionally on each iteration.
  for (int i = 0; i < list.size(); i++) {
    auto& entry = list[i];
    v8::Local<v8::Value> args[3] = {
      jsg::v8Str(isolate, entry.value),
      jsg::v8Str(isolate, entry.name),
      query,
    };
    jsg::check(cb->Call(isolate->GetCurrentContext(), this_, 3, args));
  }
}

jsg::UsvString URLSearchParams::toString() {
  // The reserve size is fairly arbitrary, we just want to avoid too many allocations.
  jsg::UsvStringBuilder builder(255);
  for (auto& entry : list) {
    // Best case here is that nothing gets percent encoded.
    if (!builder.empty()) builder.add('&');
    auto it = entry.name.begin();
    while (it) {
      auto c = *it;
      percentEncodeCodepoint(builder, c, &urlEncodedPercentEncodeSet, true);
      ++it;
    }
    builder.add('=');
    it = entry.value.begin();
    while (it) {
      auto c = *it;
      percentEncodeCodepoint(builder, c, &urlEncodedPercentEncodeSet, true);
      ++it;
    }
  }
  return builder.finish();
}

}  // workerd::api::url

