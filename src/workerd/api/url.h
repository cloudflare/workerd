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

class URL: public jsg::Object {
  // Implements the URL interface as prescribed by: https://url.spec.whatwg.org/#api

public:
  static jsg::Ref<URL> constructor(kj::String url, jsg::Optional<kj::String> base);

  kj::String getHref();
  void setHref(const v8::PropertyCallbackInfo<void>& info, kj::String value);
  // Href is the only setter that throws. All others ignore errors, leaving their values
  // unchanged.

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

  jsg::Ref<URLSearchParams> getSearchParams();

  kj::String getHash();
  void setHash(kj::String value);

  kj::String toString();
  kj::String toJSON();

  JSG_RESOURCE_TYPE(URL, CompatibilityFlags::Reader flags) {
    // Previously, we were setting all properties as instance properties,
    // which broke the ability to subclass the URL object. With the
    // feature flag set, we instead attach the properties to the
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
  }

  explicit URL(kj::Url&& u);
  // Treat as private -- needs to be public for jsg::alloc<T>()...

private:
  friend class URLSearchParams;

  struct RefcountedUrl: kj::Refcounted, kj::Url {
    template <typename... Args>
    RefcountedUrl(Args&&... args): kj::Url(kj::fwd<Args>(args)...) {}
  };

  kj::Own<RefcountedUrl> url;
  kj::Maybe<jsg::Ref<URLSearchParams>> searchParams;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(searchParams);
  }
};

class URLSearchParams: public jsg::Object {
  // TODO(cleanup): Combine implementation with FormData?
private:
  using EntryIteratorType = kj::Array<kj::String>;
  using KeyIteratorType = kj::String;
  using ValueIteratorType = kj::String;

  struct IteratorState {
    jsg::Ref<URLSearchParams> parent;
    uint index = 0;

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(parent);
    }
  };

public:
  explicit URLSearchParams(kj::Own<URL::RefcountedUrl> url);

  using Initializer = kj::OneOf<jsg::Ref<URLSearchParams>, kj::String, jsg::Dict<kj::String>,
                                kj::Array<kj::Array<kj::String>>>;

  static jsg::Ref<URLSearchParams> constructor(jsg::Optional<Initializer> init);

  void append(kj::String name, kj::String value);
  void delete_(kj::String name);
  kj::Maybe<kj::String> get(kj::String name);
  kj::Array<kj::String> getAll(kj::String name);
  bool has(kj::String name);
  void set(kj::String name, kj::String value);

  void sort();

  JSG_ITERATOR(EntryIterator, entries,
                EntryIteratorType,
                IteratorState,
                iteratorNext<EntryIteratorType>)
  JSG_ITERATOR(KeyIterator, keys,
                KeyIteratorType,
                IteratorState,
                iteratorNext<KeyIteratorType>)
  JSG_ITERATOR(ValueIterator, values,
                ValueIteratorType,
                IteratorState,
                iteratorNext<ValueIteratorType>)

  void forEach(
      jsg::V8Ref<v8::Function> callback,
      jsg::Optional<jsg::Value> thisArg,
      v8::Isolate* isolate);

  kj::String toString();

  JSG_RESOURCE_TYPE(URLSearchParams) {
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
  }

private:
  kj::Own<URL::RefcountedUrl> url;

  template <typename Type>
  static kj::Maybe<Type> iteratorNext(jsg::Lock& js, IteratorState& state) {
    if (state.index >= state.parent->url->query.size()) {
      return nullptr;
    }
    auto& [key, value] = state.parent->url->query[state.index++];
    if constexpr (kj::isSameType<Type, EntryIteratorType>()) {
      return kj::arr(kj::str(key), kj::str(value));
    } else if constexpr (kj::isSameType<Type, KeyIteratorType>()) {
      return kj::str(key);
    } else if constexpr (kj::isSameType<Type, ValueIteratorType>()) {
      return kj::str(value);
    } else {
      KJ_UNREACHABLE;
    }
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
