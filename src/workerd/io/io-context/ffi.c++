// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/io-context/ffi.rs.h>
#include <workerd/rust/jsg/ffi-inl.h>

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
        // stack, and fails the task with it, as for C++ code it runs. See "C++ JSG" below.
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

void io_context_set_fail_open(uint64_t id) {
  currentWithId(id).setFailOpen();
}

// C++ JSG
//
// Rust JSG's shims don't use C++ JSG (src/rust/jsg/README.md, "Safety rules", rule 3). This file
// has reviewed exceptions. io_context_await_io_and_run(), above, throws JsExceptionThrown, which
// is how IoContext::run() learns of an exception. The two functions below call
// IoContext::awaitJs() and jsg::Lock::exceptionToKj(), which ctx.waitUntil() and ctx.abort()
// call, so that a Rust implementation matches C++ exactly rather than reimplementing them over
// V8. Each use of C++ JSG, including getCurrentLock(), is counted in
// src/rust/jsg/cpp-jsg-allowlist.txt, so it stays within these three functions.
//
// They rely on this invariant: once currentWithId() has passed, the IoContext is current, which
// it is only inside IoContext::run() or a call from the JavaScript it runs, under the
// Worker::Lock that getCurrentLock() returns. That is the isolate lock the caller's Rust
// jsg::Lock stands for, so C++ JSG may use it, and the handles Rust passes belong to the
// caller's open HandleScope, in the IoContext's context.
//
// When C++ JSG throws JsExceptionThrown, the JavaScript exception, or termination, is pending in
// V8, where C++ JSG left it; the functions return false, and Rust passes the exception on.

bool io_context_add_wait_until(uint64_t id, ::workerd::rust::jsg::Local promise) {
  auto& context = currentWithId(id);
  auto handle = ::workerd::rust::jsg::local_from_ffi<v8::Promise>(kj::mv(promise));
  try {
    jsg::Lock& js = context.getCurrentLock();
    // As PromiseWrapper::tryUnwrap() does for a kj::Promise<void> argument once it has
    // converted it to a jsg::Promise<void>, which Rust's VoidPromise has done.
    context.addWaitUntil(context.awaitJs(js, jsg::Promise<void>(js.v8Isolate, handle)));
    return true;
  } catch (jsg::JsExceptionThrown&) {
    return false;
  }
}

bool io_context_abort(uint64_t id, ::workerd::rust::jsg::MaybeLocal reason) {
  auto& context = currentWithId(id);
  auto maybeReason = ::workerd::rust::jsg::maybe_local_from_ffi<v8::Value>(kj::mv(reason));
  try {
    // As ExecutionContext::abort() does, before it terminates execution.
    v8::Local<v8::Value> handle;
    if (maybeReason.ToLocal(&handle)) {
      jsg::Lock& js = context.getCurrentLock();
      context.abort(js.exceptionToKj(jsg::JsValue(handle)));
    } else {
      context.abort(IoContext::abortWithoutReasonException());
    }
    return true;
  } catch (jsg::JsExceptionThrown&) {
    return false;
  }
}

}  // namespace workerd::io_context
