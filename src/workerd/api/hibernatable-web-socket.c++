// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "hibernatable-web-socket.h"
#include <workerd/api/global-scope.h>
#include <workerd/jsg/ser.h>
#include <workerd/io/hibernation-manager.h>

namespace workerd::api {

HibernatableWebSocketEvent::HibernatableWebSocketEvent()
    : ExtendableEvent("webSocketMessage") {};

Worker::Actor::HibernationManager& HibernatableWebSocketEvent::getHibernationManager(
    jsg::Lock& lock) {
  auto& actor = KJ_REQUIRE_NONNULL(IoContext::current().getActor());
  return KJ_REQUIRE_NONNULL(actor.getHibernationManager());
}

HibernatableWebSocketEvent::ItemsForRelease HibernatableWebSocketEvent::prepareForRelease(
    jsg::Lock &lock, kj::StringPtr websocketId) {
  auto& manager = kj::downcast<HibernationManagerImpl>(getHibernationManager(lock));
  auto& hibernatableWebSocket = KJ_REQUIRE_NONNULL(
      manager.webSocketsForEventHandler.findEntry(websocketId));

  // Note that we don't call `claimWebSocket()` to get this, since we would lose our reference to
  // the HibernatableWebSocket (it removes it from `webSocketsForEventHandler`).
  auto websocketRef = hibernatableWebSocket.value->getActiveOrUnhibernate(lock);
  auto ownedWebSocket = kj::mv(KJ_REQUIRE_NONNULL(hibernatableWebSocket.value->ws));

  // Now that we've obtained the websocket for the event, let's free up the slots we had allocated.
  manager.webSocketsForEventHandler.erase(hibernatableWebSocket);

  return ItemsForRelease(kj::mv(websocketRef), kj::mv(ownedWebSocket));
}

jsg::Ref<WebSocket> HibernatableWebSocketEvent::claimWebSocket(jsg::Lock& lock,
    kj::StringPtr websocketId) {
  // Should only be called once per event since it removes the HibernatableWebSocket from the
  // webSocketsForEventHandler collection.
  auto& manager = kj::downcast<HibernationManagerImpl>(getHibernationManager(lock));

  // Grab it from our collection.
  auto& hibernatableWebSocket = KJ_REQUIRE_NONNULL(
      manager.webSocketsForEventHandler.findEntry(websocketId));

  // Get the reference.
  auto websocket = hibernatableWebSocket.value->getActiveOrUnhibernate(lock);

  // Now that we've obtained the websocket, we need to remove the entry from the map and make the
  // key available again.
  manager.webSocketsForEventHandler.erase(hibernatableWebSocket);

  return kj::mv(websocket);
}

kj::Promise<WorkerInterface::CustomEvent::Result> HibernatableWebSocketCustomEventImpl::run(
    kj::Own<IoContext_IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName) {
  // Mark the request as delivered because we're about to run some JS.
  auto& context = incomingRequest->getContext();
  incomingRequest->delivered();
  EventOutcome outcome = EventOutcome::OK;

  // We definitely have an actor by this point. Let's set the hibernation manager on the actor
  // before we start running any events that might need to access it.
  auto& a = KJ_REQUIRE_NONNULL(context.getActor());
  if (a.getHibernationManager() == kj::none) {
    a.setHibernationManager(kj::addRef(KJ_REQUIRE_NONNULL(manager)));
  }

  try {
    co_await context.run(
        [entrypointName=entrypointName, &context, eventParameters=consumeParams()]
        (Worker::Lock& lock) mutable {
      KJ_SWITCH_ONEOF(eventParameters.eventType) {
        KJ_CASE_ONEOF(text, HibernatableSocketParams::Text) {
          return lock.getGlobalScope().sendHibernatableWebSocketMessage(
              kj::mv(text.message),
              kj::mv(eventParameters.websocketId),
              lock,
              lock.getExportedHandler(entrypointName, context.getActor()));
        }
        KJ_CASE_ONEOF(data, HibernatableSocketParams::Data) {
          return lock.getGlobalScope().sendHibernatableWebSocketMessage(
              kj::mv(data.message),
              kj::mv(eventParameters.websocketId),
              lock,
              lock.getExportedHandler(entrypointName, context.getActor()));
        }
        KJ_CASE_ONEOF(close, HibernatableSocketParams::Close) {
          return lock.getGlobalScope().sendHibernatableWebSocketClose(
              kj::mv(close),
              kj::mv(eventParameters.websocketId),
              lock,
              lock.getExportedHandler(entrypointName, context.getActor()));
        }
        KJ_CASE_ONEOF(e, HibernatableSocketParams::Error) {
          return lock.getGlobalScope().sendHibernatableWebSocketError(
              kj::mv(e.error),
              kj::mv(eventParameters.websocketId),
              lock,
              lock.getExportedHandler(entrypointName, context.getActor()));
        }
        KJ_UNREACHABLE;
      }
    });
  } catch(kj::Exception e) {
    if (auto desc = e.getDescription();
        !jsg::isTunneledException(desc) && !jsg::isDoNotLogException(desc)) {
      LOG_EXCEPTION("HibernatableWebSocketCustomEventImpl"_kj, e);
    }
    outcome = EventOutcome::EXCEPTION;
  }

  waitUntilTasks.add(incomingRequest->drain().attach(kj::mv(incomingRequest)));

  co_return Result {
    .outcome = outcome,
  };
}

kj::Promise<WorkerInterface::CustomEvent::Result>
  HibernatableWebSocketCustomEventImpl::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    kj::TaskSet& waitUntilTasks,
    rpc::EventDispatcher::Client dispatcher) {
  auto req = dispatcher.castAs<
      rpc::HibernatableWebSocketEventDispatcher>().hibernatableWebSocketEventRequest();

  KJ_IF_SOME(rpcParameters, params.tryGet<kj::Own<HibernationReader>>()) {
    req.setMessage(rpcParameters->getMessage());
  } else {
    auto message = req.initMessage();
    auto payload = message.initPayload();
    auto& eventParameters = KJ_REQUIRE_NONNULL(params.tryGet<HibernatableSocketParams>());
    KJ_SWITCH_ONEOF(eventParameters.eventType) {
      KJ_CASE_ONEOF(text, HibernatableSocketParams::Text) {
        payload.setText(kj::mv(text.message));
      }
      KJ_CASE_ONEOF(data, HibernatableSocketParams::Data) {
        payload.setData(kj::mv(data.message));
      }
      KJ_CASE_ONEOF(close, HibernatableSocketParams::Close) {
        auto closeBuilder = payload.initClose();
        closeBuilder.setCode(close.code);
        closeBuilder.setReason(kj::mv(close.reason));
        closeBuilder.setWasClean(close.wasClean);
      }
      KJ_CASE_ONEOF(e, HibernatableSocketParams::Error) {
        payload.setError(e.error.getDescription());
      }
      KJ_UNREACHABLE;
    }
    message.setWebsocketId(kj::mv(eventParameters.websocketId));
  }

  return req.send().then([](auto resp) {
    auto respResult = resp.getResult();
    return WorkerInterface::CustomEvent::Result {
      .outcome = respResult.getOutcome(),
    };
  });
}

HibernatableWebSocketEvent::ItemsForRelease::ItemsForRelease(
    jsg::Ref<WebSocket> ref, kj::Own<kj::WebSocket> owned)
    : webSocketRef(kj::mv(ref)), ownedWebSocket(kj::mv(owned)) {}

HibernatableWebSocketCustomEventImpl::HibernatableWebSocketCustomEventImpl(
    uint16_t typeId,
    kj::TaskSet& waitUntilTasks,
    kj::Own<HibernationReader> params,
    kj::Maybe<Worker::Actor::HibernationManager&> manager)
  : typeId(typeId), waitUntilTasks(waitUntilTasks), params(kj::mv(params)) {}
HibernatableWebSocketCustomEventImpl::HibernatableWebSocketCustomEventImpl(
    uint16_t typeId,
    kj::TaskSet& waitUntilTasks,
    HibernatableSocketParams params,
    Worker::Actor::HibernationManager& manager)
  : typeId(typeId), waitUntilTasks(waitUntilTasks), params(kj::mv(params)), manager(manager) {}

HibernatableSocketParams HibernatableWebSocketCustomEventImpl::consumeParams() {
  KJ_IF_SOME(p, params.tryGet<kj::Own<HibernationReader>>()) {
    kj::Maybe<HibernatableSocketParams> eventParameters;
    auto websocketId = kj::str(p->getMessage().getWebsocketId());
    auto payload = p->getMessage().getPayload();
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

}  // namespace workerd::api
