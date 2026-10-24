#include "global-scope.h"
#include "sockets.h"

#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

#include <cerrno>

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
  explicit AsyncObservingDatagramChannel(kj::Weak<AsyncSendState> state): state(kj::mv(state)) {}

  kj::Promise<kj::Maybe<kj::Array<kj::byte>>> receive() override {
    return kj::Promise<kj::Maybe<kj::Array<kj::byte>>>(kj::NEVER_DONE);
  }

  kj::Promise<void> send(kj::ArrayPtr<const kj::byte> datagram) override {
    KJ_REQUIRE_NONNULL(state.tryGet(), "test state destroyed").called = true;
    return kj::evalLater([state = state, datagram]() {
      KJ_REQUIRE_NONNULL(state.tryGet(), "test state destroyed").observed =
          kj::heapArray<kj::byte>(datagram);
    });
  }

 private:
  kj::Weak<AsyncSendState> state;
};

bool socketHasProtocolProperty(CompatibilityFlags::Reader flags) {
  kj::Pin<AsyncSendState> state;
  TestFixture fixture(TestFixture::SetupParams{.featureFlags = flags});
  bool result = false;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto socket = setupDatagramSocket(
        env.js, kj::heap<AsyncObservingDatagramChannel>(state.addWeak()), kj::none, kj::none);
    auto& handler = KJ_ASSERT_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<Socket>>());
    auto object = KJ_ASSERT_NONNULL(
        jsg::JsValue(handler.wrap(env.js, kj::mv(socket))).tryCast<jsg::JsObject>());
    result = object.has(env.js, "protocol"_kj);
  });

  return result;
}

KJ_TEST("Socket protocol property requires experimental flag") {
  capnp::MallocMessageBuilder disabledFlagsMessage;
  auto disabledFlags = disabledFlagsMessage.initRoot<CompatibilityFlags>();
  disabledFlags.setStreamsJavaScriptControllers(true);
  KJ_EXPECT(!socketHasProtocolProperty(disabledFlags.asReader()));

  capnp::MallocMessageBuilder enabledFlagsMessage;
  auto enabledFlags = enabledFlagsMessage.initRoot<CompatibilityFlags>();
  enabledFlags.setStreamsJavaScriptControllers(true);
  enabledFlags.setWorkerdExperimental(true);
  KJ_EXPECT(socketHasProtocolProperty(enabledFlags.asReader()));
}

KJ_TEST("UDP writable stream snapshots bytes before asynchronous send") {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture(TestFixture::SetupParams{.featureFlags = flags.asReader()});
  kj::Pin<AsyncSendState> state;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto socket = setupDatagramSocket(
        env.js, kj::heap<AsyncObservingDatagramChannel>(state.addWeak()), kj::none, kj::none);

    auto data = jsg::JsUint8Array::create(env.js, "before"_kjb);
    auto& handler = KJ_ASSERT_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<Datagram>>());
    auto chunk = jsg::JsValue(handler.wrap(env.js, env.js.alloc<Datagram>(env.js, data)));
    auto writePromise = socket->getWritable(env.js).writeForTest(env.js, chunk);
    env.js.runMicrotasks();
    KJ_REQUIRE(state->called);

    data.asArrayPtr().copyFrom("after!"_kjb);
    return env.context.awaitJs(env.js, kj::mv(writePromise));
  });

  KJ_EXPECT(state->observed.asPtr() == "before"_kjb);
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

kj::StringPtr errnoFor(kj::StringPtr description) {
  return KJ_ASSERT_NONNULL(connectErrnoName(KJ_EXCEPTION(DISCONNECTED, description)));
}

KJ_TEST("os error codes map to errno names") {
#if _WIN32
  constexpr int refused = 10061, timedOut = 10060, hostUnreach = 10065, netUnreach = 10051;
  KJ_EXPECT(errnoFor("No such host is known. (os error 11001)") == "ENOTFOUND");
  KJ_EXPECT(errnoFor("The requested name is valid, but no data of the requested type was found. "
                     "(os error 11004)") == "ENOTFOUND");
  KJ_EXPECT(errnoFor("This is usually a temporary error. (os error 11002)") == "EAI_AGAIN");
#else
  constexpr int refused = ECONNREFUSED, timedOut = ETIMEDOUT, hostUnreach = EHOSTUNREACH,
                netUnreach = ENETUNREACH;
#endif
  KJ_EXPECT(errnoFor(kj::str("whatever text (os error ", refused, ")")) == "ECONNREFUSED");
  KJ_EXPECT(errnoFor(kj::str("whatever text (os error ", timedOut, ")")) == "ETIMEDOUT");
  KJ_EXPECT(errnoFor(kj::str("whatever text (os error ", hostUnreach, ")")) == "EHOSTUNREACH");
  KJ_EXPECT(errnoFor(kj::str("whatever text (os error ", netUnreach, ")")) == "ENETUNREACH");
  KJ_EXPECT(connectErrnoName(KJ_EXCEPTION(FAILED, "Permission denied (os error 13)")) == kj::none);
  KJ_EXPECT(connectErrnoName(KJ_EXCEPTION(FAILED, "(os error )")) == kj::none);
  // Congruent to `refused` modulo 2^32, so wrapping instead of rejecting would misclassify it.
  auto wrapped = kj::str("(os error ", 4294967296ull + refused, ")");
  KJ_EXPECT(connectErrnoName(KJ_EXCEPTION(FAILED, wrapped)) == kj::none);
}

KJ_TEST("connect failure exceptions map to errno names") {
  KJ_EXPECT(errnoFor("connect(): Connection refused; address = 127.0.0.1:1") == "ECONNREFUSED");
  KJ_EXPECT(errnoFor("connect(): Connection timed out") == "ETIMEDOUT");
  KJ_EXPECT(errnoFor("connect(): Operation timed out") == "ETIMEDOUT");
  KJ_EXPECT(errnoFor("connect(): No route to host") == "EHOSTUNREACH");
  KJ_EXPECT(errnoFor("connect(): Network is unreachable") == "ENETUNREACH");
  KJ_EXPECT(errnoFor("DNS lookup failed.; params.host = nope.invalid; "
                     "gai_strerror(status) = Name or service not known") == "ENOTFOUND");
  KJ_EXPECT(
      errnoFor("failed to lookup address information: Name or service not known") == "ENOTFOUND");
  KJ_EXPECT(
      errnoFor("failed to lookup address information: Temporary failure in name resolution") ==
      "EAI_AGAIN");
  KJ_EXPECT(errnoFor("DNS lookup failed.; gai_strerror(status) = Try again") == "EAI_AGAIN");
  KJ_EXPECT(connectErrnoName(KJ_EXCEPTION(FAILED, "something else went wrong")) == kj::none);
  KJ_EXPECT(connectErrnoName(KJ_EXCEPTION(FAILED, "TLS handshake timed out")) == kj::none);
}

kj::Maybe<kj::StringPtr> errnoForProxyStatus(kj::StringPtr proxyStatus) {
  kj::HttpHeaderTable table;
  kj::HttpHeaders headers(table);
  headers.addPtrPtr("Proxy-Status", proxyStatus);
  return connectErrnoName(headers);
}

KJ_TEST("Proxy-Status error types map to errno names") {
  KJ_EXPECT(KJ_ASSERT_NONNULL(errnoForProxyStatus("Cloudflare-Proxy;error=connection_refused")) ==
      "ECONNREFUSED");
  KJ_EXPECT(KJ_ASSERT_NONNULL(errnoForProxyStatus(
                "proxy; error=\"connection_timeout\"; details=x")) == "ETIMEDOUT");
  KJ_EXPECT(KJ_ASSERT_NONNULL(errnoForProxyStatus("a, b;error=dns_error")) == "ENOTFOUND");
  KJ_EXPECT(KJ_ASSERT_NONNULL(errnoForProxyStatus("p;error=dns_timeout")) == "ENOTFOUND");
  KJ_EXPECT(
      KJ_ASSERT_NONNULL(errnoForProxyStatus("p;error=destination_unavailable")) == "EHOSTUNREACH");
  KJ_EXPECT(KJ_ASSERT_NONNULL(errnoForProxyStatus("p;error=destination_ip_unroutable")) ==
      "EHOSTUNREACH");
  KJ_EXPECT(KJ_ASSERT_NONNULL(errnoForProxyStatus(
                "p;details=\"x; error=dns_error\";error=connection_refused")) == "ECONNREFUSED");
  KJ_EXPECT(KJ_ASSERT_NONNULL(errnoForProxyStatus(
                "p;details=\"upstream error=connection_timeout\";error=connection_refused")) ==
      "ECONNREFUSED");
  KJ_EXPECT(errnoForProxyStatus("Cloudflare-Proxy;error=connection_limit_reached") == kj::none);
  KJ_EXPECT(errnoForProxyStatus("Cloudflare-Proxy") == kj::none);
  KJ_EXPECT(errnoForProxyStatus("p;noterror=connection_refused") == kj::none);
  KJ_EXPECT(errnoForProxyStatus("p;details=\"error=connection_refused\"") == kj::none);
  KJ_EXPECT(errnoForProxyStatus("p;details=\"x;error=connection_refused\"") == kj::none);
  KJ_EXPECT(errnoForProxyStatus("p;details=\"a\\\"b;error=connection_refused\"") == kj::none);
  KJ_EXPECT(errnoForProxyStatus("p;rcode=error=connection_refused") == kj::none);

  kj::HttpHeaderTable table;
  kj::HttpHeaders headers(table);
  headers.addPtrPtr("proxy-status", "p;error=connection_refused");
  KJ_EXPECT(KJ_ASSERT_NONNULL(connectErrnoName(headers)) == "ECONNREFUSED");
  kj::HttpHeaders none(table);
  KJ_EXPECT(connectErrnoName(none) == kj::none);
}

}  // namespace
}  // namespace workerd::api
