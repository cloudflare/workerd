// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-factory-impl.h"

#include <workerd/api/sockets.h>
#include <workerd/api/trace.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/io/trace-stream.h>
#include <workerd/server/actor-id-impl.h>
#include <workerd/util/mimetype.h>

#include <kj/compat/http.h>

namespace workerd::server {

// =======================================================================================
// WorkerdBootstrap

// Dispatches one event to the bootstrap's service.
class WorkerdBootstrapImpl::EventDispatcherImpl final: public rpc::EventDispatcher::Server {
 public:
  EventDispatcherImpl(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      kj::Rc<IoChannelFactory::SubrequestChannel> service,
      kj::Maybe<kj::String> cfBlobJson,
      Persistent fromPersistentStub)
      : httpOverCapnpFactory(httpOverCapnpFactory),
        service(kj::mv(service)),
        cfBlobJson(kj::mv(cfBlobJson)),
        fromPersistentStub(fromPersistentStub) {}

  kj::Promise<void> getHttpService(GetHttpServiceContext context) override {
    IoChannelFactory::SubrequestMetadata metadata;
    metadata.cfBlobJson = cfBlobJson.clone();
    metadata.fromPersistentStub = fromPersistentStub;
    context.initResults(capnp::MessageSize{4, 1})
        .setHttp(httpOverCapnpFactory.kjToCapnp(getService()->startRequest(kj::mv(metadata))));
    return kj::READY_NOW;
  }

  kj::Promise<void> sendTraces(SendTracesContext context) override {
    auto traces =
        KJ_MAP(trace, context.getParams().getTraces()) { return kj::refcounted<Trace>(trace); };
    auto event = kj::heap<api::TraceCustomEvent>(api::TraceCustomEvent::TYPE, kj::mv(traces));
    auto worker = getWorker();
    auto result = co_await worker->customEvent(kj::mv(event));
    context.getResults().getResult().setOutcome(result.outcome);
  }

  kj::Promise<void> prewarm(PrewarmContext context) override {
    throwUnsupported();
  }
  kj::Promise<void> runScheduled(RunScheduledContext context) override {
    throwUnsupported();
  }
  kj::Promise<void> runAlarm(RunAlarmContext context) override {
    throwUnsupported();
  }
  kj::Promise<void> queue(QueueContext context) override {
    throwUnsupported();
  }

  kj::Promise<void> jsRpcSession(JsRpcSessionContext context) override {
    return api::JsRpcSessionCustomEvent::receiveRpc(context, getWorker());
  }

  kj::Promise<void> udpConnect(UdpConnectContext context) override {
    auto worker = getWorker();
    auto& workerRef = *worker;
    return api::UdpConnectCustomEvent::receiveRpc(context, workerRef).attach(kj::mv(worker));
  }

  kj::Promise<void> tailStreamSession(TailStreamSessionContext context) override {
    auto customEvent = kj::heap<tracing::TailStreamCustomEvent>();
    auto cap = customEvent->getCap();
    capnp::PipelineBuilder<TailStreamSessionResults> pipelineBuilder;
    pipelineBuilder.setTopLevel(cap);
    context.setPipeline(pipelineBuilder.build());
    context.getResults().setTopLevel(kj::mv(cap));

    auto worker = getWorker();
    auto result = co_await worker->customEvent(kj::mv(customEvent)).attach(kj::mv(worker));
    context.getResults().setResult(result.outcome);
  }

 private:
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  kj::Maybe<kj::Rc<IoChannelFactory::SubrequestChannel>> service;
  kj::Maybe<kj::String> cfBlobJson;
  Persistent fromPersistentStub;

  kj::Rc<IoChannelFactory::SubrequestChannel> getService() {
    auto result =
        kj::mv(KJ_ASSERT_NONNULL(service, "EventDispatcher can only be used for one request"));
    service = kj::none;
    return result;
  }

  // Events other than HTTP carry no cf blob.
  kj::Own<WorkerInterface> getWorker() {
    return getService()->startRequest({});
  }

  [[noreturn]] void throwUnsupported() {
    JSG_FAIL_REQUIRE(Error, "RPC connections don't yet support this event type.");
  }
};

WorkerdBootstrapImpl::WorkerdBootstrapImpl(kj::Rc<IoChannelFactory::SubrequestChannel> service,
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory)
    : service(kj::mv(service)),
      httpOverCapnpFactory(httpOverCapnpFactory) {}

kj::Promise<void> WorkerdBootstrapImpl::startEvent(StartEventContext context) {
  auto params = context.getParams();
  kj::Maybe<kj::String> cfBlobJson;
  if (params.hasCfBlobJson()) cfBlobJson = kj::str(params.getCfBlobJson());
  context.initResults(capnp::MessageSize{4, 1})
      .setDispatcher(kj::heap<EventDispatcherImpl>(httpOverCapnpFactory, service->addRef(),
          kj::mv(cfBlobJson), Persistent(params.getFromPersistentStub())));
  return kj::READY_NOW;
}

kj::Promise<void> factory_accept_bootstrap(const WorkerFactory& factory,
    kj::Own<kj::AsyncIoStream> stream,
    ::rust::Box<SubrequestChannel> target) {
  capnp::TwoPartyServer server(kj::heap<WorkerdBootstrapImpl>(
      subrequest_channel_into_kj(kj::mv(target)), factory.getImpl().httpOverCapnpFactory));
  co_await server.accept(*stream);
}

// =======================================================================================
// Debug port

WorkerdDebugPortImpl::WorkerdDebugPortImpl(
    ::rust::Box<ServerHandle> server, capnp::HttpOverCapnpFactory& httpOverCapnpFactory)
    : server(kj::mv(server)),
      httpOverCapnpFactory(httpOverCapnpFactory) {}

kj::Promise<void> WorkerdDebugPortImpl::getEntrypoint(GetEntrypointContext context) {
  auto params = context.getParams();
  kj::Maybe<kj::Own<Frankenvalue>> props;
  if (params.hasProps()) props = toOwn(Frankenvalue::fromCapnp(params.getProps()));
  kj::Maybe<kj::StringPtr> entrypoint;
  if (params.hasEntrypoint()) entrypoint = params.getEntrypoint();
  auto target = subrequest_channel_into_kj(server->resolve_debug_entrypoint(
      toRust(params.getService()), toRust(entrypoint), kj::mv(props)));
  context.initResults(capnp::MessageSize{4, 1})
      .setEntrypoint(kj::heap<WorkerdBootstrapImpl>(kj::mv(target), httpOverCapnpFactory));
  return kj::READY_NOW;
}

kj::Promise<void> WorkerdDebugPortImpl::getActor(GetActorContext context) {
  auto params = context.getParams();
  auto target = subrequest_channel_into_kj(server->resolve_debug_actor(
      toRust(params.getService()), toRust(params.getEntrypoint()), toRust(params.getActorId())));
  context.initResults(capnp::MessageSize{4, 1})
      .setActor(kj::heap<WorkerdBootstrapImpl>(kj::mv(target), httpOverCapnpFactory));
  return kj::READY_NOW;
}

kj::Promise<void> factory_accept_debug_port(
    const WorkerFactory& factory, kj::Own<kj::AsyncIoStream> stream) {
  auto& impl = factory.getImpl();
  capnp::TwoPartyServer rpcServer(rpc::WorkerdDebugPort::Client(
      kj::heap<WorkerdDebugPortImpl>(impl.getServer().server_clone(), impl.httpOverCapnpFactory)));
  co_await rpcServer.accept(*stream);
}

// =======================================================================================
// RPC client

RpcClient::RpcClient(kj::Own<Impl> impl): impl(kj::mv(impl)) {}
RpcClient::~RpcClient() noexcept(false) = default;
RpcClient::Impl& RpcClient::getImpl() const {
  return *impl;
}

kj::Own<RpcClient> new_rpc_client(const WorkerFactory& factory, kj::Own<kj::AsyncIoStream> stream) {
  return kj::heap<RpcClient>(kj::heap<RpcClient::Impl>(factory, kj::mv(stream)));
}

kj::Promise<rust::worker::CustomEventResult> rpc_client_custom_event(const RpcClient& client,
    kj::Own<WorkerInterface::CustomEvent> event,
    kj::Maybe<::rust::Str> cfBlobJson) {
  auto& impl = client.getImpl();
  auto bootstrap = impl.rpcSystem.bootstrap().castAs<rpc::WorkerdBootstrap>();
  auto request = bootstrap.startEventRequest(capnp::MessageSize{4, 0});
  KJ_IF_SOME(cf, cfBlobJson) request.setCfBlobJson(kj::str(cf));
  auto dispatcher = request.send().getDispatcher();
  // Workerd-to-workerd RPC does not support `restore()`, so Frankenvalues need no handler.
  auto& factory = impl.factory.getImpl();
  auto result = co_await event->sendRpc(factory.httpOverCapnpFactory, factory.byteStreamFactory,
      getUnsupportedFrankenvalueHandler(), kj::mv(dispatcher));
  co_return rust::worker::CustomEventResult{.outcome = rust::worker::toRustOutcome(result.outcome)};
}

kj::Promise<void> rpc_client_on_disconnect(const RpcClient& client) {
  return client.getImpl().rpcSystem.onDisconnect();
}

// =======================================================================================
// Inspector
//
// The devtools inspector protocol starts with HTTP GETs to /json/version and /json (or
// /json/list), which list the isolates available for inspection, each with a URL and an id the
// client then opens a WebSocket to. The Cloudflare devtools show only the first service in the
// config; Chrome's devtools can inspect every one.

class InspectorService final: public kj::HttpService, public kj::HttpServerErrorHandler {
 public:
  InspectorService(kj::Own<const kj::Executor> isolateThreadExecutor,
      kj::Timer& timer,
      kj::HttpHeaderTable::Builder& headerTableBuilder,
      InspectorServiceIsolateRegistrar& registrar)
      : isolateThreadExecutor(kj::mv(isolateThreadExecutor)),
        timer(timer),
        headerTable(headerTableBuilder.getFutureTable()),
        server(timer, headerTable, *this, kj::HttpServerSettings{.errorHandler = *this}),
        registrar(registrar) {
    registrar.attach(*this);
  }

  ~InspectorService() noexcept(false) {
    KJ_IF_SOME(r, registrar) r.detach();
  }

  void invalidateRegistrar() {
    registrar = kj::none;
  }

  kj::Promise<void> handleApplicationError(
      kj::Exception exception, kj::Maybe<kj::HttpService::Response&> response) override {
    if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
      // Just close the connection.
      co_return;
    }
    KJ_LOG(ERROR, kj::str("Uncaught exception: ", exception));
    KJ_IF_SOME(r, response) {
      co_return co_await r.sendError(500, "Internal Server Error", headerTable);
    }
  }

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    kj::HttpHeaders responseHeaders(headerTable);
    if (headers.isWebSocket()) {
      KJ_IF_SOME(pos, url.findLast('/')) {
        auto id = url.slice(pos + 1);
        KJ_IF_SOME(isolate, isolates.find(id)) {
          // The isolate is held weakly so that it need not know about the inspector; a weak ref
          // that no longer upgrades means the isolate is gone and the entry is dropped.
          KJ_IF_SOME(ref, isolate->tryAddStrongRef()) {
            KJ_LOG(INFO, kj::str("Inspector client attaching [", id, "]"));
            auto webSocket = response.acceptWebSocket(responseHeaders);
            kj::Duration timerOffset = 0 * kj::MILLISECONDS;
            try {
              co_return co_await ref->attachInspector(
                  isolateThreadExecutor->addRef(), timer, timerOffset, *webSocket);
            } catch (...) {
              auto exception = kj::getCaughtExceptionAsKj();
              if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
                KJ_LOG(INFO, "Inspector client detached"_kj);
                co_return;
              }
              kj::throwFatalException(kj::mv(exception));
            }
          } else {
            isolates.erase(id);
          }
        }
        KJ_LOG(INFO, kj::str("Unknown worker session [", id, "]"));
        co_return co_await response.sendError(404, "Unknown worker session", responseHeaders);
      }
      co_return co_await response.sendError(400, "Invalid request", responseHeaders);
    }

    if (method != kj::HttpMethod::GET) {
      co_return co_await response.sendError(501, "Unsupported Operation", responseHeaders);
    }

    if (url.endsWith("/json/version")) {
      responseHeaders.set(kj::HttpHeaderId::CONTENT_TYPE, MimeType::JSON.toString());
      auto content = kj::str("{\"Browser\": \"workerd\", \"Protocol-Version\": \"1.3\" }");
      auto out = response.send(200, "OK", responseHeaders, content.size());
      co_return co_await out->write(content.asBytes());
    } else if (url.endsWith("/json") || url.endsWith("/json/list") ||
        url.endsWith("/json/list?for_tab")) {
      responseHeaders.set(kj::HttpHeaderId::CONTENT_TYPE, MimeType::JSON.toString());
      auto baseWsUrl = KJ_UNWRAP_OR(headers.get(kj::HttpHeaderId::HOST),
          { co_return co_await response.sendError(400, "Bad Request", responseHeaders); });

      kj::Vector<kj::String> entries(isolates.size());
      kj::Vector<kj::String> toRemove;
      for (auto& entry: isolates) {
        // Upgrading the weak ref tells whether the isolate still exists.
        KJ_IF_SOME(ref, entry.value->tryAddStrongRef()) {
          (void)ref;
          kj::Vector<kj::String> fields(9);
          fields.add(kj::str("\"id\":\"", entry.key, "\""));
          fields.add(kj::str("\"title\":\"workerd: worker ", entry.key, "\""));
          fields.add(kj::str("\"type\":\"node\""));
          fields.add(kj::str("\"description\":\"workerd worker\""));
          fields.add(kj::str("\"webSocketDebuggerUrl\":\"ws://", baseWsUrl, "/", entry.key, "\""));
          fields.add(kj::str(
              "\"devtoolsFrontendUrl\":\"devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=",
              baseWsUrl, "/\""));
          fields.add(kj::str(
              "\"devtoolsFrontendUrlCompat\":\"devtools://devtools/bundled/inspector.html?experiments=true&v8only=true&ws=",
              baseWsUrl, "/\""));
          fields.add(kj::str("\"faviconUrl\":\"https://workers.cloudflare.com/favicon.ico\""));
          fields.add(kj::str("\"url\":\"https://workers.dev\""));
          entries.add(kj::str('{', kj::strArray(fields, ","), '}'));
        } else {
          toRemove.add(kj::str(entry.key));
        }
      }
      for (auto& key: toRemove) {
        isolates.erase(key);
      }

      auto content = kj::str('[', kj::strArray(entries, ","), ']');
      auto out = response.send(200, "OK", responseHeaders, content.size());
      co_return co_await out->write(content.asBytes()).attach(kj::mv(content), kj::mv(out));
    }

    co_return co_await response.sendError(500, "Not yet implemented", responseHeaders);
  }

  // Inspector connections are long-lived WebSockets that must not hold the server open, so they
  // live on this HttpServer's own TaskSet and take no part in draining.
  kj::Promise<void> listen(kj::Own<kj::ConnectionReceiver> listener) {
    co_return co_await server.listenHttp(*listener);
  }

  void registerIsolate(kj::StringPtr name, Worker::Isolate& isolate) {
    isolates.insert(kj::str(name), isolate.getWeakRef());
  }

 private:
  kj::Own<const kj::Executor> isolateThreadExecutor;
  kj::Timer& timer;
  kj::HttpHeaderTable& headerTable;
  kj::HashMap<kj::String, kj::Own<const Worker::Isolate::WeakIsolateRef>> isolates;
  kj::HttpServer server;
  kj::Maybe<InspectorServiceIsolateRegistrar&> registrar;
};

InspectorServiceIsolateRegistrar::~InspectorServiceIsolateRegistrar() noexcept(true) {
  KJ_IF_SOME(service, *inspectorService.lockExclusive()) {
    service.invalidateRegistrar();
  }
}

void InspectorServiceIsolateRegistrar::registerIsolate(
    kj::StringPtr name, Worker::Isolate& isolate) {
  KJ_IF_SOME(service, *inspectorService.lockExclusive()) {
    service.registerIsolate(name, isolate);
  }
}

void InspectorServiceIsolateRegistrar::attach(InspectorService& service) {
  *inspectorService.lockExclusive() = service;
}

void InspectorServiceIsolateRegistrar::detach() {
  *inspectorService.lockExclusive() = kj::none;
}

uint startInspector(kj::String inspectorAddress, InspectorServiceIsolateRegistrar& registrar) {
  static constexpr uint UNASSIGNED_PORT = 0;
  static constexpr uint DEFAULT_PORT = 9229;
  kj::MutexGuarded<uint> inspectorPort(UNASSIGNED_PORT);

  // V8 requires CPU profiling to start and stop on the thread that runs JavaScript, so inspector
  // messages are dispatched on this (the isolate) thread: its executor goes to the inspector
  // service, which `Isolate::attachInspector()` uses to run its dispatch loop here.
  auto isolateThreadExecutor = kj::getCurrentThreadExecutor().addRef();

  kj::Thread thread([inspectorAddress = kj::mv(inspectorAddress), &inspectorPort, &registrar,
                        isolateThreadExecutor = kj::mv(isolateThreadExecutor)]() mutable {
    kj::AsyncIoContext io = kj::setupAsyncIo();
    kj::HttpHeaderTable::Builder headerTableBuilder;
    auto inspectorService = kj::heap<InspectorService>(
        kj::mv(isolateThreadExecutor), io.provider->getTimer(), headerTableBuilder, registrar);
    auto ownHeaderTable = headerTableBuilder.build();
    auto& network = io.provider->getNetwork();

    // A failure to listen is not reported: the port is never assigned, and the starting thread
    // keeps waiting for it.
    auto listen = (kj::coCapture(
        [&network, &inspectorAddress, &inspectorPort, &inspectorService]() -> kj::Promise<void> {
      auto parsed = co_await network.parseAddress(inspectorAddress, DEFAULT_PORT);
      auto listener = parsed->listen();
      // Signals the starting thread that the inspector is ready.
      *inspectorPort.lockExclusive() = listener->getPort();
      KJ_LOG(INFO, "Inspector is listening");
      co_await inspectorService->listen(kj::mv(listener));
    }))();

    kj::NEVER_DONE.wait(io.waitScope);
  });
  thread.detach();

  return inspectorPort.when([](const uint& port) { return port != UNASSIGNED_PORT; },
      [](const uint& port) { return port; });
}

uint16_t factory_start_inspector(const WorkerFactory& factory, ::rust::Str address) {
  auto& impl = factory.getImpl();
  KJ_REQUIRE(impl.inspectorRegistrar == kj::none, "the inspector is already running");
  auto& registrar = *impl.inspectorRegistrar.emplace(kj::heap<InspectorServiceIsolateRegistrar>());
  return startInspector(kj::str(address), registrar);
}

}  // namespace workerd::server
