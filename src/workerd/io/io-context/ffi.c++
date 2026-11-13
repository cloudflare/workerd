// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/io-context/ffi.rs.h>
#include <workerd/rust/jsg/ffi-inl.h>

#include <v8-inspector.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/encoding.h>

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

namespace {

// AbortWatchers are created and destroyed only on the thread of the IoContext they watch.
thread_local size_t abortWatcherCount = 0;

}  // namespace

AbortWatcher::AbortWatcher(IoContext& context, ::rust::Box<AbortCallback> callback)
    : context(context),
      abortedAtStart(context.getAbortReason() != kj::none),
      // onAbort() never resolves: the IoContext only rejects its abort flag. The callback only
      // settles Rust's state and wakes its future, so it runs no JavaScript and needs no lock.
      watch(context.onAbort()
                .catch_([callback = kj::mv(callback)](
                            kj::Exception&& e) { callback->aborted(kj::mv(e)); })
                .eagerlyEvaluate(nullptr)) {
  ++abortWatcherCount;
}

AbortWatcher::~AbortWatcher() noexcept(false) {
  --abortWatcherCount;
}

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

size_t io_context_abort_watcher_count_for_test() {
  return abortWatcherCount;
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

void io_context_add_wait_until(uint64_t id, ::rust::Box<WaitUntilFuture> future) {
  currentWithId(id).addWaitUntil(wait_until_future_wait(kj::mv(future)));
}

// Rust JSG's shims don't use C++ JSG (src/rust/jsg/README.md, "Safety rules", rule 3). This
// function is the reviewed exception: it throws JsExceptionThrown, which is how IoContext::run()
// learns of an exception the callback left pending in V8. That use is counted in
// src/rust/jsg/cpp-jsg-allowlist.txt, so it stays within this function.
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
        // stack, and fails the task with it, as for C++ code it runs.
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

namespace {

// Text for the inspector, as jsg::toInspectorStringView() converts it: an 8-bit StringView is
// Latin-1, so only ASCII is passed as it is, and anything else as UTF-16.
class InspectorString {
 public:
  explicit InspectorString(kj::String text): text(kj::mv(text)) {
    for (auto c: this->text) {
      if (static_cast<kj::byte>(c) >= 0x80) {
        utf16 = kj::encodeUtf16(this->text);
        break;
      }
    }
  }
  KJ_DISALLOW_COPY_AND_MOVE(InspectorString);

  v8_inspector::StringView view() const {
    if (utf16 != nullptr) {
      return v8_inspector::StringView(
          reinterpret_cast<const uint16_t*>(utf16.begin()), utf16.size());
    }
    return v8_inspector::StringView(text.asBytes().begin(), text.size());
  }

 private:
  kj::String text;
  // Empty unless `text` is not ASCII.
  kj::Array<char16_t> utf16;
};

// `value` as C++ JSG describes a handle (kj::str(handle)): its detail string as UTF-8, up to its
// first NUL. kj::none if V8 fails, leaving an exception pending.
kj::Maybe<kj::String> describe(
    v8::Isolate* isolate, v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
  v8::Local<v8::String> detail;
  if (!value->ToDetailString(context).ToLocal(&detail)) return kj::none;
  v8::String::Utf8Value utf8(isolate, detail);
  if (*utf8 == nullptr) return kj::str("(couldn't stringify)");
  return kj::str(*utf8);
}

}  // namespace

bool io_context_send_exception_to_inspector(uint64_t id,
    ::workerd::rust::jsg::Isolate* isolate,
    UncaughtExceptionSource source,
    ::workerd::rust::jsg::Local exception) {
  auto& context = currentWithId(id);
  KJ_IF_SOME(inspector, context.getWorker().getIsolate().tryGetV8Inspector()) {
    // The calls below take the context.
    if (::workerd::rust::jsg::exceptionPending(isolate)) return false;
    auto value = ::workerd::rust::jsg::local_from_ffi<v8::Value>(kj::mv(exception));
    auto v8Context = isolate->GetCurrentContext();

    // From here on, as jsg::sendExceptionToInspector() does for an exception without a
    // v8::Message, such as one a promise rejected with. V8 creates a message from the current
    // stack trace, or the one the error captured.
    auto message = v8::Exception::CreateMessage(isolate, value);
    auto stackTrace = message->GetStackTrace();
    // The resource name is whatever the Script was compiled with, e.g. "worker.js".
    KJ_IF_SOME(scriptResourceName, describe(isolate, v8Context, message->GetScriptResourceName())) {
      KJ_IF_SOME(detailedMessage, describe(isolate, v8Context, message->Get())) {
        auto lineNumber = message->GetLineNumber(v8Context).FromMaybe(0);
        auto startColumn = message->GetStartColumn(v8Context).FromMaybe(0);
        InspectorString sourceText(kj::str(source));
        InspectorString detailedMessageText(kj::mv(detailedMessage));
        InspectorString urlText(kj::mv(scriptResourceName));
        // TODO(soon): EW-2636 Pass a real script ID as the last parameter instead of 0, here and
        //   in jsg::sendExceptionToInspector().
        inspector.exceptionThrown(v8Context, sourceText.view(), value, detailedMessageText.view(),
            urlText.view(), lineNumber, startColumn, inspector.createStackTrace(stackTrace), 0);
        return true;
      }
    }
    return false;
  }
  return true;
}

void io_context_log_error(::rust::Str message) {
  KJ_LOG(ERROR, kj::str(kj::arrayPtr(message.data(), message.size())));
}

}  // namespace workerd::io_context
