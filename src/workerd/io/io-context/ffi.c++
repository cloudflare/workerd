// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/io-context/current-with-id.h>
#include <workerd/io/io-context/ffi.rs.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/inspector.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/worker/bridge.h>
#include <workerd/util/use-perfetto-categories.h>

#include <v8-inspector.h>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <bit>
#include <type_traits>

namespace workerd::io_context {

void incoming_request_delivered(IncomingRequest& request,
    ::rust::Slice<const kj::byte> eventType,
    ::rust::Slice<const kj::byte> file,
    ::rust::Slice<const kj::byte> function,
    uint32_t line,
    uint32_t column) {
  // kj::SourceLocation can only be constructed for its caller, so this one is the shim's location
  // with the Rust caller's fields assigned. Rust passes static CStrs, with their NUL.
  KJ_REQUIRE(file.size() > 0 && file.back() == '\0', "the file name is not NUL-terminated");
  KJ_REQUIRE(
      function.size() > 0 && function.back() == '\0', "the function name is not NUL-terminated");
  kj::SourceLocation location;
  location.fileName = reinterpret_cast<const char*>(file.data());
  location.function = reinterpret_cast<const char*>(function.data());
  location.lineNumber = line;
  location.columnNumber = column;
  request.delivered(kj::str(kj::from<kj_rs::Rust>(eventType).asChars()),
      DropQueuedCrossContextActions::NO, location);
}

void incoming_request_set_custom_event_info(IncomingRequest& request) {
  KJ_IF_SOME(tracer, request.getWorkerTracer()) {
    tracer.setEventInfo(request, tracing::CustomEventInfo());
  }
}

void incoming_request_assert_no_actor(IncomingRequest& request) {
  KJ_ASSERT(request.getContext().getActor() == kj::none);
}

void incoming_request_set_scheduled_event_info(
    IncomingRequest& request, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron) {
  KJ_IF_SOME(tracer, request.getWorkerTracer()) {
    double eventTime = (scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS;
    tracer.setEventInfo(request,
        tracing::ScheduledEventInfo(eventTime, kj::str(kj::from<kj_rs::Rust>(cron).asChars())));
  }
}

kj::Promise<::workerd::rust::worker::ScheduledResult> incoming_request_finish_scheduled(
    kj::Own<IncomingRequest> request) {
  // kj::evalNow() makes an exception that finishScheduled() throws reject the promise: cxx has no
  // way to carry a synchronous throw from an async function to Rust.
  return kj::evalNow([&]() {
    auto& self = *request;
    return self.finishScheduled(kj::mv(request));
  }).then([](WorkerInterface::ScheduledResult result) {
    return ::workerd::rust::worker::ScheduledResult{
      .retry = result.retry,
      .outcome = ::workerd::rust::worker::toRustOutcome(result.outcome),
    };
  });
}

namespace {

// IoContext::Id holds only the process-unique uint64_t that Rust stores.
static_assert(sizeof(IoContext::Id) == sizeof(uint64_t));
static_assert(std::is_trivially_copyable_v<IoContext::Id>);

// Wraps a `Literal` (see ffi.h) without copying it.
kj::ConstString wrapLiteral(Literal bytes) {
  KJ_ASSERT(bytes.size() > 0 && bytes.back() == '\0', "the literal is not NUL-terminated");
  return kj::ConstString(reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1,
      kj::NullArrayDisposer::instance);
}

// Sets a tag on a SpanBuilder or TraceContext; see ffi.h for which strings are copied.
template <typename Span, typename Value>
void setTag(Span& span, Literal key, Value value) {
  // A TraceContext also annotates its async trace operation, if it has one, whether or not its
  // spans are observed, so only a SpanBuilder's tag can be skipped here.
  if constexpr (kj::isSameType<Span, SpanBuilder>()) {
    if (!span.isObserved()) return;
  }
  if constexpr (kj::isSameType<Value, ::rust::Str>()) {
    span.setTag(wrapLiteral(key), kj::str(kj::arrayPtr(value.data(), value.size())));
  } else if constexpr (kj::isSameType<Value, Literal>()) {
    span.setTag(wrapLiteral(key), wrapLiteral(value));
  } else {
    span.setTag(wrapLiteral(key), value);
  }
}

// Copies `bytes`, which must be exactly one serialized Cap'n Proto message, to words: Rust does
// not align them.
kj::Array<capnp::word> copyMessage(kj::ArrayPtr<const kj::byte> bytes) {
  KJ_REQUIRE(bytes.size() > 0, "the serialized message is empty");
  KJ_REQUIRE(bytes.size() % sizeof(capnp::word) == 0,
      "the serialized message is not a whole number of words");
  auto words = kj::heapArray<capnp::word>(bytes.size() / sizeof(capnp::word));
  words.asBytes().copyFrom(bytes);
  capnp::FlatArrayMessageReader reader(words);
  KJ_REQUIRE(reader.getEnd() == words.end(), "bytes follow the serialized message");
  return words;
}

// IoContext::writeLogfwdr() for a message from copyMessage().
void writeMessage(IoContext& context, uint32_t channel, kj::ArrayPtr<const capnp::word> words) {
  capnp::FlatArrayMessageReader reader(words);
  context.writeLogfwdr(channel, [&](capnp::AnyPointer::Builder builder) {
    builder.set(reader.getRoot<capnp::AnyPointer>());
  });
}

// Calls `write(context)` now or, in a Durable Object, once the output gate opens, so that the
// message does not leave before the storage writes that precede it are confirmed, as in
// AnalyticsEngine::writeDataPoint(). A later write runs in a waitUntil task, which fails if it
// throws.
template <typename Write>
void afterOutputLocks(IoContext& context, Write write) {
  KJ_IF_SOME(outputLocks, context.waitForOutputLocksIfNecessary()) {
    context.addWaitUntil(
        outputLocks.then([weakRef = context.getWeakRef(), write = kj::mv(write)]() mutable {
      // The IoContext owns its waitUntil tasks, so it is alive while one runs.
      auto& context = KJ_ASSERT_NONNULL(weakRef->tryGet());
      // IoChannelFactory::writeLogfwdr() needs the current IoContext, so the write runs in it.
      return context.run(
          [write = kj::mv(write)](Worker::Lock&, IoContext& context) mutable { write(context); });
    }));
  } else {
    write(context);
  }
}

}  // namespace

::rust::Str io_context_no_request_message() {
  return IoContext::NO_REQUEST_MESSAGE.cStr();
}

kj::Exception io_context_never_settled_exception() {
  return IoContext::neverSettledException();
}

kj::Exception io_context_memory_limit_exception() {
  return IoContext::memoryLimitException();
}

kj::Own<IsolateWeakRef> io_context_isolate_weak_ref(uint64_t id) {
  return kj::heap<IsolateWeakRef>(currentWithId(id).getWorker().getIsolate().getWeakRef());
}

bool isolate_weak_ref_has_excessively_exceeded_heap_limit(const IsolateWeakRef& weakRef) {
  // Rust calls this when it drops the half of an await_js() wait that settles it: in a cppgc
  // finalizer or the GC epilogue, or without the isolate lock, where ~AwaitJsFulfiller() runs
  // rejectNeverSettled(). It enters no V8: tryAddStrongRef() takes the weak reference's mutex and
  // an atomic reference, and hasExcessivelyExceededHeapLimit() must not (see its declaration).
  return weakRef.ref->tryAddStrongRef()
      .map([](kj::Own<const Worker::Isolate> isolate) {
    return isolate->getLimitEnforcer().hasExcessivelyExceededHeapLimit();
  }).orDefault(false);
}

kj::Maybe<kj::Own<AsyncResource>> io_context_make_await_js_resource(uint64_t id) {
  auto resource = currentWithId(id).makeAwaitJsResource();
  // An inert resource, whose id is 0, reports nothing.
  if (resource.getId() == 0) return kj::none;
  return kj::heap(kj::mv(resource));
}

AbortWatcher::AbortWatcher(IoContext& context, ::rust::Box<AbortCallback> callback)
    : context(context),
      abortedAtStart(context.getAbortReason() != kj::none),
      // onAbort() never resolves: the IoContext only rejects its abort flag. The callback only
      // settles Rust's state and wakes its future, so it runs no JavaScript and needs no lock.
      watch(context.onAbort()
                .catch_([callback = kj::mv(callback)](
                            kj::Exception&& e) { callback->aborted(kj::mv(e)); })
                .eagerlyEvaluate(nullptr)) {}

AbortWatcher::~AbortWatcher() noexcept(false) {}

kj::Maybe<kj::Exception> AbortWatcher::abortedSinceStart() const {
  if (abortedAtStart) {
    return kj::none;
  }
  return context.getAbortReason();
}

bool abort_watcher_has_pending_abort(const AbortWatcher& watcher) {
  return watcher.abortedSinceStart() != kj::none;
}

kj::Exception abort_watcher_pending_abort(const AbortWatcher& watcher) {
  return KJ_REQUIRE_NONNULL(watcher.abortedSinceStart(), "the IoContext was not aborted since");
}

kj::Own<AbortWatcher> io_context_watch_abort(uint64_t id, ::rust::Box<AbortCallback> callback) {
  return kj::heap<AbortWatcher>(currentWithId(id), kj::mv(callback));
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
  // The message is checked now, so that an invalid one fails this call rather than the write.
  auto words = copyMessage(kj::arrayPtr(message.data(), message.size()));
  afterOutputLocks(context, [channel, words = kj::mv(words)](IoContext& context) {
    writeMessage(context, channel, words);
  });
}

void io_context_write_logfwdr_with(
    uint64_t id, uint32_t channel, ::rust::Box<LogfwdrMessage> message) {
  afterOutputLocks(
      currentWithId(id), [channel, message = kj::mv(message)](IoContext& context) mutable {
    auto bytes = logfwdr_message_build(kj::mv(message));
    writeMessage(context, channel, copyMessage(kj::arrayPtr(bytes.data(), bytes.size())));
  });
}

void io_context_new_analytics_engine_request(uint64_t id) {
  currentWithId(id).getLimitEnforcer().newAnalyticsEngineRequest();
}

void io_context_add_wait_until(uint64_t id, ::rust::Box<WaitUntilFuture> future) {
  currentWithId(id).addWaitUntil(wait_until_future_wait(kj::mv(future)));
}

namespace {

// Rust JSG's shims don't use C++ JSG (src/rust/jsg/README.md, "Safety rules", rule 3). This
// function is the reviewed exception: it throws JsExceptionThrown, which is how IoContext::run()
// learns of an exception, or a termination, that Rust left pending in V8, as for C++ code it runs.
// That use is counted in src/rust/jsg/cpp-jsg-allowlist.txt, so it stays within this function.
[[noreturn]] void throwPendingToRun() {
  throw jsg::JsExceptionThrown();
}

}  // namespace

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
        // The exception, or termination, is pending in V8. run() then logs the exception as
        // uncaught, with its stack, and fails the task with it.
        throwPendingToRun();
      case IoCallbackOutcome::Panicked:
        // The termination takes effect only when V8 next runs JavaScript, in run()'s microtask
        // checkpoint. Until then, run() would treat JsExceptionThrown as an ordinary exception and
        // convert it, running JavaScript, so the task fails with a KJ exception instead.
        KJ_FAIL_ASSERT("the callback of IoContext::await_io_and_run() panicked");
    }
    KJ_UNREACHABLE;
  });
}

void incoming_request_run_in_wait_until(
    IncomingRequest& request, kj::Own<RunPrelude> prelude, ::rust::Box<WaitUntilRun> run) {
  auto& context = request.getContext();
  context.addWaitUntil(
      context.run([prelude = kj::mv(prelude), run = kj::mv(run)](
                      Worker::Lock& lock, IoContext& context) mutable -> kj::Promise<void> {
    TRACE_EVENT("workerd", perfetto::StaticString(prelude->spanName().cStr()));
    auto traceScope = context.makeRequestTraceScope(lock);
    auto handles = prelude->run(lock, context);
    auto ffiHandles =
        KJ_MAP(handle, handles) { return ::workerd::rust::jsg::to_ffi(kj::mv(handle)); };
    switch (wait_until_run_start(reinterpret_cast<uintptr_t>(lock.getIsolate()), *run,
        ::rust::Slice<const ::workerd::rust::jsg::Local>(ffiHandles.begin(), ffiHandles.size()))) {
      case IoCallbackOutcome::Succeeded:
        return wait_until_run_wait(kj::mv(run));
      case IoCallbackOutcome::Failed:
        // As in io_context_await_io_and_run(): run() logs the exception as uncaught and fails
        // the task with it.
        throwPendingToRun();
      case IoCallbackOutcome::Panicked:
        // As in io_context_await_io_and_run().
        KJ_FAIL_ASSERT("the function of IoContext::IncomingRequest::run_in_wait_until() panicked");
    }
    KJ_UNREACHABLE;
  }));
}

ReentryCallback::ReentryCallback(IoContext& context, ::rust::Box<ReentryFn> function)
    : callback(context.makeReentryCallbackWeak([function = kj::mv(function)](Worker::Lock& lock,
                                                   IoContext&,
                                                   ::rust::Box<ReentryCall> call) mutable
          -> kj::Promise<void> {
        switch (reentry_fn_run(reinterpret_cast<uintptr_t>(lock.getIsolate()), *function, *call)) {
          case ReentryOutcome::Started:
            return reentry_call_wait(kj::mv(call));
          case ReentryOutcome::Terminated:
            // The termination is pending in V8. run() then handles it as for C++ code it runs: it
            // leaves the scope of the limit enforcer, fails with the exceeded limit or the
            // exception the request was aborted with, and has its microtask checkpoint cancel the
            // microtasks queued meanwhile.
            throwPendingToRun();
          case ReentryOutcome::Panicked:
            // jsg::catch_panic() threw an internal error and requested a termination, which run()'s
            // microtask checkpoint delivers. run() would convert a JsExceptionThrown, running
            // JavaScript first, so the call fails with a KJ exception instead.
            KJ_FAIL_ASSERT("the function of IoContext::make_reentry_callback_weak() panicked");
        }
        KJ_UNREACHABLE;
      })) {}

kj::Promise<void> ReentryCallback::call(::rust::Box<ReentryCall> call) const {
  return callback(kj::mv(call));
}

kj::Own<ReentryCallback> io_context_make_reentry_callback_weak(
    uint64_t id, ::rust::Box<ReentryFn> function) {
  return kj::heap<ReentryCallback>(currentWithId(id), kj::mv(function));
}

kj::Own<ReentryPromise> reentry_callback_call(
    const ReentryCallback& callback, ::rust::Box<ReentryCall> call) {
  return kj::heap<ReentryPromise>(callback.call(kj::mv(call)));
}

kj::Promise<void> reentry_promise_wait(kj::Own<ReentryPromise> promise) {
  return kj::mv(promise->promise);
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

void trace_context_detach_async(TraceContext& context) {
  context.detachAsync();
}

void io_context_set_fail_open(uint64_t id) {
  currentWithId(id).setFailOpen();
}

void io_context_check_current(uint64_t id) {
  currentWithId(id);
}

void io_context_abort(uint64_t id, kj::Exception reason) {
  currentWithId(id).abort(kj::mv(reason));
}

void io_context_abort_without_reason(uint64_t id) {
  currentWithId(id).abort(IoContext::abortWithoutReasonException());
}

bool io_context_should_log_uncaught_exceptions() {
  return kj::_::Debug::shouldLog(kj::LogSeverity::INFO);
}

void io_context_log_uncaught_exception_stack(UncaughtExceptionSource source, ::rust::Str text) {
  // Named as in Worker::Lock::logUncaughtException(), since KJ_LOG writes the names.
  auto stack = kj::str(kj::arrayPtr(text.data(), text.size()));
  KJ_LOG(INFO, "uncaught exception", source, stack);
}

void io_context_log_uncaught_exception_value(UncaughtExceptionSource source, ::rust::Str text) {
  auto exception = kj::str(kj::arrayPtr(text.data(), text.size()));
  KJ_LOG(INFO, "uncaught exception", source, exception);
}

bool io_context_send_exception_to_inspector(uint64_t id,
    ::workerd::rust::jsg::Isolate* isolate,
    UncaughtExceptionSource source,
    ::workerd::rust::jsg::Local exception) {
  auto& context = currentWithId(id);
  KJ_IF_SOME(inspector, context.getWorker().getIsolate().tryGetV8Inspector()) {
    // The calls below take the context.
    if (::workerd::rust::jsg::exceptionPending(isolate)) return false;
    auto value = ::workerd::rust::jsg::local_from_ffi<v8::Value>(kj::mv(exception));
    // As jsg::sendExceptionToInspector() does for an exception without a v8::Message, such as one
    // a promise rejected with. V8 creates a message from the current stack trace, or the one the
    // error captured.
    auto message = v8::Exception::CreateMessage(isolate, value);
    return jsg::sendExceptionToInspector(isolate, inspector, kj::str(source), value, message);
  }
  return true;
}

void io_context_log_error(::rust::Str message) {
  KJ_LOG(ERROR, kj::str(kj::arrayPtr(message.data(), message.size())));
}

}  // namespace workerd::io_context
