#include <workerd/jsg/jsg.h>
#include <kj/compat/http.h>
#include <workerd/io/features.h>
#include <workerd/io/worker-interface.capnp.h>

#if !_MSC_VER
#include <strings.h>
#else
#define strcasecmp _stricmp
#endif

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
    jsg::JsRef<jsg::JsString> key;   // lower-cased name
    jsg::JsRef<jsg::JsString> value;  // comma-concatenation of all values seen
  };

  Headers(): guard(Guard::NONE) {}
  explicit Headers(jsg::Lock& js, jsg::Dict<jsg::ByteString, jsg::ByteString> dict);
  explicit Headers(jsg::Lock& js, const Headers& other, Guard guard = Guard::NONE);
  explicit Headers(jsg::Lock& js, const kj::HttpHeaders& other, Guard guard);

  Headers(Headers&&) = delete;
  Headers& operator=(Headers&&) = delete;

  // Make a copy of this Headers object, and preserve the guard. The normal copy constructor sets
  // the copy's guard to NONE.
  jsg::Ref<Headers> clone(jsg::Lock& js) const;

  // Fill in the given HttpHeaders with these headers. Note that strings are inserted by
  // reference, so the output must be consumed immediately.
  void shallowCopyTo(kj::HttpHeaders& out);

  // Like has(), but only call this with an already-lower-case `name`. Useful to avoid an
  // unnecessary string allocation. Not part of the JS interface.
  bool hasLowerCase(kj::StringPtr name);

  // Returns headers with lower-case name and comma-concatenated duplicates.
  enum class DisplayedHeaderOption {
    DEFAULT,
    KEYONLY,
  };
  kj::Array<DisplayedHeader> getDisplayedHeaders(jsg::Lock& js,
      DisplayedHeaderOption option = DisplayedHeaderOption::DEFAULT);

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

  // getAll is a legacy non-standard extension API that we introduced before
  // getSetCookie() was defined. We continue to support it for backwards
  // compatibility but users really ought to be using getSetCookie() now.
  kj::ArrayPtr<jsg::ByteString> getAll(jsg::ByteString name);

  // The Set-Cookie header is special in that it is the only HTTP header that
  // is not permitted to be combined into a single instance.
  kj::ArrayPtr<jsg::ByteString> getSetCookie();

  bool has(jsg::ByteString name);

  void set(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value);

  // Like set(), but ignores the header guard if set. This can only be called from C++, and may be
  // used to mutate headers before dispatching a request.
  void setUnguarded(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value);

  void append(jsg::Lock& js, jsg::ByteString name, jsg::ByteString value);

  void delete_(jsg::ByteString name);

  void forEach(jsg::Lock& js,
               jsg::Function<void(jsg::JsString, jsg::JsString, jsg::Ref<Headers>)>,
               jsg::Optional<jsg::Value>);

  bool inspectImmutable();

  JSG_ITERATOR(EntryIterator, entries,
                kj::Array<jsg::JsRef<jsg::JsString>>,
                IteratorState<DisplayedHeader>,
                entryIteratorNext)
  JSG_ITERATOR(KeyIterator, keys,
                jsg::JsRef<jsg::JsString>,
                IteratorState<jsg::JsRef<jsg::JsString>>,
                keyOrValueIteratorNext)
  JSG_ITERATOR(ValueIterator, values,
                jsg::JsRef<jsg::JsString>,
                IteratorState<jsg::JsRef<jsg::JsString>>,
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

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    for (const auto& entry : headers) {
      tracker.trackField("header", entry);
    }
  }

  static kj::uint hashCode(kj::StringPtr name) {
    KJ_STACK_ARRAY(char, buf, name.size(), 1024, 1024);
    for (int n = 0; n < name.size(); n++) {
      if ('A' <= name[n] && name[n] <= 'Z') {
        buf[n] = name[n] | 0x20;  // Convert to lower-case.
      } else {
        buf[n] = name[n];
      }
    }
    return kj::hashCode(buf);
  }

private:
  struct Header {
    jsg::ByteString name;

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
    kj::uint hash;

    Header clone() const {
      return Header(name, KJ_MAP(val, values) {
        return jsg::ByteString(kj::str(val));
      }, hash);
    }

    Header(kj::String name, kj::String value)
        : name(kj::mv(name)), hash(hashCode(this->name)) {
      values.add(jsg::ByteString(kj::mv(value)));
    }

    Header(kj::StringPtr name, kj::Array<jsg::ByteString> values, kj::uint hash)
        : name(kj::str(name)), values(kj::mv(values)), hash(hash) {}

    void add(jsg::ByteString value) {
      values.add(kj::mv(value));
    }

    void set(jsg::ByteString value) {
      values.clear();
      values.add(kj::mv(value));
    }

    JSG_MEMORY_INFO(Header) {
      tracker.trackField("name", name);
      for (const auto& value : values) {
        tracker.trackField(nullptr, value);
      }
    }
  };

  struct HeaderCallbacks {
    kj::uint keyForRow(const Header& header) const {
      return header.hash;
    }
    bool matches(const Header& header, kj::uint key) const {
      return header.hash == key;
    }
    bool matches(const Header& header, kj::StringPtr name) const {
      return Headers::hashCode(name) == keyForRow(header);
    }
    uint hashCode(kj::uint hash) const { return hash; }
    uint hashCode(kj::StringPtr name) const {
      return Headers::hashCode(name);
    }
  };

  struct HeaderTreeCallbacks {
    kj::StringPtr keyForRow(const Header& header) const {
      return header.name;
    }
    bool isBefore(const Header& header, kj::StringPtr name) const {
      return strcasecmp(header.name.cStr(), name.cStr()) < 0;
    }
    bool matches(const Header& header, kj::StringPtr name) const {
      return Headers::hashCode(name) == header.hash;
    }
  };

  kj::Table<Header, kj::HashIndex<HeaderCallbacks>,
                    kj::TreeIndex<HeaderTreeCallbacks>> headers;

  Guard guard;

  void checkGuard() {
    JSG_REQUIRE(guard == Guard::NONE, TypeError, "Can't modify immutable headers.");
  }

  static kj::Maybe<kj::Array<jsg::JsRef<jsg::JsString>>> entryIteratorNext(
      jsg::Lock& js, auto& state) {
    if (state.cursor == state.copy.end()) {
      return kj::none;
    }
    auto& ret = *state.cursor++;
    return kj::arr(kj::mv(ret.key), kj::mv(ret.value));
  }

  static kj::Maybe<jsg::JsRef<jsg::JsString>> keyOrValueIteratorNext(jsg::Lock& js, auto& state) {
    if (state.cursor == state.copy.end()) {
      return kj::none;
    }
    auto& ret = *state.cursor++;
    return kj::mv(ret);
  }
};

}  // namespace workerd::api
