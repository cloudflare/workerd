// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/entrypoint-invocation.h>

#include <kj/compat/http.h>

namespace workerd {
class BaseTracer;
class RequestObserver;
class ThreadContext;
namespace api {
class AbortController;
}  // namespace api
}  // namespace workerd

namespace workerd::entrypoint {

// The steps of a fetch event of a WorkerEntrypoint, which C++ WorkerEntrypoint::request() and the
// Rust WorkerEntrypoint (`entrypoint/fetch.rs`) take in the same order:
//
//   1. setEventInfo(), IoContext::IncomingRequest::delivered() on getRequest(), delivered()
//   2. runHandler(), and if it fails, handlerFailed(); then endHandler(), however it ends
//   3. finishProxying()
//   4. If a step above failed: prepareFailure(), then the first that applies of
//      propagateFromActor(), failOpen(), propagate() and respondWithError().
//
// Destroying a Fetch before endHandler() runs it, so a cancelled event still drains its request.
// Whether a fetch event's failure is tunnelled back to the calling Worker, rather than turned into
// an error response.
WD_STRONG_BOOL(TunnelExceptions);

class Fetch {
 public:
  Fetch(kj::Own<Invocation> invocation,
      kj::TaskSet& waitUntilTasks,
      ThreadContext& threadContext,
      kj::Maybe<kj::String> cfBlobJson,
      TunnelExceptions tunnelExceptions,
      IsDynamicDispatch isDynamicDispatch,
      kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response,
      const void* flowId);
  ~Fetch() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Fetch);

  // Stage 1: reports the event to the tracer, before the caller delivers it with
  // IoContext::IncomingRequest::delivered() on getRequest(), and then calls delivered(). The
  // caller delivers it, so that the request records the caller's location as where it was
  // delivered.
  void setEventInfo();
  IoContext::IncomingRequest& getRequest();
  void delivered();

  // Stage 2: runs the fetch handler in the IoContext. Resolves once the handler has a response,
  // keeping its deferred-proxy task for finishProxying() and reporting the return to the tracer.
  // Rejects if called after endHandler().
  kj::Promise<void> runHandler();

  // Stage 2, if runHandler() failed with `exception`: logs it, unless it is a dispatch rejection,
  // and waits for the output gate. Rejects with the exception to fail the event with: the output
  // gate's, if it broke, or else `exception`. Rejects if called after endHandler().
  kj::Promise<void> handlerFailed(kj::Exception exception);

  // Stage 2, however it ended: opens the fail-open channel if fail-open was chosen, has the
  // handler's AbortSignal abort if the client disconnected, and drains the request. After it, the
  // IoContext must not be used.
  void endHandler();

  // Stage 3: waits for the handler's deferred-proxy task, if it has one, without the IoContext.
  kj::Promise<void> finishProxying();

  // Stage 4, for the exception that a step above failed with: drops the deferred-proxy task,
  // marks the exception as delivered and logs it if handlerFailed() did not. Returns it.
  kj::Exception prepareFailure(kj::Exception exception);

  bool isActor() const {
    return actor;
  }
  bool hasFailOpenService() const {
    return failOpenService != kj::none;
  }
  bool tunnelsExceptions() const {
    return tunnelExceptions.toBool();
  }

  // Stage 4, for an actor: the exception to fail the event with, annotated as delivered to the
  // actor.
  kj::Exception propagateFromActor(kj::Exception exception);

  // Stage 4, with a fail-open service: retries the request through it, and responds with a 500 if
  // that fails.
  kj::Promise<void> failOpen(kj::Exception exception);

  // Stage 4, when exceptions are tunnelled to the caller: the exception to fail the event with.
  kj::Exception propagate(kj::Exception exception);

  // Stage 4, otherwise: reports the failure and responds with a 503 if the exception is
  // OVERLOADED, or else a 500, unless a response was already sent.
  void respondWithError(kj::Exception exception);

 private:
  class ResponseSentTracker;

  kj::Own<Invocation> invocation;
  kj::TaskSet& waitUntilTasks;
  ThreadContext& threadContext;
  kj::Maybe<kj::String> cfBlobJson;
  TunnelExceptions tunnelExceptions;
  IsDynamicDispatch isDynamicDispatch;
  kj::HttpMethod method;
  kj::StringPtr url;
  const kj::HttpHeaders& headers;
  kj::AsyncInputStream& requestBody;
  kj::Own<ResponseSentTracker> response;
  const void* flowId;

  IoContext& context;
  bool actor;
  kj::Maybe<BaseTracer&> workerTracer;
  kj::Maybe<kj::Own<RequestObserver>> metrics;
  bool handlerEnded = false;
  bool loggedExceptionEarlier = false;
  bool isInternalException = false;
  kj::Maybe<kj::Promise<void>> proxyTask;
  kj::Maybe<kj::Own<WorkerInterface>> failOpenService;
  kj::Maybe<jsg::Ref<api::AbortController>> abortController;

  RequestObserver& getMetrics();
  void sendSyntheticStatus(uint statusCode, kj::StringPtr statusText);
};

// Shared with the other events of a WorkerEntrypoint.
tracing::FetchEventInfo buildFetchEventInfo(kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::Maybe<kj::StringPtr> cfBlobJson);
kj::Exception exceptionToPropagate(bool isInternalException, kj::Exception&& exception);

}  // namespace workerd::entrypoint
