// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests the io-context crate inside and outside a request of a real worker. The Rust half is
// test_bridge.rs.

#include <workerd/io/async-trace.h>
#include <workerd/io/io-context/ffi.h>
#include <workerd/io/io-context/test_bridge.rs.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/modules.h>
#include <workerd/rust/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/test.h>

#include <bit>

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

// The value of TEST_KEY in the frame record_async_context() last saw, or "none".
kj::String recordedAsyncContext = kj::str("not recorded");
kj::Arc<jsg::AsyncContextFrame::StorageKey> TEST_KEY =
    kj::arc<jsg::AsyncContextFrame::StorageKey>();

// The IoContext the last await_io() callback ran in, if any.
kj::Maybe<uint64_t> awaitContextId;
// Fulfilled when an await_io() callback runs, if set.
kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> onAwaitCallback;

}  // namespace

void record_await_context() {
  awaitContextId = IoContext::tryGetCurrentId().map(
      [](IoContext::Id id) { return std::bit_cast<uint64_t>(id); });
  KJ_IF_SOME(fulfiller, onAwaitCallback) {
    fulfiller->fulfill();
  }
}

void record_async_context(uintptr_t isolate) {
  auto& js = jsg::Lock::from(reinterpret_cast<v8::Isolate*>(isolate));
  recordedAsyncContext = kj::str("none");
  KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(js)) {
    KJ_IF_SOME(value, frame.get(*TEST_KEY)) {
      recordedAsyncContext = jsg::JsValue(value.getHandle(js)).toString(js);
    }
  }
}

namespace {

KJ_TEST("await_io() runs its callback in the async context frame that was current") {
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

// Records the KJ-to-JS bridges an async tracker creates, and the causes of turns.
class BridgeListener final: public AsyncTraceListener {
 public:
  explicit BridgeListener(kj::Vector<kj::String>& events): events(events) {}

  void onContextBegin(uint64_t, uint64_t) override {}
  void onInit(uint64_t, const AsyncInitEvent& e) override {
    if (e.kind == AsyncKind::KJ_TO_JS) events.add(kj::str("bridge ", e.id, " ", e.name));
  }
  void onSettle(uint64_t, AsyncId, AsyncOutcome, uint64_t) override {}
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

class BridgeObserver final: public IsolateObserver {
 public:
  explicit BridgeObserver(kj::Vector<kj::String>& events): events(events) {}

  kj::Maybe<AsyncTraceConfig> getAsyncTraceConfig() const override {
    return AsyncTraceConfig{};
  }
  void addAsyncTraceSinks(
      AsyncTraceSinks& sinks, kj::StringPtr, kj::Maybe<kj::StringPtr>) const override {
    sinks.add(kj::heap<BridgeListener>(events));
  }

 private:
  kj::Vector<kj::String>& events;
};

KJ_TEST("await_io() is recorded for async tracing, as awaitIo() is") {
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

KJ_TEST("await_io() resumes the request when its future completes on another thread") {
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

KJ_TEST("while await_io() waits, the request is not treated as hung") {
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

KJ_TEST("in a Durable Object, the await_io() callback waits for the input gate") {
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

KJ_TEST("await_io() drops its future and callback when the IoContext is destroyed first") {
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

KJ_TEST("an await_io() callback that fails fails the task") {
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

KJ_TEST("an await_io() callback that panics terminates execution and fails the task") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_released(isolateOf(env), AwaitEnding::PanicInCallback);
  });
  release_await(1);
  {
    KJ_EXPECT_LOG(ERROR, "the callback of IoContext::await_io() panicked");
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

KJ_TEST("an await_io() future that panics fails the task, and its callback is dropped") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    await_released(isolateOf(env), AwaitEnding::PanicInFuture);
  });
  release_await(1);
  {
    KJ_EXPECT_LOG(ERROR, "the future that IoContext::await_io() waited for panicked");
    fixture.pollEventLoop();
  }
  KJ_EXPECT(await_observation() == AwaitObservation::Dropped);
  KJ_EXPECT(awaited_future_dropped());
  KJ_EXPECT(context->waitUntilStatus() == EventOutcome::EXCEPTION);
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
