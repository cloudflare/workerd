// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/basics.h>
#include <workerd/api/hibernation-event-params.h>
#include <workerd/api/web-socket.h>
#include <workerd/io/trace.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>

#include <kj/debug.h>

namespace workerd::api {

using HibernationReader =
    rpc::HibernatableWebSocketEventDispatcher::HibernatableWebSocketEventParams::Reader;
class HibernatableWebSocketEvent final: public ExtendableEvent {
 public:
  explicit HibernatableWebSocketEvent();

  static jsg::Ref<HibernatableWebSocketEvent> constructor(kj::String type) = delete;

  // The manager's tags are free to go away after the final close or error event is dispatched, so
  // copy them into the api::WebSocket that receives the event.
  struct ItemsForRelease {
    jsg::Ref<WebSocket> webSocketRef;
    kj::Array<kj::String> tags;

    explicit ItemsForRelease(jsg::Ref<WebSocket> ref, kj::Array<kj::String> tags);
  };

  // Gets a reference to the api::WebSocket and clones the tags owned by the HibernationManager.
  ItemsForRelease prepareForRelease(jsg::Lock& lock, kj::StringPtr websocketId);

  // Should only be called once per event, see definition for details.
  jsg::Ref<WebSocket> claimWebSocket(jsg::Lock& lock, kj::StringPtr websocketId);

  JSG_RESOURCE_TYPE(HibernatableWebSocketEvent) {
    JSG_INHERIT(ExtendableEvent);
  }

 private:
  Worker::Actor::HibernationManager& getHibernationManager(jsg::Lock& lock);
};

class HibernatableWebSocketCustomEvent final: public WorkerInterface::CustomEvent,
                                              public kj::Refcounted {
 public:
  HibernatableWebSocketCustomEvent(uint16_t typeId,
      kj::Own<HibernationReader> params,
      kj::Maybe<Worker::Actor::HibernationManager&> manager = kj::none);
  HibernatableWebSocketCustomEvent(
      uint16_t typeId, HibernatableSocketParams params, Worker::Actor::HibernationManager& manager);

  kj::Promise<Result> run(kj::Own<IoContext_IncomingRequest> incomingRequest,
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

  kj::Promise<Result> notSupported() override {
    KJ_UNIMPLEMENTED("hibernatable web socket event not supported");
  }

 private:
  // Returns `params`, but if we have a HibernationReader we convert it to a
  // HibernatableSocketParams first.
  HibernatableSocketParams consumeParams();

  // Peeks at params to extract the event type for tracing, without consuming them.
  tracing::HibernatableWebSocketEventInfo::Type getEventType() const;

  uint16_t typeId;
  kj::OneOf<HibernatableSocketParams, kj::Own<HibernationReader>> params;
  kj::Maybe<uint32_t> timeoutMs;
  kj::Maybe<Worker::Actor::HibernationManager&> manager;
};

#define EW_WEB_SOCKET_MESSAGE_ISOLATE_TYPES                                                        \
  api::HibernatableWebSocketEvent, api::HibernatableWebSocketExportedHandler
}  // namespace workerd::api
