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

// IoContext::tryCurrent().
kj::Maybe<IoContext&> io_context_try_current();

// IoContext::getId(), as a uint64_t.
uint64_t io_context_id(const IoContext& context);

// IoContext::now(), in nanoseconds since the Unix epoch.
int64_t io_context_now(uint64_t id);

// IoContext::writeLogfwdr(), for a serialized Cap'n Proto message. In a Durable Object, the
// write waits for the output gate, as AnalyticsEngine::writeDataPoint() does.
void io_context_write_logfwdr(uint64_t id, uint32_t channel, ::rust::Slice<const uint8_t> message);

// Rust types, defined in ffi.rs.
struct IoFuture;
struct IoCallback;
struct IoOwnedSlot;

// IoContext::awaitIoAndRun(): waits for `future` as a task of the IoContext, holding a pending
// event, then runs `callback` with its output in IoContext::run(), in the critical section and
// async context frame that are current when this is called. Both are dropped if the IoContext is
// destroyed first.
void io_context_await_io(
    uint64_t id, ::rust::Box<IoFuture> future, ::rust::Box<IoCallback> callback);

// Holds what IoContext::addObject() returns for a Rust value, which the IoContext then destroys
// when it is destroyed, unless this is destroyed first.
class IoOwnedObject {
 public:
  explicit IoOwnedObject(kj::Own<void> object): object(kj::mv(object)) {}

 private:
  kj::Own<void> object;
};

// IoContext::addObject() for `slot`.
kj::Own<IoOwnedObject> io_context_add_object(uint64_t id, ::rust::Box<IoOwnedSlot> slot);

}  // namespace workerd::io_context
