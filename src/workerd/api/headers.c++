#include "headers.h"

#include "simdutf.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/util/header-validation.h>
#include <workerd/util/strings.h>

namespace workerd::api {

namespace {

#define COMMON_HEADERS(V)                                                                          \
  V(ACCEPT_CHARSET, "accept-charset")                                                              \
  V(ACCEPT_ENCODING, "accept-encoding")                                                            \
  V(ACCEPT_LANGUAGE, "accept-language")                                                            \
  V(ACCEPT_RANGES, "accept-ranges")                                                                \
  V(ACCEPT, "accept")                                                                              \
  V(ACCESS_CONTROL_ALLOW_ORIGIN, "access-control-allow-origin")                                    \
  V(AGE, "age")                                                                                    \
  V(ALLOW, "allow")                                                                                \
  V(AUTHORIZATION, "authorization")                                                                \
  V(CACHE_CONTROL, "cache-control")                                                                \
  V(CONTENT_DISPOSITION, "content-disposition")                                                    \
  V(CONTENT_ENCODING, "content-encoding")                                                          \
  V(CONTENT_LANGUAGE, "content-language")                                                          \
  V(CONTENT_LENGTH, "content-length")                                                              \
  V(CONTENT_LOCATION, "content-location")                                                          \
  V(CONTENT_RANGE, "content-range")                                                                \
  V(CONTENT_TYPE, "content-type")                                                                  \
  V(COOKIE, "cookie")                                                                              \
  V(DATE, "date")                                                                                  \
  V(ETAG, "etag")                                                                                  \
  V(EXPECT, "expect")                                                                              \
  V(EXPIRES, "expires")                                                                            \
  V(FROM, "from")                                                                                  \
  V(HOST, "host")                                                                                  \
  V(IF_MATCH, "if-match")                                                                          \
  V(IF_MODIFIED_SINCE, "if-modified-since")                                                        \
  V(IF_NONE_MATCH, "if-none-match")                                                                \
  V(IF_RANGE, "if-range")                                                                          \
  V(IF_UNMODIFIED_SINCE, "if-unmodified-since")                                                    \
  V(LAST_MODIFIED, "last-modified")                                                                \
  V(LINK, "link")                                                                                  \
  V(LOCATION, "location")                                                                          \
  V(MAX_FORWARDS, "max-forwards")                                                                  \
  V(PROXY_AUTHENTICATE, "proxy-authenticate")                                                      \
  V(PROXY_AUTHORIZATION, "proxy-authorization")                                                    \
  V(RANGE, "range")                                                                                \
  V(REFERER, "referer")                                                                            \
  V(REFRESH, "refresh")                                                                            \
  V(RETRY_AFTER, "retry-after")                                                                    \
  V(SERVER, "server")                                                                              \
  V(SET_COOKIE, "set-cookie")                                                                      \
  V(STRICT_TRANSPORT_SECURITY, "strict-transport-security")                                        \
  V(TRANSFER_ENCODING, "transfer-encoding")                                                        \
  V(USER_AGENT, "user-agent")                                                                      \
  V(VARY, "vary")                                                                                  \
  V(VIA, "via")                                                                                    \
  V(WWW_AUTHENTICATE, "www-authenticate")

// Constexpr array of lowercase common header names (must match CommonHeaderName enum order
// and must be kept in sync with the ordinal values defined in http-over-capnp.capnp). Since
// it is extremely unlikely that those will change often, we hardcode them here for runtime
// efficiency.
constexpr kj::StringPtr COMMON_HEADER_NAMES[] = {nullptr,  // 0: invalid
#define V(_, str) str##_kj,
  COMMON_HEADERS(V)
#undef V
};

// Static asserts for all of the common header names to ensure they are correct and in sync
// with the enum values.
static_assert(std::size(COMMON_HEADER_NAMES) == (Headers::MAX_COMMON_HEADER_ID + 1),
    "COMMON_HEADER_NAMES must have an entry for each CommonHeaderName enum value");
static_assert(CommonHeaderIds::MAX_COMMON_HEADER_ID == Headers::MAX_COMMON_HEADER_ID);
#define V(id, str)                                                                                 \
  static_assert(COMMON_HEADER_NAMES[static_cast<uint>(capnp::CommonHeaderName::id)] == str##_kj);
COMMON_HEADERS(V)
#undef V

inline constexpr kj::StringPtr getCommonHeaderName(uint id) {
  KJ_ASSERT(id > 0 && id <= Headers::MAX_COMMON_HEADER_ID, "Invalid common header ID");
  return COMMON_HEADER_NAMES[id];
}

// The CASE_CONVERSION_TABLE maps ASCII characters to their case conversion offset. For example,
// 'A' (0x41) maps to 32, which is the offset to convert it to 'a' (0x61). This allows us to perform
// branch-free case-insensitive comparisons and hashing by adding the offset to each character.
// For non-ASCII characters, the offset is 0, which means they are not modified. This table is
// used in the strcaseeq and caseInsensitiveHash functions below.
// clang-format off
constexpr kj::byte CASE_CONVERSION_TABLE[] = {
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
  32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};
// clang-format on

// Bit paranoid but just to be safe...
static_assert(std::numeric_limits<kj::byte>::max() < kj::size(CASE_CONVERSION_TABLE));

constexpr bool strcaseeq(kj::StringPtr a, kj::StringPtr b) {
  if (a.size() != b.size()) return false;

  size_t i = 0;
  while (i + sizeof(uint64_t) <= a.size()) {
    // We assume that a is always lower-case to start.
    uint64_t wa = (static_cast<uint64_t>(static_cast<uint8_t>(a[i + 0])) << 0) |
        (static_cast<uint64_t>(static_cast<uint8_t>(a[i + 1])) << 8) |
        (static_cast<uint64_t>(static_cast<uint8_t>(a[i + 2])) << 16) |
        (static_cast<uint64_t>(static_cast<uint8_t>(a[i + 3])) << 24) |
        (static_cast<uint64_t>(static_cast<uint8_t>(a[i + 4])) << 32) |
        (static_cast<uint64_t>(static_cast<uint8_t>(a[i + 5])) << 40) |
        (static_cast<uint64_t>(static_cast<uint8_t>(a[i + 6])) << 48) |
        (static_cast<uint64_t>(static_cast<uint8_t>(a[i + 7])) << 56);

    uint8_t b0 = b[i + 0];
    uint8_t b1 = b[i + 1];
    uint8_t b2 = b[i + 2];
    uint8_t b3 = b[i + 3];
    uint8_t b4 = b[i + 4];
    uint8_t b5 = b[i + 5];
    uint8_t b6 = b[i + 6];
    uint8_t b7 = b[i + 7];

    uint64_t normalized_b = (static_cast<uint64_t>(b0 + CASE_CONVERSION_TABLE[b0]) << 0) |
        (static_cast<uint64_t>(b1 + CASE_CONVERSION_TABLE[b1]) << 8) |
        (static_cast<uint64_t>(b2 + CASE_CONVERSION_TABLE[b2]) << 16) |
        (static_cast<uint64_t>(b3 + CASE_CONVERSION_TABLE[b3]) << 24) |
        (static_cast<uint64_t>(b4 + CASE_CONVERSION_TABLE[b4]) << 32) |
        (static_cast<uint64_t>(b5 + CASE_CONVERSION_TABLE[b5]) << 40) |
        (static_cast<uint64_t>(b6 + CASE_CONVERSION_TABLE[b6]) << 48) |
        (static_cast<uint64_t>(b7 + CASE_CONVERSION_TABLE[b7]) << 56);

    if (wa != normalized_b) return false;
    i += sizeof(uint64_t);
  }

  for (; i < a.size(); ++i) {
    char cb = b[i] + CASE_CONVERSION_TABLE[static_cast<kj::byte>(b[i])];
    if (a[i] != cb) return false;
  }
  return true;
}

// A FNV hash function that is case-insensitive.
// See: https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
constexpr uint caseInsensitiveHash(kj::StringPtr name) {
  static constexpr uint32_t MULTIPLIER = 16777619u;
  uint hash = 2166136261u;
  size_t i = 0;

  while (i + sizeof(uint64_t) <= name.size()) {
    uint8_t b0 = name[i + 0];
    uint8_t b1 = name[i + 1];
    uint8_t b2 = name[i + 2];
    uint8_t b3 = name[i + 3];
    uint8_t b4 = name[i + 4];
    uint8_t b5 = name[i + 5];
    uint8_t b6 = name[i + 6];
    uint8_t b7 = name[i + 7];

    hash ^= b0 + CASE_CONVERSION_TABLE[b0];
    hash *= MULTIPLIER;
    hash ^= b1 + CASE_CONVERSION_TABLE[b1];
    hash *= MULTIPLIER;
    hash ^= b2 + CASE_CONVERSION_TABLE[b2];
    hash *= MULTIPLIER;
    hash ^= b3 + CASE_CONVERSION_TABLE[b3];
    hash *= MULTIPLIER;
    hash ^= b4 + CASE_CONVERSION_TABLE[b4];
    hash *= MULTIPLIER;
    hash ^= b5 + CASE_CONVERSION_TABLE[b5];
    hash *= MULTIPLIER;
    hash ^= b6 + CASE_CONVERSION_TABLE[b6];
    hash *= MULTIPLIER;
    hash ^= b7 + CASE_CONVERSION_TABLE[b7];
    hash *= MULTIPLIER;

    i += sizeof(uint64_t);
  }

  // Handle remaining bytes
  for (; i < name.size(); ++i) {
    uint8_t b = name[i];
    hash ^= b + CASE_CONVERSION_TABLE[b];
    hash *= MULTIPLIER;
  }

  hash = (hash >> 16) ^ hash;
  return hash;
}

constexpr size_t HEADER_MAP_SIZE = 512;

struct HeaderHashTable final {
  struct Entry {
    kj::StringPtr name;
    uint id = 0;
  };

  Entry entries[HEADER_MAP_SIZE] = {};

  // These are both calculated at compile time by the constexpr constructor.
  size_t kMaxEntrySize = 0;
  size_t kMinEntrySize = kj::maxValue;

  constexpr HeaderHashTable() {
    for (uint i = 1; i <= Headers::MAX_COMMON_HEADER_ID; ++i) {
      auto name = COMMON_HEADER_NAMES[i];
      size_t slot = caseInsensitiveHash(name) % HEADER_MAP_SIZE;
      entries[slot] = {name, i};
      kMaxEntrySize = std::max(kMaxEntrySize, name.size());
      kMinEntrySize = std::min(kMinEntrySize, name.size());
    }
  }

  constexpr uint find(kj::StringPtr name) const {
    if (name.size() < kMinEntrySize || name.size() > kMaxEntrySize) {
      return 0;
    }
    size_t slot = caseInsensitiveHash(name) % HEADER_MAP_SIZE;
    const auto& entry = entries[slot];
    if (entry.id != 0 && strcaseeq(entry.name, name)) {
      return entry.id;
    }
    return 0;  // Not found
  }

  // The isPerfectTest and isLowerTest are constexpr static assertions used
  // only the verify correctness of the hash table at compile time.
  constexpr bool isPerfectTest() const {
    for (uint i = 1; i <= Headers::MAX_COMMON_HEADER_ID; ++i) {
      auto name = COMMON_HEADER_NAMES[i];
      size_t slot = caseInsensitiveHash(name) % HEADER_MAP_SIZE;
      if (entries[slot].id != i) return false;
      if (!strcaseeq(entries[slot].name, name)) return false;
    }
    return true;
  }

  constexpr bool isLowerTest() const {
    for (uint i = 1; i <= Headers::MAX_COMMON_HEADER_ID; ++i) {
      auto name = COMMON_HEADER_NAMES[i];
      for (char c: name) {
        if ('A' <= c && c <= 'Z') return false;
      }
    }
    return true;
  }
};

constexpr HeaderHashTable HEADER_HASH_TABLE;
static_assert(HEADER_HASH_TABLE.kMaxEntrySize == 27);
static_assert(HEADER_HASH_TABLE.kMinEntrySize == 3);
static_assert(HEADER_HASH_TABLE.isPerfectTest());
static_assert(HEADER_HASH_TABLE.isLowerTest());
static_assert(HEADER_HASH_TABLE.find("accept-charset"_kj) == 1);
static_assert(HEADER_HASH_TABLE.find("AcCePt-ChArSeT"_kj) == 1);
static_assert(std::size(COMMON_HEADER_NAMES) == (Headers::MAX_COMMON_HEADER_ID + 1));

void maybeWarnIfBadHeaderString(kj::StringPtr name, kj::StringPtr str) {
  KJ_IF_SOME(context, IoContext::tryCurrent()) {
    if (context.hasWarningHandler()) {
      if (!simdutf::validate_ascii(str.begin(), str.size())) {
        // The string contains non-ASCII characters. While any 8-bit value is technically valid
        // in HTTP headers, we encode header strings as UTF-8, so we want to warn the user that
        // their header name/value may not be what they may expect based on what browsers do.
        auto utf8Hex = kj::strArray(
            KJ_MAP(b, str) { return kj::str("\\x", kj::hex(static_cast<kj::byte>(b))); }, "");
        context.logWarning(kj::str("A header value for \"", name,
            "\" contains non-ASCII "
            "characters: \"",
            str, "\" (raw bytes: \"", utf8Hex,
            "\"). As a quirk to support Unicode, we are encoding "
            "values as UTF-8 in the header, but in a browser this would likely result in a "
            "TypeError exception. Consider encoding this string in ASCII for compatibility with "
            "browser implementations of the Fetch specification."));
      }
    }
  }
}

// Left- and right-trim HTTP whitespace from `value`.
kj::String normalizeHeaderValue(kj::StringPtr name, kj::String value) {
  // Fast path: if empty, return as-is
  if (value.size() == 0) {
    return kj::mv(value);
  }

  char* begin = value.begin();
  char* end = value.end();

  while (begin < end && util::isHttpWhitespace(*begin)) {
    ++begin;
  }
  while (begin < end && util::isHttpWhitespace(*(end - 1))) {
    --end;
  }

  size_t newSize = end - begin;
  if (newSize == value.size()) {
    JSG_REQUIRE(workerd::util::isValidHeaderValue(value), TypeError, "Invalid header value.");
    maybeWarnIfBadHeaderString(name, value);
    return kj::mv(value);
  }

  auto trimmed = kj::ArrayPtr(begin, newSize);
  JSG_REQUIRE(workerd::util::isValidHeaderValue(trimmed), TypeError, "Invalid header value.");
  maybeWarnIfBadHeaderString(name, value);
  // By attaching the original array to the trimmed view, we keep the original allocation alive
  // and prevent an unnecessary copy.
  return kj::str(trimmed.attach(value.releaseArray()));
}

// The common header ID for `name`, or zero after checking that it's a valid header name.
// getHeaderKeyFor() goes through here too, so that HEADER_HASH_TABLE.find() has the one caller
// and stays inlined.
uint findCommonHeaderId(kj::StringPtr name) {
  if (uint commonId = HEADER_HASH_TABLE.find(name)) {
    return commonId;
  }
  for (char c: name) {
    JSG_REQUIRE(util::isHttpTokenChar(c), TypeError, "Invalid header name.");
  }
  return 0;
}

constexpr char toLowerAscii(char c) {
  return c + CASE_CONVERSION_TABLE[static_cast<kj::byte>(c)];
}

// Case-insensitive ordering of header names. Names that compare equal here have the same key.
bool caseInsensitiveLess(kj::StringPtr a, kj::StringPtr b) {
  size_t n = kj::min(a.size(), b.size());
  for (size_t i = 0; i < n; i++) {
    char ca = toLowerAscii(a[i]);
    char cb = toLowerAscii(b[i]);
    if (ca != cb) return static_cast<kj::byte>(ca) < static_cast<kj::byte>(cb);
  }
  return a.size() < b.size();
}

Headers::HeaderKey getHeaderKeyFor(kj::StringPtr name) {
  if (uint commonId = findCommonHeaderId(name)) {
    KJ_DASSERT(commonId > 0 && commonId <= Headers::MAX_COMMON_HEADER_ID);
    return commonId;
  }

  // Not a common header, so allocate lowercase copy for uncommon header
  return toLower(name);
}
}  // namespace

Headers::Headers(jsg::Lock& js, jsg::Dict<kj::String, kj::String> dict): guard(Guard::NONE) {
  // Because the headers might end up in either of our two tables,
  // we can't really reserve space for them up front.
  for (auto& field: dict.fields) {
    append(js, kj::mv(field.name), kj::mv(field.value));
  }
}

Headers::Headers(jsg::Lock& js, const Headers& other): guard(Guard::NONE) {
  KJ_IF_SOME(n, other.native) {
    native = kj::atomicAddRef(*n);
    return;
  }
  for (kj::uint i = 1; i < other.commonHeaders.size(); i++) {
    commonHeaders[i] =
        other.commonHeaders[i].map([](const kj::Own<Header>& h) { return h->clone(); });
  }
  uncommonHeaders.reserve(other.uncommonHeaders.size());
  for (auto& [key, header]: other.uncommonHeaders) {
    // It should not be possible to have duplicate keys here.
    uncommonHeaders.insert(kj::str(key), header->clone());
  }
}

Headers::Headers(jsg::Lock& js, const kj::HttpHeaders& other, Guard guard): guard(guard) {
  // Headers that the table has an id for are identified by that id rather than by hashing their
  // names again, when our thread's table maps the id to a common header.
  kj::Maybe<const CommonHeaderIds&> ids;
  KJ_IF_SOME(context, IoContext::tryCurrent()) {
    ids = context.getHeaderIds().commonHeaderIds;
  }

  // First collect the entries pointing at `other`'s strings, then copy all of the strings into
  // one buffer. The values aren't normalized or validated since they presumably already went
  // through that process when they were added to the kj::HttpHeaders; names that aren't common
  // headers are checked here as appending them would.
  auto entries = kj::heapArrayBuilder<NativeHeaders::Entry>(other.size());
  size_t charCount = 0;
  auto add = [&](kj::uint commonId, kj::StringPtr name, kj::StringPtr value) {
    entries.add(NativeHeaders::Entry{.commonId = commonId, .name = name, .value = value});
    charCount += name.size() + value.size() + 2;
  };
  other.forEach([&](kj::HttpHeaderId id, kj::StringPtr value) {
    auto name = id.toString();
    KJ_IF_SOME(i, ids) {
      KJ_IF_SOME(common, i.fromKj(id, name)) {
        add(static_cast<kj::uint>(common), name, value);
        return;
      }
    }
    add(findCommonHeaderId(name), name, value);
  }, [&](kj::StringPtr name, kj::StringPtr value) { add(findCommonHeaderId(name), name, value); });

  auto chars = kj::heapArray<char>(charCount);
  char* pos = chars.begin();
  auto copy = [&](kj::StringPtr str) {
    memcpy(pos, str.begin(), str.size());
    pos[str.size()] = '\0';
    kj::StringPtr result(pos, str.size());
    pos += str.size() + 1;
    return result;
  };
  for (auto& entry: entries) {
    entry.name = copy(entry.name);
    entry.value = copy(entry.value);
  }
  KJ_DASSERT(pos == chars.end());

  native = kj::atomicRefcounted<NativeHeaders>(kj::mv(chars), entries.finish());
}

void Headers::materializeNative() {
  // Keep the strings alive while we copy them, but drop `native` first so that the appends below
  // see an ordinary object.
  auto snapshot = kj::mv(KJ_ASSERT_NONNULL(native));
  native = kj::none;
  for (auto& entry: snapshot->entries) {
    if (entry.commonId != 0) {
      appendCommonUnguarded(entry.commonId, entry.name, kj::str(entry.value));
    } else {
      appendUncommonUnguarded(toLower(entry.name), kj::str(entry.name), kj::str(entry.value));
    }
  }
}

kj::Maybe<kj::String> Headers::getNativeJoined(const NativeHeaders& native, const HeaderKey& key) {
  // Usually there's exactly one match; don't build a vector for it.
  kj::Maybe<kj::StringPtr> only;
  bool several = false;
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(id, kj::uint) {
      for (auto& entry: native.entries) {
        if (entry.commonId == id) {
          several = only != kj::none;
          if (several) break;
          only = entry.value;
        }
      }
    }
    KJ_CASE_ONEOF(lower, kj::String) {
      for (auto& entry: native.entries) {
        if (entry.commonId == 0 && strcaseeq(lower, entry.name)) {
          several = only != kj::none;
          if (several) break;
          only = entry.value;
        }
      }
    }
  }
  if (several) {
    return kj::strArray(getNative(native, key), ", ");
  }
  return only.map([](kj::StringPtr value) { return kj::str(value); });
}

kj::Vector<kj::StringPtr> Headers::getNative(const NativeHeaders& native, const HeaderKey& key) {
  kj::Vector<kj::StringPtr> values;
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(id, kj::uint) {
      for (auto& entry: native.entries) {
        if (entry.commonId == id) values.add(entry.value);
      }
    }
    KJ_CASE_ONEOF(lower, kj::String) {
      for (auto& entry: native.entries) {
        if (entry.commonId == 0 && strcaseeq(lower, entry.name)) values.add(entry.value);
      }
    }
  }
  return values;
}

kj::Maybe<Headers::Header&> Headers::tryGetHeader(const HeaderKey& key) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(idx, kj::uint) {
      return commonHeaders[idx].map([](kj::Own<Header>& header) -> Header& { return *header; });
    }
    KJ_CASE_ONEOF(name, kj::String) {
      return uncommonHeaders.find(name).map(
          [](kj::Own<Header>& header) -> Header& { return *header; });
    }
  }
  KJ_UNREACHABLE;
}

jsg::Ref<Headers> Headers::clone(jsg::Lock& js) const {
  auto result = js.alloc<Headers>(js, *this);
  result->guard = guard;
  return kj::mv(result);
}

kj::Maybe<kj::Own<jsg::Wrappable>> Headers::snapshotClone() const {
  auto clone = kj::refcounted<Headers>();
  clone->guard = guard;
  KJ_IF_SOME(n, native) {
    clone->native = kj::atomicAddRef(*n);
  }
  for (auto i: kj::indices(commonHeaders)) {
    KJ_IF_SOME(header, commonHeaders[i]) {
      clone->commonHeaders[i] = header->clone();
    }
  }
  for (auto& entry: uncommonHeaders) {
    clone->uncommonHeaders.insert(kj::str(entry.key), entry.value->clone());
  }
  return ownAsWrappable(kj::mv(clone));
}

namespace {

// Adds the values of the common header `commonId`, which is `id` in `out`'s table, to `out` --
// what addPtrPtr(name, value) for each value would do, without looking the name up again.
template <typename T>
void copyCommonHeaderById(kj::HttpHeaders& out,
    kj::HttpHeaderId id,
    kj::uint commonId,
    kj::StringPtr name,
    kj::ArrayPtr<const T> values) {
  if (out.get(id) != kj::none) {
    // Let kj merge these with what `out` already holds.
    for (auto& value: values) {
      out.addPtrPtr(name, value);
    }
    return;
  }

  // kj treats an empty indexed value as absent, so values before the first non-empty one are
  // dropped rather than merged.
  while (values.size() > 0 && values[0].size() == 0) {
    values = values.slice(1);
  }
  if (values.size() == 0) {
    return;
  } else if (values.size() == 1) {
    out.setPtr(id, values[0]);
  } else if (commonId == static_cast<kj::uint>(capnp::CommonHeaderName::SET_COOKIE)) {
    // kj stores repeated Set-Cookie values as unindexed headers rather than merging them.
    out.setPtr(id, values[0]);
    for (auto& value: values.slice(1)) {
      out.addPtrPtr(name, value);
    }
  } else {
    // Duplicate HTTP headers are equivalent to the values being separated by a comma.
    auto merged = kj::strArray(values, ", ");
    out.setPtr(id, merged);
    out.takeOwnership(kj::mv(merged));
  }
}

// Adds the values of the common header `commonId` to `out`: by id given the ids of `out`'s table,
// otherwise by name.
template <typename T>
void copyCommonHeaderTo(kj::HttpHeaders& out,
    kj::Maybe<const CommonHeaderIds&> maybeIds,
    kj::uint commonId,
    kj::StringPtr name,
    kj::ArrayPtr<const T> values) {
  KJ_IF_SOME(ids, maybeIds) {
    copyCommonHeaderById(
        out, ids.toKj(static_cast<capnp::CommonHeaderName>(commonId)), commonId, name, values);
  } else {
    for (auto& value: values) {
      out.addPtrPtr(name, value);
    }
  }
}

}  // namespace

// Fill in the given HttpHeaders with these headers. Note that strings are inserted by
// reference, so the output must be consumed immediately.
void Headers::shallowCopyTo(kj::HttpHeaders& out) {
  shallowCopyToImpl(out, kj::none);
}

void Headers::shallowCopyTo(kj::HttpHeaders& out, const CommonHeaderIds& ids) {
  shallowCopyToImpl(out, ids);
}

void Headers::shallowCopyToImpl(kj::HttpHeaders& out, kj::Maybe<const CommonHeaderIds&> ids) {
  KJ_IF_SOME(n, native) {
    shallowCopyNativeTo(*n, out, ids);
    return;
  }

  for (kj::uint i = 1; i < commonHeaders.size(); i++) {
    KJ_IF_SOME(header, commonHeaders[i]) {
      kj::StringPtr name = header->name.map([](const kj::String& n) -> kj::StringPtr {
        return n;
      }).orDefault(getCommonHeaderName(i));
      copyCommonHeaderTo(out, ids, i, name, header->values.asPtr().asConst());
    }
  }

  // This is unfortunate... but we have to sort the uncommon headers by name
  // before passing them off to kj::HttpHeaders...While the actual header order
  // (for different header names) is not meaningful in HTTP, we have tests that
  // expect a certain order for reproducibility.
  struct Entry {
    kj::StringPtr name;
    kj::StringPtr value;
  };
  kj::Vector<Entry> entries(uncommonHeaders.size());
  for (auto& header: uncommonHeaders) {
    KJ_IF_SOME(name, header.value->name) {
      for (auto& value: header.value->values) {
        entries.add(Entry{.name = name, .value = value});
      }
    } else {
      for (auto& value: header.value->values) {
        entries.add(Entry{.name = header.key, .value = value});
      }
    }
  }
  std::stable_sort(entries.begin(), entries.end(),
      [](const Entry& a, const Entry& b) { return a.name < b.name; });

  for (const auto& entry: entries) {
    out.addPtrPtr(entry.name, entry.value);
  }
}

// Produces what shallowCopyTo() would after materialize(), without materializing.
void Headers::shallowCopyNativeTo(
    const NativeHeaders& native, kj::HttpHeaders& out, kj::Maybe<const CommonHeaderIds&> ids) {
  // Each common header's values go out together, in order, under the name it first arrived with.
  // Only repeated Set-Cookie values become unindexed headers, so the order in which different
  // common headers are written doesn't change the result.
  kj::FixedArray<kj::byte, MAX_COMMON_HEADER_ID + 1> counts;
  for (auto& count: counts) count = 0;
  size_t uncommonCount = 0;
  for (auto& entry: native.entries) {
    if (entry.commonId == 0) {
      ++uncommonCount;
    } else if (counts[entry.commonId] < 2) {
      ++counts[entry.commonId];
    }
  }
  for (auto i: kj::indices(native.entries)) {
    auto& entry = native.entries[i];
    if (entry.commonId == 0) continue;
    if (counts[entry.commonId] == 1) {
      copyCommonHeaderTo(out, ids, entry.commonId, entry.name, kj::arrayPtr(entry.value));
    } else if (counts[entry.commonId] == 2) {
      // The first of several entries for this header: gather the rest.
      kj::Vector<kj::StringPtr> values;
      for (auto& later: native.entries.slice(i)) {
        if (later.commonId == entry.commonId) values.add(later.value);
      }
      copyCommonHeaderTo(out, ids, entry.commonId, entry.name, values.asPtr().asConst());
      counts[entry.commonId] = 0;
    }
  }

  if (uncommonCount == 0) return;

  // As in shallowCopyTo(), the other headers go out sorted by name, every value under the name the
  // header first arrived with.
  struct Entry {
    kj::StringPtr name;
    kj::StringPtr value;
  };
  kj::Vector<Entry> entries(uncommonCount);
  for (auto& entry: native.entries) {
    if (entry.commonId == 0) entries.add(Entry{.name = entry.name, .value = entry.value});
  }
  if (entries.size() > 1) {
    // Group the entries by key, keeping arrival order within each group, and give every entry its
    // group's first name.
    auto byKey = kj::heapArray<size_t>(entries.size());
    for (auto i: kj::indices(byKey)) byKey[i] = i;
    std::stable_sort(byKey.begin(), byKey.end(),
        [&](size_t a, size_t b) { return caseInsensitiveLess(entries[a].name, entries[b].name); });
    kj::StringPtr groupName = entries[byKey[0]].name;
    for (auto i: kj::indices(byKey)) {
      auto& entry = entries[byKey[i]];
      if (i > 0 && caseInsensitiveLess(entries[byKey[i - 1]].name, entry.name)) {
        groupName = entry.name;
      }
      entry.name = groupName;
    }
    std::stable_sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) { return a.name < b.name; });
  }
  for (auto& entry: entries) {
    out.addPtrPtr(entry.name, entry.value);
  }
}

kj::Array<Headers::DisplayedHeader> Headers::getDisplayedHeaders(jsg::Lock& js) {
  materialize();
  auto getSetCookie = FeatureFlags::get(js).getHttpHeadersGetSetCookie();

  size_t reserved = 0;

  for (kj::uint i = 1; i < commonHeaders.size(); i++) {
    KJ_IF_SOME(header, commonHeaders[i]) {
      if (getSetCookie && i == static_cast<uint>(capnp::CommonHeaderName::SET_COOKIE)) {
        reserved += header->values.size();
      } else {
        reserved += 1;
      }
    }
  }
  for (auto& header: uncommonHeaders) {
    reserved += header.value->values.size();
  }
  kj::Vector<Headers::DisplayedHeader> vec(reserved);

  for (kj::uint i = 1; i < commonHeaders.size(); i++) {
    auto name = getCommonHeaderName(i);
    KJ_IF_SOME(header, commonHeaders[i]) {
      if (getSetCookie && i == static_cast<uint>(capnp::CommonHeaderName::SET_COOKIE)) {
        for (auto& value: header->values) {
          vec.add(Headers::DisplayedHeader{
            .key = kj::str(name),
            .value = kj::str(value),
          });
        }
      } else {
        vec.add(Headers::DisplayedHeader{
          .key = kj::str(name),
          .value = kj::strArray(header->values, ", "),
        });
      }
    }
  }

  for (auto& header: uncommonHeaders) {
    vec.add(Headers::DisplayedHeader{
      .key = kj::str(header.key),
      .value = kj::strArray(header.value->values, ", "),
    });
  }

  auto ret = vec.releaseAsArray();
  std::sort(ret.begin(), ret.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
  return kj::mv(ret);
}

jsg::Ref<Headers> Headers::constructor(jsg::Lock& js, jsg::Optional<Initializer> init) {
  using StringDict = jsg::Dict<kj::String, kj::String>;

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
        // a Sequence<Sequence<kj::String>> (as in the StringPairs case
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
      KJ_CASE_ONEOF(pairs, StringPairs) {
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

kj::Maybe<kj::String> Headers::get(jsg::Lock& js, kj::String name) {
  return getPtr(js, name);
}

kj::Maybe<kj::String> Headers::getPtr(jsg::Lock& js, kj::StringPtr name) {
  KJ_IF_SOME(n, native) {
    return getNativeJoined(*n, getHeaderKeyFor(name));
  }
  return tryGetHeader(getHeaderKeyFor(name)).map([](Header& header) {
    return kj::strArray(header.values, ", ");
  });
}

kj::Maybe<kj::String> Headers::getCommon(jsg::Lock& js, capnp::CommonHeaderName idx) {
  kj::uint index = static_cast<kj::uint>(idx);
  KJ_DASSERT(index <= Headers::MAX_COMMON_HEADER_ID);
  KJ_IF_SOME(n, native) {
    return getNativeJoined(*n, index);
  }
  return commonHeaders[index].map([](auto& header) { return kj::strArray(header->values, ", "); });
}

kj::Array<kj::StringPtr> Headers::getSetCookie() {
  KJ_IF_SOME(n, native) {
    return getNative(*n, static_cast<kj::uint>(capnp::CommonHeaderName::SET_COOKIE))
        .releaseAsArray();
  }
  auto& header = commonHeaders[static_cast<kj::uint>(capnp::CommonHeaderName::SET_COOKIE)];
  KJ_IF_SOME(h, header) {
    return KJ_MAP(value, h->values) { return value.asPtr(); };
  }
  return nullptr;
}

kj::Array<kj::StringPtr> Headers::getAll(kj::String name) {
  if (!strcaseeq("set-cookie"_kj, name)) {
    JSG_FAIL_REQUIRE(TypeError, "getAll() can only be used with the header name \"Set-Cookie\".");
  }

  // getSetCookie() is the standard API here. getAll(...) is our legacy non-standard extension
  // for the same use case. We continue to support getAll for backwards compatibility but moving
  // forward users really should be using getSetCookie.
  return getSetCookie();
}

bool Headers::has(kj::String name) {
  KJ_IF_SOME(n, native) {
    KJ_SWITCH_ONEOF(getHeaderKeyFor(name)) {
      KJ_CASE_ONEOF(id, kj::uint) {
        return hasCommon(static_cast<capnp::CommonHeaderName>(id));
      }
      KJ_CASE_ONEOF(lower, kj::String) {
        for (auto& entry: n->entries) {
          if (entry.commonId == 0 && strcaseeq(lower, entry.name)) return true;
        }
        return false;
      }
    }
    KJ_UNREACHABLE;
  }
  return tryGetHeader(getHeaderKeyFor(name)) != kj::none;
}

bool Headers::hasCommon(capnp::CommonHeaderName idx) {
  kj::uint index = static_cast<kj::uint>(idx);
  KJ_DASSERT(index <= Headers::MAX_COMMON_HEADER_ID);
  KJ_IF_SOME(n, native) {
    for (auto& entry: n->entries) {
      if (entry.commonId == index) return true;
    }
    return false;
  }
  return commonHeaders[index] != kj::none;
}

void Headers::set(jsg::Lock& js, kj::String name, kj::String value) {
  checkGuard();
  value = normalizeHeaderValue(name, kj::mv(value));
  setUnguarded(js, kj::mv(name), kj::mv(value));
}

void Headers::setUnguarded(jsg::Lock& js, kj::String name, kj::String value) {
  materialize();
  KJ_SWITCH_ONEOF(getHeaderKeyFor(name)) {
    KJ_CASE_ONEOF(id, kj::uint) {
      KJ_IF_SOME(existing, commonHeaders[id]) {
        existing->values.resize(1);
        existing->values[0] = kj::mv(value);
      } else {
        auto& created = commonHeaders[id].emplace(kj::heap(Header()));
        if (name != getCommonHeaderName(id)) {
          created->name = kj::mv(name);
        }
        created->values.resize(1);
        created->values[0] = kj::mv(value);
      }
      return;
    }
    KJ_CASE_ONEOF(n, kj::String) {
      using Ret = decltype(uncommonHeaders)::Entry;
      auto& header = uncommonHeaders.findOrCreate(n, [&] -> Ret {
        kj::Maybe<kj::String> maybeName;
        if (name != n) {
          maybeName = kj::mv(name);
        }
        return Ret{
          .key = kj::mv(n),
          .value = kj::heap(Header(kj::mv(maybeName))),
        };
      });
      header->values.resize(1);
      header->values[0] = kj::mv(value);
      return;
    }
  }
  KJ_UNREACHABLE;
}

kj::String Headers::normalizeCommon(capnp::CommonHeaderName idx, kj::String value) {
  return normalizeHeaderValue(getCommonHeaderName(static_cast<kj::uint>(idx)), kj::mv(value));
}

void Headers::setCommon(capnp::CommonHeaderName idx, kj::String value) {
  kj::uint index = static_cast<kj::uint>(idx);
  value = normalizeHeaderValue(getCommonHeaderName(index), kj::mv(value));
  materialize();
  KJ_IF_SOME(existing, commonHeaders[index]) {
    existing->values.resize(1);
    existing->values[0] = kj::mv(value);
  } else {
    auto& created = commonHeaders[index].emplace(kj::heap(Header()));
    created->values.resize(1);
    created->values[0] = kj::mv(value);
  }
}

void Headers::append(jsg::Lock& js, kj::String name, kj::String value) {
  checkGuard();
  value = normalizeHeaderValue(name, kj::mv(value));
  appendUnguarded(js, kj::mv(name), kj::mv(value));
}

void Headers::appendUnguarded(jsg::Lock& js, kj::String name, kj::String value) {
  materialize();
  KJ_SWITCH_ONEOF(getHeaderKeyFor(name)) {
    KJ_CASE_ONEOF(id, kj::uint) {
      KJ_IF_SOME(existing, commonHeaders[id]) {
        existing->values.add(kj::mv(value));
      } else {
        auto& created = commonHeaders[id].emplace(kj::heap(Header()));
        if (name != getCommonHeaderName(id)) {
          created->name = kj::mv(name);
        }
        created->values.resize(1);
        created->values[0] = kj::mv(value);
      }
      return;
    }
    KJ_CASE_ONEOF(n, kj::String) {
      appendUncommonUnguarded(kj::mv(n), kj::mv(name), kj::mv(value));
      return;
    }
  }
  KJ_UNREACHABLE;
}

void Headers::appendUncommonUnguarded(kj::String key, kj::String name, kj::String value) {
  KJ_IF_SOME(existing, uncommonHeaders.find(key)) {
    existing->values.add(kj::mv(value));
  } else {
    kj::Maybe<kj::String> maybeName;
    if (name != key) {
      maybeName = kj::mv(name);
    }
    auto header = kj::heap(Header(kj::mv(maybeName)));
    header->values.add(kj::mv(value));
    uncommonHeaders.insert(kj::mv(key), kj::mv(header));
  }
}

void Headers::appendCommonUnguarded(kj::uint id, kj::StringPtr name, kj::String value) {
  KJ_IF_SOME(existing, commonHeaders[id]) {
    existing->values.add(kj::mv(value));
  } else {
    auto& created = commonHeaders[id].emplace(kj::heap(Header()));
    if (name != getCommonHeaderName(id)) {
      created->name = kj::str(name);
    }
    created->values.resize(1);
    created->values[0] = kj::mv(value);
  }
}

void Headers::delete_(kj::String name) {
  checkGuard();
  materialize();
  KJ_SWITCH_ONEOF(getHeaderKeyFor(name)) {
    KJ_CASE_ONEOF(id, kj::uint) {
      commonHeaders[id] = kj::none;
      return;
    }
    KJ_CASE_ONEOF(n, kj::String) {
      uncommonHeaders.erase(n);
      return;
    }
  }
  KJ_UNREACHABLE;
}

void Headers::deleteCommon(capnp::CommonHeaderName idx) {
  kj::uint index = static_cast<kj::uint>(idx);
  KJ_DASSERT(index <= Headers::MAX_COMMON_HEADER_ID);
  materialize();
  commonHeaders[index] = kj::none;
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
  auto headers = getDisplayedHeaders(js);
  kj::Vector<kj::String> keys(headers.size());
  for (auto& header: headers) {
    keys.add(kj::mv(header.key));
  };
  return js.alloc<KeyIterator>(IteratorState<kj::String>(keys.releaseAsArray()));
}
jsg::Ref<Headers::ValueIterator> Headers::values(jsg::Lock& js) {
  // Annoyingly, the spec requires that the values iterator still be sorted by key.
  // To make this easiest, let's grab the displayed headers and then extract the values.
  // the getDisplayedHeaders() function does the sorting for us at the cost of an extra
  // copy of the names. Fortunately, enumerating by value is likely way less common than
  // other forms of iteration so the cost should be acceptable.
  auto headers = getDisplayedHeaders(js);
  kj::Vector<kj::String> values(headers.size());
  for (auto& header: headers) {
    values.add(kj::mv(header.value));
  };
  return js.alloc<ValueIterator>(IteratorState<kj::String>(values.releaseAsArray()));
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
  KJ_IF_SOME(n, native) {
    tracker.trackFieldWithSize(
        "native", n->chars.size() + n->entries.size() * sizeof(NativeHeaders::Entry));
  }
  for (const auto& header: commonHeaders) {
    tracker.trackField("header", header);
  }
  for (const auto& header: uncommonHeaders) {
    tracker.trackField(nullptr, header.value);
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

  materialize();
  serializer.writeRawUint32(static_cast<uint>(guard));

  // Write the count of headers.
  uint count = 0;
  for (auto& header: commonHeaders) {
    KJ_IF_SOME(h, header) {
      count += h->values.size();
    }
  }
  for (auto& header: uncommonHeaders) {
    count += header.value->values.size();
  }
  serializer.writeRawUint32(count);

  // Now write key/values.
  for (kj::uint i = 1; i < commonHeaders.size(); i++) {
    KJ_IF_SOME(header, commonHeaders[i]) {
      for (auto& value: header->values) {
        serializer.writeRawUint32(i);
        serializer.writeLengthDelimited(value);
      }
    }
  }
  for (auto& header: uncommonHeaders) {
    auto name = ([&] -> kj::StringPtr {
      KJ_IF_SOME(name, header.value->name) {
        return name;
      } else {
        return header.key;
      }
    })();
    for (auto& value: header.value->values) {
      serializer.writeRawUint32(0);
      serializer.writeLengthDelimited(name);
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
  JSG_REQUIRE(count <= 1024, DOMDataCloneError,
      "Deserialization failed: header count exceeds maximum (", count, ")");

  for (auto i KJ_UNUSED: kj::zeroTo(count)) {
    uint commonId = deserializer.readRawUint32();
    kj::String name;
    if (commonId == 0) {
      name = deserializer.readLengthDelimitedString();
    } else {
      KJ_ASSERT(commonId <= Headers::MAX_COMMON_HEADER_ID);
      name = kj::str(getCommonHeaderName(commonId));
    }

    auto value = deserializer.readLengthDelimitedString();

    // TODO(performance): We can avoid some copies here by constructing the
    // the Header entry directly using information from the deserializer
    // directly without relying on append.
    result->appendUnguarded(js, kj::mv(name), kj::mv(value));
  }

  // Don't actually set the guard until here because it may block the ability to call `append()`.
  result->guard = static_cast<Guard>(guard);

  return result;
}

}  // namespace workerd::api
