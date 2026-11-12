// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests the io-context crate inside and outside a request of a real worker. The Rust half is
// test_bridge.rs.

#include <workerd/io/async-trace.h>
#include <workerd/io/io-context/ffi.h>
#include <workerd/io/io-context/test-bridge-state.h>
#include <workerd/io/io-context/test_bridge.rs.h>
#include <workerd/jsg/inspector.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/modules.h>
#include <workerd/rust/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>

#include <v8-inspector.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/test.h>

#include <bit>
#include <memory>

namespace workerd::io_context::test {
namespace {

uintptr_t isolateOf(const TestFixture::V8Environment& env) {
  return reinterpret_cast<uintptr_t>(env.isolate);
}

uint64_t idOf(const IoContext& context) {
  return std::bit_cast<uint64_t>(context.getId());
}

int64_t nanosNow(IoContext& context) {
  return (context.now() - kj::UNIX_EPOCH) / kj::NANOSECONDS;
}

::rust::Slice<const uint8_t> slice(kj::ArrayPtr<const kj::byte> bytes) {
  return ::rust::Slice<const uint8_t>(bytes.begin(), bytes.size());
}

// A serialized message whose root is `text`.
kj::Array<kj::byte> serialize(kj::StringPtr text) {
  capnp::MallocMessageBuilder message;
  message.initRoot<capnp::AnyPointer>().setAs<capnp::Text>(text);
  auto words = capnp::messageToFlatArray(message);
  return kj::heapArray(words.asBytes());
}

// What the worker wrote to logfwdr, as "<channel>: <root text>" for each message.
struct LogfwdrLog {
  kj::Vector<kj::String> writes;
};

struct RecordingIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
  RecordingIoChannelFactory(TimerChannel& timer, LogfwdrLog& log)
      : DummyIoChannelFactory(timer),
        log(log) {}

  kj::Promise<void> writeLogfwdr(
      uint channel, kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage) override {
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<capnp::AnyPointer>();
    buildMessage(root);
    log.writes.add(kj::str(channel, ": ", root.getAs<capnp::Text>()));
    return kj::READY_NOW;
  }

  LogfwdrLog& log;
};

TestFixture::SetupParams recordingParams(
    LogfwdrLog& log, kj::Maybe<Worker::Actor::Id> actorId = kj::none) {
  return {
    .actorId = kj::mv(actorId),
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&log](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<RecordingIoChannelFactory>(timer, log);
  }),
  };
}

KJ_TEST("inside a request, current() finds it, and now() is its clock") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(has_current(isolateOf(env)));
    // The clock can advance between reads.
    auto before = nanosNow(env.context);
    auto now = now_nanos(isolateOf(env));
    KJ_EXPECT(before <= now && now <= nanosNow(env.context), before, now);
  });
}

KJ_TEST("now_in_current_request() is the request's clock, without a lock") {
  // A request clock that the system clock never reads.
  constexpr int64_t fixedNanos = 1'000'000'000'000;
  TestFixture fixture({.fixedNow = kj::UNIX_EPOCH + fixedNanos * kj::NANOSECONDS});
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_ASSERT(nanosNow(env.context) == fixedNanos);
    KJ_EXPECT(now_in_current_request_nanos() == fixedNanos);
  });
}

KJ_TEST("now_in_current_request() is none outside a request") {
  KJ_EXPECT_THROW_MESSAGE("not in a request", now_in_current_request_nanos());
}

KJ_TEST("write_logfwdr() writes the message to the channel") {
  LogfwdrLog log;
  TestFixture fixture(recordingParams(log));
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto message = serialize("hello");
    write_logfwdr(isolateOf(env), 7, slice(message));
  });
  KJ_ASSERT(log.writes.size() == 1);
  KJ_EXPECT(log.writes[0] == "7: hello");
}

KJ_TEST("write_logfwdr() rejects anything but exactly one message") {
  LogfwdrLog log;
  TestFixture fixture(recordingParams(log));
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto message = serialize("hello");
    KJ_EXPECT_THROW_MESSAGE(
        "the serialized message is empty", write_logfwdr(isolateOf(env), 7, slice(nullptr)));
    KJ_EXPECT_THROW_MESSAGE("not a whole number of words",
        write_logfwdr(isolateOf(env), 7, slice(message.first(message.size() - 1))));
    auto trailing = kj::heapArray<kj::byte>(message.size() + sizeof(capnp::word));
    trailing.first(message.size()).copyFrom(message);
    trailing.slice(message.size()).fill(0);
    KJ_EXPECT_THROW_MESSAGE(
        "bytes follow the serialized message", write_logfwdr(isolateOf(env), 7, slice(trailing)));
  });
  KJ_EXPECT(log.writes.size() == 0);
}

KJ_TEST("in a Durable Object, write_logfwdr() waits for the output gate") {
  LogfwdrLog log;
  TestFixture fixture(recordingParams(log, Worker::Actor::Id(kj::str("logfwdr-actor"))));
  auto gate = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(gate.promise), nullptr);

  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto message = serialize("after the gate");
    write_logfwdr(isolateOf(env), 3, slice(message));
  });
  fixture.pollEventLoop();
  KJ_EXPECT(log.writes.size() == 0);

  gate.fulfiller->fulfill();
  blocker.wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  KJ_ASSERT(log.writes.size() == 1);
  KJ_EXPECT(log.writes[0] == "3: after the gate");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("an IoContext id works only while its IoContext is current") {
  TestFixture fixture;
  auto first = fixture.newIncomingRequest();
  uint64_t firstId = 0;
  fixture.enterContext(*first, [&](const TestFixture::Environment& env) {
    firstId = idOf(env.context);
    auto before = nanosNow(env.context);
    auto now = io_context_now(firstId);
    KJ_EXPECT(before <= now && now <= nanosNow(env.context), before, now);
  });

  auto second = fixture.newIncomingRequest();
  fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
    KJ_EXPECT(idOf(env.context) != firstId);
    KJ_EXPECT_THROW_MESSAGE("not the one of the current request", io_context_now(firstId));
  });

  fixture.enterWorkerLockSynchronously([&](Worker::Lock&) {
    KJ_EXPECT_THROW_MESSAGE("there is no current request", io_context_now(firstId));
  });

  fixture.drainAndDestroy(kj::mv(second));
  fixture.drainAndDestroy(kj::mv(first));
}

KJ_TEST("await_io_and_run() runs its callback in the async context frame that was current") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    jsg::AsyncContextFrame::StorageScope scope(env.js, TEST_KEY.addRef(),
        env.js.v8Ref(v8::Local<v8::Value>(env.js.str("of the caller"_kj))));
    await_and_record_async_context(isolateOf(env));
  });
  fixture.pollEventLoop();
  KJ_EXPECT(recordedAsyncContext == "of the caller", recordedAsyncContext);
  fixture.drainAndDestroy(kj::mv(request));
}

// Records the KJ-to-JS bridges and operations an async tracker creates, their annotations, and the
// causes of turns.
class BridgeListener final: public AsyncTraceListener {
 public:
  explicit BridgeListener(kj::Vector<kj::String>& events): events(events) {}

  void onContextBegin(uint64_t, uint64_t) override {}
  void onInit(uint64_t, const AsyncInitEvent& e) override {
    if (e.kind == AsyncKind::KJ_TO_JS) events.add(kj::str("bridge ", e.id, " ", e.name));
    if (e.kind == AsyncKind::OPERATION) events.add(kj::str("operation ", e.id, " ", e.name));
  }
  void onSettle(uint64_t, AsyncId, AsyncOutcome, uint64_t) override {}
  void onAnnotate(
      uint64_t, AsyncId id, kj::ArrayPtr<const char> key, kj::ArrayPtr<const char> value) override {
    events.add(kj::str("annotate ", id, " ", key, "=", value));
  }
  void onBefore(uint64_t, AsyncId, uint64_t) override {}
  void onAfter(uint64_t, AsyncId, uint64_t) override {}
  void onDestroy(uint64_t, AsyncId, uint64_t) override {}
  void onTurn(uint64_t, const AsyncTurn& turn) override {
    events.add(kj::str("turn cause=", turn.cause));
  }
  void onContextEnd(uint64_t, uint64_t, const AsyncContextStats&) override {}

 private:
  kj::Vector<kj::String>& events;
};

// Enables async tracing, with a `Listener` that records into `events` as the sink.
template <typename Listener>
class TraceObserver final: public IsolateObserver {
 public:
  explicit TraceObserver(kj::Vector<kj::String>& events): events(events) {}

  kj::Maybe<AsyncTraceConfig> getAsyncTraceConfig() const override {
    return AsyncTraceConfig{};
  }
  void addAsyncTraceSinks(
      AsyncTraceSinks& sinks, kj::StringPtr, kj::Maybe<kj::StringPtr>) const override {
    sinks.add(kj::heap<Listener>(events));
  }

 private:
  kj::Vector<kj::String>& events;
};

using BridgeObserver = TraceObserver<BridgeListener>;

KJ_TEST("await_io_and_run() is recorded for async tracing, as awaitIo() is") {
  kj::Vector<kj::String> events;
  TestFixture fixture({
    .useRealTimers = false,
    .isolateObserver = kj::atomicRefcounted<BridgeObserver>(events),
  });
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request,
      [&](const TestFixture::Environment& env) { await_and_record_async_context(isolateOf(env)); });
  fixture.pollEventLoop();
  fixture.drainAndDestroy(kj::mv(request));

  // The wait is a bridge, and causes the turn that runs the callback.
  kj::Maybe<kj::String> bridgeId;
  for (auto& event: events) {
    if (event.startsWith("bridge ") && event.endsWith(" awaitIo")) {
      bridgeId = kj::str(event.first(event.size() - 8).slice(7));
    }
  }
  auto& id = KJ_ASSERT_NONNULL(bridgeId, kj::strArray(events, "\n"));
  bool causedTurn = false;
  for (auto& event: events) {
    if (event == kj::str("turn cause=", id)) causedTurn = true;
  }
  KJ_EXPECT(causedTurn, kj::strArray(events, "\n"));
}

KJ_TEST(
    "await_io_and_run() reports its wait under the operation of a user trace span started before") {
  kj::Vector<kj::String> events;
  TestFixture fixture({
    .useRealTimers = false,
    .isolateObserver = kj::atomicRefcounted<BridgeObserver>(events),
  });
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request,
      [&](const TestFixture::Environment& env) { await_in_user_trace_span(isolateOf(env)); });
  fixture.pollEventLoop();
  fixture.drainAndDestroy(kj::mv(request));

  // No bridge of its own: the operation causes the turn that runs the callback.
  kj::Maybe<kj::String> operationId;
  for (auto& event: events) {
    KJ_EXPECT(!event.endsWith(" awaitIo"), kj::strArray(events, "\n"));
    if (event.startsWith("operation ") && event.endsWith(" rust_operation")) {
      operationId = kj::str(event.first(event.size() - 15).slice(10));
    }
  }
  auto& id = KJ_ASSERT_NONNULL(operationId, kj::strArray(events, "\n"));
  bool causedTurn = false;
  for (auto& event: events) {
    if (event == kj::str("turn cause=", id)) causedTurn = true;
  }
  KJ_EXPECT(causedTurn, kj::strArray(events, "\n"));
}

KJ_TEST("await_io_and_run() reports its wait as its own after a user trace span is detached") {
  kj::Vector<kj::String> events;
  TestFixture fixture({
    .useRealTimers = false,
    .isolateObserver = kj::atomicRefcounted<BridgeObserver>(events),
  });
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_after_detached_user_trace_span(isolateOf(env));
  });
  fixture.pollEventLoop();
  fixture.drainAndDestroy(kj::mv(request));

  // The operation is created, but the wait is a bridge of its own, which causes the turn.
  bool hasOperation = false;
  kj::Maybe<kj::String> bridgeId;
  for (auto& event: events) {
    if (event.startsWith("operation ") && event.endsWith(" rust_detached")) hasOperation = true;
    if (event.startsWith("bridge ") && event.endsWith(" awaitIo")) {
      bridgeId = kj::str(event.first(event.size() - 8).slice(7));
    }
  }
  KJ_EXPECT(hasOperation, kj::strArray(events, "\n"));
  auto& id = KJ_ASSERT_NONNULL(bridgeId, kj::strArray(events, "\n"));
  bool causedTurn = false;
  for (auto& event: events) {
    if (event == kj::str("turn cause=", id)) causedTurn = true;
  }
  KJ_EXPECT(causedTurn, kj::strArray(events, "\n"));
}

TestFixture::SetupParams recordingSpans(RecordedSpans& records) {
  return {.requestObserverFactory = records.requestObserverFactory()};
}

KJ_TEST("trace spans are recorded with their tags, under the request's span") {
  auto records = kj::rc<RecordedSpans>();
  TestFixture fixture(recordingSpans(*records));
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { record_trace_spans(isolateOf(env)); });
  fixture.drainAndDestroy(kj::mv(request));

  auto& span = KJ_ASSERT_NONNULL(records->find("rust_span"));
  KJ_EXPECT(records->parentName(span) == "request", records->parentName(span));
  KJ_EXPECT(span.ended);
  KJ_EXPECT(
      span.tags == "bool=bool:true float=float:1.5 int=int:42 literal=str:literal str=str:dynamic",
      span.tags);

  auto& child = KJ_ASSERT_NONNULL(records->find("rust_child"));
  KJ_EXPECT(records->parentName(child) == "rust_span", records->parentName(child));
  KJ_EXPECT(child.tags == "unsigned=int:2", child.tags);

  // Its internal half; the request has no user tracing.
  auto& user = KJ_ASSERT_NONNULL(records->find("rust_user_span"));
  KJ_EXPECT(records->parentName(user) == "request", records->parentName(user));
  KJ_EXPECT(user.ended);
  KJ_EXPECT(user.tags ==
          "bool=bool:false float=float:0.5 int=int:7 literal=str:user_literal str=str:user",
      user.tags);
}

KJ_TEST("a trace span ends when its SpanBuilder is dropped") {
  auto records = kj::rc<RecordedSpans>();
  TestFixture fixture(recordingSpans(*records));
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { hold_trace_span(isolateOf(env)); });
  KJ_EXPECT(!KJ_ASSERT_NONNULL(records->find("held_span")).ended);
  drop_held_trace_span();
  KJ_EXPECT(KJ_ASSERT_NONNULL(records->find("held_span")).ended);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a user trace span's tags annotate its operation when its spans are not recorded") {
  kj::Vector<kj::String> events;
  TestFixture fixture({
    .useRealTimers = false,
    .isolateObserver = kj::atomicRefcounted<BridgeObserver>(events),
  });
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { record_trace_spans(isolateOf(env)); });
  fixture.drainAndDestroy(kj::mv(request));

  kj::Maybe<kj::String> operationId;
  for (auto& event: events) {
    if (event.startsWith("operation ") && event.endsWith(" rust_user_span")) {
      operationId = kj::str(event.first(event.size() - 15).slice(10));
    }
  }
  auto& id = KJ_ASSERT_NONNULL(operationId, kj::strArray(events, "\n"));
  kj::Vector<kj::StringPtr> annotations;
  for (auto& event: events) {
    if (event.startsWith(kj::str("annotate ", id, " "))) {
      annotations.add(event.slice(10 + id.size()));
    }
  }
  KJ_EXPECT(
      kj::strArray(annotations, " ") == "bool=false int=7 float=0.5 str=user literal=user_literal",
      kj::strArray(events, "\n"));
}

KJ_TEST("await_io_and_run() resumes the request when its future completes on another thread") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  uint64_t id = 0;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    id = idOf(env.context);
    await_released(isolateOf(env), AwaitEnding::Succeed);
  });
  fixture.pollEventLoop();
  KJ_EXPECT(await_observation() == AwaitObservation::Pending);

  // The other thread wakes this one's event loop, which the wait sleeps in until then.
  auto ran = kj::newPromiseAndFulfiller<void>();
  onAwaitCallback = kj::mv(ran.fulfiller);
  KJ_DEFER(onAwaitCallback = kj::none);
  release_await_from_thread(42);
  ran.promise.wait(fixture.getWaitScope());
  KJ_EXPECT(await_observation() == AwaitObservation::Ran);
  KJ_EXPECT(awaited_value() == 42);
  KJ_EXPECT(KJ_ASSERT_NONNULL(awaitContextId) == id);
  KJ_EXPECT(awaited_future_dropped());
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("while await_io_and_run() waits, the request is not treated as hung") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  auto aborted = context->onAbort();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_released(isolateOf(env), AwaitEnding::Succeed);
  });
  // With no pending event, the request would be aborted once the thread is idle.
  fixture.pollEventLoop();
  KJ_EXPECT(!aborted.poll(fixture.getWaitScope()));

  release_await(1);
  fixture.pollEventLoop();
  KJ_EXPECT(await_observation() == AwaitObservation::Ran);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("in a Durable Object, the await_io_and_run() callback waits for the input gate") {
  TestFixture fixture({
    .actorId = Worker::Actor::Id(kj::str("await-io-actor")),
    .useRealTimers = false,
  });
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_released(isolateOf(env), AwaitEnding::Succeed);
  });
  kj::Maybe<InputGate::Lock> gateLock =
      fixture.getActor().getInputGate().wait(nullptr).wait(fixture.getWaitScope());

  release_await(7);
  fixture.pollEventLoop();
  KJ_EXPECT(await_observation() == AwaitObservation::Pending);

  gateLock = kj::none;
  fixture.pollEventLoop();
  KJ_EXPECT(await_observation() == AwaitObservation::Ran);
  KJ_EXPECT(awaited_value() == 7);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_io_and_run() drops its future and callback when the IoContext is destroyed first") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_released(isolateOf(env), AwaitEnding::Succeed);
  });
  fixture.pollEventLoop();
  KJ_EXPECT(await_observation() == AwaitObservation::Pending);
  KJ_EXPECT(!awaited_future_dropped());
  fixture.drainAndDestroy(kj::mv(request));
  KJ_EXPECT(await_observation() == AwaitObservation::Dropped);
  KJ_EXPECT(awaited_future_dropped());
}

KJ_TEST("an await_io_and_run() callback that fails fails the task") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_released(isolateOf(env), AwaitEnding::Fail);
  });
  release_await(1);
  fixture.pollEventLoop();
  KJ_EXPECT(await_observation() == AwaitObservation::Ran);
  KJ_EXPECT(context->waitUntilStatus() == EventOutcome::EXCEPTION);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("an await_io_and_run() callback that panics terminates execution and fails the task") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_released(isolateOf(env), AwaitEnding::PanicInCallback);
  });
  release_await(1);
  {
    KJ_EXPECT_LOG(ERROR, "the callback of IoContext::await_io_and_run() panicked");
    KJ_EXPECT_LOG(ERROR, "internalMessage = the callback panicked");
    fixture.pollEventLoop();
  }
  KJ_EXPECT(await_observation() == AwaitObservation::Ran);
  KJ_EXPECT(context->waitUntilStatus() == EventOutcome::EXCEPTION);

  fixture.drainAndDestroy(kj::mv(request));

  // The termination ends with the turn that ran the callback: the isolate runs JavaScript again.
  request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto v8Context = env.isolate->GetCurrentContext();
    auto source = v8::String::NewFromUtf8Literal(env.isolate, "6 * 7");
    auto script = jsg::check(v8::Script::Compile(v8Context, source));
    KJ_EXPECT(jsg::check(script->Run(v8Context))->Int32Value(v8Context).FromJust() == 42);
  });
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("an await_io_and_run() future that panics fails the task, and its callback is dropped") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_released(isolateOf(env), AwaitEnding::PanicInFuture);
  });
  release_await(1);
  {
    KJ_EXPECT_LOG(ERROR, "the future that IoContext::await_io_and_run() waited for panicked");
    fixture.pollEventLoop();
  }
  KJ_EXPECT(await_observation() == AwaitObservation::Dropped);
  KJ_EXPECT(awaited_future_dropped());
  KJ_EXPECT(context->waitUntilStatus() == EventOutcome::EXCEPTION);
  fixture.drainAndDestroy(kj::mv(request));
}

// How the promise of the last await_promise() or await_value() has settled. It is read in a
// request of its own: the one that awaited may have nothing left to wait for, and be aborted.
kj::String awaitedOutcome(TestFixture& fixture) {
  kj::String outcome;
  auto reader = fixture.newIncomingRequest();
  fixture.enterContext(*reader, [&](const TestFixture::Environment& env) {
    outcome = kj::str(awaited_promise_outcome(isolateOf(env)));
  });
  fixture.drainAndDestroy(kj::mv(reader));
  return outcome;
}

// The value of `source`, a script, run in the current request.
v8::Local<v8::Value> runScript(const TestFixture::Environment& env, kj::StringPtr source) {
  auto v8Context = env.isolate->GetCurrentContext();
  auto script =
      jsg::check(v8::Script::Compile(v8Context, v8::Local<v8::String>(env.js.str(source))));
  return jsg::check(script->Run(v8Context));
}

// The value of `source`, a script, run in a request of its own, as a string.
kj::String evaluate(TestFixture& fixture, kj::StringPtr source) {
  kj::String value;
  auto reader = fixture.newIncomingRequest();
  fixture.enterContext(*reader, [&](const TestFixture::Environment& env) {
    value = env.js.toString(jsg::JsValue(runScript(env, source)));
  });
  fixture.drainAndDestroy(kj::mv(reader));
  return value;
}

KJ_TEST("await_io() resolves its promise with what the callback returns") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_promise(isolateOf(env), PromiseEnding::Value);
  });
  fixture.pollEventLoop();
  KJ_EXPECT(awaitedOutcome(fixture) == "pending");
  release_await(42);
  fixture.pollEventLoop();
  KJ_EXPECT(await_observation() == AwaitObservation::Ran);
  KJ_EXPECT(awaitedOutcome(fixture) == "fulfilled: 42");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_io() follows a promise that the callback returns") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_promise(isolateOf(env), PromiseEnding::Promise);
  });
  release_await(42);
  fixture.pollEventLoop();
  KJ_EXPECT(awaitedOutcome(fixture) == "fulfilled: 43");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_io() reads the then of the callback's value once, as C++ awaitIo() does") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto returnsThenable = runScript(env,
        "(function () {"
        "  let reads = 0;"
        "  return {"
        "    get then() { if (reads++) throw new TypeError('then read twice'); },"
        "    valueOf() { return 7; },"
        "  };"
        "})");
    keep_js_function(
        isolateOf(env), ::workerd::rust::jsg::to_ffi(returnsThenable.As<v8::Function>()).ptr);
    await_promise(isolateOf(env), PromiseEnding::CallJs);
  });
  release_await(42);
  fixture.pollEventLoop();
  auto outcome = awaitedOutcome(fixture);
  KJ_EXPECT(outcome == "fulfilled: 7", outcome);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_io() rejects its promise with the error the callback returns") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_promise(isolateOf(env), PromiseEnding::Fail);
  });
  release_await(42);
  fixture.pollEventLoop();
  auto outcome = awaitedOutcome(fixture);
  KJ_EXPECT(outcome == "rejected: TypeError: the callback failed", outcome);
  // The rejection is the promise's, not the task's.
  KJ_EXPECT(context->waitUntilStatus() == EventOutcome::OK);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_io() creates the error the callback returns in a reaction, with an async stack") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_promise(isolateOf(env), PromiseEnding::Fail);
    v8::Local<v8::Value> promise = ::workerd::rust::jsg::local_from_ffi<v8::Promise>(
        ::workerd::rust::jsg::Local{awaited_promise(isolateOf(env))});
    auto awaiter = runScript(env,
        "(async function awaiter(promise) {"
        "  try { await promise; } catch (e) { globalThis.awaitedStack = e.stack; }"
        "})")
                       .As<v8::Function>();
    jsg::check(
        awaiter->Call(env.isolate->GetCurrentContext(), v8::Undefined(env.isolate), 1, &promise));
  });
  release_await(42);
  fixture.pollEventLoop();
  auto stack = evaluate(fixture, "globalThis.awaitedStack");
  KJ_EXPECT(stack.startsWith("TypeError: the callback failed"), stack);
  KJ_EXPECT(stack.contains("at async awaiter"), stack);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_io() rejects its promise with what JavaScript threw in the callback") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_promise(isolateOf(env), PromiseEnding::Throw);
  });
  release_await(42);
  fixture.pollEventLoop();
  auto outcome = awaitedOutcome(fixture);
  KJ_EXPECT(outcome == "rejected: RangeError: thrown by JavaScript", outcome);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a panic in the await_io() future rejects its promise, and execution continues") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_promise(isolateOf(env), PromiseEnding::PanicInFuture);
  });
  release_await(42);
  {
    KJ_EXPECT_LOG(ERROR, "the future that IoContext::await_io() waited for panicked");
    fixture.pollEventLoop();
  }
  KJ_EXPECT(await_observation() == AwaitObservation::Dropped);
  KJ_EXPECT(awaited_future_dropped());
  auto outcome = awaitedOutcome(fixture);
  KJ_EXPECT(outcome.startsWith("rejected: Error: internal error; reference = "), outcome);
  KJ_EXPECT(context->waitUntilStatus() == EventOutcome::OK);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a panic in the await_io() callback terminates execution and settles nothing") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_promise(isolateOf(env), PromiseEnding::PanicInCallback);
  });
  release_await(42);
  {
    KJ_EXPECT_LOG(ERROR, "the callback of IoContext::await_io_and_run() panicked");
    KJ_EXPECT_LOG(ERROR, "internalMessage = the callback panicked");
    fixture.pollEventLoop();
  }
  KJ_EXPECT(await_observation() == AwaitObservation::Ran);
  KJ_EXPECT(context->waitUntilStatus() == EventOutcome::EXCEPTION);
  fixture.drainAndDestroy(kj::mv(request));

  KJ_EXPECT(awaitedOutcome(fixture) == "pending");
}

KJ_TEST("termination in the await_io() callback fails the task with the exceeded limit") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    keep_js_function(isolateOf(env),
        ::workerd::rust::jsg::to_ffi(runScript(env, "(function () {})").As<v8::Function>()).ptr);
    await_promise(isolateOf(env), PromiseEnding::Terminate);
  });
  fixture.setLimitsExceeded(EventOutcome::EXCEEDED_CPU);
  release_await(42);
  {
    KJ_EXPECT_LOG(ERROR, "the test exceeded its limits");
    fixture.pollEventLoop();
  }
  KJ_EXPECT(await_observation() == AwaitObservation::Ran);
  KJ_EXPECT(context->waitUntilStatus() == EventOutcome::EXCEEDED_CPU);
  fixture.drainAndDestroy(kj::mv(request));

  KJ_EXPECT(awaitedOutcome(fixture) == "pending");
}

KJ_TEST("a panic while dropping a completed await_io() future is logged, and the output kept") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_promise(isolateOf(env), PromiseEnding::PanicDroppingFuture);
  });
  release_await(42);
  {
    KJ_EXPECT_LOG(ERROR, "dropping the future that IoContext::await_io() waited for panicked");
    fixture.pollEventLoop();
  }
  KJ_EXPECT(awaited_future_dropped());
  KJ_EXPECT(awaitedOutcome(fixture) == "fulfilled: 42");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a panic while dropping a cancelled future is logged") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_promise(isolateOf(env), PromiseEnding::PanicDroppingFuture);
  });
  fixture.pollEventLoop();
  {
    KJ_EXPECT_LOG(ERROR, "dropping a future that the IoContext waited for panicked");
    fixture.drainAndDestroy(kj::mv(request));
  }
  KJ_EXPECT(awaited_future_dropped());
  KJ_EXPECT(await_observation() == AwaitObservation::Dropped);
}

KJ_TEST("if the IoContext is destroyed first, the await_io() promise never settles") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_promise(isolateOf(env), PromiseEnding::Value);
  });
  fixture.pollEventLoop();
  fixture.drainAndDestroy(kj::mv(request));
  KJ_EXPECT(await_observation() == AwaitObservation::Dropped);
  KJ_EXPECT(awaited_future_dropped());

  KJ_EXPECT(awaitedOutcome(fixture) == "pending");
}

KJ_TEST("in a Durable Object, an await_io() callback runs after the request that started it ends") {
  TestFixture fixture({
    .actorId = Worker::Actor::Id(kj::str("await-io-actor")),
    .useRealTimers = false,
  });
  auto context = fixture.newIoContext();
  auto first = fixture.newIncomingRequest(*context);
  fixture.enterContext(*first, [&](const TestFixture::Environment& env) {
    await_promise(isolateOf(env), PromiseEnding::Value);
  });
  // An actor's request drains until a newer one takes over its tasks.
  auto second = fixture.newIncomingRequest(*context);
  fixture.drainAndDestroy(kj::mv(first));
  KJ_EXPECT(await_observation() == AwaitObservation::Pending);

  release_await(42);
  fixture.pollEventLoop();
  KJ_EXPECT(await_observation() == AwaitObservation::Ran);
  fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
    KJ_EXPECT(kj::str(awaited_promise_outcome(isolateOf(env))) == "fulfilled: 42");
  });
  fixture.drainAndDestroy(kj::mv(second));
}

KJ_TEST("await_io_value() resolves with the future's converted output, or rejects with its error") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { await_value(isolateOf(env), false); });
  release_await(5);
  fixture.pollEventLoop();
  KJ_EXPECT(awaitedOutcome(fixture) == "fulfilled: 5");
  fixture.drainAndDestroy(kj::mv(request));

  request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { await_value(isolateOf(env), true); });
  release_await(5);
  fixture.pollEventLoop();
  auto outcome = awaitedOutcome(fixture);
  KJ_EXPECT(outcome == "rejected: TypeError: the future failed", outcome);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("add_object() drops the value with the IoContext, unless it was taken") {
  TestFixture fixture;

  auto request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { add_owned_value(isolateOf(env)); });
  fixture.drainAndDestroy(kj::mv(request));
  KJ_EXPECT(owned_value_dropped());
  drop_owned_handle();

  request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    add_owned_value(isolateOf(env));
    KJ_EXPECT(!owned_value_dropped());
    KJ_EXPECT(take_owned_value(isolateOf(env)));
    KJ_EXPECT(owned_value_dropped());
    KJ_EXPECT(!take_owned_value(isolateOf(env)));
  });
  drop_owned_handle();
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("an add_object() value can be taken only while its IoContext is current") {
  constexpr kj::StringPtr OTHER_REQUEST = "Cannot perform I/O on behalf of a different request"_kj;
  TestFixture fixture;
  auto owner = fixture.newIncomingRequest();
  auto other = fixture.newIncomingRequest();

  fixture.enterContext(
      *owner, [&](const TestFixture::Environment& env) { add_owned_value(isolateOf(env)); });
  fixture.enterContext(*other, [&](const TestFixture::Environment& env) {
    KJ_EXPECT_THROW_MESSAGE(OTHER_REQUEST, take_owned_value(isolateOf(env)));
  });
  KJ_EXPECT(!owned_value_dropped());
  fixture.enterContext(*owner,
      [&](const TestFixture::Environment& env) { KJ_EXPECT(take_owned_value(isolateOf(env))); });
  drop_owned_handle();

  // Once its IoContext is destroyed, no other is its.
  fixture.enterContext(
      *owner, [&](const TestFixture::Environment& env) { add_owned_value(isolateOf(env)); });
  fixture.drainAndDestroy(kj::mv(owner));
  fixture.enterContext(*other, [&](const TestFixture::Environment& env) {
    KJ_EXPECT_THROW_MESSAGE(OTHER_REQUEST, take_owned_value(isolateOf(env)));
  });
  drop_owned_handle();
  fixture.drainAndDestroy(kj::mv(other));
}

KJ_TEST("dropping an add_object() handle drops the value, at once only if its request is current") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    add_owned_value(isolateOf(env));
    drop_owned_handle();
    KJ_EXPECT(owned_value_dropped());
  });

  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { add_owned_value(isolateOf(env)); });
  drop_owned_handle();
  KJ_EXPECT(!owned_value_dropped());
  // The request drops the value the next time it runs.
  fixture.enterContext(
      *request, [&](const TestFixture::Environment&) { KJ_EXPECT(owned_value_dropped()); });

  auto other = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { add_owned_value(isolateOf(env)); });
  fixture.enterContext(*other, [&](const TestFixture::Environment&) {
    drop_owned_handle();
    KJ_EXPECT(!owned_value_dropped());
  });
  fixture.enterContext(
      *request, [&](const TestFixture::Environment&) { KJ_EXPECT(owned_value_dropped()); });

  fixture.drainAndDestroy(kj::mv(other));
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("an add_object() value can be used in place only while its IoContext is current") {
  constexpr kj::StringPtr OTHER_REQUEST = "Cannot perform I/O on behalf of a different request"_kj;
  TestFixture fixture;
  auto owner = fixture.newIncomingRequest();
  auto other = fixture.newIncomingRequest();

  fixture.enterContext(*owner, [&](const TestFixture::Environment& env) {
    add_owned_value(isolateOf(env));
    KJ_EXPECT(use_owned_value(isolateOf(env)) == 1);
    KJ_EXPECT(use_owned_value(isolateOf(env)) == 2);
  });
  fixture.enterContext(*other, [&](const TestFixture::Environment& env) {
    KJ_EXPECT_THROW_MESSAGE(OTHER_REQUEST, use_owned_value(isolateOf(env)));
  });
  fixture.enterContext(*owner, [&](const TestFixture::Environment& env) {
    // The value went back into the handle after each use.
    KJ_EXPECT(use_owned_value(isolateOf(env)) == 3);
    KJ_EXPECT(!owned_value_dropped());
    KJ_EXPECT(take_owned_value(isolateOf(env)));
    KJ_EXPECT(use_owned_value(isolateOf(env)) == -1);
  });
  drop_owned_handle();
  fixture.drainAndDestroy(kj::mv(other));
  fixture.drainAndDestroy(kj::mv(owner));
}

// A full garbage collection, in the request that `request` runs. It runs in an enterContext() of
// its own, so that no handle in the frames of the one that created the objects keeps them alive.
void collectGarbage(TestFixture& fixture, IoContext::IncomingRequest& request) {
  fixture.enterContext(
      request, [&](const TestFixture::Environment& env) { env.isolate->LowMemoryNotification(); });
}

// How the wait of the last await_js_start() ended.
kj::String jsOutcome() {
  return kj::str(awaited_js_outcome());
}

constexpr kj::StringPtr NEVER_SETTLED_OUTCOME =
    "rejected: jsg.Error: Promise will never complete."_kj;

KJ_TEST("await_js() resolves with the converted value") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
  });
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "pending");

  fixture.enterContext(*request,
      [&](const TestFixture::Environment& env) { settle_awaited_js(isolateOf(env), true); });
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "fulfilled: 42", jsOutcome());
  KJ_EXPECT(awaited_js_map_runs() == 1);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_js() fails with the promise's rejection") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
  });
  fixture.enterContext(*request,
      [&](const TestFixture::Environment& env) { settle_awaited_js(isolateOf(env), false); });
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "rejected: jsg.TypeError: boom", jsOutcome());
  KJ_EXPECT(awaited_js_map_runs() == 0);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_js() fails as never settled if tunneling the rejection throws") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
  });
  // As in C++, what the `stack` getter throws rejects the reaction's promise, which drops the
  // half of the wait that settles it.
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto reason = runScript(env, R"(
      const error = new Error("boom");
      Object.defineProperty(error, "stack", {
        get() { throw new TypeError("from stack getter"); },
      });
      error
    )");
    reject_awaited_js(isolateOf(env), ::workerd::rust::jsg::to_ffi(kj::mv(reason)));
  });
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == NEVER_SETTLED_OUTCOME, jsOutcome());
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_js() fails as never settled once GC collects the unsettled promise") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::DropResolver);
  });
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "pending");

  // The reactions' data is dropped inside the collection, with the request current, so the wait
  // settles there, as C++ awaitJs()'s does.
  collectGarbage(fixture, *request);
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == NEVER_SETTLED_OUTCOME, jsOutcome());
  KJ_EXPECT(awaited_js_map_runs() == 0);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a promise GC collects in another request settles the wait when its request next runs") {
  TestFixture fixture;
  auto owner = fixture.newIncomingRequest();
  auto other = fixture.newIncomingRequest();
  fixture.enterContext(*owner, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::DropResolver);
  });

  // The sender is queued for its own request to drop, as an IoOwn is.
  collectGarbage(fixture, *other);
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "pending");

  fixture.enterContext(*owner, [&](const TestFixture::Environment&) {});
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == NEVER_SETTLED_OUTCOME, jsOutcome());
  fixture.drainAndDestroy(kj::mv(other));
  fixture.drainAndDestroy(kj::mv(owner));
}

KJ_TEST("a span given to its IoContext ends with it, though its handle outlives it") {
  auto records = kj::rc<RecordedSpans>();
  TestFixture fixture(recordingSpans(*records));
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    add_owned_span(isolateOf(env));
    KJ_EXPECT(tag_owned_span(isolateOf(env)));
  });
  KJ_EXPECT(!KJ_ASSERT_NONNULL(records->find("owned_span")).ended);
  fixture.drainAndDestroy(kj::mv(request));

  auto& span = KJ_ASSERT_NONNULL(records->find("owned_span"));
  KJ_EXPECT(span.ended);
  KJ_EXPECT(records->parentName(span) == "request", records->parentName(span));
  KJ_EXPECT(span.tags == "tagged=bool:true", span.tags);
  drop_owned_span_handle();
}

KJ_TEST("a span given to its IoContext ends when its handle is dropped, once its request runs") {
  auto records = kj::rc<RecordedSpans>();
  TestFixture fixture(recordingSpans(*records));
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { add_owned_span(isolateOf(env)); });
  // Outside the request, as when the garbage collector drops what holds the handle.
  drop_owned_span_handle();
  KJ_EXPECT(!KJ_ASSERT_NONNULL(records->find("owned_span")).ended);
  fixture.enterContext(*request, [&](const TestFixture::Environment&) {
    KJ_EXPECT(KJ_ASSERT_NONNULL(records->find("owned_span")).ended);
  });
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a promise that is kept alive is not settled by GC") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
  });
  collectGarbage(fixture, *request);
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "pending");

  fixture.enterContext(*request,
      [&](const TestFixture::Environment& env) { settle_awaited_js(isolateOf(env), true); });
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "fulfilled: 42", jsOutcome());
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a cancelled await_js() does not run its conversion") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  // Once the wait is cancelled, the request has nothing else to wait for, and would be aborted
  // as hung.
  auto pendingEvent = context->registerPendingEvent();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::Cancellable);
  });
  KJ_EXPECT(cancel_awaited_js());
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "cancelled", jsOutcome());

  fixture.enterContext(*request,
      [&](const TestFixture::Environment& env) { settle_awaited_js(isolateOf(env), true); });
  fixture.pollEventLoop();
  KJ_EXPECT(awaited_js_map_runs() == 0);
  pendingEvent = nullptr;
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a cancelled await_js() whose promise GC collects drops its sender quietly") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  // As above.
  auto pendingEvent = context->registerPendingEvent();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::Cancellable);
  });
  forget_awaited_js_resolver();
  KJ_EXPECT(cancel_awaited_js());
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "cancelled", jsOutcome());
  collectGarbage(fixture, *request);
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "cancelled", jsOutcome());
  pendingEvent = nullptr;
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("destroying the IoContext drops an unsettled await_js() with its wait") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
  });
  fixture.pollEventLoop();
  fixture.drainAndDestroy(kj::mv(request));
  // The future and its callback went with the IoContext's tasks.
  KJ_EXPECT(jsOutcome() == "pending");
  forget_awaited_js_resolver();
}

constexpr kj::StringPtr MEMORY_LIMIT_OUTCOME =
    "rejected: jsg.Error: Worker has exceeded memory limit."_kj;

// The exception the wait of the last await_js_start() failed with, as the bridge converts it.
kj::Exception awaitedJsError() {
  return KJ_ASSERT_NONNULL(
      kj::runCatchingExceptions([]() { raise_awaited_js_error(); }), "the wait has not failed");
}

// `exception`'s details, as "<id>: <size>" each, separated by ", ".
kj::String describeDetails(const kj::Exception& exception) {
  return kj::strArray(KJ_MAP(detail, exception.getDetails()) {
    return kj::str(kj::hex(detail.id), ": ", detail.value.size());
  }, ", ");
}

// Checks that `exception` is IoContext::memoryLimitException(), as awaitJs() fails with it. Its
// location is the one memoryLimitException() records, which C++ and Rust share; an exception that
// Rust created would have a location in Rust instead.
void expectMemoryLimitException(const kj::Exception& exception) {
  auto expected = IoContext::memoryLimitException();
  KJ_EXPECT(exception.getType() == kj::Exception::Type::OVERLOADED, exception);
  KJ_EXPECT(exception.getDescription() == kj::str("jsg.Error: ", IoContext::MEMORY_LIMIT_MESSAGE),
      exception);
  KJ_EXPECT(exception.getFile() == kj::StringPtr(expected.getFile()), exception.getFile());
  KJ_EXPECT(exception.getLine() == expected.getLine(), exception.getLine());
  // The detail, whose value is empty, marks the outcome as out of memory.
  KJ_EXPECT(describeDetails(exception) == kj::str(kj::hex(MEMORY_LIMIT_DETAIL_ID), ": 0"),
      describeDetails(exception));
  KJ_EXPECT(RequestObserver::outcomeFromException(exception) == EventOutcome::EXCEEDED_MEMORY);
}

KJ_TEST("await_js() fails with the memory-limit error once GC collects the unsettled promise "
        "while the isolate is past its heap limit") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::DropResolver);
  });
  fixture.setHeapLimitExcessivelyExceeded(true);
  collectGarbage(fixture, *request);
  // As in C++, the error is chosen when the promise's reactions are collected, not when the
  // wait's future is next polled.
  fixture.setHeapLimitExcessivelyExceeded(false);
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == MEMORY_LIMIT_OUTCOME, jsOutcome());
  expectMemoryLimitException(awaitedJsError());
  KJ_EXPECT(awaited_js_map_runs() == 0);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_js() fails as never settled if the isolate is past its heap limit only while the "
        "wait starts") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.setHeapLimitExcessivelyExceeded(true);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::DropResolver);
  });
  fixture.setHeapLimitExcessivelyExceeded(false);
  collectGarbage(fixture, *request);
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == NEVER_SETTLED_OUTCOME, jsOutcome());
  auto error = awaitedJsError();
  KJ_EXPECT(error.getType() == kj::Exception::Type::FAILED, error);
  KJ_EXPECT(error.getDetail(MEMORY_LIMIT_DETAIL_ID) == kj::none, describeDetails(error));
  fixture.drainAndDestroy(kj::mv(request));
}

// Whether the isolate is past its heap limit while destroyContextWithHeldWait() destroys the
// IoContext.
enum class HeapLimit { EXCEEDED, NOT_EXCEEDED };

// Starts a wait that outlives its IoContext, then destroys the IoContext, with the isolate past
// its heap limit meanwhile as `heapLimit` says, and returns how the wait ended.
kj::String destroyContextWithHeldWait(HeapLimit heapLimit) {
  TestFixture fixture;
  auto watchersBefore = io_context_abort_watcher_count_for_test();
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  // The request waits for nothing that the event loop runs, and would be aborted as hung, which
  // would fail the wait first.
  auto pendingEvent = context->registerPendingEvent();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::Held);
  });
  fixture.pollEventLoop();
  KJ_EXPECT(!poll_held_awaited_js());
  fixture.setHeapLimitExcessivelyExceeded(heapLimit == HeapLimit::EXCEEDED);
  fixture.drainAndDestroy(kj::mv(request));
  pendingEvent = nullptr;
  context = nullptr;
  // The IoContext dropped the sender, and with it the abort watcher, which never saw an abort.
  KJ_EXPECT(io_context_abort_watcher_count_for_test() == watchersBefore);
  fixture.setHeapLimitExcessivelyExceeded(false);
  KJ_EXPECT(poll_held_awaited_js());
  forget_awaited_js_resolver();
  return jsOutcome();
}

KJ_TEST("destroying the IoContext while the isolate is past its heap limit fails an unsettled "
        "await_js() with the memory-limit error") {
  auto outcome = destroyContextWithHeldWait(HeapLimit::EXCEEDED);
  KJ_EXPECT(outcome == MEMORY_LIMIT_OUTCOME, outcome);
  expectMemoryLimitException(awaitedJsError());
}

KJ_TEST("destroying the IoContext fails an unsettled await_js() as never settled") {
  auto outcome = destroyContextWithHeldWait(HeapLimit::NOT_EXCEEDED);
  KJ_EXPECT(outcome == NEVER_SETTLED_OUTCOME, outcome);
}

KJ_TEST("past the heap limit, await_js() fails with the exception C++ awaitJs() fails with") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  kj::Maybe<kj::Exception> cppError;
  kj::Maybe<kj::Promise<void>> cppWait;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    // Both promises are unreachable once their resolvers are dropped, here and in Rust.
    auto paf = env.js.newPromiseAndResolver<void>();
    cppWait = env.context.awaitJs(env.js, kj::mv(paf.promise))
                  .then([]() { KJ_FAIL_EXPECT("C++ awaitJs() fulfilled"); },
                      [&cppError](kj::Exception&& e) {
      cppError = kj::mv(e);
    }).eagerlyEvaluate(nullptr);
    await_js_start(isolateOf(env), AwaitJsStart::DropResolver);
  });
  fixture.setHeapLimitExcessivelyExceeded(true);
  collectGarbage(fixture, *request);
  fixture.setHeapLimitExcessivelyExceeded(false);
  fixture.pollEventLoop();

  auto& cpp = KJ_ASSERT_NONNULL(cppError, "C++ awaitJs() has not failed");
  auto rust = awaitedJsError();
  expectMemoryLimitException(cpp);
  KJ_EXPECT(rust.getType() == cpp.getType(), rust, cpp);
  KJ_EXPECT(rust.getDescription() == cpp.getDescription(), rust, cpp);
  KJ_EXPECT(kj::StringPtr(rust.getFile()) == kj::StringPtr(cpp.getFile()), rust, cpp);
  KJ_EXPECT(rust.getLine() == cpp.getLine(), rust, cpp);
  KJ_EXPECT(describeDetails(rust) == describeDetails(cpp), rust, cpp);
  cppWait = kj::none;
  fixture.drainAndDestroy(kj::mv(request));
}

// Records the init, settle and destroy events of the awaitJs resources an async tracker creates,
// labelled as in async-trace-io-test.c++: "awaitJs#1" is the first, and "request" a request, so
// that the events of different runs compare equal.
class AwaitJsTraceListener final: public AsyncTraceListener {
 public:
  explicit AwaitJsTraceListener(kj::Vector<kj::String>& events): events(events) {}

  void onInit(uint64_t, const AsyncInitEvent& e) override {
    if (e.kind == AsyncKind::REQUEST) {
      labels.insert(e.id, kj::str("request"));
    } else if (e.kind == AsyncKind::JS_TO_KJ && e.name == "awaitJs"_kj.asArray()) {
      auto label = kj::str("awaitJs#", ++awaitJsCount);
      events.add(
          kj::str("init ", label, " trigger=", labelOf(e.trigger), " exec=", labelOf(e.execution)));
      labels.insert(e.id, kj::mv(label));
    }
  }
  void onSettle(uint64_t, AsyncId id, AsyncOutcome outcome, uint64_t) override {
    KJ_IF_SOME(label, awaitJsLabel(id)) {
      events.add(kj::str("settle ", label, " ", nameOf(outcome)));
    }
  }
  void onDestroy(uint64_t, AsyncId id, uint64_t) override {
    KJ_IF_SOME(label, awaitJsLabel(id)) {
      events.add(kj::str("destroy ", label));
    }
  }

 private:
  kj::Vector<kj::String>& events;
  kj::HashMap<AsyncId, kj::String> labels;
  uint awaitJsCount = 0;

  kj::String labelOf(AsyncId id) {
    if (id == 0) return kj::str("-");
    KJ_IF_SOME(label, labels.find(id)) {
      return kj::str(label);
    }
    return kj::str("?", id);
  }

  kj::Maybe<kj::StringPtr> awaitJsLabel(AsyncId id) {
    KJ_IF_SOME(label, labels.find(id)) {
      if (label.startsWith("awaitJs#")) return label.asPtr();
    }
    return kj::none;
  }

  static kj::StringPtr nameOf(AsyncOutcome outcome) {
    switch (outcome) {
      case AsyncOutcome::OK:
        return "OK"_kj;
      case AsyncOutcome::ERROR:
        return "ERROR"_kj;
      case AsyncOutcome::CANCELED:
        return "CANCELED"_kj;
    }
    KJ_UNREACHABLE;
  }
};

// How the wait that traceAwaitJs() starts ends.
enum class TracedWait {
  FULFILLED,
  REJECTED,
  // The conversion fails with its own error, a KJ exception in C++.
  MAP_FAILS,
  // JavaScript throws in the conversion.
  MAP_THROWS,
  CANCELLED_THEN_FULFILLED,
  CANCELLED_THEN_REJECTED,
  // GC collects the promise of a cancelled wait, which never settles.
  CANCELLED_THEN_COLLECTED,
  // GC collects the promise, which never settles.
  NEVER_SETTLED,
};

kj::StringPtr nameOf(TracedWait scenario) {
  switch (scenario) {
    case TracedWait::FULFILLED:
      return "FULFILLED"_kj;
    case TracedWait::REJECTED:
      return "REJECTED"_kj;
    case TracedWait::MAP_FAILS:
      return "MAP_FAILS"_kj;
    case TracedWait::MAP_THROWS:
      return "MAP_THROWS"_kj;
    case TracedWait::CANCELLED_THEN_FULFILLED:
      return "CANCELLED_THEN_FULFILLED"_kj;
    case TracedWait::CANCELLED_THEN_REJECTED:
      return "CANCELLED_THEN_REJECTED"_kj;
    case TracedWait::CANCELLED_THEN_COLLECTED:
      return "CANCELLED_THEN_COLLECTED"_kj;
    case TracedWait::NEVER_SETTLED:
      return "NEVER_SETTLED"_kj;
  }
  KJ_UNREACHABLE;
}

bool isCancelled(TracedWait scenario) {
  return scenario == TracedWait::CANCELLED_THEN_FULFILLED ||
      scenario == TracedWait::CANCELLED_THEN_REJECTED ||
      scenario == TracedWait::CANCELLED_THEN_COLLECTED;
}

bool isCollected(TracedWait scenario) {
  return scenario == TracedWait::NEVER_SETTLED || scenario == TracedWait::CANCELLED_THEN_COLLECTED;
}

bool fulfills(TracedWait scenario) {
  return scenario != TracedWait::REJECTED && scenario != TracedWait::CANCELLED_THEN_REJECTED;
}

enum class Implementation { CPP, RUST };

// A wait of C++ IoContext::awaitJs(), the counterpart of the one await_js_start() starts through
// Rust's await_js(). Its resolver, if kept, must be taken in a request.
struct CppAwaitJs {
  kj::Maybe<jsg::Promise<double>::Resolver> resolver;
  kj::Maybe<kj::Promise<void>> wait;
  // How many times the conversion ran, as awaited_js_map_runs() counts for Rust's.
  static inline uint mapRuns = 0;

  // Starts the wait, with the conversion `scenario` calls for, and keeps the resolver unless GC
  // is to collect the promise.
  void start(const TestFixture::Environment& env, TracedWait scenario) {
    switch (scenario) {
      case TracedWait::MAP_FAILS:
        startWith(env, scenario, [](jsg::Lock&, double) -> double {
          ++mapRuns;
          JSG_FAIL_REQUIRE(RangeError, "the conversion failed");
        });
        break;
      case TracedWait::MAP_THROWS:
        startWith(env, scenario, [](jsg::Lock& js, double) -> double {
          ++mapRuns;
          js.throwException(js.rangeError("thrown by JavaScript"));
        });
        break;
      default:
        startWith(env, scenario, [](jsg::Lock&, double value) {
          ++mapRuns;
          return value;
        });
        break;
    }
  }

  // Fulfills the promise with 42, or rejects it with a TypeError, as `scenario` says, like
  // settle_awaited_js().
  void settle(const TestFixture::Environment& env, TracedWait scenario) {
    auto taken = KJ_ASSERT_NONNULL(kj::mv(resolver), "the resolver was not kept");
    resolver = kj::none;
    if (fulfills(scenario)) {
      taken.resolve(env.js, 42.0);
    } else {
      taken.reject(env.js, env.js.typeError("boom"));
    }
  }

 private:
  template <typename Func>
  void startWith(const TestFixture::Environment& env, TracedWait scenario, Func func) {
    auto paf = env.js.newPromiseAndResolver<double>();
    wait = env.context.awaitJs(env.js, kj::mv(paf.promise), kj::mv(func))
               .then([](double) {}, [](kj::Exception&&) {
    }).eagerlyEvaluate(nullptr);
    if (!isCollected(scenario)) resolver = kj::mv(paf.resolver);
  }
};

// The way await_js_start() starts the Rust counterpart of `scenario`'s wait.
AwaitJsStart rustStart(TracedWait scenario) {
  switch (scenario) {
    case TracedWait::FULFILLED:
    case TracedWait::REJECTED:
      return AwaitJsStart::KeepResolver;
    case TracedWait::MAP_FAILS:
      return AwaitJsStart::MapFails;
    case TracedWait::MAP_THROWS:
      return AwaitJsStart::MapThrows;
    case TracedWait::CANCELLED_THEN_FULFILLED:
    case TracedWait::CANCELLED_THEN_REJECTED:
    case TracedWait::CANCELLED_THEN_COLLECTED:
      return AwaitJsStart::Cancellable;
    case TracedWait::NEVER_SETTLED:
      return AwaitJsStart::DropResolver;
  }
  KJ_UNREACHABLE;
}

// Starts a wait in a traced request through C++ awaitJs() or Rust's await_js(), has it go as
// `scenario` says, and returns the events of its awaitJs resource, one per line. "settling" and
// "settled" bracket the turn in which the promise settles or GC collects it, so the events show
// that the resource is settled or destroyed in that turn, by the reaction or the collection.
kj::String traceAwaitJs(TracedWait scenario, Implementation implementation) {
  kj::Vector<kj::String> events;
  {
    TestFixture fixture({
      .useRealTimers = false,
      .isolateObserver = kj::atomicRefcounted<TraceObserver<AwaitJsTraceListener>>(events),
    });
    auto context = fixture.newIoContext();
    auto request = fixture.newIncomingRequest(*context);
    // A cancelled wait leaves the request nothing else to wait for, and it would be aborted as
    // hung.
    auto pendingEvent = context->registerPendingEvent();
    CppAwaitJs cpp;
    CppAwaitJs::mapRuns = 0;

    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      if (implementation == Implementation::RUST) {
        await_js_start(isolateOf(env), rustStart(scenario));
        // A cancellable wait keeps its resolver; this one's promise is to be collected.
        if (scenario == TracedWait::CANCELLED_THEN_COLLECTED) forget_awaited_js_resolver();
      } else {
        cpp.start(env, scenario);
      }
    });
    fixture.pollEventLoop();

    if (isCancelled(scenario)) {
      if (implementation == Implementation::RUST) {
        KJ_EXPECT(cancel_awaited_js());
        fixture.pollEventLoop();
        KJ_EXPECT(jsOutcome() == "cancelled", jsOutcome());
      } else {
        cpp.wait = kj::none;
        fixture.pollEventLoop();
      }
    }

    events.add(kj::str("settling"));
    if (isCollected(scenario)) {
      collectGarbage(fixture, *request);
    } else {
      fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
        if (implementation == Implementation::RUST) {
          settle_awaited_js(isolateOf(env), fulfills(scenario));
        } else {
          cpp.settle(env, scenario);
        }
      });
    }
    events.add(kj::str("settled"));
    fixture.pollEventLoop();
    if (isCancelled(scenario)) {
      auto mapRuns =
          implementation == Implementation::RUST ? awaited_js_map_runs() : CppAwaitJs::mapRuns;
      KJ_EXPECT(mapRuns == 0, nameOf(scenario), mapRuns);
    }

    cpp.wait = kj::none;
    pendingEvent = nullptr;
    fixture.drainAndDestroy(kj::mv(request));
  }
  return kj::strArray(events, "\n");
}

// Runs `scenario` through C++ awaitJs() and Rust's await_js(), checks that their events are
// the same, and returns them.
kj::String traceAwaitJsOnBoth(TracedWait scenario) {
  auto cpp = traceAwaitJs(scenario, Implementation::CPP);
  auto rust = traceAwaitJs(scenario, Implementation::RUST);
  KJ_EXPECT(cpp == rust, nameOf(scenario), cpp, rust);
  return cpp;
}

// The events traceAwaitJs() expects when the wait's resource is created, then `last` happens in
// the turn in which its promise settles or GC collects it.
kj::String expectedAwaitJsEvents(kj::StringPtr last) {
  return kj::str("init awaitJs#1 trigger=request exec=request\nsettling\n", last, "\nsettled");
}

KJ_TEST("await_js() is recorded for async tracing and settled OK when fulfilled, as in C++") {
  auto events = traceAwaitJsOnBoth(TracedWait::FULFILLED);
  KJ_EXPECT(events == expectedAwaitJsEvents("settle awaitJs#1 OK"), events);
}

KJ_TEST("await_js() is settled with an error for async tracing when the promise rejects or the "
        "conversion fails, as in C++") {
  for (auto scenario: {TracedWait::REJECTED, TracedWait::MAP_FAILS, TracedWait::MAP_THROWS}) {
    auto events = traceAwaitJsOnBoth(scenario);
    KJ_EXPECT(events == expectedAwaitJsEvents("settle awaitJs#1 ERROR"), nameOf(scenario), events);
  }
}

KJ_TEST("a cancelled await_js() is still settled for async tracing as its promise settles, as in "
        "C++") {
  auto fulfilled = traceAwaitJsOnBoth(TracedWait::CANCELLED_THEN_FULFILLED);
  KJ_EXPECT(fulfilled == expectedAwaitJsEvents("settle awaitJs#1 OK"), fulfilled);
  auto rejected = traceAwaitJsOnBoth(TracedWait::CANCELLED_THEN_REJECTED);
  KJ_EXPECT(rejected == expectedAwaitJsEvents("settle awaitJs#1 ERROR"), rejected);
}

KJ_TEST("an await_js() whose promise never settles is destroyed unsettled for async tracing, as "
        "in C++") {
  for (auto scenario: {TracedWait::NEVER_SETTLED, TracedWait::CANCELLED_THEN_COLLECTED}) {
    auto events = traceAwaitJsOnBoth(scenario);
    KJ_EXPECT(events == expectedAwaitJsEvents("destroy awaitJs#1"), nameOf(scenario), events);
  }
}

// Starts a wait of Rust's await_js() as `start` says in a new request of `fixture`, which must
// not be traced, fulfills its promise or has GC collect it, and returns how the wait ended.
kj::String awaitJsUntraced(TestFixture& fixture, AwaitJsStart start) {
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    KJ_EXPECT(env.context.tryGetAsyncTracker() == kj::none);
    KJ_EXPECT(io_context_make_await_js_resource(idOf(env.context)) == kj::none);
    await_js_start(isolateOf(env), start);
  });
  if (start == AwaitJsStart::DropResolver) {
    collectGarbage(fixture, *request);
  } else {
    fixture.enterContext(*request,
        [&](const TestFixture::Environment& env) { settle_awaited_js(isolateOf(env), true); });
  }
  fixture.pollEventLoop();
  fixture.drainAndDestroy(kj::mv(request));
  return jsOutcome();
}

KJ_TEST("in an untraced request, await_js() creates no async trace resource and still settles") {
  TestFixture fixture;
  auto fulfilled = awaitJsUntraced(fixture, AwaitJsStart::KeepResolver);
  KJ_EXPECT(fulfilled == "fulfilled: 42", fulfilled);
  auto mapFailed = awaitJsUntraced(fixture, AwaitJsStart::MapFails);
  KJ_EXPECT(mapFailed == "rejected: jsg.RangeError: the conversion failed", mapFailed);
  auto neverSettled = awaitJsUntraced(fixture, AwaitJsStart::DropResolver);
  KJ_EXPECT(neverSettled == NEVER_SETTLED_OUTCOME, neverSettled);
}

// Records the uncaught exceptions the worker logs, which it logs only at INFO level, while it
// exists, and passes on other messages the log level allowed before, as in request-ops-test.c++.
// Only INFO messages are recorded, so that a failed KJ_EXPECT that quotes one reaches the test.
class UncaughtExceptionLog final: public kj::ExceptionCallback {
 public:
  UncaughtExceptionLog(): previousLevel(currentLogLevel()) {
    kj::_::Debug::setLogLevel(kj::LogSeverity::INFO);
  }
  ~UncaughtExceptionLog() noexcept(false) {
    kj::_::Debug::setLogLevel(previousLevel);
  }

  void logMessage(kj::LogSeverity severity,
      const char* file,
      int line,
      int contextDepth,
      kj::String&& text) override {
    if (severity == kj::LogSeverity::INFO && text.contains("uncaught exception")) {
      messages.add(kj::mv(text));
    } else if (severity >= previousLevel) {
      next.logMessage(severity, file, line, contextDepth, kj::mv(text));
    }
  }

  // The messages recorded since the last call, separated by " | ".
  kj::String take() {
    auto result = kj::strArray(messages, " | ");
    messages.clear();
    return result;
  }

  kj::Vector<kj::String> messages;

 private:
  kj::LogSeverity previousLevel;

  // KJ has no getter for the log level.
  static kj::LogSeverity currentLogLevel() {
    for (auto severity: {kj::LogSeverity::INFO, kj::LogSeverity::WARNING, kj::LogSeverity::ERROR,
           kj::LogSeverity::FATAL}) {
      if (kj::_::Debug::shouldLog(severity)) return severity;
    }
    return kj::LogSeverity::DBG;
  }
};

// How an await_js() rejection is logged, up to what follows "stack = " or "exception = ".
constexpr kj::StringPtr IN_PROMISE = "uncaught exception; source = Uncaught (in promise); "_kj;

// Rejects the promise of the last await_js_start() with the value of `source`, a script.
void rejectAwaitedJs(
    TestFixture& fixture, IoContext::IncomingRequest& request, kj::StringPtr source) {
  fixture.enterContext(request, [&](const TestFixture::Environment& env) {
    reject_awaited_js(isolateOf(env), ::workerd::rust::jsg::to_ffi(runScript(env, source)));
  });
}

KJ_TEST("await_js() logs a rejection as an uncaught exception, once, with its stack") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
  });
  UncaughtExceptionLog log;
  rejectAwaitedJs(fixture, *request, R"((() => {
    const error = new RangeError("boom");
    error.stack = "the stack";
    return error;
  })())");
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "rejected: jsg.RangeError: boom", jsOutcome());
  auto logged = log.take();
  KJ_EXPECT(logged == kj::str(IN_PROMISE, "stack = the stack"), logged);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_js() logs a rejection with a value that has no stack as the value") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
  });
  UncaughtExceptionLog log;
  rejectAwaitedJs(fixture, *request, R"("a string")");
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "rejected: jsg.Error: a string", jsOutcome());
  auto logged = log.take();
  KJ_EXPECT(logged == kj::str(IN_PROMISE, "exception = a string"), logged);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_js() logs what JavaScript throws in its conversion, once, then tunnels it") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::MapThrows);
  });
  UncaughtExceptionLog log;
  fixture.enterContext(*request,
      [&](const TestFixture::Environment& env) { settle_awaited_js(isolateOf(env), true); });
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "rejected: jsg.RangeError: thrown by JavaScript", jsOutcome());
  KJ_EXPECT(awaited_js_map_runs() == 1);
  KJ_EXPECT(log.messages.size() == 1, kj::strArray(log.messages, " | "));
  auto logged = log.take();
  KJ_EXPECT(
      logged.startsWith(kj::str(IN_PROMISE, "stack = RangeError: thrown by JavaScript")), logged);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_js() converts its conversion's own error, logs it and tunnels it as one thrown") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::MapFails);
  });
  UncaughtExceptionLog log;
  fixture.enterContext(*request,
      [&](const TestFixture::Environment& env) { settle_awaited_js(isolateOf(env), true); });
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "rejected: jsg.RangeError: the conversion failed", jsOutcome());
  // Tunneled as a JavaScript exception is, with the error serialized and its name and stack.
  auto detailIds = awaited_js_detail_ids();
  auto ids = kj::arrayPtr(detailIds.data(), detailIds.size());
  KJ_EXPECT(ids.findFirst(jsg::TUNNELED_EXCEPTION_DETAIL_ID) != kj::none);
  KJ_EXPECT(ids.findFirst(jsg::JS_EXCEPTION_METADATA_DETAIL_ID) != kj::none);
  KJ_EXPECT(log.messages.size() == 1, kj::strArray(log.messages, " | "));
  auto logged = log.take();
  KJ_EXPECT(
      logged.startsWith(kj::str(IN_PROMISE, "stack = RangeError: the conversion failed")), logged);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_js() redacts and logs its conversion's internal error, then tunnels it") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::MapFailsInternally);
  });
  UncaughtExceptionLog log;
  {
    KJ_EXPECT_LOG(ERROR, "internalMessage = the conversion failed internally");
    fixture.enterContext(*request,
        [&](const TestFixture::Environment& env) { settle_awaited_js(isolateOf(env), true); });
    fixture.pollEventLoop();
  }
  KJ_EXPECT(
      jsOutcome().startsWith("rejected: jsg.Error: internal error; reference = "), jsOutcome());
  auto logged = log.take();
  KJ_EXPECT(logged.startsWith(kj::str(IN_PROMISE, "stack = Error: internal error; reference = ")),
      logged);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a fulfilled await_js() logs nothing") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
  });
  UncaughtExceptionLog log;
  fixture.enterContext(*request,
      [&](const TestFixture::Environment& env) { settle_awaited_js(isolateOf(env), true); });
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "fulfilled: 42", jsOutcome());
  auto logged = log.take();
  KJ_EXPECT(logged == "", logged);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_js() logs a rejection before tunneling it, which then does not happen if logging "
        "throws") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
  });
  UncaughtExceptionLog log;
  // Tunneling reads `overloaded` first, and logging reads `stack`.
  rejectAwaitedJs(fixture, *request, R"(({
    get overloaded() { globalThis.tunneled = true; return false; },
    get stack() { throw new TypeError("from stack getter"); },
  }))");
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == NEVER_SETTLED_OUTCOME, jsOutcome());
  KJ_EXPECT(evaluate(fixture, "String(globalThis.tunneled)") == "undefined");
  auto logged = log.take();
  KJ_EXPECT(!logged.contains("Uncaught (in promise)"), logged);
  fixture.drainAndDestroy(kj::mv(request));
}

// The notifications an inspector session sends, which InspectorChannel receives.
class InspectorNotifications {
 public:
  void add(kj::String notification) {
    notifications.add(kj::mv(notification));
  }

  // The notifications received since the last call, separated by " | ", with the values of the
  // fields that differ from one report of an exception to that of the same exception, created by
  // the same source in another script, replaced with `_`.
  kj::String take() {
    auto result = kj::strArray(KJ_MAP(notification, notifications) {
      return withoutFields(
          notification, {"timestamp"_kj, "exceptionId"_kj, "objectId"_kj, "scriptId"_kj});
    }, " | ");
    notifications.clear();
    return result;
  }

 private:
  kj::Vector<kj::String> notifications;

  // `json` with the value of each field named in `names` replaced with `_`.
  static kj::String withoutFields(kj::StringPtr json, std::initializer_list<kj::StringPtr> names) {
    kj::String result = kj::str(json);
    for (auto name: names) {
      auto key = kj::str("\"", name, "\":");
      kj::Vector<char> out;
      kj::StringPtr rest = result;
      while (true) {
        KJ_IF_SOME(at, rest.find(key)) {
          out.addAll(rest.first(at + key.size()));
          rest = rest.slice(at + key.size());
          size_t end = 0;
          if (rest.startsWith("\"")) {
            end = 1;
            while (end < rest.size() && rest[end] != '"') {
              end += rest[end] == '\\' ? 2 : 1;
            }
            ++end;
          } else {
            while (end < rest.size() && rest[end] != ',' && rest[end] != '}') ++end;
          }
          out.add('_');
          rest = rest.slice(kj::min(end, rest.size()));
        } else {
          out.addAll(rest);
          break;
        }
      }
      out.add('\0');
      result = kj::String(out.releaseAsArray());
    }
    return result;
  }
};

class InspectorChannel final: public v8_inspector::V8Inspector::Channel {
 public:
  explicit InspectorChannel(InspectorNotifications& notifications): notifications(notifications) {}

  void sendResponse(int, std::unique_ptr<v8_inspector::StringBuffer>) override {}
  void sendNotification(std::unique_ptr<v8_inspector::StringBuffer> message) override {
    notifications.add(kj::str(message->string()));
  }
  void flushProtocolNotifications() override {}

 private:
  InspectorNotifications& notifications;
};

// The parameters of a fixture whose isolate has an inspector, without a DevTools server.
TestFixture::SetupParams inspectorParams(capnp::MallocMessageBuilder& flagsMessage) {
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setEnableNodeJsInspectorLocalDev(true);
  return {.featureFlags = flags.asReader(), .useRealTimers = false};
}

// Connects a session to the inspector of the isolate of `env`, and enables its Runtime domain,
// which reports exceptions. The session must be destroyed under the isolate lock.
std::unique_ptr<v8_inspector::V8InspectorSession> connectInspector(
    const TestFixture::Environment& env, InspectorChannel& channel) {
  auto& inspector = KJ_ASSERT_NONNULL(env.context.getWorker().getIsolate().tryGetV8Inspector());
  // Context group 1 is the worker's (setupContext() in worker.c++).
  auto session = inspector.connect(
      1, &channel, v8_inspector::StringView(), v8_inspector::V8Inspector::kFullyTrusted);
  auto enable = R"({"id":1,"method":"Runtime.enable"})"_kj;
  session->dispatchProtocolMessage(
      v8_inspector::StringView(enable.asBytes().begin(), enable.size()));
  return session;
}

KJ_TEST("log_uncaught_exception() reports to the inspector and logs as C++ logUncaughtException() "
        "does") {
  capnp::MallocMessageBuilder flagsMessage;
  TestFixture fixture(inspectorParams(flagsMessage));
  UncaughtExceptionLog log;
  InspectorNotifications notifications;
  InspectorChannel channel(notifications);
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto session = connectInspector(env, channel);
    notifications.take();

    struct Source {
      UncaughtExceptionSource cpp;
      LogSource rust;
    };
    for (auto source: {Source{UncaughtExceptionSource::INTERNAL, LogSource::Internal},
           Source{UncaughtExceptionSource::INTERNAL_ASYNC, LogSource::InternalAsync}}) {
      auto prefix = kj::str("uncaught exception; source = ", source.cpp, "; ");
      for (auto script:
          {
            R"(new RangeError("boom"))"_kj,
            R"((() => { const e = new Error("with\0nul"); e.stack = "before\0after"; return e; })())"_kj,
            R"(new TypeError("caf\u00e9 \u{1F600}"))"_kj,
            R"("a string")"_kj,
            R"(({ stack: 42 }))"_kj,
            R"(({}))"_kj,
            R"(Symbol("symbol"))"_kj,
            "undefined"_kj,
            "null"_kj,
          }) {
        // Each implementation gets a value of its own, since reading an error's `stack` formats
        // its stack trace, after which V8 no longer finds the location of the error in it.
        auto cppValue = runScript(env, script);
        auto rustValue = runScript(env, script);
        env.context.logUncaughtException(source.cpp, jsg::JsValue(cppValue));
        auto cppReport = notifications.take();
        auto cppLog = log.take();

        log_uncaught_exception(
            isolateOf(env), source.rust, ::workerd::rust::jsg::to_ffi(kj::mv(rustValue)));
        auto rustReport = notifications.take();
        auto rustLog = log.take();

        KJ_EXPECT(cppReport.contains(R"j("method":"Runtime.exceptionThrown")j"), script, cppReport);
        KJ_EXPECT(rustReport == cppReport, script, rustReport, cppReport);
        KJ_EXPECT(cppLog.startsWith(prefix), script, cppLog);
        KJ_EXPECT(rustLog == cppLog, script, rustLog, cppLog);
      }
    }
    // Disconnected under the isolate lock.
    session = nullptr;
  });
}

KJ_TEST("await_js() reports a rejection to the inspector once, without logging at INFO level") {
  capnp::MallocMessageBuilder flagsMessage;
  TestFixture fixture(inspectorParams(flagsMessage));
  InspectorNotifications notifications;
  InspectorChannel channel(notifications);
  std::unique_ptr<v8_inspector::V8InspectorSession> session;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  // Once the wait ends, the request has nothing else to wait for, and would be aborted as hung
  // before the session is disconnected.
  auto pendingEvent = context->registerPendingEvent();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    session = connectInspector(env, channel);
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
  });
  notifications.take();
  rejectAwaitedJs(fixture, *request, R"(new RangeError("boom"))");
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "rejected: jsg.RangeError: boom", jsOutcome());
  auto reports = notifications.take();
  KJ_EXPECT(reports.startsWith(R"j({"method":"Runtime.exceptionThrown")j"), reports);
  KJ_EXPECT(reports.contains(R"j("text":"Uncaught RangeError: boom")j"), reports);
  KJ_EXPECT(!reports.contains(" | "), reports);
  fixture.enterContext(*request, [&](const TestFixture::Environment&) { session = nullptr; });
  pendingEvent = nullptr;
  fixture.drainAndDestroy(kj::mv(request));
}

enum class LogLevel { DEFAULT, INFO };

// How many times the `stack` getter of a rejection runs while await_js() handles it, at `level`.
kj::String stackReads(LogLevel level) {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
  });
  kj::Maybe<UncaughtExceptionLog> log;
  if (level == LogLevel::INFO) log.emplace();
  rejectAwaitedJs(fixture, *request, R"(({
    get stack() {
      globalThis.stackReads = (globalThis.stackReads ?? 0) + 1;
      return "the stack";
    },
  }))");
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome().startsWith("rejected: "), jsOutcome());
  auto reads = evaluate(fixture, "String(globalThis.stackReads)");
  fixture.drainAndDestroy(kj::mv(request));
  return reads;
}

KJ_TEST("await_js() reads a rejection's stack to log it only when KJ logs at INFO level") {
  // Tunneling reads it too.
  auto quiet = stackReads(LogLevel::DEFAULT);
  auto verbose = stackReads(LogLevel::INFO);
  auto quietReads = KJ_ASSERT_NONNULL(quiet.tryParseAs<int>(), quiet);
  KJ_EXPECT(verbose == kj::str(quietReads + 1), quiet, verbose);
}

constexpr kj::Exception::DetailTypeId ABORT_DETAIL_ID = 0xab07'0000'0000'0001ull;

// The exception the tests below abort a request with. Its type is not FAILED, and it has a
// detail, so that a wait that fails with it is seen to fail with this very exception.
kj::Exception abortException() {
  auto e = KJ_EXCEPTION(OVERLOADED, "jsg.Error: aborted by the test");
  e.setDetail(ABORT_DETAIL_ID, kj::heapArray<kj::byte>({1, 2, 3}));
  return e;
}

// Checks that `actual` is `expected`, as the bridge carries an exception: with its type,
// description, location and details.
void expectSameException(const kj::Exception& actual, const kj::Exception& expected) {
  KJ_EXPECT(actual.getType() == expected.getType(), actual, expected);
  KJ_EXPECT(actual.getDescription() == expected.getDescription(), actual, expected);
  KJ_EXPECT(kj::StringPtr(actual.getFile()) == kj::StringPtr(expected.getFile()), actual, expected);
  KJ_EXPECT(actual.getLine() == expected.getLine(), actual, expected);
  KJ_EXPECT(describeDetails(actual) == describeDetails(expected), describeDetails(actual),
      describeDetails(expected));
}

// The number of await_js() abort watchers alive on this thread.
size_t abortWatchers() {
  return io_context_abort_watcher_count_for_test();
}

// A wait of C++ IoContext::awaitJs(), recorded as await_js_start() records Rust's.
struct RecordedCppAwaitJs {
  kj::Maybe<jsg::Promise<double>::Resolver> resolver;
  kj::Maybe<kj::Promise<void>> wait;
  kj::String outcome = kj::str("pending");
  kj::Maybe<kj::Exception> error;
  // How many times the conversion ran, as awaited_js_map_runs() counts for Rust's.
  static inline uint mapRuns = 0;

  // The wait, which nothing waits for until waitHeld() is called; see startHeld().
  kj::Maybe<kj::Promise<double>> held;

  void start(const TestFixture::Environment& env) {
    startHeld(env);
    waitHeld();
  }

  // Starts the wait without waiting for it, as await_js_start(Held) does.
  void startHeld(const TestFixture::Environment& env) {
    auto paf = env.js.newPromiseAndResolver<double>();
    resolver = kj::mv(paf.resolver);
    held = env.context.awaitJs(env.js, kj::mv(paf.promise), [](jsg::Lock&, double value) {
      ++mapRuns;
      return value;
    });
  }

  // Waits for the wait startHeld() started, recording its outcome.
  void waitHeld() {
    auto promise = KJ_ASSERT_NONNULL(kj::mv(held), "no held wait");
    held = kj::none;
    wait = kj::mv(promise)
               .then([&outcome = outcome](double value) {
      outcome = kj::str("fulfilled: ", value);
    }, [&outcome = outcome, &error = error](kj::Exception&& e) {
      outcome = kj::str("rejected: ", e.getDescription());
      error = kj::mv(e);
    }).eagerlyEvaluate(nullptr);
  }

  // Cancels the wait, recorded as Rust's cancelled wait is.
  void cancel() {
    wait = kj::none;
    outcome = kj::str("cancelled");
  }

  // Fulfills the promise with 42, or rejects it with a TypeError, as settle_awaited_js() does.
  void settle(jsg::Lock& js, bool fulfill) {
    auto taken = KJ_ASSERT_NONNULL(kj::mv(resolver), "the resolver was not kept");
    resolver = kj::none;
    if (fulfill) {
      taken.resolve(js, 42.0);
    } else {
      taken.reject(js, js.typeError("boom"));
    }
  }
};

// Runs `settle` outside any request, then has `context`, which was aborted and no longer runs,
// run the reactions it queued with `context` current, as V8 runs them in the next microtask
// checkpoint of the isolate: here, in the one `context` runs when it logs an uncaught exception
// with the isolate lock, as it does when the isolate has an inspector.
template <typename Func>
void settleOnceAborted(TestFixture& fixture, IoContext& context, Func&& settle) {
  fixture.enterWorkerLock([&](Worker::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](jsg::Lock& js) { settle(js); });
  });
  KJ_EXPECT_LOG(ERROR, "draining the microtask queue");
  context.logUncaughtExceptionAsync(UncaughtExceptionSource::INTERNAL,
      KJ_EXCEPTION(FAILED, "draining the microtask queue"), kj::none);
}

// When the request a wait runs in is aborted, with IoContext::abort(), relative to the wait.
enum class AbortedWait {
  // While the wait is pending. Its promise never settles.
  PENDING,
  // While the wait is pending. Once the abort has reached the wait, its promise is fulfilled.
  THEN_FULFILLED,
  // As THEN_FULFILLED, but the promise is rejected.
  THEN_REJECTED,
  // While the wait is pending, in the turn that then fulfills its promise.
  FULFILLED_IN_SAME_TURN,
  // As FULFILLED_IN_SAME_TURN, but the promise is rejected.
  REJECTED_IN_SAME_TURN,
  // While the wait is pending, in the turn in which GC then collects its unsettled promise.
  COLLECTED_IN_SAME_TURN,
  // Before the wait starts, in the turn it starts in. Its promise never settles.
  BEFORE_START,
  // Before the wait starts, in the turn it starts in, which then fulfills its promise.
  BEFORE_START_THEN_FULFILLED_IN_SAME_TURN,
  // Once its promise has been fulfilled and the wait has ended.
  AFTER_FULFILLED,
  // Once the wait has been cancelled. Then its promise is fulfilled.
  AFTER_CANCELLED,
  // As THEN_FULFILLED, but nothing waits for the wait until its promise has been fulfilled.
  THEN_FULFILLED_WAITED_LATE,
  // As BEFORE_START, but the promise is fulfilled in a later turn, and nothing waits for the
  // wait until then.
  BEFORE_START_THEN_FULFILLED_WAITED_LATE,
};

kj::StringPtr nameOf(AbortedWait scenario) {
  switch (scenario) {
    case AbortedWait::PENDING:
      return "PENDING"_kj;
    case AbortedWait::THEN_FULFILLED:
      return "THEN_FULFILLED"_kj;
    case AbortedWait::THEN_REJECTED:
      return "THEN_REJECTED"_kj;
    case AbortedWait::FULFILLED_IN_SAME_TURN:
      return "FULFILLED_IN_SAME_TURN"_kj;
    case AbortedWait::REJECTED_IN_SAME_TURN:
      return "REJECTED_IN_SAME_TURN"_kj;
    case AbortedWait::COLLECTED_IN_SAME_TURN:
      return "COLLECTED_IN_SAME_TURN"_kj;
    case AbortedWait::BEFORE_START:
      return "BEFORE_START"_kj;
    case AbortedWait::BEFORE_START_THEN_FULFILLED_IN_SAME_TURN:
      return "BEFORE_START_THEN_FULFILLED_IN_SAME_TURN"_kj;
    case AbortedWait::AFTER_FULFILLED:
      return "AFTER_FULFILLED"_kj;
    case AbortedWait::AFTER_CANCELLED:
      return "AFTER_CANCELLED"_kj;
    case AbortedWait::THEN_FULFILLED_WAITED_LATE:
      return "THEN_FULFILLED_WAITED_LATE"_kj;
    case AbortedWait::BEFORE_START_THEN_FULFILLED_WAITED_LATE:
      return "BEFORE_START_THEN_FULFILLED_WAITED_LATE"_kj;
  }
  KJ_UNREACHABLE;
}

// What a wait whose request is aborted lets the test observe.
struct AbortedWaitOutcome {
  // As jsOutcome() describes it, once the abort and the settling have been handled.
  kj::String outcome;
  kj::Maybe<kj::Exception> error;
  uint mapRuns;
  // The events of its awaitJs resource, as traceAwaitJs() records them.
  kj::String events;
  // The uncaught exceptions logged from awaiting the promise, "Uncaught (in promise)".
  kj::String inPromise;

  // All but `error`, whose location differs by implementation unless it is the abort's.
  kj::String toString() const {
    return kj::str("outcome: ", outcome, "\nmap runs: ", mapRuns, "\nevents: ", events,
        "\nuncaught (in promise): ", inPromise);
  }
};

// Starts a wait in a traced request, whose isolate has an inspector, through C++ awaitJs() or
// Rust's await_js(), aborts the request as `scenario` says, and returns what was observed.
AbortedWaitOutcome abortWait(AbortedWait scenario, Implementation implementation) {
  bool rust = implementation == Implementation::RUST;
  auto watchersBefore = abortWatchers();
  kj::Vector<kj::String> events;
  UncaughtExceptionLog log;
  AbortedWaitOutcome result;
  {
    capnp::MallocMessageBuilder flagsMessage;
    auto params = inspectorParams(flagsMessage);
    params.isolateObserver = kj::atomicRefcounted<TraceObserver<AwaitJsTraceListener>>(events);
    TestFixture fixture(kj::mv(params));
    auto context = fixture.newIoContext();
    auto request = fixture.newIncomingRequest(*context);
    // Once the wait ends, the request has nothing else to wait for, and would be aborted as hung.
    auto pendingEvent = context->registerPendingEvent();
    RecordedCppAwaitJs cpp;
    RecordedCppAwaitJs::mapRuns = 0;

    bool late = scenario == AbortedWait::THEN_FULFILLED_WAITED_LATE ||
        scenario == AbortedWait::BEFORE_START_THEN_FULFILLED_WAITED_LATE;
    auto start = [&](const TestFixture::Environment& env) {
      if (!rust) {
        if (late) {
          cpp.startHeld(env);
        } else {
          cpp.start(env);
        }
      } else if (late) {
        await_js_start(isolateOf(env), AwaitJsStart::Held);
      } else if (scenario == AbortedWait::AFTER_CANCELLED) {
        await_js_start(isolateOf(env), AwaitJsStart::Cancellable);
      } else if (scenario == AbortedWait::COLLECTED_IN_SAME_TURN) {
        await_js_start(isolateOf(env), AwaitJsStart::DropResolver);
      } else {
        await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
      }
    };
    auto settle = [&](jsg::Lock& js, bool fulfill) {
      if (rust) {
        settle_awaited_js(reinterpret_cast<uintptr_t>(js.v8Isolate), fulfill);
      } else {
        cpp.settle(js, fulfill);
      }
    };
    auto abort = [&]() { context->abort(abortException()); };

    switch (scenario) {
      case AbortedWait::PENDING:
      case AbortedWait::THEN_FULFILLED:
      case AbortedWait::THEN_REJECTED:
        fixture.enterContext(*request, start);
        fixture.pollEventLoop();
        abort();
        fixture.pollEventLoop();
        if (scenario != AbortedWait::PENDING) {
          settleOnceAborted(fixture, *context,
              [&](jsg::Lock& js) { settle(js, scenario == AbortedWait::THEN_FULFILLED); });
          fixture.pollEventLoop();
        }
        break;
      case AbortedWait::FULFILLED_IN_SAME_TURN:
      case AbortedWait::REJECTED_IN_SAME_TURN:
        fixture.enterContext(*request, start);
        fixture.pollEventLoop();
        fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
          abort();
          settle(env.js, scenario == AbortedWait::FULFILLED_IN_SAME_TURN);
        });
        fixture.pollEventLoop();
        break;
      case AbortedWait::COLLECTED_IN_SAME_TURN:
        fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
          start(env);
          // Both promises are unreachable once their resolvers are dropped.
          cpp.resolver = kj::none;
        });
        fixture.pollEventLoop();
        fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
          abort();
          env.isolate->LowMemoryNotification();
          // GC collected the reactions, and with them the sender, which holds the watcher.
          if (rust) KJ_EXPECT(abortWatchers() == watchersBefore, abortWatchers(), watchersBefore);
        });
        fixture.pollEventLoop();
        break;
      case AbortedWait::BEFORE_START:
      case AbortedWait::BEFORE_START_THEN_FULFILLED_IN_SAME_TURN:
        fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
          abort();
          start(env);
          if (scenario == AbortedWait::BEFORE_START_THEN_FULFILLED_IN_SAME_TURN) {
            settle(env.js, true);
          }
        });
        fixture.pollEventLoop();
        break;
      case AbortedWait::AFTER_FULFILLED:
        fixture.enterContext(*request, start);
        fixture.enterContext(
            *request, [&](const TestFixture::Environment& env) { settle(env.js, true); });
        fixture.pollEventLoop();
        abort();
        fixture.pollEventLoop();
        break;
      case AbortedWait::AFTER_CANCELLED:
        fixture.enterContext(*request, start);
        fixture.pollEventLoop();
        if (rust) {
          KJ_EXPECT(cancel_awaited_js());
        } else {
          cpp.cancel();
        }
        fixture.pollEventLoop();
        abort();
        fixture.pollEventLoop();
        settleOnceAborted(fixture, *context, [&](jsg::Lock& js) { settle(js, true); });
        fixture.pollEventLoop();
        break;
      case AbortedWait::THEN_FULFILLED_WAITED_LATE:
      case AbortedWait::BEFORE_START_THEN_FULFILLED_WAITED_LATE:
        if (scenario == AbortedWait::THEN_FULFILLED_WAITED_LATE) {
          fixture.enterContext(*request, start);
          fixture.pollEventLoop();
          abort();
        } else {
          fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
            abort();
            start(env);
          });
        }
        fixture.pollEventLoop();
        settleOnceAborted(fixture, *context, [&](jsg::Lock& js) { settle(js, true); });
        fixture.pollEventLoop();
        // Only now is the wait waited for.
        if (rust) {
          KJ_EXPECT(poll_held_awaited_js());
        } else {
          cpp.waitHeld();
          fixture.pollEventLoop();
        }
        break;
    }

    if (rust) {
      result.outcome = jsOutcome();
      result.mapRuns = awaited_js_map_runs();
      if (result.outcome.startsWith("rejected: ")) result.error = awaitedJsError();
    } else {
      result.outcome = kj::str(cpp.outcome);
      result.mapRuns = RecordedCppAwaitJs::mapRuns;
      result.error = kj::mv(cpp.error);
    }
    cpp.wait = kj::none;
    forget_awaited_js_resolver();
    pendingEvent = nullptr;
    fixture.drainAndDestroy(kj::mv(request));
  }
  result.events = kj::strArray(events, "\n");
  kj::Vector<kj::StringPtr> inPromise;
  for (auto& message: log.messages) {
    if (message.contains(IN_PROMISE)) inPromise.add(message);
  }
  result.inPromise = kj::str(inPromise.size());
  return result;
}

// Runs `scenario` through C++ awaitJs() and Rust's await_js(), checks that they are observed
// alike, and returns what was observed of Rust's.
AbortedWaitOutcome abortWaitOnBoth(AbortedWait scenario) {
  auto cpp = abortWait(scenario, Implementation::CPP);
  auto rust = abortWait(scenario, Implementation::RUST);
  KJ_EXPECT(cpp.toString() == rust.toString(), nameOf(scenario), cpp.toString(), rust.toString());
  KJ_IF_SOME(cppError, cpp.error) {
    KJ_IF_SOME(rustError, rust.error) {
      if (cppError.getDetail(ABORT_DETAIL_ID) != kj::none) {
        expectSameException(rustError, cppError);
      }
    } else {
      KJ_FAIL_EXPECT("only C++ failed", nameOf(scenario), cppError);
    }
  }
  return rust;
}

constexpr kj::StringPtr ABORTED_OUTCOME = "rejected: jsg.Error: aborted by the test"_kj;

// The events of the awaitJs resource of a wait whose promise settles as `last` says, or that is
// destroyed unsettled with its request.
kj::String expectedAbortedWaitEvents(kj::StringPtr last) {
  return kj::str("init awaitJs#1 trigger=request exec=request\n", last);
}

KJ_TEST("await_js() fails with the exception its request is aborted with, as C++ awaitJs() does") {
  auto outcome = abortWaitOnBoth(AbortedWait::PENDING);
  KJ_EXPECT(outcome.outcome == ABORTED_OUTCOME, outcome.outcome);
  expectSameException(KJ_ASSERT_NONNULL(outcome.error), abortException());
  KJ_EXPECT(outcome.mapRuns == 0);
  // The promise never settles, so the resource is destroyed with the request.
  KJ_EXPECT(outcome.events == expectedAbortedWaitEvents("destroy awaitJs#1"), outcome.events);
}

KJ_TEST("once the abort has failed an await_js(), its promise's fulfillment runs no conversion and "
        "settles the trace OK, as in C++") {
  auto outcome = abortWaitOnBoth(AbortedWait::THEN_FULFILLED);
  KJ_EXPECT(outcome.outcome == ABORTED_OUTCOME, outcome.outcome);
  KJ_EXPECT(outcome.mapRuns == 0);
  KJ_EXPECT(outcome.events == expectedAbortedWaitEvents("settle awaitJs#1 OK"), outcome.events);
  KJ_EXPECT(outcome.inPromise == "0", outcome.inPromise);
}

KJ_TEST("once the abort has failed an await_js(), its promise's rejection is still logged and "
        "settles the trace ERROR, as in C++") {
  auto outcome = abortWaitOnBoth(AbortedWait::THEN_REJECTED);
  KJ_EXPECT(outcome.outcome == ABORTED_OUTCOME, outcome.outcome);
  KJ_EXPECT(outcome.events == expectedAbortedWaitEvents("settle awaitJs#1 ERROR"), outcome.events);
  KJ_EXPECT(outcome.inPromise == "1", outcome.inPromise);
}

KJ_TEST("a promise that settles in the turn its request is aborted in loses to the abort, as in "
        "C++") {
  auto fulfilled = abortWaitOnBoth(AbortedWait::FULFILLED_IN_SAME_TURN);
  KJ_EXPECT(fulfilled.outcome == ABORTED_OUTCOME, fulfilled.outcome);
  auto rejected = abortWaitOnBoth(AbortedWait::REJECTED_IN_SAME_TURN);
  KJ_EXPECT(rejected.outcome == ABORTED_OUTCOME, rejected.outcome);
  KJ_EXPECT(rejected.inPromise == "1", rejected.inPromise);
}

KJ_TEST("a promise that GC collects in the turn its request is aborted in loses to the abort, as "
        "in C++") {
  auto outcome = abortWaitOnBoth(AbortedWait::COLLECTED_IN_SAME_TURN);
  KJ_EXPECT(outcome.outcome == ABORTED_OUTCOME, outcome.outcome);
  expectSameException(KJ_ASSERT_NONNULL(outcome.error), abortException());
  KJ_EXPECT(outcome.events == expectedAbortedWaitEvents("destroy awaitJs#1"), outcome.events);
}

KJ_TEST("await_js() in a request that was already aborted fails with the abort in a later turn, as "
        "in C++") {
  auto pending = abortWaitOnBoth(AbortedWait::BEFORE_START);
  KJ_EXPECT(pending.outcome == ABORTED_OUTCOME, pending.outcome);
  expectSameException(KJ_ASSERT_NONNULL(pending.error), abortException());

  // A promise that settles in the same turn settles the wait first.
  auto fulfilled = abortWaitOnBoth(AbortedWait::BEFORE_START_THEN_FULFILLED_IN_SAME_TURN);
  KJ_EXPECT(fulfilled.outcome == "fulfilled: 42", fulfilled.outcome);
  KJ_EXPECT(fulfilled.mapRuns == 1);
}

KJ_TEST("when the abort fails an await_js() doesn't depend on when it is waited for, as in C++") {
  // C++ joins the wait with onAbort() as the wait starts, and the first to fire wins, so an abort
  // wins over a promise fulfilled after it even if nothing waited for the wait until then.
  for (auto scenario: {AbortedWait::THEN_FULFILLED_WAITED_LATE,
         AbortedWait::BEFORE_START_THEN_FULFILLED_WAITED_LATE}) {
    auto outcome = abortWaitOnBoth(scenario);
    KJ_EXPECT(outcome.outcome == ABORTED_OUTCOME, nameOf(scenario), outcome.outcome);
    KJ_EXPECT(outcome.mapRuns == 0, nameOf(scenario));
    KJ_EXPECT(outcome.events == expectedAbortedWaitEvents("settle awaitJs#1 OK"), nameOf(scenario),
        outcome.events);
    expectSameException(KJ_ASSERT_NONNULL(outcome.error), abortException());
  }
}

KJ_TEST("an abort after await_js() has ended or been cancelled changes nothing, as in C++") {
  auto fulfilled = abortWaitOnBoth(AbortedWait::AFTER_FULFILLED);
  KJ_EXPECT(fulfilled.outcome == "fulfilled: 42", fulfilled.outcome);
  KJ_EXPECT(fulfilled.mapRuns == 1);
  KJ_EXPECT(fulfilled.events == expectedAbortedWaitEvents("settle awaitJs#1 OK"), fulfilled.events);

  auto cancelled = abortWaitOnBoth(AbortedWait::AFTER_CANCELLED);
  KJ_EXPECT(cancelled.outcome == "cancelled", cancelled.outcome);
  KJ_EXPECT(cancelled.mapRuns == 0);
  KJ_EXPECT(cancelled.events == expectedAbortedWaitEvents("settle awaitJs#1 OK"), cancelled.events);
}

KJ_TEST("await_js() fails with the reason Rust's ctx.abort(reason) aborts with, as C++ awaitJs() "
        "does") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  RecordedCppAwaitJs cpp;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    cpp.start(env);
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
  });
  fixture.pollEventLoop();
  kj::Maybe<kj::Exception> abortReason;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto reason = runScript(env, R"(new RangeError("custom reason"))");
    abort_request(isolateOf(env), ::workerd::rust::jsg::to_ffi(kj::mv(reason)));
    env.isolate->CancelTerminateExecution();
    abortReason = env.context.getAbortReason();
  });
  fixture.pollEventLoop();

  auto& reason = KJ_ASSERT_NONNULL(abortReason, "the request was not aborted");
  KJ_EXPECT(reason.getDescription() == "jsg.RangeError: custom reason", reason);
  // The reason is tunneled as a JavaScript exception is.
  KJ_EXPECT(reason.getDetail(jsg::TUNNELED_EXCEPTION_DETAIL_ID) != kj::none, reason);
  KJ_EXPECT(jsOutcome() == "rejected: jsg.RangeError: custom reason", jsOutcome());
  expectSameException(awaitedJsError(), reason);
  expectSameException(KJ_ASSERT_NONNULL(cpp.error, "C++ awaitJs() has not failed"), reason);
  KJ_EXPECT(awaited_js_map_runs() == 0);
  cpp.wait = kj::none;
  forget_awaited_js_resolver();
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a settled await_js() releases its abort watcher in the reaction, and the abort then "
        "changes nothing") {
  TestFixture fixture;
  auto before = abortWatchers();
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::KeepResolver);
    KJ_EXPECT(abortWatchers() == before + 1);
  });
  fixture.enterContext(*request,
      [&](const TestFixture::Environment& env) { settle_awaited_js(isolateOf(env), true); });
  // The reaction ran at the end of that turn, with the request current.
  KJ_EXPECT(abortWatchers() == before, abortWatchers(), before);
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "fulfilled: 42", jsOutcome());

  request->getContext().abort(abortException());
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "fulfilled: 42", jsOutcome());
  fixture.drainAndDestroy(kj::mv(request));
  KJ_EXPECT(abortWatchers() == before);
}

KJ_TEST("a cancelled await_js() keeps its abort watcher until its sender is dropped, and the "
        "abort then settles nothing") {
  TestFixture fixture;
  auto before = abortWatchers();
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  // As in "a cancelled await_js() does not run its conversion".
  auto pendingEvent = context->registerPendingEvent();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::Cancellable);
  });
  KJ_EXPECT(cancel_awaited_js());
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "cancelled", jsOutcome());
  // The request still owns the sender, which holds the watcher.
  KJ_EXPECT(abortWatchers() == before + 1, abortWatchers(), before);

  // The abort settles nothing, and the promise still runs no conversion.
  context->abort(abortException());
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == "cancelled", jsOutcome());
  KJ_EXPECT(awaited_js_map_runs() == 0);

  // Destroying the IoContext drops the sender, and the watcher with it.
  forget_awaited_js_resolver();
  fixture.drainAndDestroy(kj::mv(request));
  pendingEvent = nullptr;
  context = nullptr;
  KJ_EXPECT(abortWatchers() == before, abortWatchers(), before);
}

KJ_TEST("an await_js() whose promise GC collects releases its abort watcher with its sender") {
  TestFixture fixture;
  auto before = abortWatchers();
  auto owner = fixture.newIncomingRequest();
  fixture.enterContext(*owner, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::DropResolver);
  });
  collectGarbage(fixture, *owner);
  // The sender was dropped in the collection, with the request current.
  KJ_EXPECT(abortWatchers() == before, abortWatchers(), before);
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == NEVER_SETTLED_OUTCOME, jsOutcome());
  fixture.drainAndDestroy(kj::mv(owner));
}

KJ_TEST("an await_js() whose promise GC collects in another request releases its abort watcher "
        "when its request next runs") {
  TestFixture fixture;
  auto before = abortWatchers();
  auto owner = fixture.newIncomingRequest();
  auto other = fixture.newIncomingRequest();
  fixture.enterContext(*owner, [&](const TestFixture::Environment& env) {
    await_js_start(isolateOf(env), AwaitJsStart::DropResolver);
  });
  // The sender is queued for its own request to drop, with the watcher.
  collectGarbage(fixture, *other);
  KJ_EXPECT(abortWatchers() == before + 1, abortWatchers(), before);
  KJ_EXPECT(jsOutcome() == "pending");
  fixture.enterContext(*owner, [&](const TestFixture::Environment&) {});
  KJ_EXPECT(abortWatchers() == before, abortWatchers(), before);
  fixture.pollEventLoop();
  KJ_EXPECT(jsOutcome() == NEVER_SETTLED_OUTCOME, jsOutcome());
  fixture.drainAndDestroy(kj::mv(other));
  fixture.drainAndDestroy(kj::mv(owner));
}

constexpr kj::StringPtr PROBE_SPECIFIER = "test:io-context-probe"_kj;

KJ_TEST("the legacy registry creates a Rust module's exports outside the request resolving it") {
  // The type wrapper is used only to register C++ resources, which this test doesn't.
  struct UnusedTypeWrapper {};
  jsg::CompilationObserver observer;
  jsg::ModuleRegistryImpl<UnusedTypeWrapper> registry(observer);
  ::workerd::rust::jsg::RustModuleRegistry<decltype(registry), SuppressIoContextScope> adapter(
      registry);
  register_probe_module(adapter);

  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(registry.resolve(env.js, kj::Path::parse(PROBE_SPECIFIER), kj::none,
                  jsg::ModuleRegistry::ResolveOption::INTERNAL_ONLY) != kj::none);
    KJ_EXPECT(IoContext::hasCurrent());
  });
  KJ_EXPECT(take_probe_observation() == ProbeObservation::NoRequest);
}

KJ_TEST("the new registry creates a Rust module's exports outside the request resolving it") {
  constexpr auto mainModule = "export default {};"_kjc;
  const auto base = "file:///"_url;
  jsg::modules::ModuleRegistry::Builder registryBuilder(base);
  jsg::modules::ModuleBundle::BundleBuilder bundleBuilder(base);
  bundleBuilder.addEsmModule("main"_kj, jsg::copyToArc(mainModule));
  jsg::modules::ModuleBundle::BuiltinBuilder builtinBuilder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  ::workerd::rust::jsg::RustBuiltinModuleAdapter<SuppressIoContextScope> adapter(builtinBuilder);
  register_probe_module(adapter);
  registryBuilder.add(bundleBuilder.finish()).add(builtinBuilder.finish());

  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setNewModuleRegistry(true);
  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = mainModule,
    .useRealTimers = false,
    .newModuleRegistry = registryBuilder.finish(),
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(env.js.resolveInternalModule(PROBE_SPECIFIER) != kj::none);
    KJ_EXPECT(IoContext::hasCurrent());
  });
  KJ_EXPECT(take_probe_observation() == ProbeObservation::NoRequest);
}

}  // namespace
}  // namespace workerd::io_context::test
