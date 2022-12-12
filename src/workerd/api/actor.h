// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// "Actors" are the internal name for Durable Objects, because they implement a sort of actor
// model. We ended up not calling the product "Actors" publicly because we found that people who
// were familiar with actor-model programming were more confused than helped by it -- they tended
// to expect something that looked more specifically like Erlang, whereas our actors are much more
// abstractly related.

#include <kj/async.h>
#include <capnp/compat/byte-stream.h>
#include <capnp/compat/http-over-capnp.h>
#include <workerd/api/http.h>
#include <workerd/jsg/jsg.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

class ColoLocalActorNamespace: public jsg::Object {
  // A capability to an ephemeral Actor namespace.

public:
  ColoLocalActorNamespace(uint channel)
    : channel(channel) {}

  jsg::Ref<Fetcher> get(kj::String actorId);

  JSG_RESOURCE_TYPE(ColoLocalActorNamespace) {
    JSG_METHOD(get);
  }

private:
  uint channel;
};

class DurableObjectNamespace;

class DurableObjectId: public jsg::Object {
  // DurableObjectId type seen by JavaScript.

public:
  DurableObjectId(kj::Own<ActorIdFactory::ActorId> id): id(kj::mv(id)) {}

  const ActorIdFactory::ActorId& getInner() { return *id; }

  // ---------------------------------------------------------------------------
  // JS API

  kj::String toString();
  // Converts to a string which can be passed back to the constructor to reproduce the same ID.

  inline bool equals(DurableObjectId& other) { return id->equals(*other.id); }

  inline jsg::Optional<kj::StringPtr> getName() { return id->getName(); }
  // Get the name, if known.

  JSG_RESOURCE_TYPE(DurableObjectId) {
    JSG_METHOD(toString);
    JSG_METHOD(equals);
    JSG_READONLY_INSTANCE_PROPERTY(name, getName);
  }

private:
  kj::Own<ActorIdFactory::ActorId> id;

  friend class DurableObjectNamespace;
};

class DurableObject: public Fetcher {
  // Stub object used to send messages to a remote durable object.

public:
  DurableObject(jsg::Ref<DurableObjectId> id, IoOwn<OutgoingFactory> outgoingFactory,
                RequiresHostAndProtocol requiresHost)
    : Fetcher(kj::mv(outgoingFactory), requiresHost, true /* isInHouse */),
      id(kj::mv(id)) {}

  jsg::Ref<DurableObjectId> getId(v8::Isolate* isolate) { return id.addRef(); };
  jsg::Optional<kj::StringPtr> getName() { return id->getName(); }

  JSG_RESOURCE_TYPE(DurableObject) {
    JSG_INHERIT(Fetcher);

    JSG_READONLY_INSTANCE_PROPERTY(id, getId);
    JSG_READONLY_INSTANCE_PROPERTY(name, getName);

    JSG_TS_DEFINE(interface DurableObject {
      fetch(request: Request): Response | Promise<Response>;
      alarm?(): void | Promise<void>;
    });
    JSG_TS_OVERRIDE(DurableObjectStub);
    // Rename this resource type to DurableObjectStub, and make DurableObject
    // the interface implemented by users' Durable Object classes.
  }

private:
  jsg::Ref<DurableObjectId> id;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(id);
  }
};

class DurableObjectNamespace: public jsg::Object {
  // Global durable object class binding type.

public:
  DurableObjectNamespace(uint channel, kj::Own<ActorIdFactory> idFactory)
    : channel(channel), idFactory(kj::mv(idFactory)) {}

  struct NewUniqueIdOptions {
    jsg::Optional<kj::String> jurisdiction;
    // Restricts the new unique ID to a set of colos within a jurisdiction.

    JSG_STRUCT(jurisdiction);
  };

  jsg::Ref<DurableObjectId> newUniqueId(jsg::Optional<NewUniqueIdOptions> options);
  // Create a new unique ID for a durable object that will be allocated nearby the calling colo.

  jsg::Ref<DurableObjectId> idFromName(kj::String name);
  // Create a name-derived ID. Passing in the same `name` (to the same class) will always
  // produce the same ID.

  jsg::Ref<DurableObjectId> idFromString(kj::String id);
  // Create a DurableObjectId from the stringified form of the ID (as produced by calling
  // `toString()` on a durable object ID). Throws if the ID is not a 64-digit hex number, or if the
  // ID was not originally created for this class.
  //
  // The ID may be one that was originally created using either `newUniqueId()` or `idFromName()`.

  struct GetDurableObjectOptions {
    jsg::Optional<kj::String> locationHint;

    JSG_STRUCT(locationHint);
  };

  jsg::Ref<DurableObject> get(
      jsg::Ref<DurableObjectId> id,
      jsg::Optional<GetDurableObjectOptions> options,
      CompatibilityFlags::Reader featureFlags);
  // Gets a durable object by ID or creates it if it doesn't already exist.

  JSG_RESOURCE_TYPE(DurableObjectNamespace) {
    JSG_METHOD(newUniqueId);
    JSG_METHOD(idFromName);
    JSG_METHOD(idFromString);
    JSG_METHOD(get);
    JSG_TS_ROOT();
  }

private:
  uint channel;
  kj::Own<ActorIdFactory> idFactory;
};

#define EW_ACTOR_ISOLATE_TYPES                      \
  api::ColoLocalActorNamespace,                     \
  api::DurableObject,                               \
  api::DurableObjectId,                             \
  api::DurableObjectNamespace,                      \
  api::DurableObjectNamespace::NewUniqueIdOptions,  \
  api::DurableObjectNamespace::GetDurableObjectOptions

}  // namespace workerd::api
