// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/rust/jsg-cpp/jsg-cpp.h>

namespace workerd::api {

class CacheContext;

// The embedder identifies its Rust implementation of the ctx.cache resource. The C++ type
// remains the RTTI delegate until the ExecutionContext itself is ported to Rust.
struct CacheContextTag {
  static constexpr const char* NAME = "CacheContext";
  using JsgRttiDelegate = jsg::Ref<CacheContext>;

  static kj::Maybe<bool> isInstance(v8::Isolate* isolate, rust::jsg_cpp::Local value);
};

using RustCacheContext = rust::jsg_cpp::RustRef<CacheContextTag>;

class CacheContextRef {
 public:
  CacheContextRef(jsg::Ref<CacheContext> value);
  CacheContextRef(RustCacheContext value);
  CacheContextRef(CacheContextRef&&);
  ~CacheContextRef() noexcept(false);

  using JsgRttiDelegate = jsg::Ref<CacheContext>;

  // The JavaScript object, for code that has no TypeWrapper to convert this with.
  jsg::JsObject getHandle(jsg::Lock& js);

  static v8::Local<v8::Value> jsgWrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      CacheContextRef cache) {
    KJ_SWITCH_ONEOF(cache.value) {
      KJ_CASE_ONEOF(cpp, jsg::Ref<CacheContext>) {
        return typeWrapper.wrap(js, context, creator, kj::mv(cpp));
      }
      KJ_CASE_ONEOF(rust, RustCacheContext) {
        return typeWrapper.wrap(js, context, creator, kj::mv(rust));
      }
    }
    KJ_UNREACHABLE;
  }

  // Only to satisfy SelfConvertible: `ctx.cache` is a getter-only property, so nothing unwraps a
  // CacheContextRef.
  static kj::Maybe<CacheContextRef> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    KJ_IF_SOME(cpp,
        typeWrapper.tryUnwrap(
            js, context, handle, static_cast<jsg::Ref<CacheContext>*>(nullptr), parentObject)) {
      return CacheContextRef(kj::mv(cpp));
    }
    KJ_IF_SOME(rust,
        typeWrapper.tryUnwrap(
            js, context, handle, static_cast<RustCacheContext*>(nullptr), parentObject)) {
      return CacheContextRef(kj::mv(rust));
    }
    return kj::none;
  }

 private:
  kj::OneOf<jsg::Ref<CacheContext>, RustCacheContext> value;
};

// The `ctx.cache` of the current request: the embedder's Rust implementation if it supplies one
// (Worker::Api::getCtxCacheRustProperty()), otherwise its C++ one (getCtxCacheProperty()), or
// none outside a request or when the embedder supplies neither.
jsg::Optional<CacheContextRef> currentCacheContext(jsg::Lock& js);

}  // namespace workerd::api
