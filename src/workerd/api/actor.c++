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
    tracing.setTag("objectId"_kjc, actorId.asPtr());

    // Lazily initialize actorChannel
    if (actorChannel == kj::none) {
      actorChannel =
          context.getColoLocalActorChannel(channelId, actorId, tracing.getInternalSpanParent());
    }

    return KJ_REQUIRE_NONNULL(actorChannel)
        ->startRequest({.cfBlobJson = kj::mv(cfStr),
          .parentSpan = tracing.getInternalSpanParent(),
          .userSpanParent = tracing.getUserSpanParent()});
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
    tracing.setTag("objectId"_kjc, id->toString());

    // Lazily initialize actorChannel
    if (actorChannel == kj::none) {
      KJ_SWITCH_ONEOF(channelIdOrFactory) {
        KJ_CASE_ONEOF(channelId, uint) {
          actorChannel = context.getGlobalActorChannel(channelId, id->getInner(),
              kj::mv(locationHint), mode, enableReplicaRouting, routingMode,
              tracing.getInternalSpanParent(), kj::mv(version));
        }
        KJ_CASE_ONEOF(factory, kj::Own<DurableObjectNamespace::ActorChannelFactory>) {
          actorChannel = factory->getGlobalActor(id->getInner(), kj::mv(locationHint), mode,
              enableReplicaRouting, routingMode, tracing.getInternalSpanParent(), kj::mv(version));
        }
      }
    }

    return KJ_REQUIRE_NONNULL(actorChannel)
        ->startRequest({.cfBlobJson = kj::mv(cfStr),
          .parentSpan = tracing.getInternalSpanParent(),
          .userSpanParent = tracing.getUserSpanParent()});
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
    tracing.setTag("objectId"_kjc, actorId.asPtr());

    // Unlike in `GlobalActorOutgoingFactory`, we do not create this lazily, since our channel was
    // already open prior to this DO starting up.
    return actorChannel->startRequest({.cfBlobJson = kj::mv(cfStr),
      .parentSpan = tracing.getInternalSpanParent(),
      .userSpanParent = tracing.getUserSpanParent()});
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
  ActorRoutingMode routingMode = ActorRoutingMode::DEFAULT;
  KJ_IF_SOME(o, options) {
    KJ_IF_SOME(rm, o.routingMode) {
      JSG_REQUIRE(rm == "primary-only", RangeError, "unknown routingMode: ", rm);
      routingMode = ActorRoutingMode::PRIMARY_ONLY;
    }
  }

  auto& context = IoContext::current();
  kj::Maybe<kj::String> locationHint;
  kj::Maybe<ActorVersion> version;
  KJ_IF_SOME(o, options) {
    locationHint = kj::mv(o.locationHint);
    if (FeatureFlags::get(js).getEnableVersionApi()) {
      KJ_IF_SOME(v, o.version) {
        version = ActorVersion{.cohort = kj::mv(v.cohort)};
      }
    }
  }

  bool enableReplicaRouting = FeatureFlags::get(js).getReplicaRouting();

  kj::Own<Fetcher::OutgoingFactory> outgoingFactory;
  KJ_SWITCH_ONEOF(channel) {
    KJ_CASE_ONEOF(channelId, uint) {
      outgoingFactory = kj::heap<GlobalActorOutgoingFactory>(channelId, id.addRef(),
          kj::mv(locationHint), mode, enableReplicaRouting, routingMode, kj::mv(version));
    }
    KJ_CASE_ONEOF(channelFactory, IoOwn<ActorChannelFactory>) {
      outgoingFactory =
          kj::heap<GlobalActorOutgoingFactory>(kj::addRef(*channelFactory), id.addRef(),
              kj::mv(locationHint), mode, enableReplicaRouting, routingMode, kj::mv(version));
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

void DurableObjectClass::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  auto& ioctx = IoContext::current();
  auto channel = getChannel(ioctx);
  channel->requireAllowsTransfer();

  KJ_IF_SOME(handler, serializer.getExternalHandler()) {
    KJ_IF_SOME(frankenvalueHandler, kj::tryDowncast<Frankenvalue::CapTableBuilder>(handler)) {
      // Encoding a Frankenvalue (e.g. for dynamic loopback props or dynamic isolate env).
      serializer.writeRawUint32(frankenvalueHandler.add(kj::mv(channel)));
      return;
    } else KJ_IF_SOME(rpcHandler, kj::tryDowncast<RpcSerializerExternalHandler>(handler)) {
      JSG_REQUIRE(FeatureFlags::get(js).getWorkerdExperimental(), DOMDataCloneError,
          "DurableObjectClass serialization requires the 'experimental' compat flag.");

      KJ_SWITCH_ONEOF(channel->getTokenMaybeSync(IoChannelFactory::ChannelTokenUsage::RPC)) {
        KJ_CASE_ONEOF(token, kj::Array<byte>) {
          rpcHandler.write([token = kj::mv(token)](rpc::JsValue::External::Builder builder) {
            builder.setActorClassChannelToken(token);
          });
        }
        KJ_CASE_ONEOF(promise, kj::Promise<kj::Array<byte>>) {
          // Token isn't available synchronously, so we have to send a promise.
          auto paf = kj::newPromiseAndFulfiller<
              rpc::JsValue::ExternalPusher::DelayedChannelToken::Client>();

          // Arrange to send the token when it's ready.
          ioctx.addTask(
              promise.then([pusher = rpcHandler.getExternalPusher(),
                               fulfiller = kj::mv(paf.fulfiller)](kj::Array<byte> token) mutable {
            auto req = pusher.pushDelayedChannelTokenRequest(
                capnp::MessageSize{4 + token.size() / sizeof(capnp::word), 0});
            req.setToken(token);
            fulfiller->fulfill(req.send().getCap());
          }));

          // Write the promise for now.
          rpcHandler.write(
              [promise = kj::mv(paf.promise)](rpc::JsValue::External::Builder builder) mutable {
            builder.setDelayedActorClassChannelToken(kj::mv(promise));
          });
        }
      }
      return;
    }
    // TODO(someday): structuredClone() should have special handling that just reproduces the same
    //   local object. At present we have no way to recognize structuredClone() here though.
  }

  // The allow_irrevocable_stub_storage flag allows us to just embed the token inline. This format
  // is temporary, anyone using this will lose their data later.
  JSG_REQUIRE(FeatureFlags::get(js).getAllowIrrevocableStubStorage(), DOMDataCloneError,
      "DurableObjectClass cannot be serialized in this context.");
  KJ_SWITCH_ONEOF(channel->getTokenMaybeSync(IoChannelFactory::ChannelTokenUsage::STORAGE)) {
    KJ_CASE_ONEOF(token, kj::Array<byte>) {
      serializer.writeLengthDelimited(token);
    }
    KJ_CASE_ONEOF(promise, kj::Promise<kj::Array<byte>>) {
      // TODO(stub-storage): Eventually we'll serialize by pointing to an external table.
      KJ_UNIMPLEMENTED(
          "tried to store ActorClassChannel whose token is not synchronously available");
    }
  }
}

jsg::Ref<DurableObjectClass> DurableObjectClass::deserialize(
    jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer) {
  KJ_IF_SOME(handler, deserializer.getExternalHandler()) {
    KJ_IF_SOME(frankenvalueHandler, kj::tryDowncast<Frankenvalue::CapTableReader>(handler)) {
      // Decoding a Frankenvalue (e.g. for dynamic loopback props or dynamic isolate env).
      auto& cap = KJ_REQUIRE_NONNULL(frankenvalueHandler.get(deserializer.readRawUint32()),
          "serialized DurableObjectClass had invalid cap table index");

      KJ_IF_SOME(channel, kj::tryDowncast<IoChannelFactory::ActorClassChannel>(cap)) {
        // Probably decoding dynamic ctx.props.
        return js.alloc<DurableObjectClass>(IoContext::current().addObject(kj::addRef(channel)));
      } else KJ_IF_SOME(channel, kj::tryDowncast<IoChannelCapTableEntry>(cap)) {
        // Probably decoding dynamic isolate env.
        return js.alloc<DurableObjectClass>(
            channel.getChannelNumber(IoChannelCapTableEntry::Type::ACTOR_CLASS));
      } else {
        KJ_FAIL_REQUIRE(
            "DurableObjectClass capability in Frankenvalue is not a ActorClassChannel?");
      }
    } else KJ_IF_SOME(rpcHandler, kj::tryDowncast<RpcDeserializerExternalHandler>(handler)) {
      JSG_REQUIRE(FeatureFlags::get(js).getWorkerdExperimental(), DOMDataCloneError,
          "DurableObjectClass serialization requires the 'experimental' compat flag.");

      auto external = rpcHandler.read();
      auto& ioctx = IoContext::current();
      kj::Own<IoChannelFactory::ActorClassChannel> channel;

      if (external.isDelayedActorClassChannelToken()) {
        auto promise = ioctx.getExternalPusher()->unwrapDelayedChannelToken(
            external.getDelayedActorClassChannelToken());
        channel = ioctx.getIoChannelFactory().actorClassFromToken(
            IoChannelFactory::ChannelTokenUsage::RPC, kj::mv(promise));
      } else if (external.isActorClassChannelToken()) {
        channel = ioctx.getIoChannelFactory().actorClassFromToken(
            IoChannelFactory::ChannelTokenUsage::RPC, external.getActorClassChannelToken());
      } else {
        KJ_FAIL_REQUIRE("wrong external type for DurableObjectClass", external.which());
      }

      return js.alloc<DurableObjectClass>(ioctx.addObject(kj::mv(channel)));
    }
  }

  // The allow_irrevocable_stub_storage flag allows us to just embed the token inline. This format
  // is temporary, anyone using this will lose their data later.
  JSG_REQUIRE(FeatureFlags::get(js).getAllowIrrevocableStubStorage(), DOMDataCloneError,
      "DOMDataCloneError cannot be deserialized in this context.");
  auto& ioctx = IoContext::current();
  auto channel = ioctx.getIoChannelFactory().actorClassFromToken(
      IoChannelFactory::ChannelTokenUsage::STORAGE, deserializer.readLengthDelimitedBytes());
  return js.alloc<DurableObjectClass>(ioctx.addObject(kj::mv(channel)));
}

}  // namespace workerd::api
