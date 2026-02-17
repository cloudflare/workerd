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
  static constexpr kj::uint MAX_COMMON_HEADER_ID =
    static_cast<kj::uint>(capnp::CommonHeaderName::WWW_AUTHENTICATE);

  enum class Guard {
    // WARNING: This type is serialized, do not change the numeric values.
    IMMUTABLE = 0,
    REQUEST = 1,
    // REQUEST_NO_CORS,  // CORS not relevant on server side
    RESPONSE = 2,
    NONE = 3
  };

  struct DisplayedHeader {
    kj::String key;   // lower-cased name
    kj::String value; // comma-concatenation of all values seen
  };

  Headers(): guard(Guard::NONE) {}
  explicit Headers(jsg::Lock& js, jsg::Dict<kj::String, kj::String> dict);
  explicit Headers(jsg::Lock& js, const Headers& other);
  explicit Headers(jsg::Lock& js, const kj::HttpHeaders& other, Guard guard);
  KJ_DISALLOW_COPY_AND_MOVE(Headers);

  // Make a copy of this Headers object, and preserve the guard.
  jsg::Ref<Headers> clone(jsg::Lock& js) const;

  // Fill in the given HttpHeaders with these headers. Note that strings are inserted by
  // reference, so the output must be consumed immediately.
  void shallowCopyTo(kj::HttpHeaders& out);

  // Returns headers with lower-case name and comma-concatenated duplicates.
  kj::Array<DisplayedHeader> getDisplayedHeaders(jsg::Lock& js);

  using StringPair = jsg::Sequence<kj::String>;
  using StringPairs = jsg::Sequence<StringPair>;

  // Per the fetch specification, it is possible to initialize a Headers object
  // from any other object that has a Symbol.iterator implementation. Those are
  // handled in this Initializer definition using the StringPairs definition
  // that aliases jsg::Sequence<jsg::Sequence<kj::String>>. Technically,
  // the Headers object itself falls under that definition as well. However, treating
  // a Headers object as a jsg::Sequence<jsg::Sequence<T>> is nowhere near as
  // performant and has the side effect of forcing all header names to be lower-cased
  // rather than case-preserved. Instead of following the spec exactly here, we
  // choose to special case creating a Header object from another Header object.
  // This is an intentional departure from the spec.
  using Initializer = kj::OneOf<jsg::Ref<Headers>,
                                StringPairs,
                                jsg::Dict<kj::String, kj::String>>;

  static jsg::Ref<Headers> constructor(jsg::Lock& js, jsg::Optional<Initializer> init);
  kj::Maybe<kj::String> get(jsg::Lock& js, kj::String name);

  // getAll is a legacy non-standard extension API that we introduced before
  // getSetCookie() was defined. We continue to support it for backwards
  // compatibility but users really ought to be using getSetCookie() now.
  kj::Array<kj::StringPtr> getAll(kj::String name);

  // The Set-Cookie header is special in that it is the only HTTP header that
  // is not permitted to be combined into a single instance.
  kj::Array<kj::StringPtr> getSetCookie();

  bool has(kj::String name);

  void set(jsg::Lock& js, kj::String name, kj::String value);
  void append(jsg::Lock& js, kj::String name, kj::String value);
  void delete_(kj::String name);

  // The *Unguarded variations of set/append are used for internal use when we want to
  // bypass certain checks, such as the guard check. These are not intended for public use and should be used with caution.
  kj::Maybe<kj::String> getPtr(jsg::Lock& js, kj::StringPtr name);
  void setUnguarded(jsg::Lock& js, kj::String name, kj::String value);
  void appendUnguarded(jsg::Lock& js, kj::String name, kj::String value);

  // The *Common variations of get/has/set/delete are used for internal use when we want to access
  // common headers by their common enum ID. These are not intended for public use and should be
  // used with caution. These also avoid guard checks.
  kj::Maybe<kj::String> getCommon(jsg::Lock& js, capnp::CommonHeaderName idx);
  bool hasCommon(capnp::CommonHeaderName idx);
  void setCommon(capnp::CommonHeaderName idx, kj::String value);
  void deleteCommon(capnp::CommonHeaderName idx);

  void forEach(jsg::Lock& js,
               jsg::Function<void(kj::StringPtr, kj::StringPtr, jsg::Ref<Headers>)>,
               jsg::Optional<jsg::Value>);

  bool inspectImmutable();

  JSG_ITERATOR(EntryIterator, entries,
               kj::Array<kj::String>,
               IteratorState<DisplayedHeader>,
               entryIteratorNext)
  JSG_ITERATOR(KeyIterator, keys,
               kj::String,
               IteratorState<kj::String>,
               keyOrValueIteratorNext)
  JSG_ITERATOR(ValueIterator, values,
               kj::String,
               IteratorState<kj::String>,
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

  // A header is identified by either a common header ID or an uncommon header name.
  // The header key name is always identifed in lower-case form, while the original
  // casing is preserved in the actual Header struct to support case-preserving display.
  // TODO(perf): We can likely optimize this further by interning uncommon header names
  // so that we avoid repeated allocations of the same uncommon header name. Unless
  // it proves to be a performance problem, however, we can leave that for future work.
  using HeaderKey = kj::OneOf<uint, kj::String>;

private:
  struct Header final {
    // The name is only set when the casing of the name differs from the lower-cased key.
    kj::Maybe<kj::String> name;
    kj::Vector<kj::String> values;
    Header() = default;
    explicit Header(kj::Maybe<kj::String> name): name(kj::mv(name)) {
      values.reserve(1);
    }

    kj::Own<Header> clone() const {
      Header header;
      header.name = name.map([](const kj::String& s) { return kj::str(s); });
      header.values = KJ_MAP(v, values) { return kj::str(v); };
      return kj::heap(kj::mv(header));
    }

    JSG_MEMORY_INFO(Header) {
      tracker.trackField("name", name);
      for (const auto& value : values) {
        tracker.trackField("value", value);
      }
    }
  };

  // This wastes one slot, but it is a fixed array for fast access.
  kj::FixedArray<kj::Maybe<kj::Own<Header>>, MAX_COMMON_HEADER_ID + 1> commonHeaders;

  // The key is always lower-case.
  kj::HashMap<kj::String, kj::Own<Header>> uncommonHeaders;

  Guard guard;

  kj::Maybe<Header&> tryGetHeader(const HeaderKey& key);

  void checkGuard() {
    JSG_REQUIRE(guard == Guard::NONE, TypeError, "Can't modify immutable headers.");
  }

  static kj::Maybe<kj::Array<kj::String>> entryIteratorNext(jsg::Lock& js, auto& state) {
    if (state.cursor == state.copy.end()) {
      return kj::none;
    }
    auto& ret = *state.cursor++;
    return kj::arr(kj::mv(ret.key), kj::mv(ret.value));
  }

  static kj::Maybe<kj::String> keyOrValueIteratorNext(jsg::Lock& js, auto& state) {
    if (state.cursor == state.copy.end()) {
      return kj::none;
    }
    auto& ret = *state.cursor++;
    return kj::mv(ret);
  }
};

}  // namespace workerd::api
