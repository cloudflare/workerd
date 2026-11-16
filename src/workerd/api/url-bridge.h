// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

namespace url {
class URL;
class URLSearchParams;
}  // namespace url

// The C++ side of the TypeScript URL and URLSearchParams (src/per_isolate/url/), which replace
// the C++ classes under the TS_URL autogate: JSG types that accept them where C++ takes those
// classes. Each recognizes an instance by the V8-private brand its constructor stamps, without
// running JavaScript, and never recognizes a proxy. It reads the instance through the
// url/cpp_exports bootstrap module, whose functions use the implementation's own state rather
// than any method user code can replace.
//
// Neither may be read in a no-JS scope, such as V8's deserializer's.

// A TypeScript URL.
class JsUrl final {
 public:
  // The URL's serialization. The legacy backend's can throw a TypeError, as the legacy C++ URL's
  // href can.
  kj::String getHref(jsg::Lock& js) const;

  // Undefined unless `value` is a TypeScript URL.
  static kj::Maybe<JsUrl> tryUnwrap(jsg::Lock& js, jsg::JsValue value);

  // Described to RTTI, and so to the generated types, as the C++ URL.
  using JsgRttiDelegate = jsg::Ref<url::URL>;

  static v8::Local<v8::Value> jsgWrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      JsUrl value) {
    return value.object.getHandle(js);
  }

  static kj::Maybe<JsUrl> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return tryUnwrap(js, jsg::JsValue(handle));
  }

 private:
  explicit JsUrl(jsg::JsRef<jsg::JsObject> object): object(kj::mv(object)) {}

  jsg::JsRef<jsg::JsObject> object;
};

// A TypeScript URLSearchParams.
class JsUrlSearchParams final {
 public:
  // URLSearchParams.prototype.toString(), whatever the instance's own toString() does.
  kj::String serialize(jsg::Lock& js) const;

  // Undefined unless `value` is a TypeScript URLSearchParams.
  static kj::Maybe<JsUrlSearchParams> tryUnwrap(jsg::Lock& js, jsg::JsValue value);

  // Described to RTTI, and so to the generated types, as the C++ URLSearchParams.
  using JsgRttiDelegate = jsg::Ref<url::URLSearchParams>;

  static v8::Local<v8::Value> jsgWrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      JsUrlSearchParams value) {
    return value.object.getHandle(js);
  }

  static kj::Maybe<JsUrlSearchParams> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return tryUnwrap(js, jsg::JsValue(handle));
  }

 private:
  explicit JsUrlSearchParams(jsg::JsRef<jsg::JsObject> object): object(kj::mv(object)) {}

  jsg::JsRef<jsg::JsObject> object;
};

}  // namespace workerd::api
