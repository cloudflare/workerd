// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "actor.h"
#include "http.h"

#include <workerd/io/io-channels.h>

namespace workerd::api {

// LoopbackServiceStub is the type of a property of `ctx.exports` which points back at a stateless
// (non-actor) entrypoint of this Worker. It can be used as a regular Fetcher to make calls to that
// entrypoint with empty props. It can also be invoked as a function in order to specialize it with
// props and make it available for RPC.
class LoopbackServiceStub: public Fetcher {
 public:
  // Loopback services are always represented by numbered subrequest channels.
  explicit LoopbackServiceStub(uint channel)
      : Fetcher(channel, RequiresHostAndProtocol::YES, /*isInHouse=*/true),
        channel(channel) {}

  struct Options {
    jsg::Optional<jsg::JsRef<jsg::JsObject>> props;

    JSG_STRUCT(props);
  };

  struct OptionsWithVersion {
    struct Version {
      jsg::Optional<kj::Maybe<kj::String>> cohort;

      JSG_STRUCT(cohort);
    };

    jsg::Optional<jsg::JsRef<jsg::JsObject>> props;
    jsg::Optional<Version> version;

    JSG_STRUCT(props, version);
  };

  // Create a specialized Fetcher which can be passed over RPC.
  jsg::Ref<Fetcher> callImpl(jsg::Lock& js,
      jsg::Optional<jsg::JsRef<jsg::JsObject>> propsMaybe,
      jsg::Optional<OptionsWithVersion::Version> versionMaybe);

  jsg::Ref<Fetcher> call(jsg::Lock& js, Options options) {
    return callImpl(js, kj::mv(options.props), kj::none);
  }

  jsg::Ref<Fetcher> callWithVersion(jsg::Lock& js, OptionsWithVersion options) {
    return callImpl(js, kj::mv(options.props), kj::mv(options.version));
  }

  // Note that `LoopbackServiceStub` is intentionally NOT serializable, unlike its parent class
  // Fetcher. We want people to explicitly specialize the entrypoint with props before sending
  // it off to other services.

  JSG_RESOURCE_TYPE(LoopbackServiceStub, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(Fetcher);

    if (flags.getEnableVersionApi()) {
      JSG_CALLABLE(callWithVersion);
    } else {
      JSG_CALLABLE(call);
    }

    JSG_TS_ROOT();

    if (flags.getEnableVersionApi()) {
      JSG_TS_OVERRIDE(
        type LoopbackServiceStub<
          T extends Rpc.WorkerEntrypointBranded | undefined = undefined
        > = Fetcher<T> &
          ( T extends CloudflareWorkersModule.WorkerEntrypoint<any, infer Props>
          ? (opts: {props?: Props, version?: { cohort?: string | null }}) => Fetcher<T>
          : (opts: {props?: any, version?: { cohort?: string | null }}) => Fetcher<T>);
      );
    } else {
      JSG_TS_OVERRIDE(
        type LoopbackServiceStub<
          T extends Rpc.WorkerEntrypointBranded | undefined = undefined
        > = Fetcher<T> &
          ( T extends CloudflareWorkersModule.WorkerEntrypoint<any, infer Props>
          ? (opts: {props?: Props}) => Fetcher<T>
          : (opts: {props?: any}) => Fetcher<T>);
      );
    }

    // LoopbackForExport takes the type of an exported value and evaluates to the appropriate
    // loopback stub for that export.
    JSG_TS_DEFINE(
      type LoopbackForExport<
        T extends
          | (new (...args: any[]) => Rpc.EntrypointBranded)
          | ExportedHandler<any, any, any>
          | undefined = undefined
      > = T extends new (...args: any[]) => Rpc.WorkerEntrypointBranded ? LoopbackServiceStub<InstanceType<T>>
        : T extends new (...args: any[]) => Rpc.DurableObjectBranded ? LoopbackDurableObjectClass<InstanceType<T>>
        : T extends ExportedHandler<any, any, any> ? LoopbackServiceStub<undefined>
        : undefined;
    );
  }

 private:
  uint channel;
};

// Similar to LoopbackServiceStub, but for actor classes.
//
// Specifically, this is used for actor classes that do *not* have any storage configured. If you
// simply export a class extending `DurableObject` but you don't configure storage for it, it shows
// up in `ctx.exports` as this type. This can be used to create a Durable Object facet.
class LoopbackDurableObjectClass: public DurableObjectClass {
 public:
  LoopbackDurableObjectClass(uint channel): DurableObjectClass(channel), channel(channel) {}

  struct Options {
    jsg::Optional<jsg::JsRef<jsg::JsObject>> props;

    JSG_STRUCT(props);
  };

  // Create a specialized DurableObjectClass which can be passed over RPC.
  jsg::Ref<DurableObjectClass> call(jsg::Lock& js, Options options);

  JSG_RESOURCE_TYPE(LoopbackDurableObjectClass) {
    JSG_INHERIT(DurableObjectClass);
    JSG_CALLABLE(call);

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE(
      type LoopbackDurableObjectClass<
        T extends
          | Rpc.DurableObjectBranded
          | undefined = undefined
      > = DurableObjectClass<T> &
        ( T extends CloudflareWorkersModule.DurableObject<any, infer Props>
        ? (opts: {props?: Props}) => DurableObjectClass<T>
        : (opts: {props?: any}) => DurableObjectClass<T>);
    );
  }

 private:
  uint channel;
};

// LoopbackDurableObjectNamespace is similar to LoopbackDurableObjectClass, but used when the
// class has storage configured. In this case, we want a binding that behaves *both* like a
// LoopbackDurableObjectClass *and* like a DurableObjectNamespace binding. Easy enough, we'll
// inherit DurableObjectNamespace, but also make the binding invokable as a function like
// LoopbackDurableObjectClass.
class LoopbackDurableObjectNamespace: public DurableObjectNamespace {
 public:
  LoopbackDurableObjectNamespace(uint nsChannel,
      kj::Own<ActorIdFactory> idFactory,
      jsg::Ref<LoopbackDurableObjectClass> loopbackClass)
      : DurableObjectNamespace(nsChannel, kj::mv(idFactory)),
        loopbackClass(kj::mv(loopbackClass)) {}

  // getClass() accessor for use from C++ only.
  LoopbackDurableObjectClass& getClass() {
    return *loopbackClass.get();
  }

  // Invoking the binding creates a specialization of the class -- not the namespace.
  jsg::Ref<DurableObjectClass> call(jsg::Lock& js, LoopbackDurableObjectClass::Options options) {
    return loopbackClass->call(js, kj::mv(options));
  }

  // If `DurableObjectNamespace` ever becomes serializable, we actually don't want to block
  // serialization here, the way we want to for `LoopbackDurableObjectClass`, because actually
  // serializing the loopback namespace would mean serializing the namespace stub, *not* the
  // class stub. They are different things, and you might want to serialize either one.

  JSG_RESOURCE_TYPE(LoopbackDurableObjectNamespace) {
    JSG_INHERIT(DurableObjectNamespace);
    JSG_CALLABLE(call);
  }

 private:
  jsg::Ref<LoopbackDurableObjectClass> loopbackClass;
};

// Like LoopbackDurableObjectNamespace, but for colo-local (ephemeral) actor namespaces.
class LoopbackColoLocalActorNamespace: public ColoLocalActorNamespace {
 public:
  LoopbackColoLocalActorNamespace(
      uint nsChannel, jsg::Ref<LoopbackDurableObjectClass> loopbackClass)
      : ColoLocalActorNamespace(nsChannel),
        loopbackClass(kj::mv(loopbackClass)) {}

  // getClass() accessor for use from C++ only.
  LoopbackDurableObjectClass& getClass() {
    return *loopbackClass.get();
  }

  // Invoking the binding creates a specialization of the class -- not the namespace.
  jsg::Ref<DurableObjectClass> call(jsg::Lock& js, LoopbackDurableObjectClass::Options options) {
    return loopbackClass->call(js, kj::mv(options));
  }

  JSG_RESOURCE_TYPE(LoopbackColoLocalActorNamespace) {
    JSG_INHERIT(ColoLocalActorNamespace);
    JSG_CALLABLE(call);
  }

 private:
  jsg::Ref<LoopbackDurableObjectClass> loopbackClass;
};

#define EW_EXPORT_LOOPBACK_ISOLATE_TYPES                                                           \
  api::LoopbackServiceStub, api::LoopbackServiceStub::Options,                                     \
      api::LoopbackServiceStub::OptionsWithVersion,                                                \
      api::LoopbackServiceStub::OptionsWithVersion::Version, api::LoopbackDurableObjectClass,      \
      api::LoopbackDurableObjectClass::Options, api::LoopbackDurableObjectNamespace,               \
      api::LoopbackColoLocalActorNamespace

}  // namespace workerd::api
