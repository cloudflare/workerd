// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor.h"
#include "util.h"
#include "system-streams.h"
#include <kj/encoding.h>
#include <kj/compat/http.h>
#include <capnp/compat/byte-stream.h>
#include <capnp/compat/http-over-capnp.h>
#include <capnp/schema.h>
#include <capnp/message.h>

namespace workerd::api {

class LocalActorOutgoingFactory final: public Fetcher::OutgoingFactory {
public:
  LocalActorOutgoingFactory(uint channelId, kj::String actorId)
    : channelId(channelId),
      actorId(kj::mv(actorId)) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String> cfStr) override {
    auto& context = IoContext::current();

    // Lazily initialize actorChannel
    if (actorChannel == nullptr) {
      auto& context = IoContext::current();
      actorChannel = context.getColoLocalActorChannel(channelId, kj::mv(actorId));
    }

    return context.getMetrics().wrapActorSubrequestClient(context.getSubrequest(
        [&](kj::Maybe<Tracer::Span&> span, IoChannelFactory& ioChannelFactory) {
      return KJ_REQUIRE_NONNULL(actorChannel)->startRequest({
        .cfBlobJson = kj::mv(cfStr),
        .parentSpan = span
      });
    }, {
      .inHouse = true,
      .wrapMetrics = true,
      .operationName = "actor_subrequest"_kj
    }));
  }

private:
  uint channelId;
  kj::String actorId;
  kj::Maybe<kj::Own<IoChannelFactory::ActorChannel>> actorChannel;
};

class GlobalActorOutgoingFactory final: public Fetcher::OutgoingFactory {
public:
  GlobalActorOutgoingFactory(
      uint channelId,
      jsg::Ref<DurableObjectId> id)
    : channelId(channelId),
      id(kj::mv(id)) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String> cfStr) override {
    auto& context = IoContext::current();

    // Lazily initialize actorChannel
    if (actorChannel == nullptr) {
      auto& context = IoContext::current();
      actorChannel = context.getGlobalActorChannel(channelId, id->getInner());
    }

    return context.getMetrics().wrapActorSubrequestClient(context.getSubrequest(
        [&](kj::Maybe<Tracer::Span&> span, IoChannelFactory& ioChannelFactory) {
      return KJ_REQUIRE_NONNULL(actorChannel)->startRequest({
        .cfBlobJson = kj::mv(cfStr),
        .parentSpan = span
      });
    }, {
      .inHouse = true,
      .wrapMetrics = true,
      .operationName = "actor_subrequest"_kj
    }));
  }

private:
  uint channelId;
  jsg::Ref<DurableObjectId> id;
  kj::Maybe<kj::Own<IoChannelFactory::ActorChannel>> actorChannel;
};

jsg::Ref<Fetcher> ColoLocalActorNamespace::get(jsg::Lock& js, kj::String actorId) {
  JSG_REQUIRE(actorId.size() > 0 && actorId.size() <= 2048, TypeError,
      "Actor ID length must be in the range [1, 2048].");

  auto& context = IoContext::current();

  kj::Own<api::Fetcher::OutgoingFactory> factory = kj::heap<LocalActorOutgoingFactory>(
      channel, kj::mv(actorId));
  auto outgoingFactory = context.addObject(kj::mv(factory));

  bool isInHouse = true;
  return JSG_ALLOC(js, Fetcher,
      kj::mv(outgoingFactory), Fetcher::RequiresHostAndProtocol::YES, isInHouse);
}

// =======================================================================================

kj::String DurableObjectId::toString() {
  return id->toString();
}

jsg::Ref<DurableObjectId> DurableObjectNamespace::newUniqueId(
    jsg::Lock& js,
    jsg::Optional<NewUniqueIdOptions> options) {
  return JSG_ALLOC(js, DurableObjectId, idFactory->newUniqueId(options.orDefault({}).jurisdiction));
}

jsg::Ref<DurableObjectId> DurableObjectNamespace::idFromName(jsg::Lock& js, kj::String name) {
  return JSG_ALLOC(js, DurableObjectId, idFactory->idFromName(kj::mv(name)));
}

jsg::Ref<DurableObjectId> DurableObjectNamespace::idFromString(jsg::Lock& js, kj::String id) {
  return JSG_ALLOC(js, DurableObjectId, idFactory->idFromString(kj::mv(id)));
}

jsg::Ref<DurableObject> DurableObjectNamespace::get(
    jsg::Lock& js,
    jsg::Ref<DurableObjectId> id,
    CompatibilityFlags::Reader featureFlags) {
  auto& context = IoContext::current();
  auto outgoingFactory = context.addObject<Fetcher::OutgoingFactory>(
      kj::heap<GlobalActorOutgoingFactory>(channel, id.addRef()));
  auto requiresHost = featureFlags.getDurableObjectFetchRequiresSchemeAuthority()
      ? Fetcher::RequiresHostAndProtocol::YES
      : Fetcher::RequiresHostAndProtocol::NO;
  return JSG_ALLOC(js, DurableObject, kj::mv(id), kj::mv(outgoingFactory), requiresHost);
}

}  // namespace workerd::api
