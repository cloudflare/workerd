// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ that the Rust Hyperdrive (lib.rs) calls: the per-type shims of `Socket`, which it
// returns from `connect()`, and the per-method shims over the C++ that Hyperdrive uses. The
// per-method shims run their C++ in jsg_cpp::callCpp() and return false, with an exception pending,
// if it threw.

#include <workerd/rust/jsg-cpp/jsg-cpp.h>

#include <rust/cxx.h>

namespace workerd::api {

class Socket;

// Per-type shims of Socket.
kj::Maybe<kj::Own<rust::jsg_cpp::CppRef>> socket_try_unwrap(
    v8::Isolate* isolate, rust::jsg_cpp::Local value);
rust::jsg_cpp::MaybeLocal socket_wrap(v8::Isolate* isolate, const rust::jsg_cpp::CppRef& ref);

// Fills `output` with random bytes from getEntropy().
void hyperdrive_fill_entropy(::rust::Slice<uint8_t> output);

// registerHyperdriveOverrides() for the current request's global scope, routing the host and the
// synthetic IP through connectHyperdrive(), and storing the IP in `ip`.
bool hyperdrive_register_overrides(
    v8::Isolate* isolate, uint32_t channel, ::rust::Str host, uint16_t port, ::rust::String& ip);

// connectHyperdrive(), storing the Socket in `result`.
bool hyperdrive_connect(v8::Isolate* isolate,
    uint32_t channel,
    ::rust::Str host,
    uint16_t port,
    kj::Maybe<kj::Own<rust::jsg_cpp::CppRef>>& result);

}  // namespace workerd::api
