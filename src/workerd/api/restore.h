// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Code related to `ctx.restore()` and the `[restore]()` method.

#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

class Fetcher;
class JsRpcStub;

// Callback used to rehydrate (deserialize) the cap-table entries of a restore event's parameters
// once the target IoContext has been entered.
//
// In environments where Frankenvalue cap-table entries are stored in a "dehydrated" form that is
// not tied to any IoContext (e.g. the edge runtime, where they are channel tokens), a live channel
// cannot be constructed until the IoContext in which `[restore]()` runs actually exists. This
// callback is invoked via `Frankenvalue::rewriteCaps()` inside the event's `run()`, after entering
// the IoContext and before converting the parameters to JS, in order to turn each dehydrated entry
// into a live, IoContext-bound channel. If null, the parameters' caps are used as-is (this is the
// case in workerd, where caps are already live).
using RestoreRehydrateCallback =
    kj::Function<kj::Own<Frankenvalue::CapTableEntry>(kj::Own<Frankenvalue::CapTableEntry>)>;

// Abstract interface used when receiving restore events over RPC (i.e. implementing
// `EventDispatcher.restoreService()` / `restoreRpcStub()`). It provides the two embedder-specific
// operations needed to reconstruct a restore custom event from the RPC message:
//
// - deserializing the event's `params` Frankenvalue, whose cap-table encoding is
//   environment-specific (like `FrankenvalueHandler`, but for the receiving direction), and
// - constructing the `RestoreRehydrateCallback` (if any) to pass to the event.
//
// This is refcounted because the `service` capability returned by `restoreService()` must keep the
// handler alive in order to handle subsequent hops of a restore chain, which arrive as further
// `restoreService()` / `restoreRpcStub()` calls on that capability.
class RestoreParamsHandler: public kj::Refcounted {
 public:
  // Deserialize the `params` carried by a `restoreService()` / `restoreRpcStub()` RPC.
  virtual Frankenvalue fromCapnp(rpc::Frankenvalue::Reader params) = 0;

  // Construct the callback used to rehydrate the deserialized params' cap-table entries once the
  // target IoContext has been entered, or kj::none if the entries are already live.
  virtual kj::Maybe<RestoreRehydrateCallback> makeRehydrateCallback() = 0;
};

// Implementation of ctx.restore(). Invokes `[restore](params)` on the current entrypoint,
// constructs the appropriate channel token for the result, and returns a Fetcher or JsRpcStub
// imbued with that token.
jsg::Promise<jsg::Value> restoreCurrentEntrypoint(jsg::Lock& js,
    jsg::JsObject params,
    const jsg::TypeHandler<jsg::Ref<Fetcher>>& fetcherHandler,
    const jsg::TypeHandler<jsg::Ref<JsRpcStub>>& rpcStubHandler);

// Custom event that calls the `[restore]()` method of the entrypoint and expects that the result
// is a `Fetcher`. (When the result is expected to be an RpcStub instead, we use
// `RestoreRpcStubCustomEvent`.)
//
// This is used when following a restore chain in a channel token. Links in the chain other than
// the last are always expected to produce `Fetcher`s, and the last link may sometimes be a
// `Fetcher` as well (particularly when constructing a Dynamic Worker or DO Facet stub).
//
// This generally works similarly to `JsRpcSessionCustomEvent` except that instead of `getCap()` we
// have `getChannel()` returning a `SubrequestChannel`.
class RestoreServiceCustomEvent final: public WorkerInterface::CustomEvent {
 public:
  RestoreServiceCustomEvent(uint16_t typeId,
      Frankenvalue restoreParams,
      kj::Maybe<RestoreRehydrateCallback> rehydrateCaps = kj::none,
      kj::PromiseFulfillerPair<kj::Own<IoChannelFactory::SubrequestChannel>> paf =
          kj::newPromiseAndFulfiller<kj::Own<IoChannelFactory::SubrequestChannel>>())
      : channelFulfiller(kj::mv(paf.fulfiller)),
        channel(newPromisedChannel<IoChannelFactory::SubrequestChannel>(kj::mv(paf.promise))),
        typeId(typeId),
        restoreParams(kj::mv(restoreParams)),
        rehydrateCaps(kj::mv(rehydrateCaps)) {}

  ~RestoreServiceCustomEvent() noexcept(false) {
    if (channelFulfiller->isWaiting()) {
      channelFulfiller->reject(
          KJ_EXCEPTION(DISCONNECTED, "RestoreServiceCustomEvent was destroyed before completion"));
    }
  }

  kj::Promise<Result> run(kj::Own<IoContext::IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName,
      kj::Maybe<Worker::VersionInfo> versionInfo,
      Frankenvalue props,
      kj::TaskSet& waitUntilTasks,
      bool isDynamicDispatch) override;

  kj::Promise<Result> sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      FrankenvalueHandler& frankenvalueHandler,
      rpc::EventDispatcher::Client dispatcher) override;

  uint16_t getType() override {
    return typeId;
  }

  tracing::EventInfo getEventInfo() const override;

  // Get the (promised) SubrequestChannel representing the Fetcher that the restore method will
  // return when it runs.
  kj::Own<IoChannelFactory::SubrequestChannel> getChannel() {
    auto result = kj::mv(KJ_ASSERT_NONNULL(channel, "can only call getChannel() once"));
    channel = kj::none;
    return result;
  }

  kj::Promise<Result> notSupported() override {
    JSG_FAIL_REQUIRE(TypeError, "The receiver does not support restore");
  }

  void failed(const kj::Exception& e) override {
    channelFulfiller->reject(e.clone());
  }

  // Same as `EventDispatcher::Server::RestoreServiceContext` -- but that typedef is `protected`.
  using RestoreServiceContext = capnp::CallContext<rpc::EventDispatcher::RestoreServiceParams,
      rpc::EventDispatcher::RestoreServiceResults>;

  // Common implementation of `EventDispatcher::restoreService()`. Constructs the event (using
  // `paramsHandler` to deserialize the params carried by `context`), dispatches it to `worker`,
  // then returns a `WorkerdBootstrap` representing the restored service. That bootstrap also
  // accepts further `restoreService()` / `restoreRpcStub()` calls -- delivering subsequent hops of
  // a restore chain to the restored service -- so it holds on to `paramsHandler` for that purpose.
  // `ownWorker` is held alive for the duration of the dispatched event (e.g. the multi-use
  // dispatcher server).
  static kj::Promise<void> receiveRpc(RestoreServiceContext context,
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      kj::Rc<RestoreParamsHandler> paramsHandler,
      WorkerInterface& worker,
      kj::Own<void> ownWorker);

  // Like JsRpcSessionCustomEvent::WORKER_RPC_EVENT_TYPE.
  //
  // Similar to other custom events, we define this event ID in the internal codebase, but since
  // we don't create RestoreServiceCustomEvent from our internal code, we can't pass the event
  // type in -- so we hardcode it here.
  //
  // TODO(cleanup): Honestly why does this need to be passed to the constructor at all? Why store
  //   it as a member? Why not just hardcode everywhere?
  static constexpr uint16_t RESTORE_SERVICE_EVENT_TYPE = 13;

 private:
  kj::Own<kj::PromiseFulfiller<kj::Own<IoChannelFactory::SubrequestChannel>>> channelFulfiller;
  kj::Maybe<kj::Own<IoChannelFactory::SubrequestChannel>> channel;
  uint16_t typeId;
  Frankenvalue restoreParams;
  kj::Maybe<RestoreRehydrateCallback> rehydrateCaps;
};

class RestoreRpcStubCustomEvent final: public WorkerInterface::CustomEvent {
 public:
  RestoreRpcStubCustomEvent(uint16_t typeId,
      Frankenvalue restoreParams,
      kj::Maybe<RestoreRehydrateCallback> rehydrateCaps = kj::none,
      kj::PromiseFulfillerPair<rpc::JsRpcTarget::Client> paf =
          kj::newPromiseAndFulfiller<rpc::JsRpcTarget::Client>())
      : capFulfiller(kj::mv(paf.fulfiller)),
        clientCap(kj::mv(paf.promise)),
        typeId(typeId),
        restoreParams(kj::mv(restoreParams)),
        rehydrateCaps(kj::mv(rehydrateCaps)) {}

  ~RestoreRpcStubCustomEvent() noexcept(false) {
    if (capFulfiller->isWaiting()) {
      capFulfiller->reject(
          KJ_EXCEPTION(DISCONNECTED, "RestoreRpcStubCustomEvent was destroyed before completion"));
    }
  }

  kj::Promise<Result> run(kj::Own<IoContext::IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName,
      kj::Maybe<Worker::VersionInfo> versionInfo,
      Frankenvalue props,
      kj::TaskSet& waitUntilTasks,
      bool isDynamicDispatch) override;

  kj::Promise<Result> sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      FrankenvalueHandler& frankenvalueHandler,
      rpc::EventDispatcher::Client dispatcher) override;

  uint16_t getType() override {
    return typeId;
  }

  tracing::EventInfo getEventInfo() const override;

  // Get the (promised) JsRpcTarget::Client representing the stub that the restore method will
  // return when it runs.
  rpc::JsRpcTarget::Client getCap() {
    auto result = kj::mv(KJ_ASSERT_NONNULL(clientCap, "can only call getCap() once"));
    clientCap = kj::none;
    return result;
  }

  kj::Promise<Result> notSupported() override {
    JSG_FAIL_REQUIRE(TypeError, "The receiver does not support restore");
  }

  void failed(const kj::Exception& e) override {
    capFulfiller->reject(e.clone());
  }

  // Same as `EventDispatcher::Server::RestoreRpcStubContext` -- but that typedef is `protected`.
  using RestoreRpcStubContext = capnp::CallContext<rpc::EventDispatcher::RestoreRpcStubParams,
      rpc::EventDispatcher::RestoreRpcStubResults>;

  // Common implementation of `EventDispatcher::restoreRpcStub()`. Constructs the event (using
  // `paramsHandler` to deserialize the params carried by `context`), dispatches it to `worker`,
  // and hooks up the resulting `JsRpcTarget` + `session`, modeled on
  // `JsRpcSessionCustomEvent::receiveRpc()`. `ownWorker` is held alive until the session
  // completes.
  static kj::Promise<void> receiveRpc(RestoreRpcStubContext context,
      kj::Rc<RestoreParamsHandler> paramsHandler,
      WorkerInterface& worker,
      kj::Own<void> ownWorker);

  // We give the two restore events the same event type since they are semantically very similar.
  static constexpr uint16_t RESTORE_RPC_STUB_EVENT_TYPE =
      RestoreServiceCustomEvent::RESTORE_SERVICE_EVENT_TYPE;

 private:
  kj::Own<kj::PromiseFulfiller<workerd::rpc::JsRpcTarget::Client>> capFulfiller;
  kj::Maybe<rpc::JsRpcTarget::Client> clientCap;
  uint16_t typeId;
  Frankenvalue restoreParams;
  kj::Maybe<RestoreRehydrateCallback> rehydrateCaps;
};

};  // namespace workerd::api
