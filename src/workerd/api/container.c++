// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container.h"

#include <workerd/api/http.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

// =======================================================================================
// Basic lifecycle methods

Container::Container(rpc::Container::Client rpcClient, bool running)
    : rpcClient(IoContext::current().addObject(kj::heap(kj::mv(rpcClient)))),
      running(running),
      monitorState(IoContext::current().addObject(kj::refcounted<MonitorState>())) {
  // If the container is already running (e.g. pre-existing container at DO startup),
  // immediately set up background monitoring.
  monitorOnBackgroundIfNeeded();
}

void Container::start(jsg::Lock& js, jsg::Optional<StartupOptions> maybeOptions) {
  auto flags = FeatureFlags::get(js);
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

  if (flags.getWorkerdExperimental()) {
    KJ_IF_SOME(hardTimeoutMs, options.hardTimeout) {
      JSG_REQUIRE(hardTimeoutMs > 0, RangeError, "Hard timeout must be greater than 0");
      req.setHardTimeoutMs(hardTimeoutMs);
    }
  }

  req.setCompatibilityFlags(flags);

  auto& ioContext = IoContext::current();

  // Store the start promise so monitorOnBackgroundIfNeeded() can chain after it.
  // This prevents the monitor RPC from racing ahead of the start RPC — without
  // this, Docker's /wait could see a stale container from a previous run and
  // return its old exit code immediately.
  auto forkedStart = req.sendIgnoringResult().fork();
  ioContext.addTask(forkedStart.addBranch());
  startPromise = ioContext.addObject(kj::heap(kj::mv(forkedStart)));

  running = true;

  // Discard stale promises from the previous run so that a subsequent monitor()
  // call gets fresh results.
  monitorJsPromise = kj::none;
  backgroundMonitor = kj::none;

  // Create a fresh MonitorState for the new lifecycle. The old MonitorState is
  // intentionally NOT reused: any in-flight background monitor continuation from
  // the previous run holds a kj::addRef() to the old MonitorState, and if it
  // completes after this reset it would write stale data into our state. By
  // allocating a new object, the old continuation writes to an orphaned object
  // that nobody reads.
  monitorState = ioContext.addObject(kj::refcounted<MonitorState>());

  monitorOnBackgroundIfNeeded();
}

void Container::monitorOnBackgroundIfNeeded() {
  if (backgroundMonitor != kj::none || !running) {
    return;
  }

  auto& ioContext = IoContext::current();

  // Build the background monitor RPC promise. If a start() RPC is in flight,
  // chain the monitor after it completes so Docker /wait sees the newly-created
  // container rather than a stale one from a previous run.
  //
  // This background monitor is a safety net: it stores the exit result in
  // MonitorState (so a later monitor() call can return immediately without
  // awaitIo, which is critical for DO hibernation) and auto-aborts the
  // IoContext if the container crashes without the user calling monitor().
  //
  // The explicit monitor() method sends its own independent RPC, so this
  // background monitor is decoupled from the JS-facing promise.
  //
  // We copy the Cap'n Proto client while the IoContext is live (Cap'n Proto
  // clients are refcounted stubs, so copying is cheap). The copy is captured
  // by value in the .then() lambda, which runs as a KJ continuation where
  // IoContext::current() is not available — so we cannot use the IoOwn wrapper.
  rpc::Container::Client clientCopy = *rpcClient;
  kj::Promise<void> prerequisite = kj::READY_NOW;
  KJ_IF_SOME(sp, startPromise) {
    prerequisite = sp->addBranch();
  }

  // Set up the background monitoring task. We capture a weak reference to the
  // IoContext and a refcounted reference to the MonitorState, both of which are
  // safe to access from KJ promise continuations (which run without the isolate
  // lock held).
  auto bgPromise =
      prerequisite
          .then([clientCopy = kj::mv(clientCopy)]() mutable -> kj::Promise<int32_t> {
    return clientCopy.monitorRequest(capnp::MessageSize{4, 0})
        .send()
        .then([](auto&& results) -> int32_t { return results.getExitCode(); });
  })
          .then(
              [state = kj::addRef(*monitorState)](int32_t exitCode) {
    // Store the exit code so monitor() can return immediately without awaitIo().
    state->finished = true;
    state->exitCode = exitCode;
  },
              [state = kj::addRef(*monitorState), weakIoContext = ioContext.getWeakRef()](
                  kj::Exception&& error) mutable {
    // Store the error so monitor() can return immediately without awaitIo().
    state->finished = true;
    state->exception = kj::cp(error);

    // The container exited with an error. If the user hasn't called monitor()
    // explicitly, abort the IoContext so the DO is notified of the failure.
    if (!state->monitoringExplicitly) {
      weakIoContext->runIfAlive([&error](IoContext& ctx) { ctx.abort(kj::mv(error)); });
    }
  }).eagerlyEvaluate([](kj::Exception&& e) {
    KJ_LOG(ERROR, "unexpected error in container background monitor", e);
  });

  backgroundMonitor = ioContext.addObject(kj::heap(kj::mv(bgPromise)));
}

jsg::Promise<void> Container::setInactivityTimeout(jsg::Lock& js, int64_t durationMs) {
  JSG_REQUIRE(
      durationMs > 0, TypeError, "setInactivityTimeout() cannot be called with a durationMs <= 0");

  auto req = rpcClient->setInactivityTimeoutRequest();

  req.setDurationMs(durationMs);
  return IoContext::current().awaitIo(js, req.sendIgnoringResult());
}

jsg::Promise<void> Container::interceptOutboundHttp(
    jsg::Lock& js, kj::String addr, jsg::Ref<Fetcher> binding) {
  auto& ioctx = IoContext::current();
  auto channel = binding->getSubrequestChannel(ioctx);

  // Get a channel token for RPC usage, the container runtime can use this
  // token later to redeem a Fetcher.
  auto token = channel->getToken(IoChannelFactory::ChannelTokenUsage::RPC);

  auto req = rpcClient->setEgressHttpRequest();
  req.setHostPort(addr);
  req.setChannelToken(token);
  return ioctx.awaitIo(js, req.sendIgnoringResult());
}

jsg::Promise<void> Container::interceptAllOutboundHttp(jsg::Lock& js, jsg::Ref<Fetcher> binding) {
  auto& ioctx = IoContext::current();
  auto channel = binding->getSubrequestChannel(ioctx);
  auto token = channel->getToken(IoChannelFactory::ChannelTokenUsage::RPC);

  // Register for all IPv4 and IPv6 addresses (on port 80)
  auto reqV4 = rpcClient->setEgressHttpRequest();
  reqV4.setHostPort("0.0.0.0/0"_kj);
  reqV4.setChannelToken(token);

  auto reqV6 = rpcClient->setEgressHttpRequest();
  reqV6.setHostPort("::/0"_kj);
  reqV6.setChannelToken(token);

  return ioctx.awaitIo(js,
      kj::joinPromisesFailFast(kj::arr(reqV4.sendIgnoringResult(), reqV6.sendIgnoringResult())));
}

jsg::MemoizedIdentity<jsg::Promise<void>>& Container::monitor(jsg::Lock& js) {
  // If the container is already being monitored, return the existing promise.
  // This must come before the running check so that monitor() called after
  // destroy() still returns the in-flight promise rather than throwing.
  KJ_IF_SOME(memoized, monitorJsPromise) {
    return memoized;
  }

  // If the container is not running and there's no existing monitor promise,
  // return an immediately-rejected promise. We avoid throwing synchronously
  // (via JSG_REQUIRE) because existing code patterns call monitor() after
  // destroy() with .catch() — a synchronous throw would bypass .catch().
  if (!running) {
    auto jsPromise = js.rejectedPromise<void>(JSG_KJ_EXCEPTION(
        FAILED, Error, "monitor() cannot be called on a container that is not running."));
    monitorJsPromise = jsg::MemoizedIdentity<jsg::Promise<void>>(kj::mv(jsPromise));
    return KJ_ASSERT_NONNULL(monitorJsPromise);
  }

  monitorState->monitoringExplicitly = true;

  // Safe to capture `this`: the resulting promise is stored in monitorJsPromise,
  // which is traced by visitForGc(), preventing the Container from being collected
  // while the promise is alive.
  auto handleExitCode = [this](jsg::Lock& js, int32_t exitCode) {
    running = false;
    monitorState->monitoringExplicitly = false;
    // Clean up stale promise state so a subsequent start()/monitor() cycle works.
    backgroundMonitor = kj::none;
    startPromise = kj::none;

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
  };

  // Same safety note as handleExitCode above.
  auto handleError = [this](jsg::Lock& js, jsg::Value&& error) {
    running = false;
    monitorState->monitoringExplicitly = false;
    // Clean up stale promise state.
    backgroundMonitor = kj::none;
    startPromise = kj::none;
    destroyReason = kj::none;
    js.throwException(kj::mv(error));
  };

  jsg::Promise<void> jsPromise = ([&]() -> jsg::Promise<void> {
    if (monitorState->finished) {
      // The background monitor has already completed. Return an immediately-resolved
      // or immediately-rejected promise without calling awaitIo(). This avoids
      // registering a pending event, which would block DO hibernation.
      KJ_IF_SOME(error, monitorState->exception) {
        return js.rejectedPromise<void>(kj::cp(error)).catch_(js, kj::mv(handleError));
      }
      return js.resolvedPromise().then(js,
          [exitCode = monitorState->exitCode, handleExitCode = kj::mv(handleExitCode)](
              jsg::Lock& js) mutable { handleExitCode(js, exitCode); });
    }

    // Container is still running. Send a new, independent monitor RPC via awaitIo.
    // This is separate from the background monitor's RPC so that destroy() can
    // cancel the background without affecting this user-facing promise.
    return IoContext::current()
        .awaitIo(js, rpcClient->monitorRequest(capnp::MessageSize{4, 0}).send())
        .then(js,
            [handleExitCode = kj::mv(handleExitCode)](
                jsg::Lock& js, capnp::Response<rpc::Container::MonitorResults> results) mutable {
      handleExitCode(js, results.getExitCode());
    },
            kj::mv(handleError));
  })();

  monitorJsPromise = jsg::MemoizedIdentity<jsg::Promise<void>>(kj::mv(jsPromise));

  return KJ_ASSERT_NONNULL(monitorJsPromise);
}

jsg::Promise<void> Container::destroy(jsg::Lock& js, jsg::Optional<jsg::Value> error) {
  if (!running) return js.resolvedPromise();

  if (destroyReason == kj::none) {
    destroyReason = kj::mv(error);
  }

  // Mark the container as stopped immediately. This prevents monitorOnBackgroundIfNeeded()
  // from setting up new monitors for a container that is being destroyed, and makes
  // monitor() correctly reject with "container is not running".
  running = false;

  // Cancel the background monitor and start promises — the background monitor is
  // the auto-abort safety net and is no longer needed once destroy is called.
  // If monitor() was called, its independent RPC (sent via awaitIo) is unaffected
  // by this cancellation and will complete naturally (Docker /wait returns exit
  // code 137 when the container is killed).
  backgroundMonitor = kj::none;
  startPromise = kj::none;

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

    // ... fork connection promises so we can keep the original exception around ...
    auto connectionPromiseForked = connectionPromise.fork();
    auto connectionPromiseBranch = connectionPromiseForked.addBranch();
    auto connectionPromiseToKeepException = connectionPromiseForked.addBranch();

    // ... and now we can just forward our call to that ...
    try {
      co_await service->request(method, noHostUrl, newHeaders, requestBody, response)
          .exclusiveJoin(
              // never done as we do not want a Connection RPC exiting successfully
              // affecting the request
              connectionPromiseBranch.then([]() -> kj::Promise<void> { return kj::NEVER_DONE; }));
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      connectionException = kj::some(kj::mv(exception));
    }

    // ... and last but not least, if the connect() call succeeded but the connection
    // was broken, we throw that exception.
    KJ_IF_SOME(exception, connectionException) {
      co_await connectionPromiseToKeepException;
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
