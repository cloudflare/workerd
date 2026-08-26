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
#include <kj/compat/http.h>
#include <kj/test.h>
#include <kj/timer.h>

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
enum class ActorPinGate { DISABLED, ENABLED };

class AutogateScope {
 public:
  AutogateScope(TunnelReuseGate gate = TunnelReuseGate::ENABLED,
      ActorPinGate pinGate = ActorPinGate::DISABLED) {
    kj::Vector<kj::StringPtr> gates(2);
    if (gate == TunnelReuseGate::ENABLED) {
      gates.add("container-tunnel-reuse"_kj);
    }
    if (pinGate == ActorPinGate::ENABLED) {
      gates.add("container-port-actor-pin"_kj);
    }
    util::Autogate::initAutogateNamesForTest(gates.asPtr(), util::IgnoreAllAutogatesEnv::YES);
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
  // Serves a real kj::HttpServer over the tunnel that accepts a WebSocket upgrade and echoes
  // frames back. Used to exercise the upgrade path end-to-end, handshake included.
  WEBSOCKET_ECHO,
  // Like WEBSOCKET_ECHO, but the container drops the socket after the first message instead of
  // completing a close handshake. Models the remote vanishing mid-session.
  WEBSOCKET_ABRUPT_CLOSE,
  // Sends response headers, then withholds the body until the test releases it. This is the
  // deferred-proxying window: headers are through (so the IncomingRequest would be gone in
  // production) but the exchange is not finished.
  BLOCKED_BODY,
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

// Presents the two halves of a mock tunnel as one stream, so a real kj::HttpServer can be run
// over it. Port.connect() hands the directions to us separately (an inbound pipe and an outbound
// ByteStream), but HttpServer wants a single AsyncIoStream.
class JoinedTunnelStream final: public kj::AsyncIoStream {
 public:
  JoinedTunnelStream(kj::Own<kj::AsyncInputStream> in, kj::Own<capnp::ExplicitEndOutputStream> out)
      : in(kj::mv(in)),
        out(kj::mv(out)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return in->tryRead(buffer, minBytes, maxBytes);
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return in->tryGetLength();
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return out->write(buffer);
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    return out->write(pieces);
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return out->whenWriteDisconnected();
  }

  void shutdownWrite() override {
    // No-op. AsyncIoStream's half-close is synchronous, but ExplicitEndOutputStream::end() is a
    // promise, so there is nothing correct to do here synchronously; the peer sees EOF when this
    // object is destroyed.
    //
    // This is only sound because the sole user is serveWebSocket(), and a WebSocket session never
    // half-closes -- it ends by closing the whole connection. Do not reuse this class for a
    // plain HTTP response whose body is connection-close-delimited: the client would wait for an
    // EOF that never arrives.
  }

 private:
  kj::Own<kj::AsyncInputStream> in;
  kj::Own<capnp::ExplicitEndOutputStream> out;
};

// HttpService that accepts a WebSocket upgrade and echoes every message back until the peer
// closes. Runs on the container side of the tunnel.
class WebSocketEchoService final: public kj::HttpService {
 public:
  WebSocketEchoService(const kj::HttpHeaderTable& headerTable, bool abruptAfterFirstMessage)
      : headerTable(headerTable),
        abruptAfterFirstMessage(abruptAfterFirstMessage) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    KJ_REQUIRE(headers.isWebSocket(), "expected a WebSocket upgrade");
    kj::HttpHeaders responseHeaders(headerTable);
    auto ws = response.acceptWebSocket(responseHeaders);
    co_await echo(*ws).attach(kj::mv(ws));
  }

 private:
  const kj::HttpHeaderTable& headerTable;
  bool abruptAfterFirstMessage;

  kj::Promise<void> echo(kj::WebSocket& ws) {
    for (;;) {
      auto message = co_await ws.receive();
      KJ_SWITCH_ONEOF(message) {
        KJ_CASE_ONEOF(text, kj::String) {
          co_await ws.send(kj::StringPtr(text));
          // Returning drops the WebSocket, which tears the connection down with no close frame.
          if (abruptAfterFirstMessage) co_return;
        }
        KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
          co_await ws.send(data.asPtr());
        }
        KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
          co_await ws.close(close.code, close.reason);
          co_return;
        }
      }
    }
  }
};

class TestPort final: public rpc::Container::Port::Server, private kj::TaskSet::ErrorHandler {
 public:
  TestPort(capnp::ByteStreamFactory& byteStreamFactory,
      ResponseMode mode,
      size_t& connectCount,
      kj::Maybe<kj::PromiseFulfiller<void>&> closeFulfiller,
      kj::Maybe<bool&> upEndedUncleanly = kj::none,
      bool deferConnect = false,
      kj::Maybe<kj::ForkedPromise<void>&> releaseBody = kj::none)
      : byteStreamFactory(byteStreamFactory),
        mode(mode),
        connectCount(connectCount),
        closeFulfiller(closeFulfiller),
        upEndedUncleanly(upEndedUncleanly),
        deferConnect(deferConnect),
        releaseBody(releaseBody),
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
    if (mode == ResponseMode::WEBSOCKET_ECHO || mode == ResponseMode::WEBSOCKET_ABRUPT_CLOSE) {
      tasks.add(serveWebSocket(kj::mv(upPipe.in), kj::mv(down)));
    } else {
      tasks.add(serve(kj::mv(upPipe.in), kj::mv(down)));
    }
    return kj::READY_NOW;
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  ResponseMode mode;
  size_t& connectCount;
  kj::Maybe<kj::PromiseFulfiller<void>&> closeFulfiller;
  kj::Maybe<bool&> upEndedUncleanly;
  bool deferConnect;
  // Fires when a BLOCKED_BODY response should finish sending. Forked so serve() can take a branch.
  kj::Maybe<kj::ForkedPromise<void>&> releaseBody;
  kj::TimerImpl wsTimer{kj::origin<kj::TimePoint>()};
  kj::HttpHeaderTable wsHeaderTable;
  WebSocketEchoService wsService{wsHeaderTable, mode == ResponseMode::WEBSOCKET_ABRUPT_CLOSE};
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

  // `wsTimer`, `wsHeaderTable` and `wsService` are members rather than locals because HttpServer
  // holds them by reference for as long as it is serving, and the serving promise outlives this
  // call. They are declared before `tasks`, so they are destroyed after it.
  kj::Promise<void> serveWebSocket(
      kj::Own<kj::AsyncInputStream> input, kj::Own<capnp::ExplicitEndOutputStream> down) {
    auto server = kj::heap<kj::HttpServer>(wsTimer, wsHeaderTable, wsService);
    auto stream = kj::heap<JoinedTunnelStream>(kj::mv(input), kj::mv(down));
    return server->listenHttp(kj::mv(stream)).attach(kj::mv(server));
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
        case ResponseMode::BLOCKED_BODY:
          co_await down->write("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n"_kjb);
          co_await KJ_ASSERT_NONNULL(releaseBody, "BLOCKED_BODY needs a release signal")
              .addBranch();
          co_await down->write("done"_kjb);
          break;
        case ResponseMode::CONNECT_FAILURE:
        case ResponseMode::UPSTREAM_FAILURE:
        case ResponseMode::UPSTREAM_END_FAILURE:
        case ResponseMode::WEBSOCKET_ECHO:
        case ResponseMode::WEBSOCKET_ABRUPT_CLOSE:
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
      bool deferConnect = false,
      kj::Maybe<kj::ForkedPromise<void>&> releaseBody = kj::none)
      : byteStreamFactory(byteStreamFactory),
        mode(mode),
        connectCount(connectCount),
        closeFulfiller(closeFulfiller),
        upEndedUncleanly(upEndedUncleanly),
        deferConnect(deferConnect),
        releaseBody(releaseBody) {}

  kj::Promise<void> start(StartContext context) override {
    return kj::READY_NOW;
  }

  kj::Promise<void> monitor(MonitorContext context) override {
    return kj::NEVER_DONE;
  }

  kj::Promise<void> getTcpPort(GetTcpPortContext context) override {
    context.getResults().setPort(kj::heap<TestPort>(byteStreamFactory, mode, connectCount,
        closeFulfiller, upEndedUncleanly, deferConnect, releaseBody));
    return kj::READY_NOW;
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  ResponseMode mode;
  size_t& connectCount;
  kj::Maybe<kj::PromiseFulfiller<void>&> closeFulfiller;
  kj::Maybe<bool&> upEndedUncleanly;
  bool deferConnect;
  kj::Maybe<kj::ForkedPromise<void>&> releaseBody;
};

// Response that reports when the headers arrive, separately from when the exchange finishes. Used
// to observe the window between the two, which is where deferred proxying drops the IncomingRequest.
class HeaderSignalResponse final: public kj::HttpService::Response {
 public:
  explicit HeaderSignalResponse(kj::PromiseFulfiller<void>& headersFulfiller)
      : headersFulfiller(headersFulfiller) {}

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    this->statusCode = statusCode;
    headersFulfiller.fulfill();
    return kj::heap<kj::NullStream>();
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    KJ_UNREACHABLE;
  }

  uint statusCode = 0;

 private:
  kj::PromiseFulfiller<void>& headersFulfiller;
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

// Records the RequestTracker transitions that drive hibernation. `active()` fires on 0 -> 1 and
// `inactive()` on 1 -> 0, so the counts also tell us whether transitions were coalesced.
class RecordingTrackerHooks final: public RequestTracker::Hooks {
 public:
  void active() override {
    ++activeCount;
  }
  void inactive() override {
    ++inactiveCount;
  }

  uint activeCount = 0;
  uint inactiveCount = 0;
};

// A fixture whose IoContext has an actor carrying `tracker`. The plain makeFixture() has no actor
// at all, which is the "no actor to pin" case; these tests need one so that
// Worker::Actor::addRef() produces a RequestTracker::ActiveRequest.
//
// Note the fixture's IncomingRequest is built directly rather than via WorkerEntrypoint, so it
// holds no actor reference of its own. The tracker therefore starts inactive and reflects only
// what the code under test pins.
TestFixture makeActorFixture(RequestTracker& tracker) {
  return TestFixture(TestFixture::SetupParams{
    .actorId = Worker::Actor::Id(kj::str("container-pin-test")),
    .useRealTimers = false,
    .actorRequestTracker = tracker,
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

// =======================================================================================
// Actor pinning.
//
// A container tunnel is bound to the actor: the rpc::Container::Client it derives from is owned by
// Worker::Actor::Impl and arrives on the actor-start RPC, so hibernating the actor revokes the
// tunnel mid-stream. Hibernation is driven by RequestTracker reaching zero active requests, and
// deferred proxying releases the IncomingRequest -- the thing that normally holds the actor active
// -- as soon as the response headers are through. So each open connection must hold the actor
// active itself, and must stop doing so promptly when the connection ends, however it ends.
//
// Each test below runs with the pin gate both on and off, and asserts the opposite outcome in each
// case. The gate-off runs are what stop these tests from passing vacuously: the actor ref used to
// be held for the WorkerInterface's whole lifetime, which made assertions like "is the actor
// pinned while a request is in flight" true whether or not the pin mechanism worked at all.

// True iff the gate is on. Keeps the expectations below readable.
bool pinned(ActorPinGate gate) {
  return gate == ActorPinGate::ENABLED;
}

// Runs a plain fetch through a container TCP port and checks the actor is pinned for the exchange
// and released afterwards. Covers both the pooled and non-pooled tunnel paths, which are separate
// call sites in TcpPortWorkerInterface::request().
void runFetchPinTest(TunnelReuseGate gate, ActorPinGate pinGate) {
  RecordingTrackerHooks hooks;
  auto tracker = kj::refcounted<RequestTracker>(hooks);
  auto fixture = makeActorFixture(*tracker);
  AutogateScope autogateScope(gate, pinGate);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  KJ_EXPECT(!tracker->isActive(), "the fixture itself must not pin the actor");

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto container =
        env.js.alloc<Container>(rpc::Container::Client(kj::heap<TestContainerServer>(
                                    byteStreamFactory, ResponseMode::KEEP_ALIVE, connectCount)),
            true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();

    auto promise = makeTestRequest(*client, headerTable);
    KJ_EXPECT(tracker->isActive() == pinned(pinGate));

    return promise.attach(kj::mv(client), kj::mv(fetcher), kj::mv(container));
  });

  KJ_EXPECT(connectCount == 1);
  KJ_EXPECT(!tracker->isActive(), "the pin must be released once the exchange completes");
  KJ_EXPECT(hooks.activeCount == (pinned(pinGate) ? 1 : 0));
  KJ_EXPECT(hooks.inactiveCount == (pinned(pinGate) ? 1 : 0));
}

KJ_TEST("Container fetch pins the actor for a pooled tunnel") {
  runFetchPinTest(TunnelReuseGate::ENABLED, ActorPinGate::ENABLED);
}

KJ_TEST("Container fetch does not pin a pooled tunnel when gated off") {
  runFetchPinTest(TunnelReuseGate::ENABLED, ActorPinGate::DISABLED);
}

KJ_TEST("Container fetch pins the actor for a non-pooled tunnel") {
  runFetchPinTest(TunnelReuseGate::DISABLED, ActorPinGate::ENABLED);
}

KJ_TEST("Container fetch does not pin a non-pooled tunnel when gated off") {
  runFetchPinTest(TunnelReuseGate::DISABLED, ActorPinGate::DISABLED);
}

// What happens to the pin between "headers delivered" and "body finished".
enum class BlockedBodyEnding { COMPLETES, CANCELLED };

// The case the whole change exists for. In production, deferred proxying releases the
// IncomingRequest -- the thing that normally keeps the actor active -- as soon as the response
// headers are through. If the pin were released at the same point, the actor could hibernate while
// the body was still streaming and the tunnel would die mid-response. Every other fetch test here
// uses a 2-byte body that completes immediately, so none of them can see this window at all.
void runBlockedBodyPinTest(TunnelReuseGate gate, BlockedBodyEnding ending) {
  RecordingTrackerHooks hooks;
  auto tracker = kj::refcounted<RequestTracker>(hooks);
  auto fixture = makeActorFixture(*tracker);
  AutogateScope autogateScope(gate, ActorPinGate::ENABLED);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto release = kj::newPromiseAndFulfiller<void>();
    auto releaseForked = release.promise.fork();
    auto headers1 = kj::newPromiseAndFulfiller<void>();

    auto container = env.js.alloc<Container>(
        rpc::Container::Client(kj::heap<TestContainerServer>(byteStreamFactory,
            ResponseMode::BLOCKED_BODY, connectCount, kj::none, kj::none, false, releaseForked)),
        true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();

    kj::HttpHeaders headers(headerTable);
    kj::NullStream requestBody;
    HeaderSignalResponse response(*headers1.fulfiller);

    auto requestPromise = client->request(
        kj::HttpMethod::GET, "http://container/"_kj, headers, requestBody, response);

    // Wait for the headers to arrive. The body is still blocked at the container.
    co_await kj::mv(headers1.promise);
    KJ_EXPECT(response.statusCode == 200);
    KJ_EXPECT(tracker->isActive(),
        "the pin must outlive the response headers -- this is the deferred-proxying window");

    if (ending == BlockedBodyEnding::CANCELLED) {
      // Abandoning the fetch mid-body must release the pin, not strand it.
      requestPromise = nullptr;
      KJ_EXPECT(!tracker->isActive(), "cancelling mid-body must release the pin");
    } else {
      release.fulfiller->fulfill();
      co_await requestPromise;
      KJ_EXPECT(!tracker->isActive(), "completing the body must release the pin");
    }
  });

  KJ_EXPECT(connectCount == 1);
  KJ_EXPECT(hooks.activeCount == 1);
  KJ_EXPECT(hooks.inactiveCount == 1);
}

KJ_TEST("Container fetch holds the pin across a blocked body, pooled") {
  runBlockedBodyPinTest(TunnelReuseGate::ENABLED, BlockedBodyEnding::COMPLETES);
}

KJ_TEST("Container fetch holds the pin across a blocked body, non-pooled") {
  runBlockedBodyPinTest(TunnelReuseGate::DISABLED, BlockedBodyEnding::COMPLETES);
}

KJ_TEST("Container fetch releases the pin when cancelled mid-body, pooled") {
  runBlockedBodyPinTest(TunnelReuseGate::ENABLED, BlockedBodyEnding::CANCELLED);
}

KJ_TEST("Container fetch releases the pin when cancelled mid-body, non-pooled") {
  runBlockedBodyPinTest(TunnelReuseGate::DISABLED, BlockedBodyEnding::CANCELLED);
}

KJ_TEST("Container fetch does not pin when there is no actor") {
  // getTcpPort() is only reachable from a Durable Object today, but TcpPortWorkerInterface must not
  // assume that: IoContext::getActor() returns none outside an actor, and the pin is simply absent.
  // makeFixture() builds an actor-less IoContext, so this exercises that path with the gate on.
  auto fixture = makeFixture();
  AutogateScope autogateScope(TunnelReuseGate::ENABLED, ActorPinGate::ENABLED);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(env.context.getActor() == kj::none);
    auto container =
        env.js.alloc<Container>(rpc::Container::Client(kj::heap<TestContainerServer>(
                                    byteStreamFactory, ResponseMode::KEEP_ALIVE, connectCount)),
            true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    return makeTestRequest(*client, env.context.getHeaderTable())
        .attach(kj::mv(client), kj::mv(fetcher), kj::mv(container));
  });

  KJ_EXPECT(connectCount == 1);
}

// Which side ends a raw tunnel. The pin must be released either way: the actor is pinned to protect
// a connection that is in use, so anything that ends the connection must end the pin, and neither
// case may depend on the JS Socket object being dropped or garbage collected.
enum class TunnelCloser { CONTAINER, CLIENT };

void runConnectPinTest(ActorPinGate pinGate, TunnelCloser closer) {
  RecordingTrackerHooks hooks;
  auto tracker = kj::refcounted<RequestTracker>(hooks);
  auto fixture = makeActorFixture(*tracker);
  AutogateScope autogateScope(TunnelReuseGate::ENABLED, pinGate);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  // When the container is the one to close, it disconnects after the first request; when the client
  // closes, the container would happily stay open forever.
  auto mode = closer == TunnelCloser::CONTAINER ? ResponseMode::TRUNCATED_DISCONNECT
                                                : ResponseMode::KEEP_ALIVE;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = env.js.alloc<Container>(rpc::Container::Client(kj::heap<TestContainerServer>(
                                                 byteStreamFactory, mode, connectCount)),
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
    KJ_EXPECT(tracker->isActive() == pinned(pinGate), "an open tunnel must pin iff gated on");

    if (closer == TunnelCloser::CONTAINER) {
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
        auto exception = kj::getCaughtExceptionAsKj();
        // Validate the *kind* of failure. A bare catch(...) would also accept, say, a graceful
        // close being misreported, or a mock bug -- which matters here because connectImpl() races
        // clean completion against rejectWhenDisconnected().
        KJ_EXPECT(exception.getType() == kj::Exception::Type::DISCONNECTED, exception);
        threw = true;
      }
      KJ_EXPECT(threw, "expected the container's disconnect to fail connect()");
      KJ_EXPECT(response.accepted);
    } else {
      // Complete a real round trip first, so the tunnel is definitely open and the pin definitely
      // taken. Awaiting our own reads and writes is what lets the event loop turn and the tunnel
      // get established; connectPromise resumes on the loop even though nobody awaits it.
      co_await pipe.ends[1]->write("GET / HTTP/1.1\r\n\r\n"_kjb);
      char buffer[64];
      auto amount = co_await pipe.ends[1]->tryRead(buffer, 1, kj::size(buffer));
      KJ_EXPECT(amount > 0, "expected the container to respond before we abandon the tunnel");
      KJ_EXPECT(tracker->isActive() == pinned(pinGate), "still open, so still pinned");

      // Now abandon it from this side. Cancelling destroys connectImpl()'s frame, which is what
      // must drop the pin -- note there is no JS Socket here to be collected, which is the point:
      // release must not depend on one.
      connectPromise = nullptr;
    }

    KJ_EXPECT(!tracker->isActive(), "the pin must be gone as soon as the tunnel ends");
  });

  KJ_EXPECT(connectCount == 1);
  KJ_EXPECT(!tracker->isActive());
  KJ_EXPECT(hooks.activeCount == (pinned(pinGate) ? 1 : 0));
  KJ_EXPECT(hooks.inactiveCount == (pinned(pinGate) ? 1 : 0));
}

KJ_TEST("Container connect() unpins the actor when the container closes") {
  runConnectPinTest(ActorPinGate::ENABLED, TunnelCloser::CONTAINER);
}

KJ_TEST("Container connect() unpins the actor when the client closes") {
  runConnectPinTest(ActorPinGate::ENABLED, TunnelCloser::CLIENT);
}

KJ_TEST("Container connect() resolves cleanly when both directions close") {
  // Regression test for connectImpl(). Both pump tasks used to end in kj::NEVER_DONE, so the join
  // could only ever complete by *failing*: a fully clean close never resolved connect() at all,
  // the coroutine never reached co_return, and its locals -- including the pin -- were never
  // destroyed. This test asserts the clean path both resolves and releases.
  //
  // It also covers the race introduced by resolving on the clean path: connectImpl() now does
  // cleanClose.exclusiveJoin(portStream.rejectWhenDisconnected()), and the container drops its
  // capability as part of closing. If the disconnect could win that race, this test would fail
  // intermittently, so it is worth running at a high --runs_per_test.
  RecordingTrackerHooks hooks;
  auto tracker = kj::refcounted<RequestTracker>(hooks);
  auto fixture = makeActorFixture(*tracker);
  AutogateScope autogateScope(TunnelReuseGate::ENABLED, ActorPinGate::ENABLED);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = env.js.alloc<Container>(
        rpc::Container::Client(kj::heap<TestContainerServer>(
            byteStreamFactory, ResponseMode::CLOSE_AFTER_RESPONSE, connectCount)),
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
    KJ_EXPECT(tracker->isActive());

    // Half-close our side so the up pump reaches EOF; the container ends the down direction after
    // responding. Both directions closing cleanly is what makes connectImpl() resolve rather than
    // fail, and draining to EOF is what lets the down pump finish.
    co_await pipe.ends[1]->write("GET / HTTP/1.1\r\n\r\n"_kjb);
    pipe.ends[1]->shutdownWrite();

    kj::NullStream discard;
    co_await pipe.ends[1]->pumpTo(discard);

    // Must not throw: this is a graceful close, and reporting it as a disconnect would surface a
    // spurious socket error to the DO.
    co_await kj::mv(connectPromise);

    KJ_EXPECT(!tracker->isActive(), "a clean close must release the pin");
  });

  KJ_EXPECT(connectCount == 1);
  KJ_EXPECT(hooks.activeCount == 1);
  KJ_EXPECT(hooks.inactiveCount == 1);
}

KJ_TEST("Container connect() unpins when the tunnel closes, not when the socket is dropped") {
  // The distinction this test exists for: "the tunnel closed" and "JS dropped the Socket" are
  // different events, potentially far apart. A DO can hold a Socket in a Map long after the remote
  // end has gone away, and IoOwn teardown additionally waits on V8 collecting the wrapper.
  //
  // Attaching the pin to the connect promise (promise.attach(pin)) ties its release to the *later*
  // event, because a promise's attachments are destroyed when the promise node is destroyed, not
  // when the promise settles -- and kj's connect adapter attaches that node to the returned stream
  // (kj/compat/http.c++:7124), which the Socket owns via IoOwn. Holding the pin as a local in
  // connectImpl()'s coroutine body ties it to the earlier event instead, because coroutine locals
  // are destroyed at co_return.
  //
  // The promise is held in a variable and never awaited, which is what the real system does: the
  // stream owns it as a plain attachment and the coroutine inside progresses on the event loop.
  // (fork() does NOT model this -- ForkHub drops the inner node once it settles, keeping only the
  // result, so attachments are released at settle time and the distinction disappears.)
  RecordingTrackerHooks hooks;
  auto tracker = kj::refcounted<RequestTracker>(hooks);
  auto fixture = makeActorFixture(*tracker);
  AutogateScope autogateScope(TunnelReuseGate::ENABLED, ActorPinGate::ENABLED);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto paf = kj::newPromiseAndFulfiller<void>();
    auto container = env.js.alloc<Container>(
        rpc::Container::Client(kj::heap<TestContainerServer>(
            byteStreamFactory, ResponseMode::TRUNCATED_DISCONNECT, connectCount, *paf.fulfiller)),
        true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();

    auto pipe = kj::newTwoWayPipe();
    TestConnectResponse response;
    kj::HttpHeaders headers(headerTable);
    kj::HttpConnectSettings settings{.useTls = false};

    // Never awaited, never cancelled: this stands in for the Socket holding the promise.
    auto connectPromise =
        client->connect("container"_kjc, headers, *pipe.ends[0], response, settings);
    KJ_EXPECT(tracker->isActive(), "an open tunnel must pin the actor");

    // Drive the request and wait for the container to report that it has disconnected. Awaiting
    // our own I/O is what lets the event loop turn, which is what lets connectImpl() make progress.
    // The response has to be drained concurrently: the pipe is unbuffered, so otherwise the
    // container blocks writing its reply and never gets as far as disconnecting.
    kj::NullStream discard;
    auto drainResponse = pipe.ends[1]->pumpTo(discard).then(
        [](uint64_t) -> kj::Promise<void> { return kj::NEVER_DONE; });
    co_await pipe.ends[1]->write("GET / HTTP/1.1\r\n\r\n"_kjb);
    co_await paf.promise.exclusiveJoin(kj::mv(drainResponse));

    // Let the disconnect propagate into connectImpl(). Bounded by event-loop turns rather than
    // wall-clock time, so this is deterministic: with the pin held as a coroutine local it is
    // released within a turn or two of the pumps failing; if it were attached to the promise node
    // instead, `connectPromise` still owns that node here and it would never be released.
    for (uint i = 0; i < 100 && tracker->isActive(); ++i) {
      co_await kj::evalLater([]() {});
    }

    KJ_EXPECT(!tracker->isActive(),
        "the pin must be released when the tunnel closes, not when the socket is dropped");

    // Only now drop the promise, i.e. the socket.
    connectPromise = nullptr;
  });

  KJ_EXPECT(connectCount == 1);
  KJ_EXPECT(hooks.activeCount == 1);
  KJ_EXPECT(hooks.inactiveCount == 1);
}

KJ_TEST("Container connect() unpins after both sides close cleanly") {
  RecordingTrackerHooks hooks;
  auto tracker = kj::refcounted<RequestTracker>(hooks);
  auto fixture = makeActorFixture(*tracker);
  AutogateScope autogateScope(TunnelReuseGate::ENABLED, ActorPinGate::ENABLED);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = env.js.alloc<Container>(
        rpc::Container::Client(kj::heap<TestContainerServer>(
            byteStreamFactory, ResponseMode::CLOSE_AFTER_RESPONSE, connectCount)),
        true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();

    auto pipe = kj::newTwoWayPipe();
    TestConnectResponse response;
    kj::HttpHeaders headers(headerTable);
    kj::HttpConnectSettings settings{.useTls = false};

    // Keep the promise alive after both halves close, just as a retained JS Socket keeps the
    // underlying connection alive after reaching EOF.
    auto connectPromise =
        client->connect("container"_kjc, headers, *pipe.ends[0], response, settings);
    KJ_EXPECT(tracker->isActive());

    co_await pipe.ends[1]->write("GET / HTTP/1.1\r\n\r\n"_kjb);
    pipe.ends[1]->shutdownWrite();

    kj::NullStream discard;
    co_await pipe.ends[1]->pumpTo(discard);

    for (uint i = 0; i < 100 && tracker->isActive(); ++i) {
      co_await kj::evalLater([]() {});
    }

    KJ_EXPECT(!tracker->isActive(),
        "both clean half-closes must release the pin before the socket is dropped");
    connectPromise = nullptr;
  });

  KJ_EXPECT(connectCount == 1);
  KJ_EXPECT(hooks.activeCount == 1);
  KJ_EXPECT(hooks.inactiveCount == 1);
}

KJ_TEST("Container connect() does not pin when gated off, container closes") {
  runConnectPinTest(ActorPinGate::DISABLED, TunnelCloser::CONTAINER);
}

KJ_TEST("Container connect() does not pin when gated off, client closes") {
  runConnectPinTest(ActorPinGate::DISABLED, TunnelCloser::CLIENT);
}

// Every failure path must release the pin too. The mechanism is that connectImpl()/request() hold
// the pin in a coroutine local, so it is destroyed on throw as well as on normal return -- but that
// is exactly the kind of claim that deserves a test per path rather than one argument.
void runFetchFailurePinTest(ResponseMode mode) {
  RecordingTrackerHooks hooks;
  auto tracker = kj::refcounted<RequestTracker>(hooks);
  auto fixture = makeActorFixture(*tracker);
  AutogateScope autogateScope(TunnelReuseGate::ENABLED, ActorPinGate::ENABLED);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = env.js.alloc<Container>(rpc::Container::Client(kj::heap<TestContainerServer>(
                                                 byteStreamFactory, mode, connectCount)),
        true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();

    bool threw = false;
    try {
      co_await makeTestRequest(*client, headerTable);
    } catch (...) {
      threw = true;
    }
    KJ_EXPECT(threw, "expected the fetch to fail");

    for (uint i = 0; i < 100 && tracker->isActive(); ++i) {
      co_await kj::evalLater([]() {});
    }
    KJ_EXPECT(!tracker->isActive(), "a failed fetch must not leak the pin");
  });

  KJ_EXPECT(hooks.inactiveCount == hooks.activeCount, "every pin taken must have been released");
}

KJ_TEST("Container fetch does not leak the pin when the tunnel fails to open") {
  runFetchFailurePinTest(ResponseMode::CONNECT_FAILURE);
}

KJ_TEST("Container fetch does not leak the pin when the upstream write fails") {
  runFetchFailurePinTest(ResponseMode::UPSTREAM_FAILURE);
}

KJ_TEST("Container fetch does not leak the pin on a malformed response") {
  runFetchFailurePinTest(ResponseMode::MALFORMED_KEEP_OPEN);
}

KJ_TEST("Container connect() does not leak the pin when the upstream half-close fails") {
  RecordingTrackerHooks hooks;
  auto tracker = kj::refcounted<RequestTracker>(hooks);
  auto fixture = makeActorFixture(*tracker);
  AutogateScope autogateScope(TunnelReuseGate::ENABLED, ActorPinGate::ENABLED);
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
    KJ_EXPECT(tracker->isActive());

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
    KJ_EXPECT(threw, "expected the upstream half-close to fail connect()");
    KJ_EXPECT(!tracker->isActive(), "a failed half-close must not leak the pin");
  });

  KJ_EXPECT(connectCount == 1);
  KJ_EXPECT(hooks.activeCount == 1);
  KJ_EXPECT(hooks.inactiveCount == 1);
}

KJ_TEST("Container does not leak the pin when a WebSocket handshake fails") {
  // The container answers a plain 200 instead of upgrading, so openWebSocket() yields a body rather
  // than a WebSocket. The exchange still has to release the pin.
  RecordingTrackerHooks hooks;
  auto tracker = kj::refcounted<RequestTracker>(hooks);
  auto fixture = makeActorFixture(*tracker);
  AutogateScope autogateScope(TunnelReuseGate::ENABLED, ActorPinGate::ENABLED);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        env.js.alloc<Container>(rpc::Container::Client(kj::heap<TestContainerServer>(
                                    byteStreamFactory, ResponseMode::KEEP_ALIVE, connectCount)),
            true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();

    auto httpClient = kj::newHttpClient(*client);
    kj::HttpHeaders headers(headerTable);
    auto response = co_await httpClient->openWebSocket("http://container/"_kj, headers);
    KJ_EXPECT(response.statusCode == 200, "expected the upgrade to be refused");
    KJ_EXPECT(response.webSocketOrBody.is<kj::Own<kj::AsyncInputStream>>());

    // Drain the body so the exchange finishes.
    auto& body =
        KJ_ASSERT_NONNULL(response.webSocketOrBody.tryGet<kj::Own<kj::AsyncInputStream>>());
    kj::NullStream discard;
    co_await body->pumpTo(discard);

    for (uint i = 0; i < 100 && tracker->isActive(); ++i) {
      co_await kj::evalLater([]() {});
    }
    KJ_EXPECT(!tracker->isActive(), "a refused upgrade must not leak the pin");
  });

  KJ_EXPECT(connectCount == 1);
  KJ_EXPECT(hooks.activeCount == 1);
  KJ_EXPECT(hooks.inactiveCount == 1);
}

KJ_TEST("Container pins the actor once for concurrent connections on one port") {
  // newSingleUseClient() runs per operation (api/http.c++:2532), so each connection carries its own
  // pin and activeRequests counts them. RequestTracker only fires its hooks on the 0->1 and 1->0
  // edges, so N concurrent connections must produce exactly one active()/inactive() pair, and the
  // actor must stay pinned until the *last* connection closes -- not the first.
  RecordingTrackerHooks hooks;
  auto tracker = kj::refcounted<RequestTracker>(hooks);
  auto fixture = makeActorFixture(*tracker);
  AutogateScope autogateScope(TunnelReuseGate::ENABLED, ActorPinGate::ENABLED);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        env.js.alloc<Container>(rpc::Container::Client(kj::heap<TestContainerServer>(
                                    byteStreamFactory, ResponseMode::KEEP_ALIVE, connectCount)),
            true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto& headerTable = env.context.getHeaderTable();
    kj::HttpHeaders headers(headerTable);
    kj::HttpConnectSettings settings{.useTls = false};

    // Two separate getClient() calls, i.e. two separate WorkerInterfaces, i.e. two separate pins.
    auto client1 = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto client2 = fetcher->getClient(env.context, kj::none, "container"_kjc);

    auto pipe1 = kj::newTwoWayPipe();
    auto pipe2 = kj::newTwoWayPipe();
    TestConnectResponse response1;
    TestConnectResponse response2;

    auto connect1 = client1->connect("container"_kjc, headers, *pipe1.ends[0], response1, settings);
    auto connect2 = client2->connect("container"_kjc, headers, *pipe2.ends[0], response2, settings);
    KJ_EXPECT(tracker->isActive());

    // Complete a round trip on each so both tunnels are genuinely open.
    char buffer[64];
    co_await pipe1.ends[1]->write("GET / HTTP/1.1\r\n\r\n"_kjb);
    KJ_EXPECT(co_await pipe1.ends[1]->tryRead(buffer, 1, kj::size(buffer)) > 0);
    co_await pipe2.ends[1]->write("GET / HTTP/1.1\r\n\r\n"_kjb);
    KJ_EXPECT(co_await pipe2.ends[1]->tryRead(buffer, 1, kj::size(buffer)) > 0);

    KJ_EXPECT(connectCount == 2, "each raw connect() opens its own tunnel");
    KJ_EXPECT(hooks.activeCount == 1, "two connections, but only one 0->1 edge");

    // Close the first. Note we deliberately keep client1 alive: the pin was moved out of it into
    // connectImpl()'s frame, so dropping the promise alone must be enough to release one count.
    connect1 = nullptr;
    KJ_EXPECT(tracker->isActive(), "the second connection must still hold the actor");
    KJ_EXPECT(hooks.inactiveCount == 0, "releasing one of two must not fire inactive()");

    connect2 = nullptr;
    KJ_EXPECT(!tracker->isActive(), "the last connection closing must release the actor");
  });

  KJ_EXPECT(hooks.activeCount == 1);
  KJ_EXPECT(hooks.inactiveCount == 1);
}

// How a WebSocket session ends.
enum class WebSocketEnding {
  CLEAN_CLOSE,    // close handshake completes
  REMOTE_ABRUPT,  // container drops the socket with no close frame
  CLIENT_ABANDON  // we drop our end without closing
};

void runWebSocketPinTest(ActorPinGate pinGate, WebSocketEnding ending) {
  RecordingTrackerHooks hooks;
  auto tracker = kj::refcounted<RequestTracker>(hooks);
  auto fixture = makeActorFixture(*tracker);
  AutogateScope autogateScope(TunnelReuseGate::ENABLED, pinGate);
  capnp::ByteStreamFactory byteStreamFactory;
  size_t connectCount = 0;

  auto mode = ending == WebSocketEnding::REMOTE_ABRUPT ? ResponseMode::WEBSOCKET_ABRUPT_CLOSE
                                                       : ResponseMode::WEBSOCKET_ECHO;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = env.js.alloc<Container>(rpc::Container::Client(kj::heap<TestContainerServer>(
                                                 byteStreamFactory, mode, connectCount)),
        true);
    auto fetcher = container->getTcpPort(env.js, 8080);
    auto client = fetcher->getClient(env.context, kj::none, "container"_kjc);
    auto& headerTable = env.context.getHeaderTable();

    // Drive the handshake through kj's HttpClient-over-HttpService adapter rather than a hand-built
    // Response: the upgrade takes a round trip, so the accepted WebSocket only exists after a
    // co_await, and the adapter is what knows how to wait for it. It also owns the outstanding
    // request(), which is what holds the pin.
    auto httpClient = kj::newHttpClient(*client);

    kj::HttpHeaders headers(headerTable);
    auto response = co_await httpClient->openWebSocket("http://container/"_kj, headers);
    KJ_EXPECT(response.statusCode == 101);
    auto ws = kj::mv(KJ_ASSERT_NONNULL(
        response.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>(), "expected a WebSocket upgrade"));

    // Prove the session really works before asserting anything about the pin, so that a broken
    // handshake is reported as such rather than as a spurious pinning failure.
    co_await ws->send("ping"_kj);
    auto echoed = co_await ws->receive();
    KJ_EXPECT(KJ_ASSERT_NONNULL(echoed.tryGet<kj::String>()) == "ping"_kj);

    // The HTTP exchange is long over but the session is open: this is the window in which
    // hibernation used to kill the tunnel.
    KJ_EXPECT(tracker->isActive() == pinned(pinGate));

    switch (ending) {
      case WebSocketEnding::CLEAN_CLOSE: {
        co_await ws->close(1000, "done"_kj);
        auto closeMessage = co_await ws->receive();
        KJ_EXPECT(closeMessage.is<kj::WebSocket::Close>());
        break;
      }
      case WebSocketEnding::REMOTE_ABRUPT: {
        // The container has already dropped its end. Reading surfaces the disconnect.
        try {
          co_await ws->receive();
        } catch (...) {
          // Expected: the session ended without a close frame.
        }
        break;
      }
      case WebSocketEnding::CLIENT_ABANDON:
        ws = nullptr;
        httpClient = nullptr;
        break;
    }

    // Deliberately still holding `ws` and `httpClient` in the first two cases. Releasing the pin
    // must follow from the *session* ending, not from us dropping the objects that own it -- the
    // same distinction as tunnel-closed versus socket-dropped on the connect() path. Bounded by
    // event-loop turns so the teardown can propagate.
    for (uint i = 0; i < 100 && tracker->isActive(); ++i) {
      co_await kj::evalLater([]() {});
    }
    KJ_EXPECT(!tracker->isActive(), "the pin must be released when the session ends");
  });

  KJ_EXPECT(connectCount == 1);
  KJ_EXPECT(hooks.activeCount == (pinned(pinGate) ? 1 : 0));
  KJ_EXPECT(hooks.inactiveCount == (pinned(pinGate) ? 1 : 0));
}

KJ_TEST("Container WebSocket unpins the actor after a clean close") {
  runWebSocketPinTest(ActorPinGate::ENABLED, WebSocketEnding::CLEAN_CLOSE);
}

KJ_TEST("Container WebSocket unpins the actor when the remote drops abruptly") {
  runWebSocketPinTest(ActorPinGate::ENABLED, WebSocketEnding::REMOTE_ABRUPT);
}

KJ_TEST("Container WebSocket unpins the actor when the client abandons it") {
  runWebSocketPinTest(ActorPinGate::ENABLED, WebSocketEnding::CLIENT_ABANDON);
}

KJ_TEST("Container WebSocket does not pin when gated off") {
  runWebSocketPinTest(ActorPinGate::DISABLED, WebSocketEnding::CLEAN_CLOSE);
}

}  // namespace
}  // namespace workerd::api
