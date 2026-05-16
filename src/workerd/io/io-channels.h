// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/actor-id.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/frankenvalue.h>
#include <workerd/io/io-util.h>
#include <workerd/io/trace.h>
#include <workerd/io/worker-source.h>

#include <capnp/capability.h>  // for Capability
#include <kj/debug.h>
#include <kj/string.h>

namespace kj {
class HttpClient;
class Network;
}  // namespace kj

namespace workerd {

class WorkerInterface;

// Interface for talking to the Cache API. Needs to be declared here so that IoContext can
// contain it.
class CacheClient {
 public:
  struct SubrequestMetadata {
    // The `request.cf` blob, JSON-encoded.
    kj::Maybe<kj::String> cfBlobJson;

    // Specifies the parent span for the subrequest for tracing purposes.
    SpanParent parentSpan;

    // Serialized JSON value to pass in ew_compat field of control header to FL. This has the same
    // semantics as the field in IoChannelFactory::SubrequestMetadata.
    kj::Maybe<kj::String> featureFlagsForFl;
  };

  // Get the default namespace, i.e. the one that fetch() will use for caching.
  //
  // The returned client is intended to be used for one request.
  virtual kj::Own<kj::HttpClient> getDefault(SubrequestMetadata metadata) = 0;

  // Get an HttpClient for the given cache namespace.
  virtual kj::Own<kj::HttpClient> getNamespace(kj::StringPtr name, SubrequestMetadata metadata) = 0;
};

// A timer instance, used to back Date.now(), setTimeout(), etc. This object may implement
// Spectre mitigations.
class TimerChannel {
 public:
  // Call each time control enters the isolate to set up the clock.
  virtual void syncTime() = 0;

  // Return the current time. `nextTimeout` is the time at which the next setTimeout() callback
  // is scheduled; implementations performing Spectre mitigations should clamp to this value so
  // that Date.now() never goes backwards or reveals timing side channels.
  virtual kj::Date now(kj::Maybe<kj::Date> nextTimeout = kj::none) = 0;

  // Returns a promise that resolves once `now() >= when`.
  virtual kj::Promise<void> atTime(kj::Date when) = 0;

  // Returns a promise that resolves after some time. This is intended to be used for implementing
  // time limits on some sort of operation, not for implementing application-driven timing, as it does
  // not implement any Spectre mitigations.
  virtual kj::Promise<void> afterLimitTimeout(kj::Duration t) = 0;
};

class WorkerStubChannel;
struct DynamicWorkerSource;

// Each IoContext has a set of "channels" on which outgoing I/O can be initiated. All outgoing
// I/O occurs through these channels. Think of these kind of like file descriptors. They are
// often associated with bindings.
//
// For example, any call to fetch() uses a subrequest channel. The global fetch() specifically
// uses subrequest channel zero. Each service binding (aka worker-to-worker binding) is assigned
// a unique subrequest channel number, and calling `binding.fetch()` sends the request to the
// given channel.
//
// While most channels are SubrequestChannels, other channel types exist to handle I/O that is
// not subrequest-shaped. For example, a Workers Analytics Engine binding uses a logging channel.
//
// Note that each type of channel has its own number space. That is, subrequest channel 5 and
// logging channel 5 are not related.
//
// The reason we have channels, rather than binding API objects directly holding the I/O objects,
// is because binding API objects live across multiple requests, but the I/O objects may differ
// from request to request.
//
// This class encapsulates all outgoing I/O that a Worker can perform. It does not cover incoming
// I/O, i.e. the event that started the Worker. If IoChannelFactory is implemented such that
// all methods throw exceptions, then the Worker will be completely unable to communicate with
// anything in the world except for the client -- this is a useful property for sandboxing!
class IoChannelFactory {
 public:
  // Contains metadata attached to an outgoing subrequest from a worker, independent of the type
  // of request.
  struct SubrequestMetadata {
    // The `request.cf` blob, JSON-encoded.
    kj::Maybe<kj::String> cfBlobJson;

    // Specifies the parent span for the subrequest for tracing purposes.
    SpanParent parentSpan = SpanParent(nullptr);

    // User Span Parent for trace propagation. Call toSpanContext() to serialize.
    SpanParent userSpanParent = SpanParent(nullptr);

    // Serialized JSON value to pass in ew_compat field of control header to FL. If this subrequest
    // does not go directly to FL, this value is ignored. Flags marked with `$neededByFl` in
    // `compatibility-date.capnp` end up here.
    kj::Maybe<kj::String> featureFlagsForFl;

    // Timestamp for when a subrequest is started. (ms since the Unix Epoch)
    double startTime = dateNow();
  };

  // Parameters that can influence the version of a worker that is used to serve a subrequest.
  struct VersionRequest {
    // Request a version within the given cohort.
    kj::Maybe<kj::String> cohort;

    VersionRequest clone() const {
      return {
        .cohort = cohort.map([](const kj::String& s) { return kj::str(s); }),
      };
    }
  };

  virtual kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) = 0;

  // Get a Cap'n Proto RPC capability. Various binding types are backed by capabilities.
  //
  // Note that some other channel types, like actor channels, may actually be wrappers around
  // capability channels, and so may share the same channel number space, but this shouldn't be
  // assumed.
  virtual capnp::Capability::Client getCapability(uint channel) = 0;

  // Get a CacheClient, used to implement the Cache API.
  virtual kj::Own<CacheClient> getCache() = 0;

  // Get the singleton timer instance, used to back Date.now(), setTimeout(), etc. This object
  // may implement Spectre mitigations.
  virtual TimerChannel& getTimer() = 0;

  // Write a log message to a logfwdr channel. Each log binding has its own channel number.
  //
  // The IoChannelFactory already knows which member of the overall message union is expected to
  // be filled in for this channel. That member will be initialized as a pointer, and then
  // `buildMessage` will be invoked to fill in the pointer's content. The callback is always
  // executed immediately, before `writeLogfwdr()` returns a promise.
  virtual kj::Promise<void> writeLogfwdr(
      uint channel, kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage) = 0;

  enum ChannelTokenUsage {
    // Token is to be sent over RPC and hence will be converted back into a SubrequestChannel
    // soon. Such tokens have limited lifetime but are otherwise irrevocable.
    RPC,

    // Token is to be stored in long-term storage. At present this must only be allowed to be
    // used in workers that have the allow_irrevocable_stub_storage compat flag (checked by the
    // caller). In the future the format for such tokens will change.
    STORAGE,
  };

  // Object representing somehere where generic workers subrequests can be sent. Multiple requests
  // may be sent. This is an I/O type so it is only valid within the `IoContext` where it was
  // created.
  class SubrequestChannel: public kj::Refcounted, public Frankenvalue::CapTableEntry {
   public:
    // Start a new request to this target.
    //
    // Note that not all `metadata` properties make sense here, but it didn't seem worth defining
    // a new struct type. `cfBlobJson` and `parentSpan` make sense, but `featureFlagsForFl` and
    // `dynamicDispatchTarget` do not.
    //
    // Note that the caller is expected to keep the SubrequestChannel alive until it is done with
    // the returned WorkerInterface.
    virtual kj::Own<WorkerInterface> startRequest(SubrequestMetadata metadata) = 0;

    kj::Own<CapTableEntry> clone() override final {
      return kj::addRef(*this);
    }

    // Throws a JSG error if a Fetcher backed by this channel should not be serialized and passed
    // to other workers. The default implementation throws a generic error, but subclasses may
    // specialize with better errror messages -- or override to just return in order to permit the
    // serialization.
    //
    // This check is necessary especially in workerd in order to block serialization of types that,
    // in production, would be difficult or impossible to serialize. In particular,
    // dynamically-loaded workers cannot be serialized because the system does not know how to
    // reconstruct a dynamically-loaded worker from scratch.
    virtual void requireAllowsTransfer() = 0;

    // Get a token representing this SubrequestChannel which can be converted back into a
    // SubrequestChannel using subrequestChannelFromToken(). This is a convenience wrapper around
    // getTokenMaybeSync() for callers that don't care about the synchronous optimization.
    kj::Promise<kj::Array<byte>> getToken(ChannelTokenUsage usage);

    // Like getToken() but may return the token synchronously. This is what subclasses must
    // implement. The synchronous optimization is important because there is significant additional
    // overhead in the RPC system when the token cannot be created synchronously (need to use
    // ExternalPusher to send a DelayedChannelToken).
    virtual kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
        ChannelTokenUsage usage) = 0;

    // If this SubrequestChannel is just a wrapper around a promise for some later
    // SubrequestChannel, return the inner channel -- synchronously if the promise has resolved
    // already, otherwise asynchronously.
    //
    // Note that the various `IoChannelFactory` methods that take `props` or `env` objects all
    // automatically resolve all channel objects *before* passing off to the underlying
    // implementation. In the internal codebase, implementations end up needing to downcast these
    // objects to implementation-specific types, and handling the need to call getResolved()
    // in every use case would be painful, so it is taken care of in this layer.
    //
    // Default implementation returns self.
    virtual kj::OneOf<kj::Own<SubrequestChannel>, kj::Promise<kj::Own<SubrequestChannel>>>
    getResolved() {
      return kj::addRef(*this);
    }
  };

  // Obtain an object representing a particular subrequest channel.
  //
  // getSubrequestChannel(i).startRequest(meta) is exactly equivalent to startSubrequest(i, meta).
  // The reason to use this instead is when the channel is not necessarily going to be used to
  // start a subrequest immediately, but instead is going to be passed around as a capability.
  //
  // `props` and `versionRequest` can only be specified if this is a loopback channel (i.e. from
  // ctx.exports). For any other channel, they will throw.
  //
  // The non-virtual method dispatches to getSubrequestChannelResolved(), but only after resolving
  // all channels embedded in `props` (that is, calling `getResolved()` on all of them, waiting
  // for the resolutions if necessary, and replacing the caps with the resolutions).
  //
  // TODO(cleanup): Consider getting rid of `startSubrequest()` in favor of this.
  kj::Own<SubrequestChannel> getSubrequestChannel(uint channel,
      kj::Maybe<Frankenvalue> props = kj::none,
      kj::Maybe<VersionRequest> versionRequest = kj::none);

  // Underlying implementation of getSubrequestChannel(). The implementation can assume that `props`
  // contains strictly resolved channels.
  virtual kj::Own<SubrequestChannel> getSubrequestChannelResolved(
      uint channel, kj::Maybe<Frankenvalue> props, kj::Maybe<VersionRequest> versionRequest) = 0;

  // ActorChannel used to be its own type, but no longer is.
  // TODO(cleanup): Update all references.
  using ActorChannel = SubrequestChannel;

  // Get an actor stub from the given namespace for the actor with the given ID.
  //
  // `id` must have been constructed using one of the `ActorIdFactory` instances corresponding to
  // one of the worker's bindings, however it doesn't necessarily have to be from the the correct
  // `ActorIdFactory` -- if it's from some other factory, the method will throw an appropriate
  // exception.
  virtual kj::Own<ActorChannel> getGlobalActor(uint channel,
      const ActorIdFactory::ActorId& id,
      kj::Maybe<kj::String> locationHint,
      ActorGetMode mode,
      bool enableReplicaRouting,
      ActorRoutingMode routingMode,
      SpanParent parentSpan,
      kj::Maybe<ActorVersion> version) = 0;

  // Get an actor stub from the given namespace for the actor with the given name.
  virtual kj::Own<ActorChannel> getColoLocalActor(
      uint channel, kj::StringPtr id, SpanParent parentSpan) = 0;

  // ActorClassChannel is a reference to an actor class in another worker. This class acts as a
  // token which can be passed into other interfaces that might use the actor class, particularly
  // Worker::Actor::FacetManager.
  class ActorClassChannel: public kj::Refcounted, public Frankenvalue::CapTableEntry {
   public:
    kj::Own<CapTableEntry> clone() override final {
      return kj::addRef(*this);
    }

    // Same as the corresponding methods on SubrequestChannel.
    virtual void requireAllowsTransfer() = 0;
    kj::Promise<kj::Array<byte>> getToken(ChannelTokenUsage usage);
    virtual kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
        ChannelTokenUsage usage) = 0;
    virtual kj::OneOf<kj::Own<ActorClassChannel>, kj::Promise<kj::Own<ActorClassChannel>>>
    getResolved() {
      return kj::addRef(*this);
    }

    // This class has no functional methods, since it serves as a token to be passed to other
    // interfaces (namely the facets API).
  };

  // Get an actor class binding corresponding to the given channel number.
  //
  // `props` can only be specified if this is a loopback channel (i.e. from ctx.exports). For any
  // other channel, it will throw.
  //
  // The non-virtual method dispatches to getActorClassResolved(), but only after resolving
  // all channels embedded in `props` (that is, calling `getResolved()` on all of them, waiting
  // for the resolutions if necessary, and replacing the caps with the resolutions).
  kj::Own<ActorClassChannel> getActorClass(uint channel, kj::Maybe<Frankenvalue> props = kj::none);

  // Underlying implementation of getActorClass(). The implementation can assume that `props`
  // contains strictly resolved channels.
  virtual kj::Own<ActorClassChannel> getActorClassResolved(
      uint channel, kj::Maybe<Frankenvalue> props) = 0;

  // Aborts all actors except those in namespaces marked with `preventEviction`.
  virtual void abortAllActors(kj::Maybe<kj::Exception&> reason) {
    KJ_UNIMPLEMENTED("Only implemented by single-tenant workerd runtime");
  }

  // Aborts all actors, cancels all alarms, and deletes all underlying storage for evictable
  // namespaces. After this, DOs can be recreated with clean state. Useful for test isolation.
  virtual void deleteAllActors(kj::Maybe<kj::Exception&> reason) {
    KJ_UNIMPLEMENTED("Only implemented by single-tenant workerd runtime");
  }

  // In workerd, the handler aborts the process (unless used on a dynamic
  // worker). In the edge runtime it will condemn and terminate the current
  // isolate.
  virtual void abortIsolate(kj::StringPtr reason) = 0;

  // Use a dynamic Worker loader binding to obtain an Worker by name. If name is null, or if the named Worker doesn't already exist, the callback will be called to fetch the source code from which the Worker should be created.
  virtual kj::Own<WorkerStubChannel> loadIsolate(uint loaderChannel,
      kj::Maybe<kj::String> name,
      kj::Function<kj::Promise<DynamicWorkerSource>()> fetchSource) {
    JSG_FAIL_REQUIRE(Error, "Dynamic worker loading is not supported by this runtime.");
  }

  // Get the network for connecting to workerd debug ports.
  // This is used by the workerdDebugPort binding to connect to remote workerd instances.
  virtual kj::Network& getWorkerdDebugPortNetwork() {
    JSG_FAIL_REQUIRE(Error, "WorkerdDebugPort bindings are not supported by this runtime.");
  }

  // Converts a token created with {SubrequestChannel,ActorClassChannel}::getToken() back into a
  // live channel. Default implementations throw.
  virtual kj::Own<SubrequestChannel> subrequestChannelFromToken(
      ChannelTokenUsage usage, kj::ArrayPtr<const byte> token);
  virtual kj::Own<ActorClassChannel> actorClassFromToken(
      ChannelTokenUsage usage, kj::ArrayPtr<const byte> token);

  // Overloads which accept a promise. Any attempts to use the channel will have to wait for the
  // token to arrive first, but this should be transparent.
  kj::Own<SubrequestChannel> subrequestChannelFromToken(
      ChannelTokenUsage usage, kj::Promise<kj::Array<byte>> token);
  kj::Own<ActorClassChannel> actorClassFromToken(
      ChannelTokenUsage usage, kj::Promise<kj::Array<byte>> token);

  // Return a strong reference to this same factory. Used in the implementations of
  // getSubrequestChannel() and getActorClass() when delayed resolution is needed.
  //
  // TODO(cleanup): This is hacky. IoChannelFactory isn't declared to simply extend kj::Refcounted
  //   because the workerd implementation is privately implemented by Server::WorkerService, which
  //   inherits kj::Refcounted a different way. But maybe it's time for Server::WorkerService to
  //   stop working that way?
  virtual kj::Own<void> addRef() = 0;
};

// ResourceLimits provides a means to control the resource allocation for a worker stage via a
// set of optionally overridden parameters.
struct ResourceLimits {
  jsg::Optional<uint32_t> cpuMs;
  jsg::Optional<uint32_t> subRequests;

  JSG_STRUCT(cpuMs, subRequests);

  ResourceLimits clone() const {
    return {cpuMs, subRequests};
  }
};

// Represents a dynamically-loaded Worker to which requests can be sent.
//
// This object is returned before the Worker actually loads, so if any errors occur while loading,
// any requests sent to the Worker will fail, propagating the exception.
class WorkerStubChannel: public kj::Refcounted {
 public:
  // As with IoChannelFactory::getSubrequestChannel(), the non-virtual method waits for `props` to
  // resolve first, then calls the virtual method.
  kj::Own<IoChannelFactory::SubrequestChannel> getEntrypoint(
      kj::Maybe<kj::String> name, Frankenvalue props, kj::Maybe<ResourceLimits> limits);
  virtual kj::Own<IoChannelFactory::SubrequestChannel> getEntrypointResolved(
      kj::Maybe<kj::String> name, Frankenvalue props, kj::Maybe<ResourceLimits> limits) = 0;

  // As with IoChannelFactory::getActorClass(), the non-virtual method waits for `props` to
  // resolve first, then calls the virtual method.
  kj::Own<IoChannelFactory::ActorClassChannel> getActorClass(
      kj::Maybe<kj::String> name, Frankenvalue props, kj::Maybe<ResourceLimits> limits);
  virtual kj::Own<IoChannelFactory::ActorClassChannel> getActorClassResolved(
      kj::Maybe<kj::String> name, Frankenvalue props, kj::Maybe<ResourceLimits> limits) = 0;

  // TODO(someday): Allow caller to enumerate entrypoints?
};

// Source code needed to dynamically load a Worker.
struct DynamicWorkerSource {
  WorkerSource source;
  CompatibilityFlags::Reader compatibilityFlags;

  kj::Maybe<ResourceLimits> limits;

  // `env` object to pass to the loaded worker. Can contain anything that can be serialized to
  // a `Frankenvalue` (which should eventually include all binding types, RPC stubs, etc.).
  Frankenvalue env;

  // Where should global fetch() (and connect()) be sent?
  kj::Maybe<kj::Own<IoChannelFactory::SubrequestChannel>> globalOutbound;

  // Tail workers that should receive tail events for invocations of the dynamic worker.
  kj::Array<kj::Own<IoChannelFactory::SubrequestChannel>> tails;
  kj::Array<kj::Own<IoChannelFactory::SubrequestChannel>> streamingTails;

  // Owns any data structures pointed into by the other members. (E.g. `source` contains a lot of
  // `StringPtr`s; `ownContent` owns the backing buffer for them.)
  kj::Own<void> ownContent;

  // Indicates whether ownContent is holding onto a Cap'n Proto RPC response. This is important
  // to know because such an RPC response must be destroyed on the same thread where it was
  //  created, and generally should be destroyed "relatively soon", not kept around forever. If
  //  this is false, then it is perfectly safe to transfer ownership of ownContent between threads
  //  and keep it alive indefinitely long.
  bool ownContentIsRpcResponse = true;

  // Clone the DynamicWorkerSource. Caller must provide a new reference to use as `ownContent`,
  // which must be a refcount on the same content since the pointers will not be updated. Note
  // that if `ownContentIsRpcResponse` is false, then `ownContent` could be passed off to other
  // threads and as such the refcount had better be atomic.
  DynamicWorkerSource clone(kj::Own<void> newOwnContent) {
    return {
      .source = source.clone(),
      .compatibilityFlags = compatibilityFlags,
      .limits = limits.map([](auto& limits) { return limits.clone(); }),
      .env = env.clone(),
      .globalOutbound = mapAddRef(globalOutbound),
      .tails = KJ_MAP(t, tails) { return kj::addRef(*t); },
      .streamingTails = KJ_MAP(t, streamingTails) { return kj::addRef(*t); },
      .ownContent = kj::mv(newOwnContent),
      .ownContentIsRpcResponse = ownContentIsRpcResponse,
    };
  }

  // Walks through all channels in `env` and other properties and ensures that they point at
  // resolved objects by calling their `getResolved()` methods.
  kj::Promise<void> ensureAllResolved();
};

// A Frankenvalue::CapTableEntry which directly references a numbered I/O channel. This is ONLY
// valid to use when the `Frankenvalue` is being deserialized as the `env` object of an isolate.
// The caller should use frankenvalue.rewriteCaps() to rewrite the cap table entries into
// IoChannelCapTableEntry, building the I/O channel table as it goes.
class IoChannelCapTableEntry final: public Frankenvalue::CapTableEntry {
 public:
  enum Type {
    SUBREQUEST,
    ACTOR_CLASS,
    // TODO(someday): Other channel types, maybe.
  };

  IoChannelCapTableEntry(Type type, uint channel): type(type), channel(channel) {}

  Type getType() const {
    return type;
  }

  // Throws if type doesn't match.
  uint getChannelNumber(Type expectedType);

  kj::Own<CapTableEntry> clone() override;
  kj::Own<CapTableEntry> threadSafeClone() const override;

 private:
  Type type;
  uint channel;
};

}  // namespace workerd
