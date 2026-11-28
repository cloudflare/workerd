// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "entrypoint-fetch.h"

#include <workerd/api/basics.h>
#include <workerd/api/deferred-proxy.h>
#include <workerd/api/global-scope.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/sentry.h>
#include <workerd/util/strings.h>
#include <workerd/util/uncaught-exception-source.h>
#include <workerd/util/use-perfetto-categories.h>

namespace workerd::entrypoint {

namespace {

// True for a retry-claim or predecessor rejection, which the runtime raises before the fetch
// handler runs. The sender classifies it by its details, so it must propagate unchanged.
bool isActorDispatchRejection(const kj::Exception& exception) {
  return exception.getDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID) != kj::none ||
      exception.getDetail(jsg::ACTOR_PREDECESSOR_REJECTED_DETAIL_ID) != kj::none;
}

}  // namespace

// To match our historical behavior (when we used to pull the headers from the JavaScript object
// later on), headers are canonicalized: names are lower-cased and values with the same name are
// combined into a comma-delimited list. (This explicitly breaks the Set-Cookie header,
// incidentally, but should be equivalent for all other headers.)
tracing::FetchEventInfo buildFetchEventInfo(kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::Maybe<kj::StringPtr> cfBlobJson) {
  kj::String cfJson;
  KJ_IF_SOME(c, cfBlobJson) {
    cfJson = kj::str(c);
  }

  kj::TreeMap<kj::String, kj::Vector<kj::StringPtr>> traceHeaders;
  headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
    kj::String lower = toLower(name);
    auto& slot = traceHeaders.findOrCreate(
        lower, [&]() { return decltype(traceHeaders)::Entry{kj::mv(lower), {}}; });
    slot.add(value);
  });
  auto traceHeadersArray = KJ_MAP(entry, traceHeaders) {
    return tracing::FetchEventInfo::Header(kj::mv(entry.key), kj::strArray(entry.value, ", "));
  };

  return tracing::FetchEventInfo(method, kj::str(url), kj::mv(cfJson), kj::mv(traceHeadersArray));
}

kj::Exception exceptionToPropagate(bool isInternalException, kj::Exception&& exception) {
  if (isInternalException) {
    // We've already logged it here, the only thing that matters to the client is that we failed
    // due to an internal error. Note that this does not need to be labeled "remote." since jsg
    // will sanitize it as an internal error. Note that we use `setDescription()` to preserve
    // the exception type for `jsg::exceptionToJs(...)` downstream.
    exception.setDescription(kj::str("worker_do_not_log; Request failed due to internal error"));
    return kj::mv(exception);
  } else {
    // We do not care how many remote capnp servers this went through since we are returning
    // it to the worker via jsg.
    // TODO(someday) We also do this stripping when making the tunneled exception for
    // `jsg::isTunneledException(...)`. It would be lovely if we could simply store some type
    // instead of `loggedExceptionEarlier`. It would save use some work.
    kj::Maybe<kj::String> newDescription;
    {
      auto description = jsg::stripRemoteExceptionPrefix(exception.getDescription());
      if (!description.startsWith("remote.")) {
        // If we already were annotated as remote from some other worker entrypoint, no point
        // adding an additional prefix.
        newDescription = kj::str("remote.", description);
      }
    }
    KJ_IF_SOME(description, newDescription) {
      exception.setDescription(kj::mv(description));
    }
    return kj::mv(exception);
  }
}

class Fetch::ResponseSentTracker final: public kj::HttpService::Response {
 public:
  ResponseSentTracker(kj::HttpService::Response& inner): inner(inner) {}
  KJ_DISALLOW_COPY_AND_MOVE(ResponseSentTracker);

  bool isSent() const {
    return sent;
  }
  uint getHttpResponseStatus() const {
    return httpResponseStatus;
  }

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    TRACE_EVENT(
        "workerd", "WorkerEntrypoint::ResponseSentTracker::send()", "statusCode", statusCode);
    sent = true;
    httpResponseStatus = statusCode;
    return inner.send(statusCode, statusText, headers, expectedBodySize);
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    TRACE_EVENT("workerd", "WorkerEntrypoint::ResponseSentTracker::acceptWebSocket()");
    sent = true;
    return inner.acceptWebSocket(headers);
  }

 private:
  uint httpResponseStatus = 0;
  kj::HttpService::Response& inner;
  bool sent = false;
};

Fetch::Fetch(kj::Own<Invocation> invocation,
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
    const void* flowId)
    : invocation(kj::mv(invocation)),
      waitUntilTasks(waitUntilTasks),
      threadContext(threadContext),
      cfBlobJson(kj::mv(cfBlobJson)),
      tunnelExceptions(tunnelExceptions),
      isDynamicDispatch(isDynamicDispatch),
      method(method),
      url(url),
      headers(headers),
      requestBody(requestBody),
      response(kj::heap<ResponseSentTracker>(response)),
      flowId(flowId),
      context(this->invocation->request->getContext()),
      actor(context.getActor() != kj::none) {}

Fetch::~Fetch() noexcept(false) {
  // A cancelled event ends the handler, then drops the deferred-proxy task, so that it never pins
  // the IoContext past the event. A destructor may run while an exception unwinds, so an
  // exception from endHandler() is logged rather than thrown.
  if (!handlerEnded && metrics != kj::none) {
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() { endHandler(); })) {
      KJ_LOG(ERROR, "ending a cancelled fetch handler failed", exception);
    }
  }
  proxyTask = kj::none;
}

RequestObserver& Fetch::getMetrics() {
  return *KJ_ASSERT_NONNULL(metrics, "the fetch event was not delivered");
}

void Fetch::setEventInfo() {
  auto& incomingRequest = getRequest();

  // HACK: Capture workerTracer directly, it's unclear how to acquire the right tracer from context
  // when we need it (for DOs, IoContext may point to a different WorkerTracer by the time we use
  // it). The tracer lives as long or longer than the IoContext (based on being co-owned
  // by IncomingRequest and PipelineTracer) so long enough.
  KJ_IF_SOME(t, incomingRequest.getWorkerTracer()) {
    t.setEventInfo(incomingRequest, buildFetchEventInfo(method, url, headers, cfBlobJson));
    workerTracer = t;
  }
}

IoContext::IncomingRequest& Fetch::getRequest() {
  KJ_REQUIRE(invocation->request.get() != nullptr, "the fetch handler already ended");
  return *invocation->request;
}

void Fetch::delivered() {
  metrics = kj::addRef(getRequest().getMetrics());
}

kj::Promise<void> Fetch::runHandler() {
  // endHandler() drains the request, which may destroy `context`.
  KJ_REQUIRE(!handlerEnded, "the fetch handler already ended");
  TRACE_EVENT_BEGIN("workerd", "WorkerEntrypoint::request() waiting on context",
      PERFETTO_TRACK_FROM_POINTER(&context), PERFETTO_FLOW_FROM_POINTER(flowId));

  api::DeferredProxy<void> deferredProxy = co_await context.run(
      [this, entrypointName = invocation->entrypointName.map([](kj::String& name) {
    return kj::str(name);
  }),
          metrics = kj::addRef(getMetrics())](Worker::Lock& lock, IoContext& context) mutable {
    TRACE_EVENT_END("workerd", PERFETTO_TRACK_FROM_POINTER(&context));
    TRACE_EVENT("workerd", "WorkerEntrypoint::request() run", PERFETTO_FLOW_FROM_POINTER(flowId));
    auto traceScope = context.makeRequestTraceScope(lock);

    kj::Maybe<jsg::Ref<api::AbortSignal>> signal;
    auto featureFlags = FeatureFlags::get(lock);
    if (featureFlags.getEnableRequestSignal()) {
      auto abortSignalFlag = featureFlags.getRequestSignalPassthrough()
          ? api::AbortSignal::Flag::NONE
          : api::AbortSignal::Flag::IGNORE_FOR_SUBREQUESTS;
      jsg::Lock& js = lock;
      signal.emplace(abortController.emplace(js.alloc<api::AbortController>(js, abortSignalFlag))
                         ->getSignal());
    }

    // Getting the handler finishes actor construction, so the claim sees a constructed actor
    // but still precedes the choice and invocation of the fetch handler.
    auto handler = lock.getExportedHandler(
        entrypointName.map([](kj::String& name) -> kj::StringPtr { return name; }),
        kj::mv(invocation->versionInfo), kj::mv(invocation->props), context.getActor(),
        isDynamicDispatch.toBool());
    auto retryable = IsRetryableHandler::NO;
    KJ_IF_SOME(h, handler) {
      retryable = h->isFetchRetryable(lock);
    }
    metrics->claimRetryTokenBeforeUserCode(retryable);
    return lock.getGlobalScope().request(
        method, url, headers, requestBody, *response, cfBlobJson, lock, handler, kj::mv(signal));
  });

  // Record the proxy task and the tracer return time on the success path.
  TRACE_EVENT("workerd", "WorkerEntrypoint::request() deferred proxy step",
      PERFETTO_FLOW_FROM_POINTER(flowId));
  proxyTask = kj::mv(deferredProxy.proxyTask);
  KJ_IF_SOME(t, workerTracer) {
    auto httpResponseStatus = response->getHttpResponseStatus();
    if (httpResponseStatus != 0) {
      t.setReturn(context.now(), tracing::FetchResponseInfo(httpResponseStatus));
    } else {
      t.setReturn(context.now());
    }
  }
}

kj::Promise<void> Fetch::handlerFailed(kj::Exception exception) {
  // endHandler() drains the request, which may destroy `context`.
  KJ_REQUIRE(!handlerEnded, "the fetch handler already ended");
  TRACE_EVENT("workerd", "WorkerEntrypoint::request() catch", PERFETTO_FLOW_FROM_POINTER(flowId));
  // Log JS exceptions to the JS console, if inspector is attached. This also has the effect
  // of logging internal errors to syslog. A dispatch rejection is a runtime decision rather
  // than an uncaught exception, so it is not logged.
  loggedExceptionEarlier = true;
  bool dispatchRejected = isActorDispatchRejection(exception);
  if (!dispatchRejected) {
    context.logUncaughtExceptionAsync(
        UncaughtExceptionSource::REQUEST_HANDLER, exception.clone(), *invocation->request);
  }

  // Record a failure if cancellation interrupts either wait. Otherwise the WorkerInterface
  // wrapper reports it. An output-gate failure takes precedence over the handler failure.
  kj::Maybe<kj::Exception> outputGateException;
  auto metricsForCatch = kj::addRef(getMetrics());
  auto reportFailure = kj::defer([&]() {
    KJ_IF_SOME(e, outputGateException) {
      metricsForCatch->reportFailure(e);
    } else {
      metricsForCatch->reportFailure(exception);
    }
  });

  // Do not allow the exception to escape the isolate without waiting for the output gate to
  // open. Note that in the success path, this is taken care of in `FetchEvent::respondWith()`.
  // If the gate is broken, that exception propagates and replaces the original, unless the
  // original is a dispatch rejection.
  KJ_TRY {
    co_await context.waitForOutputLocks();
  }
  KJ_CATCH(e) {
    if (!dispatchRejected) {
      outputGateException.emplace(kj::mv(e));
    }
  }
  TRACE_EVENT("workerd", "WorkerEntrypoint::request() after output lock wait",
      PERFETTO_TERMINATING_FLOW_FROM_POINTER(flowId));
  // Yield to give a pending cancellation (e.g., the caller dropping our promise because
  // the upstream WebSocket was torn down) a chance to take effect before propagating to
  // the final catch. The original `.then()` chain had an implicit yield point here where
  // the chain crossed into the next `.then` after this catch; without it, downstream
  // observers can mistake a canceled request for one that threw.
  co_await kj::yield();
  KJ_IF_SOME(e, outputGateException) {
    reportFailure.cancel();
    kj::throwException(kj::mv(e));
  }
  reportFailure.cancel();
  kj::throwException(kj::mv(exception));
}

void Fetch::endHandler() {
  KJ_REQUIRE(!handlerEnded, "the fetch handler already ended");
  handlerEnded = true;

  // The request has been canceled, but allow it to continue executing in the background.
  if (context.isFailOpen()) {
    // Fail-open behavior has been chosen, we'd better save an interface that we can use for
    // that purpose later.
    //
    // Opening the channel can throw, as a null global outbound does. This may run in a
    // destructor (C++ request()'s KJ_DEFER, or ~Fetch() when the event is cancelled), possibly
    // while the handler's exception unwinds, so it must not throw.
    // Instead the fail-open request fails with the exception, as it would have had the
    // channel opened lazily, as a channel to another process does.
    KJ_TRY {
      failOpenService = context.getSubrequestChannelNoChecks(
          IoContext::NEXT_CLIENT_CHANNEL, false, kj::mv(cfBlobJson));
    }
    KJ_CATCH(exception) {
      failOpenService = WorkerInterface::fromException(kj::mv(exception));
    }
  }

  // When the client disconnects, trigger an abort on request.signal, unless the request has
  // already completed normally, or failed with an exception.
  // TODO(perf): Don't add a task to trigger the abort unless we know it has at least one
  // listener.
  if (proxyTask == kj::none && !loggedExceptionEarlier && abortController != kj::none) {
    auto ctrl = KJ_ASSERT_NONNULL(abortController).addRef();
    context.addWaitUntil(context.run([ctrl = kj::mv(ctrl)](Worker::Lock& lock) mutable {
      ctrl->getSignal()->triggerAbort(
          lock, JSG_KJ_EXCEPTION(DISCONNECTED, DOMAbortError, "The client has disconnected"));
    }));
  }

  // Release reference to the AbortController.
  // Either the waitUntilTask holds a reference to it, or it will never be triggered at all.
  abortController = kj::none;

  auto& request = *invocation->request;
  request.drain(waitUntilTasks, kj::mv(invocation->request));
}

kj::Promise<void> Fetch::finishProxying() {
  KJ_IF_SOME(p, proxyTask) {
    TRACE_EVENT("workerd", "WorkerEntrypoint::request() finish proxying",
        PERFETTO_TERMINATING_FLOW_FROM_POINTER(flowId));
    // Now that the IoContext is dropped (unless it had waitUntil()s), we can finish proxying
    // without pinning it or the isolate into memory.
    KJ_TRY {
      co_await p;
      // Match the continuation boundary in the old promise chain. This lets the completed
      // awaiter's WebSocket ownership unwind before the HTTP request handler completes.
      co_await kj::yield();
    }
    KJ_CATCH(e) {
      getMetrics().reportFailure(e, RequestObserver::FailureSource::DEFERRED_PROXY);
      // See the matching yield in handlerFailed().
      co_await kj::yield();
      kj::throwException(kj::mv(e));
    }
  }
  // Drop the task once it is done.
  proxyTask = kj::none;
}

kj::Exception Fetch::prepareFailure(kj::Exception exception) {
  // Drop the deferred-proxy task, if finishProxying() didn't, before the failure is handled.
  proxyTask = kj::none;

  TRACE_EVENT("workerd", "WorkerEntrypoint::request() exception",
      PERFETTO_TERMINATING_FLOW_FROM_POINTER(flowId));

  // Stage 1 called `delivered()`, so exceptions reaching this catch are post-delivery failures.
  markExceptionAsDelivered(exception);

  isInternalException = !jsg::isTunneledException(exception.getDescription()) &&
      !jsg::isDoNotLogException(exception.getDescription());
  if (!loggedExceptionEarlier) {
    // This exception seems to have originated during the deferred proxy task, so it was not
    // logged to the IoContext earlier.
    if (exception.getType() != kj::Exception::Type::DISCONNECTED && isInternalException) {
      LOG_EXCEPTION("workerEntrypoint", exception);
    } else {
      KJ_LOG(INFO, exception);  // Run with --verbose to see exception logs.
    }
  }

  if (response->isSent()) {
    // Can't fail open if a response was already started.
    failOpenService = kj::none;
  }

  return kj::mv(exception);
}

void Fetch::sendSyntheticStatus(uint statusCode, kj::StringPtr statusText) {
  KJ_TRY {
    if (response->isSent()) return;
    kj::HttpHeaders errorHeaders(threadContext.getHeaderTable());
    response->send(statusCode, statusText, errorHeaders, static_cast<uint64_t>(0));
    KJ_IF_SOME(t, workerTracer) {
      t.setReturn(kj::none, tracing::FetchResponseInfo(response->getHttpResponseStatus()));
    }
  }
  KJ_CATCH(e) {
    markExceptionAsDelivered(e);
    kj::throwException(kj::mv(e));
  }
}

kj::Exception Fetch::propagateFromActor(kj::Exception exception) {
  // Reaching this point means the request reached the actor (delivered() ran in
  // stage 1), so user code may have run. Annotate every failure, whatever its type, so the
  // caller-side actor-call classifier knows it must not be retried, even if it is retryable.
  // Preserve not-delivered only for a predecessor rejection, which occurs before the fetch
  // handler runs, though possibly after the actor constructor. Set before
  // exceptionToPropagate() so the detail survives the internal-exception description rewrite
  // and serializes back across the RPC boundary.
  if (!jsg::isActorPredecessorRejection(exception)) {
    exception.releaseDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID);
    exception.setDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID, kj::heapArray<kj::byte>(0));
  }
  // TODO(cleanup): We'd really like to tunnel exceptions any time a worker is calling another
  // worker, not just for actors (and W2W below), but getting that right will require cleaning
  // up error handling more generally.
  return exceptionToPropagate(isInternalException, kj::mv(exception));
}

kj::Promise<void> Fetch::failOpen(kj::Exception exception) {
  auto& metrics = getMetrics();
  // We're catching the exception, but metrics should still indicate an exception.
  metrics.reportFailure(exception);

  auto serviceOwn = kj::mv(KJ_ASSERT_NONNULL(failOpenService));
  failOpenService = kj::none;
  metrics.setFailedOpen(true);
  KJ_TRY {
    co_await serviceOwn->request(method, url, headers, requestBody, *response);
  }
  KJ_CATCH(e) {
    metrics.setFailedOpen(false);
    // Avoid logging recognized external errors here, such as invalid headers returned from
    // the server.
    if (e.getType() != kj::Exception::Type::DISCONNECTED &&
        !jsg::isTunneledException(e.getDescription()) &&
        !jsg::isDoNotLogException(e.getDescription())) {
      LOG_EXCEPTION("failOpenFallback", e);
    }
    sendSyntheticStatus(500, "Internal Server Error"_kj);
  }
}

kj::Exception Fetch::propagate(kj::Exception exception) {
  // Like with an actor, we want to return exceptions back to the caller. This must come after
  // fail-open handling so that fail-open continues to operate normally.
  return exceptionToPropagate(isInternalException, kj::mv(exception));
}

void Fetch::respondWithError(kj::Exception exception) {
  // We're catching the exception and replacing it with 5xx, but metrics should still indicate
  // an exception.
  getMetrics().reportFailure(exception);
  if (exception.getType() == kj::Exception::Type::OVERLOADED) {
    sendSyntheticStatus(503, "Service Unavailable"_kj);
  } else {
    sendSyntheticStatus(500, "Internal Server Error"_kj);
  }
}

}  // namespace workerd::entrypoint
