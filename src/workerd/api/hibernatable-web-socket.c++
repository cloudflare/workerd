// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "hibernatable-web-socket.h"

#include <workerd/api/global-scope.h>
#include <workerd/io/legacy-hibernation-manager.h>
#include <workerd/io/limit-enforcer.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/ser.h>

namespace workerd::api {

namespace {

// Hibernatable sockets bypass the regular readLoop (which marks non-hibernatable receives), so we
// mark the receive here, in-scope, from both the Text and Data dispatch branches. The enforcer
// captures the timestamp, so this side stays time-agnostic.
void markHibernatableWebSocketReceive(IoContext& context) {
  context.getWorker().getIsolate().getLimitEnforcer().markPerfEvent("ws_received"_kjc);
}

}  // namespace

HibernatableWebSocketEvent::HibernatableWebSocketEvent(): ExtendableEvent("webSocketMessage") {};

HibernatableWebSocketEvent::ItemsForRelease HibernatableWebSocketEvent::prepareForRelease(
    jsg::Lock& lock, kj::StringPtr websocketId) {
  auto& hibernatableWebSocket = LegacyHibernationManagerImpl::takeWebSocketForEvent(websocketId);
  auto websocketRef = hibernatableWebSocket.getActiveOrUnhibernate(lock);
  auto ownedWebSocket = kj::mv(KJ_REQUIRE_NONNULL(hibernatableWebSocket.ws));
  auto tags = hibernatableWebSocket.cloneTags();

  return ItemsForRelease(kj::mv(websocketRef), kj::mv(ownedWebSocket), kj::mv(tags));
}

jsg::Ref<WebSocket> HibernatableWebSocketEvent::claimWebSocket(
    jsg::Lock& lock, kj::StringPtr websocketId) {
  // Should only be called once per event since it deregisters the event's HibernatableWebSocket.
  auto& hibernatableWebSocket = LegacyHibernationManagerImpl::takeWebSocketForEvent(websocketId);
  return hibernatableWebSocket.getActiveOrUnhibernate(lock);
}

kj::Promise<WorkerInterface::CustomEvent::Result> HibernatableWebSocketCustomEvent::run(
    kj::Own<IoContext_IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName,
    kj::Maybe<Worker::VersionInfo> versionInfo,
    Frankenvalue props,
    kj::TaskSet& waitUntilTasks,
    bool isDynamicDispatch) {
  // Mark the request as delivered because we're about to run some JS.
  auto& context = incomingRequest->getContext();
  incomingRequest->delivered();

  KJ_DEFER({ incomingRequest->drain(waitUntilTasks, kj::mv(incomingRequest)); });

  EventOutcome outcome = EventOutcome::OK;

  auto eventParameters = consumeParams();

  // Deliberately outside the try below. Failing to route this event is not a failure of the
  // application's handler: the catch block would log it against user code and report the event as
  // merely having thrown, leaving the message dropped with nothing that says why.
  ensureHibernationManagerForEvent(
      KJ_REQUIRE_NONNULL(context.getActor()), eventParameters.websocketId);

  try {
    co_await context.run(
        [entrypointName = entrypointName, eventParameters = kj::mv(eventParameters),
            versionInfo = kj::mv(versionInfo), props = kj::mv(props),
            isDynamicDispatch](Worker::Lock& lock, IoContext& context) mutable {
      KJ_SWITCH_ONEOF(eventParameters.eventType) {
        KJ_CASE_ONEOF(text, HibernatableSocketParams::Text) {
          markHibernatableWebSocketReceive(context);
          return lock.getGlobalScope().sendHibernatableWebSocketMessage(context,
              kj::mv(text.message), eventParameters.eventTimeoutMs,
              kj::mv(eventParameters.websocketId), lock,
              lock.getExportedHandler(entrypointName, kj::mv(versionInfo), kj::mv(props),
                  context.getActor(), isDynamicDispatch));
        }
        KJ_CASE_ONEOF(data, HibernatableSocketParams::Data) {
          markHibernatableWebSocketReceive(context);
          return lock.getGlobalScope().sendHibernatableWebSocketMessage(context,
              kj::mv(data.message), eventParameters.eventTimeoutMs,
              kj::mv(eventParameters.websocketId), lock,
              lock.getExportedHandler(entrypointName, kj::mv(versionInfo), kj::mv(props),
                  context.getActor(), isDynamicDispatch));
        }
        KJ_CASE_ONEOF(close, HibernatableSocketParams::Close) {
          return lock.getGlobalScope().sendHibernatableWebSocketClose(context, kj::mv(close),
              eventParameters.eventTimeoutMs, kj::mv(eventParameters.websocketId), lock,
              lock.getExportedHandler(entrypointName, kj::mv(versionInfo), kj::mv(props),
                  context.getActor(), isDynamicDispatch));
        }
        KJ_CASE_ONEOF(e, HibernatableSocketParams::Error) {
          return lock.getGlobalScope().sendHibernatableWebSocketError(context, kj::mv(e.error),
              eventParameters.eventTimeoutMs, kj::mv(eventParameters.websocketId), lock,
              lock.getExportedHandler(entrypointName, kj::mv(versionInfo), kj::mv(props),
                  context.getActor(), isDynamicDispatch));
        }
        KJ_UNREACHABLE;
      }
    });
  } catch (kj::Exception& e) {
    if (auto desc = e.getDescription();
        !jsg::isTunneledException(desc) && !jsg::isDoNotLogException(desc)) {
      LOG_EXCEPTION("HibernatableWebSocketCustomEvent"_kj, e);
    }
    incomingRequest->getMetrics().reportFailure(e);
    context.logUncaughtExceptionAsync(UncaughtExceptionSource::ASYNC_TASK, e.clone());
    outcome = EventOutcome::EXCEPTION;
  }

  co_return Result{
    .outcome = outcome,
  };
}

void HibernatableWebSocketCustomEvent::ensureHibernationManagerForEvent(
    Worker::Actor& actor, kj::StringPtr websocketId) {
  // Resolve the manager before running any JS, so that an event that cannot be routed fails here
  // rather than from inside a handler that has already observed it.
  auto& manager = KJ_REQUIRE_NONNULL(LegacyHibernationManagerImpl::findManagerForEvent(websocketId),
      "hibernatable WebSocket event manager was not found for this event ID");

  // The adopt branch below has no manager of its own to compare against, so this is what stops an
  // actor adopting a manager another actor is still using -- and with it, that actor's sockets.
  //
  // Identity is checked twice because neither check covers the other. The ID outlives the owning
  // actor, so it still rejects an unrelated actor once a code update has destroyed the owner, which
  // is precisely when a replacement adopts the manager. The instance check then rejects a second
  // actor that is live at the same time as the owner and shares its ID -- the case the ID alone
  // would admit, and the one that would cross isolates.
  KJ_IF_SOME(ownerId, manager.getOwningActorId()) {
    KJ_REQUIRE(Worker::Actor::idsEqual(ownerId, actor.getId()),
        "hibernatable WebSocket event names a hibernation manager owned by a different actor");
  }
  KJ_IF_SOME(owner, manager.getOwningActor()) {
    KJ_REQUIRE(&owner == &actor,
        "hibernatable WebSocket event names a hibernation manager owned by a different live actor");
  }

  KJ_IF_SOME(existing, actor.getHibernationManager()) {
    // The ID is looked up in a registry shared by every manager on the event loop, so it is the one
    // part of an event that does not come from this actor. Requiring that it name the manager this
    // actor already uses is what keeps an ID from reaching an actor that does not own the socket;
    // before the ID existed, the lookup happened in this actor's own manager and could not stray.
    //
    // A code-update wake does not need the mismatching case: the replacement actor is handed the
    // manager that has been holding its sockets, so by the time an event runs the two already
    // agree. An actor holding some other manager means the event was misrouted, or that the actor
    // built a second manager while this event was in flight -- and delivering anyway would strand
    // the socket in a manager the actor's own getWebSockets() cannot see.
    KJ_REQUIRE(&existing == &manager,
        "hibernatable WebSocket event ID names a different hibernation manager than the receiving "
        "actor's");
  } else {
    // A fresh actor starts without a manager and has to adopt the one holding the socket this event
    // arrived on. Otherwise it would build a second manager on its first acceptWebSocket() call and
    // split the actor's sockets across the two.
    actor.setHibernationManager(manager.addRef());
  }
}

kj::Promise<WorkerInterface::CustomEvent::Result> HibernatableWebSocketCustomEvent::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    FrankenvalueHandler& frankenvalueHandler,
    rpc::EventDispatcher::Client dispatcher) {
  auto req = dispatcher.castAs<rpc::HibernatableWebSocketEventDispatcher>()
                 .hibernatableWebSocketEventRequest();

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
    KJ_IF_SOME(t, eventParameters.eventTimeoutMs) {
      message.setEventTimeoutMs(t);
    }
  }

  // The registration this event relies on is owned by the task that is delivering it, which
  // deregisters the event once this promise settles either way.
  return req.send().then([](auto resp) {
    auto respResult = resp.getResult();
    return WorkerInterface::CustomEvent::Result{
      .outcome = respResult.getOutcome(),
    };
  });
}

HibernatableWebSocketEvent::ItemsForRelease::ItemsForRelease(
    jsg::Ref<WebSocket> ref, kj::Own<kj::WebSocket> owned, kj::Array<kj::String> tags)
    : webSocketRef(kj::mv(ref)),
      ownedWebSocket(kj::mv(owned)),
      tags(kj::mv(tags)) {}

HibernatableWebSocketCustomEvent::HibernatableWebSocketCustomEvent(
    uint16_t typeId, kj::Own<HibernationReader> params)
    : typeId(typeId),
      params(kj::mv(params)) {}
HibernatableWebSocketCustomEvent::HibernatableWebSocketCustomEvent(
    uint16_t typeId, HibernatableSocketParams params)
    : typeId(typeId),
      params(kj::mv(params)) {}

// Try to extract event type from params if available
tracing::HibernatableWebSocketEventInfo::Type HibernatableWebSocketCustomEvent::getEventType()
    const {
  KJ_SWITCH_ONEOF(params) {
    KJ_CASE_ONEOF(socketParams, HibernatableSocketParams) {
      KJ_SWITCH_ONEOF(socketParams.eventType) {
        KJ_CASE_ONEOF(_, HibernatableSocketParams::Text) {
          return tracing::HibernatableWebSocketEventInfo::Message{};
        }
        KJ_CASE_ONEOF(_, HibernatableSocketParams::Data) {
          return tracing::HibernatableWebSocketEventInfo::Message{};
        }
        KJ_CASE_ONEOF(close, HibernatableSocketParams::Close) {
          return tracing::HibernatableWebSocketEventInfo::Close{close.code, close.wasClean};
        }
        KJ_CASE_ONEOF(_, HibernatableSocketParams::Error) {
          return tracing::HibernatableWebSocketEventInfo::Error{};
        }
      }
    }
    KJ_CASE_ONEOF(reader, kj::Own<HibernationReader>) {
      // Parse the HibernationReader to determine the actual event type
      auto payload = reader->getMessage().getPayload();
      switch (payload.which()) {
        case rpc::HibernatableWebSocketEventMessage::Payload::TEXT:
        case rpc::HibernatableWebSocketEventMessage::Payload::DATA:
          return tracing::HibernatableWebSocketEventInfo::Message{};
        case rpc::HibernatableWebSocketEventMessage::Payload::CLOSE: {
          auto close = payload.getClose();
          return tracing::HibernatableWebSocketEventInfo::Close{
            close.getCode(), close.getWasClean()};
        }
        case rpc::HibernatableWebSocketEventMessage::Payload::ERROR:
          return tracing::HibernatableWebSocketEventInfo::Error{};
      }
    }
  }
  KJ_UNREACHABLE;
}

tracing::EventInfo HibernatableWebSocketCustomEvent::getEventInfo() const {
  return tracing::HibernatableWebSocketEventInfo(getEventType());
}

HibernatableSocketParams HibernatableWebSocketCustomEvent::consumeParams() {
  KJ_IF_SOME(p, params.tryGet<kj::Own<HibernationReader>>()) {
    kj::Maybe<HibernatableSocketParams> eventParameters;
    auto websocketId = kj::str(p->getMessage().getWebsocketId());
    auto payload = p->getMessage().getPayload();
    switch (payload.which()) {
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
            close.getCode(), kj::str(close.getReason()), close.getWasClean(), kj::mv(websocketId));
        break;
      }
      case rpc::HibernatableWebSocketEventMessage::Payload::ERROR: {
        eventParameters.emplace(
            KJ_EXCEPTION(FAILED, kj::str(payload.getError())), kj::mv(websocketId));
        break;
      }
    }
    KJ_REQUIRE_NONNULL(eventParameters).setTimeout(p->getMessage().getEventTimeoutMs());
    return kj::mv(KJ_REQUIRE_NONNULL(eventParameters));
  }
  return kj::mv(KJ_REQUIRE_NONNULL(params.tryGet<HibernatableSocketParams>()));
}

}  // namespace workerd::api
