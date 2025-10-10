// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container.h"

#include <workerd/api/http.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

// =======================================================================================
// Basic lifecycle methods

Container::Container(rpc::Container::Client rpcClient, bool running)
    : rpcClient(IoContext::current().addObject(kj::heap(kj::mv(rpcClient)))),
      running(running) {}

void Container::start(jsg::Lock& js, jsg::Optional<StartupOptions> maybeOptions) {
  CompatibilityFlags::Reader flags = js.getCompatibilityFlags();
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

  KJ_IF_SOME(env, options.env) {
    auto list = req.initEnvironmentVariables(env.fields.size());
    for (auto i: kj::indices(env.fields)) {
      auto field = &env.fields[i];
      JSG_REQUIRE(field->name.findFirst('=') == kj::none, Error,
          "Environment variable names cannot contain '=': ", field->name);

      JSG_REQUIRE(field->name.findFirst('\0') == kj::none, Error,
          "Environment variable names cannot contain '\\0': ", field->name);

      JSG_REQUIRE(field->value.findFirst('\0') == kj::none, Error,
          "Environment variable values cannot contain '\\0': ", field->name);

      list.set(i, str(field->name, "=", field->value));
    }
  }

  IoContext::current().addTask(req.sendIgnoringResult());

  running = true;

  if (flags.getWorkerdExperimental()) {
    KJ_IF_SOME(hardTimeout, options.hardTimeout) {
      uint32_t timeoutMs = 0;
      KJ_SWITCH_ONEOF(hardTimeout) {
        KJ_CASE_ONEOF(ms, int64_t) {
          JSG_REQUIRE(ms > 0 && ms <= UINT32_MAX, TypeError, "Hard timeout must be between 1 and ", UINT32_MAX, " ms");
          timeoutMs = static_cast<uint32_t>(ms);
        }
        KJ_CASE_ONEOF(duration, kj::String) {
          // Parse duration string like "30s", "5m", "2h", "1000ms"
          JSG_REQUIRE(duration.size() >= 2, TypeError, "Invalid duration format: ", duration);
          
          auto unit = duration[duration.size() - 1];
          auto numberStr = duration.slice(0, duration.size() - 1);
          
          // Handle "ms" suffix
          if (duration.endsWith("ms")) {
            JSG_REQUIRE(duration.size() >= 3, TypeError, "Invalid duration format: ", duration);
            numberStr = duration.slice(0, duration.size() - 2);
            unit = 'x';
          }
          
          KJ_IF_SOME(value, numberStr.parseAs<int64_t>()) {
            JSG_REQUIRE(value > 0, TypeError, "Invalid duration number: ", numberStr);
          } else {
            JSG_FAIL_REQUIRE(TypeError, "Invalid duration number: ", numberStr);
          }
          
          int64_t timeoutMsLong = 0;
          switch (unit) {
            case 'x': timeoutMsLong = value; break;
            case 's': timeoutMsLong = value * 1000; break;
            case 'm': timeoutMsLong = value * 60 * 1000; break;
            case 'h': timeoutMsLong = value * 60 * 60 * 1000; break;
            default:
              JSG_FAIL_REQUIRE(TypeError, "Invalid duration unit '", kj::str(unit), "'. Use 'ms', 's', 'm', or 'h'");
          }
          
          JSG_REQUIRE(timeoutMsLong > 0 && timeoutMsLong <= UINT32_MAX, TypeError, 
                      "Hard timeout must be between 1 and ", UINT32_MAX, " ms (about 49.7 days)");
          timeoutMs = static_cast<uint32_t>(timeoutMsLong);
        }
      }
      
      JSG_REQUIRE(timeoutMs > 0, TypeError, "Hard timeout must be greater than 0");
      req.setHardTimeoutMs(timeoutMs);
    }
  }
}

jsg::Promise<void> Container::setInactivityTimeout(jsg::Lock& js, int64_t durationMs) {
  JSG_REQUIRE(durationMs > 0 && durationMs <= UINT32_MAX, TypeError, 
              "setInactivityTimeout() durationMs must be between 1 and ", UINT32_MAX, " ms (about 49.7 days)");

  auto req = rpcClient->setInactivityTimeoutRequest();

  req.setDurationMs(static_cast<uint32_t>(durationMs));
  return IoContext::current().awaitIo(js, req.sendIgnoringResult());
}


jsg::Promise<void> Container::monitor(jsg::Lock& js) {
  JSG_REQUIRE(running, Error, "monitor() cannot be called on a container that is not running.");

  return IoContext::current()
      .awaitIo(js, rpcClient->monitorRequest(capnp::MessageSize{4, 0}).send())
      .then(js, [this](jsg::Lock& js, capnp::Response<rpc::Container::MonitorResults> results) {
    running = false;
    auto exitCode = results.getExitCode();
    KJ_IF_SOME(d, destroyReason) {
      jsg::Value error = kj::mv(d);
      destroyReason = kj::none;
      js.throwException(kj::mv(error));
    }

    if (exitCode != 0) {
      auto err = js.error(kj::str("Container exited with unexpected exit code: ", exitCode));
      KJ_ASSERT_NONNULL(err.tryCast<jsg::JsObject>()).set(js, "exitCode", js.num(exitCode));
      js.throwException(err);
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
      js, rpcClient->destroyRequest(capnp::MessageSize{4, 0}).sendIgnoringResult());
}

void Container::signal(jsg::Lock& js, int signo) {
  JSG_REQUIRE(signo > 0 && signo <= 64, RangeError, "Invalid signal number.");
  JSG_REQUIRE(running, Error, "signal() cannot be called on a container that is not running.");

  auto req = rpcClient->signalRequest(capnp::MessageSize{4, 0});
  req.setSigno(signo);
  IoContext::current().addTask(req.sendIgnoringResult());
}

// =======================================================================================
// getTcpPort()

// `getTcpPort()` returns a `Fetcher`, on which `fetch()` and `connect()` can be called. `Fetcher`
// is a JavaScript wrapper around `WorkerInterface`, so we need to implement that.
class Container::TcpPortWorkerInterface final: public WorkerInterface {
 public:
  TcpPortWorkerInterface(capnp::ByteStreamFactory& byteStreamFactory,
      kj::EntropySource& entropySource,
      const kj::HttpHeaderTable& headerTable,
      rpc::Container::Port::Client port)
      : byteStreamFactory(byteStreamFactory),
        entropySource(entropySource),
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
        "Connecting to a container using HTTPS is not currently supported; use HTTP instead. "
        "TLS is unnecessary anyway, as the connection is already secure by default.");

    // Schemes other than http: and https: should have been rejected earlier, but let's verify.
    KJ_REQUIRE(parsedUrl.scheme == "http");

    // We need to convert the URL from proxy format (full URL in request line) to host format
    // (path in request line, hostname in Host header).
    auto newHeaders = headers.cloneShallow();
    newHeaders.setPtr(kj::HttpHeaderId::HOST, parsedUrl.host);
    auto noHostUrl = parsedUrl.toString(kj::Url::Context::HTTP_REQUEST);

    // Make a TCP connection...
    auto pipe = kj::newTwoWayPipe();
    kj::Maybe<kj::Exception> connectionException = kj::none;

    auto connectionPromise = connectImpl(*pipe.ends[1]);

    // ... and then stack an HttpClient on it ...
    auto client = kj::newHttpClient(headerTable, *pipe.ends[0], {.entropySource = entropySource});

    // ... and then adapt that to an HttpService ...
    auto service = kj::newHttpService(*client);

    // ... and now we can just forward our call to that.
    try {
      co_await service->request(method, noHostUrl, newHeaders, requestBody, response);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      connectionException = kj::some(kj::mv(exception));
    }

    // we prefer an exception from the container service that might've caused
    // the error in the first place, that's why we await for the connectionPromise
    KJ_IF_SOME(exception, connectionException) {
      co_await connectionPromise;
      kj::throwFatalException(kj::mv(exception));
    }
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
  kj::EntropySource& entropySource;
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
      kj::EntropySource& entropySource,
      const kj::HttpHeaderTable& headerTable,
      rpc::Container::Port::Client port)
      : byteStreamFactory(byteStreamFactory),
        entropySource(entropySource),
        headerTable(headerTable),
        port(kj::mv(port)) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String> cfStr) override {
    // At present we have no use for `cfStr`.
    return kj::heap<TcpPortWorkerInterface>(byteStreamFactory, entropySource, headerTable, port);
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  kj::EntropySource& entropySource;
  const kj::HttpHeaderTable& headerTable;
  rpc::Container::Port::Client port;
};

jsg::Ref<Fetcher> Container::getTcpPort(jsg::Lock& js, int port) {
  JSG_REQUIRE(port > 0 && port < 65536, TypeError, "Invalid port number: ", port);

  auto req = rpcClient->getTcpPortRequest(capnp::MessageSize{4, 0});
  req.setPort(port);

  auto& ioctx = IoContext::current();

  kj::Own<Fetcher::OutgoingFactory> factory =
      kj::heap<TcpPortOutgoingFactory>(ioctx.getByteStreamFactory(), ioctx.getEntropySource(),
          ioctx.getHeaderTable(), req.send().getPort());

  return js.alloc<Fetcher>(
      ioctx.addObject(kj::mv(factory)), Fetcher::RequiresHostAndProtocol::YES, true);
}

}  // namespace workerd::api
