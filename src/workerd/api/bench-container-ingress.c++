// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Compares managed container ingress with and without tunnel reuse against the same HTTP path
// without the Cap'n Proto tunnel. All variants use a real in-memory HTTP connection and issue
// requests sequentially, making this a per-request latency benchmark rather than a concurrency
// benchmark. In-process Cap'n Proto path shortening may understate cross-process tunnel costs.
//
// Run with: bazel run @workerd//src/workerd/api:bench-container-ingress

#include "container.h"

#include <workerd/api/http.h>
#include <workerd/io/container.capnp.h>
#include <workerd/io/io-context.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

#include <capnp/compat/byte-stream.h>
#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/timer.h>

namespace workerd::api {
namespace {

constexpr size_t BODY_SIZE = 64;

constexpr size_t REQUESTS_PER_ITER = 256;

enum class TunnelReuse { DISABLED, ENABLED };

class AutogateScope {
 public:
  AutogateScope(TunnelReuse tunnelReuse) {
    if (tunnelReuse == TunnelReuse::ENABLED) {
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

class BackendHttpService final: public kj::HttpService {
 public:
  BackendHttpService(const kj::HttpHeaderTable& headerTable, kj::ArrayPtr<const kj::byte> body)
      : headerTable(headerTable),
        body(body) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    co_await requestBody.readAllBytes(BODY_SIZE + 1);
    kj::HttpHeaders responseHeaders(headerTable);
    auto out = response.send(200, "OK", responseHeaders, body.size());
    co_await out->write(body);
  }

 private:
  const kj::HttpHeaderTable& headerTable;
  kj::ArrayPtr<const kj::byte> body;
};

class CountingSink final: public kj::AsyncOutputStream {
 public:
  explicit CountingSink(size_t& counter): counter(counter) {}
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    counter += buffer.size();
    return kj::READY_NOW;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto piece: pieces) counter += piece.size();
    return kj::READY_NOW;
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

 private:
  size_t& counter;
};

class BenchResponse final: public kj::HttpService::Response {
 public:
  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    this->statusCode = statusCode;
    return kj::heap<CountingSink>(bytesReceived);
  }
  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    KJ_UNIMPLEMENTED("no websockets in this benchmark");
  }
  uint statusCode = 0;
  size_t bytesReceived = 0;
};

class MockPort final: public rpc::Container::Port::Server, private kj::TaskSet::ErrorHandler {
 public:
  MockPort(kj::Timer& timer,
      const kj::HttpHeaderTable& headerTable,
      kj::HttpService& backend,
      capnp::ByteStreamFactory& byteStreamFactory)
      : byteStreamFactory(byteStreamFactory),
        server(timer, headerTable, backend),
        tasks(*this) {}

  kj::Promise<void> connect(ConnectContext context) override {
    auto params = context.getParams();

    auto downOut = byteStreamFactory.capnpToKj(params.getDown());

    auto upPipe = kj::newOneWayPipe();
    context.getResults().setUp(byteStreamFactory.kjToCapnp(kj::mv(upPipe.out)));

    auto containerConn = kj::heap<HalfDuplexJoin>(kj::mv(upPipe.in), kj::mv(downOut));

    tasks.add(server.listenHttp(kj::mv(containerConn)));
    return kj::READY_NOW;
  }

 private:
  class HalfDuplexJoin final: public kj::AsyncIoStream {
   public:
    HalfDuplexJoin(kj::Own<kj::AsyncInputStream> readSide, kj::Own<kj::AsyncOutputStream> writeSide)
        : readSide(kj::mv(readSide)),
          writeSide(kj::mv(writeSide)) {}
    kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
      return readSide->tryRead(buffer, minBytes, maxBytes);
    }
    kj::Maybe<uint64_t> tryGetLength() override {
      return readSide->tryGetLength();
    }
    kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
      return writeSide->write(buffer);
    }
    kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
      return writeSide->write(pieces);
    }
    kj::Promise<void> whenWriteDisconnected() override {
      return writeSide->whenWriteDisconnected();
    }
    void shutdownWrite() override {
      writeSide = nullptr;
    }

   private:
    kj::Own<kj::AsyncInputStream> readSide;
    kj::Own<kj::AsyncOutputStream> writeSide;
  };

  capnp::ByteStreamFactory& byteStreamFactory;
  kj::HttpServer server;
  kj::TaskSet tasks;
  void taskFailed(kj::Exception&&) override {}
};

class MockContainerServer final: public rpc::Container::Server {
 public:
  MockContainerServer(kj::Timer& timer,
      const kj::HttpHeaderTable& headerTable,
      kj::HttpService& backend,
      capnp::ByteStreamFactory& byteStreamFactory)
      : timer(timer),
        headerTable(headerTable),
        backend(backend),
        byteStreamFactory(byteStreamFactory) {}

  kj::Promise<void> getTcpPort(GetTcpPortContext context) override {
    context.getResults().setPort(
        kj::heap<MockPort>(timer, headerTable, backend, byteStreamFactory));
    return kj::READY_NOW;
  }

 private:
  kj::Timer& timer;
  const kj::HttpHeaderTable& headerTable;
  kj::HttpService& backend;
  capnp::ByteStreamFactory& byteStreamFactory;
};

class DirectWorkerInterface final: public WorkerInterface {
 public:
  explicit DirectWorkerInterface(kj::HttpClient& client): client(client) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    auto service = kj::newHttpService(client);
    co_await service->request(method, url, headers, requestBody, response);
  }
  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    KJ_UNIMPLEMENTED("connect not used");
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_UNREACHABLE;
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_UNREACHABLE;
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_UNREACHABLE;
  }

 private:
  kj::HttpClient& client;
};

class DirectOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  explicit DirectOutgoingFactory(kj::HttpClient& client): client(client) {}
  Result newSingleUseClient(
      kj::Maybe<kj::String> cfStr, MakeUserSpanParent makeUserSpanParent) override {
    auto result = IoContext::current().getSubrequestNoChecks(
        [this, &makeUserSpanParent](auto& tracing, auto& channelFactory) {
      makeUserSpanParent(tracing);
      return kj::heap<DirectWorkerInterface>(client);
    }, {.inHouse = false, .wrapMetrics = false});
    return {.client = kj::mv(result), .spanParents = kj::none};
  }

 private:
  kj::HttpClient& client;
};

kj::Promise<void> oneManagedRequest(
    WorkerInterface& client, const kj::HttpHeaderTable& headerTable) {
  kj::HttpHeaders headers(headerTable);
  BenchResponse response;
  kj::NullStream emptyBody;
  co_await client.request(
      kj::HttpMethod::GET, "http://container/"_kj, headers, emptyBody, response);
  KJ_ASSERT(response.statusCode == 200);
  KJ_ASSERT(response.bytesReceived == BODY_SIZE);
}

kj::Promise<void> oneDirectRequest(kj::HttpClient& client, const kj::HttpHeaderTable& headerTable) {
  kj::HttpHeaders headers(headerTable);
  auto req = client.request(kj::HttpMethod::GET, "/"_kj, headers);
  req.body = nullptr;
  auto response = co_await req.response;
  KJ_ASSERT(response.statusCode == 200);
  size_t bytesReceived = 0;
  CountingSink sink(bytesReceived);
  co_await response.body->pumpTo(sink);
  KJ_ASSERT(bytesReceived == BODY_SIZE);
}

TestFixture::SetupParams setupParams() {
  return TestFixture::SetupParams{.useRealTimers = false};
}

// TODO(perf): This benchmark uses fake timers and in-process kj::newTwoWayPipe transports, so the
//   simulated tunnel/network latency is zero. That means the Managed vs. ManagedReuse comparison
//   measures only CPU and allocation cost, not the per-connection round-trip that tunnel pooling
//   is meant to amortize. To make the comparison representative, inject a configurable synthetic
//   delay into the tunnel transport (e.g. a stream wrapper that defers reads/writes on the shared
//   fake timer) so the establishment RTT shows up in the numbers.

kj::TimerImpl& sharedFakeTimer() {
  static kj::TimerImpl inst{kj::origin<kj::TimePoint>()};
  return inst;
}

kj::Own<kj::HttpClient> makePooledHttp(
    kj::Timer& timer, const kj::HttpHeaderTable& headerTable, kj::ArrayPtr<const kj::byte> body) {
  auto pipe = kj::newTwoWayPipe();
  auto backend = kj::heap<BackendHttpService>(headerTable, body);
  auto server = kj::heap<kj::HttpServer>(timer, headerTable, *backend);
  auto serverTask = server->listenHttp(kj::mv(pipe.ends[1]));
  auto clientEnd = kj::mv(pipe.ends[0]);
  auto client = kj::newHttpClient(headerTable, *clientEnd);
  return client.attach(kj::mv(clientEnd), kj::mv(serverTask), kj::mv(server), kj::mv(backend));
}

template <typename MakeClient>
void runWorkerInterfaceBench(benchmark::State& state,
    TestFixture& fixture,
    IoContext::IncomingRequest& request,
    const kj::HttpHeaderTable& headerTable,
    MakeClient&& makeClient) {
  auto runBatch = [&]() {
    kj::Array<kj::Own<WorkerInterface>> clients;
    fixture.enterContext(request, [&](const TestFixture::Environment& env) {
      auto builder = kj::heapArrayBuilder<kj::Own<WorkerInterface>>(REQUESTS_PER_ITER);
      for (size_t i = 0; i < REQUESTS_PER_ITER; i++) {
        builder.add(makeClient(env));
      }
      clients = builder.finish();
    });
    for (auto& client: clients) {
      oneManagedRequest(*client, headerTable).wait(fixture.getWaitScope());
    }
  };

  runBatch();
  for (auto _: state) {
    runBatch();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(REQUESTS_PER_ITER));
}

void runManagedBench(
    benchmark::State& state, TestFixture::SetupParams params, TunnelReuse tunnelReuse) {
  TestFixture fixture(kj::mv(params));
  AutogateScope autogateScope(tunnelReuse);

  capnp::ByteStreamFactory serverByteStreamFactory;
  auto body = kj::heapArray<kj::byte>(BODY_SIZE);
  body.asPtr().fill('x');

  auto ioContext = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*ioContext);

  auto backend = kj::heap<BackendHttpService>(ioContext->getHeaderTable(), body.asPtr());

  jsg::Ref<Container> container = nullptr;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    container = env.js.alloc<Container>(
        rpc::Container::Client(kj::heap<MockContainerServer>(
            sharedFakeTimer(), env.context.getHeaderTable(), *backend, serverByteStreamFactory)),
        true);
  });

  auto& headerTable = ioContext->getHeaderTable();
  runWorkerInterfaceBench(
      state, fixture, *request, headerTable, [&](const TestFixture::Environment& env) {
    auto fetcher = container->getTcpPort(env.js, 8080);
    return fetcher->getClient(env.context, kj::none, "container"_kjc);
  });
}

void Managed(benchmark::State& state) {
  runManagedBench(state, setupParams(), TunnelReuse::DISABLED);
}
void ManagedReuse(benchmark::State& state) {
  runManagedBench(state, setupParams(), TunnelReuse::ENABLED);
}

void FetchDirect(benchmark::State& state) {
  TestFixture fixture(setupParams());

  auto body = kj::heapArray<kj::byte>(BODY_SIZE);
  body.asPtr().fill('x');

  auto ioContext = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*ioContext);

  auto client = makePooledHttp(sharedFakeTimer(), ioContext->getHeaderTable(), body.asPtr());
  jsg::Ref<Fetcher> fetcher = nullptr;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    kj::Own<Fetcher::OutgoingFactory> factory = kj::heap<DirectOutgoingFactory>(*client);
    fetcher = env.js.alloc<Fetcher>(
        env.context.addObject(kj::mv(factory)), Fetcher::RequiresHostAndProtocol::YES, true);
  });

  auto& headerTable = ioContext->getHeaderTable();
  runWorkerInterfaceBench(
      state, fixture, *request, headerTable, [&](const TestFixture::Environment& env) {
    return fetcher->getClient(env.context, kj::none, "container"_kjc);
  });
}

void Direct(benchmark::State& state) {
  TestFixture fixture(setupParams());

  auto body = kj::heapArray<kj::byte>(BODY_SIZE);
  body.asPtr().fill('x');

  auto ioContext = fixture.newIoContext();
  auto client = makePooledHttp(sharedFakeTimer(), ioContext->getHeaderTable(), body.asPtr());
  auto& headerTable = ioContext->getHeaderTable();
  auto& waitScope = fixture.getWaitScope();

  auto runBatch = [&]() {
    for (size_t i = 0; i < REQUESTS_PER_ITER; i++) {
      oneDirectRequest(*client, headerTable).wait(waitScope);
    }
  };

  runBatch();
  for (auto _: state) {
    runBatch();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(REQUESTS_PER_ITER));
}

WD_BENCHMARK(Managed);
WD_BENCHMARK(ManagedReuse);
WD_BENCHMARK(FetchDirect);
WD_BENCHMARK(Direct);

}  // namespace
}  // namespace workerd::api
