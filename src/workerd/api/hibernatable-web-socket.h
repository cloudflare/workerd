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
struct HibernatableWebSocketCustomEventTestAccess;
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
};

class HibernatableWebSocketCustomEvent final: public WorkerInterface::CustomEvent,
                                              public kj::Refcounted {
 public:
  // The event carries no manager reference: the hibernation manager that owns the socket is found
  // from the event's WebSocket ID when the event runs. An RPC-delivered event could not carry a C++
  // reference anyway, and a local one cannot know whether the actor it reaches still uses the same
  // manager.
  HibernatableWebSocketCustomEvent(uint16_t typeId, kj::Own<HibernationReader> params);
  HibernatableWebSocketCustomEvent(uint16_t typeId, HibernatableSocketParams params);

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
  friend struct HibernatableWebSocketCustomEventTestAccess;

  // Returns `params`, but if we have a HibernationReader we convert it to a
  // HibernatableSocketParams first.
  HibernatableSocketParams consumeParams();

  // Checks that the manager owning `websocketId` is reachable before any handler reaches for it, and
  // installs it on the actor when the actor has none -- as a fresh actor, including the one a
  // code-update wake creates, may be. Throws if the actor already holds a different manager, which
  // means the event reached an actor that does not own the socket.
  void ensureHibernationManagerForEvent(Worker::Actor& actor, kj::StringPtr websocketId);

  // Peeks at params to extract the event type for tracing, without consuming them.
  tracing::HibernatableWebSocketEventInfo::Type getEventType() const;

  uint16_t typeId;
  kj::OneOf<HibernatableSocketParams, kj::Own<HibernationReader>> params;
  kj::Maybe<uint32_t> timeoutMs;
};

#define EW_WEB_SOCKET_MESSAGE_ISOLATE_TYPES                                                        \
  api::HibernatableWebSocketEvent, api::HibernatableWebSocketExportedHandler
}  // namespace workerd::api
