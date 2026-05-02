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
      kj::PromiseFulfillerPair<kj::Own<IoChannelFactory::SubrequestChannel>> paf =
          kj::newPromiseAndFulfiller<kj::Own<IoChannelFactory::SubrequestChannel>>())
      : channelFulfiller(kj::mv(paf.fulfiller)),
        channel(newPromisedChannel<IoChannelFactory::SubrequestChannel>(kj::mv(paf.promise))),
        typeId(typeId),
        restoreParams(kj::mv(restoreParams)) {}

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
};

class RestoreRpcStubCustomEvent final: public WorkerInterface::CustomEvent {
 public:
  RestoreRpcStubCustomEvent(uint16_t typeId,
      Frankenvalue restoreParams,
      kj::PromiseFulfillerPair<rpc::JsRpcTarget::Client> paf =
          kj::newPromiseAndFulfiller<rpc::JsRpcTarget::Client>())
      : capFulfiller(kj::mv(paf.fulfiller)),
        clientCap(kj::mv(paf.promise)),
        typeId(typeId),
        restoreParams(kj::mv(restoreParams)) {}

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

  // We give the two restore events the same event type since they are semantically very similar.
  static constexpr uint16_t RESTORE_RPC_STUB_EVENT_TYPE =
      RestoreServiceCustomEvent::RESTORE_SERVICE_EVENT_TYPE;

 private:
  kj::Own<kj::PromiseFulfiller<workerd::rpc::JsRpcTarget::Client>> capFulfiller;
  kj::Maybe<rpc::JsRpcTarget::Client> clientCap;
  uint16_t typeId;
  Frankenvalue restoreParams;
};

};  // namespace workerd::api
