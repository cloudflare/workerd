// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-entrypoint.h"
#include <capnp/message.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/sentry.h>
#include <workerd/api/global-scope.h>
#include <workerd/util/thread-scopes.h>

namespace workerd {

class WorkerEntrypoint::ResponseSentTracker final: public kj::HttpService::Response {
  // Simple wrapper around `HttpService::Response` to let us know if the response was sent
  // already.
public:
  ResponseSentTracker(kj::HttpService::Response& inner)
      : inner(inner) {}
  KJ_DISALLOW_COPY_AND_MOVE(ResponseSentTracker);

  bool isSent() const { return sent; }

  kj::Own<kj::AsyncOutputStream> send(
      uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = nullptr) override {
    sent = true;
    return inner.send(statusCode, statusText, headers, expectedBodySize);
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    sent = true;
    return inner.acceptWebSocket(headers);
  }

private:
  kj::HttpService::Response& inner;
  bool sent = false;
};

kj::Own<WorkerInterface> WorkerEntrypoint::construct(
                                   ThreadContext& threadContext,
                                   kj::Own<const Worker> worker,
                                   kj::Maybe<kj::StringPtr> entrypointName,
                                   kj::Maybe<kj::Own<Worker::Actor>> actor,
                                   kj::Own<LimitEnforcer> limitEnforcer,
                                   kj::Own<void> ioContextDependency,
                                   kj::Own<IoChannelFactory> ioChannelFactory,
                                   kj::Own<RequestObserver> metrics,
                                   kj::TaskSet& waitUntilTasks,
                                   bool tunnelExceptions,
                                   kj::Maybe<kj::Own<WorkerTracer>> workerTracer,
                                   kj::Maybe<kj::String> cfBlobJson) {
  auto obj = kj::heap<WorkerEntrypoint>(kj::Badge<WorkerEntrypoint>(), threadContext,
      waitUntilTasks, tunnelExceptions, entrypointName, kj::mv(cfBlobJson));
  obj->init(kj::mv(worker), kj::mv(actor), kj::mv(limitEnforcer),
      kj::mv(ioContextDependency), kj::mv(ioChannelFactory), kj::addRef(*metrics),
      kj::mv(workerTracer));
  auto& wrapper = metrics->wrapWorkerInterface(*obj);
  return kj::attachRef(wrapper, kj::mv(obj), kj::mv(metrics));
}

WorkerEntrypoint::WorkerEntrypoint(kj::Badge<WorkerEntrypoint> badge,
                                   ThreadContext& threadContext,
                                   kj::TaskSet& waitUntilTasks,
                                   bool tunnelExceptions,
                                   kj::Maybe<kj::StringPtr> entrypointName,
                                   kj::Maybe<kj::String> cfBlobJson)
    : threadContext(threadContext),
      waitUntilTasks(waitUntilTasks),
      tunnelExceptions(tunnelExceptions),
      entrypointName(entrypointName),
      cfBlobJson(kj::mv(cfBlobJson)) {}

void WorkerEntrypoint::init(
    kj::Own<const Worker> worker,
    kj::Maybe<kj::Own<Worker::Actor>> actor,
    kj::Own<LimitEnforcer> limitEnforcer,
    kj::Own<void> ioContextDependency,
    kj::Own<IoChannelFactory> ioChannelFactory,
    kj::Own<RequestObserver> metrics,
    kj::Maybe<kj::Own<WorkerTracer>> workerTracer) {
  // We need to construct the IoContext -- unless this is an actor and it already has a
  // IoContext, in which case we reuse it.

  auto newContext = [&]() {
    auto actorRef = actor.map([](kj::Own<Worker::Actor>& ptr) -> Worker::Actor& {
      return *ptr;
    });

    return kj::refcounted<IoContext>(
        threadContext, kj::mv(worker), actorRef, kj::mv(limitEnforcer))
            .attach(kj::mv(ioContextDependency));
  };

  kj::Own<IoContext> context;
  KJ_IF_MAYBE(a, actor) {
    KJ_IF_MAYBE(rc, a->get()->getIoContext()) {
      context = kj::addRef(*rc);
    } else {
      context = newContext();
      a->get()->setIoContext(kj::addRef(*context));
    }
  } else {
    context = newContext();
  }

  incomingRequest = kj::heap<IoContext::IncomingRequest>(
      kj::mv(context), kj::mv(ioChannelFactory), kj::mv(metrics),
      kj::mv(workerTracer))
      .attach(kj::mv(actor));
}

kj::Promise<void> WorkerEntrypoint::request(
    kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody, Response& response) {
  auto incomingRequest = kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest,
                                "request() can only be called once"));
  this->incomingRequest = nullptr;
  incomingRequest->delivered();
  auto& context = incomingRequest->getContext();

  auto wrappedResponse = kj::heap<ResponseSentTracker>(response);

  bool isActor = context.getActor() != nullptr;

  KJ_IF_MAYBE(t, incomingRequest->getWorkerTracer()) {
    auto timestamp = context.now();
    kj::String cfJson;
    KJ_IF_MAYBE(c, cfBlobJson) {
      cfJson = kj::str(*c);
    }

    // To match our historical behavior (when we used to pull the headers from the JavaScript
    // object later on), we need to canonicalize the headers, including:
    // - Lower-case the header name.
    // - Combine multiple headers with the same name into a comma-delimited list. (This explicitly
    //   breaks the Set-Cookie header, incidentally, but should be equivalent for all other
    //   headers.)
    kj::TreeMap<kj::String, kj::Vector<kj::StringPtr>> traceHeaders;
    headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
      kj::String lower = api::toLower(name);
      auto& slot = traceHeaders.findOrCreate(lower,
          [&]() { return decltype(traceHeaders)::Entry {kj::mv(lower), {}}; });
      slot.add(value);
    });
    auto traceHeadersArray = KJ_MAP(entry, traceHeaders) {
      return Trace::FetchEventInfo::Header(kj::mv(entry.key),
          kj::strArray(entry.value, ", "));
    };

    t->setEventInfo(timestamp, Trace::FetchEventInfo(method, kj::str(url),
        kj::mv(cfJson), kj::mv(traceHeadersArray)));
  }

  auto metricsForCatch = kj::addRef(incomingRequest->getMetrics());

  return context.run(
      [this, &context, method, url, &headers, &requestBody,
       &metrics = incomingRequest->getMetrics(),
       &wrappedResponse = *wrappedResponse, entrypointName = entrypointName]
      (Worker::Lock& lock) mutable {
    jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);

    return lock.getGlobalScope().request(
        method, url, headers, requestBody, wrappedResponse,
        cfBlobJson, lock, lock.getExportedHandler(entrypointName, context.getActor()));
  }).then([this](api::DeferredProxy<void> deferredProxy) {
    proxyTask = kj::mv(deferredProxy.proxyTask);
  }).exclusiveJoin(context.onAbort())
      .catch_([this,&context](kj::Exception&& exception) mutable -> kj::Promise<void> {
    // Log JS exceptions to the JS console, if fiddle is attached. This also has the effect of
    // logging internal errors to syslog.
    loggedExceptionEarlier = true;
    context.logUncaughtExceptionAsync(UncaughtExceptionSource::REQUEST_HANDLER,
                                      kj::cp(exception));

    // Do not allow the exception to escape the isolate without waiting for the output gate to
    // open. Note that in the success path, this is taken care of in `FetchEvent::respondWith()`.
    return context.waitForOutputLocks()
        .then([exception = kj::mv(exception)]() mutable
              -> kj::Promise<void> { return kj::mv(exception); });
  }).attach(kj::defer([this,incomingRequest = kj::mv(incomingRequest),&context]() mutable {
    // The request has been canceled, but allow it to continue executing in the background.
    if (context.isFailOpen()) {
      // Fail-open behavior has been chosen, we'd better save an HttpClient that we can use for
      // that purpose later.
      failOpenClient = context.getHttpClientNoChecks(IoContext::NEXT_CLIENT_CHANNEL, false,
                                                     kj::mv(cfBlobJson));
    }
    auto promise = incomingRequest->drain().attach(kj::mv(incomingRequest));
    maybeAddGcPassForTest(context, promise);
    waitUntilTasks.add(kj::mv(promise));
  })).then([this]() -> kj::Promise<void> {
    // Now that the IoContext is dropped (unless it had waitUntil()s), we can finish proxying
    // without pinning it or the isolate into memory.
    KJ_IF_MAYBE(p, proxyTask) {
      return kj::mv(*p);
    } else {
      return kj::READY_NOW;
    }
  }).attach(kj::defer([this]() mutable {
    // If we're being cancelled, we need to make sure `proxyTask` gets canceled.
    proxyTask = nullptr;
  })).catch_([this,wrappedResponse = kj::mv(wrappedResponse),isActor,
              method, url, &headers, &requestBody, metrics = kj::mv(metricsForCatch)]
             (kj::Exception&& exception) mutable -> kj::Promise<void> {
    // Don't return errors to end user.

    auto isInternalException = !jsg::isTunneledException(exception.getDescription())
        && !jsg::isDoNotLogException(exception.getDescription());
    if (!loggedExceptionEarlier) {
      // This exception seems to have originated during the deferred proxy task, so it was not
      // logged to the IoContext earlier.
      if (exception.getType() != kj::Exception::Type::DISCONNECTED && isInternalException) {
        LOG_EXCEPTION("workerEntrypoint", exception);
      } else {
        KJ_LOG(INFO, exception);  // Run with --verbose to see exception logs.
      }
    }

    auto exceptionToPropagate = [&]() {
      if (isInternalException) {
        // We've already logged it here, the only thing that matters to the client is that we failed
        // due to an internal error. Note that this does not need to be labeled "remote." since jsg
        // will sanitize it as an internal error. Note that we use `setDescription()` to preserve
        // the exception type for `cjfs::makeInternalError(...)` downstream.
        exception.setDescription(kj::str(
            "worker_do_not_log; Request failed due to internal error"));
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

    };

    if (wrappedResponse->isSent()) {
      // We can't fail open if the response was already sent, so set `failOpenClient` null so that
      // that branch isn't taken below.
      failOpenClient = nullptr;
    }

    if (isActor) {
      // We want to tunnel exceptions from actors back to the caller.
      // TODO(cleanup): We'd really like to tunnel exceptions any time a worker is calling another
      // worker, not just for actors (and W2W below), but getting that right will require cleaning
      // up error handling more generally.
      return exceptionToPropagate();
    } else KJ_IF_MAYBE(client, failOpenClient) {
      // Fall back to origin.

      // We're catching the exception, but metrics should still indicate an exception.
      metrics->reportFailure(exception);

      auto promise = kj::evalNow([&] {
        // kj::newHttpService adapts an HttpClient to look like an HttpService which makes it
        // easier to forward the call.
        auto httpWrapper = kj::newHttpService(**client);
        auto promise = httpWrapper->request(
            method, url, headers, requestBody, *wrappedResponse);
        metrics->setFailedOpen(true);
        return promise.attach(kj::mv(httpWrapper), kj::mv(*client));
      });
      return promise.catch_([this,wrappedResponse = kj::mv(wrappedResponse),
                             metrics = kj::mv(metrics)]
                            (kj::Exception&& e) mutable {
        metrics->setFailedOpen(false);
        if (e.getType() != kj::Exception::Type::DISCONNECTED &&
            // Avoid logging recognized external errors here, such as invalid headers returned from
            // the server.
            !jsg::isTunneledException(e.getDescription()) &&
            !jsg::isDoNotLogException(e.getDescription())) {
          LOG_EXCEPTION("failOpenFallback", e);
        }
        if (!wrappedResponse->isSent()) {
          kj::HttpHeaders headers(threadContext.getHeaderTable());
          wrappedResponse->send(500, "Internal Server Error", headers, uint64_t(0));
        }
      });
    } else if (tunnelExceptions) {
      // Like with the isActor check, we want to return exceptions back to the caller.
      // We don't want to handle this case the same as the isActor case though, since we want
      // fail-open to operate normally, which means this case must happen after fail-open handling.
      return exceptionToPropagate();
    } else {
      // Return error.

      // We're catching the exception and replacing it with 5xx, but metrics should still indicate
      // an exception.
      metrics->reportFailure(exception);

      // We can't send an error response if a response was already started; we can only drop the
      // connection in that case.
      if (!wrappedResponse->isSent()) {
        kj::HttpHeaders headers(threadContext.getHeaderTable());
        if (exception.getType() == kj::Exception::Type::OVERLOADED) {
          wrappedResponse->send(503, "Service Unavailable", headers, uint64_t(0));
        } else {
          wrappedResponse->send(500, "Internal Server Error", headers, uint64_t(0));
        }
      }

      return kj::READY_NOW;
    }
  });
}

kj::Promise<void> WorkerEntrypoint::connect(kj::StringPtr host, const kj::HttpHeaders& headers,
    kj::AsyncIoStream& connection, ConnectResponse& response,
    kj::HttpConnectSettings settings) {
  JSG_FAIL_REQUIRE(TypeError, "Incoming CONNECT on a worker not supported");
}

void WorkerEntrypoint::prewarm(kj::StringPtr url) {
  // Nothing to do, the worker is already loaded.

  auto incomingRequest = kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest,
                                "prewarm() can only be called once"));
  incomingRequest->getMetrics().setIsPrewarm();

  // Intentionally don't call incomingRequest->delivered() for prewarm requests.

  // TODO(someday): Ideally, middleware workers would forward prewarm() to the next stage. At
  //   present we don't have a good way to decide what stage that is, especially given that we'll
  //   be switching to `next` being a binding in the future.
}

kj::Promise<WorkerInterface::ScheduledResult> WorkerEntrypoint::runScheduled(
    kj::Date scheduledTime,
    kj::StringPtr cron) {
  auto incomingRequest = kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest,
                                "runScheduled() can only be called once"));
  this->incomingRequest = nullptr;
  incomingRequest->delivered();
  auto& context = incomingRequest->getContext();

  KJ_ASSERT(context.getActor() == nullptr);
  // This code currently doesn't work with actors because cancellations occur immediately, without
  // calling context->drain(). We don't ever send scheduled events to actors. If we do, we'll have
  // to think more about this.

  KJ_IF_MAYBE(t, context.getWorkerTracer()) {
    double eventTime = (scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS;
    t->setEventInfo(context.now(), Trace::ScheduledEventInfo(eventTime, kj::str(cron)));
  }

  // Scheduled handlers run entirely in waitUntil() tasks.
  context.addWaitUntil(context.run(
      [scheduledTime, cron, entrypointName=entrypointName, &context,
       &metrics = incomingRequest->getMetrics()]
      (Worker::Lock& lock) mutable {
    jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);

    lock.getGlobalScope().startScheduled(scheduledTime, cron, lock,
        lock.getExportedHandler(entrypointName, context.getActor()));
  }));

  auto promise = incomingRequest->finishScheduled().then([&context](bool completed) mutable {
    return WorkerInterface::ScheduledResult {
      .retry = context.shouldRetryScheduled(),
      .outcome = completed ? context.waitUntilStatus() : EventOutcome::EXCEEDED_CPU
    };
  }).attach(kj::mv(incomingRequest));

  maybeAddGcPassForTest(context, promise);

  return promise;
}

kj::Promise<WorkerInterface::AlarmResult> WorkerEntrypoint::runAlarm(
    kj::Date scheduledTime) {
  auto incomingRequest = kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest,
                                "runAlarm() can only be called once"));
  this->incomingRequest = nullptr;
  // Note: Don't call incomingRequest->delivered() until we've de-duped (later on).
  auto& context = incomingRequest->getContext();

  //alarm() should only work with actors
  auto& actor = KJ_REQUIRE_NONNULL(context.getActor());

  auto promise = actor.dedupAlarm(scheduledTime,
      [this,&context,scheduledTime,incomingRequest = kj::mv(incomingRequest)]() mutable
      -> kj::Promise<WorkerInterface::AlarmResult> {
    incomingRequest->delivered();

    KJ_IF_MAYBE(t, incomingRequest->getWorkerTracer()) {
      t->setEventInfo(context.now(), Trace::AlarmEventInfo(scheduledTime));
    }

    // Date.now() < scheduledTime when the alarm comes in, since we subtract
    // elapsed CPU time from the time of last I/O in the implementation of Date.now().
    // This difference could be used to implement a spectre timer, so we have to wait a little
    // longer until Date.now() = scheduledTime.
    return context.atTime(scheduledTime).then(
        [this, scheduledTime, &context, incomingRequest = kj::mv(incomingRequest)]() mutable {
      return context.run(
          [scheduledTime, entrypointName=entrypointName, &context,
           &metrics = incomingRequest->getMetrics()]
          (Worker::Lock& lock){
        jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);

        return lock.getGlobalScope().runAlarm(scheduledTime, lock,
            lock.getExportedHandler(entrypointName, context.getActor()));
      }).attach(kj::defer([this, incomingRequest = kj::mv(incomingRequest)]() mutable {
        // The alarm has finished but allow the request to continue executing in the background.
        waitUntilTasks.add(incomingRequest->drain().attach(kj::mv(incomingRequest)));
      }));
    });
  });

  maybeAddGcPassForTest(context, promise);

  return promise;
}

kj::Promise<bool> WorkerEntrypoint::test() {
  auto incomingRequest = kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest,
                                "test() can only be called once"));
  this->incomingRequest = nullptr;
  incomingRequest->delivered();

  auto& context = incomingRequest->getContext();

  context.addWaitUntil(context.run(
      [entrypointName=entrypointName, &context, &metrics = incomingRequest->getMetrics()]
      (Worker::Lock& lock) mutable -> kj::Promise<void> {
    jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);

    return context.awaitJs(lock.getGlobalScope()
        .test(lock, lock.getExportedHandler(entrypointName, context.getActor())));
  }));

  auto promise = incomingRequest->finishScheduled().then([&context](bool completed) mutable {
    auto outcome = completed ? context.waitUntilStatus() : EventOutcome::EXCEEDED_CPU;
    return outcome == EventOutcome::OK;
  }).attach(kj::mv(incomingRequest));

  maybeAddGcPassForTest(context, promise);
  return promise;
}

kj::Promise<WorkerInterface::CustomEvent::Result>
    WorkerEntrypoint::customEvent(kj::Own<CustomEvent> event) {
  auto incomingRequest = kj::mv(KJ_REQUIRE_NONNULL(this->incomingRequest,
                                "customEvent() can only be called once"));
  this->incomingRequest = nullptr;

  auto& context = incomingRequest->getContext();
  auto promise = event->run(kj::mv(incomingRequest), entrypointName).attach(kj::mv(event));

  // TODO(cleanup): In theory `context` may have been destroyed by now if `event->run()` dropped
  //   the `incomingRequest` synchronously. No current implementation does that, and
  //   maybeAddGcPassForTest() is a no-op outside of tests, so I'm ignoring the theoretical problem
  //   for now. Otherwise we will need to `atomicAddRef()` the `Worker` at some point earlier on
  //   but I'd like to avoid that in the non-test case.
  maybeAddGcPassForTest(context, promise);
  return promise;
}

template <typename T>
void WorkerEntrypoint::maybeAddGcPassForTest(
    IoContext& context, kj::Promise<T>& promise) {
  if (isPredictableModeForTest()) {
    auto worker = kj::atomicAddRef(context.getWorker());
    if constexpr (kj::isSameType<T, void>()) {
      promise = promise.then([worker = kj::mv(worker)]() {
        jsg::V8StackScope stackScope;
        auto lock = worker->getIsolate().getApiIsolate().lock(stackScope);
        lock->requestGcForTesting();
      });
    } else {
      promise = promise.then([worker = kj::mv(worker)](auto res) {
        jsg::V8StackScope stackScope;
        auto lock = worker->getIsolate().getApiIsolate().lock(stackScope);
        lock->requestGcForTesting();
        return res;
      });
    }
  }
}

} // namespace workerd
