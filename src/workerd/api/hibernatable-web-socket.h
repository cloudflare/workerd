// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/basics.h>
#include <workerd/api/hibernation-event-params.h>
#include <workerd/api/web-socket.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>

#include <kj/debug.h>
#include <kj/time.h>

namespace workerd::api {

using HibernationReader =
    rpc::HibernatableWebSocketEventDispatcher::HibernatableWebSocketEventParams::Reader;
class HibernatableWebSocketEvent final: public ExtendableEvent {
 public:
  explicit HibernatableWebSocketEvent();

  static jsg::Ref<HibernatableWebSocketEvent> constructor(kj::String type) = delete;

  // When we call a close or error event, we need to move the owned websocket and the tags back into
  // the api::WebSocket to extend their lifetimes. This is because the HibernatableWebSocket, which
  // has owned these things for the entire duration of the connection, is free to go away after we
  // dispatch the final event. JS may still want to access the underlying kj::WebSocket or the tags,
  // so we have to transfer ownership to JS-land.
  struct ItemsForRelease {
    jsg::Ref<WebSocket> webSocketRef;
    kj::Own<kj::WebSocket> ownedWebSocket;
    kj::Array<kj::String> tags;

    explicit ItemsForRelease(
        jsg::Ref<WebSocket> ref, kj::Own<kj::WebSocket> owned, kj::Array<kj::String> tags);
  };

  // Call this when transferring ownership of the kj::WebSocket and tags to the api::WebSocket.
  //
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
  HibernatableWebSocketCustomEventImpl(uint16_t typeId,
      kj::Own<HibernationReader> params,
      kj::Maybe<Worker::Actor::HibernationManager&> manager = kj::none);
  HibernatableWebSocketCustomEventImpl(
      uint16_t typeId, HibernatableSocketParams params, Worker::Actor::HibernationManager& manager);

  kj::Promise<Result> run(kj::Own<IoContext_IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName,
      Frankenvalue props,
      kj::TaskSet& waitUntilTasks) override;

  kj::Promise<Result> sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      rpc::EventDispatcher::Client dispatcher) override;

  uint16_t getType() override {
    return typeId;
  }

  kj::Promise<Result> notSupported() override {
    KJ_UNIMPLEMENTED("hibernatable web socket event not supported");
  }

 private:
  // Returns `params`, but if we have a HibernationReader we convert it to a
  // HibernatableSocketParams first.
  HibernatableSocketParams consumeParams();

  uint16_t typeId;
  kj::OneOf<HibernatableSocketParams, kj::Own<HibernationReader>> params;
  kj::Maybe<uint32_t> timeoutMs;
  kj::Maybe<Worker::Actor::HibernationManager&> manager;
};

#define EW_WEB_SOCKET_MESSAGE_ISOLATE_TYPES                                                        \
  api::HibernatableWebSocketEvent, api::HibernatableWebSocketExportedHandler
}  // namespace workerd::api
