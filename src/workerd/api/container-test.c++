// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container.h"

#include <workerd/api/http.h>
#include <workerd/io/observer.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>
#include <workerd/util/stream-utils.h>

#include <capnp/compat/byte-stream.h>
#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

constexpr tracing::TraceId EXPECTED_TRACE_ID(0x0123456789abcdef, 0xfedcba9876543210);
constexpr tracing::SpanId EXPECTED_PARENT_SPAN_ID(0x123456789abcdef0);
constexpr tracing::TraceFlags EXPECTED_TRACE_FLAGS(0x01);

void expectSpanContext(rpc::SpanContext::Reader reader) {
  auto spanContext = tracing::SpanContext::fromCapnp(reader);
  KJ_EXPECT(spanContext.getTraceId() == EXPECTED_TRACE_ID);
  KJ_EXPECT(KJ_ASSERT_NONNULL(spanContext.getSpanId()) == EXPECTED_PARENT_SPAN_ID);
  KJ_EXPECT(KJ_ASSERT_NONNULL(spanContext.getTraceFlags()) == EXPECTED_TRACE_FLAGS);
}

class TracingRequestObserver final: public RequestObserver {
 public:
  SpanParent getSpan() override {
    return SpanParent::fromSpanContext(
        tracing::SpanContext(EXPECTED_TRACE_ID, EXPECTED_PARENT_SPAN_ID, EXPECTED_TRACE_FLAGS));
  }
};

struct CapturedInstance {
  bool isCustom = false;
  kj::String named;
  kj::String image;
  double vcpu = 0;
  uint64_t memoryMib = 0;
  uint64_t diskMb = 0;
};

class MockContainerServer final: public rpc::Container::Server {
 public:
  explicit MockContainerServer(kj::Own<kj::PromiseFulfiller<CapturedInstance>> fulfiller)
      : startFulfiller(kj::mv(fulfiller)) {}
  MockContainerServer(bool& directoryCalled, bool& containerCalled)
      : directoryCalled(directoryCalled),
        containerCalled(containerCalled) {}

  kj::Promise<void> start(StartContext context) override {
    auto params = context.getParams();
    auto instance = params.getInstance();
    CapturedInstance captured;
    auto source = params.getSource();
    if (source.which() == rpc::Container::StartParams::Source::IMAGE) {
      captured.image = kj::str(source.getImage());
    }
    switch (instance.which()) {
      case rpc::Container::StartInstance::NAMED:
        captured.named = kj::str(instance.getNamed());
        break;
      case rpc::Container::StartInstance::CUSTOM: {
        auto custom = instance.getCustom();
        captured.isCustom = true;
        captured.vcpu = custom.getVcpu();
        captured.memoryMib = custom.getMemoryMib();
        captured.diskMb = custom.getDiskMb();
        break;
      }
    }
    KJ_REQUIRE_NONNULL(startFulfiller)->fulfill(kj::mv(captured));
    return kj::READY_NOW;
  }

  kj::Promise<void> monitor(MonitorContext context) override {
    return kj::NEVER_DONE;
  }

  kj::Promise<void> snapshotDirectory(SnapshotDirectoryContext context) override {
    auto params = context.getParams();
    KJ_EXPECT(params.hasSpanContext());
    expectSpanContext(params.getSpanContext());
    KJ_EXPECT(params.getDir() == "/data");
    KJ_EXPECT(params.getName() == "directory-snapshot");
    KJ_REQUIRE_NONNULL(directoryCalled) = true;

    auto snapshot = context.getResults().initSnapshot();
    snapshot.setId("directory-snapshot-id");
    snapshot.setSize(123);
    snapshot.setDir(params.getDir());
    snapshot.setName(params.getName());
    return kj::READY_NOW;
  }

  kj::Promise<void> snapshotContainer(SnapshotContainerContext context) override {
    auto params = context.getParams();
    KJ_EXPECT(params.hasSpanContext());
    expectSpanContext(params.getSpanContext());
    KJ_EXPECT(params.getName() == "container-snapshot");
    KJ_REQUIRE_NONNULL(containerCalled) = true;

    auto snapshot = context.getResults().initSnapshot();
    snapshot.setId("container-snapshot-id");
    snapshot.setSize(456);
    snapshot.setName(params.getName());
    return kj::READY_NOW;
  }

 private:
  kj::Maybe<kj::Own<kj::PromiseFulfiller<CapturedInstance>>> startFulfiller;
  kj::Maybe<bool&> directoryCalled;
  kj::Maybe<bool&> containerCalled;
};

class RestartContainerServer final: public rpc::Container::Server {
 public:
  kj::Promise<void> start(StartContext context) override {
    ++startCount;
    return kj::READY_NOW;
  }

  kj::Promise<void> monitor(MonitorContext context) override {
    if (startCount > 1) {
      context.getResults().setExitCode(0);
      return kj::READY_NOW;
    }

    auto paf = kj::newPromiseAndFulfiller<void>();
    pendingMonitor = kj::mv(context);
    monitorFulfiller = kj::mv(paf.fulfiller);
    return kj::mv(paf.promise);
  }

  kj::Promise<void> destroy(DestroyContext context) override {
    KJ_REQUIRE(startCount == 1);
    KJ_REQUIRE_NONNULL(pendingMonitor).getResults().setExitCode(0);
    KJ_REQUIRE_NONNULL(monitorFulfiller)->fulfill();
    return kj::READY_NOW;
  }

 private:
  uint startCount = 0;
  kj::Maybe<MonitorContext> pendingMonitor;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> monitorFulfiller;
};

// Records the arguments passed to exec()/resize() so tests can assert the pty option is piped
// through correctly.
struct ExecObservations {
  bool execCalled = false;
  bool hadPty = false;
  uint initialCols = 0;
  uint initialRows = 0;
  uint resizeCols = 0;
  uint resizeRows = 0;
};

class MockExecProcessHandle final: public rpc::Container::ProcessHandle::Server {
 public:
  MockExecProcessHandle(capnp::ByteStreamFactory& byteStreamFactory,
      ExecObservations& observations,
      kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> resizeFulfiller)
      : byteStreamFactory(byteStreamFactory),
        observations(observations),
        resizeFulfiller(kj::mv(resizeFulfiller)) {}

  kj::Promise<void> wait(WaitContext context) override {
    context.getResults().setExitCode(0);
    return kj::READY_NOW;
  }

  kj::Promise<void> stdinWriter(StdinWriterContext context) override {
    context.getResults().setWriter(byteStreamFactory.kjToCapnp(
        capnp::ExplicitEndOutputStream::wrap(newNullOutputStream(), []() {})));
    return kj::READY_NOW;
  }

  kj::Promise<void> resize(ResizeContext context) override {
    observations.resizeCols = context.getParams().getCols();
    observations.resizeRows = context.getParams().getRows();
    KJ_IF_SOME(fulfiller, resizeFulfiller) {
      fulfiller->fulfill();
    }
    return kj::READY_NOW;
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  ExecObservations& observations;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> resizeFulfiller;
};

class MockExecContainerServer final: public rpc::Container::Server {
 public:
  MockExecContainerServer(capnp::ByteStreamFactory& byteStreamFactory,
      ExecObservations& observations,
      kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> resizeFulfiller)
      : byteStreamFactory(byteStreamFactory),
        observations(observations),
        resizeFulfiller(kj::mv(resizeFulfiller)) {}

  kj::Promise<void> monitor(MonitorContext context) override {
    return kj::NEVER_DONE;
  }

  kj::Promise<void> exec(ExecContext context) override {
    observations.execCalled = true;
    auto params = context.getParams().getParams();
    observations.hadPty = params.hasPty();
    if (params.hasPty()) {
      observations.initialCols = params.getPty().getCols();
      observations.initialRows = params.getPty().getRows();
    }

    auto process = context.getResults().initProcess();
    process.setPid(4321);
    process.setHandle(
        kj::heap<MockExecProcessHandle>(byteStreamFactory, observations, kj::mv(resizeFulfiller)));
    return kj::READY_NOW;
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  ExecObservations& observations;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> resizeFulfiller;
};

struct CapturedDirectorySnapshot {
  bool hasSnapshotId = false;
  kj::String snapshotId;
  kj::String restorePath;
};

// Captures the directorySnapshots forwarded by Container::start() so tests can assert how each
// DirectorySnapshotRestoreParams is translated into the RPC request.
class DirectorySnapshotStartServer final: public rpc::Container::Server {
 public:
  explicit DirectorySnapshotStartServer(kj::Vector<CapturedDirectorySnapshot>& captured)
      : captured(captured) {}

  kj::Promise<void> start(StartContext context) override {
    for (auto entry: context.getParams().getDirectorySnapshots()) {
      captured.add(CapturedDirectorySnapshot{
        .hasSnapshotId = entry.hasSnapshotId(),
        .snapshotId = kj::str(entry.getSnapshotId()),
        .restorePath = kj::str(entry.getRestorePath()),
      });
    }
    return kj::READY_NOW;
  }

  kj::Promise<void> monitor(MonitorContext context) override {
    return kj::NEVER_DONE;
  }

 private:
  kj::Vector<CapturedDirectorySnapshot>& captured;
};

Container::DirectorySnapshot makeDirectorySnapshot(kj::StringPtr id, kj::StringPtr dir) {
  return Container::DirectorySnapshot{
    .id = kj::str(id),
    .size = 0,
    .dir = kj::str(dir),
    .name = kj::none,
  };
}

enum class TunnelReuseGate { DISABLED, ENABLED };

class AutogateScope {
 public:
  AutogateScope(TunnelReuseGate gate = TunnelReuseGate::ENABLED) {
    if (gate == TunnelReuseGate::ENABLED) {
      util::Autogate::initAutogateNamesForTest(
          {"container-tunnel-reuse"_kj}, util::IgnoreAllAutogatesEnv::YES);
    } else {
      util::Autogate::initAutogateNamesForTest({}, util::IgnoreAllAutogatesEnv::YES);
    }
  }

  ~AutogateScope() noexcept(false) {
    util::Autogate::deinitAutogate();
  }
};

enum class ResponseMode {
  KEEP_ALIVE,
  CLOSE_AFTER_RESPONSE,
  CLOSE_DELIMITED,
  CONNECTION_CLOSE,
  MALFORMED_KEEP_OPEN,
  TRUNCATED_DISCONNECT,
  CONNECT_FAILURE,
  UPSTREAM_FAILURE,
  UPSTREAM_END_FAILURE,
};

enum class WaitForClose { NO, YES };

class FailingOutput final: public kj::AsyncOutputStream {
 public:
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return JSG_KJ_EXCEPTION(DISCONNECTED, Error, "Upstream write failed");
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    return JSG_KJ_EXCEPTION(DISCONNECTED, Error, "Upstream write failed");
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }
};

class FailingEndOutput final: public capnp::ExplicitEndOutputStream {
 public:
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return kj::READY_NOW;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    return kj::READY_NOW;
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

  kj::Promise<void> end() override {
    return JSG_KJ_EXCEPTION(DISCONNECTED, Error, "Upstream end failed");
  }
};

class TestPort final: public rpc::Container::Port::Server, private kj::TaskSet::ErrorHandler {
 public:
  TestPort(capnp::ByteStreamFactory& byteStreamFactory,
      ResponseMode mode,
      size_t& connectCount,
      kj::Maybe<kj::PromiseFulfiller<void>&> closeFulfiller,
      kj::Maybe<bool&> upEndedUncleanly = kj::none,
      bool deferConnect = false)
      : byteStreamFactory(byteStreamFactory),
        mode(mode),
        connectCount(connectCount),
        closeFulfiller(closeFulfiller),
        upEndedUncleanly(upEndedUncleanly),
        deferConnect(deferConnect),
        tasks(*this) {}

  kj::Promise<void> connect(ConnectContext context) override {
    ++connectCount;
    if (mode == ResponseMode::CONNECT_FAILURE) {
      return JSG_KJ_EXCEPTION(FAILED, Error, "Testing error path");
    }
    if (mode == ResponseMode::UPSTREAM_FAILURE) {
      heldDown = byteStreamFactory.capnpToKjExplicitEnd(context.getParams().getDown());
      context.getResults().setUp(byteStreamFactory.kjToCapnp(kj::heap<FailingOutput>()));
      return kj::READY_NOW;
    }
    if (mode == ResponseMode::UPSTREAM_END_FAILURE) {
      heldDown = byteStreamFactory.capnpToKjExplicitEnd(context.getParams().getDown());
      context.getResults().setUp(byteStreamFactory.kjToCapnp(kj::heap<FailingEndOutput>()));
      return kj::READY_NOW;
    }
    auto upPipe = kj::newOneWayPipe();
    kj::Own<kj::AsyncOutputStream> up = kj::mv(upPipe.out);
    KJ_IF_SOME(flag, upEndedUncleanly) {
      // Wrap the receive end so that a clean end() vs. an unclean capability drop becomes
      // observable: the callback fires only if the up stream is destroyed without end().
      up = capnp::ExplicitEndOutputStream::wrap(kj::mv(up), [&flag]() { flag = true; });
    }
    auto upCap = byteStreamFactory.kjToCapnp(kj::mv(up));
    auto down = byteStreamFactory.capnpToKjExplicitEnd(context.getParams().getDown());
    if (deferConnect) {
      // Mirror the real container service: the Port.connect() RPC does not resolve until the
      // connection is torn down, and `up` is delivered early via promise pipelining
      // (context.setPipeline) so the caller can use the tunnel before connect() returns. Returning
      // the serve() promise directly (rather than backgrounding it via tasks and resolving now)
      // keeps the call outstanding for the connection's lifetime. Code under test must therefore
      // hand back a usable stream without waiting for this RPC to complete; otherwise it deadlocks.
      capnp::PipelineBuilder<ConnectResults> pipeline;
      pipeline.setUp(upCap);
      context.setPipeline(pipeline.build());
      context.getResults().setUp(kj::mv(upCap));
      return serve(kj::mv(upPipe.in), kj::mv(down));
    }
    context.getResults().setUp(kj::mv(upCap));
    tasks.add(serve(kj::mv(upPipe.in), kj::mv(down)));
    return kj::READY_NOW;
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  ResponseMode mode;
  size_t& connectCount;
  kj::Maybe<kj::PromiseFulfiller<void>&> closeFulfiller;
  kj::Maybe<bool&> upEndedUncleanly;
  bool deferConnect;
  kj::TaskSet tasks;
  kj::Maybe<kj::Own<capnp::ExplicitEndOutputStream>> heldDown;

  void notifyClose() {
    KJ_IF_SOME(fulfiller, closeFulfiller) {
      fulfiller.fulfill();
    }
  }

  // Reads a request's header block. Resolves to true once a full \r\n\r\n-delimited block has been
  // read, or false if the client cleanly closed the connection first. A client close is a normal
  // end-of-connection for a keep-alive server, not an error, so it must not throw (see taskFailed).
  kj::Promise<bool> readRequest(kj::AsyncInputStream& input) {
    kj::byte buffer[1024];
    size_t matched = 0;
    constexpr kj::byte delimiter[] = {'\r', '\n', '\r', '\n'};
    while (matched < kj::size(delimiter)) {
      auto amount = co_await input.tryRead(buffer, 1, kj::size(buffer));
      if (amount == 0) co_return false;
      for (auto byte: kj::arrayPtr(buffer, amount)) {
        if (byte == delimiter[matched]) {
          ++matched;
        } else {
          matched = byte == delimiter[0] ? 1 : 0;
        }
      }
    }
    co_return true;
  }

  kj::Promise<void> serve(
      kj::Own<kj::AsyncInputStream> input, kj::Own<capnp::ExplicitEndOutputStream> down) {
    for (;;) {
      if (!co_await readRequest(*input)) co_return;
      switch (mode) {
        case ResponseMode::KEEP_ALIVE:
          co_await down->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK"_kjb);
          break;
        case ResponseMode::CLOSE_AFTER_RESPONSE:
          co_await down->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK"_kjb);
          co_await down->end();
          notifyClose();
          co_return;
        case ResponseMode::CLOSE_DELIMITED:
          co_await down->write("HTTP/1.1 200 OK\r\n\r\nOK"_kjb);
          co_await down->end();
          notifyClose();
          co_return;
        case ResponseMode::CONNECTION_CLOSE:
          co_await down->write(
              "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK"_kjb);
          co_await down->end();
          notifyClose();
          co_return;
        case ResponseMode::TRUNCATED_DISCONNECT:
          co_await down->write("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort"_kjb);
          notifyClose();
          // Intentionally omit end() to simulate an unclean capability drop.
          co_return;
        case ResponseMode::CONNECT_FAILURE:
        case ResponseMode::UPSTREAM_FAILURE:
        case ResponseMode::UPSTREAM_END_FAILURE:
          KJ_UNREACHABLE;
        case ResponseMode::MALFORMED_KEEP_OPEN:
          co_await down->write("not an HTTP response\r\n\r\n"_kjb);
          co_await kj::Promise<void>(kj::NEVER_DONE);
      }
    }
  }

  void taskFailed(kj::Exception&& exception) override {
    // serve() exits cleanly when the client closes the connection (see readRequest), so a failure
    // reaching here is not part of normal teardown. Tolerate the benign DISCONNECTED/OVERLOADED
    // exceptions that a teardown race on the write half can still surface, but log anything else as
    // an ERROR -- which fails the test -- so unexpected mock or code-under-test breakage does not
    // pass silently. (This is the same benign/interesting split as util::isInterestingException,
    // inlined to keep the test free of that dependency.)
    if (exception.getType() == kj::Exception::Type::DISCONNECTED ||
        exception.getType() == kj::Exception::Type::OVERLOADED) {
      return;
    }
    KJ_LOG(ERROR, "unexpected container mock serve() failure", exception);
  }
};

class TestContainerServer final: public rpc::Container::Server {
 public:
  TestContainerServer(capnp::ByteStreamFactory& byteStreamFactory,
      ResponseMode mode,
      size_t& connectCount,
      kj::Maybe<kj::PromiseFulfiller<void>&> closeFulfiller = kj::none,
      kj::Maybe<bool&> upEndedUncleanly = kj::none,
      bool deferConnect = false)
      : byteStreamFactory(byteStreamFactory),
        mode(mode),
        connectCount(connectCount),
        closeFulfiller(closeFulfiller),
        upEndedUncleanly(upEndedUncleanly),
        deferConnect(deferConnect) {}

  kj::Promise<void> start(StartContext context) override {
    return kj::READY_NOW;
  }

  kj::Promise<void> monitor(MonitorContext context) override {
    return kj::NEVER_DONE;
  }

  kj::Promise<void> getTcpPort(GetTcpPortContext context) override {
    context.getResults().setPort(kj::heap<TestPort>(
        byteStreamFactory, mode, connectCount, closeFulfiller, upEndedUncleanly, deferConnect));
    return kj::READY_NOW;
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  ResponseMode mode;
  size_t& connectCount;
  kj::Maybe<kj::PromiseFulfiller<void>&> closeFulfiller;
  kj::Maybe<bool&> upEndedUncleanly;
  bool deferConnect;
};

class TestResponse final: public kj::HttpService::Response {
 public:
  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    this->statusCode = statusCode;
    return kj::heap<kj::NullStream>();
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    KJ_UNREACHABLE;
  }

  uint statusCode = 0;
};

class TestConnectResponse final: public kj::HttpService::ConnectResponse {
 public:
  void accept(uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {
    accepted = true;
  }

  kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    KJ_UNREACHABLE;
  }

  bool accepted = false;
};

kj::Promise<void> makeTestRequest(WorkerInterface& client, const kj::HttpHeaderTable& headerTable) {
  kj::HttpHeaders headers(headerTable);
  kj::NullStream requestBody;
  TestResponse response;
  co_await client.request(
      kj::HttpMethod::GET, "http://container/"_kj, headers, requestBody, response);
  KJ_EXPECT(response.statusCode == 200);
}

kj::Promise<void> makeTwoTestRequests(WorkerInterface& first,
    WorkerInterface& second,
    const kj::HttpHeaderTable& headerTable,
    kj::Promise<void> beforeSecond) {
  co_await makeTestRequest(first, headerTable);
  co_await beforeSecond;
  co_await makeTestRequest(second, headerTable);
}

TestFixture makeFixture();

void runTunnelTest(ResponseMode mode,
    size_t expectedConnectCount,
    TunnelReuseGate gate = TunnelReuseGate::ENABLED,
    WaitForClose waitForClose = WaitForClose::NO) {
  auto fixture = makeFixture();
  AutogateScope autogateScope(gate);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;
  kj::Promise<void> beforeSecond = kj::READY_NOW;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> closeFulfiller;
  if (waitForClose == WaitForClose::YES) {
    auto paf = kj::newPromiseAndFulfiller<void>();
    beforeSecond = kj::mv(paf.promise);
    closeFulfiller = kj::mv(paf.fulfiller);
  }

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto container = env.js.alloc<Container>(
        rpc::Container::Client(kj::heap<TestContainerServer>(byteStreamFactory, mode, connectCount,
            closeFulfiller.map([](kj::Own<kj::PromiseFulfiller<void>>& fulfiller)
                                   -> kj::PromiseFulfiller<void>& { return *fulfiller; }))),
        true);
    auto firstFetcher = container->getTcpPort(env.js, 8080);
    auto secondFetcher = container->getTcpPort(env.js, 8080);
    auto first = firstFetcher->getClient(env.context, kj::none, "container"_kjc);
    auto second = secondFetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();
    return makeTwoTestRequests(*first, *second, headerTable, kj::mv(beforeSecond))
        .attach(kj::mv(first), kj::mv(second), kj::mv(firstFetcher), kj::mv(secondFetcher),
            kj::mv(container));
  });

  KJ_EXPECT(connectCount == expectedConnectCount);
}

TestFixture makeFixture() {
  return TestFixture(TestFixture::SetupParams{
    .useRealTimers = false,
    .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
        []() -> kj::Own<RequestObserver> { return kj::refcounted<TracingRequestObserver>(); }),
  });
}

KJ_TEST("Container::start monitors a container that exits immediately") {
  class ImmediateExitContainerServer final: public rpc::Container::Server {
   public:
    kj::Promise<void> start(StartContext context) override {
      started = true;
      return kj::READY_NOW;
    }

    kj::Promise<void> monitor(MonitorContext context) override {
      KJ_EXPECT(started);
      context.getResults().setExitCode(0);
      return kj::READY_NOW;
    }

    bool started = false;
  };

  auto fixture = makeFixture();
  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto weakContext = env.context.getWeakRef();
    auto container =
        kj::rc<Container>(rpc::Container::Client(kj::heap<ImmediateExitContainerServer>()), false);

    container->start(env.js, kj::none);
    KJ_EXPECT(container->getRunning());

    // Give the queued start and monitor RPCs bounded time to run.
    for (auto i = 0; i < 10; ++i) {
      co_await kj::evalLater([]() {});
    }

    auto& context = KJ_ASSERT_NONNULL(weakContext->tryGet());
    co_await context.run([container = kj::mv(container)](
                             Worker::Lock&) mutable { KJ_EXPECT(!container->getRunning()); });
  });
}

KJ_TEST("Container::destroy updates running before restart and clears the old reason") {
  auto fixture = makeFixture();
  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        env.js.alloc<Container>(rpc::Container::Client(kj::heap<RestartContainerServer>()), false);
    container->start(env.js, kj::none);

    auto lifecycle =
        container->destroy(env.js, jsg::Value(env.js.v8Isolate, env.js.error("first lifecycle")))
            .then(env.js, [container = container.addRef()](jsg::Lock& js) mutable {
      KJ_EXPECT(!container->getRunning());
      container->start(js, kj::none);
      KJ_EXPECT(container->getRunning());
      return container->monitor(js);
    });

    return env.context.awaitJs(env.js, kj::mv(lifecycle));
  });
}

KJ_TEST("Container::start forwards a named instance type") {
  auto fixture = makeFixture();
  auto paf = kj::newPromiseAndFulfiller<CapturedInstance>();
  auto promise = kj::mv(paf.promise);

  fixture.runInIoContext([promise = kj::mv(promise), fulfiller = kj::mv(paf.fulfiller)](
                             const TestFixture::Environment& env) mutable {
    auto container = kj::rc<Container>(
        rpc::Container::Client(kj::heap<MockContainerServer>(kj::mv(fulfiller))), false);
    container->start(env.js,
        Container::StartupOptions{
          .instance = kj::str("standard-1"),
        });
    return kj::mv(promise)
        .then([](CapturedInstance captured) {
      KJ_EXPECT(!captured.isCustom);
      KJ_EXPECT(captured.named == "standard-1");
    }).attach(kj::mv(container));
  });
}

KJ_TEST("Container::start forwards custom instance resources") {
  auto fixture = makeFixture();
  auto paf = kj::newPromiseAndFulfiller<CapturedInstance>();
  auto promise = kj::mv(paf.promise);

  fixture.runInIoContext([promise = kj::mv(promise), fulfiller = kj::mv(paf.fulfiller)](
                             const TestFixture::Environment& env) mutable {
    auto container = kj::rc<Container>(
        rpc::Container::Client(kj::heap<MockContainerServer>(kj::mv(fulfiller))), false);
    container->start(env.js,
        Container::StartupOptions{
          .instance =
              Container::StartResources{
                .vcpu = 0.5,
                .memoryMib = 4096,
                .diskMb = 20000,
              },
        });
    return kj::mv(promise)
        .then([](CapturedInstance captured) {
      KJ_EXPECT(captured.isCustom);
      KJ_EXPECT(captured.vcpu == 0.5);
      KJ_EXPECT(captured.memoryMib == 4096);
      KJ_EXPECT(captured.diskMb == 20000);
    }).attach(kj::mv(container));
  });
}

KJ_TEST("Container::start forwards an image") {
  auto fixture = makeFixture();
  auto paf = kj::newPromiseAndFulfiller<CapturedInstance>();
  auto promise = kj::mv(paf.promise);

  fixture.runInIoContext([promise = kj::mv(promise), fulfiller = kj::mv(paf.fulfiller)](
                             const TestFixture::Environment& env) mutable {
    auto container = kj::rc<Container>(
        rpc::Container::Client(kj::heap<MockContainerServer>(kj::mv(fulfiller))), false);
    container->start(env.js,
        Container::StartupOptions{
          .image = kj::str("registry.example.com/image:tag"),
        });
    return kj::mv(promise)
        .then([](CapturedInstance captured) {
      KJ_EXPECT(captured.image == "registry.example.com/image:tag");
    }).attach(kj::mv(container));
  });
}

KJ_TEST("Container::snapshotDirectory propagates the current span context") {
  bool directoryCalled = false;
  bool containerCalled = false;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto container = kj::rc<Container>(
        rpc::Container::Client(kj::heap<MockContainerServer>(directoryCalled, containerCalled)),
        true);
    auto promise = container->snapshotDirectory(env.js,
        Container::DirectorySnapshotOptions{
          .dir = kj::str("/data"),
          .name = kj::str("directory-snapshot"),
        });
    return env.context.awaitJs(env.js, kj::mv(promise)).attach(kj::mv(container));
  });

  KJ_EXPECT(directoryCalled);
  KJ_EXPECT(!containerCalled);
}

KJ_TEST("Container::snapshotContainer propagates the current span context") {
  bool directoryCalled = false;
  bool containerCalled = false;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto container = kj::rc<Container>(
        rpc::Container::Client(kj::heap<MockContainerServer>(directoryCalled, containerCalled)),
        true);
    auto promise = container->snapshotContainer(env.js,
        Container::SnapshotOptions{
          .name = kj::str("container-snapshot"),
        });
    return env.context.awaitJs(env.js, kj::mv(promise)).attach(kj::mv(container));
  });

  KJ_EXPECT(!directoryCalled);
  KJ_EXPECT(containerCalled);
}

KJ_TEST("Container::start restores a directory snapshot using the snapshot's own dir") {
  kj::Vector<CapturedDirectorySnapshot> captured;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = kj::rc<Container>(
        rpc::Container::Client(kj::heap<DirectorySnapshotStartServer>(captured)), false);

    auto snapshots = kj::heapArrayBuilder<Container::DirectorySnapshotRestoreParams>(1);
    snapshots.add(Container::DirectorySnapshotRestoreParams{
      .snapshot = makeDirectorySnapshot("snap-id"_kj, "/data"_kj),
      .mountPoint = kj::none,
    });
    container->start(env.js,
        Container::StartupOptions{
          .directorySnapshots = snapshots.finish(),
        });

    // Give the queued start RPC bounded time to run.
    for (auto i = 0; i < 10; ++i) {
      co_await kj::yield();
    }
  });

  KJ_ASSERT(captured.size() == 1);
  KJ_EXPECT(captured[0].hasSnapshotId);
  KJ_EXPECT(captured[0].snapshotId == "snap-id");
  KJ_EXPECT(captured[0].restorePath == "/data");
}

KJ_TEST("Container::start lets mountPoint override the snapshot's dir as the restore path") {
  kj::Vector<CapturedDirectorySnapshot> captured;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = kj::rc<Container>(
        rpc::Container::Client(kj::heap<DirectorySnapshotStartServer>(captured)), false);

    auto snapshots = kj::heapArrayBuilder<Container::DirectorySnapshotRestoreParams>(1);
    snapshots.add(Container::DirectorySnapshotRestoreParams{
      .snapshot = makeDirectorySnapshot("snap-id"_kj, "/data"_kj),
      .mountPoint = kj::str("/mnt/elsewhere"),
    });
    container->start(env.js,
        Container::StartupOptions{
          .directorySnapshots = snapshots.finish(),
        });

    for (auto i = 0; i < 10; ++i) {
      co_await kj::evalLater([]() {});
    }
  });

  KJ_ASSERT(captured.size() == 1);
  KJ_EXPECT(captured[0].hasSnapshotId);
  KJ_EXPECT(captured[0].snapshotId == "snap-id");
  KJ_EXPECT(captured[0].restorePath == "/mnt/elsewhere");
}

KJ_TEST("Container::start restores to mountPoint when no snapshot is given") {
  kj::Vector<CapturedDirectorySnapshot> captured;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = kj::rc<Container>(
        rpc::Container::Client(kj::heap<DirectorySnapshotStartServer>(captured)), false);

    auto snapshots = kj::heapArrayBuilder<Container::DirectorySnapshotRestoreParams>(1);
    snapshots.add(Container::DirectorySnapshotRestoreParams{
      .snapshot = kj::none,
      .mountPoint = kj::str("/mnt/data"),
    });
    container->start(env.js,
        Container::StartupOptions{
          .directorySnapshots = snapshots.finish(),
        });

    for (auto i = 0; i < 10; ++i) {
      co_await kj::evalLater([]() {});
    }
  });

  KJ_ASSERT(captured.size() == 1);
  KJ_EXPECT(!captured[0].hasSnapshotId);
  KJ_EXPECT(captured[0].snapshotId == "");
  KJ_EXPECT(captured[0].restorePath == "/mnt/data");
}

KJ_TEST("Container::start requires mountPoint when no snapshot is given") {
  kj::Vector<CapturedDirectorySnapshot> captured;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = kj::rc<Container>(
        rpc::Container::Client(kj::heap<DirectorySnapshotStartServer>(captured)), false);

    auto snapshots = kj::heapArrayBuilder<Container::DirectorySnapshotRestoreParams>(1);
    snapshots.add(Container::DirectorySnapshotRestoreParams{
      .snapshot = kj::none,
      .mountPoint = kj::none,
    });

    bool threw = false;
    JSG_TRY(env.js) {
      container->start(env.js,
          Container::StartupOptions{
            .directorySnapshots = snapshots.finish(),
          });
    }
    JSG_CATCH(exception KJ_UNUSED) {
      threw = true;
    }
    KJ_EXPECT(threw, "start() should throw when neither snapshot nor mountPoint is given");

    return kj::READY_NOW;
  });

  KJ_EXPECT(captured.size() == 0);
}

KJ_TEST("Container::exec forwards pty options and resize() sends a resize RPC") {
  ExecObservations observations;
  auto fixture = makeFixture();

  auto paf = kj::newPromiseAndFulfiller<void>();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = kj::rc<Container>(
        rpc::Container::Client(kj::heap<MockExecContainerServer>(
            env.context.getByteStreamFactory(), observations, kj::mv(paf.fulfiller))),
        true);

    ExecOptions options;
    options.pty = kj::OneOf<bool, ExecPtyOptions>(
        ExecPtyOptions{.cols = static_cast<uint16_t>(120), .rows = static_cast<uint16_t>(40)});

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options))
                         .then(env.js, [](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      KJ_EXPECT(process->getIsPty());
      process->resize(js, 100, 50);
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise))
        .then([promise = kj::mv(paf.promise)]() mutable {
      return kj::mv(promise);
    }).attach(kj::mv(container));
  });

  KJ_EXPECT(observations.execCalled);
  KJ_EXPECT(observations.hadPty);
  KJ_EXPECT(observations.initialCols == 120);
  KJ_EXPECT(observations.initialRows == 40);
  KJ_EXPECT(observations.resizeCols == 100);
  KJ_EXPECT(observations.resizeRows == 50);
}

KJ_TEST("Container::exec without pty leaves the process non-pty and rejects resize()") {
  ExecObservations observations;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::rc<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                              env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::none)
                         .then(env.js, [](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      KJ_EXPECT(!process->getIsPty());

      bool threw = false;
      JSG_TRY(js) {
        process->resize(js, 80, 24);
      }
      JSG_CATCH(exception KJ_UNUSED) {
        threw = true;
      }
      KJ_EXPECT(threw, "resize() should throw when the process is not a PTY");
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise)).attach(kj::mv(container));
  });

  KJ_EXPECT(observations.execCalled);
  KJ_EXPECT(!observations.hadPty);
}

KJ_TEST("Container::exec with pty: true boolean shorthand enables PTY with default dimensions") {
  ExecObservations observations;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::rc<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                              env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    ExecOptions options;
    options.pty = kj::OneOf<bool, ExecPtyOptions>(true);

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options))
                         .then(env.js, [](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      KJ_EXPECT(process->getIsPty());
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise)).attach(kj::mv(container));
  });

  KJ_EXPECT(observations.execCalled);
  KJ_EXPECT(observations.hadPty);
  // Boolean shorthand sends no dimensions; the server uses its defaults.
  KJ_EXPECT(observations.initialCols == 0);
  KJ_EXPECT(observations.initialRows == 0);
}

KJ_TEST("Container::exec with pty: {} empty object enables PTY with server defaults") {
  ExecObservations observations;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::rc<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                              env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    ExecOptions options;
    options.pty = kj::OneOf<bool, ExecPtyOptions>(ExecPtyOptions{});

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options))
                         .then(env.js, [](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      KJ_EXPECT(process->getIsPty());
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise)).attach(kj::mv(container));
  });

  KJ_EXPECT(observations.execCalled);
  KJ_EXPECT(observations.hadPty);
  // An object with no dimensions sends no dimensions; the server uses its defaults.
  KJ_EXPECT(observations.initialCols == 0);
  KJ_EXPECT(observations.initialRows == 0);
}

KJ_TEST("Container::exec with pty rejects explicit stderr: \"pipe\"") {
  ExecObservations observations;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::rc<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                              env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    ExecOptions options;
    options.pty = kj::OneOf<bool, ExecPtyOptions>(true);
    options.$stderr.emplace(kj::str("pipe"));

    bool threw = false;
    JSG_TRY(env.js) {
      container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options));
    }
    JSG_CATCH(exception KJ_UNUSED) {
      threw = true;
    }
    KJ_EXPECT(threw, "exec() should throw when stderr is \"pipe\" with PTY enabled");

    return kj::READY_NOW;
  });
}

KJ_TEST("Container::exec resize() rejects zero dimensions") {
  ExecObservations observations;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::rc<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                              env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    ExecOptions options;
    options.pty = kj::OneOf<bool, ExecPtyOptions>(true);

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options))
                         .then(env.js, [](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      // Zero cols should throw
      bool threw = false;
      JSG_TRY(js) {
        process->resize(js, 0, 24);
      }
      JSG_CATCH(exception KJ_UNUSED) {
        threw = true;
      }
      KJ_EXPECT(threw, "resize() should reject zero cols");

      // Zero rows should throw
      threw = false;
      JSG_TRY(js) {
        process->resize(js, 80, 0);
      }
      JSG_CATCH(exception KJ_UNUSED) {
        threw = true;
      }
      KJ_EXPECT(threw, "resize() should reject zero rows");
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise)).attach(kj::mv(container));
  });
}

KJ_TEST("Container::exec resize() rejects out-of-range dimensions") {
  ExecObservations observations;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::rc<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                              env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    ExecOptions options;
    options.pty = kj::OneOf<bool, ExecPtyOptions>(true);

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options))
                         .then(env.js, [](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      // Cols > 65535 should throw
      bool threw = false;
      JSG_TRY(js) {
        process->resize(js, 65536, 24);
      }
      JSG_CATCH(exception KJ_UNUSED) {
        threw = true;
      }
      KJ_EXPECT(threw, "resize() should reject cols > 65535");

      // Negative cols should throw
      threw = false;
      JSG_TRY(js) {
        process->resize(js, -1, 24);
      }
      JSG_CATCH(exception KJ_UNUSED) {
        threw = true;
      }
      KJ_EXPECT(threw, "resize() should reject negative cols");
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise)).attach(kj::mv(container));
  });
}

KJ_TEST("Container::exec process outlives the IoContext its abort action was registered in") {
  // An ExecProcess is a jsg::Object, so GC or isolate teardown destroys it — potentially long
  // after the IoContext that was current when exec() registered its kill-on-abort action on the
  // signal. Releasing that registration therefore has to stay safe once that context is gone.
  ExecObservations observations;
  auto fixture = makeFixture();

  kj::Maybe<jsg::Ref<ExecProcess>> survivor;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::rc<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                              env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    ExecOptions options;
    options.signal = env.js.alloc<AbortSignal>();

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options))
                         .then(env.js, [&survivor](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      // Stands in for a Worker stashing the process somewhere that outlives the request.
      survivor = kj::mv(process);
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise)).attach(kj::mv(container));
  });

  KJ_EXPECT(observations.execCalled);
  KJ_EXPECT(survivor != kj::none);

  // Drop the process from a later request's context, which is where a stashed one would
  // ordinarily be collected.
  fixture.runInIoContext([&](const TestFixture::Environment&) { survivor = kj::none; });
}

KJ_TEST("Container reuses a healthy HTTP tunnel") {
  runTunnelTest(ResponseMode::KEEP_ALIVE, 1);
}

KJ_TEST("Container discards a tunnel closed after a complete response") {
  runTunnelTest(ResponseMode::CLOSE_AFTER_RESPONSE, 2, TunnelReuseGate::ENABLED, WaitForClose::YES);
}

KJ_TEST("Container propagates clean EOF for a close-delimited response") {
  runTunnelTest(ResponseMode::CLOSE_DELIMITED, 2, TunnelReuseGate::ENABLED, WaitForClose::YES);
}

KJ_TEST("Container fetch completes when Port.connect() stays outstanding") {
  // The real container service keeps the Port.connect() RPC outstanding for the lifetime of the
  // connection: the response is not sent until the tunnel is torn down. The tunnel must therefore
  // become usable as soon as the pipelined `up`/`down` streams exist, without waiting for connect()
  // to resolve -- otherwise the request writes and the connect completion deadlock on each other.
  // `deferConnect` reproduces that server behavior; a fetcher that awaited connect() before
  // returning the stream would hang here.
  auto fixture = makeFixture();
  AutogateScope autogateScope(TunnelReuseGate::ENABLED);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto container = env.js.alloc<Container>(
        rpc::Container::Client(kj::heap<TestContainerServer>(byteStreamFactory,
            ResponseMode::KEEP_ALIVE, connectCount, kj::none, kj::none, /*deferConnect=*/true)),
        true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();
    return makeTestRequest(*client, headerTable)
        .attach(kj::mv(client), kj::mv(fetcher), kj::mv(container));
  });

  KJ_EXPECT(connectCount == 1);
}

void runFetchFailureTest(ResponseMode mode,
    TunnelReuseGate gate,
    kj::Maybe<kj::StringPtr> expectedDescription = kj::none) {
  auto fixture = makeFixture();
  AutogateScope autogateScope(gate);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = env.js.alloc<Container>(rpc::Container::Client(kj::heap<TestContainerServer>(
                                                 byteStreamFactory, mode, connectCount)),
        true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();

    kj::HttpHeaders headers(headerTable);
    kj::NullStream requestBody;
    TestResponse response;
    bool threw = false;
    try {
      co_await client->request(
          kj::HttpMethod::GET, "http://container/"_kj, headers, requestBody, response);
    } catch (...) {
      KJ_IF_SOME(description, expectedDescription) {
        auto exception = kj::getCaughtExceptionAsKj();
        KJ_EXPECT(exception.getDescription() == description, exception);
      }
      threw = true;
    }
    KJ_EXPECT(threw, "expected container fetch to fail");
  });

  KJ_EXPECT(connectCount == 1);
}

KJ_TEST("Container fails a fetch when a pooled tunnel disconnects uncleanly") {
  runFetchFailureTest(ResponseMode::TRUNCATED_DISCONNECT, TunnelReuseGate::ENABLED);
}

KJ_TEST("Container fails a fetch when a non-pooled tunnel disconnects uncleanly") {
  runFetchFailureTest(ResponseMode::TRUNCATED_DISCONNECT, TunnelReuseGate::DISABLED);
}

KJ_TEST("Container preserves pooled Port.connect() errors") {
  runFetchFailureTest(
      ResponseMode::CONNECT_FAILURE, TunnelReuseGate::ENABLED, "jsg.Error: Testing error path"_kj);
}

KJ_TEST("Container preserves non-pooled Port.connect() errors") {
  runFetchFailureTest(
      ResponseMode::CONNECT_FAILURE, TunnelReuseGate::DISABLED, "jsg.Error: Testing error path"_kj);
}

KJ_TEST("Container propagates pooled upstream pump failures") {
  runFetchFailureTest(ResponseMode::UPSTREAM_FAILURE, TunnelReuseGate::ENABLED,
      "jsg.Error: Upstream write failed"_kj);
}

KJ_TEST("Container propagates non-pooled upstream pump failures") {
  runFetchFailureTest(ResponseMode::UPSTREAM_FAILURE, TunnelReuseGate::DISABLED,
      "jsg.Error: Upstream write failed"_kj);
}

KJ_TEST("Container fails a raw connect() when the tunnel disconnects uncleanly") {
  auto fixture = makeFixture();
  AutogateScope autogateScope;
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = env.js.alloc<Container>(
        rpc::Container::Client(kj::heap<TestContainerServer>(
            byteStreamFactory, ResponseMode::TRUNCATED_DISCONNECT, connectCount)),
        true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();

    auto pipe = kj::newTwoWayPipe();
    TestConnectResponse response;
    kj::NullStream discard;
    kj::HttpHeaders headers(headerTable);
    kj::HttpConnectSettings settings{.useTls = false};

    auto connectPromise =
        client->connect("container"_kjc, headers, *pipe.ends[0], response, settings);

    // Drive both directions because the pipe is unbuffered.
    auto driveRequest =
        pipe.ends[1]->write("GET / HTTP/1.1\r\n\r\n"_kjb).then([]() -> kj::Promise<void> {
      return kj::NEVER_DONE;
    });
    auto drainResponse = pipe.ends[1]->pumpTo(discard).then(
        [](uint64_t) -> kj::Promise<void> { return kj::NEVER_DONE; });

    bool threw = false;
    try {
      co_await connectPromise.exclusiveJoin(kj::mv(driveRequest))
          .exclusiveJoin(kj::mv(drainResponse));
    } catch (...) {
      threw = true;
    }
    KJ_EXPECT(response.accepted);
    KJ_EXPECT(threw, "expected the truncated tunnel to fail connect()");
  });

  KJ_EXPECT(connectCount == 1);
}

KJ_TEST("Container aborts the up tunnel when a connect() is torn down mid-request") {
  // When the tunnel is torn down before the client has finished writing its request (so no clean
  // half-close/end() has happened), the up direction must be dropped uncleanly -- i.e. reported to
  // the container as an abort, not as a completed request. This matches the pre-tunnel-reuse
  // behavior, where the up ExplicitEndOutputStream was simply destroyed without end() on any
  // abnormal teardown.
  auto fixture = makeFixture();
  AutogateScope autogateScope;
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;
  bool upEndedUncleanly = false;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = env.js.alloc<Container>(
        rpc::Container::Client(kj::heap<TestContainerServer>(byteStreamFactory,
            ResponseMode::TRUNCATED_DISCONNECT, connectCount, kj::none, upEndedUncleanly)),
        true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();

    auto pipe = kj::newTwoWayPipe();
    TestConnectResponse response;
    kj::NullStream discard;
    kj::HttpHeaders headers(headerTable);
    kj::HttpConnectSettings settings{.useTls = false};

    auto connectPromise =
        client->connect("container"_kjc, headers, *pipe.ends[0], response, settings);

    // Write a partial request and then leave the write half open (NEVER_DONE), so the up pump
    // never sees EOF and never performs a clean end(). The container-side truncated disconnect is
    // what tears the tunnel down.
    auto driveRequest =
        pipe.ends[1]->write("GET / HTTP/1.1\r\n\r\n"_kjb).then([]() -> kj::Promise<void> {
      return kj::NEVER_DONE;
    });
    auto drainResponse = pipe.ends[1]->pumpTo(discard).then(
        [](uint64_t) -> kj::Promise<void> { return kj::NEVER_DONE; });

    try {
      co_await connectPromise.exclusiveJoin(kj::mv(driveRequest))
          .exclusiveJoin(kj::mv(drainResponse));
    } catch (...) {
      // Expected: the truncated tunnel fails the connect().
    }

    // Let the aborted up capability's drop propagate back to the container-side receiver.
    for (auto i = 0; i < 20 && !upEndedUncleanly; ++i) {
      co_await kj::evalLater([]() {});
    }
  });

  KJ_EXPECT(connectCount == 1);
  KJ_EXPECT(upEndedUncleanly, "up tunnel should be aborted, not cleanly ended, on teardown");
}

KJ_TEST("Container fails a raw connect() when the upstream half-close fails") {
  auto fixture = makeFixture();
  AutogateScope autogateScope;
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = env.js.alloc<Container>(
        rpc::Container::Client(kj::heap<TestContainerServer>(
            byteStreamFactory, ResponseMode::UPSTREAM_END_FAILURE, connectCount)),
        true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();

    auto pipe = kj::newTwoWayPipe();
    TestConnectResponse response;
    kj::HttpHeaders headers(headerTable);
    kj::HttpConnectSettings settings{.useTls = false};

    auto connectPromise =
        client->connect("container"_kjc, headers, *pipe.ends[0], response, settings);
    auto closeWrite = pipe.ends[1]->write("request"_kjb).then([&pipe]() -> kj::Promise<void> {
      pipe.ends[1]->shutdownWrite();
      return kj::NEVER_DONE;
    });

    bool threw = false;
    try {
      co_await connectPromise.exclusiveJoin(kj::mv(closeWrite));
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_EXPECT(exception.getDescription() == "jsg.Error: Upstream end failed", exception);
      threw = true;
    }
    KJ_EXPECT(response.accepted);
    KJ_EXPECT(threw, "expected the upstream half-close to fail connect()");
  });

  KJ_EXPECT(connectCount == 1);
}

KJ_TEST("Container does not reuse tunnels when the autogate is disabled") {
  runTunnelTest(ResponseMode::KEEP_ALIVE, 2, TunnelReuseGate::DISABLED);
}

KJ_TEST("Container start invalidates pooled tunnels") {
  auto fixture = makeFixture();
  AutogateScope autogateScope;
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;
  auto ioContext = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*ioContext);
  jsg::Ref<Container> container = nullptr;
  kj::Own<WorkerInterface> first;
  kj::Own<WorkerInterface> second;

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    container =
        env.js.alloc<Container>(rpc::Container::Client(kj::heap<TestContainerServer>(
                                    byteStreamFactory, ResponseMode::KEEP_ALIVE, connectCount)),
            false);
    auto fetcher = container->getTcpPort(env.js, 8080);
    first = fetcher->getClient(env.context, kj::none, "container"_kjc);
  });
  makeTestRequest(*first, ioContext->getHeaderTable()).wait(fixture.getWaitScope());

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    container->start(env.js, kj::none);
    auto fetcher = container->getTcpPort(env.js, 8080);
    second = fetcher->getClient(env.context, kj::none, "container"_kjc);
  });
  makeTestRequest(*second, ioContext->getHeaderTable()).wait(fixture.getWaitScope());

  KJ_EXPECT(connectCount == 2);
}

KJ_TEST("Container TCP port fetcher keeps pooled state alive") {
  auto fixture = makeFixture();
  AutogateScope autogateScope;
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto container =
        kj::rc<Container>(rpc::Container::Client(kj::heap<TestContainerServer>(
                              byteStreamFactory, ResponseMode::KEEP_ALIVE, connectCount)),
            true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    container = nullptr;
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    return makeTestRequest(*client, env.context.getHeaderTable())
        .attach(kj::mv(client), kj::mv(fetcher));
  });

  KJ_EXPECT(connectCount == 1);
}

KJ_TEST("Container request errors do not wait for open tunnel pumps") {
  auto fixture = makeFixture();
  AutogateScope autogateScope;
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;
  bool rejected = false;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto container = env.js.alloc<Container>(
        rpc::Container::Client(kj::heap<TestContainerServer>(
            byteStreamFactory, ResponseMode::MALFORMED_KEEP_OPEN, connectCount)),
        true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();
    return makeTestRequest(*client, headerTable)
        .then([]() { KJ_FAIL_EXPECT("malformed response should fail"); },
            [&rejected](kj::Exception&& exception) {
      rejected = true;
    }).attach(kj::mv(client), kj::mv(fetcher), kj::mv(container));
  });

  KJ_EXPECT(rejected);
  KJ_EXPECT(connectCount == 1);
}

}  // namespace
}  // namespace workerd::api
