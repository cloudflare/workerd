// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The per-type and per-method shims through which the Rust ExecutionContext (ffi.rs) uses C++
// resources. Each takes the isolate, locked by this thread with a context entered and a
// HandleScope open, and handles in that scope. The per-method shims run their C++ in
// jsg_cpp::callCpp(), and return false with an exception pending if it threw.
//
// These are C++ API code, like the classes they bind, not shims of Rust JSG: they live beside
// the API that uses them, in src/workerd/api, and use C++ JSG as those classes do, so the C++
// JSG allowlist of src/rust/jsg does not check them (src/rust/jsg/README.md, "Safety rules",
// rule 3). They go away with the C++ classes.

#include <workerd/rust/jsg-cpp/jsg-cpp.h>

#include <rust/cxx.h>

namespace workerd::api {
class AccessContext;
class CacheContext;
class Fetcher;
class Tracing;
}  // namespace workerd::api

namespace workerd::api::execution_context {

namespace jsg_cpp = ::workerd::rust::jsg_cpp;

// Per-type shims.
kj::Maybe<kj::Own<jsg_cpp::CppRef>> fetcher_try_unwrap(v8::Isolate* isolate, jsg_cpp::Local value);
jsg_cpp::MaybeLocal fetcher_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref);
kj::Maybe<kj::Own<jsg_cpp::CppRef>> tracing_try_unwrap(v8::Isolate* isolate, jsg_cpp::Local value);
jsg_cpp::MaybeLocal tracing_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref);
kj::Maybe<kj::Own<jsg_cpp::CppRef>> cache_context_try_unwrap(
    v8::Isolate* isolate, jsg_cpp::Local value);
jsg_cpp::MaybeLocal cache_context_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref);
kj::Maybe<kj::Own<jsg_cpp::CppRef>> access_context_try_unwrap(
    v8::Isolate* isolate, jsg_cpp::Local value);
jsg_cpp::MaybeLocal access_context_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref);

// Per-method shims.
bool fetcher_map_virtual_host(
    v8::Isolate* isolate, const Fetcher& fetcher, uint16_t port, ::rust::String& result);
bool new_tracing(v8::Isolate* isolate, kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result);
bool current_cache_context(v8::Isolate* isolate, kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result);
bool current_access_context(v8::Isolate* isolate, kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result);
jsg_cpp::MaybeLocal restore_current_entrypoint(v8::Isolate* isolate, jsg_cpp::Local params);

}  // namespace workerd::api::execution_context
