// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor.h"

#include <workerd/io/features.h>

#include <capnp/compat/byte-stream.h>
#include <capnp/compat/http-over-capnp.h>
#include <capnp/message.h>
#include <capnp/schema.h>
#include <kj/compat/http.h>
#include <kj/encoding.h>

namespace workerd::api {

kj::Own<WorkerInterface> LocalActorOutgoingFactory::newSingleUseClient(
    kj::Maybe<kj::String> cfStr) {
  auto& context = IoContext::current();

  return context.getMetrics().wrapActorSubrequestClient(context.getSubrequest(
      [&](TraceContext& tracing, IoChannelFactory& ioChannelFactory) {
    if (tracing.span.isObserved()) {
      tracing.span.setTag("actor_id"_kjc, kj::str(actorId));
    }

    // Lazily initialize actorChannel
    if (actorChannel == kj::none) {
      actorChannel = context.getColoLocalActorChannel(channelId, actorId, tracing.span);
    }

    return KJ_REQUIRE_NONNULL(actorChannel)
        ->startRequest({.cfBlobJson = kj::mv(cfStr), .tracing = tracing});
  },
      {.inHouse = true,
        .wrapMetrics = true,
        .operationName = kj::ConstString("durable_object_subrequest"_kjc)}));
}

kj::Own<WorkerInterface> GlobalActorOutgoingFactory::newSingleUseClient(
    kj::Maybe<kj::String> cfStr) {
  auto& context = IoContext::current();

  return context.getMetrics().wrapActorSubrequestClient(context.getSubrequest(
      [&](TraceContext& tracing, IoChannelFactory& ioChannelFactory) {
    if (tracing.span.isObserved()) {
      tracing.span.setTag("actor_id"_kjc, id->toString());
    }

    // Lazily initialize actorChannel
    if (actorChannel == kj::none) {
      KJ_SWITCH_ONEOF(channelIdOrFactory) {
        KJ_CASE_ONEOF(channelId, uint) {
          actorChannel = context.getGlobalActorChannel(channelId, id->getInner(),
              kj::mv(locationHint), mode, enableReplicaRouting, tracing.span);
        }
        KJ_CASE_ONEOF(factory, kj::Own<DurableObjectNamespace::ActorChannelFactory>) {
          actorChannel = factory->getGlobalActor(
              id->getInner(), kj::mv(locationHint), mode, enableReplicaRouting, tracing.span);
        }
      }
    }

    return KJ_REQUIRE_NONNULL(actorChannel)
        ->startRequest({.cfBlobJson = kj::mv(cfStr), .tracing = tracing});
  },
      {.inHouse = true,
        .wrapMetrics = true,
        .operationName = kj::ConstString("durable_object_subrequest"_kjc)}));
}

kj::Own<WorkerInterface> ReplicaActorOutgoingFactory::newSingleUseClient(
    kj::Maybe<kj::String> cfStr) {
  auto& context = IoContext::current();

  return context.getMetrics().wrapActorSubrequestClient(context.getSubrequest(
      [&](TraceContext& tracing, IoChannelFactory& ioChannelFactory) {
    if (tracing.span.isObserved()) {
      tracing.span.setTag("actor_id"_kjc, kj::heapString(actorId));
    }

    // Unlike in `GlobalActorOutgoingFactory`, we do not create this lazily, since our channel was
    // already open prior to this DO starting up.
    return actorChannel->startRequest({.cfBlobJson = kj::mv(cfStr), .tracing = tracing});
  },
      {.inHouse = true,
        .wrapMetrics = true,
        .operationName = kj::ConstString("durable_object_subrequest"_kjc)}));
}

jsg::Ref<Fetcher> ColoLocalActorNamespace::get(jsg::Lock& js, kj::String actorId) {
  JSG_REQUIRE(actorId.size() > 0 && actorId.size() <= 2048, TypeError,
      "Actor ID length must be in the range [1, 2048].");

  auto& context = IoContext::current();

  kj::Own<api::Fetcher::OutgoingFactory> factory =
      kj::heap<LocalActorOutgoingFactory>(channel, kj::mv(actorId));
  auto outgoingFactory = context.addObject(kj::mv(factory));

  bool isInHouse = true;
  return js.alloc<Fetcher>(
      kj::mv(outgoingFactory), Fetcher::RequiresHostAndProtocol::YES, isInHouse);
}

// =======================================================================================

kj::String DurableObjectId::toString() {
  return id->toString();
}

jsg::Ref<DurableObjectId> DurableObjectNamespace::newUniqueId(
    jsg::Lock& js, jsg::Optional<NewUniqueIdOptions> options) {
  return js.alloc<DurableObjectId>(
      idFactory->newUniqueId(options.orDefault({}).jurisdiction.orDefault(kj::none)));
}

jsg::Ref<DurableObjectId> DurableObjectNamespace::idFromName(jsg::Lock& js, kj::String name) {
  return js.alloc<DurableObjectId>(idFactory->idFromName(kj::mv(name)));
}

jsg::Ref<DurableObjectId> DurableObjectNamespace::idFromString(jsg::Lock& js, kj::String id) {
  return js.alloc<DurableObjectId>(idFactory->idFromString(kj::mv(id)));
}

jsg::Ref<DurableObject> DurableObjectNamespace::getByName(
    jsg::Lock& js, kj::String name, jsg::Optional<GetDurableObjectOptions> options) {
  auto id = js.alloc<DurableObjectId>(idFactory->idFromName(kj::mv(name)));
  return getImpl(js, ActorGetMode::GET_OR_CREATE, kj::mv(id), kj::mv(options));
}

jsg::Ref<DurableObject> DurableObjectNamespace::get(
    jsg::Lock& js, jsg::Ref<DurableObjectId> id, jsg::Optional<GetDurableObjectOptions> options) {
  return getImpl(js, ActorGetMode::GET_OR_CREATE, kj::mv(id), kj::mv(options));
}

jsg::Ref<DurableObject> DurableObjectNamespace::getExisting(
    jsg::Lock& js, jsg::Ref<DurableObjectId> id, jsg::Optional<GetDurableObjectOptions> options) {
  return getImpl(js, ActorGetMode::GET_EXISTING, kj::mv(id), kj::mv(options));
}

jsg::Ref<DurableObject> DurableObjectNamespace::getImpl(jsg::Lock& js,
    ActorGetMode mode,
    jsg::Ref<DurableObjectId> id,
    jsg::Optional<GetDurableObjectOptions> options) {
  JSG_REQUIRE(idFactory->matchesJurisdiction(id->getInner()), TypeError,
      "get called on jurisdictional subnamespace with an ID from a different jurisdiction");

  auto& context = IoContext::current();
  kj::Maybe<kj::String> locationHint = kj::none;
  KJ_IF_SOME(o, options) {
    locationHint = kj::mv(o.locationHint);
  }

  bool enableReplicaRouting = FeatureFlags::get(js).getReplicaRouting();

  kj::Own<Fetcher::OutgoingFactory> outgoingFactory;
  KJ_SWITCH_ONEOF(channel) {
    KJ_CASE_ONEOF(channelId, uint) {
      outgoingFactory = kj::heap<GlobalActorOutgoingFactory>(
          channelId, id.addRef(), kj::mv(locationHint), mode, enableReplicaRouting);
    }
    KJ_CASE_ONEOF(channelFactory, IoOwn<ActorChannelFactory>) {
      outgoingFactory = kj::heap<GlobalActorOutgoingFactory>(kj::addRef(*channelFactory),
          id.addRef(), kj::mv(locationHint), mode, enableReplicaRouting);
    }
  }

  auto requiresHost = FeatureFlags::get(js).getDurableObjectFetchRequiresSchemeAuthority()
      ? Fetcher::RequiresHostAndProtocol::YES
      : Fetcher::RequiresHostAndProtocol::NO;
  return js.alloc<DurableObject>(
      kj::mv(id), context.addObject(kj::mv(outgoingFactory)), requiresHost);
}

jsg::Ref<DurableObjectNamespace> DurableObjectNamespace::jurisdiction(
    jsg::Lock& js, jsg::Optional<kj::Maybe<kj::String>> maybeJurisdiction) {
  auto newIdFactory = idFactory->cloneWithJurisdiction(maybeJurisdiction.orDefault(kj::none));

  KJ_SWITCH_ONEOF(channel) {
    KJ_CASE_ONEOF(channelId, uint) {
      return js.alloc<api::DurableObjectNamespace>(channelId, kj::mv(newIdFactory));
    }
    KJ_CASE_ONEOF(channelFactory, IoOwn<ActorChannelFactory>) {
      return js.alloc<api::DurableObjectNamespace>(
          IoContext::current().addObject(kj::addRef(*channelFactory)), kj::mv(newIdFactory));
    }
  }

  KJ_UNREACHABLE;
}

kj::Own<IoChannelFactory::ActorClassChannel> DurableObjectClass::getChannel(IoContext& ioctx) {
  KJ_SWITCH_ONEOF(channel) {
    KJ_CASE_ONEOF(number, uint) {
      return ioctx.getIoChannelFactory().getActorClass(number);
    }
    KJ_CASE_ONEOF(object, IoOwn<IoChannelFactory::ActorClassChannel>) {
      return kj::addRef(*object);
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd::api
