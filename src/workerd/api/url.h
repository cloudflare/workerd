// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <kj/refcount.h>
#include <kj/compat/url.h>

namespace workerd::api {

class URLSearchParams;

// The legacy, non-standard URL logic, over kj::Url: what URL and URLSearchParams share. A URL
// and its searchParams share one LegacyUrl; a URLSearchParams made on its own has one with only
// a query.
//
// The href setter is the only one that reports failure. The others ignore a value they reject,
// leaving the URL unchanged.
class LegacyUrl final: public kj::Refcounted {
 public:
  LegacyUrl() = default;
  explicit LegacyUrl(kj::Url&& url);

  // Throws TypeError: "Invalid URL string.", or with a base, "Invalid base URL string." or
  // "Invalid relative URL string.".
  static kj::Own<LegacyUrl> parse(kj::String url, kj::Maybe<kj::String> base);
  static kj::Maybe<kj::Own<LegacyUrl>> tryParse(kj::String url, kj::Maybe<kj::String> base);

  // May throw a TypeError for a URL that kj::Url cannot stringify.
  kj::String getHref() const;
  // Returns false, leaving the URL unchanged, if `value` is not a URL.
  bool setHref(kj::String value);

  kj::String getOrigin() const;

  kj::String getProtocol() const;
  void setProtocol(kj::String value);

  kj::String getUsername() const;
  void setUsername(kj::String value);

  kj::String getPassword() const;
  void setPassword(kj::String value);

  kj::String getHost() const;
  void setHost(kj::String value);

  kj::String getHostname() const;
  void setHostname(kj::String value);

  kj::String getPort() const;
  void setPort(kj::String value);

  kj::String getPathname() const;
  void setPathname(kj::String value);

  kj::String getSearch() const;
  void setSearch(kj::String value);

  kj::String getHash() const;
  void setHash(kj::String value);

  // The query, as a URLSearchParams list.
  size_t size() const {
    return url.query.size();
  }
  void append(kj::String name, kj::String value);
  // Removes the entries named `name`, and with `value` if given.
  void delete_(kj::StringPtr name, kj::Maybe<kj::StringPtr> value);
  kj::Maybe<kj::String> get(kj::StringPtr name) const;
  kj::Array<kj::String> getAll(kj::StringPtr name) const;
  bool has(kj::StringPtr name, kj::Maybe<kj::StringPtr> value) const;
  // Sets the first entry named `name` to `value` and removes the others, or appends one.
  void set(kj::String name, kj::String value);
  // Sorts by name in UTF-16 code units, stably.
  void sort();
  kj::StringPtr nameAt(size_t index) const;
  kj::StringPtr valueAt(size_t index) const;
  // URLSearchParams.toString(): every entry as `name=value`, application/x-www-form-urlencoded.
  kj::String serialize() const;

  // Replaces this URL with a copy of `other`.
  void assign(const LegacyUrl& other);
  // Replaces the query with `query` parsed as application/x-www-form-urlencoded, after one
  // leading '?', reporting the parsed entries' memory to `adjustment`.
  void assignQueryString(
      kj::ArrayPtr<const char> query, jsg::ExternalMemoryAdjustment& adjustment);
  // Replaces the query with the given (name, value) pairs.
  void assignQueryPairs(kj::Array<kj::Url::QueryParam> pairs);

  size_t memorySize() const;

 private:
  kj::Url url;
};

// Implements the URL interface as prescribed by: https://url.spec.whatwg.org/#api
// This is the legacy, non-standard implementation.
class URL: public jsg::Object {
 public:
  static jsg::Ref<URL> constructor(jsg::Lock& js, kj::String url, jsg::Optional<kj::String> base);

  // Href is the only setter that throws. All others ignore errors, leaving their values
  // unchanged.
  kj::String getHref();

  // Href is the only setter that throws. All others ignore errors, leaving their values
  // unchanged.
  void setHref(jsg::Lock& js, kj::String value);

  kj::String getOrigin();

  kj::String getProtocol();
  void setProtocol(kj::String value);

  kj::String getUsername();
  void setUsername(kj::String value);

  kj::String getPassword();
  void setPassword(kj::String value);

  kj::String getHost();
  void setHost(kj::String value);

  kj::String getHostname();
  void setHostname(kj::String value);

  kj::String getPort();
  void setPort(kj::String value);

  kj::String getPathname();
  void setPathname(kj::String value);

  kj::String getSearch();
  void setSearch(kj::String value);

  jsg::Ref<URLSearchParams> getSearchParams(jsg::Lock& js);

  kj::String getHash();
  void setHash(kj::String value);

  kj::String toString();
  kj::String toJSON();

  JSG_RESOURCE_TYPE(URL, CompatibilityFlags::Reader flags) {
    // Previously, we were setting all properties as instance properties,
    // which broke the ability to subclass the URL object. With the
    // compatibility flag set, we instead attach the properties to the
    // prototype.
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_PROTOTYPE_PROPERTY(href, getHref, setHref);
      JSG_READONLY_PROTOTYPE_PROPERTY(origin, getOrigin);
      JSG_PROTOTYPE_PROPERTY(protocol, getProtocol, setProtocol);
      JSG_PROTOTYPE_PROPERTY(username, getUsername, setUsername);
      JSG_PROTOTYPE_PROPERTY(password, getPassword, setPassword);
      JSG_PROTOTYPE_PROPERTY(host, getHost, setHost);
      JSG_PROTOTYPE_PROPERTY(hostname, getHostname, setHostname);
      JSG_PROTOTYPE_PROPERTY(port, getPort, setPort);
      JSG_PROTOTYPE_PROPERTY(pathname, getPathname, setPathname);
      JSG_PROTOTYPE_PROPERTY(search, getSearch, setSearch);
      JSG_READONLY_PROTOTYPE_PROPERTY(searchParams, getSearchParams);
      JSG_PROTOTYPE_PROPERTY(hash, getHash, setHash);
    } else {
      JSG_INSTANCE_PROPERTY(href, getHref, setHref);
      JSG_READONLY_INSTANCE_PROPERTY(origin, getOrigin);
      JSG_INSTANCE_PROPERTY(protocol, getProtocol, setProtocol);
      JSG_INSTANCE_PROPERTY(username, getUsername, setUsername);
      JSG_INSTANCE_PROPERTY(password, getPassword, setPassword);
      JSG_INSTANCE_PROPERTY(host, getHost, setHost);
      JSG_INSTANCE_PROPERTY(hostname, getHostname, setHostname);
      JSG_INSTANCE_PROPERTY(port, getPort, setPort);
      JSG_INSTANCE_PROPERTY(pathname, getPathname, setPathname);
      JSG_INSTANCE_PROPERTY(search, getSearch, setSearch);
      JSG_READONLY_INSTANCE_PROPERTY(searchParams, getSearchParams);
      JSG_INSTANCE_PROPERTY(hash, getHash, setHash);
    }

    JSG_METHOD(toString);
    JSG_METHOD(toJSON);

    JSG_TS_OVERRIDE({
      constructor(url: string | URL, base?: string | URL);
    });
    // Allow URLs which get coerced to strings in either constructor parameter
  }

  // Treat as private -- needs to be public for js.alloc<T>()...
  explicit URL(kj::Own<LegacyUrl> url);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackFieldWithSize("inner", url->memorySize());
    tracker.trackField("searchParams", searchParams);
  }

 private:
  kj::Own<LegacyUrl> url;
  kj::Maybe<jsg::Ref<URLSearchParams>> searchParams;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(searchParams);
  }
};

class URLSearchParams: public jsg::Object {
  // TODO(cleanup): Combine implementation with FormData?
 private:
  struct IteratorState {
    jsg::Ref<URLSearchParams> parent;
    uint index = 0;

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(parent);
    }
  };

 public:
  explicit URLSearchParams(kj::Own<LegacyUrl> url);

  using Initializer = kj::OneOf<jsg::Ref<URLSearchParams>, kj::String, jsg::Dict<kj::String>,
                                kj::Array<kj::Array<kj::String>>>;

  static jsg::Ref<URLSearchParams> constructor(jsg::Lock& js, jsg::Optional<Initializer> init);

  void append(kj::String name, kj::String value);
  void delete_(kj::String name);
  kj::Maybe<kj::String> get(kj::String name);
  kj::Array<kj::String> getAll(kj::String name);
  bool has(kj::String name);
  void set(kj::String name, kj::String value);

  void sort();

  JSG_ITERATOR(EntryIterator, entries,
                kj::Array<kj::String>,
                IteratorState,
                entryIteratorNext)
  JSG_ITERATOR(KeyIterator, keys,
                kj::String,
                IteratorState,
                keyIteratorNext)
  JSG_ITERATOR(ValueIterator, values,
                kj::String,
                IteratorState,
                valueIteratorNext)

  void forEach(jsg::Lock&,
               jsg::Function<void(kj::StringPtr, kj::StringPtr, jsg::Ref<URLSearchParams>)>,
               jsg::Optional<jsg::Value>);

  kj::String toString();

  inline uint getSize() { return url->size(); }

  JSG_RESOURCE_TYPE(URLSearchParams, CompatibilityFlags::Reader flags) {
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

    JSG_ITERABLE(entries);

    JSG_METHOD(toString);

    if (flags.getSpecCompliantUrl()) {
      // This is a hack. The non-spec-compliant URLSearchParams type is used in
      // the Response and Request constructors. This means that when the
      // TypeScript generation scripts are visiting root types for inclusion,
      // we'll always visit the non-spec-compliant type even if we have the
      // "url-standard" flag enabled. Rather than updating those usages based
      // on which flags are enabled, we just delete the non-spec complaint
      // declaration in an override if "url-standard" is enabled.
      JSG_TS_OVERRIDE(type URLSearchParams = never);
    } else {
      JSG_TS_OVERRIDE({
        constructor(init?: URLSearchParams | string | Record<string, string> | [key: string, value: string][]);

        entries(): IterableIterator<[key: string, value: string]>;
        [Symbol.iterator](): IterableIterator<[key: string, value: string]>;

        forEach<This = unknown>(callback: (this: This, value: string, key: string, parent: URLSearchParams) => void, thisArg?: This): void;
      });
    }
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackFieldWithSize("url", url->memorySize());
  }

 private:
  kj::Own<LegacyUrl> url;
  kj::Maybe<jsg::ExternalMemoryAdjustment> externalMemoryAdjustment;

  static kj::Maybe<kj::Array<kj::String>> entryIteratorNext(jsg::Lock& js, IteratorState& state) {
    if (state.index >= state.parent->url->size()) {
      return kj::none;
    }
    auto index = state.index++;
    auto& url = *state.parent->url;
    return kj::arr(kj::str(url.nameAt(index)), kj::str(url.valueAt(index)));
  }

  static kj::Maybe<kj::String> keyIteratorNext(jsg::Lock& js, IteratorState& state) {
    if (state.index >= state.parent->url->size()) {
      return kj::none;
    }
    return kj::str(state.parent->url->nameAt(state.index++));
  }

  static kj::Maybe<kj::String> valueIteratorNext(jsg::Lock& js, IteratorState& state) {
    if (state.index >= state.parent->url->size()) {
      return kj::none;
    }
    return kj::str(state.parent->url->valueAt(state.index++));
  }
};

#define EW_URL_ISOLATE_TYPES                  \
  api::URL,                                   \
  api::URLSearchParams,                       \
  api::URLSearchParams::EntryIterator,        \
  api::URLSearchParams::EntryIterator::Next,  \
  api::URLSearchParams::KeyIterator,          \
  api::URLSearchParams::KeyIterator::Next,    \
  api::URLSearchParams::ValueIterator,        \
  api::URLSearchParams::ValueIterator::Next

}  // namespace workerd::api
