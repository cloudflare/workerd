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

#include <algorithm>
#include <chrono>
#include <thread>

namespace workerd::io_context::test {
namespace {

uintptr_t isolateOf(const TestFixture::V8Environment& env) {
  return reinterpret_cast<uintptr_t>(env.isolate);
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

KJ_TEST("outside a request, current() fails with the error C++ IoContext::current() throws") {
  TestFixture fixture;
  fixture.enterWorkerLock([&](Worker::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](jsg::Lock& js) {
      KJ_EXPECT(!has_current(reinterpret_cast<uintptr_t>(js.v8Isolate)));
      auto rustMessage = current_error(reinterpret_cast<uintptr_t>(js.v8Isolate));
      js.tryCatch([&]() {
        IoContext::current();
        KJ_FAIL_EXPECT("IoContext::current() should throw outside a request");
      }, [&](jsg::Value exception) {
        auto message = jsg::JsValue(exception.getHandle(js)).toString(js);
        KJ_EXPECT(message == kj::str("Error: ", rustMessage.c_str()), message, rustMessage.c_str());
      });
    });
  });
}

KJ_TEST("inside a request, current() finds it, and now() is its clock") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(has_current(isolateOf(env)));
    KJ_EXPECT(current_error(isolateOf(env)) == ::rust::String());
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
    firstId = io_context_id(env.context);
    auto before = nanosNow(env.context);
    auto now = io_context_now(firstId);
    KJ_EXPECT(before <= now && now <= nanosNow(env.context), before, now);
  });

  auto second = fixture.newIncomingRequest();
  fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
    KJ_EXPECT(io_context_id(env.context) != firstId);
    KJ_EXPECT_THROW_MESSAGE("not the one of the current request", io_context_now(firstId));
  });

  fixture.enterWorkerLockSynchronously([&](Worker::Lock&) {
    KJ_EXPECT_THROW_MESSAGE("there is no current request", io_context_now(firstId));
  });

  fixture.drainAndDestroy(kj::mv(second));
  fixture.drainAndDestroy(kj::mv(first));
}

KJ_TEST("await_io() resumes the request when its future completes on another thread") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request,
      [&](const TestFixture::Environment& env) { await_from_thread(isolateOf(env), 42); });
  KJ_EXPECT(await_observation() == AwaitObservation::Pending);

  // The other thread wakes this one's event loop.
  for (auto i KJ_UNUSED: kj::zeroTo(2000)) {
    if (await_observation() != AwaitObservation::Pending) break;
    fixture.pollEventLoop();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  KJ_EXPECT(await_observation() == AwaitObservation::RanInRequest);
  KJ_EXPECT(awaited_value() == 42);
  fixture.drainAndDestroy(kj::mv(request));
}

// The value of TEST_KEY in the frame record_async_context() last saw, or "none".
kj::String recordedAsyncContext = kj::str("not recorded");
kj::Arc<jsg::AsyncContextFrame::StorageKey> TEST_KEY =
    kj::arc<jsg::AsyncContextFrame::StorageKey>();

}  // namespace

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

// Records the KJ-to-JS bridges and operations an async tracker creates, and the causes of turns.
class BridgeListener final: public AsyncTraceListener {
 public:
  explicit BridgeListener(kj::Vector<kj::String>& events): events(events) {}

  void onContextBegin(uint64_t, uint64_t) override {}
  void onInit(uint64_t, const AsyncInitEvent& e) override {
    if (e.kind == AsyncKind::KJ_TO_JS) events.add(kj::str("bridge ", e.id, " ", e.name));
    if (e.kind == AsyncKind::OPERATION) events.add(kj::str("operation ", e.id, " ", e.name));
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

KJ_TEST("await_io() reports its wait under the operation of a user trace span started before") {
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

// A span a SpanRecorder records: its name, the index of its parent, its tags as "key=type:value"
// in key order, and whether it has ended.
struct RecordedSpan {
  kj::String name;
  uint parent;
  kj::String tags;
  bool ended = false;
};

struct SpanRecords: public kj::Refcounted {
  kj::Vector<RecordedSpan> spans;

  kj::Maybe<RecordedSpan&> find(kj::StringPtr name) {
    for (auto& span: spans) {
      if (span.name == name) return span;
    }
    return kj::none;
  }
};

class SpanRecorder final: public SpanObserver {
 public:
  SpanRecorder(kj::Rc<SpanRecords> records, uint parent)
      : records(kj::mv(records)),
        index(this->records->spans.size()) {
    this->records->spans.add(RecordedSpan{.name = kj::str(), .parent = parent, .tags = kj::str()});
  }

  kj::Rc<SpanObserver> newChild() override {
    return kj::rc<SpanRecorder>(records.addRef(), index);
  }
  void onOpen(kj::ConstString operationName, kj::Date) override {
    records->spans[index].name = kj::str(operationName);
  }
  void onClose(kj::Date, Span::TagMap&& tags, kj::Vector<Span::Log>&&) override {
    auto entries = KJ_MAP(entry, tags) {
      kj::String value;
      KJ_SWITCH_ONEOF(entry.value) {
        KJ_CASE_ONEOF(str, kj::ConstString) {
          value = kj::str("str:", str);
        }
        KJ_CASE_ONEOF(b, bool) {
          value = kj::str("bool:", b);
        }
        KJ_CASE_ONEOF(d, double) {
          value = kj::str("float:", d);
        }
        KJ_CASE_ONEOF(i, int64_t) {
          value = kj::str("int:", i);
        }
      }
      return kj::str(entry.key, "=", value);
    };
    std::sort(entries.begin(), entries.end());
    records->spans[index].tags = kj::strArray(entries, " ");
    records->spans[index].ended = true;
  }

 private:
  kj::Rc<SpanRecords> records;
  uint index;
};

class SpanRequestObserver final: public RequestObserver {
 public:
  explicit SpanRequestObserver(kj::Rc<SpanRecords> records)
      : span(kj::rc<SpanRecorder>(kj::mv(records), kj::maxValue), "request"_kjc) {}

  SpanParent getSpan() override {
    return SpanParent(span);
  }

 private:
  SpanBuilder span;
};

TestFixture::SetupParams recordingSpans(kj::Rc<SpanRecords>& records) {
  return {
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&records]() -> kj::Own<RequestObserver> {
    return kj::refcounted<SpanRequestObserver>(records.addRef());
  }),
  };
}

KJ_TEST("trace spans are recorded with their tags, under the request's span") {
  auto records = kj::rc<SpanRecords>();
  TestFixture fixture(recordingSpans(records));
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { record_trace_spans(isolateOf(env)); });
  fixture.drainAndDestroy(kj::mv(request));

  auto& root = KJ_ASSERT_NONNULL(records->find("request"));
  auto rootIndex = &root - records->spans.begin();
  auto& span = KJ_ASSERT_NONNULL(records->find("rust_span"));
  KJ_EXPECT(span.parent == rootIndex);
  KJ_EXPECT(span.ended);
  KJ_EXPECT(
      span.tags == "bool=bool:true float=float:1.5 int=int:42 literal=str:literal str=str:dynamic",
      span.tags);

  auto& child = KJ_ASSERT_NONNULL(records->find("rust_child"));
  KJ_EXPECT(child.parent == &span - records->spans.begin());
  KJ_EXPECT(child.tags == "replaced=int:2", child.tags);

  // Its internal half; the request has no user tracing.
  auto& user = KJ_ASSERT_NONNULL(records->find("rust_user_span"));
  KJ_EXPECT(user.parent == rootIndex);
  KJ_EXPECT(user.ended);
  KJ_EXPECT(user.tags ==
          "bool=bool:false float=float:0.5 int=int:7 literal=str:user_literal str=str:user",
      user.tags);
}

KJ_TEST("a trace span ends when its SpanBuilder is dropped") {
  auto records = kj::rc<SpanRecords>();
  TestFixture fixture(recordingSpans(records));
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { hold_trace_span(isolateOf(env)); });
  KJ_EXPECT(!KJ_ASSERT_NONNULL(records->find("held_span")).ended);
  drop_held_trace_span();
  KJ_EXPECT(KJ_ASSERT_NONNULL(records->find("held_span")).ended);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("trace spans record nothing, and tags are ignored, when the request is not traced") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { record_trace_spans(isolateOf(env)); });
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("await_io() drops its future and callback when the request ends first") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { await_forever(isolateOf(env)); });
  fixture.pollEventLoop();
  KJ_EXPECT(await_observation() == AwaitObservation::Pending);
  fixture.drainAndDestroy(kj::mv(request));
  KJ_EXPECT(await_observation() == AwaitObservation::Dropped);
}

KJ_TEST("add_object() drops the value with the request, unless it was taken") {
  TestFixture fixture;

  auto request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { add_owned_value(isolateOf(env)); });
  fixture.drainAndDestroy(kj::mv(request));
  KJ_EXPECT(owned_value_dropped());
  KJ_EXPECT(!take_owned_value());

  request = fixture.newIncomingRequest();
  fixture.enterContext(
      *request, [&](const TestFixture::Environment& env) { add_owned_value(isolateOf(env)); });
  KJ_EXPECT(!owned_value_dropped());
  KJ_EXPECT(take_owned_value());
  KJ_EXPECT(owned_value_dropped());
  fixture.drainAndDestroy(kj::mv(request));
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
