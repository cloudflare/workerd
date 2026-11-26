// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The shims through which Rust builds, inspects and hands back a `Frankenvalue` (ffi.rs). They are
// C++ API code, which uses C++ JSG as `Frankenvalue` does; see src/rust/jsg/README.md, "Safety
// rules", rule 3.
//
// Rust holds a `Frankenvalue` as a `kj::Own<Frankenvalue>`. A shim that takes an isolate takes the
// isolate of the caller's lock, locked by this thread with a context entered and a HandleScope
// open, and handles in that scope. It runs its C++ in jsg_cpp::callCpp(), and fails, with an
// exception pending, if the C++ threw.

#include <workerd/io/frankenvalue.h>
#include <workerd/rust/jsg-cpp/jsg-cpp.h>

namespace workerd::frankenvalue {

namespace jsg_cpp = ::workerd::rust::jsg_cpp;

// Frankenvalue(): the empty object.
kj::Own<Frankenvalue> frankenvalue_new();

// Frankenvalue::fromJs(js, value), stored in `result`. Returns false if it threw.
bool frankenvalue_from_js(
    v8::Isolate* isolate, jsg_cpp::Local value, kj::Maybe<kj::Own<Frankenvalue>>& result);

// value.toJs(js), stored in `result`. Returns false if it threw.
bool frankenvalue_to_js(
    v8::Isolate* isolate, const Frankenvalue& value, jsg_cpp::MaybeLocal& result);

// value.estimateSize().
size_t frankenvalue_estimate_size(const Frankenvalue& value);

// value.empty().
bool frankenvalue_is_empty(const Frankenvalue& value);

// value.clone(). Needs no isolate lock, but must run on the thread that owns the value's
// capabilities, as the Rust type ensures.
kj::Own<Frankenvalue> frankenvalue_clone(const Frankenvalue& value);

}  // namespace workerd::frankenvalue
