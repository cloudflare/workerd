// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>
#include <workerd/rust/jsg/ffi.h>

#include <rust/cxx.h>

#include <cstdint>

// The IoContext calls `ffi.rs` makes.
namespace workerd::rust::io_context {

// IoContext::current(). Outside a request, `exception` receives the JavaScript error it throws.
IoContext& io_context_current(jsg::Isolate* isolate, jsg::MaybeLocal& exception);

// IoContext::tryCurrent(), or null.
IoContext* io_context_try_current();

// IoContext::now(), in nanoseconds since the Unix epoch.
int64_t io_context_now(IoContext& context);

// IoContext::writeLogfwdr(), for a serialized Cap'n Proto message.
void io_context_write_logfwdr(
    IoContext& context, uint32_t channel, ::rust::Slice<const uint8_t> message);

}  // namespace workerd::rust::io_context
