// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "global-scope.h"

#include <kj/encoding.h>

#include <workerd/api/system-streams.h>
#include <workerd/jsg/async-context.h>
#include <workerd/jsg/ser.h>
#include <workerd/jsg/util.h>
#include <workerd/io/trace.h>
#include <workerd/io/io-context.h>
#include <workerd/util/sentry.h>
#include <workerd/util/sentry.h>
#include <workerd/util/own-util.h>

namespace workerd::api {

namespace {

class NeuterableInputStream: public kj::AsyncInputStream, public kj::Refcounted {
  // An InputStream that can be disconnected. Used for request body, which becomes invalid as
  // soon as the response is returned.

public:
  NeuterableInputStream(kj::AsyncInputStream& inner): inner(&inner) {}

  enum NeuterReason {
    SENT_RESPONSE,
    THREW_EXCEPTION,
    CLIENT_DISCONNECTED
  };

  void neuter(NeuterReason reason) {
    if (inner.is<kj::AsyncInputStream*>()) {
      inner = reason;
      if (!canceler.isEmpty()) {
        canceler.cancel(makeException(reason));
      }
    }
  }

  kj::Promise<size_t> read(void* buffer, size_t minBytes, size_t maxBytes) override {
    return canceler.wrap(getStream().read(buffer, minBytes, maxBytes));
  }
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return canceler.wrap(getStream().tryRead(buffer, minBytes, maxBytes));
  }
  kj::Maybe<uint64_t> tryGetLength() override {
    return getStream().tryGetLength();
  }
  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    return canceler.wrap(getStream().pumpTo(output, amount));
  }

private:
  kj::OneOf<kj::AsyncInputStream*, NeuterReason> inner;
  kj::Canceler canceler;

  kj::AsyncInputStream& getStream() {
    KJ_SWITCH_ONEOF(inner) {
      KJ_CASE_ONEOF(stream, kj::AsyncInputStream*) {
        return *stream;
      }
      KJ_CASE_ONEOF(reason, NeuterReason) {
        kj::throwFatalException(makeException(reason));
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Exception makeException(NeuterReason reason) {
    switch (reason) {
      case SENT_RESPONSE:
        return JSG_KJ_EXCEPTION(FAILED, TypeError,
            "Can't read from request stream after response has been sent.");
      case THREW_EXCEPTION:
        return JSG_KJ_EXCEPTION(FAILED, TypeError,
            "Can't read from request stream after responding with an exception.");
      case CLIENT_DISCONNECTED:
        return JSG_KJ_EXCEPTION(DISCONNECTED, TypeError,
            "Can't read from request stream because client disconnected.");
    }
    KJ_UNREACHABLE;
  }
};

}  // namespace

void ExecutionContext::waitUntil(kj::Promise<void> promise) {
  IoContext::current().addWaitUntil(kj::mv(promise));
}

void ExecutionContext::passThroughOnException() {
  IoContext::current().setFailOpen();
}

ServiceWorkerGlobalScope::ServiceWorkerGlobalScope(v8::Isolate* isolate)
    : unhandledRejections(
        [this](jsg::Lock& js,
                v8::PromiseRejectEvent event,
                jsg::V8Ref<v8::Promise> promise,
                jsg::Value value) {
          // If async context tracking is enabled, then we need to ensure that we enter the frame
          // associated with the promise before we invoke the unhandled rejection callback handling.
          jsg::AsyncContextFrame::Scope scope(js,
              jsg::AsyncContextFrame::tryGetContext(js, promise));
          auto ev = jsg::alloc<PromiseRejectionEvent>(event, kj::mv(promise), kj::mv(value));
          dispatchEventImpl(js, kj::mv(ev));
        }) {}

void ServiceWorkerGlobalScope::clear() {
  removeAllHandlers();
  unhandledRejections.clear();
}

kj::Promise<DeferredProxy<void>> ServiceWorkerGlobalScope::request(
    kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody, kj::HttpService::Response& response,
    kj::Maybe<kj::StringPtr> cfBlobJson,
    Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler) {
  // To construct a ReadableStream object, we're supposed to pass in an Own<AsyncInputStream>, so
  // that it can drop the reference whenever it gets GC'd. But in this case the stream's lifetime
  // is not under our control -- it's attached to the request. So, we wrap it in a
  // NeuterableInputStream which allows us to disconnect the stream before it becomes invalid.
  auto ownRequestBody = kj::refcounted<NeuterableInputStream>(requestBody);
  auto deferredNeuter = kj::defer([ownRequestBody = kj::addRef(*ownRequestBody)]() mutable {
    // Make sure to cancel the request body stream since the native stream is no longer valid once
    // the returned promise completes. Note that the KJ HTTP library deals with the fact that we
    // haven't consumed the entire request body.
    ownRequestBody->neuter(NeuterableInputStream::CLIENT_DISCONNECTED);
  });
  KJ_ON_SCOPE_FAILURE(ownRequestBody->neuter(NeuterableInputStream::THREW_EXCEPTION));

  auto& ioContext = IoContext::current();
  auto isolate = lock.getIsolate();

  kj::Maybe<jsg::V8Ref<v8::Object>> cf;

  KJ_IF_MAYBE(c, cfBlobJson) {
    auto jsonString = jsg::v8Str(isolate, *c);

    auto handle = jsg::check(v8::JSON::Parse(isolate->GetCurrentContext(), jsonString));
    // For the inbound request, we make the `cf` blob immutable.
    jsg::recursivelyFreeze(isolate->GetCurrentContext(), handle);
    KJ_ASSERT(handle->IsObject());
    cf = jsg::V8Ref(isolate, handle.As<v8::Object>());
  }

  auto jsHeaders = jsg::alloc<Headers>(headers, Headers::Guard::REQUEST);
  // We do not automatically decode gzipped request bodies because the fetch() standard doesn't
  // specify any automatic encoding of requests. https://github.com/whatwg/fetch/issues/589
  auto b = newSystemStream(kj::addRef(*ownRequestBody), StreamEncoding::IDENTITY);
  auto jsStream = jsg::alloc<ReadableStream>(ioContext, kj::mv(b));

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
  if (headers.get(kj::HttpHeaderId::CONTENT_LENGTH) != nullptr ||
      headers.get(kj::HttpHeaderId::TRANSFER_ENCODING) != nullptr ||
      requestBody.tryGetLength().orDefault(1) > 0) {
    body = Body::ExtractedBody(jsStream.addRef());
  }

  auto jsRequest = jsg::alloc<Request>(
      method, url, Request::Redirect::MANUAL, kj::mv(jsHeaders),
      jsg::alloc<Fetcher>(IoContext::NEXT_CLIENT_CHANNEL,
                           Fetcher::RequiresHostAndProtocol::YES),
      nullptr /** AbortSignal **/, kj::mv(cf), kj::mv(body));
  // I set the redirect mode to manual here, so that by default scripts that just pass requests
  // through to a fetch() call will behave the same as scripts which don't call .respondWith(): if
  // the request results in a redirect, the visitor will see that redirect.

  auto event = jsg::alloc<FetchEvent>(kj::mv(jsRequest));

  uint tasksBefore = ioContext.taskCount();

  bool useDefaultHandling;
  KJ_IF_MAYBE(h, exportedHandler) {
    KJ_IF_MAYBE(f, h->fetch) {
      auto promise = (*f)(lock, event->getRequest(), h->env.addRef(isolate), h->getCtx(isolate));
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
      response.sendError(500, "Internal Server Error", ioContext.getHeaderTable());
      return addNoopDeferredProxy(kj::READY_NOW);
    } else {
      auto client = ioContext.getHttpClient(
          IoContext::NEXT_CLIENT_CHANNEL, false,
          cfBlobJson.map([](kj::StringPtr s) { return kj::str(s); }),
          "fetch_default"_kj);
      auto adapter = kj::newHttpService(*client);
      auto promise = adapter->request(method, url, headers, requestBody, response);
      // Default handling doesn't rely on the IoContext at all so we can return it as a
      // deferred proxy task.
      return DeferredProxy<void> { promise.attach(kj::mv(adapter), kj::mv(client)) };
    }
  } else KJ_IF_MAYBE(promise, event->getResponsePromise(lock)) {
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

    return ioContext.awaitJs(promise->then(kj::implicitCast<jsg::Lock&>(lock),
        ioContext.addFunctor(
            [&response, allowWebSocket = headers.isWebSocket(),
             canceled = kj::addRef(*canceled), &headers]
            (jsg::Lock& js, jsg::Ref<Response> innerResponse)
            -> IoOwn<kj::Promise<DeferredProxy<void>>> {
      auto& context = IoContext::current();
      if (canceled->value) {
        // Oops, the client disconnected before the response was ready to send. `response` is
        // a dangling reference, let's not use it.
        return context.addObject(kj::heap(addNoopDeferredProxy(kj::READY_NOW)));
      } else {
        return context.addObject(kj::heap(innerResponse->send(
            js, response, { .allowWebSocket = allowWebSocket }, headers)));
      }
    }))).attach(kj::defer([canceled = kj::mv(canceled)]() mutable { canceled->value = true; }))
        .then([ownRequestBody = kj::mv(ownRequestBody), deferredNeuter = kj::mv(deferredNeuter)]
              (DeferredProxy<void> deferredProxy) mutable {
      // In the case of bidirectional streaming, the request body stream needs to remain valid
      // while proxying the response. So, arrange for neutering to happen only after the proxy
      // task finishes.
      deferredProxy.proxyTask = deferredProxy.proxyTask
          .then([body = kj::addRef(*ownRequestBody)]() mutable {
        body->neuter(NeuterableInputStream::SENT_RESPONSE);
      }, [body = kj::addRef(*ownRequestBody)](kj::Exception&& e) mutable {
        body->neuter(NeuterableInputStream::THREW_EXCEPTION);
        kj::throwFatalException(kj::mv(e));
      }).attach(kj::mv(deferredNeuter));

      return deferredProxy;
    }, [body = kj::mv(body2)](kj::Exception&& e) mutable -> DeferredProxy<void> {
      // HACK: We depend on the fact that the success-case lambda above hasn't been destroyed yet
      //   so `deferredNeuter` hasn't been destroyed yet.
      body->neuter(NeuterableInputStream::THREW_EXCEPTION);
      kj::throwFatalException(kj::mv(e));
    });
  } else {
    // The service worker API says that if default handling is prevented and respondWith() wasn't
    // called, the request should result in "a network error".
    return KJ_EXCEPTION(DISCONNECTED, "preventDefault() called but respondWith() not called");
  }
}

void ServiceWorkerGlobalScope::sendTraces(kj::ArrayPtr<kj::Own<Trace>> traces,
    Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler) {
  auto isolate = lock.getIsolate();

  auto event = jsg::alloc<TraceEvent>(traces);

  KJ_IF_MAYBE(h, exportedHandler) {
    KJ_IF_MAYBE(f, h->trace) {
      auto promise = (*f)(lock, event->getTraces(), h->env.addRef(isolate), h->getCtx(isolate));
      event->waitUntil(kj::mv(promise));
    } else {
      lock.logWarningOnce(
          "Attempted to send trace but we lack a handler, "
          "did you remember to export a trace() function?");
      JSG_FAIL_REQUIRE(Error, "Handler does not export a trace() function.");
    }
  } else {
    // Fire off the handlers.
    dispatchEventImpl(lock, event.addRef());

    // We assume no action is necessary for "default" trace handling.
  }
}

void ServiceWorkerGlobalScope::startScheduled(
    kj::Date scheduledTime,
    kj::StringPtr cron,
    Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler) {
  auto& context = IoContext::current();

  double eventTime = (scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS;

  auto event = jsg::alloc<ScheduledEvent>(eventTime, cron);

  auto isolate = lock.getIsolate();

  KJ_IF_MAYBE(h, exportedHandler) {
    KJ_IF_MAYBE(f, h->scheduled) {
      auto promise = (*f)(lock, jsg::alloc<ScheduledController>(event.addRef()),
                          h->env.addRef(isolate), h->getCtx(isolate));
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

kj::Promise<WorkerInterface::AlarmResult> ServiceWorkerGlobalScope::runAlarm(
    kj::Date scheduledTime,
    Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler) {

  auto& context = IoContext::current();
  auto& actor = KJ_ASSERT_NONNULL(context.getActor());
  auto& persistent = KJ_ASSERT_NONNULL(actor.getPersistent());
  auto maybeDeferredDelete = persistent.armAlarmHandler(scheduledTime);

  KJ_IF_MAYBE(deferredDelete, maybeDeferredDelete) {
    auto& handler = KJ_REQUIRE_NONNULL(exportedHandler);
    if (handler.alarm == nullptr) {

      lock.logWarningOnce(
          "Attempted to run a scheduled alarm without a handler, "
          "did you remember to export an alarm() function?");
      return WorkerInterface::AlarmResult {
        .retry = false,
        .outcome = EventOutcome::SCRIPT_NOT_FOUND
      };
    }

    auto& alarm = KJ_ASSERT_NONNULL(handler.alarm);

    auto alarmResultPromise = context
        .run([exportedHandler, &alarm](Worker::Lock& lock)
        -> kj::Promise<WorkerInterface::AlarmResult> {
      return alarm(lock).then([]() -> kj::Promise<WorkerInterface::AlarmResult> {
        return WorkerInterface::AlarmResult {
          .retry = false,
          .outcome = EventOutcome::OK
        };
      });
    });

    return alarmResultPromise
        .catch_([&context, deferredDelete = kj::mv(*deferredDelete)](kj::Exception&& e) mutable {
      auto& actor = KJ_ASSERT_NONNULL(context.getActor());
      auto& persistent = KJ_ASSERT_NONNULL(actor.getPersistent());
      persistent.cancelDeferredAlarmDeletion();

      if (auto desc = e.getDescription();
          !jsg::isTunneledException(desc) && !jsg::isDoNotLogException(desc)) {
        LOG_EXCEPTION("alarmRun"_kj, e);
      }
      EventOutcome outcome = EventOutcome::EXCEPTION;
      KJ_IF_MAYBE(status, context.getLimitEnforcer().getLimitsExceeded()) {
        outcome = *status;
      }
      return WorkerInterface::AlarmResult {
        .retry = true,
        .retryCountsAgainstLimit = !context.isOutputGateBroken(),
        .outcome = outcome
      };
    }).then([&context](WorkerInterface::AlarmResult result) -> kj::Promise<WorkerInterface::AlarmResult> {
      return context.waitForOutputLocks().then([result]() {
        return kj::mv(result);
      }, [](kj::Exception&& e) {
        if (auto desc = e.getDescription();
            !jsg::isTunneledException(desc) && !jsg::isDoNotLogException(desc)) {
          if (isInterestingException(e)) {
            LOG_EXCEPTION("alarmOutputLock"_kj, e);
          } else {
            LOG_NOSENTRY(ERROR, "output lock broke after executing alarm", e);
          }
        }
        return WorkerInterface::AlarmResult {
          .retry = true,
          .retryCountsAgainstLimit = false,
          .outcome = EventOutcome::EXCEPTION
        };
      });
    });
  } else {
    return WorkerInterface::AlarmResult {
      .retry = false,
      .outcome = EventOutcome::CANCELED
    };
  }
}

void ServiceWorkerGlobalScope::emitPromiseRejection(
    jsg::Lock& js,
    v8::PromiseRejectEvent event,
    jsg::V8Ref<v8::Promise> promise,
    jsg::Value value) {

  const auto hasHandlers = [this] {
    return getHandlerCount("unhandledrejection"_kj) +
           getHandlerCount("rejectionhandled"_kj);
  };

  const auto hasInspector = [] {
    if (!IoContext::hasCurrent()) return false;
    return IoContext::current().isInspectorEnabled();
  };

  if (hasHandlers() || hasInspector()) {
    unhandledRejections.report(js, event, kj::mv(promise), kj::mv(value));
  }
}

kj::String ServiceWorkerGlobalScope::btoa(v8::Local<v8::Value> data, v8::Isolate* isolate) {
  auto str = jsg::check(data->ToString(isolate->GetCurrentContext()));

  // We could implement btoa() by accepting a kj::String, but then we'd have to check that it
  // doesn't have any multibyte code points. Easier to perform that test using v8::String's
  // ContainsOnlyOneByte() function.
  JSG_REQUIRE(str->ContainsOnlyOneByte(), DOMInvalidCharacterError,
      "btoa() can only operate on characters in the Latin1 (ISO/IEC 8859-1) range.");

  // TODO(perf): v8::String sometimes holds a char pointer rather than a uint16_t pointer, which is
  //   why v8::String::IsOneByte() is both faster than ContainsOnlyOneByte() and prone to false
  //   negatives. Conceivably we could take advantage of this fact to completely avoid the later
  //   WriteOneByte() call in some cases!
  auto buf = kj::heapArray<kj::byte>(str->Length());
  str->WriteOneByte(isolate, buf.begin(), 0, buf.size());

  return kj::encodeBase64(buf);
}
v8::Local<v8::String> ServiceWorkerGlobalScope::atob(kj::String data, v8::Isolate* isolate) {
  auto decoded = kj::decodeBase64(data.asArray());

  JSG_REQUIRE(!decoded.hadErrors, DOMInvalidCharacterError,
      "atob() called with invalid base64-encoded data. (Only whitespace, '+', '/', alphanumeric "
      "ASCII, and up to two terminal '=' signs when the input data length is divisible by 4 are "
      "allowed.)");

  // Similar to btoa() taking a v8::Value, we return a v8::String directly, as this allows us to
  // construct a string from the non-nul-terminated array returned from decodeBase64(). This avoids
  // making a copy purely to append a nul byte.
  return jsg::v8StrFromLatin1(isolate, decoded.asBytes());
}

void ServiceWorkerGlobalScope::queueMicrotask(
    jsg::Lock& js,
    v8::Local<v8::Function> task) {
  KJ_IF_MAYBE(context, jsg::AsyncContextFrame::current(js)) {
    task = context->wrap(js, task);
  }
  js.v8Isolate->EnqueueMicrotask(task);
}

v8::Local<v8::Value> ServiceWorkerGlobalScope::structuredClone(
    v8::Local<v8::Value> value,
    jsg::Optional<StructuredCloneOptions> maybeOptions,
    v8::Isolate* isolate) {
  kj::Maybe<kj::ArrayPtr<jsg::Value>> transfers;
  KJ_IF_MAYBE(options, maybeOptions) {
    transfers = options->transfer.map([&](kj::Array<jsg::Value>& transfer) {
      return transfer.asPtr();
    });
  }

  return jsg::structuredClone(value, isolate, transfers);
}

TimeoutId::NumberType ServiceWorkerGlobalScope::setTimeoutInternal(
    jsg::Function<void()> function,
    double msDelay) {
  auto timeoutId = IoContext::current().setTimeoutImpl(
      timeoutIdGenerator,
      /** repeats = */ false,
      kj::mv(function),
      msDelay);
  return timeoutId.toNumber();
}

TimeoutId::NumberType ServiceWorkerGlobalScope::setTimeout(
    jsg::Lock& js,
    jsg::V8Ref<v8::Function> function,
    jsg::Optional<double> msDelay,
    jsg::Varargs args) {
  KJ_IF_MAYBE(context, jsg::AsyncContextFrame::current(js)) {
    function = js.v8Ref(context->wrap(js, function));
  }
  auto argv = kj::heapArrayFromIterable<jsg::Value>(kj::mv(args));
  auto timeoutId = IoContext::current().setTimeoutImpl(
      timeoutIdGenerator,
      /* repeats = */ false,
      [function = function.addRef(js),
       argv = kj::mv(argv)]
       (jsg::Lock& js) mutable {
    auto context = js.v8Isolate->GetCurrentContext();
    auto localFunction = function.getHandle(js);
    auto localArgs = KJ_MAP(arg, argv) {
      return arg.getHandle(js);
    };
    auto argc = localArgs.size();

    // Cast to void to discard the result value.
    (void)jsg::check(localFunction->Call(context, context->Global(), argc, &localArgs.front()));
  }, msDelay.orDefault(0));
  return timeoutId.toNumber();
}

void ServiceWorkerGlobalScope::clearTimeout(kj::Maybe<TimeoutId::NumberType> timeoutId) {
  KJ_IF_MAYBE(id, timeoutId) {
    IoContext::current().clearTimeoutImpl(TimeoutId::fromNumber(*id));
  }
}

TimeoutId::NumberType ServiceWorkerGlobalScope::setInterval(
    jsg::Lock& js,
    jsg::V8Ref<v8::Function> function,
    jsg::Optional<double> msDelay,
    jsg::Varargs args) {
  KJ_IF_MAYBE(context, jsg::AsyncContextFrame::current(js)) {
    function = js.v8Ref(context->wrap(js, function));
  }
  auto argv = kj::heapArrayFromIterable<jsg::Value>(kj::mv(args));
  auto timeoutId = IoContext::current().setTimeoutImpl(
      timeoutIdGenerator,
      /* repeats = */ true,
      [function = function.addRef(js),
       argv = kj::mv(argv)]
       (jsg::Lock& js) mutable {
    auto context = js.v8Isolate->GetCurrentContext();
    auto localFunction = function.getHandle(js);
    auto localArgs = KJ_MAP(arg, argv) {
      return arg.getHandle(js);
    };
    auto argc = localArgs.size();

    // Cast to void to discard the result value.
    (void)jsg::check(localFunction->Call(context, context->Global(), argc, &localArgs.front()));
  }, msDelay.orDefault(0));
  return timeoutId.toNumber();
}

jsg::Promise<jsg::Ref<Response>> ServiceWorkerGlobalScope::fetch(
    jsg::Lock& js, kj::OneOf<jsg::Ref<Request>, kj::String> requestOrUrl,
    jsg::Optional<Request::Initializer> requestInit,
    CompatibilityFlags::Reader featureFlags) {
  return fetchImpl(js, nullptr, kj::mv(requestOrUrl), kj::mv(requestInit), featureFlags);
}

jsg::Ref<Socket> ServiceWorkerGlobalScope::connect(
    jsg::Lock& js, kj::String address, jsg::Optional<SocketOptions> options,
    CompatibilityFlags::Reader featureFlags) {
  return connectImpl(js, nullptr, kj::mv(address), featureFlags);
}

}  // namespace workerd::api
