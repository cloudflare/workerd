#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/memory.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/worker-interface.capnp.h>
#include <kj/compat/http.h>

namespace workerd::api {

class Headers final: public jsg::Object {
private:
  template <typename T>
  struct IteratorState {
    kj::Array<T> copy;
    decltype(copy.begin()) cursor = copy.begin();
  };

public:
  enum class Guard {
    // WARNING: This type is serialized, do not change the numeric values.
    IMMUTABLE = 0,
    REQUEST = 1,
    // REQUEST_NO_CORS,  // CORS not relevant on server side
    RESPONSE = 2,
    NONE = 3
  };

  struct DisplayedHeader {
    jsg::ByteString key;   // lower-cased name
    jsg::ByteString value; // comma-concatenation of all values seen
  };

  Headers(): guard(Guard::NONE) {}
  explicit Headers(jsg::Lock& js, jsg::Dict<jsg::ByteString, jsg::ByteString> dict);
  explicit Headers(jsg::Lock& js, const Headers& other);
  explicit Headers(jsg::Lock& js, const kj::HttpHeaders& other, Guard guard);
  KJ_DISALLOW_COPY_AND_MOVE(Headers);

  // Make a copy of this Headers object, and preserve the guard.
  jsg::Ref<Headers> clone(jsg::Lock& js) const;

  // Fill in the given HttpHeaders with these headers. Note that strings are inserted by
  // reference, so the output must be consumed immediately.
  void shallowCopyTo(kj::HttpHeaders& out);

  // Like has(), but only call this with an already-lower-case `name`. Useful to avoid an
  // unnecessary string allocation. Not part of the JS interface.
  bool hasLowerCase(kj::StringPtr name);

  // Returns headers with lower-case name and comma-concatenated duplicates.
  kj::Array<DisplayedHeader> getDisplayedHeaders(jsg::Lock& js);

  using ByteStringPair = jsg::Sequence<jsg::ByteString>;
  using ByteStringPairs = jsg::Sequence<ByteStringPair>;

  // Per the fetch specification, it is possible to initialize a Headers object
  // from any other object that has a Symbol.iterator implementation. Those are
  // handled in this Initializer definition using the ByteStringPairs definition
  // that aliases jsg::Sequence<jsg::Sequence<jsg::ByteString>>. Technically,
  // the Headers object itself falls under that definition as well. However, treating
  // a Headers object as a jsg::Sequence<jsg::Sequence<T>> is nowhere near as
  // performant and has the side effect of forcing all header names to be lower-cased
  // rather than case-preserved. Instead of following the spec exactly here, we
  // choose to special case creating a Header object from another Header object.
  // This is an intentional departure from the spec.
  using Initializer = kj::OneOf<jsg::Ref<Headers>,
                                ByteStringPairs,
                                jsg::Dict<jsg::ByteString, jsg::ByteString>>;

  static jsg::Ref<Headers> constructor(jsg::Lock& js, jsg::Optional<Initializer> init);
  kj::Maybe<jsg::ByteString> get(jsg::Lock& js, jsg::ByteString name);

  kj::Maybe<jsg::ByteString> getNoChecks(jsg::Lock& js, kj::StringPtr name);

  kj::Maybe<jsg::ByteString> getCommon(jsg::Lock& js, capnp::CommonHeaderName idx);

  // getAll is a legacy non-standard extension API that we introduced before
  // getSetCookie() was defined. We continue to support it for backwards
  // compatibility but users really ought to be using getSetCookie() now.
  kj::ArrayPtr<jsg::ByteString> getAll(jsg::ByteString name);

  // The Set-Cookie header is special in that it is the only HTTP header that
  // is not permitted to be combined into a single instance.
  kj::ArrayPtr<jsg::ByteString> getSetCookie();

  bool has(jsg::ByteString name);

  // A fast-patch version of has() for common (indexed) headers.
  bool hasCommon(capnp::CommonHeaderName idx);

  void set(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value);
  void setCommon(jsg::Lock& js, capnp::CommonHeaderName idx, jsg::ByteString value);

  // Like set(), but ignores the header guard if set. This can only be called from C++, and may be
  // used to mutate headers before dispatching a request.
  void setUnguarded(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value);

  void append(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value);
  void appendUnguarded(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value);

  void delete_(jsg::ByteString name);

  void forEach(jsg::Lock& js,
               jsg::Function<void(kj::StringPtr, kj::StringPtr, jsg::Ref<Headers>)>,
               jsg::Optional<jsg::Value>);

  bool inspectImmutable();

  JSG_ITERATOR(EntryIterator, entries,
                kj::Array<jsg::ByteString>,
                IteratorState<DisplayedHeader>,
                entryIteratorNext)
  JSG_ITERATOR(KeyIterator, keys,
                jsg::ByteString,
                IteratorState<jsg::ByteString>,
                keyOrValueIteratorNext)
  JSG_ITERATOR(ValueIterator, values,
                jsg::ByteString,
                IteratorState<jsg::ByteString>,
                keyOrValueIteratorNext)

  // JavaScript API.

  JSG_RESOURCE_TYPE(Headers, CompatibilityFlags::Reader flags) {
    JSG_METHOD(get);
    JSG_METHOD(getAll);
    if (flags.getHttpHeadersGetSetCookie()) {
      JSG_METHOD(getSetCookie);
    }
    JSG_METHOD(has);
    JSG_METHOD(set);
    JSG_METHOD(append);
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(forEach);
    JSG_METHOD(entries);
    JSG_METHOD(keys);
    JSG_METHOD(values);

    JSG_INSPECT_PROPERTY(immutable, inspectImmutable);

    JSG_ITERABLE(entries);

    JSG_TS_DEFINE(type HeadersInit = Headers | Iterable<Iterable<string>> | Record<string, string>);
    // All type aliases get inlined when exporting RTTI, but this type alias is included by
    // the official TypeScript types, so users might be depending on it.

    JSG_TS_OVERRIDE({
      constructor(init?: HeadersInit);

      entries(): IterableIterator<[key: string, value: string]>;
      [Symbol.iterator](): IterableIterator<[key: string, value: string]>;

      forEach<This = unknown>(callback: (this: This, value: string, key: string, parent: Headers) => void, thisArg?: This): void;
    });
  }

  void serialize(jsg::Lock& js, jsg::Serializer& serializer);
  static jsg::Ref<Headers> deserialize(
      jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer);

  JSG_SERIALIZABLE(rpc::SerializationTag::HEADERS);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  // The header key can be stored either as a common header ID (uint) or an an
  // UncommonHeaderKey. This class encapsulates the lower-cased header name and
  // precomputed hash code for efficient comparisons. It is used for any header
  // name that is not in the common header list.
  class UncommonHeaderKey final {
   public:
    UncommonHeaderKey(UncommonHeaderKey&&) = default;
    UncommonHeaderKey& operator=(UncommonHeaderKey&&) = default;
    KJ_DISALLOW_COPY(UncommonHeaderKey);

    inline bool operator==(const UncommonHeaderKey& other) const;
    inline bool operator==(kj::StringPtr otherName) const;

    inline uint hashCode() const { return hash; }
    inline kj::StringPtr getName() const { return name; }

    JSG_MEMORY_INFO(UncommonHeaderKey) {
      tracker.trackField("name", name);
    }

    inline UncommonHeaderKey clone() const {
      return UncommonHeaderKey(kj::str(name), hash);
    }

   private:
    // The name is expected to be stored in lower-case form, but we do not
    // enforce that actually within the struct. It is the responsibility of
    // the caller to ensure it is appropriately lower-cased.
    kj::String name;
    kj::uint hash;

    // Critically, because of hash collisions, we must still compare the full string
    // to determine equality but we can use the hash code to avoid unnecessary string
    // comparisons.
    inline UncommonHeaderKey(kj::String name);
    inline UncommonHeaderKey(kj::StringPtr name);
    inline UncommonHeaderKey(kj::String name, kj::uint hash): name(kj::mv(name)), hash(hash) {}
    friend class Headers;
  };

  // A header is identified by either a common header ID or an uncommon header name.
  // The header key name is always identifed in lower-case form, while the original
  // casing is preserved in the actual Header struct to support case-preserving display.
  using HeaderKey = kj::OneOf<uint, UncommonHeaderKey>;

  static HeaderKey getHeaderKeyFor(kj::StringPtr name);
  static bool headerKeyEquals(const HeaderKey& a, const HeaderKey& b);
  static HeaderKey cloneHeaderKey(const HeaderKey& key);
  static bool isSetCookie(const HeaderKey& key);

  struct Header final {
    // The header key, either a common header ID or an uncommon header name.
    HeaderKey key;
    // If the casing of the header name does not match the lower-cased version, we
    // store the original casing here for display purposes. If the casing matches, this
    // remains unset to avoid redundant allocation.
    kj::Maybe<jsg::ByteString> name;

    // We intentionally do not comma-concatenate header values of the same name, as we need to be
    // able to re-serialize them separately. This is particularly important for the Set-Cookie
    // header, which uses a date format that requires a comma. This would normally suggest using a
    // std::multimap, but we also need to be able to display the values in comma-concatenated form
    // via Headers.entries()[1] in order to be Fetch-conformant. Storing a vector of strings in a
    // std::map makes this easier, and also makes it easy to honor the "first header name casing is
    // used for all duplicate header names" rule[2] that the Fetch spec mandates.
    //
    // See: 1: https://fetch.spec.whatwg.org/#concept-header-list-sort-and-combine
    //      2: https://fetch.spec.whatwg.org/#concept-header-list-append
    kj::Vector<jsg::ByteString> values;

    kj::StringPtr getKeyName() const;

    // If the casing of the header name matches the lower-cased version, this
    // returns the key name, otherwise it returns the preserved-casing name.
    kj::StringPtr getHeaderName() const;

    explicit Header(jsg::ByteString name, kj::Vector<jsg::ByteString> values);
    explicit Header(jsg::ByteString name, jsg::ByteString value);
    explicit Header(HeaderKey key, kj::Maybe<jsg::ByteString> name,
                    kj::Vector<jsg::ByteString> values);
    explicit Header(HeaderKey key, jsg::ByteString name, jsg::ByteString value);

    Header clone() const;

    JSG_MEMORY_INFO(Header) {
      tracker.trackField("key", key.tryGet<UncommonHeaderKey>());
      tracker.trackField("name", name);
      for (const auto& value : values) {
        tracker.trackField(nullptr, value);
      }
    }
  };

  struct HeaderCallbacks final {
    inline static const HeaderKey& keyForRow(const Header& header) { return header.key; }
    inline static HeaderKey& keyForRow(Header& header) { return header.key; }
    static bool matches(Header& header, const HeaderKey& other);
    static bool matches(Header& header, kj::StringPtr otherName);
    static bool matches(Header& header, capnp::CommonHeaderName commondId);
    static kj::uint hashCode(const HeaderKey& key);
    static kj::uint hashCode(capnp::CommonHeaderName commondId);
  };

  kj::Table<Header, kj::HashIndex<HeaderCallbacks>> headers;

  Guard guard;

  void checkGuard() {
    JSG_REQUIRE(guard == Guard::NONE, TypeError, "Can't modify immutable headers.");
  }

  static kj::Maybe<kj::Array<jsg::ByteString>> entryIteratorNext(jsg::Lock& js, auto& state) {
    if (state.cursor == state.copy.end()) {
      return kj::none;
    }
    auto& ret = *state.cursor++;
    return kj::arr(kj::mv(ret.key), kj::mv(ret.value));
  }

  static kj::Maybe<jsg::ByteString> keyOrValueIteratorNext(jsg::Lock& js, auto& state) {
    if (state.cursor == state.copy.end()) {
      return kj::none;
    }
    auto& ret = *state.cursor++;
    return kj::mv(ret);
  }
};

}  // namespace workerd::api
