#include "headers.h"

#include "util.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/util/header-validation.h>
#include <workerd/util/strings.h>

namespace workerd::api {

namespace {
// If any more headers are added to the CommonHeaderName enum later, we should be careful about
// introducing them into serialization. We need to roll out a change that recognizes the new IDs
// before rolling out a change that sends them. MAX_COMMON_HEADER_ID is the max value we're willing
// to send.
constexpr size_t MAX_COMMON_HEADER_ID =
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
//
// TODO(perf): We can potentially optimize this further by using the mechanisms within
// http-over-capnp, which also has a mapping of common header names to kj::HttpHeaderIds.
// However, accessing that functionality requires some amount of new API to be added to
// capnproto which needs to be carefully weighed. There's also the fact that, currently,
// the HttpOverCapnpFactory is accessed via IoContext and the Headers object can be
// created outside of an IoContext. Some amount of additional refactoring would be needed
// to make it work. For now, this hardcoded table is sufficient and efficient enough.
#define V(Name) Name##_kj,
constexpr kj::StringPtr COMMON_HEADER_NAMES[] = {nullptr,  // 0: invalid
  COMMON_HEADERS(V)};
#undef V

inline constexpr kj::StringPtr getCommonHeaderName(uint id) {
  KJ_ASSERT(id > 0 && id <= MAX_COMMON_HEADER_ID, "Invalid common header ID");
  return COMMON_HEADER_NAMES[id];
}

constexpr bool strcaseeq(kj::StringPtr a, kj::StringPtr b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    // Convert to lowercase for comparison
    if ('A' <= ca && ca <= 'Z') ca += 32;
    if ('A' <= cb && cb <= 'Z') cb += 32;
    if (ca != cb) return false;
  }
  return true;
}

constexpr uint caseInsensitiveHash(kj::StringPtr name) {
  uint hash = 2166136261u;
  for (size_t i = 0; i < name.size(); ++i) {
    char c = name[i];
    if ('A' <= c && c <= 'Z') c += 32;
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  return hash;
}

constexpr size_t HEADER_MAP_SIZE = 128;

// Constexpr hash table for case-insensitive mapping of header names to their
// common header id (if any).
struct HeaderHashTable final {
  struct Entry {
    kj::StringPtr name;
    uint id;
  };

  Entry entries[HEADER_MAP_SIZE] = {};

  constexpr HeaderHashTable() {
    for (size_t i = 0; i < HEADER_MAP_SIZE; ++i) {
      entries[i] = {nullptr, 0};
    }

    for (uint i = 1; i <= MAX_COMMON_HEADER_ID; ++i) {
      auto name = COMMON_HEADER_NAMES[i];
      size_t slot = caseInsensitiveHash(name) % HEADER_MAP_SIZE;
      while (entries[slot].id != 0) {
        slot = (slot + 1) % HEADER_MAP_SIZE;
      }
      entries[slot] = {name, i};
    }
  }

  constexpr uint find(kj::StringPtr name) const {
    if (name == nullptr) return 0;

    size_t slot = caseInsensitiveHash(name) % HEADER_MAP_SIZE;

    // Linear probe until we find a match or empty slot
    for (size_t probes = 0; probes < HEADER_MAP_SIZE; ++probes) {
      const auto& entry = entries[slot];
      if (entry.id == 0) return 0;
      if (entry.name.size() == name.size() && strcaseeq(entry.name, name)) {
        return entry.id;
      }
      slot = (slot + 1) % HEADER_MAP_SIZE;
    }
    return 0;  // Not found
  }
};

constexpr HeaderHashTable HEADER_HASH_TABLE;
// Quick check to verify that the hash table is constructed correctly.
static_assert(HEADER_HASH_TABLE.find("accept-charset"_kj) == 1);
static_assert(HEADER_HASH_TABLE.find("AcCePt-ChArSeT"_kj) == 1);

static_assert(std::size(COMMON_HEADER_NAMES) == (MAX_COMMON_HEADER_ID + 1));

inline constexpr void requireValidHeaderName(const jsg::ByteString& name) {
  for (char c: name) {
    JSG_REQUIRE(util::isHttpTokenChar(c), TypeError, "Invalid header name.");
  }
}

// Left- and right-trim HTTP whitespace from `value`.
kj::String normalizeHeaderValue(jsg::Lock& js, jsg::ByteString value) {
  JSG_REQUIRE(workerd::util::isValidHeaderValue(value), TypeError, "Invalid header value.");
  // Fast path: if empty, return as-is
  if (value.size() == 0) return kj::mv(value);

  char* begin = value.begin();
  char* end = value.end();

  while (begin < end && util::isHttpWhitespace(*begin)) ++begin;
  while (begin < end && util::isHttpWhitespace(*(end - 1))) --end;

  size_t newSize = end - begin;
  if (newSize == value.size()) return kj::mv(value);

  return kj::str(kj::ArrayPtr(begin, newSize));
}

constexpr bool isSetCookie(const Headers::HeaderKey& key) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(commonId, uint) {
      return commonId == static_cast<uint>(capnp::CommonHeaderName::SET_COOKIE);
    }
    KJ_CASE_ONEOF(uncommonKey, kj::String) {
      // This case really shouldn't happen since "set-cookie" is a common header,
      // but just in case...
      return uncommonKey == "set-cookie";
    }
  }
  KJ_UNREACHABLE;
}

constexpr Headers::HeaderKey getHeaderKeyFor(kj::StringPtr name) {
  if (uint commonId = HEADER_HASH_TABLE.find(name)) {
    KJ_DASSERT(commonId > 0 && commonId <= MAX_COMMON_HEADER_ID);
    return commonId;
  }

  // Not a common header, so allocate lowercase copy for uncommon header
  return toLower(name);
}

constexpr Headers::HeaderKey cloneHeaderKey(const Headers::HeaderKey& key) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(commonId, uint) {
      return commonId;
    }
    KJ_CASE_ONEOF(uncommonKey, kj::String) {
      return kj::str(uncommonKey);
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

Headers::Header::Header(HeaderKey key, kj::Maybe<kj::String> name, kj::Vector<kj::String> values)
    : key(kj::mv(key)),
      name(kj::mv(name)),
      values(kj::mv(values)) {}

kj::StringPtr Headers::Header::Header::getKeyName() const {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(commonId, uint) {
      return COMMON_HEADER_NAMES[commonId];
    }
    KJ_CASE_ONEOF(uncommonKey, kj::String) {
      return uncommonKey;
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
  return Header(cloneHeaderKey(key), name.map([](const kj::String& n) { return kj::str(n); }),
      KJ_MAP(value, values) { return kj::str(value); });
}

bool Headers::HeaderCallbacks::matches(Header& header, const HeaderKey& other) {
  return header.key == other;
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
    KJ_CASE_ONEOF(uncommonKey, kj::String) {
      return kj::hashCode(uncommonKey);
    }
  }
  KJ_UNREACHABLE;
}

kj::uint Headers::HeaderCallbacks::hashCode(capnp::CommonHeaderName commondId) {
  return kj::hashCode(commondId);
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
    appendUnguarded(js, kj::str(name), kj::str(value));
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
            .key = kj::str(header.getKeyName()),
            .value = kj::str(value),
          });
        }
      } else {
        vec.add(Headers::DisplayedHeader{
          .key = kj::str(header.getKeyName()),
          .value = kj::strArray(header.values, ", "),
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
        .key = kj::str(header.getKeyName()),
        .value = kj::strArray(header.values, ", "),
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

kj::Maybe<kj::String> Headers::get(jsg::Lock& js, jsg::ByteString name) {
  requireValidHeaderName(name);
  return getUnguarded(js, name.asPtr());
}

kj::Maybe<kj::String> Headers::getUnguarded(jsg::Lock&, kj::StringPtr name) {
  KJ_IF_SOME(found, headers.find(getHeaderKeyFor(name))) {
    return kj::strArray(found.values, ", ");
  }
  return kj::none;
}

kj::Maybe<kj::String> Headers::getCommon(jsg::Lock& js, capnp::CommonHeaderName idx) {
  KJ_DASSERT(static_cast<size_t>(idx) <= MAX_COMMON_HEADER_ID);
  KJ_IF_SOME(found, headers.find(idx)) {
    return kj::strArray(found.values, ", ");
  }
  return kj::none;
}

kj::Array<kj::StringPtr> Headers::getSetCookie() {
  KJ_IF_SOME(found, headers.find(capnp::CommonHeaderName::SET_COOKIE)) {
    return KJ_MAP(value, found.values) { return value.asPtr(); };
  }
  return nullptr;
}

kj::Array<kj::StringPtr> Headers::getAll(jsg::ByteString name) {
  requireValidHeaderName(name);

  if (!strcaseeq(name, "set-cookie"_kj)) {
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
  setUnguarded(js, kj::mv(name), normalizeHeaderValue(js, kj::mv(value)));
}

void Headers::setUnguarded(jsg::Lock& js, kj::String name, kj::String value) {
  auto key = getHeaderKeyFor(name);
  auto& header = headers.findOrCreate(key, [&]() {
    Header header(kj::mv(key));
    if (header.getHeaderName() != name) {
      header.name = kj::mv(name);
    }
    return kj::mv(header);
  });
  header.values.clear();
  header.values.add(kj::mv(value));
}

void Headers::setCommon(capnp::CommonHeaderName idx, kj::String value) {
  KJ_DASSERT(static_cast<size_t>(idx) <= MAX_COMMON_HEADER_ID);
  HeaderKey key = static_cast<uint>(idx);
  auto& header = headers.findOrCreate(key, [&]() { return Header(kj::mv(key)); });
  header.values.clear();
  header.values.add(kj::mv(value));
}

void Headers::append(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value) {
  checkGuard();
  requireValidHeaderName(name);
  appendUnguarded(js, kj::mv(name), normalizeHeaderValue(js, kj::mv(value)));
}

void Headers::appendUnguarded(jsg::Lock& js, kj::String name, kj::String value) {
  auto key = getHeaderKeyFor(name);
  auto& header = headers.findOrCreate(key, [&]() {
    Header header(kj::mv(key));
    if (header.getHeaderName() != name) {
      header.name = kj::mv(name);
    }
    return kj::mv(header);
  });
  header.values.add(kj::mv(value));
}

void Headers::delete_(jsg::ByteString name) {
  checkGuard();
  requireValidHeaderName(name);
  headers.eraseMatch(getHeaderKeyFor(name));
}

void Headers::deleteCommon(capnp::CommonHeaderName idx) {
  headers.eraseMatch(idx);
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
    kj::Vector<kj::String> keysCopy;
    for (auto& header: headers) {
      // Set-Cookie headers must be handled specially. They should never be combined into a
      // single value, so the values iterator must separate them. It seems a bit silly, but
      // the keys iterator can end up having multiple set-cookie instances.
      if (isSetCookie(header.key)) {
        auto values = getSetCookie();
        for (auto n = 0; n < values.size(); n++) {
          keysCopy.add(kj::str(header.getKeyName()));
        }
      } else {
        keysCopy.add(kj::str(header.getKeyName()));
      }
    }
    auto ret = keysCopy.releaseAsArray();
    std::sort(ret.begin(), ret.end(), [](const auto& a, const auto& b) { return a < b; });
    return js.alloc<KeyIterator>(IteratorState<kj::String>{kj::mv(ret)});
  } else {
    auto keysCopy = KJ_MAP(header, headers) { return kj::str(header.getKeyName()); };
    std::sort(keysCopy.begin(), keysCopy.end(), [](const auto& a, const auto& b) { return a < b; });
    return js.alloc<KeyIterator>(IteratorState<kj::String>{kj::mv(keysCopy)});
  }
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
        KJ_CASE_ONEOF(_, kj::String) {
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
    result->appendUnguarded(js, kj::mv(name), kj::mv(value));
  }

  // Don't actually set the guard until here because it may block the ability to call `append()`.
  result->guard = static_cast<Guard>(guard);

  return result;
}

}  // namespace workerd::api
