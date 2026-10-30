// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/io-context/ffi.rs.h>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <bit>
#include <type_traits>

namespace workerd::io_context {

namespace {

// IoContext::Id holds only the process-unique uint64_t that Rust stores.
static_assert(sizeof(IoContext::Id) == sizeof(uint64_t));
static_assert(std::is_trivially_copyable_v<IoContext::Id>);

// The current IoContext, which must be the one `id` identifies.
IoContext& currentWithId(uint64_t id) {
  auto& context = KJ_REQUIRE_NONNULL(IoContext::tryCurrent(), "there is no current request");
  KJ_REQUIRE(context.getId() == std::bit_cast<IoContext::Id>(id),
      "the IoContext is not the one of the current request");
  return context;
}

// Wraps a `Literal` (see ffi.h) without copying it.
kj::ConstString wrapLiteral(Literal bytes) {
  KJ_ASSERT(bytes.size() > 0 && bytes.back() == '\0', "the literal is not NUL-terminated");
  return kj::ConstString(reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1,
      kj::NullArrayDisposer::instance);
}

// Sets a tag on a SpanBuilder or TraceContext, if the span is observed; see ffi.h for which
// strings are copied.
template <typename Span, typename Value>
void setTag(Span& span, Literal key, Value value) {
  if (!span.isObserved()) return;
  if constexpr (kj::isSameType<Value, ::rust::Str>()) {
    span.setTag(wrapLiteral(key), kj::str(kj::arrayPtr(value.data(), value.size())));
  } else if constexpr (kj::isSameType<Value, Literal>()) {
    span.setTag(wrapLiteral(key), wrapLiteral(value));
  } else {
    span.setTag(wrapLiteral(key), value);
  }
}

}  // namespace

::rust::Str io_context_no_request_message() {
  return IoContext::NO_REQUEST_MESSAGE.cStr();
}

kj::Maybe<uint64_t> io_context_try_current_id() {
  return IoContext::tryGetCurrentId().map(
      [](IoContext::Id id) { return std::bit_cast<uint64_t>(id); });
}

void io_context_require_current(uint64_t id) {
  IoContext::requireCurrentOrThrowJs(std::bit_cast<IoContext::Id>(id));
}

int64_t io_context_now(uint64_t id) {
  return (currentWithId(id).now() - kj::UNIX_EPOCH) / kj::NANOSECONDS;
}

void io_context_write_logfwdr(uint64_t id, uint32_t channel, ::rust::Slice<const uint8_t> message) {
  auto& context = currentWithId(id);

  // Rust does not align the bytes to words, so they are copied.
  auto bytes = kj::arrayPtr(message.data(), message.size());
  KJ_REQUIRE(bytes.size() > 0, "the serialized message is empty");
  KJ_REQUIRE(bytes.size() % sizeof(capnp::word) == 0,
      "the serialized message is not a whole number of words");
  auto words = kj::heapArray<capnp::word>(bytes.size() / sizeof(capnp::word));
  words.asBytes().copyFrom(bytes);
  capnp::FlatArrayMessageReader reader(words);
  KJ_REQUIRE(reader.getEnd() == words.end(), "bytes follow the serialized message");

  KJ_IF_SOME(outputLocks, context.waitForOutputLocksIfNecessary()) {
    // In a Durable Object, the message must not leave before the storage writes that precede it
    // are confirmed, as in AnalyticsEngine::writeDataPoint(). The message is copied now, so that
    // an invalid one fails this call rather than the write.
    auto copy = kj::heap<capnp::MallocMessageBuilder>();
    copy->getRoot<capnp::AnyPointer>().set(reader.getRoot<capnp::AnyPointer>());
    context.addWaitUntil(
        outputLocks.then([weakRef = context.getWeakRef(), channel, copy = kj::mv(copy)]() mutable {
      // The IoContext owns its waitUntil tasks, so it is alive while one runs.
      auto& context = KJ_ASSERT_NONNULL(weakRef->tryGet());
      // IoChannelFactory::writeLogfwdr() needs the current IoContext, so the write runs in it.
      return context.run([channel, copy = kj::mv(copy)](Worker::Lock&, IoContext& context) mutable {
        context.writeLogfwdr(channel, [&](capnp::AnyPointer::Builder builder) {
          builder.set(copy->getRoot<capnp::AnyPointer>().asReader());
        });
      });
    }));
  } else {
    context.writeLogfwdr(channel, [&](capnp::AnyPointer::Builder builder) {
      builder.set(reader.getRoot<capnp::AnyPointer>());
    });
  }
}

void io_context_await_io_and_run(
    uint64_t id, ::rust::Box<IoFuture> future, ::rust::Box<IoCallback> callback) {
  auto& context = currentWithId(id);
  // If the future panics, awaitIoAndRun() fails the task, dropping the callback without running it.
  context.awaitIoAndRun(io_future_wait(kj::mv(future)),
      [callback = kj::mv(callback)](Worker::Lock& lock, ::rust::Box<IoOutput>&& output) mutable {
    auto isolate = reinterpret_cast<uintptr_t>(lock.getIsolate());
    switch (io_callback_run(isolate, kj::mv(callback), kj::mv(output))) {
      case IoCallbackOutcome::Succeeded:
        return;
      case IoCallbackOutcome::Failed:
        // The exception, or termination, is pending in V8. JsExceptionThrown is how
        // IoContext::run() expects to learn of it: it then logs the exception as uncaught, with its
        // stack, and fails the task with it, as for C++ code it runs. This is the one use of C++
        // JSG here, counted in src/rust/jsg/cpp-jsg-allowlist.txt.
        throw jsg::JsExceptionThrown();
      case IoCallbackOutcome::Panicked:
        // The termination takes effect only when V8 next runs JavaScript, in run()'s microtask
        // checkpoint. Until then, run() would treat JsExceptionThrown as an ordinary exception and
        // convert it, running JavaScript, so the task fails with a KJ exception instead.
        KJ_FAIL_ASSERT("the callback of IoContext::await_io_and_run() panicked");
    }
    KJ_UNREACHABLE;
  });
}

kj::Own<IoOwnedObject> io_context_add_object(uint64_t id, ::rust::Box<IoOwnedSlot> slot) {
  auto& context = currentWithId(id);
  return kj::heap<IoOwnedObject>(
      context.addObject(kj::heap<::rust::Box<IoOwnedSlot>>(kj::mv(slot))));
}

kj::Own<SpanBuilder> io_context_make_trace_span(uint64_t id, Literal name) {
  return kj::heap(currentWithId(id).makeTraceSpan(wrapLiteral(name)));
}

kj::Own<TraceContext> io_context_make_user_trace_span(uint64_t id, Literal name) {
  return kj::heap(currentWithId(id).makeUserTraceSpan(wrapLiteral(name)));
}

kj::Own<SpanBuilder> span_builder_new_child(SpanBuilder& span, Literal name) {
  return kj::heap(span.newChild(wrapLiteral(name)));
}

void span_builder_set_tag_bool(SpanBuilder& span, Literal key, bool value) {
  setTag(span, key, value);
}

void span_builder_set_tag_int(SpanBuilder& span, Literal key, int64_t value) {
  setTag(span, key, value);
}

void span_builder_set_tag_float(SpanBuilder& span, Literal key, double value) {
  setTag(span, key, value);
}

void span_builder_set_tag_str(SpanBuilder& span, Literal key, ::rust::Str value) {
  setTag(span, key, value);
}

void span_builder_set_tag_literal(SpanBuilder& span, Literal key, Literal value) {
  setTag(span, key, value);
}

void trace_context_set_tag_bool(TraceContext& context, Literal key, bool value) {
  setTag(context, key, value);
}

void trace_context_set_tag_int(TraceContext& context, Literal key, int64_t value) {
  setTag(context, key, value);
}

void trace_context_set_tag_float(TraceContext& context, Literal key, double value) {
  setTag(context, key, value);
}

void trace_context_set_tag_str(TraceContext& context, Literal key, ::rust::Str value) {
  setTag(context, key, value);
}

void trace_context_set_tag_literal(TraceContext& context, Literal key, Literal value) {
  setTag(context, key, value);
}

}  // namespace workerd::io_context
