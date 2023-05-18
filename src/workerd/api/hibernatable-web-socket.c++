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

HibernatableWebSocketEvent::ItemsForRelease HibernatableWebSocketEvent::prepareForRelease(
    jsg::Lock &lock) {
  auto& actor = KJ_REQUIRE_NONNULL(IoContext::current().getActor());
  auto& manager = kj::downcast<HibernationManagerImpl>(
      KJ_REQUIRE_NONNULL(actor.getHibernationManager()));
  auto& hibernatableWebSocket = KJ_REQUIRE_NONNULL(manager.webSocketForEventHandler);

  // `getWebSocket()` requires `HibernatableWebSocket::ws` be non-null, so it must be called first.
  // The explicit ctor makes it less likely we accidentally move the owned websocket first.
  return ItemsForRelease(getWebSocket(lock), kj::mv(KJ_REQUIRE_NONNULL(hibernatableWebSocket.ws)));
}

jsg::Ref<WebSocket> HibernatableWebSocketEvent::getWebSocket(jsg::Lock& lock) {
  auto& actor = KJ_REQUIRE_NONNULL(IoContext::current().getActor());
  auto& manager = kj::downcast<HibernationManagerImpl>(
      KJ_REQUIRE_NONNULL(actor.getHibernationManager()));
  auto& hibernatableWebSocket = KJ_REQUIRE_NONNULL(manager.webSocketForEventHandler);
  return hibernatableWebSocket.getActiveOrUnhibernate(lock);
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
  if (a.getHibernationManager() == nullptr) {
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
              lock,
              lock.getExportedHandler(entrypointName, context.getActor()));
        }
        KJ_CASE_ONEOF(data, HibernatableSocketParams::Data) {
          return lock.getGlobalScope().sendHibernatableWebSocketMessage(
              kj::mv(data.message),
              lock,
              lock.getExportedHandler(entrypointName, context.getActor()));
        }
        KJ_CASE_ONEOF(close, HibernatableSocketParams::Close) {
          return lock.getGlobalScope().sendHibernatableWebSocketClose(
              kj::mv(close),
              lock,
              lock.getExportedHandler(entrypointName, context.getActor()));
        }
        KJ_CASE_ONEOF(e, HibernatableSocketParams::Error) {
          return lock.getGlobalScope().sendHibernatableWebSocketError(
              kj::mv(e.error),
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

  KJ_IF_MAYBE(rpcParameters, params.tryGet<kj::Own<HibernationReader>>()) {
    req.setMessage((*rpcParameters)->getMessage());
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
  }

  return req.send().then([](auto resp) {
    auto respResult = resp.getResult();
    return WorkerInterface::CustomEvent::Result {
      .outcome = respResult.getOutcome(),
    };
  });
}

}  // namespace workerd::api
