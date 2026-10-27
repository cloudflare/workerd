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

// Span names, tag keys and literal tag values below are passed as `literal` bytes: a
// NUL-terminated string including its NUL, which must outlive the span, as a string literal
// does. They are not copied.
using Literal = ::rust::Slice<const uint8_t>;

// IoContext::makeTraceSpan() and makeUserTraceSpan(), in the current async context frame.
kj::Own<SpanBuilder> io_context_make_trace_span(uint64_t id, Literal name);
kj::Own<TraceContext> io_context_make_user_trace_span(uint64_t id, Literal name);

// SpanBuilder::newChild().
kj::Own<SpanBuilder> span_builder_new_child(SpanBuilder& span, Literal name);

// SpanBuilder::setTag() and TraceContext::setTag(). On a span that is not observed, these do
// nothing. On one that is, a `::rust::Str` value is copied, since it is not NUL-terminated and
// may not outlive the call; the key and a `Literal` value are not.
void span_builder_set_tag_bool(SpanBuilder& span, Literal key, bool value);
void span_builder_set_tag_int(SpanBuilder& span, Literal key, int64_t value);
void span_builder_set_tag_float(SpanBuilder& span, Literal key, double value);
void span_builder_set_tag_str(SpanBuilder& span, Literal key, ::rust::Str value);
void span_builder_set_tag_literal(SpanBuilder& span, Literal key, Literal value);
void trace_context_set_tag_bool(TraceContext& context, Literal key, bool value);
void trace_context_set_tag_int(TraceContext& context, Literal key, int64_t value);
void trace_context_set_tag_float(TraceContext& context, Literal key, double value);
void trace_context_set_tag_str(TraceContext& context, Literal key, ::rust::Str value);
void trace_context_set_tag_literal(TraceContext& context, Literal key, Literal value);

}  // namespace workerd::io_context
