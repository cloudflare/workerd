#include "global-scope.h"
#include "sockets.h"

#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

// Minimal WorkerInterface that tracks when connect() is called and exposes the pipe.
class MockConnectWorkerInterface final: public WorkerInterface {
 public:
  MockConnectWorkerInterface(
      bool& connectCalled, kj::HttpHeaderTable& headerTable, kj::Maybe<kj::AsyncIoStream&>& pipeEnd)
      : connectCalled(connectCalled),
        headerTable(headerTable),
        pipeEnd(pipeEnd) {}

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    connectCalled = true;
    pipeEnd = connection;
    kj::HttpHeaders responseHeaders(headerTable);
    response.accept(200, "OK"_kj, responseHeaders);
    return kj::NEVER_DONE;
  }

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }

 private:
  bool& connectCalled;
  kj::HttpHeaderTable& headerTable;
  kj::Maybe<kj::AsyncIoStream&>& pipeEnd;
};

struct ConnectTestIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
  ConnectTestIoChannelFactory(TimerChannel& timer,
      bool& connectCalled,
      kj::HttpHeaderTable& headerTable,
      kj::Maybe<kj::AsyncIoStream&>& pipeEnd)
      : DummyIoChannelFactory(timer),
        connectCalled(connectCalled),
        headerTable(headerTable),
        pipeEnd(pipeEnd) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    return kj::heap<MockConnectWorkerInterface>(connectCalled, headerTable, pipeEnd);
  }

  void abortIsolate(kj::StringPtr reason) override {
    JSG_FAIL_REQUIRE(Error, "abortIsolate() is not implemented for this runtime.");
  }

  bool& connectCalled;
  kj::HttpHeaderTable& headerTable;
  kj::Maybe<kj::AsyncIoStream&>& pipeEnd;
};

// Turn-based timeout: resolves after n event loop turns, returning 0.
kj::Promise<size_t> turnTimeout(int n) {
  for (int i = 0; i < n; i++) {
    co_await kj::evalLater([]() {});
  }
  co_return 0;
}

struct AsyncSendState {
  bool called = false;
  kj::Array<kj::byte> observed;
};

class AsyncObservingDatagramChannel final: public DatagramChannel {
 public:
  explicit AsyncObservingDatagramChannel(AsyncSendState& state): state(state) {}

  kj::Promise<kj::Maybe<kj::Array<kj::byte>>> receive() override {
    return kj::Promise<kj::Maybe<kj::Array<kj::byte>>>(kj::NEVER_DONE);
  }

  kj::Promise<void> send(kj::ArrayPtr<const kj::byte> datagram) override {
    state.called = true;
    return kj::evalLater(
        [state = &state, datagram]() { state->observed = kj::heapArray<kj::byte>(datagram); });
  }

 private:
  AsyncSendState& state;
};

KJ_TEST("UDP writable stream snapshots bytes before asynchronous send") {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture(TestFixture::SetupParams{.featureFlags = flags.asReader()});
  AsyncSendState state;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto socket = setupDatagramSocket(
        env.js, kj::heap<AsyncObservingDatagramChannel>(state), kj::none, kj::none);

    auto data = jsg::JsUint8Array::create(env.js, "before"_kjb);
    auto& handler = KJ_ASSERT_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<Datagram>>());
    auto chunk = jsg::JsValue(handler.wrap(env.js, env.js.alloc<Datagram>(env.js, data)));
    auto writePromise = socket->getWritable(env.js).writeForTest(env.js, chunk);
    env.js.runMicrotasks();
    KJ_REQUIRE(state.called);

    data.asArrayPtr().copyFrom("after!"_kjb);
    return env.context.awaitJs(env.js, kj::mv(writePromise));
  });

  KJ_EXPECT(state.observed.asPtr() == "before"_kjb);
}

class EndedDatagramChannel final: public DatagramChannel {
 public:
  kj::Promise<kj::Maybe<kj::Array<kj::byte>>> receive() override {
    return kj::Maybe<kj::Array<kj::byte>>(kj::none);
  }

  kj::Promise<void> send(kj::ArrayPtr<const kj::byte>) override {
    KJ_FAIL_ASSERT("an ended datagram channel should not be written to");
  }
};

void runDatagramEofTest(bool useTsStreams) {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  flags.setTypeScriptImplementedStreams(useTsStreams);
  TestFixture fixture(TestFixture::SetupParams{
    .featureFlags = flags.asReader(),
    .autogates = useTsStreams ? kj::Maybe(kj::arr("per-isolate-javascript-bootstrap"_kj))
                              : kj::Maybe<kj::Array<kj::StringPtr>>(kj::none),
  });

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto socket = setupDatagramSocket(env.js, kj::heap<EndedDatagramChannel>(), kj::none, kj::none);
    auto& handler = KJ_ASSERT_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<Socket>>());
    auto object = KJ_ASSERT_NONNULL(
        jsg::JsValue(handler.wrap(env.js, socket.addRef())).tryCast<jsg::JsObject>());
    auto closed = KJ_ASSERT_NONNULL(object.get(env.js, "closed"_kj).tryCast<jsg::JsPromise>());
    auto closedPromise = env.context.awaitJs(env.js,
        env.js.toPromise(closed).then(env.js, [](jsg::Lock&, jsg::Value) -> size_t { return 1; }));
    auto readPromise = env.context.awaitJs(env.js, socket->getReadable(env.js).text(env.js, 1024));

    return readPromise.then([closedPromise = kj::mv(closedPromise)](kj::String text) mutable {
      KJ_EXPECT(text.size() == 0);
      return closedPromise.exclusiveJoin(turnTimeout(20)).then([](size_t closed) {
        KJ_EXPECT(closed == 1, "socket.closed must resolve after datagram read EOF");
      });
    });
  });
}

KJ_TEST("UDP socket.closed resolves at read EOF") {
  runDatagramEofTest(false);
}

KJ_TEST("UDP socket.closed resolves at read EOF (TypeScript streams)") {
  runDatagramEofTest(true);
}

struct DatagramEofWriteState {
  uint receiveCalls = 0;
  uint sendCalls = 0;
  kj::Vector<kj::String> sent;
  bool socketClosed = false;
};

class QueuedDatagramChannel final: public DatagramChannel {
 public:
  QueuedDatagramChannel(DatagramEofWriteState& state, kj::Promise<void> sendAllowed)
      : state(state),
        sendAllowed(kj::mv(sendAllowed)) {}

  kj::Promise<kj::Maybe<kj::Array<kj::byte>>> receive() override {
    if (state.receiveCalls++ == 0) {
      return kj::Maybe<kj::Array<kj::byte>>(kj::heapArray<kj::byte>("inbound"_kjb));
    }
    return kj::Maybe<kj::Array<kj::byte>>(kj::none);
  }

  kj::Promise<void> send(kj::ArrayPtr<const kj::byte> datagram) override {
    auto allowed = state.sendCalls++ == 0 ? kj::mv(sendAllowed) : kj::Promise<void>(kj::READY_NOW);
    return allowed.then([state = &state, data = kj::heapString(datagram.asChars())]() mutable {
      state->sent.add(kj::mv(data));
    });
  }

 private:
  DatagramEofWriteState& state;
  kj::Promise<void> sendAllowed;
};

void runDatagramEofPendingWritesTest(bool useTsStreams) {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  flags.setTypeScriptImplementedStreams(useTsStreams);
  TestFixture fixture(TestFixture::SetupParams{
    .featureFlags = flags.asReader(),
    .autogates = useTsStreams ? kj::Maybe(kj::arr("per-isolate-javascript-bootstrap"_kj))
                              : kj::Maybe<kj::Array<kj::StringPtr>>(kj::none),
  });
  DatagramEofWriteState state;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto sendAllowed = kj::newPromiseAndFulfiller<void>();
    auto socket = setupDatagramSocket(env.js,
        kj::heap<QueuedDatagramChannel>(state, kj::mv(sendAllowed.promise)), kj::none, kj::none);
    auto& handler = KJ_ASSERT_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<Socket>>());
    auto object = KJ_ASSERT_NONNULL(
        jsg::JsValue(handler.wrap(env.js, socket.addRef())).tryCast<jsg::JsObject>());
    auto writable = KJ_ASSERT_NONNULL(object.get(env.js, "writable"_kj).tryCast<jsg::JsObject>());
    auto getWriter =
        KJ_ASSERT_NONNULL(writable.get(env.js, "getWriter"_kj).tryCast<jsg::JsFunction>());
    auto writer = KJ_ASSERT_NONNULL(getWriter.call(env.js, writable).tryCast<jsg::JsObject>());
    auto write = KJ_ASSERT_NONNULL(writer.get(env.js, "write"_kj).tryCast<jsg::JsFunction>());
    auto& datagramHandler = KJ_ASSERT_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<Datagram>>());
    for (auto text: {"first"_kjb, "last"_kjb}) {
      auto data = jsg::JsUint8Array::create(env.js, text);
      auto chunk = jsg::JsValue(datagramHandler.wrap(env.js, env.js.alloc<Datagram>(env.js, data)));
      env.js.toPromise(write.call(env.js, writer, chunk)).markAsHandled(env.js);
    }
    auto closedPromise = env.context.awaitJs(env.js,
        env.js.toPromise(object.get(env.js, "closed"_kj))
            .then(env.js, [&state](jsg::Lock&, jsg::Value) -> size_t {
      state.socketClosed = true;
      return 1;
    }));
    auto writableClosed = env.context.awaitJs(env.js,
        env.js.toPromise(writer.get(env.js, "closed"_kj))
            .then(env.js, [](jsg::Lock&, jsg::Value) -> size_t { return 1; }));

    auto readable = KJ_ASSERT_NONNULL(object.get(env.js, "readable"_kj).tryCast<jsg::JsObject>());
    auto getReader =
        KJ_ASSERT_NONNULL(readable.get(env.js, "getReader"_kj).tryCast<jsg::JsFunction>());
    auto reader = KJ_ASSERT_NONNULL(getReader.call(env.js, readable).tryCast<jsg::JsObject>());
    auto read = KJ_ASSERT_NONNULL(reader.get(env.js, "read"_kj).tryCast<jsg::JsFunction>());
    auto readPromise =
        env.js.toPromise(read.call(env.js, reader))
            .then(env.js,
                JSG_VISITABLE_LAMBDA((reader = reader.addRef(env.js)), (reader),
                    (jsg::Lock & js, jsg::Value value) mutable {
                      auto result = KJ_ASSERT_NONNULL(
                          jsg::JsValue(value.getHandle(js)).tryCast<jsg::JsObject>());
                      KJ_EXPECT(result.get(js, "done"_kj).isFalse());
                      auto chunk =
                          KJ_ASSERT_NONNULL(result.get(js, "value"_kj).tryCast<jsg::JsObject>());
                      auto data =
                          KJ_ASSERT_NONNULL(chunk.get(js, "data"_kj).tryCast<jsg::JsUint8Array>());
                      KJ_EXPECT(data.asArrayPtr() == "inbound"_kjb);
                      auto object = reader.getHandle(js);
                      auto read =
                          KJ_ASSERT_NONNULL(object.get(js, "read"_kj).tryCast<jsg::JsFunction>());
                      return js.toPromise(read.call(js, object));
                    }))
            .then(env.js, [](jsg::Lock& js, jsg::Value value) {
      auto result = KJ_ASSERT_NONNULL(jsg::JsValue(value.getHandle(js)).tryCast<jsg::JsObject>());
      KJ_EXPECT(result.get(js, "done"_kj).isTrue());
    });

    return env.context.awaitJs(env.js, kj::mv(readPromise))
        .then(kj::coCapture(
            [&state, sendAllowed = kj::mv(sendAllowed.fulfiller),
                closedPromise = kj::mv(closedPromise),
                writableClosed = kj::mv(writableClosed)]() mutable -> kj::Promise<void> {
      co_await turnTimeout(20);
      KJ_EXPECT(state.receiveCalls == 2);
      KJ_EXPECT(state.sendCalls == 1);
      KJ_EXPECT(state.sent.empty());
      KJ_EXPECT(!state.socketClosed, "read EOF must wait for pending sends");
      sendAllowed->fulfill();
      auto closed = co_await closedPromise.exclusiveJoin(turnTimeout(20));
      KJ_EXPECT(closed == 1, "socket.closed must resolve after pending sends finish");
      KJ_EXPECT(state.sent.size() == 2);
      if (state.sent.size() == 2) {
        KJ_EXPECT(state.sent[0] == "first"_kj);
        KJ_EXPECT(state.sent[1] == "last"_kj);
      }
      if (closed == 1) {
        auto closedWriter = co_await writableClosed.exclusiveJoin(turnTimeout(20));
        KJ_EXPECT(closedWriter == 1, "read EOF must close the locked writer");
      }
    }));
  });
}

KJ_TEST("UDP socket.closed waits for queued writes with a locked writer") {
  runDatagramEofPendingWritesTest(false);
}

KJ_TEST("UDP socket.closed waits for queued writes with a locked writer (TypeScript streams)") {
  runDatagramEofPendingWritesTest(true);
}

void runDatagramEofGcTest(bool useTsStreams) {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  flags.setTypeScriptImplementedStreams(useTsStreams);
  TestFixture fixture(TestFixture::SetupParams{
    .featureFlags = flags.asReader(),
    .autogates = useTsStreams ? kj::Maybe(kj::arr("per-isolate-javascript-bootstrap"_kj))
                              : kj::Maybe<kj::Array<kj::StringPtr>>(kj::none),
  });
  auto request = fixture.newIncomingRequest();
  // Keep the request active independently of socket reachability.
  auto pendingEvent = request->getContext().registerPendingEvent();
  jsg::WeakRef<Socket> droppedSocket = nullptr;

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    env.js.withinHandleScope([&]() {
      auto socket =
          setupDatagramSocket(env.js, kj::heap<EndedDatagramChannel>(), kj::none, kj::none);
      auto& handler = KJ_ASSERT_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<Socket>>());
      handler.wrap(env.js, socket.addRef());
      droppedSocket = socket.getWeakRef(env.js);
    });
  });
  fixture.pollEventLoop();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    env.js.v8Isolate->LowMemoryNotification();
    KJ_EXPECT(droppedSocket.tryAddRef(env.js) == kj::none,
        "observing EOF must not keep an unread socket alive");
  });

  kj::Maybe<JsReadableStream> readable;
  kj::Maybe<kj::Promise<size_t>> closed;
  jsg::WeakRef<Socket> retainedSocket = nullptr;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    env.js.withinHandleScope([&]() {
      auto socket =
          setupDatagramSocket(env.js, kj::heap<EndedDatagramChannel>(), kj::none, kj::none);
      auto& handler = KJ_ASSERT_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<Socket>>());
      auto object = KJ_ASSERT_NONNULL(
          jsg::JsValue(handler.wrap(env.js, socket.addRef())).tryCast<jsg::JsObject>());
      retainedSocket = socket.getWeakRef(env.js);
      readable = socket->getReadable(env.js);
      closed = env.context.awaitJs(env.js,
          env.js.toPromise(object.get(env.js, "closed"_kj))
              .then(env.js, [](jsg::Lock&, jsg::Value) -> size_t { return 1; }));
    });
  });
  fixture.pollEventLoop();

  kj::Maybe<kj::Promise<kj::String>> read;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    env.js.v8Isolate->LowMemoryNotification();
    KJ_EXPECT(retainedSocket.tryAddRef(env.js) != kj::none,
        "a retained readable must preserve EOF socket closure");
    read = env.context.awaitJs(env.js, KJ_ASSERT_NONNULL(readable).text(env.js, 1024));
  });
  KJ_EXPECT(KJ_ASSERT_NONNULL(read).wait(fixture.getWaitScope()).size() == 0);
  auto didClose =
      KJ_ASSERT_NONNULL(closed).exclusiveJoin(turnTimeout(20)).wait(fixture.getWaitScope());
  KJ_EXPECT(didClose == 1, "a destructured readable must still close its socket at EOF");
}

KJ_TEST("UDP EOF observation follows readable reachability") {
  runDatagramEofGcTest(false);
}

KJ_TEST("UDP EOF observation follows readable reachability (TypeScript streams)") {
  runDatagramEofGcTest(true);
}

// The output-gate write test body, run against both stream backends: with useTsStreams
// the typescript_implemented_streams compat flag (plus the bootstrap autogate) is enabled
// and the socket's streams are TypeScript-implemented.
void runSocketWriteOutputGateTest(bool useTsStreams) {
  bool connectCalled = false;
  kj::HttpHeaderTable headerTable;
  kj::Maybe<kj::AsyncIoStream&> pipeEnd;

  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setTypeScriptImplementedStreams(useTsStreams);

  Worker::Actor::Id actorId = kj::str("test-actor-write");
  TestFixture fixture(TestFixture::SetupParams{
    .featureFlags = flags.asReader(),
    .autogates = useTsStreams ? kj::Maybe(kj::arr("per-isolate-javascript-bootstrap"_kj))
                              : kj::Maybe<kj::Array<kj::StringPtr>>(kj::none),
    .actorId = kj::mv(actorId),
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<ConnectTestIoChannelFactory>(timer, connectCalled, headerTable, pipeEnd);
  }),
  });

  static constexpr kj::StringPtr errorsToIgnore[] = {
    "failed to invoke drain()"_kj,
    "no subrequests"_kj,
  };

  fixture.runInIoContext(kj::Function<kj::Promise<void>(const TestFixture::Environment&)>(
                             [&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& actor = env.context.getActorOrThrow();

    // Step 1: Connect before locking the gate so the pipe is established.
    auto socket = connectImpl(env.js, kj::none, kj::str("localhost:1234"), kj::none);
    env.js.runMicrotasks();

    // Prepare write data and lock gate BEFORE any co_await (Worker lock still held).
    auto paf = kj::newPromiseAndFulfiller<void>();
    auto blocker = actor.getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);
    jsg::JsValue jsBuffer = jsg::JsUint8Array::create(env.js, "hi"_kjb);
    socket->getWritable(env.js).writeForTest(env.js, jsBuffer).markAsHandled(env.js);

    // Connect can be deferred by other pending output locks. Wait for it.
    // After co_await, Worker lock is released -- no V8 calls allowed.
    for (int i = 0; i < 10 && pipeEnd == kj::none; i++) {
      co_await kj::evalLater([]() {});
    }
    KJ_ASSERT(connectCalled);
    auto& pipe = KJ_ASSERT_NONNULL(pipeEnd);

    // Step 4: Race tryRead against a turn-based timeout. The output gate is locked,
    // so the write drain is stuck on outputLock — data cannot reach the pipe.
    auto buf = kj::heapArray<kj::byte>(2);
    auto bytesRead =
        co_await pipe.tryRead(buf.begin(), 1, buf.size()).exclusiveJoin(turnTimeout(20));
    KJ_EXPECT(bytesRead == 0, "read must time out while output gate is locked");

    // Step 5: Release the gate.
    paf.fulfiller->fulfill();

    // Step 6: Read again — data should arrive now.
    bytesRead = co_await pipe.tryRead(buf.begin(), 1, buf.size());
    KJ_EXPECT(bytesRead == 2, "read must succeed after output gate releases");
    KJ_EXPECT(buf[0] == 'h');
    KJ_EXPECT(buf[1] == 'i');
  }),
      errorsToIgnore);
}

KJ_TEST("socket writes are blocked by output gate") {
  runSocketWriteOutputGateTest(false);
}

KJ_TEST("socket writes are blocked by output gate (TypeScript streams)") {
  runSocketWriteOutputGateTest(true);
}

// Connect deferral test runs last -- its drain errors fire during process exit.
KJ_TEST("connectImpl defers connect until output gate clears") {
  bool connectCalled = false;
  kj::HttpHeaderTable headerTable;
  kj::Maybe<kj::AsyncIoStream&> pipeEnd;

  Worker::Actor::Id actorId = kj::str("test-actor");
  TestFixture fixture(TestFixture::SetupParams{
    .actorId = kj::mv(actorId),
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<ConnectTestIoChannelFactory>(timer, connectCalled, headerTable, pipeEnd);
  }),
  });

  static constexpr kj::StringPtr errorsToIgnore[] = {
    "failed to invoke drain()"_kj,
    "no subrequests"_kj,
  };

  fixture.runInIoContext(kj::Function<kj::Promise<void>(const TestFixture::Environment&)>(
                             [&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& actor = env.context.getActorOrThrow();
    auto paf = kj::newPromiseAndFulfiller<void>();
    auto blocker = actor.getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);

    auto socket = connectImpl(env.js, kj::none, kj::str("localhost:1234"), kj::none);

    co_await kj::evalLater([]() {});
    KJ_EXPECT(!connectCalled, "connect must not happen while output gate is locked");
    paf.fulfiller->fulfill();
    co_await kj::evalLater([]() {});
    KJ_EXPECT(connectCalled, "connect must happen after output gate releases");
  }),
      errorsToIgnore);
}

}  // namespace
}  // namespace workerd::api
