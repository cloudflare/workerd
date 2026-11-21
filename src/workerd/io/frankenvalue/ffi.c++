// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/rust/jsg/ffi-inl.h>

namespace workerd::frankenvalue {

kj::Own<Frankenvalue> frankenvalue_new() {
  return kj::heap<Frankenvalue>();
}

bool frankenvalue_from_js(
    v8::Isolate* isolate, jsg_cpp::Local value, kj::Maybe<kj::Own<Frankenvalue>>& result) {
  return jsg_cpp::callCpp(isolate, [&](jsg::Lock& js) {
    auto handle = ::workerd::rust::jsg::local_from_ffi<v8::Value>(kj::mv(value));
    result = kj::heap(Frankenvalue::fromJs(js, jsg::JsValue(handle)));
  });
}

// Rust's `&T` of an opaque type is interior-mutable, so the shims below may call non-const
// methods.

bool frankenvalue_to_js(
    v8::Isolate* isolate, const Frankenvalue& value, jsg_cpp::MaybeLocal& result) {
  return jsg_cpp::callCpp(isolate, [&](jsg::Lock& js) {
    // toJs() escapes its value from a HandleScope of its own into the caller's.
    v8::Local<v8::Value> handle = const_cast<Frankenvalue&>(value).toJs(js);
    result = ::workerd::rust::jsg::maybe_local_to_ffi(v8::MaybeLocal<v8::Value>(handle));
  });
}

size_t frankenvalue_estimate_size(const Frankenvalue& value) {
  return value.estimateSize();
}

bool frankenvalue_is_empty(const Frankenvalue& value) {
  return value.empty();
}

kj::Own<Frankenvalue> frankenvalue_clone(const Frankenvalue& value) {
  return kj::heap(const_cast<Frankenvalue&>(value).clone());
}

}  // namespace workerd::frankenvalue
