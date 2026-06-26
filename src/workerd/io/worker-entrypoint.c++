// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-entrypoint.h"

#include <workerd/api/basics.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/util.h>
#include <workerd/io/access-info.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/io/limit-enforcer.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/http-util.h>
#include <workerd/util/sentry.h>
#include <workerd/util/strings.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/uncaught-exception-source.h>
#include <workerd/util/use-perfetto-categories.h>

#include <capnp/message.h>
#include <kj/compat/http.h>

namespace workerd {

namespace {
// Wrapper around a Worker that handles receiving a new event from the outside. In particular,
// this handles:
// - Creating a IoContext and making it current.
// - Executing the worker under lock.
// - Catching exceptions and converting them to HTTP error responses.
//   - Or, falling back to proxying if passThroughOnException() was used.
// - Finish waitUntil() tasks.
class WorkerEntrypoint final: public WorkerInterface {
 public:
  // Call this instead of the constructor. It actually adds a wrapper object around the
  // `WorkerEntrypoint`, but the wrapper still implements `WorkerInterface`.
  //
  // WorkerEntrypoint will create a IoContext, and that IoContext may outlive the
  // WorkerEntrypoint by means of a waitUntil() task. Any object(s) which must be kept alive to
  // support the worker for the lifetime of the IoContext (e.g., subsequent pipeline stages)
  // must be passed in via `ioContextDependency`.
  //
  // If this is NOT a zone worker, then `zoneDefaultWorkerLimits` should be a default instance of
  // WorkerLimits::Reader. Hence this is not necessarily the same as
  // topLevelRequest.getZoneDefaultWorkerLimits(), since the top level request may be shared between
  // zone and non-zone workers.
  static kj::Own<WorkerInterface> construct(ThreadContext& threadContext,
      kj::Own<const Worker> worker,
      kj::Maybe<kj::StringPtr> entrypointName,
      Frankenvalue props,
      kj::Maybe<kj::Own<Worker::Actor>> actor,
      kj::Own<LimitEnforcer> limitEnforcer,
      kj::Own<void> ioContextDependency,
      kj::Own<IoChannelFactory> ioChannelFactory,
      kj::Own<RequestObserver> metrics,
      kj::TaskSet& waitUntilTasks,
      bool tunnelExceptions,
      kj::Maybe<kj::Own<BaseTracer>> workerTracer,
      kj::Maybe<kj::String> cfBlobJson,
      kj::Maybe<Worker::VersionInfo> versionInfo,
      kj::Maybe<tracing::InvocationSpanContext> maybeTriggerInvocationSpan,
      bool isDynamicDispatch,
      kj::Maybe<kj::Own<AccessInfo>> accessInfo,
      kj::Maybe<kj::Own<IoChannelFactory::SelfTokenFactory>> selfTokenFactory,
      Persistent fromPersistentStub);

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override;
  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override;
  kj::Promise<void> prewarm(kj::StringPtr url) override;
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override;
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override;
  kj::Promise<kj::Maybe<kj::Date>> abandonAlarm(kj::Date scheduledTime) override;
  kj::Promise<bool> test() override;
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override;

 private:
  class ResponseSentTracker;

  // Members initialized at startup.

  ThreadContext& threadContext;
  kj::TaskSet& waitUntilTasks;
  kj::Maybe<kj::Canceler&> canceler;
  kj::Maybe<kj::Own<IoContext::IncomingRequest>> incomingRequest;
  bool tunnelExceptions;
  bool isDynamicDispatch;
  kj::Maybe<kj::StringPtr> entrypointName;
  Frankenvalue props;
  kj::Maybe<kj::String> cfBlobJson;
  kj::Maybe<Worker::VersionInfo> versionInfo;

  // Hacky members used to hold some temporary state while processing a request.
  // See gory details in WorkerEntrypoint::request().

  kj::Maybe<kj::Promise<void>> proxyTask;
  kj::Maybe<kj::Own<WorkerInterface>> failOpenService;
  bool loggedExceptionEarlier = false;
  kj::Maybe<jsg::Ref<api::AbortController>> abortController;

  void init(kj::Own<const Worker> worker,
      kj::Maybe<kj::Own<Worker::Actor>> actor,
      kj::Own<LimitEnforcer> limitEnforcer,
      kj::Own<void> ioContextDependency,
      kj::Own<IoChannelFactory> ioChannelFactory,
      kj::Own<RequestObserver> metrics,
      kj::Maybe<kj::Own<BaseTracer>> workerTracer,
      kj::Maybe<tracing::InvocationSpanContext> maybeTriggerInvocationSpan,
      kj::Maybe<kj::Own<AccessInfo>> accessInfo,
      kj::Maybe<kj::Own<IoChannelFactory::SelfTokenFactory>> selfTokenFactory);

  kj::Promise<void> requestImpl(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response);

  kj::Promise<WorkerEntrypoint::AlarmResult> runAlarmImpl(
      kj::Own<IoContext::IncomingRequest> incomingRequest,
      kj::Date scheduledTime,
      uint32_t retryCount);

  template <typename T>
  kj::Promise<T> wrapWithCanceler(kj::Promise<T> promise) {
    KJ_IF_SOME(c, canceler) {
      return c.wrap(kj::mv(promise));
    } else {
      return kj::mv(promise);
    }
  }

 public:  // For kj::heap() only; pretend this is private.
  WorkerEntrypoint(kj::Badge<WorkerEntrypoint> badge,
      ThreadContext& threadContext,
      kj::TaskSet& waitUntilTasks,
      kj::Maybe<kj::Canceler&> canceler,
      bool tunnelExceptions,
      bool isDynamicDispatch,
      kj::Maybe<kj::StringPtr> entrypointName,
      Frankenvalue props,
      kj::Maybe<kj::String> cfBlobJson,
      kj::Maybe<Worker::VersionInfo> versionInfo);
};

// Simple wrapper around `HttpService::Response` to let us know if the response was sent
// already.
class WorkerEntrypoint::ResponseSentTracker final: public kj::HttpService::Response {
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

kj::Own<WorkerInterface> WorkerEntrypoint::construct(ThreadContext& threadContext,
    kj::Own<const Worker> worker,
    kj::Maybe<kj::StringPtr> entrypointName,
    Frankenvalue props,
    kj::Maybe<kj::Own<Worker::Actor>> actor,
    kj::Own<LimitEnforcer> limitEnforcer,
    kj::Own<void> ioContextDependency,
    kj::Own<IoChannelFactory> ioChannelFactory,
    kj::Own<RequestObserver> metrics,
    kj::TaskSet& waitUntilTasks,
    bool tunnelExceptions,
    kj::Maybe<kj::Own<BaseTracer>> workerTracer,
    kj::Maybe<kj::String> cfBlobJson,
    kj::Maybe<Worker::VersionInfo> versionInfo,
    kj::Maybe<tracing::InvocationSpanContext> maybeTriggerInvocationSpan,
    bool isDynamicDispatch,
    kj::Maybe<kj::Own<AccessInfo>> accessInfo,
    kj::Maybe<kj::Own<IoChannelFactory::SelfTokenFactory>> selfTokenFactory,
    Persistent fromPersistentStub) {
  TRACE_EVENT("workerd", "WorkerEntrypoint::construct()");

  // If this request came from a stored ("persistent") stub, re-verify that the target worker still
  // has the `allow_irrevocable_stub_storage` compat flag enabled. The flag may have been removed
  // since the stub was stored, in which case we must reject the request rather than silently
  // honoring a stub that should no longer be reachable. This is the single choke point through
  // which every event type funnels.
  if (fromPersistentStub &&
      !worker->getIsolate().getApi().getFeatureFlags().getAllowIrrevocableStubStorage()) {
    return WorkerInterface::fromException(JSG_KJ_EXCEPTION(FAILED, Error,
        "Invoked a Worker from a stored stub, but the target worker no longer has the "
        "allow_irrevocable_stub_storage compatibility flag enabled."));
  }

  // Arrange to forcefully cancel work when the Actor is aborted.
  kj::Maybe<kj::Canceler&> canceler;
  KJ_IF_SOME(a, actor) {
    canceler = a->getAbortCanceler();
  }

  auto obj = kj::heap<WorkerEntrypoint>(kj::Badge<WorkerEntrypoint>(), threadContext,
      waitUntilTasks, canceler, tunnelExceptions, isDynamicDispatch, entrypointName, kj::mv(props),
      kj::mv(cfBlobJson), kj::mv(versionInfo));
  obj->init(kj::mv(worker), kj::mv(actor), kj::mv(limitEnforcer), kj::mv(ioContextDependency),
      kj::mv(ioChannelFactory), kj::addRef(*metrics), kj::mv(workerTracer),
      kj::mv(maybeTriggerInvocationSpan), kj::mv(accessInfo), kj::mv(selfTokenFactory));
  auto& wrapper = metrics->wrapWorkerInterface(*obj);
  return kj::attachRef(wrapper, kj::mv(obj), kj::mv(metrics));
}

WorkerEntrypoint::WorkerEntrypoint(kj::Badge<WorkerEntrypoint> badge,
    ThreadContext& threadContext,
    kj::TaskSet& waitUntilTasks,
    kj::Maybe<kj::Canceler&> canceler,
    bool tunnelExceptions,
    bool isDynamicDispatch,
    kj::Maybe<kj::StringPtr> entrypointName,
    Frankenvalue props,
    kj::Maybe<kj::String> cfBlobJson,
    kj::Maybe<Worker::VersionInfo> versionInfo)
    : threadContext(threadContext),
      waitUntilTasks(waitUntilTasks),
      canceler(canceler),
      tunnelExceptions(tunnelExceptions),
      isDynamicDispatch(isDynamicDispatch),
      entrypointName(entrypointName),
      props(kj::mv(props)),
      cfBlobJson(kj::mv(cfBlobJson)),
      versionInfo(kj::mv(versionInfo)) {}

void WorkerEntrypoint::init(kj::Own<const Worker> worker,
    kj::Maybe<kj::Own<Worker::Actor>> actor,
    kj::Own<LimitEnforcer> limitEnforcer,
    kj::Own<void> ioContextDependency,
    kj::Own<IoChannelFactory> ioChannelFactory,
    kj::Own<RequestObserver> metrics,
    kj::Maybe<kj::Own<BaseTracer>> workerTracer,
    kj::Maybe<tracing::InvocationSpanContext> maybeTriggerInvocationSpan,
    kj::Maybe<kj::Own<AccessInfo>> accessInfo,
    kj::Maybe<kj::Own<IoChannelFactory::SelfTokenFactory>> selfTokenFactory) {
  TRACE_EVENT("workerd", "WorkerEntrypoint::init()");
  // We need to construct the IoContext -- unless this is an actor and it already has a
  // IoContext, in which case we reuse it.

  auto newContext = [&]() {
    TRACE_EVENT("workerd", "WorkerEntrypoint::init() create new IoContext");
    auto actorRef = actor.map([](kj::Own<Worker::Actor>& ptr) -> Worker::Actor& { return *ptr; });

    // Attaching to refcount instance is safe here since this instance stays alive for the lifetime
    // of the associated WorkerInterface, other references may be created below for actors requests
    // in separate init() calls but this ioContextDependency does not need to live as long as those
    // instances.
    return kj::refcounted<IoContext>(threadContext, kj::mv(worker), actorRef, kj::mv(limitEnforcer))
        .attachToThisReference(kj::mv(ioContextDependency));
  };

  kj::Own<IoContext> context;
  KJ_IF_SOME(a, actor) {
    KJ_IF_SOME(rc, a.get()->getIoContext()) {
      context = kj::addRef(rc);
    } else {
      context = newContext();
      a.get()->setIoContext(kj::addRef(*context));
    }
  } else {
    context = newContext();
  }

  incomingRequest = kj::heap<IoContext::IncomingRequest>(kj::mv(context), kj::mv(ioChannelFactory),
      kj::mv(metrics), kj::mv(workerTracer), kj::mv(maybeTriggerInvocationSpan), kj::mv(accessInfo),
      kj::mv(selfTokenFactory))
                        .attach(kj::mv(actor));
}

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
    auto description = jsg::stripRemoteExceptionPrefix(exception.getDescription());
    if (!description.startsWith("remote.")) {
      // If we already were annotated as remote from some other worker entrypoint, no point
      // adding an additional prefix.
      exception.setDescription(kj::str("remote.", description));
    }
    return kj::mv(exception);
  }
}

kj::Promise<void> WorkerEntrypoint::request(kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody,
    Response& response) {
  return wrapWithCanceler(requestImpl(method, url, headers, requestBody, response));
}

kj::Promise<void> WorkerEntrypoint::requestImpl(kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody,
    Response& response) {
  TRACE_EVENT("workerd", "WorkerEntrypoint::request()", "url", url.cStr(),
      PERFETTO_FLOW_FROM_POINTER(this));

  // ----- Stage 1: Set up per-request state. -----

  auto incomingRequest =
      kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest, "request() can only be called once"));
  this->incomingRequest = kj::none;
  auto& context = incomingRequest->getContext();
  auto wrappedResponse = kj::heap<ResponseSentTracker>(response);
  bool isActor = context.getActor() != kj::none;

  // HACK: Capture workerTracer directly, it's unclear how to acquire the right tracer from context
  // when we need it (for DOs, IoContext may point to a different WorkerTracer by the time we use
  // it). The tracer lives as long or longer than the IoContext (based on being co-owned
  // by IncomingRequest and PipelineTracer) so long enough.
  kj::Maybe<BaseTracer&> workerTracer;
  KJ_IF_SOME(t, incomingRequest->getWorkerTracer()) {
    t.setEventInfo(*incomingRequest, buildFetchEventInfo(method, url, headers, cfBlobJson));
    workerTracer = t;
  }

  incomingRequest->delivered();

  auto metricsForCatch = kj::addRef(incomingRequest->getMetrics());
  auto metricsForProxyTask = kj::addRef(incomingRequest->getMetrics());

  TRACE_EVENT_BEGIN("workerd", "WorkerEntrypoint::request() waiting on context",
      PERFETTO_TRACK_FROM_POINTER(&context), PERFETTO_FLOW_FROM_POINTER(this));

  KJ_TRY {
    // Cancel any in-flight deferred-proxy task on the way out, including on cancellation of this
    // request and including when the fail-open fallback (in the outer KJ_CATCH) runs. This is the
    // outermost cleanup so that the proxy task is never left pinning the IoContext past this
    // function. (It's a no-op when `proxyTask` was never set, e.g. if Stage 2 threw.)
    KJ_DEFER({ proxyTask = kj::none; });

    // ----- Stage 2: Run the JS request handler. -----

    {
      // Drain the incoming request and trigger the client-disconnect abort signal on scope exit
      // (success, failure, or cancellation). This must run regardless of outcome so that the
      // incoming request is always drained and the AbortController is released; it must also run
      // before final error handling so that `failOpenService` is populated when needed.
      KJ_DEFER({
        // The request has been canceled, but allow it to continue executing in the background.
        if (context.isFailOpen()) {
          // Fail-open behavior has been chosen, we'd better save an interface that we can use for
          // that purpose later.
          failOpenService = context.getSubrequestChannelNoChecks(
              IoContext::NEXT_CLIENT_CHANNEL, false, kj::mv(cfBlobJson));
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

        incomingRequest->drain(waitUntilTasks, kj::mv(incomingRequest));
      });

      KJ_TRY {
        api::DeferredProxy<void> deferredProxy =
            co_await context.run([this, &context, method, url, &headers, &requestBody,
                                     &wrappedResponse = *wrappedResponse,
                                     entrypointName = entrypointName](Worker::Lock& lock) mutable {
          TRACE_EVENT_END("workerd", PERFETTO_TRACK_FROM_POINTER(&context));
          TRACE_EVENT(
              "workerd", "WorkerEntrypoint::request() run", PERFETTO_FLOW_FROM_POINTER(this));
          jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);
          jsg::AsyncContextFrame::StorageScope userTraceScope =
              context.makeUserAsyncTraceScope(lock);

          kj::Maybe<jsg::Ref<api::AbortSignal>> signal;
          auto featureFlags = FeatureFlags::get(lock);
          if (featureFlags.getEnableRequestSignal()) {
            auto abortSignalFlag = featureFlags.getRequestSignalPassthrough()
                ? api::AbortSignal::Flag::NONE
                : api::AbortSignal::Flag::IGNORE_FOR_SUBREQUESTS;
            jsg::Lock& js = lock;
            signal.emplace(
                abortController.emplace(js.alloc<api::AbortController>(js, abortSignalFlag))
                    ->getSignal());
          }

          return lock.getGlobalScope().request(method, url, headers, requestBody, wrappedResponse,
              cfBlobJson, lock,
              lock.getExportedHandler(entrypointName, kj::mv(versionInfo), kj::mv(props),
                  context.getActor(), isDynamicDispatch),
              kj::mv(signal));
        });

        // Record the proxy task and the tracer return time on the success path.
        TRACE_EVENT("workerd", "WorkerEntrypoint::request() deferred proxy step",
            PERFETTO_FLOW_FROM_POINTER(this));
        proxyTask = kj::mv(deferredProxy.proxyTask);
        KJ_IF_SOME(t, workerTracer) {
          auto httpResponseStatus = wrappedResponse->getHttpResponseStatus();
          if (httpResponseStatus != 0) {
            t.setReturn(context.now(), tracing::FetchResponseInfo(httpResponseStatus));
          } else {
            t.setReturn(context.now());
          }
        }
      }
      KJ_CATCH(exception) {
        TRACE_EVENT(
            "workerd", "WorkerEntrypoint::request() catch", PERFETTO_FLOW_FROM_POINTER(this));
        // Log JS exceptions to the JS console, if inspector is attached. This also has the effect
        // of logging internal errors to syslog.
        loggedExceptionEarlier = true;
        context.logUncaughtExceptionAsync(
            UncaughtExceptionSource::REQUEST_HANDLER, exception.clone());

        // Do not allow the exception to escape the isolate without waiting for the output gate to
        // open. Note that in the success path, this is taken care of in `FetchEvent::respondWith()`.
        // If the gate is broken, that exception propagates and replaces the original.
        co_await context.waitForOutputLocks();
        TRACE_EVENT("workerd", "WorkerEntrypoint::request() after output lock wait",
            PERFETTO_TERMINATING_FLOW_FROM_POINTER(this));
        // Yield to give a pending cancellation (e.g., the caller dropping our promise because
        // the upstream WebSocket was torn down) a chance to take effect before propagating to
        // the final catch. The original `.then()` chain had an implicit yield point here where
        // the chain crossed into the next `.then` after this catch; without it, downstream
        // observers can mistake a canceled request for one that threw.
        co_await kj::yield();
        kj::throwFatalException(kj::mv(exception));
      }
    }  // Above KJ_DEFER fires here: abort signal + drain.

    // ----- Stage 3: Wait for the deferred-proxy task (if any). -----

    KJ_IF_SOME(p, proxyTask) {
      TRACE_EVENT("workerd", "WorkerEntrypoint::request() finish proxying",
          PERFETTO_TERMINATING_FLOW_FROM_POINTER(this));
      // Now that the IoContext is dropped (unless it had waitUntil()s), we can finish proxying
      // without pinning it or the isolate into memory.
      KJ_TRY {
        co_await p;
      }
      KJ_CATCH(e) {
        metricsForProxyTask->reportFailure(e, RequestObserver::FailureSource::DEFERRED_PROXY);
        // See the matching yield in stage 2's catch.
        co_await kj::yield();
        kj::throwFatalException(kj::mv(e));
      }
    }
  }
  KJ_CATCH(exception) {
    // ----- Stage 4: Handle whatever exception escaped the stages above. -----

    TRACE_EVENT("workerd", "WorkerEntrypoint::request() exception",
        PERFETTO_TERMINATING_FLOW_FROM_POINTER(this));

    auto isInternalException = !jsg::isTunneledException(exception.getDescription()) &&
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

    if (wrappedResponse->isSent()) {
      // Can't fail open if a response was already started.
      failOpenService = kj::none;
    }

    auto sendSyntheticStatus = [&](uint statusCode, kj::StringPtr statusText) {
      if (wrappedResponse->isSent()) return;
      kj::HttpHeaders errorHeaders(threadContext.getHeaderTable());
      wrappedResponse->send(statusCode, statusText, errorHeaders, static_cast<uint64_t>(0));
      KJ_IF_SOME(t, workerTracer) {
        t.setReturn(kj::none, tracing::FetchResponseInfo(wrappedResponse->getHttpResponseStatus()));
      }
    };

    // Decide what to do with the exception. Exactly one of these branches runs:
    //   1. Actor -> tunnel exception back to the caller.
    //   2. Fail-open service configured -> retry the request through it.
    //   3. `tunnelExceptions` set (worker-to-worker) -> tunnel exception back to the caller.
    //   4. Otherwise -> synthesize a 5xx response.

    if (isActor) {
      // TODO(cleanup): We'd really like to tunnel exceptions any time a worker is calling another
      // worker, not just for actors (and W2W below), but getting that right will require cleaning
      // up error handling more generally.
      auto propagated = exceptionToPropagate(isInternalException, kj::mv(exception));
      // See the matching yield in stage 2's catch.
      co_await kj::yield();
      kj::throwFatalException(kj::mv(propagated));
    }

    KJ_IF_SOME(service, failOpenService) {
      // We're catching the exception, but metrics should still indicate an exception.
      metricsForCatch->reportFailure(exception);

      auto serviceOwn = kj::mv(service);
      metricsForCatch->setFailedOpen(true);
      KJ_TRY {
        co_await serviceOwn->request(method, url, headers, requestBody, *wrappedResponse);
      }
      KJ_CATCH(e) {
        metricsForCatch->setFailedOpen(false);
        // Avoid logging recognized external errors here, such as invalid headers returned from
        // the server.
        if (e.getType() != kj::Exception::Type::DISCONNECTED &&
            !jsg::isTunneledException(e.getDescription()) &&
            !jsg::isDoNotLogException(e.getDescription())) {
          LOG_EXCEPTION("failOpenFallback", e);
        }
        sendSyntheticStatus(500, "Internal Server Error"_kj);
      }
      co_return;
    }

    if (tunnelExceptions) {
      // Like with the isActor check, we want to return exceptions back to the caller. This case
      // must happen after fail-open handling so that fail-open continues to operate normally.
      auto propagated = exceptionToPropagate(isInternalException, kj::mv(exception));
      // See the matching yield in stage 2's catch.
      co_await kj::yield();
      kj::throwFatalException(kj::mv(propagated));
    }

    // We're catching the exception and replacing it with 5xx, but metrics should still indicate
    // an exception.
    metricsForCatch->reportFailure(exception);
    if (exception.getType() == kj::Exception::Type::OVERLOADED) {
      sendSyntheticStatus(503, "Service Unavailable"_kj);
    } else {
      sendSyntheticStatus(500, "Internal Server Error"_kj);
    }
  }
}

kj::Promise<void> WorkerEntrypoint::connect(kj::StringPtr host,
    const kj::HttpHeaders& headers,
    kj::AsyncIoStream& connection,
    ConnectResponse& response,
    kj::HttpConnectSettings settings) {
  TRACE_EVENT("workerd", "WorkerEntrypoint::connect()");
  auto incomingRequest =
      kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest, "connect() can only be called once"));
  this->incomingRequest = kj::none;
  auto& context = incomingRequest->getContext();
  auto featureFlags = context.getWorker().getIsolate().getApi().getFeatureFlags();

  if (featureFlags.getConnectPassThrough()) {
    incomingRequest->delivered();

    KJ_DEFER({
      // Since we called incomingRequest->delivered, we are obliged to call `drain()`.
      incomingRequest->drain(waitUntilTasks, kj::mv(incomingRequest));
    });
    // connect_pass_through feature flag means we should just forward the connect request on to
    // the global outbound.

    auto next = context.getSubrequestChannelNoChecks(
        IoContext::NEXT_CLIENT_CHANNEL, false, kj::mv(cfBlobJson));

    // Note: Intentionally return without co_await so that the `incomingRequest` is destroyed,
    //   because we don't have any need to keep the context around.
    return next->connect(host, headers, connection, response, settings);
  } else if (!featureFlags.getWorkerdExperimental()) {
    JSG_FAIL_REQUIRE(TypeError, "Incoming CONNECT on a worker not supported");
  }

  // TODO(soon): Implement basic TLS support for connect handler.
  JSG_REQUIRE(!settings.useTls, Error, "Incoming CONNECT with TLS not supported");
  // Capture workerTracer, see request() for rationale.
  kj::Maybe<BaseTracer&> workerTracer;

  bool isActor = context.getActor() != kj::none;

  KJ_IF_SOME(t, incomingRequest->getWorkerTracer()) {
    t.setEventInfo(*incomingRequest, tracing::ConnectEventInfo());
    workerTracer = t;
  }
  incomingRequest->delivered();

  auto metricsForCatch = kj::addRef(incomingRequest->getMetrics());

  return wrapWithCanceler(
      context
          .run([this, &headers, &context, &connection, &response, entrypointName = entrypointName,
                   versionInfo = kj::mv(versionInfo),
                   host = kj::str(host)](Worker::Lock& lock) mutable {
    jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);
    jsg::AsyncContextFrame::StorageScope userTraceScope = context.makeUserAsyncTraceScope(lock);

    return lock.getGlobalScope().connect(kj::mv(host), headers, connection, response, lock,
        lock.getExportedHandler(entrypointName, kj::mv(versionInfo), kj::mv(props),
            context.getActor(), isDynamicDispatch));
  })
          .then([&context, workerTracer]() {
    KJ_IF_SOME(t, workerTracer) {
      t.setReturn(context.now());
    }
  })
          .catch_([this, &context](kj::Exception&& exception) mutable -> kj::Promise<void> {
    // Log JS exceptions to the JS console, if inspector is attached. This also has the effect of
    // logging internal errors to syslog.
    loggedExceptionEarlier = true;
    context.logUncaughtExceptionAsync(UncaughtExceptionSource::REQUEST_HANDLER, exception.clone());

    // Do not allow the exception to escape the isolate without waiting for the output gate to
    // open. Note that in the success path, this is taken care of in `FetchEvent::respondWith()`.
    return context.waitForOutputLocks().then(
        [exception = kj::mv(exception)]() mutable -> kj::Promise<void> {
      return kj::mv(exception);
    });
  })
          .attach(kj::defer([this, incomingRequest = kj::mv(incomingRequest)]() mutable {
    // The request has been canceled, but allow it to continue executing in the background.
    incomingRequest->drain(waitUntilTasks, kj::mv(incomingRequest));
  }))
          .catch_([this, isActor, &response, metrics = kj::mv(metricsForCatch), workerTracer](
                      kj::Exception&& exception) mutable -> kj::Promise<void> {
    // Don't return errors to end user.
    auto isInternalException = !jsg::isTunneledException(exception.getDescription()) &&
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

    if (isActor || tunnelExceptions) {
      // We want to tunnel exceptions from actors back to the caller.
      // TODO(cleanup): We'd really like to tunnel exceptions any time a worker is calling another
      // worker, not just for actors (and W2W below), but getting that right will require cleaning
      // up error handling more generally.
      return exceptionToPropagate(isInternalException, kj::mv(exception));
    } else {
      // Return error.

      // We're catching the exception and replacing it with 5xx, but metrics should still indicate
      // an exception.
      metrics->reportFailure(exception);

      kj::HttpHeaders headers(threadContext.getHeaderTable());
      if (exception.getType() == kj::Exception::Type::OVERLOADED) {
        response.reject(503, "Service Unavailable", headers, static_cast<uint64_t>(0));
      } else {
        response.reject(500, "Internal Server Error", headers, static_cast<uint64_t>(0));
      }
      // TODO(o11y): Should we also indicate a return response code for TCP?
      KJ_IF_SOME(t, workerTracer) {
        t.setReturn(kj::none);
      }

      return kj::READY_NOW;
    }
  }));
}

kj::Promise<void> WorkerEntrypoint::prewarm(kj::StringPtr url) {
  // Nothing to do, the worker is already loaded.
  TRACE_EVENT("workerd", "WorkerEntrypoint::prewarm()", "url", url.cStr());
  auto incomingRequest =
      kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest, "prewarm() can only be called once"));
  incomingRequest->getMetrics().setIsPrewarm();

  // Intentionally don't call incomingRequest->delivered() for prewarm requests and do not create
  // an Onset event, prewarm is not being traced.

  // TODO(someday): Ideally, middleware workers would forward prewarm() to the next stage. At
  //   present we don't have a good way to decide what stage that is, especially given that we'll
  //   be switching to `next` being a binding in the future.
  return kj::READY_NOW;
}

kj::Promise<WorkerInterface::ScheduledResult> WorkerEntrypoint::runScheduled(
    kj::Date scheduledTime, kj::StringPtr cron) {
  TRACE_EVENT("workerd", "WorkerEntrypoint::runScheduled()");
  auto incomingRequest =
      kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest, "runScheduled() can only be called once"));
  this->incomingRequest = kj::none;
  auto& context = incomingRequest->getContext();

  KJ_ASSERT(context.getActor() == kj::none);
  // This code currently doesn't work with actors because cancellations occur immediately, without
  // calling context->drain(). We don't ever send scheduled events to actors. If we do, we'll have
  // to think more about this.

  double eventTime = (scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS;

  KJ_IF_SOME(t, incomingRequest->getWorkerTracer()) {
    t.setEventInfo(*incomingRequest, tracing::ScheduledEventInfo(eventTime, kj::str(cron)));
  }

  incomingRequest->delivered();

  // Scheduled handlers run entirely in waitUntil() tasks.
  context.addWaitUntil(
      context.run([scheduledTime, cron, entrypointName = entrypointName,
                      versionInfo = kj::mv(versionInfo), props = kj::mv(props), &context,
                      &metrics = incomingRequest->getMetrics()](Worker::Lock& lock) mutable {
    TRACE_EVENT("workerd", "WorkerEntrypoint::runScheduled() run");
    jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);
    jsg::AsyncContextFrame::StorageScope userTraceScope = context.makeUserAsyncTraceScope(lock);

    lock.getGlobalScope().startScheduled(scheduledTime, cron, lock,
        lock.getExportedHandler(
            entrypointName, kj::mv(versionInfo), kj::mv(props), context.getActor()));
  }));

  static auto constexpr waitForFinished = [](kj::Own<IoContext::IncomingRequest> request)
      -> kj::Promise<WorkerInterface::ScheduledResult> {
    TRACE_EVENT("workerd", "WorkerEntrypoint::runScheduled() waitForFinished()");
    return request->finishScheduled(kj::mv(request));
  };

  return wrapWithCanceler(waitForFinished(kj::mv(incomingRequest)));
}

kj::Promise<WorkerInterface::AlarmResult> WorkerEntrypoint::runAlarmImpl(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Date scheduledTime,
    uint32_t retryCount) {
  // We want to de-duplicate alarm requests as follows:
  // - An alarm must not be canceled once it is running, UNLESS the whole actor is shut down.
  // - If multiple alarm invocations arrive with the same scheduled time, we only run one.
  // - If we are asked to schedule an alarm while one is running, we wait for the running alarm to
  //   finish.
  // - However, we schedule no more than one alarm. If another one (with yet another different
  //   scheduled time) arrives while we still have one running and one scheduled, we discard the
  //   previous scheduled alarm.

  TRACE_EVENT("workerd", "WorkerEntrypoint::runAlarmImpl()");

  auto& context = incomingRequest->getContext();
  auto& actor = KJ_REQUIRE_NONNULL(context.getActor(), "alarm() should only work with actors");

  KJ_IF_SOME(promise, actor.getAlarm(scheduledTime)) {
    // There is a pre-existing alarm for `scheduledTime`, we can just wait for its result.
    // TODO(someday) If the request responsible for fulfilling this alarm were to be cancelled, then
    // we could probably take over and try to fulfill it ourselves. Maybe we'd want to loop on
    // `actor.getAlarm()`? We'd have to distinguish between rescheduling and request cancellation.
    auto outcome = co_await promise;
    co_return AlarmResult{.retry = outcome.retry,
      .retryCountsAgainstLimit = outcome.retryCountsAgainstLimit,
      .outcome = outcome.outcome};
  }

  // There isn't a pre-existing alarm, we can set event info and call `delivered()` (which emits
  // metrics events).
  KJ_IF_SOME(t, incomingRequest->getWorkerTracer()) {
    t.setEventInfo(*incomingRequest, tracing::AlarmEventInfo(scheduledTime));
  }

  incomingRequest->delivered();

  auto scheduleAlarmResult = co_await actor.scheduleAlarm(scheduledTime);
  KJ_SWITCH_ONEOF(scheduleAlarmResult) {
    KJ_CASE_ONEOF(af, WorkerInterface::AlarmFulfiller) {
      // We're now in charge of running this alarm!
      auto cancellationGuard = kj::defer([&af]() {
        // Our promise chain was cancelled, let's cancel our fulfiller for any other requests
        // that were waiting on us.
        af.cancel();
      });

      KJ_DEFER({
        // The alarm has finished but allow the request to continue executing in the background.
        incomingRequest->drain(waitUntilTasks, kj::mv(incomingRequest));
      });

      try {
        auto result =
            co_await context.run([scheduledTime, retryCount, entrypointName = entrypointName,
                                     versionInfo = kj::mv(versionInfo), props = kj::mv(props),
                                     &context](Worker::Lock& lock) mutable {
          jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);
          jsg::AsyncContextFrame::StorageScope userTraceScope =
              context.makeUserAsyncTraceScope(lock);

          // If we have an invalid timeout, set it to the default value of 15 minutes.
          auto timeout = context.getLimitEnforcer().getAlarmLimit();
          if (timeout == 0 * kj::MILLISECONDS) {
            LOG_NOSENTRY(WARNING, "Invalid alarm timeout value. Using 15 minutes", timeout);
            timeout = 15 * kj::MINUTES;
          }

          auto handler = lock.getExportedHandler(
              entrypointName, kj::mv(versionInfo), kj::mv(props), context.getActor());
          return lock.getGlobalScope().runAlarm(scheduledTime, timeout, retryCount, lock, handler);
        });

        // The alarm handler was successfully complete. We must guarantee this same alarm does not
        // run again.
        if (result.outcome == EventOutcome::OK) {
          // When an alarm handler completes its execution, the alarm is marked ready for deletion in
          // actor-cache. This alarm change will only be reflected in the alarmsXX table, once cache
          // flushes and changes are written to storage.
          // If there are any pending flushes, they are locked with the actor output gate until
          // they complete. We should wait until the output gate locks are released.
          // If we don't wait, it's possible for alarm manager to pull the wrong alarm value (the
          // same alarm that just completed) from storage before these changes are actually made,
          // rerunning it, when it shouldn't.
          co_await actor.getOutputGate().wait(context.getCurrentTraceSpan());
        }

        // We succeeded, inform any other entrypoints that may be waiting upon us.
        af.fulfill(result.asOutcome());
        cancellationGuard.cancel();
        co_return kj::mv(result);
      } catch (const kj::Exception& e) {
        // We failed, inform any other entrypoints that may be waiting upon us.
        af.reject(e);
        cancellationGuard.cancel();
        throw;
      }
    }
    KJ_CASE_ONEOF(outcome, WorkerInterface::AlarmOutcome) {
      // The alarm was cancelled while we were waiting to run, go ahead and return the result.
      co_return AlarmResult{.retry = outcome.retry,
        .retryCountsAgainstLimit = outcome.retryCountsAgainstLimit,
        .outcome = outcome.outcome};
    }
  }

  KJ_UNREACHABLE;
}

kj::Promise<WorkerInterface::AlarmResult> WorkerEntrypoint::runAlarm(
    kj::Date scheduledTime, uint32_t retryCount) {
  TRACE_EVENT("workerd", "WorkerEntrypoint::runAlarm()");
  auto incomingRequest =
      kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest, "runAlarm() can only be called once"));
  this->incomingRequest = kj::none;

  auto& context = incomingRequest->getContext();
  auto result =
      co_await wrapWithCanceler(runAlarmImpl(kj::mv(incomingRequest), scheduledTime, retryCount));
  KJ_IF_SOME(t, context.getWorkerTracer()) {
    t.setReturn(context.now());
  }
  co_return result;
}

kj::Promise<kj::Maybe<kj::Date>> WorkerEntrypoint::abandonAlarm(kj::Date scheduledTime) {
  TRACE_EVENT("workerd", "WorkerEntrypoint::abandonAlarm()");
  // This does not require running the user's alarm handler -- it's a pure actor-state cleanup.
  // Access the actor directly from the IoContext without going through the JS dispatch machinery.
  auto& req =
      KJ_REQUIRE_NONNULL(incomingRequest, "abandonAlarm() called without an incoming request");
  auto& actor = KJ_REQUIRE_NONNULL(
      req->getContext().getActor(), "abandonAlarm() should only work with actors");
  auto& persistent = KJ_REQUIRE_NONNULL(
      actor.getPersistent(), "abandonAlarm() requires actor with persistent storage");
  return persistent.abandonAlarm(scheduledTime);
}

kj::Promise<bool> WorkerEntrypoint::test() {
  TRACE_EVENT("workerd", "WorkerEntrypoint::test()");
  auto incomingRequest =
      kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest, "test() can only be called once"));
  this->incomingRequest = kj::none;
  auto& context = incomingRequest->getContext();
  KJ_IF_SOME(t, incomingRequest->getWorkerTracer()) {
    t.setEventInfo(*incomingRequest, tracing::CustomEventInfo());
  }

  incomingRequest->delivered();

  context.addWaitUntil(
      context.run([entrypointName = entrypointName, versionInfo = kj::mv(versionInfo),
                      props = kj::mv(props), &context, &metrics = incomingRequest->getMetrics()](
                      Worker::Lock& lock) mutable -> kj::Promise<void> {
    TRACE_EVENT("workerd", "WorkerEntrypoint::test() run");
    jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);
    jsg::AsyncContextFrame::StorageScope userTraceScope = context.makeUserAsyncTraceScope(lock);

    return context.awaitJs(lock,
        lock.getGlobalScope().test(lock,
            lock.getExportedHandler(
                entrypointName, kj::mv(versionInfo), kj::mv(props), context.getActor())));
  }));

  static auto constexpr waitForFinished =
      [](kj::Own<IoContext::IncomingRequest> request) -> kj::Promise<bool> {
    TRACE_EVENT("workerd", "WorkerEntrypoint::test() waitForFinished()");

    auto scheduledResult = co_await request->finishScheduled(kj::mv(request));

    // Not adding a return event here – we only provide rudimentary tracing support for test events
    // (enough so that we can get logs/spans from them in wd-tests), so this is not needed in
    // practice.

    co_return scheduledResult.outcome == EventOutcome::OK;
  };

  return wrapWithCanceler(waitForFinished(kj::mv(incomingRequest)));
}

kj::Promise<WorkerInterface::CustomEvent::Result> WorkerEntrypoint::customEvent(
    kj::Own<CustomEvent> event) {
  TRACE_EVENT("workerd", "WorkerEntrypoint::customEvent()", "type", event->getType());
  auto incomingRequest =
      kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest, "customEvent() can only be called once"));
  this->incomingRequest = kj::none;

  // Set event info BEFORE calling run() to ensure onset event is reported before
  // any user code executes (particularly important for actors whose constructors may run
  // during delivered()).
  KJ_IF_SOME(t, incomingRequest->getWorkerTracer()) {
    t.setEventInfo(*incomingRequest, event->getEventInfo());
  }

  return wrapWithCanceler(event
                              ->run(kj::mv(incomingRequest), entrypointName, kj::mv(versionInfo),
                                  kj::mv(props), waitUntilTasks, isDynamicDispatch)
                              .attach(kj::mv(event)));
}

}  // namespace

kj::Own<WorkerInterface> newWorkerEntrypoint(ThreadContext& threadContext,
    kj::Own<const Worker> worker,
    kj::Maybe<kj::StringPtr> entrypointName,
    Frankenvalue props,
    kj::Maybe<kj::Own<Worker::Actor>> actor,
    kj::Own<LimitEnforcer> limitEnforcer,
    kj::Own<void> ioContextDependency,
    kj::Own<IoChannelFactory> ioChannelFactory,
    kj::Own<RequestObserver> metrics,
    kj::TaskSet& waitUntilTasks,
    bool tunnelExceptions,
    kj::Maybe<kj::Own<BaseTracer>> workerTracer,
    kj::Maybe<kj::String> cfBlobJson,
    kj::Maybe<Worker::VersionInfo> versionInfo,
    kj::Maybe<tracing::InvocationSpanContext> maybeTriggerInvocationSpan,
    bool isDynamicDispatch,
    kj::Maybe<kj::Own<AccessInfo>> accessInfo,
    kj::Maybe<kj::Own<IoChannelFactory::SelfTokenFactory>> selfTokenFactory,
    Persistent fromPersistentStub) {
  return WorkerEntrypoint::construct(threadContext, kj::mv(worker), kj::mv(entrypointName),
      kj::mv(props), kj::mv(actor), kj::mv(limitEnforcer), kj::mv(ioContextDependency),
      kj::mv(ioChannelFactory), kj::mv(metrics), waitUntilTasks, tunnelExceptions,
      kj::mv(workerTracer), kj::mv(cfBlobJson), kj::mv(versionInfo),
      kj::mv(maybeTriggerInvocationSpan), isDynamicDispatch, kj::mv(accessInfo),
      kj::mv(selfTokenFactory), fromPersistentStub);
}

}  // namespace workerd
