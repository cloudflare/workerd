// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/actor-id.h>
#include <kj/string.h>
#include <kj/debug.h>
#include <workerd/io/trace.h>

namespace kj { class HttpClient; }

namespace workerd {

class WorkerInterface;

// Interface for talking to the Cache API. Needs to be declared here so that IoContext can
// contain it.
class CacheClient {
public:
  // Get the default namespace, i.e. the one that fetch() will use for caching.
  //
  // The returned client is intended to be used for one request. `cfBlobJson` and `parentSpan` have
  // the same meaning as in `IoContext::SubrequestMetadata`.
  virtual kj::Own<kj::HttpClient> getDefault(
      kj::Maybe<kj::String> cfBlobJson, SpanParent parentSpan) = 0;

  // Get an HttpClient for the given cache namespace.
  //
  // The returned client is intended to be used for one request. `parentSpan` has the same meaning
  // as in `IoContext::SubrequestMetadata`.
  virtual kj::Own<kj::HttpClient> getNamespace(
      kj::StringPtr name, kj::Maybe<kj::String> cfBlobJson,
      SpanParent parentSpan) = 0;
};

// A timer instance, used to back Date.now(), setTimeout(), etc. This object may implement
// Spectre mitigations.
class TimerChannel {
public:
  // Call each time control enters the isolate to set up the clock.
  virtual void syncTime() = 0;

  // Return the current time.
  virtual kj::Date now() = 0;

  // Returns a promise that resolves once `now() >= when`.
  virtual kj::Promise<void> atTime(kj::Date when) = 0;

  // Returns a promise that resolves after some time. This is intended to be used for implementing
  // time limits on some sort of operation, not for implementing application-driven timing, as it does
  // not implement any spectre mitigations.
  virtual kj::Promise<void> afterLimitTimeout(kj::Duration t) = 0;
};

// Each IoContext has a set of "channels" on which outgoing I/O can be initiated. All outgoing
// I/O occurs through these channels. Think of these kind of like file descriptors. They are
// often associated with bindings.
//
// For example, any call to fetch() uses a subrequest channel. The global fetch() specifically
// uses subrequest channel zero. Each service binding (aka worker-to-worker binding) is assigned
// a unique subrequest channel number, and calling `binding.fetch()` sends the request to the
// given channel.
//
// While most channels are SubrequestChannels, other channel types exit to handle I/O that is
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
    SpanParent parentSpan = nullptr;

    // Serialized JSON value to pass in ew_compat field of control header to FL. If this subrequest
    // does not go directly to FL, this value is ignored. Flags marked with `$neededByFl` in
    // `compatibility-date.capnp` end up here.
    //
    // This string remains valid at least until either the request has returned response headers
    // or has been canceled. (In practice, this string's lifetime is that of the Isolate making
    // the request.)
    kj::Maybe<kj::StringPtr> featureFlagsForFl;
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

  // Stub for a remote actor. Allows sending requests to the actor. Multiple requests may be
  // sent, and they will be delivered in the order they are sent (e-order). This is an I/O type
  // so it is only valid within the `IoContext` where it was created.
  class ActorChannel {
  public:
    // Start a new request to this actor.
    //
    // Note that not all `metadata` properties make sense here, but it didn't seem worth defining
    // a new struct type. `cfBlobJson` and `parentSpan` make sense, but `featureFlagsForFl` and
    // `dynamicDispatchTarget` do not.
    virtual kj::Own<WorkerInterface> startRequest(SubrequestMetadata metadata) = 0;
  };

  // Get an actor stub from the given namespace for the actor with the given ID.
  //
  // `id` must have been constructed using one of the `ActorIdFactory` instances corresponding to
  // one of the worker's bindings, however it doesn't necessarily have to be from the the correct
  // `ActorIdFactory` -- if it's from some other factory, the method will throw an appropriate
  // exception.
  virtual kj::Own<ActorChannel> getGlobalActor(uint channel, const ActorIdFactory::ActorId& id,
      kj::Maybe<kj::String> locationHint, ActorGetMode mode, SpanParent parentSpan) = 0;

  // Get an actor stub from the given namespace for the actor with the given name.
  virtual kj::Own<ActorChannel> getColoLocalActor(uint channel, kj::StringPtr id,
      SpanParent parentSpan) = 0;

  // Aborts all actors except those in namespaces marked with `preventEviction`.
  virtual void abortAllActors() {
    KJ_UNIMPLEMENTED("Only implemented by single-tenant workerd runtime");
  }
};

} // namespace workerd
