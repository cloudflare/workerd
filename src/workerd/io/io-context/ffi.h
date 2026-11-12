// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>
#include <workerd/rust/jsg/ffi.h>

#include <rust/cxx.h>

#include <cstdint>

// The IoContext calls `ffi.rs` makes. Rust identifies an IoContext by its IoContext::Id, as a
// uint64_t, and holds no reference to it. Each call below that takes an `id` uses the current
// IoContext, and fails unless `id` identifies it: ids are never reused, so a stale id cannot
// reach another request's IoContext, or one that has been destroyed.
namespace workerd::io_context {

// The message of the error IoContext::current() throws outside a request.
::rust::Str io_context_no_request_message();

// The message of the Error with which IoContext::awaitJs() fails if JavaScript never settles the
// promise.
::rust::Str io_context_never_settled_message();

// Throws IoContext::memoryLimitException(), which Rust receives as the error it fails with, with
// the exception's type, description, location and details.
void io_context_throw_memory_limit_exception();

// A weak reference to the Worker::Isolate of an IoContext, from Worker::Isolate::getWeakRef(), as
// IoContext::awaitJs() takes one. It may outlive the isolate, and may be used and destroyed on
// any thread without the isolate lock: AtomicWeakRef's refcount is atomic, and a mutex guards its
// pointer to the isolate, which the isolate clears when it is destroyed. A struct of its own,
// since Rust can't name a kj::Own<const T>.
struct IsolateWeakRef {
  kj::Own<const Worker::Isolate::WeakIsolateRef> ref;
};

// A weak reference to the isolate of the IoContext that `id` identifies.
kj::Own<IsolateWeakRef> io_context_isolate_weak_ref(uint64_t id);

// True if the isolate still exists and IsolateLimitEnforcer::hasExcessivelyExceededHeapLimit()
// says so, which is how IoContext::AwaitJsFulfiller::rejectNeverSettled() decides. Enters no V8
// and needs no isolate lock, so it may be called in a GC finalizer. The caller must keep the
// isolate alive meanwhile, as the IoContext that owns an await_js() wait does through its
// Worker: the strong reference this takes must not be the last one, whose release would destroy
// the isolate.
bool isolate_weak_ref_has_excessively_exceeded_heap_limit(const IsolateWeakRef& weakRef);

// IoContext::tryGetCurrentId(), as a uint64_t.
kj::Maybe<uint64_t> io_context_try_current_id();

// IoContext::requireCurrentOrThrowJs(): throws the error for an I/O object used by another
// request unless `id` identifies the current IoContext.
void io_context_require_current(uint64_t id);

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
// destroyed first. If `future` panics, or `callback` fails or panics, the task fails.
void io_context_await_io_and_run(
    uint64_t id, ::rust::Box<IoFuture> future, ::rust::Box<IoCallback> callback);

// Holds what IoContext::addObject() returns for a Rust value, which the IoContext then destroys
// when it is destroyed, unless this is destroyed first.
class IoOwnedObject {
 public:
  explicit IoOwnedObject(IoOwn<::rust::Box<IoOwnedSlot>> object): object(kj::mv(object)) {}

 private:
  IoOwn<::rust::Box<IoOwnedSlot>> object;
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

// SpanBuilder::setTag() and TraceContext::setTag(). On a SpanBuilder that is not observed, these
// do nothing. Otherwise, a `::rust::Str` value is copied, since it is not NUL-terminated and may
// not outlive the call; the key and a `Literal` value are not. A TraceContext's tag also annotates
// its async trace operation, if it has one, whether or not its spans are observed, as
// TraceContext::setTag() does.
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

// TraceContext::detachAsync().
void trace_context_detach_async(TraceContext& context);

// IoContext::setFailOpen(), for ctx.passThroughOnException().
void io_context_set_fail_open(uint64_t id);

// Fails unless `id` identifies the current IoContext, as every call here that takes one does.
void io_context_check_current(uint64_t id);

// IoContext::abort() with `reason`, which Rust encoded with jsg::Lock::exception_to_kj(), for
// ctx.abort(reason). Does not terminate execution.
void io_context_abort(uint64_t id, kj::Exception reason);

// IoContext::abort() with IoContext::abortWithoutReasonException(), for ctx.abort() without a
// reason. Does not terminate execution.
void io_context_abort_without_reason(uint64_t id);

// The call below uses C++ JSG, as a reviewed exception; see "C++ JSG" in ffi.c++. It takes a
// handle in the scope of the caller's lock, and returns false if JavaScript threw, leaving the
// exception pending, which Rust turns into a jsg::Error for which is_pending() is true.

// IoContext::addWaitUntil(IoContext::awaitJs(promise)), for ctx.waitUntil().
bool io_context_add_wait_until(uint64_t id, ::workerd::rust::jsg::Local promise);

// Logs `message` as an error, as KJ_LOG(ERROR) does.
void io_context_log_error(::rust::Str message);

// The parts of Worker::Lock::logUncaughtException(source, exception, message) that need C++, for
// a JavaScript value and without a v8::Message. Rust reads the value's `stack` itself.

// Whether KJ logs at INFO level (workerd --verbose), at which uncaught exceptions are logged.
bool io_context_should_log_uncaught_exceptions();

// KJ_LOG(INFO, "uncaught exception", source, stack), as an uncaught exception that is an object
// whose `stack` is not undefined is logged.
void io_context_log_uncaught_exception_stack(UncaughtExceptionSource source, ::rust::Str stack);

// KJ_LOG(INFO, "uncaught exception", source, exception), as any other uncaught exception is logged.
void io_context_log_uncaught_exception_value(UncaughtExceptionSource source, ::rust::Str exception);

// If the isolate of the current IoContext has an inspector, reports `exception` to it as thrown,
// as jsg::sendExceptionToInspector() does with no v8::Message: with a message V8 creates for the
// value, and `source` as its text. If it has one, returns false, and reports nothing, if an
// exception or a termination is pending, or if V8 fails to describe the message, leaving an
// exception pending. Takes a handle in the scope of the caller's lock.
bool io_context_send_exception_to_inspector(uint64_t id,
    ::workerd::rust::jsg::Isolate* isolate,
    UncaughtExceptionSource source,
    ::workerd::rust::jsg::Local exception);

}  // namespace workerd::io_context
