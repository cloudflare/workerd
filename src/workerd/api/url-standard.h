// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/hash.h>
#include "form-data.h"
#include <workerd/api/blob.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>
#include <workerd/io/compatibility-date.capnp.h>

namespace workerd::api {
// The original URL implementation based on kj::Url is not compliant with the
// WHATWG URL standard, but we can't get rid of it. This is an alternate
// implementation that is based on the spec. It can be enabled using a
// configuration flag. We put it in its own namespace to keep its classes
// from conflicting with the old implementation.
namespace url {

class URL;

// The URLSearchParams object is a wrapper for application/x-www-form-urlencoded
// data. It can be used by itself or with URL (every URL object has a searchParams
// attribute that is kept in sync).
class URLSearchParams: public jsg::Object {
  template <typename T>
  struct IteratorState {
    jsg::Ref<URLSearchParams> self;
    T inner;
    IteratorState(jsg::Ref<URLSearchParams> self, T t) : self(kj::mv(self)), inner(kj::mv(t)) {}
    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(self);
    }
  };
public:
  using StringPair = jsg::Sequence<jsg::USVString>;
  using StringPairs = jsg::Sequence<StringPair>;

  using Initializer = kj::OneOf<StringPairs,
                                jsg::Dict<jsg::USVString, jsg::USVString>,
                                jsg::USVString>;

  // Constructor called by the static constructor method.
  URLSearchParams(Initializer init);

  // Constructor called by the URL class when created.
  URLSearchParams(kj::Maybe<kj::ArrayPtr<const char>> maybeQuery, URL& url);

  static jsg::Ref<URLSearchParams> constructor(jsg::Lock& js, jsg::Optional<Initializer> init);

  void append(jsg::USVString name, jsg::USVString value);
  void delete_(jsg::Lock& js, jsg::USVString name, jsg::Optional<jsg::USVString> value);
  kj::Maybe<kj::ArrayPtr<const char>> get(jsg::USVString name);
  kj::Array<kj::ArrayPtr<const char>> getAll(jsg::USVString name);
  bool has(jsg::Lock& js, jsg::USVString name, jsg::Optional<jsg::USVString> value);
  void set(jsg::USVString name, jsg::USVString value);
  void sort();

  JSG_ITERATOR(EntryIterator, entries,
               kj::Array<kj::ArrayPtr<const char>>,
               IteratorState<jsg::UrlSearchParams::EntryIterator>,
               entryIteratorNext)
  JSG_ITERATOR(KeyIterator, keys,
               kj::ArrayPtr<const char>,
               IteratorState<jsg::UrlSearchParams::KeyIterator>,
               keyIteratorNext)
  JSG_ITERATOR(ValueIterator, values,
               kj::ArrayPtr<const char>,
               IteratorState<jsg::UrlSearchParams::ValueIterator>,
               valueIteratorNext)

  void forEach(jsg::Lock&,
               jsg::Function<void(kj::StringPtr, kj::StringPtr, jsg::Ref<URLSearchParams>)>,
               jsg::Optional<jsg::JsValue>);

  kj::String toString();

  uint getSize();

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
    JSG_METHOD(toString);
    JSG_ITERABLE(entries);

    if (!flags.getSpecCompliantUrl()) {
      // This is a hack. The spec-compliant URLSearchParams type is used in the Body constructor,
      // see https://github.com/cloudflare/workerd/blob/v1.20241127.0/src/workerd/api/http.h#L255
      // This means that when the TypeScript generation scripts are visiting root types for
      // inclusion, we'll always visit the spec-compliant type even if we have the "url-standard"
      // flag disabled. Rather than updating those usages based on which flags are enabled, we just
      // delete the spec compliant declaration in an override if "url-standard" is disabled.
      // We do the same for the non-spec-compliant URLSearchParams
      // (https://github.com/cloudflare/workerd/blob/v1.20241127.0/src/workerd/api/url.h#L219).
      JSG_TS_OVERRIDE(type URLSearchParams = never);
    } else if (flags.getUrlSearchParamsDeleteHasValueArg()) {
      JSG_TS_OVERRIDE(URLSearchParams {
        entries(): IterableIterator<[key: string, value: string]>;
        [Symbol.iterator](): IterableIterator<[key: string, value: string]>;

        forEach<This = unknown>(callback: (this: This, value: string, key: string, parent: URLSearchParams) => void, thisArg?: This): void;
      });
    } else {
      JSG_TS_OVERRIDE(URLSearchParams {
        delete(name: string): void;
        has(name: string): boolean;

        entries(): IterableIterator<[key: string, value: string]>;
        [Symbol.iterator](): IterableIterator<[key: string, value: string]>;

        forEach<This = unknown>(callback: (this: This, value: string, key: string, parent: URLSearchParams) => void, thisArg?: This): void;
      });
    }
    // Rename from urlURLSearchParams
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("inner", inner);
    tracker.trackField("url", maybeUrl);
  }

private:
  jsg::UrlSearchParams inner;
  kj::Maybe<URL&> maybeUrl;

  // Updates the associated URL (if any) with the serialized contents of this URLSearchParam
  void update();

  // Updates the contents of this URLSearchParam with the current contents of the associated
  // URLs search component.
  void reset();

  static kj::Maybe<kj::Array<kj::ArrayPtr<const char>>> entryIteratorNext(
      jsg::Lock& js,
      IteratorState<jsg::UrlSearchParams::EntryIterator>& state);
  static kj::Maybe<kj::ArrayPtr<const char>> keyIteratorNext(
      jsg::Lock& js,
      IteratorState<jsg::UrlSearchParams::KeyIterator>& state);
  static kj::Maybe<kj::ArrayPtr<const char>> valueIteratorNext(
      jsg::Lock& js,
      IteratorState<jsg::UrlSearchParams::ValueIterator>& state);

  friend class URL;
};

// The humble URL object, in all its spec-compliant glory.
// The majority of the implementation is covered by jsg::Url.
class URL: public jsg::Object {
public:
  URL(kj::StringPtr url, kj::Maybe<kj::StringPtr> base = kj::none);
  ~URL() noexcept(false) override;

  static jsg::Ref<URL> constructor(jsg::Lock& js, jsg::USVString url, jsg::Optional<jsg::USVString> base);

  static kj::Maybe<jsg::Ref<URL>> parse(jsg::Lock& js, jsg::USVString url, jsg::Optional<jsg::USVString> base) {
    // Method should not throw if the parse fails
    return js.tryCatch([&]() -> kj::Maybe<jsg::Ref<URL>> {
      return constructor(js, kj::mv(url), kj::mv(base));
    }, [](auto) -> kj::Maybe<jsg::Ref<URL>> { return kj::none; });
  }

  kj::ArrayPtr<const char> getHref();
  void setHref(jsg::Lock& js, jsg::USVString value);

  kj::Array<const char> getOrigin();

  kj::ArrayPtr<const char> getProtocol();
  void setProtocol(jsg::USVString value);

  kj::ArrayPtr<const char> getUsername();
  void setUsername(jsg::USVString value);

  kj::ArrayPtr<const char> getPassword();
  void setPassword(jsg::USVString value);

  kj::ArrayPtr<const char> getHost();
  void setHost(jsg::USVString value);

  kj::ArrayPtr<const char> getHostname();
  void setHostname(jsg::USVString value);

  kj::ArrayPtr<const char> getPort();
  void setPort(jsg::USVString value);

  kj::ArrayPtr<const char> getPathname();
  void setPathname(jsg::USVString value);

  kj::ArrayPtr<const char> getSearch();
  void setSearch(jsg::USVString value);

  kj::ArrayPtr<const char> getHash();
  void setHash(jsg::USVString value);

  jsg::Ref<URLSearchParams> getSearchParams(jsg::Lock& js);

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
  static bool canParse(jsg::USVString url, jsg::Optional<jsg::USVString> base = kj::none);
  static jsg::JsString createObjectURL(jsg::Lock& js, kj::OneOf<jsg::Ref<File>, jsg::Ref<Blob>> object);
  static void revokeObjectURL(jsg::Lock& js, jsg::USVString object_url);

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
    JSG_STATIC_METHOD(parse);
    JSG_STATIC_METHOD(createObjectURL);
    JSG_STATIC_METHOD(revokeObjectURL);

    JSG_TS_OVERRIDE(URL {
      constructor(url: string | URL, base?: string | URL);
    });
    // Rename from urlURL, and allow URLs which get coerced to strings in either
    // constructor parameter
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("inner", inner);
    tracker.trackField("searchParams", maybeSearchParams);
  }

private:
  jsg::Url inner;
  kj::Maybe<jsg::Ref<URLSearchParams>> maybeSearchParams;

  void visitForGc(jsg::GcVisitor& visitor);

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

