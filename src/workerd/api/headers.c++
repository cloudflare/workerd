#include "headers.h"

#include "util.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/util/http-util.h>
#include <workerd/util/strings.h>

#include <kj/parse/char.h>

#include <set>

namespace workerd::api {

namespace {
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

// Left- and right-trim HTTP whitespace from `value`.
jsg::ByteString normalizeHeaderValue(jsg::Lock& js, jsg::ByteString value) {
  warnIfBadHeaderString(value);

  kj::ArrayPtr<char> slice = value;
  auto isHttpWhitespace = [](char c) { return c == '\t' || c == '\r' || c == '\n' || c == ' '; };
  while (slice.size() > 0 && isHttpWhitespace(slice.front())) {
    slice = slice.slice(1, slice.size());
  }
  while (slice.size() > 0 && isHttpWhitespace(slice.back())) {
    slice = slice.first(slice.size() - 1);
  }
  if (slice.size() == value.size()) {
    return kj::mv(value);
  }
  return jsg::ByteString(kj::str(slice));
}

void requireValidHeaderName(const jsg::ByteString& name) {
  // TODO(cleanup): Code duplication with kj/compat/http.c++

  warnIfBadHeaderString(name);

  constexpr auto HTTP_SEPARATOR_CHARS = kj::parse::anyOfChars("()<>@,;:\\\"/[]?={} \t");
  // RFC2616 section 2.2: https://www.w3.org/Protocols/rfc2616/rfc2616-sec2.html#sec2.2

  constexpr auto HTTP_TOKEN_CHARS = kj::parse::controlChar.orChar('\x7f')
                                        .orGroup(kj::parse::whitespaceChar)
                                        .orGroup(HTTP_SEPARATOR_CHARS)
                                        .invert();
  // RFC2616 section 2.2: https://www.w3.org/Protocols/rfc2616/rfc2616-sec2.html#sec2.2
  // RFC2616 section 4.2: https://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.2

  for (char c: name) {
    JSG_REQUIRE(HTTP_TOKEN_CHARS.contains(c), TypeError, "Invalid header name.");
  }
}

void requireValidHeaderValue(kj::StringPtr value) {
  // TODO(cleanup): Code duplication with kj/compat/http.c++

  for (char c: value) {
    JSG_REQUIRE(c != '\0' && c != '\r' && c != '\n', TypeError, "Invalid header value.");
  }
}

// If any more headers are added to the CommonHeaderName enum later, we should be careful about
// introducing them into serialization. We need to roll out a change that recognizes the new IDs
// before rolling out a change that sends them. MAX_COMMON_HEADER_ID is the max value we're willing
// to send.
static constexpr size_t MAX_COMMON_HEADER_ID =
    static_cast<size_t>(capnp::CommonHeaderName::WWW_AUTHENTICATE);

// Constexpr array of lowercase common header names (must match CommonHeaderName enum order
// and must be kept in sync with the ordinal values defined in http-over-capnp.capnp). Since
// it is extremely unlikely that those will change often, we hardcode them here for runtime
// efficiency.
static constexpr const char* COMMON_HEADER_NAMES[] = {
  nullptr,                        // 0: invalid
  "accept-charset",               // 1
  "accept-encoding",              // 2
  "accept-language",              // 3
  "accept-ranges",                // 4
  "accept",                       // 5
  "access-control-allow-origin",  // 6
  "age",                          // 7
  "allow",                        // 8
  "authorization",                // 9
  "cache-control",                // 10
  "content-disposition",          // 11
  "content-encoding",             // 12
  "content-language",             // 13
  "content-length",               // 14
  "content-location",             // 15
  "content-range",                // 16
  "content-type",                 // 17
  "cookie",                       // 18
  "date",                         // 19
  "etag",                         // 20
  "expect",                       // 21
  "expires",                      // 22
  "from",                         // 23
  "host",                         // 24
  "if-match",                     // 25
  "if-modified-since",            // 26
  "if-none-match",                // 27
  "if-range",                     // 28
  "if-unmodified-since",          // 29
  "last-modified",                // 30
  "link",                         // 31
  "location",                     // 32
  "max-forwards",                 // 33
  "proxy-authenticate",           // 34
  "proxy-authorization",          // 35
  "range",                        // 36
  "referer",                      // 37
  "refresh",                      // 38
  "retry-after",                  // 39
  "server",                       // 40
  "set-cookie",                   // 41
  "strict-transport-security",    // 42
  "transfer-encoding",            // 43
  "user-agent",                   // 44
  "vary",                         // 45
  "via",                          // 46
  "www-authenticate",             // 47
};

kj::String getCommonHeaderName(uint id) {
  KJ_ASSERT(id > 0 && id <= MAX_COMMON_HEADER_ID, "Invalid common header ID");
  auto name = COMMON_HEADER_NAMES[id];
  KJ_DASSERT(name != nullptr);
  return kj::str(name);
}

kj::Maybe<uint> getCommonHeaderId(kj::StringPtr name) {
  // It really shouldn't be possible for a name to be empty but just in case...
  if (name.size() == 0) return kj::none;
  for (uint i = 1; i <= MAX_COMMON_HEADER_ID; ++i) {
    KJ_DASSERT(COMMON_HEADER_NAMES[i] != nullptr);
    if (name == COMMON_HEADER_NAMES[i]) return i;
  }
  return kj::none;
}

static_assert(std::size(COMMON_HEADER_NAMES) == (MAX_COMMON_HEADER_ID + 1));
}  // namespace

Headers::Headers(jsg::Lock& js, jsg::Dict<jsg::ByteString, jsg::ByteString> dict)
    : guard(Guard::NONE) {
  for (auto& field: dict.fields) {
    append(js, kj::mv(field.name), kj::mv(field.value));
  }
}

Headers::Headers(jsg::Lock& js, const Headers& other): guard(Guard::NONE) {
  for (auto& header: other.headers) {
    Header copy{
      jsg::ByteString(kj::str(header.second.key)),
      jsg::ByteString(kj::str(header.second.name)),
      KJ_MAP(value, header.second.values) { return jsg::ByteString(kj::str(value)); },
    };
    kj::StringPtr keyRef = copy.key;
    KJ_ASSERT(headers.insert(std::make_pair(keyRef, kj::mv(copy))).second);
  }
}

Headers::Headers(jsg::Lock& js, const kj::HttpHeaders& other, Guard guard): guard(Guard::NONE) {
  // TODO(soon): Remove this. Throw if the any header values are invalid.
  throwIfInvalidHeaderValue(other);
  other.forEach([this, &js](auto name, auto value) {
    append(js, jsg::ByteString(kj::str(name)), jsg::ByteString(kj::str(value)));
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
    for (auto& value: entry.second.values) {
      out.addPtrPtr(entry.second.name, value);
    }
  }
}

bool Headers::hasLowerCase(kj::StringPtr name) {
#ifdef KJ_DEBUG
  for (auto c: name) {
    KJ_DREQUIRE(!('A' <= c && c <= 'Z'));
  }
#endif
  return headers.contains(name);
}

kj::Array<Headers::DisplayedHeader> Headers::getDisplayedHeaders(jsg::Lock& js) {
  if (FeatureFlags::get(js).getHttpHeadersGetSetCookie()) {
    kj::Vector<Headers::DisplayedHeader> copy;
    for (auto& entry: headers) {
      if (entry.first == "set-cookie") {
        // For set-cookie entries, we iterate each individually without
        // combining them.
        for (auto& value: entry.second.values) {
          copy.add(Headers::DisplayedHeader{
            .key = jsg::ByteString(kj::str(entry.first)),
            .value = jsg::ByteString(kj::str(value)),
          });
        }
      } else {
        copy.add(Headers::DisplayedHeader{.key = jsg::ByteString(kj::str(entry.first)),
          .value = jsg::ByteString(kj::strArray(entry.second.values, ", "))});
      }
    }
    return copy.releaseAsArray();
  } else {
    // The old behavior before the standard getSetCookie() API was introduced...
    auto headersCopy = KJ_MAP(mapEntry, headers) {
      const auto& header = mapEntry.second;
      return DisplayedHeader{
        jsg::ByteString(kj::str(header.key)), jsg::ByteString(kj::strArray(header.values, ", "))};
    };
    return headersCopy;
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

kj::Maybe<jsg::ByteString> Headers::getNoChecks(jsg::Lock& js, kj::StringPtr name) {
  auto iter = headers.find(toLower(name));
  if (iter == headers.end()) {
    return kj::none;
  } else {
    return jsg::ByteString(kj::strArray(iter->second.values, ", "));
  }
}

kj::ArrayPtr<jsg::ByteString> Headers::getSetCookie() {
  auto iter = headers.find("set-cookie");
  if (iter == headers.end()) {
    return nullptr;
  } else {
    return iter->second.values.asPtr();
  }
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
  return headers.contains(toLower(kj::mv(name)));
}

void Headers::set(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value) {
  checkGuard();
  requireValidHeaderName(name);
  value = normalizeHeaderValue(js, kj::mv(value));
  requireValidHeaderValue(value);
  setUnguarded(js, kj::mv(name), kj::mv(value));
}

void Headers::setUnguarded(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value) {
  // The variation of toLower we use here creates a copy.
  auto key = jsg::ByteString(toLower(name));
  auto [iter, emplaced] = headers.try_emplace(key, kj::mv(key), kj::mv(name), kj::mv(value));
  if (!emplaced) {
    // Overwrite existing value(s).
    iter->second.values.clear();
    iter->second.values.add(kj::mv(value));
  }
}

void Headers::append(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value) {
  checkGuard();
  requireValidHeaderName(name);
  // The variation of toLower we use here creates a copy.
  auto key = jsg::ByteString(toLower(name));
  value = normalizeHeaderValue(js, kj::mv(value));
  requireValidHeaderValue(value);
  auto [iter, emplaced] = headers.try_emplace(key, kj::mv(key), kj::mv(name), kj::mv(value));
  if (!emplaced) {
    iter->second.values.add(kj::mv(value));
  }
}

void Headers::delete_(jsg::ByteString name) {
  checkGuard();
  requireValidHeaderName(name);
  headers.erase(toLower(kj::mv(name)));
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
    for (auto& entry: headers) {
      // Set-Cookie headers must be handled specially. They should never be combined into a
      // single value, so the values iterator must separate them. It seems a bit silly, but
      // the keys iterator can end up having multiple set-cookie instances.
      if (entry.first == "set-cookie") {
        for (auto n = 0; n < entry.second.values.size(); n++) {
          keysCopy.add(jsg::ByteString(kj::str(entry.first)));
        }
      } else {
        keysCopy.add(jsg::ByteString(kj::str(entry.first)));
      }
    }
    return js.alloc<KeyIterator>(IteratorState<jsg::ByteString>{keysCopy.releaseAsArray()});
  } else {
    auto keysCopy =
        KJ_MAP(mapEntry, headers) { return jsg::ByteString(kj::str(mapEntry.second.key)); };
    return js.alloc<KeyIterator>(IteratorState<jsg::ByteString>{kj::mv(keysCopy)});
  }
}
jsg::Ref<Headers::ValueIterator> Headers::values(jsg::Lock& js) {
  if (FeatureFlags::get(js).getHttpHeadersGetSetCookie()) {
    kj::Vector<jsg::ByteString> values;
    for (auto& entry: headers) {
      // Set-Cookie headers must be handled specially. They should never be combined into a
      // single value, so the values iterator must separate them.
      if (entry.first == "set-cookie") {
        for (auto& value: entry.second.values) {
          values.add(jsg::ByteString(kj::str(value)));
        }
      } else {
        values.add(jsg::ByteString(kj::strArray(entry.second.values, ", ")));
      }
    }
    return js.alloc<ValueIterator>(IteratorState<jsg::ByteString>{values.releaseAsArray()});
  } else {
    auto valuesCopy = KJ_MAP(mapEntry, headers) {
      return jsg::ByteString(kj::strArray(mapEntry.second.values, ", "));
    };
    return js.alloc<ValueIterator>(IteratorState<jsg::ByteString>{kj::mv(valuesCopy)});
  }
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
    count += entry.second.values.size();
  }
  serializer.writeRawUint32(count);

  // Now write key/values.
  for (auto& entry: headers) {
    auto& header = entry.second;
    auto commonId = getCommonHeaderId(header.key);
    for (auto& value: header.values) {
      KJ_IF_SOME(c, commonId) {
        serializer.writeRawUint32(c);
      } else {
        serializer.writeRawUint32(0);
        serializer.writeLengthDelimited(header.name);
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
      name = getCommonHeaderName(commonId);
    }

    auto value = deserializer.readLengthDelimitedString();

    result->append(js, jsg::ByteString(kj::mv(name)), jsg::ByteString(kj::mv(value)));
  }

  // Don't actually set the guard until here because it may block the ability to call `append()`.
  result->guard = static_cast<Guard>(guard);

  return result;
}

}  // namespace workerd::api
