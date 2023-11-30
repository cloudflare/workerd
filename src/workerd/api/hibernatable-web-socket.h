// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/debug.h>

#include <workerd/io/worker-interface.capnp.h>
#include <workerd/io/worker-interface.h>
#include <workerd/api/basics.h>
#include <workerd/api/web-socket.h>
#include <workerd/api/hibernation-event-params.h>

namespace workerd::api {

using HibernationReader =
    rpc::HibernatableWebSocketEventDispatcher::HibernatableWebSocketEventParams::Reader;
class HibernatableWebSocketEvent final: public ExtendableEvent {
public:
  explicit HibernatableWebSocketEvent();

  static jsg::Ref<HibernatableWebSocketEvent> constructor(kj::String type) = delete;

  // When we call a close or error event, we need to move the owned websocket back into the
  // api::WebSocket to extend its lifetime. The way we obtain the websocket from the
  // HibernationManager is somewhat fragile, so it's better if we group the reference and owned
  // websocket together.
  struct ItemsForRelease {
    jsg::Ref<WebSocket> webSocketRef;
    kj::Own<kj::WebSocket> ownedWebSocket;

    explicit ItemsForRelease(jsg::Ref<WebSocket> ref, kj::Own<kj::WebSocket> owned);
  };

  // Only call this once (when transferring ownership of the websocket back to the api::WebSocket).
  // Gets a reference to the api::WebSocket, and moves the owned kj::WebSocket out of the
  // HibernatableWebSocket whose event we are currently delivering.
  ItemsForRelease prepareForRelease(jsg::Lock& lock, kj::StringPtr websocketId);

  // Should only be called once per event, see definition for details.
  jsg::Ref<WebSocket> claimWebSocket(jsg::Lock& lock, kj::StringPtr websocketId);

  JSG_RESOURCE_TYPE(HibernatableWebSocketEvent) {
    JSG_INHERIT(ExtendableEvent);
  }
private:
  Worker::Actor::HibernationManager& getHibernationManager(jsg::Lock& lock);
};

class HibernatableWebSocketCustomEventImpl final: public WorkerInterface::CustomEvent,
    public kj::Refcounted {
public:
  HibernatableWebSocketCustomEventImpl(
      uint16_t typeId,
      kj::TaskSet& waitUntilTasks,
      kj::Own<HibernationReader> params,
      kj::Maybe<Worker::Actor::HibernationManager&> manager=kj::none);
  HibernatableWebSocketCustomEventImpl(
      uint16_t typeId,
      kj::TaskSet& waitUntilTasks,
      HibernatableSocketParams params,
      Worker::Actor::HibernationManager& manager);

  kj::Promise<Result> run(
      kj::Own<IoContext_IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName) override;

  kj::Promise<Result> sendRpc(
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      kj::TaskSet& waitUntilTasks,
      rpc::EventDispatcher::Client dispatcher) override;

  uint16_t getType() override {
    return typeId;
  }

private:
  // Returns `params`, but if we have a HibernationReader we convert it to a
  // HibernatableSocketParams first.
  HibernatableSocketParams consumeParams();

  uint16_t typeId;
  kj::TaskSet& waitUntilTasks;
  kj::OneOf<HibernatableSocketParams, kj::Own<HibernationReader>> params;
  kj::Maybe<Worker::Actor::HibernationManager&> manager;
};

#define EW_WEB_SOCKET_MESSAGE_ISOLATE_TYPES      \
  api::HibernatableWebSocketEvent,       \
  api::HibernatableWebSocketExportedHandler
}  // namespace workerd::api
