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

    explicit ItemsForRelease(jsg::Ref<WebSocket> ref, kj::Own<kj::WebSocket> owned)
        : webSocketRef(kj::mv(ref)), ownedWebSocket(kj::mv(owned)) {}
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
      kj::Maybe<Worker::Actor::HibernationManager&> manager=nullptr)
    : typeId(typeId), waitUntilTasks(waitUntilTasks), params(kj::mv(params)) {}
  HibernatableWebSocketCustomEventImpl(
      uint16_t typeId,
      kj::TaskSet& waitUntilTasks,
      HibernatableSocketParams params,
      Worker::Actor::HibernationManager& manager)
    : typeId(typeId), waitUntilTasks(waitUntilTasks), params(kj::mv(params)), manager(manager) {}

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
  HibernatableSocketParams consumeParams() {
    KJ_IF_MAYBE(p, params.tryGet<kj::Own<HibernationReader>>()) {
      kj::Maybe<HibernatableSocketParams> eventParameters;
      auto websocketId = kj::str((*p)->getMessage().getWebsocketId());
      auto payload = (*p)->getMessage().getPayload();
      switch(payload.which()) {
        case rpc::HibernatableWebSocketEventMessage::Payload::TEXT: {
          eventParameters.emplace(kj::str(payload.getText()), kj::mv(websocketId));
          break;
        }
        case rpc::HibernatableWebSocketEventMessage::Payload::DATA: {
          kj::Array<byte> b = kj::heapArray(payload.getData().asBytes());
          eventParameters.emplace(kj::mv(b), kj::mv(websocketId));
          break;
        }
        case rpc::HibernatableWebSocketEventMessage::Payload::CLOSE: {
          auto close = payload.getClose();
          eventParameters.emplace(
              close.getCode(),
              kj::str(close.getReason()),
              close.getWasClean(),
              kj::mv(websocketId));
          break;
        }
        case rpc::HibernatableWebSocketEventMessage::Payload::ERROR: {
          eventParameters.emplace(
              KJ_EXCEPTION(FAILED, kj::str(payload.getError())),
              kj::mv(websocketId));
          break;
        }
      }
      return kj::mv(KJ_REQUIRE_NONNULL(eventParameters));
    }
    return kj::mv(KJ_REQUIRE_NONNULL(params.tryGet<HibernatableSocketParams>()));
  }

  uint16_t typeId;
  kj::TaskSet& waitUntilTasks;
  kj::OneOf<HibernatableSocketParams, kj::Own<HibernationReader>> params;
  kj::Maybe<Worker::Actor::HibernationManager&> manager;
};

#define EW_WEB_SOCKET_MESSAGE_ISOLATE_TYPES      \
  api::HibernatableWebSocketEvent,       \
  api::HibernatableWebSocketExportedHandler
}  // namespace workerd::api
