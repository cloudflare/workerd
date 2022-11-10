// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/dom-exception.h>
#include "basics.h"
#include "http.h"
#include "encoding.h"
#include "url.h"
#include "url-standard.h"
#include "urlpattern.h"
#include "form-data.h"
#include "crypto.h"
#include "cache.h"
#include "html-rewriter.h"
#include "trace.h"
#include "scheduled.h"
#include "blob.h"

namespace workerd::api {

using DOMException = jsg::DOMException;
// We need access to DOMException within this namespace so JSG_NESTED_TYPE can name it correctly.

class Navigator: public jsg::Object {
  // A subset of the standard Navigator API.
public:
  kj::StringPtr getUserAgent() { return "Cloudflare-Workers"_kj; }

  JSG_RESOURCE_TYPE(Navigator) {
    JSG_READONLY_INSTANCE_PROPERTY(userAgent, getUserAgent);
  }
};

class PromiseRejectionEvent: public Event {
public:
  PromiseRejectionEvent(
      v8::PromiseRejectEvent type,
      jsg::V8Ref<v8::Promise> promise,
      jsg::Value reason)
      : Event(getEventName(type)),
        promise(kj::mv(promise)),
        reason(kj::mv(reason)) {}

  static jsg::Ref<PromiseRejectionEvent> constructor(kj::String type) = delete;

  jsg::V8Ref<v8::Promise> getPromise(v8::Isolate* isolate) { return promise.addRef(isolate); }
  jsg::Value getReason(v8::Isolate* isolate) { return reason.addRef(isolate); }

  JSG_RESOURCE_TYPE(PromiseRejectionEvent) {
    JSG_INHERIT(Event);
    JSG_READONLY_INSTANCE_PROPERTY(promise, getPromise);
    JSG_READONLY_INSTANCE_PROPERTY(reason, getReason);
  }

private:
  static kj::String getEventName(v8::PromiseRejectEvent type) {
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
  jsg::V8Ref<v8::Promise> promise;
  jsg::Value reason;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(promise, reason);
  }
};

class WorkerGlobalScope: public EventTarget {
public:
  jsg::Unimplemented importScripts(kj::String s) { return {}; };

  JSG_RESOURCE_TYPE(WorkerGlobalScope) {
    JSG_INHERIT(EventTarget);

    JSG_NESTED_TYPE(EventTarget);

    JSG_METHOD(importScripts);

    JSG_TS_DEFINE(type WorkerGlobalScopeEventMap = {
      fetch: FetchEvent;
      scheduled: ScheduledEvent;
      unhandledrejection: PromiseRejectionEvent;
      rejectionhandled: PromiseRejectionEvent;
    });
    JSG_TS_OVERRIDE(extends EventTarget<WorkerGlobalScopeEventMap>);
  }

  static jsg::Ref<WorkerGlobalScope> constructor() = delete;
  // Because EventTarget has a constructor(), we have to explicitly delete
  // the constructor() here or we'll end up with compilation errors
  // (EventTarget's constructor confuses the hasConstructorMethod in resource.h)
};

class ExecutionContext: public jsg::Object {
public:
  void waitUntil(kj::Promise<void> promise);
  void passThroughOnException();

  JSG_RESOURCE_TYPE(ExecutionContext) {
    JSG_METHOD(waitUntil);
    JSG_METHOD(passThroughOnException);
  }
};

struct ExportedHandler {
  // Type signature for handlers exported from the root module.
  //
  // We define each handler method as a LenientOptional rather than as a plain Optional in order to
  // treat incorrect types as if the field is undefined. Without this, Durable Object class
  // constructors that set a field with one of these names would cause confusing type errors.

  typedef jsg::Promise<jsg::Ref<api::Response>> FetchHandler(
      jsg::Ref<api::Request> request, jsg::Value env, jsg::Optional<jsg::Ref<ExecutionContext>> ctx);
  jsg::LenientOptional<jsg::Function<FetchHandler>> fetch;

  typedef kj::Promise<void> TraceHandler(
      kj::Array<jsg::Ref<TraceItem>> traces, jsg::Value env, jsg::Optional<jsg::Ref<ExecutionContext>> ctx);
  jsg::LenientOptional<jsg::Function<TraceHandler>> trace;

  typedef kj::Promise<void> ScheduledHandler(
      jsg::Ref<ScheduledController> controller, jsg::Value env, jsg::Optional<jsg::Ref<ExecutionContext>> ctx);
  jsg::LenientOptional<jsg::Function<ScheduledHandler>> scheduled;

  typedef kj::Promise<void> AlarmHandler();
  // Alarms are only exported on DOs, which receive env bindings from the constructor
  jsg::LenientOptional<jsg::Function<AlarmHandler>> alarm;

  jsg::SelfRef self;
  // Self-ref potentially allows extracting other custom handlers from the object.

  JSG_STRUCT(fetch, trace, scheduled, alarm, self);

  JSG_STRUCT_TS_ROOT();
  // ExportedHandler isn't included in the global scope, but we still want to
  // include it in type definitions.

  JSG_STRUCT_TS_DEFINE(
    type ExportedHandlerFetchHandler<Env = unknown> = (request: Request, env: Env, ctx: ExecutionContext) => Response | Promise<Response>;
    type ExportedHandlerTraceHandler<Env = unknown> = (traces: TraceItem[], env: Env, ctx: ExecutionContext) => void | Promise<void>;
    type ExportedHandlerScheduledHandler<Env = unknown> = (controller: ScheduledController, env: Env, ctx: ExecutionContext) => void | Promise<void>;
  );
  JSG_STRUCT_TS_OVERRIDE(<Env = unknown> {
    fetch?: ExportedHandlerFetchHandler<Env>;
    trace?: ExportedHandlerTraceHandler<Env>;
    scheduled?: ExportedHandlerScheduledHandler<Env>;
    alarm: never;
  });
  // Make `env` parameter generic

  jsg::Value env = nullptr;
  jsg::Optional<jsg::Ref<ExecutionContext>> ctx = nullptr;
  // Values to pass for `env` and `ctx` when calling handlers. Note these have to be the last members
  // so that they don't interfere with `JSG_STRUCT`'s machinations.
  //
  // TODO(cleanup): These are shoved here as a bit of a hack. At present, this is convenient and
  //   works for all use cases. If we have bindings or things on ctx that vary on a per-request basis,
  //   this won't work as well, I guess, but we can cross that bridge when we come to it.

  jsg::Optional<jsg::Ref<ExecutionContext>> getCtx(v8::Isolate* isolate) {
    return ctx.map([&](jsg::Ref<ExecutionContext>& p) { return p.addRef(); });
  }
};

class ServiceWorkerGlobalScope: public WorkerGlobalScope {
  // Global object API exposed to JavaScript.

public:
  ServiceWorkerGlobalScope(v8::Isolate* isolate)
      : unhandledRejections(
          [this](jsg::Lock& js,
                 v8::PromiseRejectEvent event,
                 jsg::V8Ref<v8::Promise> promise,
                 jsg::Value value) {
            auto ev = jsg::alloc<PromiseRejectionEvent>(event, kj::mv(promise), kj::mv(value));
            dispatchEventImpl(js, kj::mv(ev));
          }) {}

  void clear();
  // Drop all references to JavaScript objects so that the context can be garbage-collected. Call
  // this when the context will never be used again and should be disposed.
  //
  // TODO(someday): We should instead implement V8's GC visitor interface so that we don't have
  //   to hold persistent references.

  kj::Promise<DeferredProxy<void>> request(
      kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, kj::HttpService::Response& response,
      kj::Maybe<kj::StringPtr> cfBlobJson,
      Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler);
  // Received request (called from C++, not JS).
  //
  // If `exportedHandler` is provided, the request will be delivered to it rather than to event
  // listeners.
  //
  // TODO(cleanup): Factor out the shared code used between old-style event listeners vs. module
  //   exports and move that code somewhere more appropriate.

  void sendTraces(kj::ArrayPtr<kj::Own<Trace>> traces,
      Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler);
  // Received sendTraces (called from C++, not JS).

  void startScheduled(
      kj::Date scheduledTime,
      kj::StringPtr cron,
      Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler);
  // Start a scheduled event (called from C++, not JS). It is the caller's responsibility to wait
  // for waitUntil()s in order to construct the final ScheduledResult.

  kj::Promise<WorkerInterface::AlarmResult> runAlarm(
      kj::Date scheduledTime,
      Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler);
  // Received runAlarm (called from C++, not JS).

  void emitPromiseRejection(
      jsg::Lock& js,
      v8::PromiseRejectEvent event,
      jsg::V8Ref<v8::Promise> promise,
      jsg::Value value);

  // ---------------------------------------------------------------------------
  // JS API

  kj::String btoa(v8::Local<v8::Value> data, v8::Isolate* isolate);
  v8::Local<v8::String> atob(kj::String data, v8::Isolate* isolate);

  void queueMicrotask(v8::Local<v8::Function> task, v8::Isolate* isolate);

  struct StructuredCloneOptions {
    jsg::Optional<kj::Array<jsg::Value>> transfer;
    JSG_STRUCT(transfer);
    JSG_STRUCT_TS_OVERRIDE(StructuredSerializeOptions);
  };

  v8::Local<v8::Value> structuredClone(
      v8::Local<v8::Value> value,
      jsg::Optional<StructuredCloneOptions> options,
      v8::Isolate* isolate);

  TimeoutId::NumberType setTimeout(
      jsg::V8Ref<v8::Function> function,
      jsg::Optional<double> msDelay,
      jsg::Varargs args,
      v8::Isolate* isolate);
  void clearTimeout(kj::Maybe<TimeoutId::NumberType> timeoutId);

  TimeoutId::NumberType setTimeoutInternal(
      jsg::Function<void()> function,
      double msDelay);

  TimeoutId::NumberType setInterval(
      jsg::V8Ref<v8::Function> function,
      jsg::Optional<double> msDelay,
      jsg::Varargs args,
      v8::Isolate* isolate);
  void clearInterval(kj::Maybe<TimeoutId::NumberType> timeoutId) { clearTimeout(timeoutId); }

  jsg::Promise<jsg::Ref<Response>> fetch(
      jsg::Lock& js, kj::OneOf<jsg::Ref<Request>, kj::String> request,
      jsg::Optional<Request::Initializer> requestInitr,
      CompatibilityFlags::Reader featureFlags);

  jsg::Ref<ServiceWorkerGlobalScope> getSelf() {
    return JSG_THIS;
  }

  jsg::Ref<Crypto> getCrypto() {
    return jsg::alloc<Crypto>();
  }

  jsg::Ref<Scheduler> getScheduler() {
    return jsg::alloc<Scheduler>();
  }

  jsg::Ref<Navigator> getNavigator() {
    return jsg::alloc<Navigator>();
  }

  jsg::Unimplemented getOrigin() { return {}; }
  // TODO(conform): A browser-side service worker returns the origin for the URL on which it was
  //   installed, e.g. https://www.example.com for a service worker downloaded from
  //   https://www.example.com/sw.js. This seems like something we could provide to scripts during
  //   a FetchEvent callback (in between .request() and FetchEvent.respondWith()), and otherwise
  //   throw.

  jsg::Ref<CacheStorage> getCaches() {
    return jsg::alloc<CacheStorage>();
  }

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
    JSG_READONLY_INSTANCE_PROPERTY(origin, getOrigin);

    JSG_NESTED_TYPE(Event);
    JSG_NESTED_TYPE(ExtendableEvent);
    JSG_NESTED_TYPE(PromiseRejectionEvent);
    JSG_NESTED_TYPE(FetchEvent);
    JSG_NESTED_TYPE(TraceEvent);
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

    if (flags.getStreamsJavaScriptControllers()) {
      JSG_NESTED_TYPE(ReadableStreamBYOBRequest);
      JSG_NESTED_TYPE(ReadableStreamDefaultController);
      JSG_NESTED_TYPE(ReadableByteStreamController);
      JSG_NESTED_TYPE(WritableStreamDefaultController);
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

      fetch(input: RequestInfo, init?: RequestInit): Promise<Response>;
    });
  }

  TimeoutId::Generator timeoutIdGenerator;
  // The generator for all timeout IDs associated with this scope.

private:
  jsg::UnhandledRejectionHandler unhandledRejections;

  // Global properties such as scheduler, crypto, caches, self, and origin should
  // be monkeypatchable / mutable at the global scope.
};

#define EW_GLOBAL_SCOPE_ISOLATE_TYPES                    \
  api::WorkerGlobalScope,                                \
  api::ServiceWorkerGlobalScope,                         \
  api::ExecutionContext,                                 \
  api::ExportedHandler,                                  \
  api::ServiceWorkerGlobalScope::StructuredCloneOptions, \
  api::PromiseRejectionEvent,                            \
  api::Navigator
// The list of global-scope.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE
}  // namespace workerd::api
