// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "global-scope.h"

#include "simdutf.h"

#include <workerd/api/cache.h>
#include <workerd/api/crypto/crypto.h>
#include <workerd/api/events.h>
#include <workerd/api/eventsource.h>
#include <workerd/api/hibernatable-web-socket.h>
#include <workerd/api/scheduled.h>
#include <workerd/api/system-streams.h>
#include <workerd/api/trace.h>
#include <workerd/api/util.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/async-context.h>
#include <workerd/jsg/ser.h>
#include <workerd/jsg/util.h>
#include <workerd/util/sentry.h>
#include <workerd/util/stream-utils.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/uncaught-exception-source.h>
#include <workerd/util/use-perfetto-categories.h>

#include <kj/encoding.h>

namespace workerd::api {

namespace {

enum class NeuterReason { SENT_RESPONSE, THREW_EXCEPTION, CLIENT_DISCONNECTED };

kj::Exception makeNeuterException(NeuterReason reason) {
  switch (reason) {
    case NeuterReason::SENT_RESPONSE:
      return JSG_KJ_EXCEPTION(
          FAILED, TypeError, "Can't read from request stream after response has been sent.");
    case NeuterReason::THREW_EXCEPTION:
      return JSG_KJ_EXCEPTION(
          FAILED, TypeError, "Can't read from request stream after responding with an exception.");
    case NeuterReason::CLIENT_DISCONNECTED:
      return JSG_KJ_EXCEPTION(
          DISCONNECTED, TypeError, "Can't read from request stream because client disconnected.");
  }
  KJ_UNREACHABLE;
}

kj::String getEventName(v8::PromiseRejectEvent type) {
  switch (type) {
    case v8::PromiseRejectEvent::kPromiseRejectWithNoHandler:
      return kj::str("unhandledrejection");
    case v8::PromiseRejectEvent::kPromiseHandlerAddedAfterReject:
      return kj::str("rejectionhandled");
    default:
      // Events are not emitted for the other reject types.
      KJ_UNREACHABLE;
  }
}

}  // namespace

PromiseRejectionEvent::PromiseRejectionEvent(
    v8::PromiseRejectEvent type, jsg::V8Ref<v8::Promise> promise, jsg::Value reason)
    : Event(getEventName(type)),
      promise(kj::mv(promise)),
      reason(kj::mv(reason)) {}

void PromiseRejectionEvent::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(promise, reason);
}

void ExecutionContext::waitUntil(kj::Promise<void> promise) {
  IoContext::current().addWaitUntil(kj::mv(promise));
}

void ExecutionContext::passThroughOnException() {
  IoContext::current().setFailOpen();
}

void ExecutionContext::abort(jsg::Lock& js, jsg::Optional<jsg::Value> reason) {
  // TODO(someday): Maybe instead of throwing we should TerminateExecution() here? But that
  //   requires some more extensive changes.
  KJ_IF_SOME(r, reason) {
    IoContext::current().abort(js.exceptionToKj(r.addRef(js)));
    js.throwException(kj::mv(r));
  } else {
    auto e =
        JSG_KJ_EXCEPTION(FAILED, Error, "Worker execution was aborted due to call to ctx.abort().");
    IoContext::current().abort(kj::cp(e));
    kj::throwFatalException(kj::mv(e));
  }
}

namespace {
template <typename T>
jsg::LenientOptional<T> mapAddRef(jsg::Lock& js, jsg::LenientOptional<T>& function) {
  return function.map([&](T& a) { return a.addRef(js); });
}
}  // namespace

ExportedHandler ExportedHandler::clone(jsg::Lock& js) {
  return ExportedHandler{
    .fetch{mapAddRef(js, fetch)},
    .tail{mapAddRef(js, tail)},
    .trace{mapAddRef(js, trace)},
    .tailStream{mapAddRef(js, tailStream)},
    .scheduled{mapAddRef(js, scheduled)},
    .alarm{mapAddRef(js, alarm)},
    .test{mapAddRef(js, test)},
    .webSocketMessage{mapAddRef(js, webSocketMessage)},
    .webSocketClose{mapAddRef(js, webSocketClose)},
    .webSocketError{mapAddRef(js, webSocketError)},
    .self{js.v8Isolate, self.getHandle(js.v8Isolate)},
    .env{env.addRef(js)},
    .ctx{getCtx()},
    .missingSuperclass = missingSuperclass,
  };
}

ServiceWorkerGlobalScope::ServiceWorkerGlobalScope()
    : unhandledRejections([this](jsg::Lock& js,
                              v8::PromiseRejectEvent event,
                              jsg::V8Ref<v8::Promise> promise,
                              jsg::Value value) {
        // If async context tracking is enabled, then we need to ensure that we enter the frame
        // associated with the promise before we invoke the unhandled rejection callback handling.
        auto ev = js.alloc<PromiseRejectionEvent>(event, kj::mv(promise), kj::mv(value));
        dispatchEventImpl(js, kj::mv(ev));
      }) {}

void ServiceWorkerGlobalScope::clear() {
  removeAllHandlers();
  unhandledRejections.clear();
}

kj::Promise<DeferredProxy<void>> ServiceWorkerGlobalScope::request(kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody,
    kj::HttpService::Response& response,
    kj::Maybe<kj::StringPtr> cfBlobJson,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler,
    kj::Maybe<jsg::Ref<AbortSignal>> abortSignal) {
  TRACE_EVENT("workerd", "ServiceWorkerGlobalScope::request()");
  // To construct a ReadableStream object, we're supposed to pass in an Own<AsyncInputStream>, so
  // that it can drop the reference whenever it gets GC'ed. But in this case the stream's lifetime
  // is not under our control -- it's attached to the request. So, we wrap it in a
  // NeuterableInputStream which allows us to disconnect the stream before it becomes invalid.
  auto ownRequestBody = newNeuterableInputStream(requestBody);
  auto deferredNeuter = kj::defer([ownRequestBody = kj::addRef(*ownRequestBody)]() mutable {
    // Make sure to cancel the request body stream since the native stream is no longer valid once
    // the returned promise completes. Note that the KJ HTTP library deals with the fact that we
    // haven't consumed the entire request body.
    ownRequestBody->neuter(makeNeuterException(NeuterReason::CLIENT_DISCONNECTED));
  });
  KJ_ON_SCOPE_FAILURE(ownRequestBody->neuter(makeNeuterException(NeuterReason::THREW_EXCEPTION)));

  auto& ioContext = IoContext::current();
  jsg::Lock& js = lock;

  CfProperty cf(cfBlobJson);

  auto jsHeaders = js.alloc<Headers>(js, headers, Headers::Guard::REQUEST);
  // We do not automatically decode gzipped request bodies because the fetch() standard doesn't
  // specify any automatic encoding of requests. https://github.com/whatwg/fetch/issues/589
  auto b = newSystemStream(kj::addRef(*ownRequestBody), StreamEncoding::IDENTITY);
  auto jsStream = js.alloc<ReadableStream>(ioContext, kj::mv(b));

  // If the request has "no body", we want `request.body` to be null. But, this is not the same
  // thing as the request having a body that happens to be empty. Unfortunately, KJ HTTP gives us
  // a zero-length AsyncInputStream either way, so we can't just check the stream length.
  //
  // The HTTP spec says: "The presence of a message body in a request is signaled by a
  // Content-Length or Transfer-Encoding header field." RFC 7230, section 3.3.
  // https://tools.ietf.org/html/rfc7230#section-3.3
  //
  // But, the request was not necessarily received over HTTP! It could be from another worker in
  // a pipeline, or it could have been received over RPC. In either case, the headers don't
  // necessarily mean anything; the calling worker can fill them in however it wants.
  //
  // So, we decide if the body is null if both headers are missing AND the stream is known to have
  // zero length. And on the sending end (fetchImpl() in http.c++), if we're sending a request with
  // a non-null body that is known to be empty, we explicitly set Content-Length: 0. This should
  // mean that in all worker-to-worker interactions, if the sender provided a non-null body, the
  // receiver will receive a non-null body, independent of anything else.
  //
  // TODO(cleanup): Should KJ HTTP interfaces explicitly communicate the difference between a
  //   missing body and an empty one?
  kj::Maybe<Body::ExtractedBody> body;
  if (headers.get(kj::HttpHeaderId::CONTENT_LENGTH) != kj::none ||
      headers.get(kj::HttpHeaderId::TRANSFER_ENCODING) != kj::none ||
      requestBody.tryGetLength().orDefault(1) > 0) {
    body = Body::ExtractedBody(jsStream.addRef());
  }

  // If the request doesn't specify "Content-Length" or "Transfer-Encoding", set "Content-Length"
  // to the body length if it's known. This ensures handlers for worker-to-worker requests can
  // access known body lengths if they're set, without buffering bodies.
  if (body != kj::none && headers.get(kj::HttpHeaderId::CONTENT_LENGTH) == kj::none &&
      headers.get(kj::HttpHeaderId::TRANSFER_ENCODING) == kj::none) {
    // We can't use headers.set() here as headers is marked const. Instead, we call set() on the
    // JavaScript headers object, ignoring the REQUEST guard that usually makes them immutable.
    KJ_IF_SOME(l, requestBody.tryGetLength()) {
      jsHeaders->setUnguarded(
          js, js.accountedByteString("Content-Length"_kj), js.accountedByteString(kj::str(l)));
    } else {
      jsHeaders->setUnguarded(
          js, js.accountedByteString("Transfer-Encoding"_kj), js.accountedByteString("chunked"_kj));
    }
  }

  auto jsRequest = js.alloc<Request>(js, method, url, Request::Redirect::MANUAL, kj::mv(jsHeaders),
      jsg::alloc<Fetcher>(IoContext::NEXT_CLIENT_CHANNEL, Fetcher::RequiresHostAndProtocol::YES),
      /* signal */ kj::mv(abortSignal), kj::mv(cf), kj::mv(body),
      /* thisSignal */ kj::none, Request::CacheMode::NONE);

  // signal vs thisSignal
  // --------------------
  // The fetch spec definition of Request has a distinction between the
  // "signal" (which is an optional AbortSignal passed in with the options), and "this' signal",
  // which is an AbortSignal that is always available via the request.signal accessor.
  //
  // redirect
  // --------
  // I set the redirect mode to manual here, so that by default scripts that just pass requests
  // through to a fetch() call will behave the same as scripts which don't call .respondWith(): if
  // the request results in a redirect, the visitor will see that redirect.

  auto event = js.alloc<FetchEvent>(kj::mv(jsRequest));

  uint tasksBefore = ioContext.taskCount();

  // We'll drop our span once the promise (fetch handler result) resolves.
  kj::Maybe<SpanBuilder> span = ioContext.makeTraceSpan("fetch_handler"_kjc);
  bool useDefaultHandling;
  KJ_IF_SOME(h, exportedHandler) {
    KJ_IF_SOME(f, h.fetch) {
      auto promise = f(lock, event->getRequest(), h.env.addRef(js), h.getCtx());
      event->respondWith(lock, kj::mv(promise));
      useDefaultHandling = false;
    } else {
      // In modules mode we don't have a concept of "default handling".
      lock.logWarningOnce("Received a FetchEvent but we lack a handler for FetchEvents. "
                          "Did you remember to export a fetch() function?");
      JSG_FAIL_REQUIRE(Error, "Handler does not export a fetch() function.");
    }
  } else {
    // Fire off the handlers.
    useDefaultHandling = dispatchEventImpl(lock, event.addRef());
  }

  if (useDefaultHandling) {
    // No one called respondWith() or preventDefault(). Go directly to subrequest.

    if (ioContext.taskCount() > tasksBefore) {
      lock.logWarning(
          "FetchEvent handler did not call respondWith() before returning, but initiated some "
          "asynchronous task. That task will be canceled and default handling will occur -- the "
          "request will be sent unmodified to your origin. Remember that you must call "
          "respondWith() *before* the event handler returns, if you don't want default handling. "
          "You cannot call it asynchronously later on. If you need to wait for I/O (e.g. a "
          "subrequest) before generating a Response, then call respondWith() with a Promise (for "
          "the eventual Response) as the argument.");
    }

    if (jsStream->isDisturbed()) {
      lock.logUncaughtException(
          "Script consumed request body but didn't call respondWith(). Can't forward request.");
      return addNoopDeferredProxy(
          response.sendError(500, "Internal Server Error", ioContext.getHeaderTable()));
    } else {
      auto client = ioContext.getHttpClient(IoContext::NEXT_CLIENT_CHANNEL, false,
          cfBlobJson.map([](kj::StringPtr s) { return kj::str(s); }), "fetch_default"_kjc);
      auto adapter = kj::newHttpService(*client);
      auto promise = adapter->request(method, url, headers, requestBody, response);
      // Default handling doesn't rely on the IoContext at all so we can return it as a
      // deferred proxy task.
      return DeferredProxy<void>{promise.attach(kj::mv(adapter), kj::mv(client))};
    }
  } else KJ_IF_SOME(promise, event->getResponsePromise(lock)) {
    auto body2 = kj::addRef(*ownRequestBody);

    // HACK: If the client disconnects, the `response` reference is no longer valid. But our
    //   promise resolves in JavaScript space, so won't be canceled. So we need to track
    //   cancellation separately. We use a weird refcounted boolean.
    // TODO(cleanup): Is there something less ugly we can do here?
    struct RefcountedBool: public kj::Refcounted {
      bool value;
      RefcountedBool(bool value): value(value) {}
    };
    auto canceled = kj::refcounted<RefcountedBool>(false);

    return ioContext
        .awaitJs(lock,
            promise.then(kj::implicitCast<jsg::Lock&>(lock),
                ioContext.addFunctor(
                    [&response, allowWebSocket = headers.isWebSocket(),
                        canceled = kj::addRef(*canceled), &headers, span = kj::mv(span)](
                        jsg::Lock& js, jsg::Ref<Response> innerResponse) mutable
                    -> IoOwn<kj::Promise<DeferredProxy<void>>> {
      JSG_REQUIRE(innerResponse->getType() != "error"_kj, TypeError,
          "Return value from serve handler must not be an error response (like Response.error())");

      auto& context = IoContext::current();
      // Drop our fetch_handler span now that the promise has resolved.
      span = kj::none;
      if (canceled->value) {
        // Oops, the client disconnected before the response was ready to send. `response` is
        // a dangling reference, let's not use it.
        return context.addObject(kj::heap(addNoopDeferredProxy(kj::READY_NOW)));
      } else {
        return context.addObject(kj::heap(
            innerResponse->send(js, response, {.allowWebSocket = allowWebSocket}, headers)));
      }
    })))
        .attach(kj::defer([canceled = kj::mv(canceled)]() mutable { canceled->value = true; }))
        .then(
            [ownRequestBody = kj::mv(ownRequestBody), deferredNeuter = kj::mv(deferredNeuter)](
                DeferredProxy<void> deferredProxy) mutable {
      // In the case of bidirectional streaming, the request body stream needs to remain valid
      // while proxying the response. So, arrange for neutering to happen only after the proxy
      // task finishes.
      deferredProxy.proxyTask = deferredProxy.proxyTask
                                    .then([body = kj::addRef(*ownRequestBody)]() mutable {
        body->neuter(makeNeuterException(NeuterReason::SENT_RESPONSE));
      }, [body = kj::addRef(*ownRequestBody)](kj::Exception&& e) mutable {
        body->neuter(makeNeuterException(NeuterReason::THREW_EXCEPTION));
        kj::throwFatalException(kj::mv(e));
      }).attach(kj::mv(deferredNeuter));

      return deferredProxy;
    },
            [body = kj::mv(body2)](kj::Exception&& e) mutable -> DeferredProxy<void> {
      // HACK: We depend on the fact that the success-case lambda above hasn't been destroyed yet
      //   so `deferredNeuter` hasn't been destroyed yet.
      body->neuter(makeNeuterException(NeuterReason::THREW_EXCEPTION));
      kj::throwFatalException(kj::mv(e));
    });
  } else {
    // The service worker API says that if default handling is prevented and respondWith() wasn't
    // called, the request should result in "a network error".
    return KJ_EXCEPTION(DISCONNECTED, "preventDefault() called but respondWith() not called");
  }
}

void ServiceWorkerGlobalScope::sendTraces(kj::ArrayPtr<kj::Own<Trace>> traces,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {
  auto isolate = lock.getIsolate();
  jsg::Lock& js = lock;

  KJ_IF_SOME(h, exportedHandler) {
    KJ_IF_SOME(f, h.tail) {
      auto tailEvent = js.alloc<TailEvent>(lock, "tail"_kj, traces);
      auto promise = f(lock, tailEvent->getEvents(), h.env.addRef(isolate), h.getCtx());
      tailEvent->waitUntil(kj::mv(promise));
    } else KJ_IF_SOME(f, h.trace) {
      auto traceEvent = js.alloc<TailEvent>(lock, "trace"_kj, traces);
      auto promise = f(lock, traceEvent->getEvents(), h.env.addRef(isolate), h.getCtx());
      traceEvent->waitUntil(kj::mv(promise));
    } else {
      lock.logWarningOnce("Attempted to send events but we lack a handler, "
                          "did you remember to export a tail() function?");
      JSG_FAIL_REQUIRE(Error, "Handler does not export a tail() function.");
    }
  } else {
    // Fire off the handlers.
    // We only create both events here.
    auto tailEvent = js.alloc<TailEvent>(lock, "tail"_kj, traces);
    auto traceEvent = js.alloc<TailEvent>(lock, "trace"_kj, traces);
    dispatchEventImpl(lock, tailEvent.addRef());
    dispatchEventImpl(lock, traceEvent.addRef());

    // We assume no action is necessary for "default" trace handling.
  }
}

void ServiceWorkerGlobalScope::startScheduled(kj::Date scheduledTime,
    kj::StringPtr cron,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {
  auto& context = IoContext::current();
  jsg::Lock& js = lock;

  double eventTime = (scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS;

  auto event = js.alloc<ScheduledEvent>(eventTime, cron);

  auto isolate = lock.getIsolate();

  KJ_IF_SOME(h, exportedHandler) {
    KJ_IF_SOME(f, h.scheduled) {
      auto promise =
          f(lock, js.alloc<ScheduledController>(event.addRef()), h.env.addRef(isolate), h.getCtx());
      event->waitUntil(kj::mv(promise));
    } else {
      lock.logWarningOnce(
          "Received a ScheduledEvent but we lack a handler for ScheduledEvents "
          "(a.k.a. Cron Triggers). Did you remember to export a scheduled() function?");
      context.setNoRetryScheduled();
      JSG_FAIL_REQUIRE(Error, "Handler does not export a scheduled() function");
    }
  } else {
    // Fire off the handlers after confirming there is at least one.
    if (getHandlerCount("scheduled") == 0) {
      lock.logWarningOnce(
          "Received a ScheduledEvent but we lack an event listener for scheduled events "
          "(a.k.a. Cron Triggers). Did you remember to call addEventListener(\"scheduled\", ...)?");
      context.setNoRetryScheduled();
      JSG_FAIL_REQUIRE(Error, "No event listener registered for scheduled events.");
    }
    dispatchEventImpl(lock, event.addRef());
  }
}

kj::Promise<WorkerInterface::AlarmResult> ServiceWorkerGlobalScope::runAlarm(kj::Date scheduledTime,
    kj::Duration timeout,
    uint32_t retryCount,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {

  auto& context = IoContext::current();
  auto& actor = KJ_ASSERT_NONNULL(context.getActor());
  auto& persistent = KJ_ASSERT_NONNULL(actor.getPersistent());

  kj::String actorId;
  KJ_SWITCH_ONEOF(actor.getId()) {
    KJ_CASE_ONEOF(f, kj::Own<ActorIdFactory::ActorId>) {
      actorId = f->toString();
    }
    KJ_CASE_ONEOF(s, kj::String) {
      actorId = kj::str(s);
    }
  }

  KJ_SWITCH_ONEOF(persistent.armAlarmHandler(scheduledTime, false, actorId)) {
    KJ_CASE_ONEOF(armResult, ActorCacheInterface::RunAlarmHandler) {
      auto& handler = KJ_REQUIRE_NONNULL(exportedHandler);
      if (handler.alarm == kj::none) {

        lock.logWarningOnce("Attempted to run a scheduled alarm without a handler, "
                            "did you remember to export an alarm() function?");
        return WorkerInterface::AlarmResult{
          .retry = false, .outcome = EventOutcome::SCRIPT_NOT_FOUND};
      }

      auto& alarm = KJ_ASSERT_NONNULL(handler.alarm);

      return context
          .run([exportedHandler, &context, timeout, retryCount, &alarm,
                   maybeAsyncContext = jsg::AsyncContextFrame::currentRef(lock)](
                   Worker::Lock& lock) mutable -> kj::Promise<WorkerInterface::AlarmResult> {
        jsg::AsyncContextFrame::Scope asyncScope(lock, maybeAsyncContext);
        // We want to limit alarm handler walltime to 15 minutes at most. If the timeout promise
        // completes we want to cancel the alarm handler. If the alarm handler promise completes
        // first timeout will be canceled.
        jsg::Lock& js = lock;
        auto timeoutPromise = context.afterLimitTimeout(timeout).then(
            [&context]() -> kj::Promise<WorkerInterface::AlarmResult> {
          // We don't want to delete the alarm since we have not successfully completed the alarm
          // execution.
          auto& actor = KJ_ASSERT_NONNULL(context.getActor());
          auto& persistent = KJ_ASSERT_NONNULL(actor.getPersistent());
          persistent.cancelDeferredAlarmDeletion();

          LOG_NOSENTRY(WARNING, "Alarm exceeded its allowed execution time");
          // Report alarm handler failure and log it.
          auto e = KJ_EXCEPTION(OVERLOADED,
              "broken.dropped; worker_do_not_log; jsg.Error: Alarm exceeded its allowed execution time");
          context.getMetrics().reportFailure(e);

          // We don't want the handler to keep running after timeout.
          context.abort(kj::mv(e));
          // We want timed out alarms to be treated as user errors. As such, we'll mark them as
          // retriable, and we'll count the retries against the alarm retries limit. This will ensure
          // that the handler will attempt to run for a number of times before giving up and deleting
          // the alarm.
          return WorkerInterface::AlarmResult{
            .retry = true, .retryCountsAgainstLimit = true, .outcome = EventOutcome::EXCEEDED_CPU};
        });

        return alarm(lock, js.alloc<AlarmInvocationInfo>(retryCount))
            .then([]() -> kj::Promise<WorkerInterface::AlarmResult> {
          return WorkerInterface::AlarmResult{.retry = false, .outcome = EventOutcome::OK};
        }).exclusiveJoin(kj::mv(timeoutPromise));
      })
          .catch_([&context, deferredDelete = kj::mv(armResult.deferredDelete)](
                      kj::Exception&& e) mutable {
        auto& actor = KJ_ASSERT_NONNULL(context.getActor());
        auto& persistent = KJ_ASSERT_NONNULL(actor.getPersistent());
        persistent.cancelDeferredAlarmDeletion();

        context.getMetrics().reportFailure(e);

        // This will include the error in inspector/tracers and log to syslog if internal.
        context.logUncaughtExceptionAsync(UncaughtExceptionSource::ALARM_HANDLER, kj::mv(e));

        EventOutcome outcome = EventOutcome::EXCEPTION;
        KJ_IF_SOME(status, context.getLimitEnforcer().getLimitsExceeded()) {
          outcome = status;
        }

        kj::String actorId;
        KJ_SWITCH_ONEOF(actor.getId()) {
          KJ_CASE_ONEOF(f, kj::Own<ActorIdFactory::ActorId>) {
            actorId = f->toString();
          }
          KJ_CASE_ONEOF(s, kj::String) {
            actorId = kj::str(s);
          }
        }

        // We only want to retry against limits if it's a user error. By default let's check if the
        // output gate is broken.
        auto shouldRetryCountsAgainstLimits = !context.isOutputGateBroken();

        // We want to alert if we aren't going to count this alarm retry against limits
        if (auto desc = e.getDescription(); !jsg::isTunneledException(desc) &&
            !jsg::isDoNotLogException(desc) && context.isOutputGateBroken()) {
          LOG_NOSENTRY(ERROR, "output lock broke during alarm execution", actorId, e);
        } else if (context.isOutputGateBroken()) {
          if (e.getDetail(jsg::EXCEPTION_IS_USER_ERROR) != kj::none) {
            // The handler failed because the user overloaded the object. It's their fault, we'll not
            // retry forever.
            shouldRetryCountsAgainstLimits = true;
          }

          // We don't usually log these messages, but it's useful to know the real reason we failed
          // to correctly investigate stuck alarms.
          LOG_NOSENTRY(ERROR,
              "output lock broke during alarm execution without an interesting error description",
              actorId, e, shouldRetryCountsAgainstLimits);
        }
        return WorkerInterface::AlarmResult{.retry = true,
          .retryCountsAgainstLimit = shouldRetryCountsAgainstLimits,
          .outcome = outcome};
      })
          .then([&context](WorkerInterface::AlarmResult result)
                    -> kj::Promise<WorkerInterface::AlarmResult> {
        return context.waitForOutputLocks().then(
            [result]() { return kj::mv(result); }, [&context](kj::Exception&& e) {
          auto& actor = KJ_ASSERT_NONNULL(context.getActor());
          kj::String actorId;
          KJ_SWITCH_ONEOF(actor.getId()) {
            KJ_CASE_ONEOF(f, kj::Own<ActorIdFactory::ActorId>) {
              actorId = f->toString();
            }
            KJ_CASE_ONEOF(s, kj::String) {
              actorId = kj::str(s);
            }
          }
          // We only want to retry against limits if it's a user error. By default let's assume it's our
          // fault.
          auto shouldRetryCountsAgainstLimits = false;
          if (auto desc = e.getDescription();
              !jsg::isTunneledException(desc) && !jsg::isDoNotLogException(desc)) {
            if (isInterestingException(e)) {
              LOG_EXCEPTION("alarmOutputLock"_kj, e);
            } else {
              LOG_NOSENTRY(ERROR, "output lock broke after executing alarm", actorId, e);
            }
          } else {
            if (e.getDetail(jsg::EXCEPTION_IS_USER_ERROR) != kj::none) {
              // The handler failed because the user overloaded the object. It's their fault, we'll not
              // retry forever.
              shouldRetryCountsAgainstLimits = true;
            }
          }
          return WorkerInterface::AlarmResult{.retry = true,
            .retryCountsAgainstLimit = shouldRetryCountsAgainstLimits,
            .outcome = EventOutcome::EXCEPTION};
        });
      });
    }
    KJ_CASE_ONEOF(armResult, ActorCacheInterface::CancelAlarmHandler) {
      return armResult.waitBeforeCancel.then([]() {
        return WorkerInterface::AlarmResult{.retry = false, .outcome = EventOutcome::CANCELED};
      });
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<void> ServiceWorkerGlobalScope::test(
    Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler) {
  // TODO(someday): For Service Workers syntax, do we want addEventListener("test")? Not supporting
  //   it for now.
  ExportedHandler& eh = JSG_REQUIRE_NONNULL(
      exportedHandler, Error, "Tests are not currently supported with Service Workers syntax.");

  auto& testHandler =
      JSG_REQUIRE_NONNULL(eh.test, Error, "Entrypoint does not export a test() function.");

  jsg::Lock& js = lock;
  return testHandler(lock, js.alloc<TestController>(), eh.env.addRef(lock), eh.getCtx());
}

// This promise is used to set the timeout for hibernatable websocket events. It's expected to be
// dropped in most cases, as long as the hibernatable websocket event promise completes before it.
kj::Promise<void> ServiceWorkerGlobalScope::eventTimeoutPromise(uint32_t timeoutMs) {
  auto& actor = KJ_ASSERT_NONNULL(IoContext::current().getActor());
  co_await IoContext::current().afterLimitTimeout(timeoutMs * kj::MILLISECONDS);
  // This is the ActorFlushReason for eviction in Cloudflare's internal implementation.
  auto evictionCode = 2;
  actor.shutdown(evictionCode,
      KJ_EXCEPTION(DISCONNECTED,
          "broken.dropped; jsg.Error: Actor exceeded event execution time and was disconnected."));
}

kj::Promise<void> ServiceWorkerGlobalScope::setHibernatableEventTimeout(
    kj::Promise<void> event, kj::Maybe<uint32_t> eventTimeoutMs) {
  // If we have a maximum event duration timeout set, we should prevent the actor from running
  // for more than the user selected duration.
  auto timeoutMs = eventTimeoutMs.orDefault((uint32_t)0);
  if (timeoutMs > 0) {
    return event.exclusiveJoin(eventTimeoutPromise(timeoutMs));
  }
  return event;
}

void ServiceWorkerGlobalScope::sendHibernatableWebSocketMessage(
    kj::OneOf<kj::String, kj::Array<byte>> message,
    kj::Maybe<uint32_t> eventTimeoutMs,
    kj::String websocketId,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {
  jsg::Lock& js = lock;
  auto event = js.alloc<HibernatableWebSocketEvent>();
  // Even if no handler is exported, we need to claim the websocket so it's removed from the map.
  auto websocket = event->claimWebSocket(lock, websocketId);

  KJ_IF_SOME(h, exportedHandler) {
    KJ_IF_SOME(handler, h.webSocketMessage) {
      event->waitUntil(setHibernatableEventTimeout(
          handler(lock, kj::mv(websocket), kj::mv(message)), eventTimeoutMs));
    }
    // We want to deliver a message, but if no webSocketMessage handler is exported, we shouldn't fail
  }
}

void ServiceWorkerGlobalScope::sendHibernatableWebSocketClose(HibernatableSocketParams::Close close,
    kj::Maybe<uint32_t> eventTimeoutMs,
    kj::String websocketId,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {
  jsg::Lock& js = lock;
  auto event = js.alloc<HibernatableWebSocketEvent>();

  // Even if no handler is exported, we need to claim the websocket so it's removed from the map.
  //
  // We won't be dispatching any further events because we've received a close, so we return the
  // owned websocket back to the api::WebSocket.
  auto releasePackage = event->prepareForRelease(lock, websocketId);
  auto websocket = kj::mv(releasePackage.webSocketRef);
  websocket->initiateHibernatableRelease(lock, kj::mv(releasePackage.ownedWebSocket),
      kj::mv(releasePackage.tags), api::WebSocket::HibernatableReleaseState::CLOSE);
  KJ_IF_SOME(h, exportedHandler) {
    KJ_IF_SOME(handler, h.webSocketClose) {
      event->waitUntil(setHibernatableEventTimeout(
          handler(lock, kj::mv(websocket), close.code, kj::mv(close.reason), close.wasClean),
          eventTimeoutMs));
    }
    // We want to deliver close, but if no webSocketClose handler is exported, we shouldn't fail
  }
}

void ServiceWorkerGlobalScope::sendHibernatableWebSocketError(kj::Exception e,
    kj::Maybe<uint32_t> eventTimeoutMs,
    kj::String websocketId,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {
  jsg::Lock& js = lock;
  auto event = js.alloc<HibernatableWebSocketEvent>();

  // Even if no handler is exported, we need to claim the websocket so it's removed from the map.
  //
  // We won't be dispatching any further events because we've encountered an error, so we return
  // the owned websocket back to the api::WebSocket.
  auto releasePackage = event->prepareForRelease(lock, websocketId);
  auto& websocket = releasePackage.webSocketRef;
  websocket->initiateHibernatableRelease(lock, kj::mv(releasePackage.ownedWebSocket),
      kj::mv(releasePackage.tags), WebSocket::HibernatableReleaseState::ERROR);

  KJ_IF_SOME(h, exportedHandler) {
    KJ_IF_SOME(handler, h.webSocketError) {
      event->waitUntil(setHibernatableEventTimeout(
          handler(js, kj::mv(websocket), js.exceptionToJs(kj::mv(e))), eventTimeoutMs));
    }
    // We want to deliver an error, but if no webSocketError handler is exported, we shouldn't fail
  }
}

void ServiceWorkerGlobalScope::emitPromiseRejection(jsg::Lock& js,
    v8::PromiseRejectEvent event,
    jsg::V8Ref<v8::Promise> promise,
    jsg::Value value) {

  const auto hasHandlers = [this] {
    return getHandlerCount("unhandledrejection"_kj) + getHandlerCount("rejectionhandled"_kj);
  };

  const auto hasInspector = [] {
    if (!IoContext::hasCurrent()) return false;
    return IoContext::current().isInspectorEnabled();
  };

  if (hasHandlers() || hasInspector()) {
    unhandledRejections.report(js, event, kj::mv(promise), kj::mv(value));
  }
}

jsg::JsString ServiceWorkerGlobalScope::btoa(jsg::Lock& js, jsg::JsString str) {
  // We could implement btoa() by accepting a kj::String, but then we'd have to check that it
  // doesn't have any multibyte code points. Easier to perform that test using v8::String's
  // ContainsOnlyOneByte() function.
  JSG_REQUIRE(str.containsOnlyOneByte(), DOMInvalidCharacterError,
      "btoa() can only operate on characters in the Latin1 (ISO/IEC 8859-1) range.");
  auto strArray = str.toArray<kj::byte>(js);
  auto expected_length = simdutf::base64_length_from_binary(strArray.size());
  KJ_STACK_ARRAY(kj::byte, result, expected_length, 1024, 1024);
  auto written = simdutf::binary_to_base64(
      strArray.asChars().begin(), strArray.size(), result.asChars().begin());
  return js.str(result.first(written));
}
jsg::JsString ServiceWorkerGlobalScope::atob(jsg::Lock& js, kj::String data) {
  auto decoded = kj::decodeBase64(data.asArray());

  JSG_REQUIRE(!decoded.hadErrors, DOMInvalidCharacterError,
      "atob() called with invalid base64-encoded data. (Only whitespace, '+', '/', alphanumeric "
      "ASCII, and up to two terminal '=' signs when the input data length is divisible by 4 are "
      "allowed.)");

  // Similar to btoa() taking a v8::Value, we return a v8::String directly, as this allows us to
  // construct a string from the non-nul-terminated array returned from decodeBase64(). This avoids
  // making a copy purely to append a nul byte.
  return js.str(decoded.asBytes());
}

void ServiceWorkerGlobalScope::queueMicrotask(jsg::Lock& js, jsg::Function<void()> task) {
  auto fn = js.wrapSimpleFunction(js.v8Context(),
      JSG_VISITABLE_LAMBDA((this, fn = kj::mv(task)), (fn),
          (jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) {
            js.tryCatch([&] {
              // The function won't be called with any arguments, so we can
              // safely ignore anything passed in to args.
              fn(js);
            }, [&](jsg::Value exception) {
              // The reportError call itself can potentially throw errors. Let's catch
              // and report them as well.
              js.tryCatch([&] { reportError(js, jsg::JsValue(exception.getHandle(js))); },
                  [&](jsg::Value exception) {
                // An error was thrown by the 'error' event handler. That's unfortunate.
                // Let's log the error and just continue. It won't be possible to actually
                // catch or handle this error so logging is really the only way to notify
                // folks about it.
                auto val = jsg::JsValue(exception.getHandle(js));
                // If the value is an object that has a stack property, log that so we get
                // the stack trace if it is an exception.
                KJ_IF_SOME(obj, val.tryCast<jsg::JsObject>()) {
                auto stack = obj.get(js, "stack"_kj);
                if (!stack.isUndefined()) {
                js.reportError(stack);
                return;
                }
                } else {
                }  // Here to avoid a compile warning
                // Otherwise just log the stringified value generically.
                js.reportError(val);
              });
            });
          }));

  js.v8Isolate->EnqueueMicrotask(fn);
}

jsg::JsValue ServiceWorkerGlobalScope::structuredClone(
    jsg::Lock& js, jsg::JsValue value, jsg::Optional<StructuredCloneOptions> maybeOptions) {
  KJ_IF_SOME(options, maybeOptions) {
    KJ_IF_SOME(transfer, options.transfer) {
      auto transfers = KJ_MAP(i, transfer) { return i.getHandle(js); };
      return value.structuredClone(js, kj::mv(transfers));
    }
  }
  return value.structuredClone(js);
}

TimeoutId::NumberType ServiceWorkerGlobalScope::setTimeoutInternal(
    jsg::Function<void()> function, double msDelay) {
  auto timeoutId = IoContext::current().setTimeoutImpl(timeoutIdGenerator,
      /** repeats = */ false, kj::mv(function), msDelay);
  return timeoutId.toNumber();
}

TimeoutId::NumberType ServiceWorkerGlobalScope::setTimeout(jsg::Lock& js,
    jsg::Function<void(jsg::Arguments<jsg::Value>)> function,
    jsg::Optional<double> msDelay,
    jsg::Arguments<jsg::Value> args) {
  function.setReceiver(js.v8Ref<v8::Value>(js.v8Context()->Global()));
  auto fn = [function = kj::mv(function), args = kj::mv(args),
                context = jsg::AsyncContextFrame::currentRef(js)](jsg::Lock& js) mutable {
    jsg::AsyncContextFrame::Scope scope(js, context);
    function(js, kj::mv(args));
  };
  auto timeoutId = IoContext::current().setTimeoutImpl(timeoutIdGenerator,
      /* repeat */ false, [function = kj::mv(fn)](jsg::Lock& js) mutable { function(js); },
      msDelay.orDefault(0));
  return timeoutId.toNumber();
}

void ServiceWorkerGlobalScope::clearTimeout(jsg::Lock& js, kj::Maybe<jsg::JsNumber> timeoutId) {
  KJ_IF_SOME(rawId, timeoutId) {
    // Browsers does not throw an error when "unsafe" integers are passed to the clearTimeout method.
    // Let's make sure we ignore those values, just like browsers and other runtimes.
    KJ_IF_SOME(id, rawId.toSafeInteger(js)) {
      IoContext::current().clearTimeoutImpl(TimeoutId::fromNumber(id));
    }
  }
}

TimeoutId::NumberType ServiceWorkerGlobalScope::setInterval(jsg::Lock& js,
    jsg::Function<void(jsg::Arguments<jsg::Value>)> function,
    jsg::Optional<double> msDelay,
    jsg::Arguments<jsg::Value> args) {
  function.setReceiver(js.v8Ref<v8::Value>(js.v8Context()->Global()));
  auto fn = [function = kj::mv(function), args = kj::mv(args),
                context = jsg::AsyncContextFrame::currentRef(js)](jsg::Lock& js) mutable {
    jsg::AsyncContextFrame::Scope scope(js, context);
    // Because the fn is called multiple times, we will clone the args on each call.
    auto argv = KJ_MAP(i, args) { return i.addRef(js); };
    function(js, jsg::Arguments(kj::mv(argv)));
  };
  auto timeoutId = IoContext::current().setTimeoutImpl(timeoutIdGenerator,
      /* repeat */ true, [function = kj::mv(fn)](jsg::Lock& js) mutable { function(js); },
      msDelay.orDefault(0));
  return timeoutId.toNumber();
}

void ServiceWorkerGlobalScope::clearInterval(jsg::Lock& js, kj::Maybe<jsg::JsNumber> timeoutId) {
  clearTimeout(js, kj::mv(timeoutId));
}

jsg::Ref<Crypto> ServiceWorkerGlobalScope::getCrypto(jsg::Lock& js) {
  return js.alloc<Crypto>(js);
}

jsg::Ref<CacheStorage> ServiceWorkerGlobalScope::getCaches(jsg::Lock& js) {
  return js.alloc<CacheStorage>(js);
}

jsg::Promise<jsg::Ref<Response>> ServiceWorkerGlobalScope::fetch(jsg::Lock& js,
    kj::OneOf<jsg::Ref<Request>, kj::String> requestOrUrl,
    jsg::Optional<Request::Initializer> requestInit) {
  return fetchImpl(js, kj::none, kj::mv(requestOrUrl), kj::mv(requestInit));
}

void ServiceWorkerGlobalScope::reportError(jsg::Lock& js, jsg::JsValue error) {
  // Per the spec, we are going to first emit an error event on the global object.
  // If that event is not prevented, we will log the error to the console. Note
  // that we do not throw the error at all.
  auto message = v8::Exception::CreateMessage(js.v8Isolate, error);
  auto event = js.alloc<ErrorEvent>(kj::str("error"),
      ErrorEvent::ErrorEventInit{.message = kj::str(message->Get()),
        .filename = kj::str(message->GetScriptResourceName()),
        .lineno = jsg::check(message->GetLineNumber(js.v8Context())),
        .colno = jsg::check(message->GetStartColumn(js.v8Context())),
        .error = jsg::JsRef(js, error)});
  if (dispatchEventImpl(js, kj::mv(event))) {
    // If the value is an object that has a stack property, log that so we get
    // the stack trace if it is an exception.
    KJ_IF_SOME(obj, error.tryCast<jsg::JsObject>()) {
      auto stack = obj.get(js, "stack"_kj);
      if (!stack.isUndefined()) {
        js.reportError(stack);
        return;
      }
    }
    // Otherwise just log the stringified value generically.
    js.reportError(error);
  }
}

jsg::JsValue ServiceWorkerGlobalScope::getBuffer(jsg::Lock& js) {
  constexpr auto kSpecifier = "node:buffer"_kj;
  KJ_IF_SOME(module, js.resolveModule(kSpecifier)) {
    auto def = module.get(js, "default"_kj);
    auto obj = KJ_ASSERT_NONNULL(def.tryCast<jsg::JsObject>());
    auto buffer = obj.get(js, "Buffer"_kj);
    JSG_REQUIRE(buffer.isFunction(), TypeError, "Invalid node:buffer implementation");
    return buffer;
  } else {
    // If we are unable to resolve the node:buffer module here, it likely
    // means that we don't actually have a module registry installed. Just
    // return undefined in this case.
    return js.undefined();
  }
}

jsg::JsValue ServiceWorkerGlobalScope::getProcess(jsg::Lock& js) {
  constexpr auto kSpecifier = "node:process"_kj;
  KJ_IF_SOME(module, js.resolveModule(kSpecifier)) {
    auto def = module.get(js, "default"_kj);
    JSG_REQUIRE(def.isObject(), TypeError, "Invalid node:process implementation");
    return def;
  } else {
    // If we are unable to resolve the node:process module here, it likely
    // means that we don't actually have a module registry installed. Just
    // return undefined in this case.
    return js.undefined();
  }
}

double Performance::now() {
  // We define performance.now() for compatibility purposes, but due to Spectre concerns it
  // returns exactly what Date.now() returns.
  return dateNow();
}

jsg::Ref<StorageManager> Navigator::getStorage(jsg::Lock& js) {
  return js.alloc<StorageManager>();
}

bool Navigator::sendBeacon(jsg::Lock& js, kj::String url, jsg::Optional<Body::Initializer> body) {
  if (IoContext::hasCurrent()) {
    auto v8Context = js.v8Context();
    auto& global =
        jsg::extractInternalPointer<ServiceWorkerGlobalScope, true>(v8Context, v8Context->Global());
    auto promise = global.fetch(js, kj::mv(url),
        Request::InitializerDict{
          .method = kj::str("POST"),
          .body = kj::mv(body),
        });

    auto& context = IoContext::current();

    context.addWaitUntil(context.awaitJs(js, kj::mv(promise)).ignoreResult());
    return true;
  }

  // We cannot schedule a beacon to be sent outside of a request context.
  return false;
}

// ======================================================================================

Immediate::Immediate(IoContext& context, TimeoutId timeoutId)
    : contextRef(context.getWeakRef()),
      timeoutId(timeoutId) {}

void Immediate::dispose() {
  contextRef->runIfAlive([&](IoContext& context) { context.clearTimeoutImpl(timeoutId); });
}

jsg::Ref<Immediate> ServiceWorkerGlobalScope::setImmediate(jsg::Lock& js,
    jsg::Function<void(jsg::Arguments<jsg::Value>)> function,
    jsg::Arguments<jsg::Value> args) {

  // This is an approximation of the Node.js setImmediate global API.
  // We implement it in terms of setting a 0 ms timeout. This is not
  // how Node.js does it so there will be some edge cases where the
  // timing of the callback will differ relative to the equivalent
  // operations in Node.js. For the vast majority of cases, users
  // really shouldn't be able to tell a difference. It would likely
  // only be somewhat pathological edge cases that could be affected
  // by the differences. Unfortunately, changing this later to match
  // Node.js would likely be a breaking change for some users that
  // would require a compat flag... but that's OK for now?

  auto& context = IoContext::current();
  auto fn = [function = kj::mv(function), args = kj::mv(args),
                context = jsg::AsyncContextFrame::currentRef(js)](jsg::Lock& js) mutable {
    jsg::AsyncContextFrame::Scope scope(js, context);
    function(js, kj::mv(args));
  };
  auto timeoutId = context.setTimeoutImpl(timeoutIdGenerator,
      /* repeats = */ false, [function = kj::mv(fn)](jsg::Lock& js) mutable { function(js); }, 0);
  return js.alloc<Immediate>(context, timeoutId);
}

void ServiceWorkerGlobalScope::clearImmediate(kj::Maybe<jsg::Ref<Immediate>> maybeImmediate) {
  KJ_IF_SOME(immediate, maybeImmediate) {
    immediate->dispose();
  }
}

jsg::JsObject Cloudflare::getCompatibilityFlags(jsg::Lock& js) {
  auto flags = FeatureFlags::get(js);
  auto obj = js.objNoProto();
  auto dynamic = capnp::toDynamic(flags);
  auto schema = dynamic.getSchema();

  bool skipExperimental = !flags.getWorkerdExperimental();

  for (auto field: schema.getFields()) {
    // If this is an experimental flag, we expose it only if the experimental mode
    // is enabled.
    auto annotations = field.getProto().getAnnotations();
    bool skip = false;
    if (skipExperimental) {
      for (auto annotation: annotations) {
        if (annotation.getId() == EXPERIMENTAl_ANNOTATION_ID) {
          skip = true;
          break;
        }
      }
    }
    if (skip) continue;

    // Note that disable flags are not exposed.
    for (auto annotation: annotations) {
      if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
        obj.setReadOnly(
            js, annotation.getValue().getText(), js.boolean(dynamic.get(field).as<bool>()));
      }
    }
  }

  obj.seal(js);
  return obj;
}

}  // namespace workerd::api
