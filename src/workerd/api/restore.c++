// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "restore.h"

#include "global-scope.h"
#include "http.h"
#include "worker-rpc.h"

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

    KJ_SWITCH_ONEOF(result) {
      KJ_CASE_ONEOF(fetcher, jsg::Ref<Fetcher>) {
        auto baseChannel = fetcher->getSubrequestChannel(ioctx);
        auto channel = factory.makeRestoredSubrequestChannel(
            kj::addRef(*selfTokenFactory), kj::mv(restoreParams), kj::mv(baseChannel));
        auto restored = js.alloc<Fetcher>(ioctx.addObject(kj::mv(channel)));
        return jsg::Value(js.v8Isolate, fetcherHandler.wrap(js, kj::mv(restored)));
      }
      KJ_CASE_ONEOF(stub, jsg::Ref<JsRpcStub>) {
        auto channel =
            factory.makeRestoredRpcChannel(kj::addRef(*selfTokenFactory), kj::mv(restoreParams));
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
    auto channel = co_await ioctx.run(
        [this, entrypointName = entrypointName, &ioctx, versionInfo = kj::mv(versionInfo),
            props = kj::mv(props), isDynamicDispatch](Worker::Lock& lock) mutable {
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

    channelFulfiller->fulfill(kj::mv(channel));

    co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
  }
  KJ_CATCH(exception) {
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
  restoreParams.toCapnp(req.initParams());
  auto sent = req.send();

  channelFulfiller->fulfill(kj::refcounted<RestoredRpcSubrequestChannel>(
      httpOverCapnpFactory, byteStreamFactory, frankenvalueHandler, sent.getService()));

  co_await sent.ignoreResult();

  co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
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
    auto cap = co_await ioctx.run(
        [this, entrypointName = entrypointName, &ioctx, versionInfo = kj::mv(versionInfo),
            props = kj::mv(props), isDynamicDispatch](Worker::Lock& lock) mutable {
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

    co_return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
  }
  KJ_CATCH(exception) {
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
  restoreParams.toCapnp(req.initParams());
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

};  // namespace workerd::api
