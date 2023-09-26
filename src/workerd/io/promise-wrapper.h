// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>

namespace workerd {

template <typename Self>
class PromiseWrapper {
public:
  template <typename T>
  static constexpr const char* getName(kj::Promise<T>*) { return "Promise"; }

  // Explicitly disallow V8 handles, which are not safe for the KJ event loop to own directly.
  template <typename T>
  static constexpr const char* getName(kj::Promise<v8::Global<T>>*) = delete;
  template <typename T>
  static constexpr const char* getName(kj::Promise<v8::Local<T>>*) = delete;

  template <typename T>
  static constexpr const char* getName(kj::Promise<jsg::Ref<T>>*) { return "Promise"; }
  // For some reason, this wasn't needed for jsg::V8Ref<T>...

  template <typename T>
  v8::Local<v8::Promise> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator,
      kj::Promise<T> promise) {
    auto jsPromise = IoContext::current().awaitIoLegacy(kj::mv(promise));
    return static_cast<Self&>(*this).wrap(context, kj::mv(creator), kj::mv(jsPromise));
  }

  template <typename T>
  kj::Maybe<kj::Promise<T>> tryUnwrap(
        v8::Local<v8::Context> context, v8::Local<v8::Value> handle, kj::Promise<T>*,
        kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto& wrapper = static_cast<Self&>(*this);
    auto jsPromise = KJ_UNWRAP_OR_RETURN(wrapper.tryUnwrap(
        context, handle, (jsg::Promise<T>*)nullptr, parentObject), kj::none);
    auto& js = jsg::Lock::from(context->GetIsolate());
    return IoContext::current().awaitJs(js, kj::mv(jsPromise));
  }

  template <typename T>
  v8::Local<v8::Context> newContext(v8::Isolate* isolate, kj::Promise<T> value) = delete;
  template <bool isContext = false, typename T>
  v8::Local<v8::FunctionTemplate> getTemplate(v8::Isolate* isolate, kj::Promise<T>*) = delete;
};

} // namespace workerd
