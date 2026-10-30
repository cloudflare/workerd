// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>

#include <rust/cxx.h>

#include <cstdint>

// The IoContext calls `ffi.rs` makes. Rust identifies an IoContext by its IoContext::Id, as a
// uint64_t, and holds no reference to it. Each call below that takes an `id` uses the current
// IoContext, and fails unless `id` identifies it: ids are never reused, so a stale id cannot
// reach another request's IoContext, or one that has been destroyed.
namespace workerd::io_context {

// The message of the error IoContext::current() throws outside a request.
::rust::Str io_context_no_request_message();

// IoContext::tryGetCurrentId(), as a uint64_t.
kj::Maybe<uint64_t> io_context_try_current_id();

// IoContext::now(), in nanoseconds since the Unix epoch.
int64_t io_context_now(uint64_t id);

// IoContext::writeLogfwdr(), for a serialized Cap'n Proto message. In a Durable Object, the
// write waits for the output gate, as AnalyticsEngine::writeDataPoint() does.
void io_context_write_logfwdr(uint64_t id, uint32_t channel, ::rust::Slice<const uint8_t> message);

}  // namespace workerd::io_context
