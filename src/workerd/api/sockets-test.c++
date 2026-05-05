#include "global-scope.h"
#include "sockets.h"

#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

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

KJ_TEST("socket writes are blocked by output gate") {
  bool connectCalled = false;
  kj::HttpHeaderTable headerTable;
  kj::Maybe<kj::AsyncIoStream&> pipeEnd;

  Worker::Actor::Id actorId = kj::str("test-actor-write");
  TestFixture fixture(TestFixture::SetupParams{
    .actorId = kj::mv(actorId),
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Own<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Own<IoChannelFactory> {
    return kj::heap<ConnectTestIoChannelFactory>(timer, connectCalled, headerTable, pipeEnd);
  }),
  });

  static constexpr kj::StringPtr errorsToIgnore[] = {
    "failed to invoke drain()"_kj,
    "no subrequests"_kj,
  };

  fixture.runInIoContext(kj::Function<kj::Promise<void>(const TestFixture::Environment&)>(
                             [&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& actor = env.context.getActorOrThrow();

    // Step 1: Connect without gate lock so the pipe is established.
    auto socket = connectImplNoOutputLock(env.js, kj::none, kj::str("localhost:1234"), kj::none);
    env.js.runMicrotasks();

    // Prepare write data and lock gate BEFORE any co_await (Worker lock still held).
    auto paf = kj::newPromiseAndFulfiller<void>();
    auto blocker = actor.getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);
    auto writable = socket->getWritable();
    auto data = kj::heapArray<kj::byte>({'h', 'i'});
    auto jsBuffer = env.js.bytes(kj::mv(data)).getHandle(env.js);
    writable->getController().write(env.js, jsBuffer).markAsHandled(env.js);

    // With autogate (@all-autogates), connect is deferred. Wait for it.
    // After co_await, Worker lock is released — no V8 calls allowed.
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

// Connect deferral test runs last — its drain errors fire during process exit.
KJ_TEST("connectImplNoOutputLock defers connect until output gate clears") {
  bool connectCalled = false;
  kj::HttpHeaderTable headerTable;
  kj::Maybe<kj::AsyncIoStream&> pipeEnd;

  Worker::Actor::Id actorId = kj::str("test-actor");
  TestFixture fixture(TestFixture::SetupParams{
    .actorId = kj::mv(actorId),
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Own<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Own<IoChannelFactory> {
    return kj::heap<ConnectTestIoChannelFactory>(timer, connectCalled, headerTable, pipeEnd);
  }),
  });

  bool autogateOn = util::Autogate::isEnabled(util::AutogateKey::TCP_SOCKET_CONNECT_OUTPUT_GATE);

  static constexpr kj::StringPtr errorsToIgnore[] = {
    "failed to invoke drain()"_kj,
    "no subrequests"_kj,
  };

  fixture.runInIoContext(kj::Function<kj::Promise<void>(const TestFixture::Environment&)>(
                             [&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& actor = env.context.getActorOrThrow();
    auto paf = kj::newPromiseAndFulfiller<void>();
    auto blocker = actor.getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);

    auto socket = connectImplNoOutputLock(env.js, kj::none, kj::str("localhost:1234"), kj::none);

    if (autogateOn) {
      co_await kj::evalLater([]() {});
      KJ_EXPECT(!connectCalled, "connect must not happen while output gate is locked");
      paf.fulfiller->fulfill();
      co_await kj::evalLater([]() {});
      KJ_EXPECT(connectCalled, "connect must happen after output gate releases");
    } else {
      KJ_EXPECT(connectCalled, "without autogate, connect must happen synchronously");
      paf.fulfiller->fulfill();
      co_await kj::evalLater([]() {});
    }
  }),
      errorsToIgnore);
}

}  // namespace
}  // namespace workerd::api
