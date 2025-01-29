// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container.h"

#include <workerd/api/http.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

// =======================================================================================
// Basic lifecycle methods

Container::Container(rpc::Container::Client rpcClient)
    : rpcClient(IoContext::current().addObject(kj::heap(kj::mv(rpcClient)))) {}

void Container::start(jsg::Lock& js, jsg::Optional<StartupOptions> maybeOptions) {
  JSG_REQUIRE(!running, Error, "start() cannot be called on a container that is already running.");

  StartupOptions options = kj::mv(maybeOptions).orDefault({});

  auto req = rpcClient->startRequest();
  KJ_IF_SOME(entrypoint, options.entrypoint) {
    auto list = req.initEntrypoint(entrypoint.size());
    for (auto i: kj::indices(entrypoint)) {
      list.set(i, entrypoint[i]);
    }
  }
  req.setEnableInternet(options.enableInternet);

  IoContext::current().addTask(req.send().ignoreResult());

  running = true;
}

jsg::Promise<void> Container::monitor(jsg::Lock& js) {
  JSG_REQUIRE(running, Error, "monitor() cannot be called on a container that is not running.");

  return IoContext::current()
      .awaitIo(js, rpcClient->monitorRequest(capnp::MessageSize{4, 0}).send().ignoreResult())
      .then(js, [this](jsg::Lock& js) {
    running = false;
    KJ_IF_SOME(d, destroyReason) {
      jsg::Value error = kj::mv(d);
      destroyReason = kj::none;
      js.throwException(kj::mv(error));
    }
  }, [this](jsg::Lock& js, jsg::Value&& error) {
    running = false;
    destroyReason = kj::none;
    js.throwException(kj::mv(error));
  });
}

jsg::Promise<void> Container::destroy(jsg::Lock& js, jsg::Optional<jsg::Value> error) {
  if (!running) return js.resolvedPromise();

  if (destroyReason == kj::none) {
    destroyReason = kj::mv(error);
  }

  return IoContext::current().awaitIo(
      js, rpcClient->destroyRequest(capnp::MessageSize{4, 0}).send().ignoreResult());
}

void Container::signal(jsg::Lock& js, int signo) {
  JSG_REQUIRE(signo > 0 && signo <= 64, RangeError, "Invalid signal number.");
  JSG_REQUIRE(running, Error, "signal() cannot be called on a container that is not running.");

  auto req = rpcClient->signalRequest(capnp::MessageSize{4, 0});
  req.setSigno(signo);
  IoContext::current().addTask(req.send().ignoreResult());
}

// =======================================================================================
// getTcpPort()

// `getTcpPort()` returns a `Fetcher`, on which `fetch()` and `connect()` can be called. `Fetcher`
// is a JavaScript wrapper around `WorkerInterface`, so we need to implement that.
class Container::TcpPortWorkerInterface final: public WorkerInterface {
 public:
  TcpPortWorkerInterface(capnp::ByteStreamFactory& byteStreamFactory,
      const kj::HttpHeaderTable& headerTable,
      rpc::Container::Port::Client port)
      : byteStreamFactory(byteStreamFactory),
        headerTable(headerTable),
        port(kj::mv(port)) {}

  // Implements fetch(), i.e., HTTP requests. We form a TCP connection, then run HTTP over it
  // (as opposed to, say, speaking http-over-capnp to the container service).
  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    // URLs should have been validated earlier in the stack, so parsing the URL should succeed.
    auto parsedUrl = KJ_REQUIRE_NONNULL(kj::Url::tryParse(url, kj::Url::Context::HTTP_PROXY_REQUEST,
                                            {.percentDecode = false, .allowEmpty = true}),
        "invalid url?", url);

    // We don't support TLS.
    JSG_REQUIRE(parsedUrl.scheme != "https", Error,
        "Connencting to a container using HTTPS is not currently supported; use HTTP instead. "
        "TLS is unnecessary anyway, as the connection is already secure by default.");

    // Schemes other than http: and https: should have been rejected earlier, but let's verify.
    KJ_REQUIRE(parsedUrl.scheme == "http");

    // We need to convert the URL from proxy format (full URL in request line) to host format
    // (path in request line, hostname in Host header).
    auto newHeaders = headers.cloneShallow();
    newHeaders.set(kj::HttpHeaderId::HOST, parsedUrl.host);
    auto noHostUrl = parsedUrl.toString(kj::Url::Context::HTTP_REQUEST);

    // Make a TCP connection...
    auto pipe = kj::newTwoWayPipe();
    auto connectionPromise =
        connectImpl(*pipe.ends[1]).then([]() -> kj::Promise<void> { return kj::NEVER_DONE; });

    // ... and then stack an HttpClient on it ...
    auto client = kj::newHttpClient(headerTable, *pipe.ends[0]);

    // ... and then adapt that to an HttpService ...
    auto service = kj::newHttpService(*client);

    // ... and now we can just forward our call to that.
    co_await connectionPromise.exclusiveJoin(
        service->request(method, noHostUrl, newHeaders, requestBody, response));
  }

  // Implements connect(), i.e., forms a raw socket.
  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    JSG_REQUIRE(!settings.useTls, Error,
        "Connencting to a container using TLS is not currently supported. It is unnecessary "
        "anyway, as the connection is already secure by default.");

    auto promise = connectImpl(connection);

    kj::HttpHeaders responseHeaders(headerTable);
    response.accept(200, "OK", responseHeaders);

    return promise;
  }

  // The only `CustomEvent` that can happen through `Fetcher` is a JSRPC call. Maybe we will
  // support this someday? But not today.
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }

  // There's no way to invoke the remaining event types via `Fetcher`.
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
  capnp::ByteStreamFactory& byteStreamFactory;
  const kj::HttpHeaderTable& headerTable;
  rpc::Container::Port::Client port;

  // Connect to the port and pump bytes to/from `connection`. Used by both request() and
  // connect().
  kj::Promise<void> connectImpl(kj::AsyncIoStream& connection) {
    // A lot of the following is copied from
    // capnp::HttpOverCapnpFactory::KjToCapnpHttpServiceAdapter::connect().
    auto req = port.connectRequest(capnp::MessageSize{4, 1});
    auto downPipe = kj::newOneWayPipe();
    req.setDown(byteStreamFactory.kjToCapnp(kj::mv(downPipe.out)));
    auto pipeline = req.send();

    // Make sure the request message isn't pinned into memory through the co_await below.
    { auto drop = kj::mv(req); }

    auto downPumpTask =
        downPipe.in->pumpTo(connection)
            .then([&connection, down = kj::mv(downPipe.in)](uint64_t) -> kj::Promise<void> {
      connection.shutdownWrite();
      return kj::NEVER_DONE;
    });
    auto up = pipeline.getUp();

    auto upStream = byteStreamFactory.capnpToKjExplicitEnd(up);
    auto upPumpTask = connection.pumpTo(*upStream)
                          .then([&upStream = *upStream](uint64_t) mutable {
      return upStream.end();
    }).then([up = kj::mv(up), upStream = kj::mv(upStream)]() mutable -> kj::Promise<void> {
      return kj::NEVER_DONE;
    });

    co_await pipeline.ignoreResult();
    co_await kj::joinPromisesFailFast(kj::arr(kj::mv(upPumpTask), kj::mv(downPumpTask)));
  }
};

// `Fetcher` actually wants us to give it a factory that creates a new `WorkerInterface` for each
// request, so this is that.
class Container::TcpPortOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  TcpPortOutgoingFactory(capnp::ByteStreamFactory& byteStreamFactory,
      const kj::HttpHeaderTable& headerTable,
      rpc::Container::Port::Client port)
      : byteStreamFactory(byteStreamFactory),
        headerTable(headerTable),
        port(kj::mv(port)) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String> cfStr) override {
    // At present we have no use for `cfStr`.
    return kj::heap<TcpPortWorkerInterface>(byteStreamFactory, headerTable, port);
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  const kj::HttpHeaderTable& headerTable;
  rpc::Container::Port::Client port;
};

jsg::Ref<Fetcher> Container::getTcpPort(jsg::Lock& js, int port) {
  JSG_REQUIRE(port > 0 && port < 65536, TypeError, "Invalid port number: ", port);

  auto req = rpcClient->getTcpPortRequest(capnp::MessageSize{4, 0});
  req.setPort(port);

  auto& ioctx = IoContext::current();

  kj::Own<Fetcher::OutgoingFactory> factory = kj::heap<TcpPortOutgoingFactory>(
      ioctx.getByteStreamFactory(), ioctx.getHeaderTable(), req.send().getPort());

  return jsg::alloc<Fetcher>(
      ioctx.addObject(kj::mv(factory)), Fetcher::RequiresHostAndProtocol::YES, true);
}

}  // namespace workerd::api
