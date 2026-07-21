// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "restore.h"

#include "global-scope.h"
#include "http.h"
#include "worker-rpc.h"

#include <workerd/io/tracer.h>
#include <workerd/util/completion-membrane.h>

namespace workerd::api {

using RestoreResult = kj::OneOf<jsg::Ref<Fetcher>, jsg::Ref<JsRpcStub>>;

// Invoke the entrypoint's `[restore]()` method and coerce the result to either Fetcher or
// JsRpcStub.
static jsg::Promise<RestoreResult> invokeRestoreAndCoerce(
    jsg::Lock& js, jsg::JsObject target, jsg::JsValue params) {
  auto restoreValue = target.get(js, js.symbolInternal("cloudflare:workers:restore"));
  auto restoreFunction = JSG_REQUIRE_NONNULL(restoreValue.tryCast<jsg::JsFunction>(), TypeError,
      "The WorkerEntrypoint or DurableObject class does not implement a [restore]() method.");

  auto result = restoreFunction.call(js, target, params);
  return js.toPromise(result).then(js, [](jsg::Lock& js, jsg::Value value) -> RestoreResult {
    auto jsValue = jsg::JsValue(value.getHandle(js));
    auto object = JSG_REQUIRE_NONNULL(jsValue.tryCast<jsg::JsObject>(), TypeError,
        "The [restore]() method must return a ServiceStub or RpcStub.");

    KJ_IF_SOME(fetcher, object.tryUnwrapAs<Fetcher>(js)) {
      return fetcher.addRef();
    }

    KJ_IF_SOME(stub, object.tryUnwrapAs<JsRpcStub>(js)) {
      return stub.addRef();
    }

    // Types which implicitly convert to RpcStub (RpcTargets and functions) should be accepted
    // as a return value, too.
    if (JsRpcStub::shouldImplicitlyStubify(js, object)) {
      return JsRpcStub::constructor(js, object);
    }

    JSG_FAIL_REQUIRE(TypeError, "The [restore]() method must return a ServiceStub or RpcStub.");
  });
}

jsg::Promise<jsg::Value> restoreCurrentEntrypoint(jsg::Lock& js,
    jsg::JsObject params,
    const jsg::TypeHandler<jsg::Ref<Fetcher>>& fetcherHandler,
    const jsg::TypeHandler<jsg::Ref<JsRpcStub>>& rpcStubHandler) {
  auto restoreParams = Frankenvalue::fromJs(js, jsg::JsValue(params));
  auto target = IoContext::current().getEntrypointHandler(js);

  return invokeRestoreAndCoerce(js, target, params)
      .then(js,
          [&fetcherHandler, &rpcStubHandler, restoreParams = kj::mv(restoreParams)](
              jsg::Lock& js, RestoreResult result) mutable -> jsg::Value {
    auto& ioctx = IoContext::current();
    auto selfTokenFactory = JSG_REQUIRE_NONNULL(ioctx.getSelfTokenFactory(), Error,
        "ctx.restore() cannot be used in this context because the system does not know how to "
        "restore this context itself, in order to be able to invoke its [restore]() method. "
        "This may happen if the current worker is a Dynamic Worker or a Durable Object Facet (or "
        "both), and the immediate caller did not specify how to restore it. To fix this, ensure "
        "that the caller of this Worker uses a stub that was created via its own "
        "context.restore().");

    auto& factory = ioctx.getIoChannelFactory();

    Persistent persistent = Persistent(FeatureFlags::get(js).getAllowIrrevocableStubStorage());

    KJ_SWITCH_ONEOF(result) {
      KJ_CASE_ONEOF(fetcher, jsg::Ref<Fetcher>) {
        auto baseChannel = fetcher->getSubrequestChannel(ioctx);
        auto channel = factory.makeRestoredSubrequestChannel(
            kj::addRef(*selfTokenFactory), kj::mv(restoreParams), kj::mv(baseChannel), persistent);
        auto restored = js.alloc<Fetcher>(ioctx.addObject(kj::mv(channel)));
        return jsg::Value(js.v8Isolate, fetcherHandler.wrap(js, kj::mv(restored)));
      }
      KJ_CASE_ONEOF(stub, jsg::Ref<JsRpcStub>) {
        auto channel = factory.makeRestoredRpcChannel(
            kj::addRef(*selfTokenFactory), kj::mv(restoreParams), persistent);
        auto client = stub->getClient();
        stub->dispose();
        auto restored = js.alloc<JsRpcStub>(
            ioctx.addObject(kj::heap(kj::mv(client))), ioctx.addObject(kj::mv(channel)));
        return jsg::Value(js.v8Isolate, rpcStubHandler.wrap(js, kj::mv(restored)));
      }
    }
    KJ_UNREACHABLE;
  });
}

namespace {

// SubrequestChannel implementation used when RestoreServiceCustomEvent is sent over RPC.
//
// Note that this SubrequestChannel is *always* wrapped in a `RestoredSubrequestChannel` (either
// the one in `ChannelTokenHandler` or the one in the edge runtime). This means this class really
// only has to implement `startRequest()`. Other methods will never be called, nor will anyone
// ever try to downcast this to some more specialized interface.
class RestoredRpcSubrequestChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  RestoredRpcSubrequestChannel(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      FrankenvalueHandler& frankenvalueHandler,
      rpc::WorkerdBootstrap::Client rpcClient)
      : httpOverCapnpFactory(httpOverCapnpFactory),
        byteStreamFactory(byteStreamFactory),
        frankenvalueHandler(frankenvalueHandler),
        rpcClient(kj::mv(rpcClient)) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    capnp::MessageSize sizeHint{4, 0};
    KJ_IF_SOME(cf, metadata.cfBlobJson) {
      sizeHint.wordCount += cf.size() / sizeof(capnp::word);
    }

    auto req = rpcClient.startEventRequest(sizeHint);
    KJ_IF_SOME(cf, metadata.cfBlobJson) {
      req.setCfBlobJson(cf);
    }
    req.setFromPersistentStub(metadata.fromPersistentStub.toBool());
    auto dispatcher = req.sendForPipeline().getDispatcher();

    return kj::heap<RpcWorkerInterface>(
        httpOverCapnpFactory, byteStreamFactory, frankenvalueHandler, kj::mv(dispatcher));
  }

  void requireAllowsTransfer() override {
    KJ_FAIL_ASSERT("this should never be called because we should be wrapped in a "
                   "`RestoredSubrequestChannel`");
  }

  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_FAIL_ASSERT("this should never be called because we should be wrapped in a "
                   "`RestoredSubrequestChannel`");
  }

 private:
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  capnp::ByteStreamFactory& byteStreamFactory;
  FrankenvalueHandler& frankenvalueHandler;
  rpc::WorkerdBootstrap::Client rpcClient;
};

// Wraps a restored service channel and keeps the restore event alive until the channel is no
// longer held. Some channel implementations are tied to the IoContext where [restore]() ran.
class LifetimeExtendedSubrequestChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  LifetimeExtendedSubrequestChannel(kj::Own<IoChannelFactory::SubrequestChannel> inner,
      kj::Own<kj::PromiseFulfiller<void>> doneFulfiller)
      : inner(kj::mv(inner)),
        doneFulfiller(kj::mv(doneFulfiller)) {}

  ~LifetimeExtendedSubrequestChannel() noexcept(false) {
    if (doneFulfiller->isWaiting()) {
      doneFulfiller->fulfill();
    }
  }

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return inner->startRequest(kj::mv(metadata));
  }

  void requireAllowsTransfer() override {
    inner->requireAllowsTransfer();
  }

  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    return inner->getTokenMaybeSync(usage);
  }

 private:
  kj::Own<IoChannelFactory::SubrequestChannel> inner;
  kj::Own<kj::PromiseFulfiller<void>> doneFulfiller;
};

// EventDispatcher server that forwards events to a restored service channel. This is a minimal
// duplicate of the (private) `Server::WorkerdBootstrapImpl::EventDispatcherImpl` in workerd's
// server.c++, used to implement the `service :WorkerdBootstrap` result of `restoreService()` when
// the restore event is received over RPC. A restored ServiceStub is only ever used to make HTTP
// requests, make JS-RPC calls, or receive the next hop of a restore chain, so other event types
// are unsupported.
class RestoredServiceEventDispatcher final: public rpc::EventDispatcher::Server {
 public:
  RestoredServiceEventDispatcher(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      kj::Own<IoChannelFactory::SubrequestChannel> service,
      kj::Maybe<kj::String> cfBlobJson,
      Persistent fromPersistentStub,
      kj::Rc<RestoreParamsHandler> paramsHandler)
      : httpOverCapnpFactory(httpOverCapnpFactory),
        service(kj::mv(service)),
        cfBlobJson(kj::mv(cfBlobJson)),
        fromPersistentStub(fromPersistentStub),
        paramsHandler(kj::mv(paramsHandler)) {}

  kj::Promise<void> getHttpService(GetHttpServiceContext context) override {
    context.initResults(capnp::MessageSize{4, 1})
        .setHttp(httpOverCapnpFactory.kjToCapnp(getWorker()));
    return kj::READY_NOW;
  }

  kj::Promise<void> jsRpcSession(JsRpcSessionContext context) override {
    return api::JsRpcSessionCustomEvent::receiveRpc(context, getWorker());
  }

  kj::Promise<void> restoreService(RestoreServiceContext context) override {
    // The next hop of a restore chain: the caller wants to invoke `[restore]()` on the service
    // that our own restore event produced (e.g. the token's base is an actor whose `[restore]()`
    // returned a facet, and now the facet's own `[restore]()` must be invoked).
    auto worker = getWorker();
    auto& workerRef = *worker;
    return RestoreServiceCustomEvent::receiveRpc(
        context, httpOverCapnpFactory, paramsHandler.addRef(), workerRef, kj::mv(worker));
  }

  kj::Promise<void> restoreRpcStub(RestoreRpcStubContext context) override {
    // The final hop of a restore chain whose result is an RpcStub rather than a service.
    auto worker = getWorker();
    auto& workerRef = *worker;
    return RestoreRpcStubCustomEvent::receiveRpc(
        context, paramsHandler.addRef(), workerRef, kj::mv(worker));
  }

 private:
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  kj::Maybe<kj::Own<IoChannelFactory::SubrequestChannel>> service;
  kj::Maybe<kj::String> cfBlobJson;
  Persistent fromPersistentStub;
  kj::Rc<RestoreParamsHandler> paramsHandler;

  kj::Own<WorkerInterface> getWorker() {
    auto movedService =
        kj::mv(KJ_ASSERT_NONNULL(service, "EventDispatcher can only be used for one request"));

    IoChannelFactory::SubrequestMetadata metadata;
    metadata.cfBlobJson = kj::mv(cfBlobJson);
    // Set when the restore chain continues *through* the restored service (see the comment in the
    // sending code, e.g. edgeworker's `RestoredSubrequestChannel::startRequest()`): the next hop's
    // `[restore]()` params came from storage, so the target worker's opt-in to stub storage must
    // be re-verified.
    metadata.fromPersistentStub = fromPersistentStub;
    auto worker = movedService->startRequest(kj::mv(metadata));

    return worker.attach(kj::mv(movedService));
  }
};

// WorkerdBootstrap server returned by `restoreService()` over RPC. Each `startEvent()` starts a
// fresh request against the restored service channel. Also owns the `[restore]()` event's running
// promise so that the restoration stays alive as long as the client holds this bootstrap.
class RestoredServiceBootstrap final: public rpc::WorkerdBootstrap::Server {
 public:
  RestoredServiceBootstrap(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      kj::Own<IoChannelFactory::SubrequestChannel> service,
      kj::Promise<void> eventTask,
      kj::Rc<RestoreParamsHandler> paramsHandler)
      : httpOverCapnpFactory(httpOverCapnpFactory),
        eventTask(eventTask.eagerlyEvaluate(nullptr)),
        service(kj::mv(service)),
        paramsHandler(kj::mv(paramsHandler)) {}

  kj::Promise<void> startEvent(StartEventContext context) override {
    kj::Maybe<kj::String> cfBlobJson;
    auto params = context.getParams();
    if (params.hasCfBlobJson()) {
      cfBlobJson = kj::str(params.getCfBlobJson());
    }
    context.initResults(capnp::MessageSize{4, 1})
        .setDispatcher(kj::heap<RestoredServiceEventDispatcher>(httpOverCapnpFactory,
            kj::addRef(*service), kj::mv(cfBlobJson), Persistent(params.getFromPersistentStub()),
            paramsHandler.addRef()));
    return kj::READY_NOW;
  }

 private:
  capnp::HttpOverCapnpFactory& httpOverCapnpFactory;
  kj::Promise<void> eventTask;
  kj::Own<IoChannelFactory::SubrequestChannel> service;
  kj::Rc<RestoreParamsHandler> paramsHandler;
};

}  // namespace

tracing::EventInfo RestoreServiceCustomEvent::getEventInfo() const {
  return tracing::CustomEventInfo();
}

kj::Promise<WorkerInterface::CustomEvent::Result> RestoreServiceCustomEvent::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName,
    kj::Maybe<Worker::VersionInfo> versionInfo,
    Frankenvalue props,
    kj::TaskSet& waitUntilTasks,
    bool isDynamicDispatch) {
  IoContext& ioctx = incomingRequest->getContext();

  incomingRequest->delivered();

  KJ_DEFER({ incomingRequest->drain(waitUntilTasks, kj::mv(incomingRequest)); });

  KJ_TRY {
    auto channel =
        co_await ioctx.run([this, entrypointName = entrypointName,
                               versionInfo = kj::mv(versionInfo), props = kj::mv(props),
                               isDynamicDispatch](Worker::Lock& lock, IoContext& ioctx) mutable {
      // Now that we're inside the IoContext, rehydrate any cap-table entries in the params (e.g.
      // in the edge runtime, turn dehydrated channel tokens into live channels). See
      // RestoreRehydrateCallback.
      KJ_IF_SOME(rehydrate, rehydrateCaps) {
        restoreParams.rewriteCaps([&rehydrate](kj::Own<Frankenvalue::CapTableEntry> cap) {
          return rehydrate(kj::mv(cap));
        });
      }

      auto params = restoreParams.toJs(lock);
      auto handler = JSG_REQUIRE_NONNULL(
          lock.getExportedHandler(entrypointName, kj::mv(versionInfo), kj::mv(props),
              ioctx.getActor(), isDynamicDispatch),
          Error,
          "This worker previously implemented a [restore]() method, but is now a Service Worker, "
          "which can't.");
      auto target = jsg::JsObject(handler->self.getHandle(lock));

      return ioctx.awaitJs(lock,
          invokeRestoreAndCoerce(lock, target, params)
              .then(lock, [&ioctx](jsg::Lock& js, RestoreResult result) {
        KJ_SWITCH_ONEOF(result) {
          KJ_CASE_ONEOF(fetcher, jsg::Ref<Fetcher>) {
            return fetcher->getSubrequestChannel(ioctx);
          }
          KJ_CASE_ONEOF(stub, jsg::Ref<JsRpcStub>) {
            // TODO(someday): Arguably we could allow this case and let people "upgrade" an RpcStub
            //   to a ServiceStub?
            JSG_FAIL_REQUIRE(TypeError,
                "The [restore]() method originally returned a ServiceStub, but on replay it "
                "returned an RpcStub instead. It must always return the same type.");
          }
        }
        KJ_UNREACHABLE;
      }));
    });

    auto [donePromise, doneFulfiller] = kj::newPromiseAndFulfiller<void>();

    channelFulfiller->fulfill(
        kj::refcounted<LifetimeExtendedSubrequestChannel>(kj::mv(channel), kj::mv(doneFulfiller)));

    // Keep the restore event's IoContext alive as long as the restored service channel exists.
    co_await donePromise.exclusiveJoin(ioctx.onAbort());
    KJ_IF_SOME(t, ioctx.getWorkerTracer()) {
      t.setReturn(ioctx.now());
    }

    co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
  }
  KJ_CATCH(exception) {
    incomingRequest->getMetrics().reportFailure(exception);
    channelFulfiller->reject(kj::mv(exception));

    co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::EXCEPTION};
  }
}

kj::Promise<WorkerInterface::CustomEvent::Result> RestoreServiceCustomEvent::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    FrankenvalueHandler& frankenvalueHandler,
    rpc::EventDispatcher::Client dispatcher) {
  auto req = dispatcher.restoreServiceRequest();
  frankenvalueHandler.toCapnp(restoreParams, req.initParams());
  auto sent = req.send();

  channelFulfiller->fulfill(kj::refcounted<RestoredRpcSubrequestChannel>(
      httpOverCapnpFactory, byteStreamFactory, frankenvalueHandler, sent.getService()));

  co_await sent;

  co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
}

kj::Promise<void> RestoreServiceCustomEvent::receiveRpc(RestoreServiceContext context,
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    kj::Rc<RestoreParamsHandler> paramsHandler,
    WorkerInterface& worker,
    kj::Own<void> ownWorker) {
  auto event = kj::heap<RestoreServiceCustomEvent>(RESTORE_SERVICE_EVENT_TYPE,
      paramsHandler->fromCapnp(context.getParams().getParams()),
      paramsHandler->makeRehydrateCallback());

  // Grab the (promised) restored channel off the event before dispatching it.
  auto channel = event->getChannel();

  // Dispatch the event to the worker. The event runs `[restore]()` and fulfills `channel`. We
  // keep the event's promise running (and the worker alive) as long as the returned bootstrap is
  // held, by storing it inside the bootstrap.
  auto eventTask = worker.customEvent(kj::mv(event)).ignoreResult().attach(kj::mv(ownWorker));

  context.initResults(capnp::MessageSize{4, 2})
      .setService(kj::heap<RestoredServiceBootstrap>(
          httpOverCapnpFactory, kj::mv(channel), kj::mv(eventTask), kj::mv(paramsHandler)));

  return kj::READY_NOW;
}

// -----------------------------------------------------------------------------

tracing::EventInfo RestoreRpcStubCustomEvent::getEventInfo() const {
  return tracing::CustomEventInfo();
}

kj::Promise<WorkerInterface::CustomEvent::Result> RestoreRpcStubCustomEvent::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName,
    kj::Maybe<Worker::VersionInfo> versionInfo,
    Frankenvalue props,
    kj::TaskSet& waitUntilTasks,
    bool isDynamicDispatch) {
  IoContext& ioctx = incomingRequest->getContext();

  incomingRequest->delivered();

  KJ_DEFER({
    // waitUntil() should allow extending execution on the server side even when the client
    // disconnects.
    incomingRequest->drain(waitUntilTasks, kj::mv(incomingRequest));
  });

  KJ_TRY {
    auto cap =
        co_await ioctx.run([this, entrypointName = entrypointName,
                               versionInfo = kj::mv(versionInfo), props = kj::mv(props),
                               isDynamicDispatch](Worker::Lock& lock, IoContext& ioctx) mutable {
      // Now that we're inside the IoContext, rehydrate any cap-table entries in the params (e.g.
      // in the edge runtime, turn dehydrated channel tokens into live channels). See
      // RestoreRehydrateCallback.
      KJ_IF_SOME(rehydrate, rehydrateCaps) {
        restoreParams.rewriteCaps([&rehydrate](kj::Own<Frankenvalue::CapTableEntry> cap) {
          return rehydrate(kj::mv(cap));
        });
      }

      auto params = restoreParams.toJs(lock);
      auto handler =
          JSG_REQUIRE_NONNULL(lock.getExportedHandler(entrypointName, kj::mv(versionInfo),
                                  kj::mv(props), ioctx.getActor(), isDynamicDispatch),
              Error,
              "This worker previously implemented a [restore]() method, but is now a Service "
              "Worker, which can't.");
      auto target = jsg::JsObject(handler->self.getHandle(lock));

      return ioctx.awaitJs(lock,
          invokeRestoreAndCoerce(lock, target, params)
              .then(lock, [](jsg::Lock& js, RestoreResult result) -> rpc::JsRpcTarget::Client {
        KJ_SWITCH_ONEOF(result) {
          KJ_CASE_ONEOF(fetcher, jsg::Ref<Fetcher>) {
            JSG_FAIL_REQUIRE(TypeError,
                "The [restore]() method originally returned an RpcStub, but on replay it "
                "returned a ServiceStub instead. It must always return the same type.");
          }
          KJ_CASE_ONEOF(stub, jsg::Ref<JsRpcStub>) {
            auto client = stub->getClient();
            stub->dispose();
            return client;
          }
        }
        KJ_UNREACHABLE;
      }));
    });

    auto [donePromise, doneFulfiller] = kj::newPromiseAndFulfiller<void>();

    capFulfiller->fulfill(
        capnp::membrane(kj::mv(cap), kj::refcounted<CompletionMembrane>(kj::mv(doneFulfiller))));

    // `donePromise` resolves once there are no longer any capabilities pointing between the client
    // and server as part of this session.
    co_await donePromise.exclusiveJoin(ioctx.onAbort());
    KJ_IF_SOME(t, ioctx.getWorkerTracer()) {
      t.setReturn(ioctx.now());
    }

    co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
  }
  KJ_CATCH(exception) {
    incomingRequest->getMetrics().reportFailure(exception);
    capFulfiller->reject(kj::mv(exception));

    co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::EXCEPTION};
  }
}

kj::Promise<WorkerInterface::CustomEvent::Result> RestoreRpcStubCustomEvent::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    FrankenvalueHandler& frankenvalueHandler,
    rpc::EventDispatcher::Client dispatcher) {
  // This contains a lot of similar code to JsRpcSessionCustomEvent::sendRpc() for setting
  // up membranes, handling the returned JsRpcSession, etc.

  auto revokePaf = kj::newPromiseAndFulfiller<void>();
  KJ_DEFER({
    if (revokePaf.fulfiller->isWaiting()) {
      revokePaf.fulfiller->reject(KJ_EXCEPTION(DISCONNECTED, "JS-RPC-restore session canceled"));
    }
  });

  auto req = dispatcher.restoreRpcStubRequest();
  frankenvalueHandler.toCapnp(restoreParams, req.initParams());
  auto sent = req.send();

  rpc::JsRpcTarget::Client cap = sent.getTarget();

  cap = capnp::membrane(kj::mv(cap), kj::refcounted<RevokerMembrane>(kj::mv(revokePaf.promise)));

  auto completionPaf = kj::newPromiseAndFulfiller<void>();
  cap = capnp::membrane(
      kj::mv(cap), kj::refcounted<CompletionMembrane>(kj::mv(completionPaf.fulfiller)));

  capFulfiller->fulfill(kj::mv(cap));

  auto session = sent.getSession();
  { auto drop = kj::mv(sent); }
  try {
    co_await session.whenResolved().exclusiveJoin(kj::mv(completionPaf.promise));
  } catch (...) {
    auto e = kj::getCaughtExceptionAsKj();
    if (revokePaf.fulfiller->isWaiting()) {
      revokePaf.fulfiller->reject(e.clone());
    }
    kj::throwFatalException(kj::mv(e));
  }

  co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
}

kj::Promise<void> RestoreRpcStubCustomEvent::receiveRpc(RestoreRpcStubContext context,
    kj::Rc<RestoreParamsHandler> paramsHandler,
    WorkerInterface& worker,
    kj::Own<void> ownWorker) {
  auto event = kj::heap<RestoreRpcStubCustomEvent>(RESTORE_RPC_STUB_EVENT_TYPE,
      paramsHandler->fromCapnp(context.getParams().getParams()),
      paramsHandler->makeRehydrateCallback());

  // Modeled on JsRpcSessionCustomEvent::receiveRpc(): dispatch the event, read the target cap off
  // it, and tie the returned session to the event's completion.
  auto cap = event->getCap();

  auto promise = worker.customEvent(kj::mv(event));

  auto results = context.getResults(capnp::MessageSize{4, 2});
  results.setTarget(kj::mv(cap));

  // The returned session capability keeps the session alive; dropping it cancels the event.
  results.setSession(promise.then([ownWorker = kj::mv(ownWorker)](auto outcome) {
    return rpc::JsRpcSession::Client(nullptr);
  }));

  return kj::READY_NOW;
}

};  // namespace workerd::api
