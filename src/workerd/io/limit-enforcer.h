// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/io/observer.h>

namespace workerd {

struct ActorCacheSharedLruOptions;
class IoContext;

static constexpr size_t DEFAULT_MAX_PBKDF2_ITERATIONS = 100'000;

// Interface for an object that enforces resource limits on an Isolate level.
//
// See also LimitEnforcer, which enforces on a per-request level.
class IsolateLimitEnforcer {
public:
  // Get CreateParams to pass when constructing a new isolate.
  virtual v8::Isolate::CreateParams getCreateParams() = 0;

  // Further customize the isolate immediately after startup.
  virtual void customizeIsolate(v8::Isolate* isolate) = 0;

  virtual const ActorCacheSharedLruOptions getActorCacheLruOptions() = 0;

  // Like LimitEnforcer::enterJs(), but used to enforce limits on script startup.
  //
  // When the returned scope object is dropped, if a limit was exceeded, then `error` will be
  // filled in to indicate what happened, otherwise it is left null.
  virtual kj::Own<void> enterStartupJs(
      jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const = 0;

  // used to enforce limits on Python script startup.
  virtual kj::Own<void> enterStartupPython(
      jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const = 0;

  // Like enterStartupJs(), but used when compiling a dynamically-imported module.
  virtual kj::Own<void> enterDynamicImportJs(
      jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const = 0;

  // Like enterStartupJs(), but used to enforce tight limits in cases where we just intend
  // to log an error to the inspector or the like.
  virtual kj::Own<void> enterLoggingJs(
      jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const = 0;

  // Like enterStartupJs(), but used when receiving commands via the inspector protocol.
  virtual kj::Own<void> enterInspectorJs(
      jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const = 0;

  // Notifies the enforcer that a request has been completed. The enforcer is more lenient about
  // limits if several requests have been completed, vs. if limits are broken right off the bat.
  virtual void completedRequest(kj::StringPtr id) const = 0;

  // Called whenever exiting JavaScript execution (i.e. releasing the isolate lock). The enforcer
  // may perform some resource usage checks at this time.
  //
  // Returns true if the isolate has exceeded limits and become condemned.
  virtual bool exitJs(jsg::Lock& lock) const = 0;

  // Report resource usage metrics to the given isolate metrics object.
  virtual void reportMetrics(IsolateObserver& isolateMetrics) const = 0;

  // Called when performing a cypto key derivation function (like pbkdf2) to determine if
  // if the requested number of iterations is acceptable. If kj::none is returned, the
  // number of iterations requested is acceptable. If a number is returned, the requested
  // iterations is unacceptable and the return value specifies the maximum.
  virtual kj::Maybe<size_t> checkPbkdfIterations(jsg::Lock& js, size_t iterations) const {
    // By default, historically we've limited this to 100,000 iterations max. We'll set
    // that as the default for now. To set a default of no-limit, this would be changed
    // to return kj::none. Note, this current default limit is *WAY* below the recommended
    // minimum iterations for pbkdf2.
    // TODO(maybe): We might consider emitting a warning if the number of iterations is
    // too low to be safe.
    if (iterations > DEFAULT_MAX_PBKDF2_ITERATIONS) return DEFAULT_MAX_PBKDF2_ITERATIONS;
    return kj::none;
  }
};

// Abstract interface that enforces resource limits on a IoContext.
class LimitEnforcer  {
public:
  // Called just after taking the isolate lock, before executing JavaScript code, to enforce
  // limits on that code execution, particularly the CPU limit. The returned `Own<void>` should
  // be dropped when JavaScript is done, before unlocking the isolate.
  virtual kj::Own<void> enterJs(jsg::Lock& lock, IoContext& context) = 0;

  // Called on each new event delivered that should cause an actor's resource limits to be
  // "topped up". This method does nothing if the IoContext is not an actor. Note that this must
  // not be called while in a JS scope, i.e. when `enterJs()` has been called and the returned
  // object not yet dropped.
  virtual void topUpActor() = 0;
  // TODO(cleanup): This is called in WebSocket when receiving a message, but should we do
  //   something more generic like use a membrane to detect any incoming RPC call?

  // Called before starting a new subrequest. Throws a JSG exception if the limit has been
  // reached.
  //
  // `isInHouse` is true for types of subrequests which we need to be "in house" (i.e. to another
  // Cloudflare service, like Workers KV) and thus should not be subject to the same limits as
  // external subrequests.
  virtual void newSubrequest(bool isInHouse) = 0;

  enum class KvOpType { GET, PUT, LIST, DELETE };
  // Called before starting a KV operation. Throws a JSG exception if the operation should be
  // blocked due to exceeding limits, such as the free tier daily operation limit.
  virtual void newKvRequest(KvOpType op) = 0;

  // Called before starting an attempt to write to the Analytics Engine. Throws
  // a JSG exception if the operation should be blocked due to exceeding limits.
  virtual void newAnalyticsEngineRequest() = 0;

  // Applies a time limit to draining a request (i.e. waiting for `waitUntil()`s after the
  // response has been sent). Returns a promise that will resolve (without error) when the time
  // limit has expired. This should be joined with the drain task.
  //
  // This should not be called for actors, which are evicted when the supervisor decides to
  // evict them, not on a timeout basis.
  virtual kj::Promise<void> limitDrain() = 0;

  // Like limitDrain() but applies a time limit to scheduled event processing.
  virtual kj::Promise<void> limitScheduled() = 0;

  // Like limitDrain() and limitScheduled() but applies a time limit to alarm event processing.
  virtual kj::Duration getAlarmLimit() = 0;

  // Gets a byte size limit to apply to operations that will buffer a possibly large amount of
  // data in C++ memory, such as reading an entire HTTP response into an `ArrayBuffer`.
  virtual size_t getBufferingLimit() = 0;

  // If a limit has been exceeded which prevents further JavaScript execution, such as the CPU or
  // memory limit, returns a request status code indicating which one. Returns null if no limits
  // are exceeded.
  virtual kj::Maybe<EventOutcome> getLimitsExceeded() = 0;

  // Reutrns a promise that will reject if and when a limit is exceeded that prevents further
  // JavaScript execution, such as the CPU or memory limit.
  virtual kj::Promise<void> onLimitsExceeded() = 0;

  // Throws an exception if a limit has already been exceeded which prevents further JavaScript
  // execution, such as the CPU or memory limit.
  virtual void requireLimitsNotExceeded() = 0;

  // Report resource usage metrics to the given request metrics object.
  virtual void reportMetrics(RequestObserver& requestMetrics) = 0;

  // Quota for total PUTs to cache in MB, or kj::none for the default.
  virtual kj::Maybe<uint64_t> getCachePUTLimitMB() = 0;
};

}  // namespace workerd
