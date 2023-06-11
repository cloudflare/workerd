// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/hash.h>
#include "form-data.h"
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/string.h>
#include <workerd/io/compatibility-date.capnp.h>

namespace workerd::api {
  // The original URL implementation based on kj::Url is not compliant with the
  // WHATWG URL standard, but we can't get rid of it. This is an alternate
  // implementation that is based on the spec. It can be enabled using a
  // configuration flag. We put it in it's own namespace to keep it's classes
  // from conflicting with the old implementation.
namespace url {

struct OpaqueOrigin {};
// An internal structure representing a URL origin that cannot be serialized.

struct TupleOrigin {
  // "Special scheme" URLs have an origin that is composed of the scheme, host, and port.
  // https://html.spec.whatwg.org/multipage/origin.html#concept-origin-tuple
  jsg::UsvStringPtr scheme;
  jsg::UsvStringPtr host;
  kj::Maybe<uint16_t> port = nullptr;
};

using Origin = kj::OneOf<OpaqueOrigin, TupleOrigin>;

struct UrlRecord {
  // The internal representation of a parsed URL.
  using Path = kj::OneOf<jsg::UsvString, kj::Array<jsg::UsvString>>;

  jsg::UsvString scheme;
  jsg::UsvString username;
  jsg::UsvString password;
  kj::Maybe<jsg::UsvString> host;
  kj::Maybe<uint16_t> port;
  Path path = kj::Array<jsg::UsvString>();
  kj::Maybe<jsg::UsvString> query;
  kj::Maybe<jsg::UsvString> fragment;
  bool special;

  enum class GetHrefOption {
    NONE,
    EXCLUDE_FRAGMENT,
  };

  Origin getOrigin();
  jsg::UsvString getHref(GetHrefOption option = GetHrefOption::NONE);
  jsg::UsvString getPathname();

  void setUsername(jsg::UsvStringPtr username);
  void setPassword(jsg::UsvStringPtr password);

  bool operator==(UrlRecord& other);
  bool operator!=(UrlRecord& other) { return !operator==(other); }

  bool equivalentTo(UrlRecord& other, GetHrefOption option = GetHrefOption::NONE);
};

class URL;

class URLSearchParams: public jsg::Object {
  // The URLSearchParams object is a wrapper for application/x-www-form-urlencoded
  // data. It can be used by itself or with URL (every URL object has a searchParams
  // attribute that is kept in sync).
private:
  struct IteratorState {
    jsg::Ref<URLSearchParams> parent;
    uint index = 0;
    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(parent);
    }
  };
public:
  using UsvStringPair = jsg::Sequence<jsg::UsvString>;
  using UsvStringPairs = jsg::Sequence<UsvStringPair>;

  using Initializer = kj::OneOf<UsvStringPairs,
                                jsg::Dict<jsg::UsvString, jsg::UsvString>,
                                jsg::UsvString>;

  URLSearchParams(Initializer init);
  // Constructor called by the static constructor method.

  URLSearchParams(kj::Maybe<jsg::UsvString>& maybeQuery, URL& url);
  // Constructor called by the URL class when created.

  static jsg::Ref<URLSearchParams> constructor(jsg::Optional<Initializer> init) {
    return jsg::alloc<URLSearchParams>(kj::mv(init).orDefault(jsg::usv()));
  }

  void append(jsg::UsvString name, jsg::UsvString value);
  void delete_(jsg::Lock& js, jsg::UsvString name, jsg::Optional<jsg::UsvString> value);
  kj::Maybe<jsg::UsvStringPtr> get(jsg::UsvString name);
  kj::Array<jsg::UsvStringPtr> getAll(jsg::UsvString name);
  bool has(jsg::Lock& js, jsg::UsvString name, jsg::Optional<jsg::UsvString> value);
  void set(jsg::UsvString name, jsg::UsvString value);
  void sort();

  JSG_ITERATOR(EntryIterator, entries,
               kj::Array<jsg::UsvStringPtr>,
               IteratorState,
               entryIteratorNext)
  JSG_ITERATOR(KeyIterator, keys,
               jsg::UsvStringPtr,
               IteratorState,
               keyIteratorNext)
  JSG_ITERATOR(ValueIterator, values,
               jsg::UsvStringPtr,
               IteratorState,
               valueIteratorNext)

  void forEach(
      jsg::V8Ref<v8::Function> callback,
      jsg::Optional<jsg::Value> thisArg,
      v8::Isolate* isolate);

  jsg::UsvString toString();

  inline uint getSize() { return list.size(); }

  JSG_RESOURCE_TYPE(URLSearchParams) {
    JSG_READONLY_PROTOTYPE_PROPERTY(size, getSize);
    JSG_METHOD(append);
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(get);
    JSG_METHOD(getAll);
    JSG_METHOD(has);
    JSG_METHOD(set);
    JSG_METHOD(sort);
    JSG_METHOD(entries);
    JSG_METHOD(keys);
    JSG_METHOD(values);
    JSG_METHOD(forEach);
    JSG_METHOD(toString);
    JSG_ITERABLE(entries);

    JSG_TS_OVERRIDE(URLSearchParams {
      entries(): IterableIterator<[key: string, value: string]>;
      [Symbol.iterator](): IterableIterator<[key: string, value: string]>;

      forEach<This = unknown>(callback: (this: This, value: string, key: string, parent: URLSearchParams) => void, thisArg?: This): void;
    });
    // Rename from urlURLSearchParams
  }

private:
  struct Entry {
    jsg::UsvString name;
    jsg::UsvString value;
    uint hash;

    Entry(jsg::UsvString name, jsg::UsvString value)
        : name(kj::mv(name)),
          value(kj::mv(value)),
          hash(kj::hashCode(this->name.storage())) {}

    Entry(Entry&&) = default;
    Entry& operator=(Entry&&) = default;

    inline uint hashCode() const { return hash; }
    inline bool operator==(const Entry& other) const { return name == other.name; }
  };

  kj::Vector<Entry> list;
  kj::Maybe<URL&> maybeUrl;

  void update();
  void reset(kj::Maybe<jsg::UsvStringPtr> value = nullptr);

  void init(Initializer init);
  void parse(jsg::UsvStringPtr input);

  static kj::Maybe<kj::Array<jsg::UsvStringPtr>> entryIteratorNext(
      jsg::Lock& js, IteratorState& state) {
    if (state.index >= state.parent->list.size()) {
      return nullptr;
    }
    auto& entry = state.parent->list[state.index++];
    return kj::arr<jsg::UsvStringPtr>(entry.name, entry.value);
  }

  static kj::Maybe<jsg::UsvStringPtr> keyIteratorNext(jsg::Lock& js, IteratorState& state) {
    if (state.index >= state.parent->list.size()) {
      return nullptr;
    }
    auto& entry = state.parent->list[state.index++];
    return entry.name.asPtr();
  }

  static kj::Maybe<jsg::UsvStringPtr> valueIteratorNext(jsg::Lock& js, IteratorState& state) {
    if (state.index >= state.parent->list.size()) {
      return nullptr;
    }
    auto& entry = state.parent->list[state.index++];
    return entry.value.asPtr();
  }

  friend class URL;
};

#define EW_URL_PARSE_STATES(V) \
  V(SCHEME_START) \
  V(SCHEME) \
  V(NO_SCHEME) \
  V(SPECIAL_RELATIVE_OR_AUTHORITY) \
  V(PATH_OR_AUTHORITY) \
  V(RELATIVE) \
  V(RELATIVE_SLASH) \
  V(SPECIAL_AUTHORITY_SLASHES) \
  V(SPECIAL_AUTHORITY_IGNORE_SLASHES) \
  V(AUTHORITY) \
  V(HOST) \
  V(HOSTNAME) \
  V(PORT) \
  V(FILE) \
  V(FILE_SLASH) \
  V(FILE_HOST) \
  V(PATH_START) \
  V(PATH) \
  V(OPAQUE_PATH) \
  V(QUERY) \
  V(FRAGMENT)

class URL: public jsg::Object {
  // The humble URL object, in all its spec-compliant glory.
public:
  enum class ParseState {
  #define V(name) name,
    EW_URL_PARSE_STATES(V)
  #undef V
  };

  static kj::Maybe<UrlRecord> parse(
      jsg::UsvStringPtr input,
      jsg::Optional<UrlRecord&> maybeBase = nullptr,
      kj::Maybe<UrlRecord&> maybeRecord = nullptr,
      kj::Maybe<ParseState> maybeStateOverride = nullptr);

  URL(jsg::UsvStringPtr url, jsg::Optional<jsg::UsvStringPtr> base = nullptr);

  ~URL() noexcept(false) override;

  static inline jsg::Ref<URL> constructor(
      jsg::UsvString url,
      jsg::Optional<jsg::UsvString> base) {
    return jsg::alloc<URL>(
        kj::mv(url),
        base.map([](jsg::UsvString& base) { return base.asPtr(); }));
  }

  jsg::UsvString getHref();
  void setHref(jsg::UsvString value);

  jsg::UsvString getOrigin();

  jsg::UsvString getProtocol();
  void setProtocol(jsg::UsvString value);

  jsg::UsvStringPtr getUsername();
  void setUsername(jsg::UsvString value);

  jsg::UsvStringPtr getPassword();
  void setPassword(jsg::UsvString value);

  jsg::UsvString getHost();
  void setHost(jsg::UsvString value);

  jsg::UsvStringPtr getHostname();
  void setHostname(jsg::UsvString value);

  jsg::UsvString getPort();
  void setPort(jsg::UsvString value);

  jsg::UsvString getPathname();
  void setPathname(jsg::UsvString value);

  jsg::UsvString getSearch();
  void setSearch(jsg::UsvString value);

  jsg::UsvString getHash();
  void setHash(jsg::UsvString value);

  inline jsg::Ref<URLSearchParams> getSearchParams(v8::Isolate* isolate) {
    KJ_IF_MAYBE(searchParams, maybeSearchParams) {
      return searchParams->addRef();
    }
    auto searchParams = jsg::alloc<URLSearchParams>(inner.query, *this);
    maybeSearchParams = searchParams.addRef();
    return kj::mv(searchParams);
  }

  UrlRecord& getRecord() KJ_LIFETIMEBOUND { return inner; }

  static bool canParse(jsg::UsvString url,
                       jsg::Optional<jsg::UsvString> base = nullptr);
  // Standard utility that returns true if the given input can be
  // successfully parsed as a URL. This is useful for validating
  // URL inputs without incurring the additional cost of constructing
  // and throwing an error. For example:
  //
  // const urls = [
  //   'https://example.org/good',
  //   'not a url'
  // ].filter((test) => URL.canParse(test));
  //

  JSG_RESOURCE_TYPE(URL) {
    JSG_READONLY_PROTOTYPE_PROPERTY(origin, getOrigin);
    JSG_PROTOTYPE_PROPERTY(href, getHref, setHref);
    JSG_PROTOTYPE_PROPERTY(protocol, getProtocol, setProtocol);
    JSG_PROTOTYPE_PROPERTY(username, getUsername, setUsername);
    JSG_PROTOTYPE_PROPERTY(password, getPassword, setPassword);
    JSG_PROTOTYPE_PROPERTY(host, getHost, setHost);
    JSG_PROTOTYPE_PROPERTY(hostname, getHostname, setHostname);
    JSG_PROTOTYPE_PROPERTY(port, getPort, setPort);
    JSG_PROTOTYPE_PROPERTY(pathname, getPathname, setPathname);
    JSG_PROTOTYPE_PROPERTY(search, getSearch, setSearch);
    JSG_PROTOTYPE_PROPERTY(hash, getHash, setHash);
    JSG_READONLY_PROTOTYPE_PROPERTY(searchParams, getSearchParams);
    JSG_METHOD_NAMED(toJSON, getHref);
    JSG_METHOD_NAMED(toString, getHref);
    JSG_STATIC_METHOD(canParse);

    JSG_TS_OVERRIDE(URL {
      constructor(url: string | URL, base?: string | URL);
    });
    // Rename from urlURL, and allow URLs which get coerced to strings in either constructor parameter
  }

  static bool isSpecialScheme(jsg::UsvStringPtr scheme);
  static kj::Maybe<uint16_t> defaultPortForScheme(jsg::UsvStringPtr scheme);

private:
  UrlRecord inner;
  kj::Maybe<jsg::Ref<URLSearchParams>> maybeSearchParams;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(maybeSearchParams);
  }

  friend class URLSearchParams;
};

#define EW_URL_STANDARD_ISOLATE_TYPES              \
  api::url::URL,                                   \
  api::url::URLSearchParams,                       \
  api::url::URLSearchParams::EntryIterator,        \
  api::url::URLSearchParams::EntryIterator::Next,  \
  api::url::URLSearchParams::KeyIterator,          \
  api::url::URLSearchParams::KeyIterator::Next,    \
  api::url::URLSearchParams::ValueIterator,        \
  api::url::URLSearchParams::ValueIterator::Next

}  // namespace url
}  // namespace api

