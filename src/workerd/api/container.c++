// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container.h"

#include <workerd/api/http.h>
#include <workerd/api/system-streams.h>
#include <workerd/io/io-context.h>
#include <workerd/util/stream-utils.h>
#include <capnp/membrane.h>


namespace workerd::api {

// =======================================================================================
// Basic lifecycle methods
//

enum class NeuterReason { SENT_RESPONSE, THREW_EXCEPTION, CLIENT_DISCONNECTED };

kj::Exception makeNeuterException(NeuterReason reason) {
  switch (reason) {
    case NeuterReason::SENT_RESPONSE:
      return JSG_KJ_EXCEPTION(
          FAILED, TypeError, "Can't read from request stream after response has been sent.");
    case NeuterReason::THREW_EXCEPTION:
      return JSG_KJ_EXCEPTION(
          FAILED, TypeError, "Can't read from request stream after responding with an exception.");
    case NeuterReason::CLIENT_DISCONNECTED:
      return JSG_KJ_EXCEPTION(
          DISCONNECTED, TypeError, "Can't read from request stream because client disconnected.");
  }
  KJ_UNREACHABLE;
}

class Container::TcpPortConnectHandler final: public rpc::Container::TcpHandler::Server,
                                              public kj::HttpService {
  public:
    TcpPortConnectHandler(
        IoContext& ctx, jsg::Lock& lock, capnp::ByteStreamFactory& byteStreamFactory,
    jsg::Function<jsg::Promise<jsg::Ref<workerd::api::Response>>(jsg::Ref<workerd::api::Request>)>&& handle)
    : js(lock), byteStreamFactory(byteStreamFactory),
        handle(kj::mv(handle)),
        timer(kj::origin<kj::TimePoint>()),
        enterIsolateAndCall(ctx.makeReentryCallback<IoContext::TOP_UP>(
            [this, &ctx](jsg::Lock& lock, ConnectContext context) {
                return connectImpl(lock, ctx, context);
            }
        )),
        enterIsolateAndRequest(ctx.makeReentryCallback<IoContext::TOP_UP>(
            [this, &ctx](jsg::Lock& lock,
                      kj::HttpMethod method,
                      kj::StringPtr url,
                      kj::Own<kj::HttpHeaders> headers,
                      kj::Own<NeuterableInputStream> requestBody,
                      Response& response) {
                    return requestImpl(
                            lock,
                            ctx,
                            method,
                            url,
                            kj::mv(headers),
                            kj::mv(requestBody),
                            response);
            })) {}

  protected:
     class OutputStreamBreakout final: public capnp::ExplicitEndOutputStream {
          // Dumb class which wraps one side of an AsyncIoStream as an AsyncOutputStream.
          // TODO(cleanup): This really ought to be in KJ or something.
         public:
          OutputStreamBreakout(kj::Own<kj::AsyncIoStream> inner): inner(kj::mv(inner)) {}
          ~OutputStreamBreakout() noexcept(false) {
            KJ_IF_SOME(i, inner) {
              i->shutdownWrite();
            }
          }

          kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
            return getInner().write(buffer);
          }
          kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
            return getInner().write(pieces);
          }

          kj::Maybe<kj::Promise<uint64_t>> tryPumpFrom(
              kj::AsyncInputStream& input, uint64_t amount) override {
            return getInner().tryPumpFrom(input, amount);
          }

          kj::Promise<void> whenWriteDisconnected() override {
            return getInner().whenWriteDisconnected();
          }

          kj::Promise<void> end() override {
            getInner().shutdownWrite();
            inner = kj::none;
            return kj::READY_NOW;
          }

         private:
          kj::Maybe<kj::Own<kj::AsyncIoStream>> inner;

          kj::AsyncIoStream& getInner() { return *KJ_ASSERT_NONNULL(inner, "already called end()"); }
        };

    kj::Promise<void> connect(ConnectContext context) override {
        return enterIsolateAndCall(context);
    }

    kj::Promise<void> connectImpl(jsg::Lock& js, IoContext& ctx, ConnectContext context) {
        return ctx.run([this, &ctx, context](jsg::Lock& js) mutable -> kj::Promise<void> {
        auto ownedContext = capnp::CallContextHook::from(context).addRef();
        auto pipe = kj::newTwoWayPipe();
        KJ_LOG(ERROR, "TEST: Lets go first");
        auto httpServer = kj::heap<kj::HttpServer>(timer, headerTable, *this);

        auto promise = httpServer->listenHttp(kj::mv(pipe.ends[1])).catch_([](kj::Exception&& e) {
            KJ_LOG(ERROR, "TEST: what?", e);
        }).attach(kj::mv(httpServer));

         KJ_LOG(ERROR, "TEST: Lets go? (CONNECT) ah ");
         auto stream = kj::refcountedWrapper(kj::mv(pipe.ends[0]));
        KJ_LOG(ERROR, "TEST: Lets go? (CONNECT) ah 2 ");

         auto up = byteStreamFactory.kjToCapnp(kj::heap<OutputStreamBreakout>(stream->addWrappedRef()));
        KJ_LOG(ERROR, "TEST: Lets go? (CONNECT) 0");

         auto down = byteStreamFactory.capnpToKj(context.getParams().getDown());
        KJ_LOG(ERROR, "TEST: Lets go? (CONNECT) 1");

         auto downPump =
             stream->getWrapped().pumpTo(*down).attach(kj::mv(down), kj::mv(stream)).ignoreResult();
         KJ_LOG(ERROR, "TEST: Lets go? (CONNECT) 2");

         capnp::PipelineBuilder<ConnectResults> pipeline;
         pipeline.setUp(up);
         context.setPipeline(pipeline.build());
         context.getResults().setUp(kj::mv(up));
         KJ_LOG(ERROR, "TEST: Lets go? (CONNECT) 3");


         auto promiseIo = ctx.awaitIo(js,
            kj::joinPromisesFailFast(kj::arr(kj::mv(promise), kj::mv(downPump))), [](jsg::Lock& lock) {
         });
         ctx.addTask(ctx.awaitJs(js, kj::mv(promiseIo)));
         return kj::READY_NOW;
        });
    }

    kj::Promise<void> request(
      kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response
    ) override {
        auto headersCloned = kj::heap(headers.cloneShallow());
        auto ownRequestBody = newNeuterableInputStream(requestBody);
        return enterIsolateAndRequest(method, url, kj::mv(headersCloned), kj::mv(ownRequestBody), response);
    }

    // implement this, by sending to jsg callback.
    // Should init a httpServer in the constructor by using *this.
    // see fake-container-service for more details.
    kj::Promise<void> requestImpl(jsg::Lock& js, IoContext& ctx, kj::HttpMethod method,
      kj::StringPtr url,
      kj::Own<kj::HttpHeaders> headers,
      kj::Own<NeuterableInputStream> ownRequestBody,
      Response& response) {

      KJ_LOG(ERROR, "TEST: 2 Lets go? (CONNECT) 1");
      auto deferredNeuter = kj::defer([ownRequestBody = kj::addRef(*ownRequestBody)]() mutable {
        // Make sure to cancel the request body stream since the native stream is no longer valid once
        // the returned promise completes. Note that the KJ HTTP library deals with the fact that we
        // haven't consumed the entire request body.
        ownRequestBody->neuter(makeNeuterException(NeuterReason::CLIENT_DISCONNECTED));
      });

      KJ_ON_SCOPE_FAILURE(ownRequestBody->neuter(makeNeuterException(NeuterReason::THREW_EXCEPTION)));
      KJ_LOG(ERROR, "TEST: 2 Lets go? (CONNECT) 2");

      auto& ioContext = IoContext::current();
      const kj::HttpHeaders& constHeaders = *headers;
      auto jsHeaders = js.alloc<Headers>(js, constHeaders, Headers::Guard::REQUEST);
      auto b = newSystemStream(kj::addRef(*ownRequestBody), StreamEncoding::IDENTITY);
      auto jsStream = js.alloc<ReadableStream>(ioContext, kj::mv(b));
          kj::Maybe<Body::ExtractedBody> body;
      if (headers->get(kj::HttpHeaderId::CONTENT_LENGTH) != kj::none ||
          headers->get(kj::HttpHeaderId::TRANSFER_ENCODING) != kj::none ||
          ownRequestBody->tryGetLength().orDefault(1) > 0) {
        body = Body::ExtractedBody(jsStream.addRef());
      }

      KJ_LOG(ERROR, "TEST: 2 Lets go? (CONNECT) 3");

      if (body != kj::none && headers->get(kj::HttpHeaderId::CONTENT_LENGTH) == kj::none &&
          headers->get(kj::HttpHeaderId::TRANSFER_ENCODING) == kj::none) {
        // We can't use headers.set() here as headers is marked const. Instead, we call set() on the
        // JavaScript headers object, ignoring the REQUEST guard that usually makes them immutable.
        KJ_IF_SOME(l, ownRequestBody->tryGetLength()) {
          jsHeaders->setUnguarded(
              js, jsg::ByteString(kj::str("Content-Length")), jsg::ByteString(kj::str(l)));
        } else {
          jsHeaders->setUnguarded(
              js, jsg::ByteString(kj::str("Transfer-Encoding")), jsg::ByteString(kj::str("chunked")));
        }
      }

      CfProperty cf = CfProperty("{}"_kj);
      auto jsRequest = js.alloc<Request>(js, method, url, Request::Redirect::MANUAL, kj::mv(jsHeaders),
       kj::none,
       /* signal */ kj::none, kj::mv(cf), kj::mv(body),
       /* thisSignal */ kj::none, Request::CacheMode::NONE);
      // co_await ioContext.awaitJs(js, handle(js, kj::mv(jsRequest)).then(js,
      //   [&response, &headers](jsg::Lock& js, jsg::Ref<workerd::api::Response> res) mutable {
      //     KJ_LOG(ERROR, "TEST: Interesting?", res->getType());
      //     auto& context = IoContext::current();
      //     return context.addObject(kj::heap(res->send(js, response, {}, headers)));
      //   }));
      return kj::READY_NOW;
    }

    using kj::HttpService::connect;

  private:
    jsg::Lock& js;
    capnp::ByteStreamFactory& byteStreamFactory;
    jsg::Function<jsg::Promise<jsg::Ref<workerd::api::Response>>(jsg::Ref<workerd::api::Request>)> handle;
    kj::TimerImpl timer;
    kj::HttpHeaderTable headerTable;

    kj::Function<kj::Promise<void>(ConnectContext connectContext)> enterIsolateAndCall;
    kj::Function<kj::Promise<void>(kj::HttpMethod method,
        kj::StringPtr url,
        kj::Own<kj::HttpHeaders> headers,
        kj::Own<NeuterableInputStream> requestBody,
        Response& response)> enterIsolateAndRequest;
    // kj::HttpServer httpServer;
};

Container::Container(rpc::Container::Client rpcClient, bool running)
    : rpcClient(IoContext::current().addObject(kj::heap(kj::mv(rpcClient)))),
      running(running) {}

void Container::listenHttp(jsg::Lock& js, kj::String addr, jsg::Function<jsg::Promise<jsg::Ref<Response>>(jsg::Ref<Request>)> cb) {
    auto req = rpcClient->listenTcpRequest();
    auto& ioctx = IoContext::current();
    req.setHandler(kj::heap<TcpPortConnectHandler>(ioctx, js, ioctx.getByteStreamFactory(), kj::mv(cb)));
    auto cap = req.send();
    KJ_LOG(ERROR, "TEST: Lets go?");
    capabilities.add(cap.getHandle());
}

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
}

jsg::Promise<void> Container::setInactivityTimeout(jsg::Lock& js, int64_t durationMs) {
  JSG_REQUIRE(
      durationMs > 0, TypeError, "setInactivityTimeout() cannot be called with a durationMs <= 0");

  auto req = rpcClient->setInactivityTimeoutRequest();

  req.setDurationMs(durationMs);
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
