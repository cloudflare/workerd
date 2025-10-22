#include "headers.h"

#include "util.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/util/strings.h>

#ifdef _MSC_VER
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#endif

namespace workerd::api {

namespace {
// If any more headers are added to the CommonHeaderName enum later, we should be careful about
// introducing them into serialization. We need to roll out a change that recognizes the new IDs
// before rolling out a change that sends them. MAX_COMMON_HEADER_ID is the max value we're willing
// to send.
static constexpr size_t MAX_COMMON_HEADER_ID =
    static_cast<size_t>(capnp::CommonHeaderName::WWW_AUTHENTICATE);

#define COMMON_HEADERS(V)                                                                          \
  V("accept-charset")                                                                              \
  V("accept-encoding")                                                                             \
  V("accept-language")                                                                             \
  V("accept-ranges")                                                                               \
  V("accept")                                                                                      \
  V("access-control-allow-origin")                                                                 \
  V("age")                                                                                         \
  V("allow")                                                                                       \
  V("authorization")                                                                               \
  V("cache-control")                                                                               \
  V("content-disposition")                                                                         \
  V("content-encoding")                                                                            \
  V("content-language")                                                                            \
  V("content-length")                                                                              \
  V("content-location")                                                                            \
  V("content-range")                                                                               \
  V("content-type")                                                                                \
  V("cookie")                                                                                      \
  V("date")                                                                                        \
  V("etag")                                                                                        \
  V("expect")                                                                                      \
  V("expires")                                                                                     \
  V("from")                                                                                        \
  V("host")                                                                                        \
  V("if-match")                                                                                    \
  V("if-modified-since")                                                                           \
  V("if-none-match")                                                                               \
  V("if-range")                                                                                    \
  V("if-unmodified-since")                                                                         \
  V("last-modified")                                                                               \
  V("link")                                                                                        \
  V("location")                                                                                    \
  V("max-forwards")                                                                                \
  V("proxy-authenticate")                                                                          \
  V("proxy-authorization")                                                                         \
  V("range")                                                                                       \
  V("referer")                                                                                     \
  V("refresh")                                                                                     \
  V("retry-after")                                                                                 \
  V("server")                                                                                      \
  V("set-cookie")                                                                                  \
  V("strict-transport-security")                                                                   \
  V("transfer-encoding")                                                                           \
  V("user-agent")                                                                                  \
  V("vary")                                                                                        \
  V("via")                                                                                         \
  V("www-authenticate")

// Constexpr array of lowercase common header names (must match CommonHeaderName enum order
// and must be kept in sync with the ordinal values defined in http-over-capnp.capnp). Since
// it is extremely unlikely that those will change often, we hardcode them here for runtime
// efficiency.
#define V(Name) Name,
static constexpr const char* COMMON_HEADER_NAMES[] = {nullptr,  // 0: invalid
  COMMON_HEADERS(V)};
#undef V

constexpr size_t constexprStrlen(const char* str) {
  return *str ? 1 + constexprStrlen(str + 1) : 0;
}

// Helper to avoid recalculating lengths of common headers at runtime repeatedly
static constexpr size_t COMMON_HEADER_NAME_LENGTHS[] = {0,  // 0: invalid (nullptr)
#define V(n) constexprStrlen(n),
  COMMON_HEADERS(V)
#undef V
};

inline constexpr kj::StringPtr getCommonHeaderName(uint id) {
  KJ_ASSERT(id > 0 && id <= MAX_COMMON_HEADER_ID, "Invalid common header ID");
  kj::StringPtr name = COMMON_HEADER_NAMES[id];
  KJ_DASSERT(name != nullptr);
  return name;
}

// Case-insensitive lookup of common header ID. This avoids allocating a lowercase copy
// when the header is common. Returns kj::none if not a common header.
// TODO(perf): It's possible to optimize this further with a good hash function but
// for now a linear scan is sufficient.
constexpr kj::Maybe<uint> getCommonHeaderId(kj::StringPtr name) {
  size_t len = name.size();
  if (len == 0) return kj::none;
  for (uint i = 1; i <= MAX_COMMON_HEADER_ID; ++i) {
    KJ_DASSERT(COMMON_HEADER_NAMES[i] != nullptr);
    // If the lengths don't match or the first character doesn't match, skip full comparison
    if (len != COMMON_HEADER_NAME_LENGTHS[i]) continue;
    if (strncasecmp(name.begin(), COMMON_HEADER_NAMES[i], len) == 0) {
      return i;
    }
  }
  return kj::none;
}

static_assert(std::size(COMMON_HEADER_NAMES) == (MAX_COMMON_HEADER_ID + 1));

void warnIfBadHeaderString(const jsg::ByteString& byteString) {
  if (IoContext::hasCurrent()) {
    auto& context = IoContext::current();
    if (context.isInspectorEnabled()) {
      if (byteString.warning == jsg::ByteString::Warning::CONTAINS_EXTENDED_ASCII) {
        // We're in a bit of a pickle: the script author is using our API correctly, but we're doing
        // the wrong thing by UTF-8-encoding their bytes. To help the author understand the issue,
        // we can show the string that they would be putting in the header if we implemented the
        // spec correctly, and the string that is actually going get serialized onto the wire.
        auto rawHex = kj::strArray(KJ_MAP(b, fastEncodeUtf16(byteString.asArray())) {
          KJ_ASSERT(b < 256);  // Guaranteed by StringWrapper having set CONTAINS_EXTENDED_ASCII.
          return kj::str("\\x", kj::hex(kj::byte(b)));
        }, "");
        auto utf8Hex =
            kj::strArray(
                KJ_MAP(b, byteString) { return kj::str("\\x", kj::hex(kj::byte(b))); }, "");

        context.logWarning(kj::str("Problematic header name or value: \"", byteString,
            "\" (raw bytes: \"", rawHex,
            "\"). "
            "This string contains 8-bit characters in the range 0x80 - 0xFF. As a quirk to support "
            "Unicode, we encode header strings in UTF-8, meaning the actual header name/value on "
            "the wire will be \"",
            utf8Hex,
            "\". Consider encoding this string in ASCII for "
            "compatibility with browser implementations of the Fetch specifications."));
      } else if (byteString.warning == jsg::ByteString::Warning::CONTAINS_UNICODE) {
        context.logWarning(kj::str("Invalid header name or value: \"", byteString,
            "\". Per the Fetch specification, the "
            "Headers class may only accept header names and values which contain 8-bit characters. "
            "That is, they must not contain any Unicode code points greater than 0xFF. As a quirk, "
            "we are encoding this string in UTF-8 in the header, but in a browser this would "
            "result in a TypeError exception. Consider encoding this string in ASCII for "
            "compatibility with browser implementations of the Fetch specification."));
      }
    }
  }
}

// TODO(perf): This can be optimized further using a lookup table.
constexpr bool isHttpWhitespace(char c) {
  return c == '\t' || c == '\r' || c == '\n' || c == ' ';
}

// TODO(perf): This can be optimized further using a lookup table.
constexpr bool isValidHeaderValueChar(char c) {
  return c != '\0' && c != '\r' && c != '\n';
}

// Left- and right-trim HTTP whitespace from `value`.
jsg::ByteString normalizeHeaderValue(jsg::Lock& js, jsg::ByteString value) {
  warnIfBadHeaderString(value);
  // Fast path: if empty, return as-is
  if (value.size() == 0) return kj::mv(value);

  char* begin = value.begin();
  char* end = value.end();

  while (begin < end && isHttpWhitespace(*begin)) ++begin;
  while (begin < end && isHttpWhitespace(*(end - 1))) --end;

  size_t newSize = end - begin;
  if (newSize == value.size()) return kj::mv(value);

  return jsg::ByteString(kj::str(kj::ArrayPtr(begin, newSize)));
}

// Fast lookup table for valid HTTP token characters (RFC 2616).
// Valid token chars are: !#$%&'*+-.0-9A-Z^_`a-z|~
// (i.e., any CHAR except CTLs or separators)
static constexpr uint8_t HTTP_TOKEN_CHAR_TABLE[] = {
  // Control characters 0x00-0x1F and 0x7F are invalid
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x00-0x07
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x08-0x0F
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x10-0x17
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x18-0x1F
  0, 1, 0, 1, 1, 1, 1, 1,  // 0x20-0x27: SP!"#$%&'
  0, 0, 1, 1, 0, 1, 1, 0,  // 0x28-0x2F: ()*+,-./
  1, 1, 1, 1, 1, 1, 1, 1,  // 0x30-0x37: 01234567
  1, 1, 0, 0, 0, 0, 0, 0,  // 0x38-0x3F: 89:;<=>?
  0, 1, 1, 1, 1, 1, 1, 1,  // 0x40-0x47: @ABCDEFG
  1, 1, 1, 1, 1, 1, 1, 1,  // 0x48-0x4F: HIJKLMNO
  1, 1, 1, 1, 1, 1, 1, 1,  // 0x50-0x57: PQRSTUVW
  1, 1, 1, 0, 0, 0, 1, 1,  // 0x58-0x5F: XYZ[\]^_
  1, 1, 1, 1, 1, 1, 1, 1,  // 0x60-0x67: `abcdefg
  1, 1, 1, 1, 1, 1, 1, 1,  // 0x68-0x6F: hijklmno
  1, 1, 1, 1, 1, 1, 1, 1,  // 0x70-0x77: pqrstuvw
  1, 1, 1, 0, 1, 0, 1, 0,  // 0x78-0x7F: xyz{|}~DEL
  // Extended ASCII 0x80-0xFF are all invalid per RFC 2616
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x80-0x87
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x88-0x8F
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x90-0x97
  0, 0, 0, 0, 0, 0, 0, 0,  // 0x98-0x9F
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xA0-0xA7
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xA8-0xAF
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xB0-0xB7
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xB8-0xBF
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xC0-0xC7
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xC8-0xCF
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xD0-0xD7
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xD8-0xDF
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xE0-0xE7
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xE8-0xEF
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xF0-0xF7
  0, 0, 0, 0, 0, 0, 0, 0,  // 0xF8-0xFF
};

inline void requireValidHeaderName(const jsg::ByteString& name) {
  // TODO(cleanup): Code duplication with kj/compat/http.c++
  warnIfBadHeaderString(name);

  for (char c: name) {
    JSG_REQUIRE(HTTP_TOKEN_CHAR_TABLE[static_cast<uint8_t>(c)], TypeError, "Invalid header name.");
  }
}

inline void requireValidHeaderValue(kj::StringPtr value) {
  for (char c: value) {
    JSG_REQUIRE(isValidHeaderValueChar(c), TypeError, "Invalid header value.");
  }
}
}  // namespace

Headers::UncommonHeaderKey::UncommonHeaderKey(kj::String name)
    : name(kj::mv(name)),
      hash(kj::hashCode(this->name)) {}

Headers::UncommonHeaderKey::UncommonHeaderKey(kj::StringPtr name)
    : name(kj::str(name)),
      hash(kj::hashCode(this->name)) {}

bool Headers::UncommonHeaderKey::operator==(const UncommonHeaderKey& other) const {
  // The same hash code is a necessary but not sufficient condition for equality.
  return hash == other.hash && name == other.name;
}

bool Headers::UncommonHeaderKey::operator==(kj::StringPtr otherName) const {
  if (name.size() != otherName.size()) return false;
  return strncasecmp(name.begin(), otherName.begin(), name.size()) == 0;
}

Headers::HeaderKey Headers::getHeaderKeyFor(kj::StringPtr name) {
  KJ_IF_SOME(commonId, getCommonHeaderId(name)) {
    return commonId;
  }

  // Not a common header, so allocate lowercase copy for uncommon header
  return UncommonHeaderKey(toLower(name));
}

Headers::HeaderKey Headers::cloneHeaderKey(const HeaderKey& key) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(commonId, uint) {
      return commonId;
    }
    KJ_CASE_ONEOF(uncommonKey, UncommonHeaderKey) {
      return uncommonKey.clone();
    }
  }
  KJ_UNREACHABLE;
}

bool Headers::isSetCookie(const HeaderKey& key) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(commonId, uint) {
      return commonId == static_cast<uint>(capnp::CommonHeaderName::SET_COOKIE);
    }
    KJ_CASE_ONEOF(uncommonKey, UncommonHeaderKey) {
      // This case really shouldn't happen since "set-cookie" is a common header,
      // but just in case...
      return uncommonKey == "set-cookie";
    }
  }
  KJ_UNREACHABLE;
}

bool Headers::headerKeyEquals(const HeaderKey& a, const HeaderKey& b) {
  KJ_SWITCH_ONEOF(a) {
    KJ_CASE_ONEOF(aCommonId, uint) {
      KJ_IF_SOME(bCommonId, b.tryGet<uint>()) {
        return aCommonId == bCommonId;
      }
      return false;
    }
    KJ_CASE_ONEOF(aUncommonKey, UncommonHeaderKey) {
      KJ_IF_SOME(bUncommonKey, b.tryGet<UncommonHeaderKey>()) {
        return aUncommonKey == bUncommonKey;
      }
      return false;
    }
  }
  KJ_UNREACHABLE;
}

Headers::Header::Header(jsg::ByteString name, kj::Vector<jsg::ByteString> values)
    : key(getHeaderKeyFor(name)),
      values(kj::mv(values)) {
  if (getKeyName() != name) {
    // The casing of the provided name does not match the lower-cased version
    // held by the key, so we need to preserve the original casing for display
    // purposes.
    this->name = kj::mv(name);
  }
}

Headers::Header::Header(jsg::ByteString name, jsg::ByteString value)
    : key(getHeaderKeyFor(name)),
      values(1) {
  values.add(kj::mv(value));
  if (getKeyName() != name) {
    // The casing of the provided name does not match the lower-cased version
    // held by the key, so we need to preserve the original casing for display
    // purposes.
    this->name = kj::mv(name);
  }
}

Headers::Header::Header(
    HeaderKey key, kj::Maybe<jsg::ByteString> name, kj::Vector<jsg::ByteString> values)
    : key(kj::mv(key)),
      name(kj::mv(name)),
      values(kj::mv(values)) {}

Headers::Header::Header(HeaderKey key, jsg::ByteString name, jsg::ByteString value)
    : key(kj::mv(key)),
      values(1) {
  values.add(kj::mv(value));
  if (getKeyName() != name) {
    this->name = kj::mv(name);
  }
}

kj::StringPtr Headers::Header::Header::getKeyName() const {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(commonId, uint) {
      return COMMON_HEADER_NAMES[commonId];
    }
    KJ_CASE_ONEOF(uncommonKey, UncommonHeaderKey) {
      return uncommonKey.getName();
    }
  }
  KJ_UNREACHABLE;
}

kj::StringPtr Headers::Header::getHeaderName() const {
  KJ_IF_SOME(preservedName, name) {
    return preservedName;
  }
  return getKeyName();
}

Headers::Header Headers::Header::clone() const {
  return Header(cloneHeaderKey(key),
      name.map([](const kj::String& n) { return jsg::ByteString(kj::str(n)); }),
      KJ_MAP(value, values) { return jsg::ByteString(kj::str(value)); });
}

bool Headers::HeaderCallbacks::matches(Header& header, const HeaderKey& other) {
  return headerKeyEquals(header.key, other);
}

bool Headers::HeaderCallbacks::matches(Header& header, kj::StringPtr otherName) {
  return matches(header, getHeaderKeyFor(otherName));
}

bool Headers::HeaderCallbacks::matches(Header& header, capnp::CommonHeaderName commondId) {
  KJ_IF_SOME(headerCommonId, header.key.tryGet<uint>()) {
    return headerCommonId == static_cast<uint>(commondId);
  }
  return false;
}

kj::uint Headers::HeaderCallbacks::hashCode(const HeaderKey& key) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(commonId, uint) {
      return kj::hashCode(commonId);
    }
    KJ_CASE_ONEOF(uncommonKey, UncommonHeaderKey) {
      return uncommonKey.hashCode();
    }
  }
  KJ_UNREACHABLE;
}

kj::uint Headers::HeaderCallbacks::hashCode(capnp::CommonHeaderName commondId) {
  return kj::hashCode(static_cast<uint>(commondId));
}

Headers::Headers(jsg::Lock& js, jsg::Dict<jsg::ByteString, jsg::ByteString> dict)
    : guard(Guard::NONE) {
  headers.reserve(dict.fields.size());
  for (auto& field: dict.fields) {
    append(js, kj::mv(field.name), kj::mv(field.value));
  }
}

Headers::Headers(jsg::Lock& js, const Headers& other): guard(Guard::NONE) {
  headers.reserve(other.headers.size());
  for (auto& header: other.headers) {
    // There really shouldn't be any duplicate headers in other, but just in case, use upsert
    // and we'll just ignore duplicates.
    headers.upsert(header.clone(), [](auto&, auto&&) {});
  }
}

Headers::Headers(jsg::Lock& js, const kj::HttpHeaders& other, Guard guard): guard(Guard::NONE) {
  headers.reserve(other.size());
  other.forEach([this, &js](auto name, auto value) {
    // We have to copy the strings here but we can avoid normalizing and validating since
    // they presumably already went through that process when they were added to the
    // kj::HttpHeader instance.
    appendUnguarded(js, jsg::ByteString(kj::str(name)), jsg::ByteString(kj::str(value)));
  });

  this->guard = guard;
}

jsg::Ref<Headers> Headers::clone(jsg::Lock& js) const {
  auto result = js.alloc<Headers>(js, *this);
  result->guard = guard;
  return kj::mv(result);
}

// Fill in the given HttpHeaders with these headers. Note that strings are inserted by
// reference, so the output must be consumed immediately.
void Headers::shallowCopyTo(kj::HttpHeaders& out) {
  for (auto& entry: headers) {
    for (auto& value: entry.values) {
      out.addPtrPtr(entry.getHeaderName(), value);
    }
  }
}

bool Headers::hasLowerCase(kj::StringPtr name) {
#ifdef KJ_DEBUG
  for (auto c: name) {
    KJ_DREQUIRE(!('A' <= c && c <= 'Z'));
  }
#endif
  return headers.find(getHeaderKeyFor(name)) != kj::none;
}

kj::Array<Headers::DisplayedHeader> Headers::getDisplayedHeaders(jsg::Lock& js) {
  if (FeatureFlags::get(js).getHttpHeadersGetSetCookie()) {
    kj::Vector<Headers::DisplayedHeader> vec;
    size_t reserved = 0;
    for (auto& header: headers) {
      if (isSetCookie(header.key)) {
        reserved += header.values.size();
      } else {
        reserved += 1;
      }
    }
    vec.reserve(reserved);
    for (auto& header: headers) {
      if (isSetCookie(header.key)) {
        // For set-cookie entries, we iterate each individually without combining them.
        for (auto& value: header.values) {
          vec.add(Headers::DisplayedHeader{
            .key = jsg::ByteString(kj::str(header.getKeyName())),
            .value = jsg::ByteString(kj::str(value)),
          });
        }
      } else {
        vec.add(Headers::DisplayedHeader{
          .key = jsg::ByteString(kj::str(header.getKeyName())),
          .value = jsg::ByteString(kj::strArray(header.values, ", ")),
        });
      }
    }
    auto ret = vec.releaseAsArray();
    std::sort(ret.begin(), ret.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    return kj::mv(ret);
  } else {
    // The old behavior before the standard getSetCookie() API was introduced...
    kj::Vector<Headers::DisplayedHeader> vec(headers.size());
    for (auto& header: headers) {
      vec.add(DisplayedHeader{
        .key = jsg::ByteString(kj::str(header.getKeyName())),
        .value = jsg::ByteString(kj::strArray(header.values, ", ")),
      });
    }
    auto ret = vec.releaseAsArray();
    std::sort(ret.begin(), ret.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    return kj::mv(ret);
  }
}

jsg::Ref<Headers> Headers::constructor(jsg::Lock& js, jsg::Optional<Initializer> init) {
  using StringDict = jsg::Dict<jsg::ByteString, jsg::ByteString>;

  KJ_IF_SOME(i, init) {
    KJ_SWITCH_ONEOF(kj::mv(i)) {
      KJ_CASE_ONEOF(dict, StringDict) {
        return js.alloc<Headers>(js, kj::mv(dict));
      }
      KJ_CASE_ONEOF(headers, jsg::Ref<Headers>) {
        return js.alloc<Headers>(js, *headers);
        // It's important to note here that we are treating the Headers object
        // as a special case here. Per the fetch spec, we *should* be grabbing
        // the Symbol.iterator off the Headers object and interpreting it as
        // a Sequence<Sequence<ByteString>> (as in the ByteStringPairs case
        // below). However, special casing Headers like we do here is more
        // performant and has other side effects such as preserving the casing
        // of header names that have been received.
        //
        // This does mean that we fail one of the more pathological (and kind
        // of weird) Web Platform Tests for this API:
        //
        //   const h = new Headers();
        //   h[Symbol.iterator] = function * () { yield ["test", "test"]; };
        //   const headers = new Headers(h);
        //   console.log(headers.has("test"));
        //
        // The spec would say headers.has("test") here should be true. With our
        // implementation here, however, we are ignoring the Symbol.iterator so
        // the test fails.
      }
      KJ_CASE_ONEOF(pairs, ByteStringPairs) {
        auto dict = KJ_MAP(entry, pairs) {
          JSG_REQUIRE(entry.size() == 2, TypeError,
              "To initialize a Headers object from a sequence, each inner sequence "
              "must have exactly two elements.");
          return StringDict::Field{kj::mv(entry[0]), kj::mv(entry[1])};
        };
        return js.alloc<Headers>(js, StringDict{kj::mv(dict)});
      }
    }
  }

  return js.alloc<Headers>();
}

kj::Maybe<jsg::ByteString> Headers::get(jsg::Lock& js, jsg::ByteString name) {
  requireValidHeaderName(name);
  return getNoChecks(js, name.asPtr());
}

kj::Maybe<jsg::ByteString> Headers::getNoChecks(jsg::Lock&, kj::StringPtr name) {
  KJ_IF_SOME(found, headers.find(getHeaderKeyFor(name))) {
    return jsg::ByteString(kj::strArray(found.values, ", "));
  }
  return kj::none;
}

kj::Maybe<jsg::ByteString> Headers::getCommon(jsg::Lock& js, capnp::CommonHeaderName idx) {
  KJ_DASSERT(static_cast<size_t>(idx) <= MAX_COMMON_HEADER_ID);
  KJ_IF_SOME(found, headers.find(idx)) {
    return jsg::ByteString(kj::strArray(found.values, ", "));
  }
  return kj::none;
}

kj::ArrayPtr<jsg::ByteString> Headers::getSetCookie() {
  KJ_IF_SOME(found, headers.find(capnp::CommonHeaderName::SET_COOKIE)) {
    return found.values.asPtr();
  }
  return nullptr;
}

kj::ArrayPtr<jsg::ByteString> Headers::getAll(jsg::ByteString name) {
  requireValidHeaderName(name);

  if (strcasecmp(name.cStr(), "set-cookie") != 0) {
    JSG_FAIL_REQUIRE(TypeError, "getAll() can only be used with the header name \"Set-Cookie\".");
  }

  // getSetCookie() is the standard API here. getAll(...) is our legacy non-standard extension
  // for the same use case. We continue to support getAll for backwards compatibility but moving
  // forward users really should be using getSetCookie.
  return getSetCookie();
}

bool Headers::has(jsg::ByteString name) {
  requireValidHeaderName(name);
  return headers.find(getHeaderKeyFor(name)) != kj::none;
}

bool Headers::hasCommon(capnp::CommonHeaderName idx) {
  KJ_DASSERT(static_cast<size_t>(idx) <= MAX_COMMON_HEADER_ID);
  return headers.find(idx) != kj::none;
}

void Headers::set(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value) {
  checkGuard();
  requireValidHeaderName(name);
  value = normalizeHeaderValue(js, kj::mv(value));
  requireValidHeaderValue(value);
  setUnguarded(js, kj::mv(name), kj::mv(value));
}

void Headers::setCommon(jsg::Lock& js, capnp::CommonHeaderName idx, jsg::ByteString value) {
  KJ_DASSERT(static_cast<size_t>(idx) <= MAX_COMMON_HEADER_ID);
  HeaderKey key = static_cast<uint>(idx);
  kj::Vector<jsg::ByteString> values(1);
  values.add(kj::mv(value));
  headers.upsert(Header(kj::mv(key), kj::none, kj::mv(values)),
      [](auto& existing, auto&& replacement) { existing.values = kj::mv(replacement.values); });
}

void Headers::setUnguarded(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value) {
  // We're unconditionally replacing existing values.
  headers.upsert(Header(kj::mv(name), kj::mv(value)),
      [](auto& existing, auto&& replacement) { existing.values = kj::mv(replacement.values); });
}

void Headers::append(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value) {
  checkGuard();
  requireValidHeaderName(name);
  value = normalizeHeaderValue(js, kj::mv(value));
  requireValidHeaderValue(value);
  appendUnguarded(js, kj::mv(name), kj::mv(value));
}

void Headers::appendUnguarded(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value) {
  // If the header already exists, we just add to its values.
  auto key = getHeaderKeyFor(name);
  KJ_IF_SOME(found, headers.find(key)) {
    found.values.add(kj::mv(value));
  } else {
    headers.insert(Header(kj::mv(key), kj::mv(name), kj::mv(value)));
  }
}

void Headers::delete_(jsg::ByteString name) {
  checkGuard();
  requireValidHeaderName(name);
  headers.eraseMatch(getHeaderKeyFor(name));
}

// There are a couple implementation details of the Headers iterators worth calling out.
//
// 1. Each iterator gets its own copy of the keys and/or values of the headers. While nauseating
//    from a performance perspective, this solves both the iterator -> iterable lifetime dependence
//    and the iterator invalidation issue: i.e., it's impossible for a user to unsafely modify the
//    Headers data structure while iterating over it, because they are simply two separate data
//    structures. By empirical testing, this seems to be how Chrome implements Headers iteration.
//
//    Other alternatives bring their own pitfalls. We could store a Ref of the parent Headers
//    object, solving the lifetime issue. To solve the iterator invalidation issue, we could store a
//    copy of the currently-iterated-over key and use std::upper_bound() to find the next entry
//    every time we want to increment the iterator (making the increment operation O(lg n) rather
//    than O(1)); or we could make each Header entry in the map store a set of back-pointers to all
//    live iterators pointing to it, with delete_() incrementing all iterators in the set whenever
//    it deletes a header entry. Neither hack appealed to me.
//
// 2. Notice that the next() member function of the iterator classes moves the string(s) they
//    contain, rather than making a copy of them as in the FormData iterators. This is safe to do
//    because, unlike FormData, these iterators have their own copies of the strings, and since they
//    are forward-only iterators, we know we won't need the strings again.
//
// TODO(perf): On point 1, perhaps we could avoid most copies by using a copy-on-write strategy
//   applied to the header map elements? We'd still copy the whole data structure to avoid iterator
//   invalidation, but the elements would be cheaper to copy.

jsg::Ref<Headers::EntryIterator> Headers::entries(jsg::Lock& js) {
  return js.alloc<EntryIterator>(IteratorState<DisplayedHeader>{getDisplayedHeaders(js)});
}
jsg::Ref<Headers::KeyIterator> Headers::keys(jsg::Lock& js) {
  if (FeatureFlags::get(js).getHttpHeadersGetSetCookie()) {
    kj::Vector<jsg::ByteString> keysCopy;
    for (auto& header: headers) {
      // Set-Cookie headers must be handled specially. They should never be combined into a
      // single value, so the values iterator must separate them. It seems a bit silly, but
      // the keys iterator can end up having multiple set-cookie instances.
      if (isSetCookie(header.key)) {
        for (auto n = 0; n < header.values.size(); n++) {
          keysCopy.add(jsg::ByteString(kj::str(header.getKeyName())));
        }
      } else {
        keysCopy.add(jsg::ByteString(kj::str(header.getKeyName())));
      }
    }
    auto ret = keysCopy.releaseAsArray();
    std::sort(ret.begin(), ret.end(), [](const auto& a, const auto& b) { return a < b; });
    return js.alloc<KeyIterator>(IteratorState<jsg::ByteString>{kj::mv(ret)});
  } else {
    auto keysCopy =
        KJ_MAP(header, headers) { return jsg::ByteString(kj::str(header.getKeyName())); };
    std::sort(keysCopy.begin(), keysCopy.end(), [](const auto& a, const auto& b) { return a < b; });
    return js.alloc<KeyIterator>(IteratorState<jsg::ByteString>{kj::mv(keysCopy)});
  }
}
jsg::Ref<Headers::ValueIterator> Headers::values(jsg::Lock& js) {
  // Annoyingly, the spec requires that the values iterator still be sorted by key.
  // To make this easiest, let's grab the displayed headers and then extract the values.
  // the getDisplayedHeaders() function does the sorting for us at the cost of an extra
  // copy of the names. Fortunately, enumerating by value is likely way less common than
  // other forms of iteration so the cost should be acceptable.
  auto headers = getDisplayedHeaders(js);
  kj::Vector<jsg::ByteString> values(headers.size());
  for (auto& header: headers) {
    values.add(kj::mv(header.value));
  };
  return js.alloc<ValueIterator>(IteratorState<jsg::ByteString>(values.releaseAsArray()));
}

void Headers::forEach(jsg::Lock& js,
    jsg::Function<void(kj::StringPtr, kj::StringPtr, jsg::Ref<Headers>)> callback,
    jsg::Optional<jsg::Value> thisArg) {
  auto receiver = js.v8Undefined();
  KJ_IF_SOME(arg, thisArg) {
    auto handle = arg.getHandle(js);
    if (!handle->IsNullOrUndefined()) {
      receiver = handle;
    }
  }
  callback.setReceiver(js.v8Ref(receiver));

  for (auto& entry: getDisplayedHeaders(js)) {
    callback(js, entry.value, entry.key, JSG_THIS);
  }
}

bool Headers::inspectImmutable() {
  return guard != Guard::NONE;
}

void Headers::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  for (const auto& header: headers) {
    tracker.trackField(nullptr, header);
  }
}

// -----------------------------------------------------------------------------
// serialization of headers
//
// http-over-capnp.capnp has a nice list of common header names, taken from the HTTP/2 standard.
// We'll use it as an optimization.
//
// Note that using numeric IDs for headers implies we lose the original capitalization. However,
// the JS Headers API doesn't actually give the application any way to observe the capitalization
// of header names -- it only becomes relevant when serializing over HTTP/1.1. And at that point,
// we are actually free to change the capitalization anyway, and we commonly do (KJ itself will
// normalize capitalization of all registered headers, and http-over-capnp also loses
// capitalization). So, it's certainly not worth it to try to keep the original capitalization
// across serialization.

void Headers::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  // We serialize as a series of key-value pairs. Each value is a length-delimited string. Each key
  // is a common header ID, or the value zero to indicate an uncommon header, which is then
  // followed by a length-delimited name.

  serializer.writeRawUint32(static_cast<uint>(guard));

  // Write the count of headers.
  uint count = 0;
  for (auto& entry: headers) {
    count += entry.values.size();
  }
  serializer.writeRawUint32(count);

  // Now write key/values.
  for (auto& header: headers) {
    for (auto& value: header.values) {
      KJ_SWITCH_ONEOF(header.key) {
        KJ_CASE_ONEOF(commonId, uint) {
          serializer.writeRawUint32(commonId);
        }
        KJ_CASE_ONEOF(uncommonKey, UncommonHeaderKey) {
          serializer.writeRawUint32(0);
          serializer.writeLengthDelimited(header.getHeaderName());
        }
      }
      serializer.writeLengthDelimited(value);
    }
  }
}

jsg::Ref<Headers> Headers::deserialize(
    jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer) {
  auto result = js.alloc<Headers>();
  uint guard = deserializer.readRawUint32();
  KJ_REQUIRE(guard <= static_cast<uint>(Guard::NONE), "unknown guard value");

  uint count = deserializer.readRawUint32();

  for (auto i KJ_UNUSED: kj::zeroTo(count)) {
    uint commonId = deserializer.readRawUint32();
    kj::String name;
    if (commonId == 0) {
      name = deserializer.readLengthDelimitedString();
    } else {
      KJ_ASSERT(commonId <= MAX_COMMON_HEADER_ID);
      name = kj::str(getCommonHeaderName(commonId));
    }

    auto value = deserializer.readLengthDelimitedString();

    // TODO(performance): We can avoid some copies here by constructing the
    // the Header entry directly using information from the deserializer
    // directly without relying on append.
    result->append(js, jsg::ByteString(kj::mv(name)), jsg::ByteString(kj::mv(value)));
  }

  // Don't actually set the guard until here because it may block the ability to call `append()`.
  result->guard = static_cast<Guard>(guard);

  return result;
}

}  // namespace workerd::api
