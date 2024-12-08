// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "basics.h"
#include "hibernation-event-params.h"
#include "http.h"

#include <workerd/io/io-timers.h>
#include <workerd/jsg/jsg.h>
#ifdef WORKERD_EXPERIMENTAL_ENABLE_WEBGPU
#include <workerd/api/gpu/gpu.h>
#endif

namespace workerd::jsg {
class DOMException;
}  // namespace workerd::jsg

namespace workerd::api {

class TailEvent;
class Cache;
class CacheStorage;
class Crypto;
class CryptoKey;
class ErrorEvent;
class EventSource;
class FixedLengthStream;
class SubtleCrypto;
class TextDecoder;
class TextEncoder;
class HTMLRewriter;
class IdentityTransformStream;
class Response;
class TraceItem;
class TransformStream;
class ScheduledController;
class ScheduledEvent;
class ReadableStream;
class ReadableStreamDefaultReader;
class ReadableStreamBYOBReader;
class ReadableStreamBYOBRequest;
class WritableStream;
class WritableStreamDefaultWriter;
class ReadableStreamBYOBRequest;
class ReadableStreamDefaultController;
class ReadableByteStreamController;
class WritableStreamDefaultController;
class CompressionStream;
class DecompressionStream;
class TextEncoderStream;
class TextDecoderStream;
class URLPattern;
class Blob;
class File;
class FormData;

class URL;
class URLSearchParams;
namespace url {
class URL;
class URLSearchParams;
}  // namespace url

// We need access to DOMException within this namespace so JSG_NESTED_TYPE can name it correctly.
using DOMException = jsg::DOMException;

// A subset of the standard Navigator API.
class Navigator: public jsg::Object {
 public:
  kj::StringPtr getUserAgent() {
    return "Cloudflare-Workers"_kj;
  }
#ifdef WORKERD_EXPERIMENTAL_ENABLE_WEBGPU
  jsg::Ref<api::gpu::GPU> getGPU(CompatibilityFlags::Reader flags);
#endif

  bool sendBeacon(jsg::Lock& js, kj::String url, jsg::Optional<Body::Initializer> body);

  JSG_RESOURCE_TYPE(Navigator) {
    JSG_METHOD(sendBeacon);
    JSG_READONLY_INSTANCE_PROPERTY(userAgent, getUserAgent);
#ifdef WORKERD_EXPERIMENTAL_ENABLE_WEBGPU
    JSG_READONLY_INSTANCE_PROPERTY(gpu, getGPU);
#endif
  }
};

class Performance: public jsg::Object {
 public:
  // We always return a time origin of 0, making performance.now() equivalent to Date.now(). There
  // is no other appropriate time origin to use given that the Worker platform is intended to be
  // treated like one big computer rather than many individual instances. In particular, if and
  // when we start snapshotting applications after startup and then starting instances from that
  // snapshot, what would the right time origin be? The time when the snapshot was created? This
  // seems to leak implementation details in a weird way.
  //
  // Note that the purpose of `timeOrigin` is normally to allow `now()` to return a more-precise
  // measurement. Measuring against a recent time allows the values returned by `now()` to be
  // smaller in magnitude, which allows them to be more precise due to the nature of floating
  // point numbers. In our case, though, we don't return precise measurements from this interface
  // anyway, for Spectre reasons -- it returns the same as Date.now().
  double getTimeOrigin() {
    return 0.0;
  }

  double now();

  JSG_RESOURCE_TYPE(Performance) {
    JSG_READONLY_INSTANCE_PROPERTY(timeOrigin, getTimeOrigin);
    JSG_METHOD(now);
  }
};

// Exposed as a global to provide access to certain Cloudflare-specific
// configuration details. This is not a standard API and great care should
// be taken when deciding to expose new properties or methods here.
class Cloudflare: public jsg::Object {
 public:
  // Return an object containing the state of all compatibility flags known to the runtime.
  jsg::JsObject getCompatibilityFlags(jsg::Lock& js);

  JSG_RESOURCE_TYPE(Cloudflare) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(compatibilityFlags, getCompatibilityFlags);

    JSG_TS_OVERRIDE({ readonly compatibilityFlags: Record<string, boolean>;
    });
  }
};

class PromiseRejectionEvent: public Event {
 public:
  PromiseRejectionEvent(
      v8::PromiseRejectEvent type, jsg::V8Ref<v8::Promise> promise, jsg::Value reason);

  static jsg::Ref<PromiseRejectionEvent> constructor(kj::String type) = delete;

  jsg::V8Ref<v8::Promise> getPromise(jsg::Lock& js) {
    return promise.addRef(js);
  }
  jsg::Value getReason(jsg::Lock& js) {
    return reason.addRef(js);
  }

  JSG_RESOURCE_TYPE(PromiseRejectionEvent) {
    JSG_INHERIT(Event);
    JSG_READONLY_INSTANCE_PROPERTY(promise, getPromise);
    JSG_READONLY_INSTANCE_PROPERTY(reason, getReason);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("promise", promise);
    tracker.trackField("reason", reason);
  }

 private:
  jsg::V8Ref<v8::Promise> promise;
  jsg::Value reason;

  void visitForGc(jsg::GcVisitor& visitor);
};

class WorkerGlobalScope: public EventTarget, public jsg::ContextGlobal {
 public:
  jsg::Unimplemented importScripts(kj::String s) {
    return {};
  };

  JSG_RESOURCE_TYPE(WorkerGlobalScope, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(EventTarget);

    JSG_NESTED_TYPE(EventTarget);

    if (!flags.getNoImportScripts()) {
      JSG_METHOD(importScripts);
    }

    JSG_TS_DEFINE(type WorkerGlobalScopeEventMap = {
      fetch: FetchEvent;
      scheduled: ScheduledEvent;
      queue: QueueEvent;
      unhandledrejection: PromiseRejectionEvent;
      rejectionhandled: PromiseRejectionEvent;
    });
    JSG_TS_OVERRIDE(extends EventTarget<WorkerGlobalScopeEventMap>);
  }

  // Because EventTarget has a constructor(), we have to explicitly delete
  // the constructor() here or we'll end up with compilation errors
  // (EventTarget's constructor confuses the hasConstructorMethod in resource.h)
  static jsg::Ref<WorkerGlobalScope> constructor() = delete;
};

// Controller type for test handler.
//
// At present, this has no methods. It is defined for consistency with other handlers and on the
// assumption that we'll probably want to put something here someday.
class TestController: public jsg::Object {
 public:
  JSG_RESOURCE_TYPE(TestController) {}
};

class ExecutionContext: public jsg::Object {
 public:
  ExecutionContext(jsg::Lock& js): props(js, js.obj()) {}
  ExecutionContext(jsg::Lock& js, jsg::JsValue props): props(js, props) {}

  void waitUntil(kj::Promise<void> promise);
  void passThroughOnException();

  // Cancels the current execution context with the given exception, causing all execution to stop
  // and throwing an error at the client.
  void abort(jsg::Lock& js, jsg::Optional<jsg::Value> reason);

  jsg::JsValue getProps(jsg::Lock& js) {
    return props.getHandle(js);
  }

  JSG_RESOURCE_TYPE(ExecutionContext, CompatibilityFlags::Reader flags) {
    JSG_METHOD(waitUntil);
    JSG_METHOD(passThroughOnException);
    JSG_LAZY_INSTANCE_PROPERTY(props, getProps);

    if (flags.getWorkerdExperimental()) {
      // TODO(soon): Before making this generally available we need to:
      // * Consider whether to use TerminateExecution() instead of throwing.
      // * Make sure it's really not possible for more code to run in the context after abort().
      //   Currently, abort() triggers in a partially async way so there's an opportunity for some
      //   other event in the event queue to squeeze in.
      // * Try to ensure that the provided error is actually the one that propagates out of event
      //   handlers. Currently this is not consistently true.
      // * Make sure all event handlers actually honor onAbort().
      // * Enable the Durable Object version at the same time -- and make sure they're suitably
      //   consistent with each other.
      JSG_METHOD(abort);
    }
  }

 private:
  jsg::JsRef<jsg::JsValue> props;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(props);
  }
};

// AlarmEventInfo is a jsg::Object used to pass alarm invocation info to an alarm handler.
class AlarmInvocationInfo: public jsg::Object {
 public:
  AlarmInvocationInfo(uint32_t retry): retryCount(retry) {}

  bool getIsRetry() {
    return retryCount > 0;
  }
  uint32_t getRetryCount() {
    return retryCount;
  }

  JSG_RESOURCE_TYPE(AlarmInvocationInfo) {
    JSG_READONLY_INSTANCE_PROPERTY(isRetry, getIsRetry);
    JSG_READONLY_INSTANCE_PROPERTY(retryCount, getRetryCount);
  }

 private:
  uint32_t retryCount = 0;
};

// Type signature for handlers exported from the root module.
//
// We define each handler method as a LenientOptional rather than as a plain Optional in order to
// treat incorrect types as if the field is undefined. Without this, Durable Object class
// constructors that set a field with one of these names would cause confusing type errors.
struct ExportedHandler {
  typedef jsg::Promise<jsg::Ref<api::Response>> FetchHandler(jsg::Ref<api::Request> request,
      jsg::Value env,
      jsg::Optional<jsg::Ref<ExecutionContext>> ctx);
  jsg::LenientOptional<jsg::Function<FetchHandler>> fetch;

  typedef kj::Promise<void> TailHandler(kj::Array<jsg::Ref<TraceItem>> events,
      jsg::Value env,
      jsg::Optional<jsg::Ref<ExecutionContext>> ctx);
  jsg::LenientOptional<jsg::Function<TailHandler>> tail;
  jsg::LenientOptional<jsg::Function<TailHandler>> trace;

  typedef kj::Promise<void> ScheduledHandler(jsg::Ref<ScheduledController> controller,
      jsg::Value env,
      jsg::Optional<jsg::Ref<ExecutionContext>> ctx);
  jsg::LenientOptional<jsg::Function<ScheduledHandler>> scheduled;

  typedef kj::Promise<void> AlarmHandler(jsg::Ref<AlarmInvocationInfo> alarmInfo);
  // Alarms are only exported on DOs, which receive env bindings from the constructor
  jsg::LenientOptional<jsg::Function<AlarmHandler>> alarm;

  typedef jsg::Promise<void> TestHandler(jsg::Ref<TestController> controller,
      jsg::Value env,
      jsg::Optional<jsg::Ref<ExecutionContext>> ctx);
  jsg::LenientOptional<jsg::Function<TestHandler>> test;

  typedef kj::Promise<void> HibernatableWebSocketMessageHandler(
      jsg::Ref<WebSocket>, kj::OneOf<kj::String, kj::Array<byte>> message);
  jsg::LenientOptional<jsg::Function<HibernatableWebSocketMessageHandler>> webSocketMessage;

  typedef kj::Promise<void> HibernatableWebSocketCloseHandler(
      jsg::Ref<WebSocket>, int code, kj::String reason, bool wasClean);
  jsg::LenientOptional<jsg::Function<HibernatableWebSocketCloseHandler>> webSocketClose;

  typedef kj::Promise<void> HibernatableWebSocketErrorHandler(jsg::Ref<WebSocket>, jsg::Value);
  jsg::LenientOptional<jsg::Function<HibernatableWebSocketErrorHandler>> webSocketError;

  // Self-ref potentially allows extracting other custom handlers from the object.
  jsg::SelfRef self;

  JSG_STRUCT(fetch,
      tail,
      trace,
      scheduled,
      alarm,
      test,
      webSocketMessage,
      webSocketClose,
      webSocketError,
      self);

  JSG_STRUCT_TS_ROOT();
  // ExportedHandler isn't included in the global scope, but we still want to
  // include it in type definitions.

  JSG_STRUCT_TS_DEFINE(
    type ExportedHandlerFetchHandler<Env = unknown, CfHostMetadata = unknown> = (request: Request<CfHostMetadata, IncomingRequestCfProperties<CfHostMetadata>>, env: Env, ctx: ExecutionContext) => Response | Promise<Response>;
    type ExportedHandlerTailHandler<Env = unknown> = (events: TraceItem[], env: Env, ctx: ExecutionContext) => void | Promise<void>;
    type ExportedHandlerTraceHandler<Env = unknown> = (traces: TraceItem[], env: Env, ctx: ExecutionContext) => void | Promise<void>;
    type ExportedHandlerScheduledHandler<Env = unknown> = (controller: ScheduledController, env: Env, ctx: ExecutionContext) => void | Promise<void>;
    type ExportedHandlerQueueHandler<Env = unknown, Message = unknown> = (batch: MessageBatch<Message>, env: Env, ctx: ExecutionContext) => void | Promise<void>;
    type ExportedHandlerTestHandler<Env = unknown> = (controller: TestController, env: Env, ctx: ExecutionContext) => void | Promise<void>;
  );
  JSG_STRUCT_TS_OVERRIDE(<Env = unknown, QueueHandlerMessage = unknown, CfHostMetadata = unknown> {
    email?: EmailExportedHandler<Env>;
    fetch?: ExportedHandlerFetchHandler<Env, CfHostMetadata>;
    tail?: ExportedHandlerTailHandler<Env>;
    trace?: ExportedHandlerTraceHandler<Env>;
    scheduled?: ExportedHandlerScheduledHandler<Env>;
    alarm: never;
    webSocketMessage: never;
    webSocketClose: never;
    webSocketError: never;
    queue?: ExportedHandlerQueueHandler<Env, QueueHandlerMessage>;
    test?: ExportedHandlerTestHandler<Env>;
  });
  // Make `env` parameter generic

  // Values to pass for `env` and `ctx` when calling handlers. Note these have to be the last members
  // so that they don't interfere with `JSG_STRUCT`'s machinations.

  // env and ctx values that need to be passed to the handler function. If the ExportedHandler
  // represents a class instance (e.g. Durable Object instance), then `env` is is the JS value
  // `undefined` and `ctx` is `kj::none`.
  // TODO(cleanup): Why isn't `env` a `jsg::Optional` too? Or maybe the pair should be wrapped in
  //   a struct that is `Maybe`?
  jsg::Value env = nullptr;
  jsg::Optional<jsg::Ref<ExecutionContext>> ctx = kj::none;
  // TODO(cleanup): These are shoved here as a bit of a hack. At present, this is convenient and
  //   works for all use cases. If we have bindings or things on ctx that vary on a per-request basis,
  //   this won't work as well, I guess, but we can cross that bridge when we come to it.

  // If true, this is a Durable Object class that failed to extend `DurableObject`. We will not
  // permit RPC to this class.
  bool missingSuperclass = false;

  jsg::Optional<jsg::Ref<ExecutionContext>> getCtx() {
    return ctx.map([&](jsg::Ref<ExecutionContext>& p) { return p.addRef(); });
  }
};

// An approximation of Node.js setImmediate `Immediate` object.
// This is used only when the `nodejs_compat_v2` compatibility flag is enabled.
class Immediate final: public jsg::Object {
 public:
  Immediate(IoContext& context, TimeoutId timeoutId);

  // In Node.js, the "ref" mechanism refers to whether or not an i/o object
  // will keep the libuv event loop alive (and therefore keep the process alive).
  // We do not implement a similar mechanism in workerd. These are here only to
  // satisfy the API contract for the `Immediate` object but are never expected
  // to actually do anything.
  bool hasRef() {
    return false;
  }
  void ref() { /* non-op */ }
  void unref() { /* non-op */ }

  void dispose();

  JSG_RESOURCE_TYPE(Immediate) {
    JSG_METHOD(ref);
    JSG_METHOD(unref);
    JSG_METHOD(hasRef);
    JSG_DISPOSE(dispose);
  }

 private:
  // On the off chance user code holds onto to the Ref<Immediate> longer than
  // the IoContext remains alive, let's maintain just a weak reference to the
  // IoContext here to avoid problems. This reference is used only for handling
  // the dipose operation, so it should be perfectly fine for it to be weak
  // and a non-op after the IoContext is gone.
  kj::Own<IoContext::WeakRef> contextRef;
  TimeoutId timeoutId;
};

// Global object API exposed to JavaScript.
class ServiceWorkerGlobalScope: public WorkerGlobalScope {
 public:
  ServiceWorkerGlobalScope(v8::Isolate* isolate);

  // Drop all references to JavaScript objects so that the context can be garbage-collected. Call
  // this when the context will never be used again and should be disposed.
  void clear();
  // TODO(someday): We should instead implement V8's GC visitor interface so that we don't have
  //   to hold persistent references.

  // Received request (called from C++, not JS).
  //
  // If `exportedHandler` is provided, the request will be delivered to it rather than to event
  // listeners.
  kj::Promise<DeferredProxy<void>> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response,
      kj::Maybe<kj::StringPtr> cfBlobJson,
      Worker::Lock& lock,
      kj::Maybe<ExportedHandler&> exportedHandler);
  // TODO(cleanup): Factor out the shared code used between old-style event listeners vs. module
  //   exports and move that code somewhere more appropriate.

  // Received sendTraces (called from C++, not JS).
  void sendTraces(kj::ArrayPtr<kj::Own<Trace>> traces,
      Worker::Lock& lock,
      kj::Maybe<ExportedHandler&> exportedHandler);

  // Start a scheduled event (called from C++, not JS). It is the caller's responsibility to wait
  // for waitUntil()s in order to construct the final ScheduledResult.
  void startScheduled(kj::Date scheduledTime,
      kj::StringPtr cron,
      Worker::Lock& lock,
      kj::Maybe<ExportedHandler&> exportedHandler);

  // Received runAlarm (called from C++, not JS).
  kj::Promise<WorkerInterface::AlarmResult> runAlarm(kj::Date scheduledTime,
      kj::Duration timeout,
      uint32_t retryCount,
      Worker::Lock& lock,
      kj::Maybe<ExportedHandler&> exportedHandler);

  // Received test() (called from C++, not JS). See WorkerInterface::test(). This version returns
  // a jsg::Promise<void>; it fails if an exception is thrown. WorkerEntrypoint will catch these
  // and report them.
  jsg::Promise<void> test(Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler);

  kj::Promise<void> eventTimeoutPromise(uint32_t timeoutMs);
  kj::Promise<void> setHibernatableEventTimeout(
      kj::Promise<void> event, kj::Maybe<uint32_t> eventTimeoutMs);

  void sendHibernatableWebSocketMessage(kj::OneOf<kj::String, kj::Array<byte>> message,
      kj::Maybe<uint32_t> eventTimeoutMs,
      kj::String websocketId,
      Worker::Lock& lock,
      kj::Maybe<ExportedHandler&> exportedHandler);

  void sendHibernatableWebSocketClose(HibernatableSocketParams::Close close,
      kj::Maybe<uint32_t> eventTimeoutMs,
      kj::String websocketId,
      Worker::Lock& lock,
      kj::Maybe<ExportedHandler&> exportedHandler);

  void sendHibernatableWebSocketError(kj::Exception e,
      kj::Maybe<uint32_t> eventTimeoutMs,
      kj::String websocketId,
      Worker::Lock& lock,
      kj::Maybe<ExportedHandler&> exportedHandler);

  void emitPromiseRejection(jsg::Lock& js,
      v8::PromiseRejectEvent event,
      jsg::V8Ref<v8::Promise> promise,
      jsg::Value value);

  // ---------------------------------------------------------------------------
  // JS API

  jsg::JsString btoa(jsg::Lock& js, jsg::JsValue data);
  jsg::JsString atob(jsg::Lock& js, kj::String data);

  void queueMicrotask(jsg::Lock& js, v8::Local<v8::Function> task);

  struct StructuredCloneOptions {
    jsg::Optional<kj::Array<jsg::JsRef<jsg::JsValue>>> transfer;
    JSG_STRUCT(transfer);
    JSG_STRUCT_TS_OVERRIDE(StructuredSerializeOptions);
  };

  jsg::JsValue structuredClone(
      jsg::Lock& js, jsg::JsValue value, jsg::Optional<StructuredCloneOptions> options);

  TimeoutId::NumberType setTimeout(jsg::Lock& js,
      jsg::Function<void(jsg::Arguments<jsg::Value>)> function,
      jsg::Optional<double> msDelay,
      jsg::Arguments<jsg::Value> args);
  void clearTimeout(kj::Maybe<TimeoutId::NumberType> timeoutId);

  TimeoutId::NumberType setTimeoutInternal(jsg::Function<void()> function, double msDelay);

  TimeoutId::NumberType setInterval(jsg::Lock& js,
      jsg::Function<void(jsg::Arguments<jsg::Value>)> function,
      jsg::Optional<double> msDelay,
      jsg::Arguments<jsg::Value> args);
  void clearInterval(kj::Maybe<TimeoutId::NumberType> timeoutId) {
    clearTimeout(timeoutId);
  }

  jsg::Promise<jsg::Ref<Response>> fetch(jsg::Lock& js,
      kj::OneOf<jsg::Ref<Request>, kj::String> request,
      jsg::Optional<Request::Initializer> requestInitr);

  jsg::Ref<ServiceWorkerGlobalScope> getSelf() {
    return JSG_THIS;
  }

  // Implemented in global-scope.c++ to avoid including crypto.h
  jsg::Ref<Crypto> getCrypto();

  jsg::Ref<Scheduler> getScheduler() {
    return jsg::alloc<Scheduler>();
  }

  jsg::Ref<Navigator> getNavigator() {
    return jsg::alloc<Navigator>();
  }

  jsg::Ref<Performance> getPerformance() {
    return jsg::alloc<Performance>();
  }

  jsg::Ref<Cloudflare> getCloudflare() {
    return jsg::alloc<Cloudflare>();
  }

  // The origin is unknown, return "null" as described in
  // https://html.spec.whatwg.org/multipage/browsers.html#concept-origin-opaque.
  kj::StringPtr getOrigin() {
    return "null";
  }

  jsg::Ref<CacheStorage> getCaches();

  void reportError(jsg::Lock& js, jsg::JsValue error);

  // When the nodejs_compat_v2 compatibility flag is enabled, we expose the Node.js
  // compat Buffer and process at the global scope in all modules as lazy instance
  // properties.
  jsg::JsValue getBuffer(jsg::Lock& js);
  jsg::JsValue getProcess(jsg::Lock& js);
  jsg::Ref<Immediate> setImmediate(jsg::Lock& js,
      jsg::Function<void(jsg::Arguments<jsg::Value>)> function,
      jsg::Arguments<jsg::Value> args);
  void clearImmediate(kj::Maybe<jsg::Ref<Immediate>> immediate);

  JSG_RESOURCE_TYPE(ServiceWorkerGlobalScope, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(WorkerGlobalScope);

    JSG_NESTED_TYPE(DOMException);
    JSG_NESTED_TYPE(WorkerGlobalScope);

    JSG_METHOD(btoa);
    JSG_METHOD(atob);

    JSG_METHOD(setTimeout);
    JSG_METHOD(clearTimeout);
    JSG_METHOD(setInterval);
    JSG_METHOD(clearInterval);
    JSG_METHOD(queueMicrotask);
    JSG_METHOD(structuredClone);
    JSG_METHOD(reportError);

    JSG_METHOD(fetch);

    // Unlike regular interface attributes, which Web IDL requires us to
    // implement as prototype properties, the global scope is special --
    // interface attributes defined on the global scope must be implemented as
    // instance properties. As an additional wrinkle, many of these properties
    // are supposed to be readonly, but in practice most browsers do not fully
    // honor that part of the spec, and allow user scripts to override many of
    // the properties.
    //
    // Using JSG_LAZY_INSTANCE_PROPERTY here to expose new global properties
    // ensures that any new global property we expose can be monkeypatched by
    // user code without us having to handle any of the storage. The
    // first time the properties are accessed, the getter will be invoked
    // if the user has not already set the value for the property themselves.
    // This should be the default choice for all new global properties that
    // are not methods or nested types.
    //
    // We make an exception for origin, and define it as a readonly instance
    // property, because we currently do not provide any implementation for it.

    JSG_LAZY_INSTANCE_PROPERTY(self, getSelf);
    JSG_LAZY_INSTANCE_PROPERTY(crypto, getCrypto);
    JSG_LAZY_INSTANCE_PROPERTY(caches, getCaches);
    JSG_LAZY_INSTANCE_PROPERTY(scheduler, getScheduler);
    JSG_LAZY_INSTANCE_PROPERTY(performance, getPerformance);
    JSG_LAZY_INSTANCE_PROPERTY(Cloudflare, getCloudflare);
    JSG_READONLY_INSTANCE_PROPERTY(origin, getOrigin);

    JSG_NESTED_TYPE(Event);
    JSG_NESTED_TYPE(ExtendableEvent);
    JSG_NESTED_TYPE(CustomEvent);
    JSG_NESTED_TYPE(PromiseRejectionEvent);
    JSG_NESTED_TYPE(FetchEvent);
    JSG_NESTED_TYPE(TailEvent);
    JSG_NESTED_TYPE_NAMED(TailEvent, TraceEvent);
    JSG_NESTED_TYPE(ScheduledEvent);
    JSG_NESTED_TYPE(MessageEvent);
    JSG_NESTED_TYPE(CloseEvent);
    JSG_NESTED_TYPE(ReadableStreamDefaultReader);
    JSG_NESTED_TYPE(ReadableStreamBYOBReader);
    JSG_NESTED_TYPE(ReadableStream);
    JSG_NESTED_TYPE(WritableStream);
    JSG_NESTED_TYPE(WritableStreamDefaultWriter);
    JSG_NESTED_TYPE(TransformStream);
    JSG_NESTED_TYPE(ByteLengthQueuingStrategy);
    JSG_NESTED_TYPE(CountQueuingStrategy);
    JSG_NESTED_TYPE(ErrorEvent);

    JSG_NESTED_TYPE(EventSource);

    if (flags.getStreamsJavaScriptControllers()) {
      JSG_NESTED_TYPE(ReadableStreamBYOBRequest);
      JSG_NESTED_TYPE(ReadableStreamDefaultController);
      JSG_NESTED_TYPE(ReadableByteStreamController);
      JSG_NESTED_TYPE(WritableStreamDefaultController);
      JSG_NESTED_TYPE(TransformStreamDefaultController);
    }

    if (flags.getNodeJsCompatV2()) {
      JSG_LAZY_INSTANCE_PROPERTY(Buffer, getBuffer);
      JSG_LAZY_INSTANCE_PROPERTY(process, getProcess);
      JSG_LAZY_INSTANCE_PROPERTY(global, getSelf);
      JSG_METHOD(setImmediate);
      JSG_METHOD(clearImmediate);
    }

    JSG_NESTED_TYPE(CompressionStream);
    JSG_NESTED_TYPE(DecompressionStream);
    JSG_NESTED_TYPE(TextEncoderStream);
    JSG_NESTED_TYPE(TextDecoderStream);

    JSG_NESTED_TYPE(Headers);
    JSG_NESTED_TYPE(Body);
    JSG_NESTED_TYPE(Request);
    JSG_NESTED_TYPE(Response);
    JSG_NESTED_TYPE(WebSocket);
    JSG_NESTED_TYPE(WebSocketPair);
    JSG_NESTED_TYPE(WebSocketRequestResponsePair);

    JSG_NESTED_TYPE(AbortController);
    JSG_NESTED_TYPE(AbortSignal);

    JSG_NESTED_TYPE(TextDecoder);
    JSG_NESTED_TYPE(TextEncoder);

    if (flags.getGlobalNavigator()) {
      JSG_LAZY_INSTANCE_PROPERTY(navigator, getNavigator);
      JSG_NESTED_TYPE(Navigator);
    }

    if (flags.getSpecCompliantUrl()) {
      JSG_NESTED_TYPE_NAMED(url::URL, URL);
      JSG_NESTED_TYPE_NAMED(url::URLSearchParams, URLSearchParams);
    } else {
      JSG_NESTED_TYPE(URL);
      JSG_NESTED_TYPE(URLSearchParams);
    }
    JSG_NESTED_TYPE(URLPattern);

    JSG_NESTED_TYPE(Blob);
    JSG_NESTED_TYPE(File);
    JSG_NESTED_TYPE(FormData);

    JSG_NESTED_TYPE(Crypto);
    JSG_NESTED_TYPE(SubtleCrypto);
    JSG_NESTED_TYPE(CryptoKey);

    JSG_NESTED_TYPE(CacheStorage);
    JSG_NESTED_TYPE(Cache);

    // Off-spec extensions.
    JSG_NESTED_TYPE(FixedLengthStream);
    JSG_NESTED_TYPE(IdentityTransformStream);
    JSG_NESTED_TYPE(HTMLRewriter);

#ifdef WORKERD_EXPERIMENTAL_ENABLE_WEBGPU
    // WebGPU
    JSG_NESTED_TYPE_NAMED(api::gpu::GPUAdapter, GPUAdapter);
    JSG_NESTED_TYPE_NAMED(api::gpu::GPUOutOfMemoryError, GPUOutOfMemoryError);
    JSG_NESTED_TYPE_NAMED(api::gpu::GPUValidationError, GPUValidationError);
    JSG_NESTED_TYPE_NAMED(api::gpu::GPUInternalError, GPUInternalError);
    JSG_NESTED_TYPE_NAMED(api::gpu::GPUDeviceLostInfo, GPUDeviceLostInfo);
    JSG_NESTED_TYPE_NAMED(api::gpu::GPUBufferUsage, GPUBufferUsage);
    JSG_NESTED_TYPE_NAMED(api::gpu::GPUShaderStage, GPUShaderStage);
    JSG_NESTED_TYPE_NAMED(api::gpu::GPUMapMode, GPUMapMode);
    JSG_NESTED_TYPE_NAMED(api::gpu::GPUTextureUsage, GPUTextureUsage);
    JSG_NESTED_TYPE_NAMED(api::gpu::GPUColorWrite, GPUColorWrite);
#endif

    JSG_TS_ROOT();
    JSG_TS_DEFINE(
      interface Console {
        "assert"(condition?: boolean, ...data: any[]): void;
        clear(): void;
        count(label?: string): void;
        countReset(label?: string): void;
        debug(...data: any[]): void;
        dir(item?: any, options?: any): void;
        dirxml(...data: any[]): void;
        error(...data: any[]): void;
        group(...data: any[]): void;
        groupCollapsed(...data: any[]): void;
        groupEnd(): void;
        info(...data: any[]): void;
        log(...data: any[]): void;
        table(tabularData?: any, properties?: string[]): void;
        time(label?: string): void;
        timeEnd(label?: string): void;
        timeLog(label?: string, ...data: any[]): void;
        timeStamp(label?: string): void;
        trace(...data: any[]): void;
        warn(...data: any[]): void;
      }
      const console: Console;

      type BufferSource = ArrayBufferView | ArrayBuffer;
      type TypedArray =
        | Int8Array
        | Uint8Array
        | Uint8ClampedArray
        | Int16Array
        | Uint16Array
        | Int32Array
        | Uint32Array
        | Float32Array
        | Float64Array
        | BigInt64Array
        | BigUint64Array;

      namespace WebAssembly {
        class CompileError extends Error {
          constructor(message?: string);
        }
        class RuntimeError extends Error {
          constructor(message?: string);
        }

        type ValueType = "anyfunc" | "externref" | "f32" | "f64" | "i32" | "i64" | "v128";
        interface GlobalDescriptor {
          value: ValueType;
          mutable?: boolean;
        }
        class Global {
          constructor(descriptor: GlobalDescriptor, value?: any);
          value: any;
          valueOf(): any;
        }

        type ImportValue = ExportValue | number;
        type ModuleImports = Record<string, ImportValue>;
        type Imports = Record<string, ModuleImports>;
        type ExportValue = Function | Global | Memory | Table;
        type Exports = Record<string, ExportValue>;
        class Instance {
          constructor(module: Module, imports?: Imports);
          readonly exports: Exports;
        }

        interface MemoryDescriptor {
          initial: number;
          maximum?: number;
          shared?: boolean;
        }
        class Memory {
          constructor(descriptor: MemoryDescriptor);
          readonly buffer: ArrayBuffer;
          grow(delta: number): number;
        }

        type ImportExportKind = "function" | "global" | "memory" | "table";
        interface ModuleExportDescriptor {
          kind: ImportExportKind;
          name: string;
        }
        interface ModuleImportDescriptor {
          kind: ImportExportKind;
          module: string;
          name: string;
        }
        abstract class Module {
          static customSections(module: Module, sectionName: string): ArrayBuffer[];
          static exports(module: Module): ModuleExportDescriptor[];
          static imports(module: Module): ModuleImportDescriptor[];
        }

        type TableKind = "anyfunc" | "externref";
        interface TableDescriptor {
          element: TableKind;
          initial: number;
          maximum?: number;
        }
        class Table {
          constructor(descriptor: TableDescriptor, value?: any);
          readonly length: number;
          get(index: number): any;
          grow(delta: number, value?: any): number;
          set(index: number, value?: any): void;
        }

        function instantiate(module: Module, imports?: Imports): Promise<Instance>;
        function validate(bytes: BufferSource): boolean;
      }
    );
    // workerd disables dynamic WebAssembly compilation, so `compile()`, `compileStreaming()`, the
    // `instantiate()` override taking a `BufferSource` and `instantiateStreaming()` are omitted.
    // `Module` is also declared `abstract` to disable its `BufferSource` constructor.

    JSG_TS_OVERRIDE({
      btoa(data: string): string;

      setTimeout(callback: (...args: any[]) => void, msDelay?: number): number;
      setTimeout<Args extends any[]>(callback: (...args: Args) => void, msDelay?: number, ...args: Args): number;

      setInterval(callback: (...args: any[]) => void, msDelay?: number): number;
      setInterval<Args extends any[]>(callback: (...args: Args) => void, msDelay?: number, ...args: Args): number;

      structuredClone<T>(value: T, options?: StructuredSerializeOptions): T;

      fetch(input: RequestInfo | URL, init?: RequestInit<RequestInitCfProperties>): Promise<Response>;
    });
  }

  TimeoutId::Generator timeoutIdGenerator;
  // The generator for all timeout IDs associated with this scope.

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("unhandledRejections", unhandledRejections);
  }

 private:
  jsg::UnhandledRejectionHandler unhandledRejections;

  // Global properties such as scheduler, crypto, caches, self, and origin should
  // be monkeypatchable / mutable at the global scope.
};

#define EW_GLOBAL_SCOPE_ISOLATE_TYPES                                                              \
  api::WorkerGlobalScope, api::ServiceWorkerGlobalScope, api::TestController,                      \
      api::ExecutionContext, api::ExportedHandler,                                                 \
      api::ServiceWorkerGlobalScope::StructuredCloneOptions, api::PromiseRejectionEvent,           \
      api::Navigator, api::Performance, api::AlarmInvocationInfo, api::Immediate, api::Cloudflare
// The list of global-scope.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE
}  // namespace workerd::api
