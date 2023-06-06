// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Classes to manage lifetime of workers, scripts, and isolates.

#include <workerd/io/worker-interface.h>
#include <workerd/io/limit-enforcer.h>
#include <kj/compat/http.h>
#include <workerd/io/outcome.capnp.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/async-context.h>
#include <kj/mutex.h>
#include <workerd/io/io-channels.h>
#include <workerd/io/actor-storage.capnp.h>
#include <workerd/io/request-tracker.h>
#include <workerd/io/actor-cache.h>  // because we can't forward-declare ActorCache::SharedLru.

namespace v8 { class Isolate; }

namespace workerd {

namespace jsg {
  class V8System;
  class V8StackScope;
  class DOMException;
  class ModuleRegistry;
}

namespace api {
  class DurableObjectState;
  class DurableObjectStorage;
  class ServiceWorkerGlobalScope;
  struct ExportedHandler;
  struct CryptoAlgorithm;
  struct QueueExportedHandler;
  class WebSocket;
}

class IoContext;
class InputGate;
class OutputGate;

typedef jsg::Constructor<api::ExportedHandler(
      jsg::Ref<api::DurableObjectState> durableObject, jsg::Value env)>
    DurableObjectConstructor;
// Type signature of a durable object implementation class.

typedef kj::OneOf<DurableObjectConstructor, api::ExportedHandler> NamedExport;
// The type of a top-level export -- either a simple handler or a durable object class.

class Worker: public kj::AtomicRefcounted {
  // An instance of a Worker.
  //
  // Typically each worker script is loaded into a single Worker instance which is reused by
  // multiple requests. The Worker can only be used by one thread at a time, so multiple requests
  // for the same worker can block each other. JavaScript code is asynchronous, though, so any such
  // blocking should be brief.
  //
  // Note: This class should be referred to as "Worker instance" in cases where the bare word
  //   "Worker" is ambiguous. I considered naming the class WorkerInstance, but it feels redundant
  //   for a class name to end in "Instance". ("I have an instance of WorkerInstance...")

public:
  class Script;
  class Isolate;
  class ApiIsolate;

  class ValidationErrorReporter {
  public:
    virtual void addError(kj::String error) = 0;
    virtual void addHandler(kj::Maybe<kj::StringPtr> exportName, kj::StringPtr type) = 0;
  };

  class LockType;

  explicit Worker(kj::Own<const Script> script,
                  kj::Own<WorkerObserver> metrics,
                  kj::FunctionParam<void(
                      jsg::Lock& lock, const ApiIsolate& apiIsolate,
                      v8::Local<v8::Object> target)> compileBindings,
                  IsolateObserver::StartType startType,
                  SpanParent parentSpan, LockType lockType,
                  kj::Maybe<ValidationErrorReporter&> errorReporter = nullptr);
  // `compileBindings()` is a callback that constructs all of the bindings and adds them as
  // properties to `target`.

  ~Worker() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Worker);

  const Script& getScript() const { return *script; }
  const Isolate& getIsolate() const;

  const WorkerObserver& getMetrics() const { return *metrics; }

  class Lock;

  class AsyncLock;
  kj::Promise<AsyncLock> takeAsyncLockWithoutRequest(SpanParent parentSpan) const;
  kj::Promise<AsyncLock> takeAsyncLock(RequestObserver& request) const;
  // Places this thread into the queue of threads which are interested in locking this isolate,
  // and returns when it is this thread's turn. The thread must still obtain a `Worker::Lock`, but
  // by obtaining an `AsyncLock` first, the thread ensures that it is not fighting over the lock
  // with many other threads, and all interested threads get their fair turn.
  //
  // The version accepting a `request` metrics object accumulates lock timing data and reports the
  // data via `request`'s trace span.

  class Actor;

  kj::Promise<AsyncLock> takeAsyncLockWhenActorCacheReady(kj::Date now, Actor& actor,
      RequestObserver& request) const;
  // Like takeAsyncLock(), but also takes care of actor cache time-based eviction and backpressure.

  class WarnAboutIsolateLockScope {
    // Create on stack in scopes where any attempt to take an isolate lock should log a warning.
    // Isolate locks can block for a relatively long time, so we especially try to avoid taking
    // them while any other locks are held.
  public:
    WarnAboutIsolateLockScope();
    inline ~WarnAboutIsolateLockScope() noexcept(false) {
      if (!released) release();
    }
    KJ_DISALLOW_COPY(WarnAboutIsolateLockScope);
    inline WarnAboutIsolateLockScope(WarnAboutIsolateLockScope&& other)
        : released(other.released) {
      other.released = true;
    }

    void release();

  private:
    bool released = false;
  };

private:
  kj::Own<const Script> script;

  template <typename Observer>
  class TeardownFinishedGuard {
    // RAII object to call `teardownFinished()` on an observer for you.
  public:
    TeardownFinishedGuard(Observer& ref): ref(ref) {}
    ~TeardownFinishedGuard() noexcept(false) {
      ref.teardownFinished();
    }
    KJ_DISALLOW_COPY_AND_MOVE(TeardownFinishedGuard);

  private:
    Observer& ref;
  };

  kj::Own<WorkerObserver> metrics;
  TeardownFinishedGuard<WorkerObserver&> teardownGuard { *metrics };
  // metrics needs to be first to be destroyed last to correctly capture destruction timing.
  // it needs script to report destruction time, so it comes right after that.

  struct Impl;
  kj::Own<Impl> impl;

  class InspectorClient;
  class AsyncWaiter;

  static void handleLog(
      jsg::Lock& js, LogLevel level, const v8::FunctionCallbackInfo<v8::Value>& info);
};

class Worker::Script: public kj::AtomicRefcounted {
  // A compiled script within an Isolate, but which hasn't been instantiated into a particular
  // context (Worker).

public:
  ~Script() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Script);

  inline kj::StringPtr getId() const { return id; }
  const Isolate& getIsolate() const { return *isolate; }

  bool isModular() const;

  struct CompiledGlobal {
    jsg::V8Ref<v8::String> name;
    jsg::V8Ref<v8::Value> value;
  };

  struct ScriptSource {
    kj::StringPtr mainScript;
    // Content of the script (JavaScript). Pointer is valid only until the Script constructor
    // returns.

    kj::StringPtr mainScriptName;
    // Name of the script, used as the script origin for stack traces. Pointer is valid only until
    // the Script constructor returns.

    kj::Function<kj::Array<CompiledGlobal>(jsg::Lock& lock, const ApiIsolate& apiIsolate)>
        compileGlobals;
    // Callback which will compile the script-level globals, returning a list of them.
  };
  struct ModulesSource {
    kj::StringPtr mainModule;
    // Path to the main module, which can be looked up in thne module registry. Pointer is valid
    // only until the Script constructor returns.

    kj::Function<kj::Own<jsg::ModuleRegistry>(jsg::Lock& lock, const ApiIsolate& apiIsolate)>
        compileModules;
    // Callback which will construct the module registry and load all the modules into it.
  };
  using Source = kj::OneOf<ScriptSource, ModulesSource>;

private:
  kj::Own<const Isolate> isolate;
  kj::String id;

  struct Impl;
  kj::Own<Impl> impl;

  friend class Worker;

public:  // pretend this is private (needs to be public because allocated through template)
  explicit Script(kj::Own<const Isolate> isolate, kj::StringPtr id, Source source,
                  IsolateObserver::StartType startType, bool logNewScript,
                  kj::Maybe<ValidationErrorReporter&> errorReporter);
};

class Worker::Isolate: public kj::AtomicRefcounted {
  // Multiple zones may share the same script. We would like to compile each script only once,
  // yet still provide strong separation between zones. To that end, each Script gets a V8
  // Isolate, while each Zone sharing that script gets a JavaScript context (global object).
  //
  // Note that this means that multiple workers sharing the same script cannot execute
  // concurrently. Worker::Lock takes care of this.
  //
  // An Isolate maintains weak maps of Workers and Scripts loaded within it.
  //
  // An Isolate is persisted by strong references given to each `Worker::Script` returned from
  // `newScript()`. At various points, other strong references are made, but these are generally
  // ephemeral. So when the last script is destructed, the isolate can be expected to also be
  // destructed soon.

public:
  enum class InspectorPolicy {
    // Determines whether a devtools inspector client can be attached.
    DISALLOW,
    ALLOW_UNTRUSTED,
    ALLOW_FULLY_TRUSTED,
  };

  explicit Isolate(kj::Own<ApiIsolate> apiIsolate,
                   kj::Own<IsolateObserver>&& metrics,
                   kj::StringPtr id,
                   kj::Own<IsolateLimitEnforcer> limitEnforcer,
                   InspectorPolicy inspectorPolicy);
  // Creates an isolate with the given ID. The ID only matters for metrics-reporting purposes.
  // Usually it matches the script ID. An exception is preview isolates: there, each preview
  // session has one isolate which may load many iterations of the script (this allows the
  // inspector session to stay open across them).

  ~Isolate() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Isolate);

  const IsolateObserver& getMetrics() const { return *metrics; }

  inline kj::StringPtr getId() const { return id; }

  kj::Own<const Worker::Script> newScript(
      kj::StringPtr id, Script::Source source,
      IsolateObserver::StartType startType, bool logNewScript = false,
      kj::Maybe<ValidationErrorReporter&> errorReporter = nullptr) const;
  // Parses the given code to create a new script object and returns it.

  const IsolateLimitEnforcer& getLimitEnforcer() const { return *limitEnforcer; }
  const ApiIsolate& getApiIsolate() const { return *apiIsolate; }

  uint getCurrentLoad() const;
  // Returns the number of threads currently blocked trying to lock this isolate's mutex (using
  // takeAsyncLock()).

  uint getLockSuccessCount() const;
  // Returns a count that is incremented upon every successful lock.

  kj::Promise<void> attachInspector(
      kj::Timer& timer,
      kj::Duration timerOffset,
      kj::HttpService::Response& response,
      const kj::HttpHeaderTable& headerTable,
      kj::HttpHeaderId controlHeaderId) const;
  // Accepts a connection to the V8 inspector and handles requests until the client disconnects.

  kj::Promise<void> attachInspector(
      kj::Timer& timer,
      kj::Duration timerOffset,
      kj::WebSocket& webSocket) const;

  void logWarning(kj::StringPtr description, Worker::Lock& lock);
  void logWarningOnce(kj::StringPtr description, Worker::Lock& lock);
  // Log a warning to the inspector if attached, and log an INFO severity message. logWarningOnce()
  // only logs the warning if it has not already been logged for this worker instance.

  void logErrorOnce(kj::StringPtr description);
  // Log an ERROR severity message, if it has not already been logged for this worker instance.

  kj::Own<WorkerInterface> wrapSubrequestClient(
      kj::Own<WorkerInterface> client,
      kj::HttpHeaderId contentEncodingHeaderId,
      RequestObserver& requestMetrics) const;
  // Wrap an HttpClient to report subrequests to inspector.

  kj::Maybe<kj::StringPtr> getFeatureFlagsForFl() const {
    return featureFlagsForFl;
  }

  void completedRequest() const;
  // Called after each completed request. Does not require a lock.

  kj::Promise<AsyncLock> takeAsyncLockWithoutRequest(SpanParent parentSpan) const;
  kj::Promise<AsyncLock> takeAsyncLock(RequestObserver&) const;
  // See Worker::takeAsyncLock().

  bool isInspectorEnabled() const;

  class WeakIsolateRef: public kj::AtomicRefcounted {
    // Represents a weak reference back to the isolate that code within the isolate can use as an
    // indirect pointer when they want to be able to race destruction safely. A caller wishing to
    // use a weak reference to the isolate should acquire a strong reference to weakIsolateRef.
    // That will ensure it's always safe to invoke `tryAddStrongRef` to try to obtain a strong
    // reference of the underlying isolate. This is because the Isolate's destructor will explicitly
    // clear the underlying pointer that would be dereferenced by `tryAddStrongRef`. This means that
    // after the refcount reaches 0, `tryAddStrongRef` is always still safe to invoke even if the
    // underlying Isolate memory has been deallocated (provided ownership of the weak isolate
    // reference is retained).
    // TODO(someday): This can be templatized & even integrated into KJ. That's why the method
    // bodies are defined ineline.
  public:
    WeakIsolateRef(Isolate* thisArg) : this_(thisArg) {}

    kj::Maybe<kj::Own<const Worker::Isolate>> tryAddStrongRef() const {
      // This tries to materialize a strong reference to the isolate. It will fail if the isolate's
      // refcount has already dropped to 0. As discussed in the class, the lifetime of this weak
      // reference can exceed the lifetime of the isolate it's tracking.
      auto lock = this_.lockShared();
      if (*lock == nullptr) {
        return nullptr;
      }

      return kj::atomicAddRefWeak(**lock);
    }

    void invalidate() const {
      // This is invoked by the Isolate destructor to clear the pointer. That means that any racing
      // code will never try to invoke `atomicAddRefWeak` on the instance any more. Any code racing
      // in between the refcount dropping to 0 and the invalidation getting invoked will still fail
      // to acquire a strong reference. Any code acquiring a strong reference prior to the refcount
      // dropping to 0 will prevent invalidation until that extra reference is dropped.
      *this_.lockExclusive() = nullptr;
    }

  private:
    kj::MutexGuarded<const Isolate*> this_;
  };

  kj::Own<const WeakIsolateRef> getWeakRef() const {
    return kj::atomicAddRef(*weakIsolateRef);
  }

private:
  kj::Promise<AsyncLock> takeAsyncLockImpl(
      kj::Maybe<kj::Own<IsolateObserver::LockTiming>> lockTiming) const;

  kj::String id;
  kj::Own<IsolateLimitEnforcer> limitEnforcer;
  kj::Own<ApiIsolate> apiIsolate;

  kj::Maybe<kj::String> featureFlagsForFl;
  // If non-null, a serialized JSON object with a single "flags" property, which is a list of
  // compatibility enable-flags that are relevant to FL.

  kj::Own<IsolateObserver> metrics;
  Worker::TeardownFinishedGuard<IsolateObserver&> teardownGuard { *metrics };

  struct Impl;
  kj::Own<Impl> impl;

  kj::Own<const WeakIsolateRef> weakIsolateRef;
  // This is a weak reference that can be used to safely (in a multi-threaded context) try to
  // acquire a strong reference to the isolate. To do that add a strong reference to the
  // weakIsolateRef while it's safe and then call tryAddStrongRef which will return a strong
  // reference if the object isn't being destroyed (it's safe to call this even if the destructor
  // has already run).

  class InspectorChannelImpl;
  kj::Maybe<InspectorChannelImpl&> currentInspectorSession;

  struct AsyncWaiterList {
    kj::Maybe<AsyncWaiter&> head = nullptr;
    kj::Maybe<AsyncWaiter&>* tail = &head;

    ~AsyncWaiterList() noexcept;
  };
  kj::MutexGuarded<AsyncWaiterList> asyncWaiters;
  // Mutex-guarded linked list of threads waiting for an async lock on this worker. The lock
  // protects the `AsyncWaiterList` as well as the next/prev pointers in each `AsyncWaiter` that
  // is currently in the list.
  //
  // TODO(perf): Use a lock-free list? Tricky to get right. `asyncWaiters` should only be locked
  //   briefly so there's probably not that much to gain.

  friend class Worker::AsyncLock;

  void disconnectInspector();

  void logMessage(v8::Local<v8::Context> context, uint16_t type, kj::StringPtr description);
  // Log a message as if with console.{log,warn,error,etc}. `type` must be one of the cdp::LogType
  // enum, which unfortunately we cannot forward-declare, ugh.

  class SubrequestClient final: public WorkerInterface {
  public:
    explicit SubrequestClient(kj::Own<const Isolate> isolate,
        kj::Own<WorkerInterface> inner, kj::HttpHeaderId contentEncodingHeaderId,
        RequestObserver& requestMetrics)
        : constIsolate(kj::mv(isolate)), inner(kj::mv(inner)),
          contentEncodingHeaderId(contentEncodingHeaderId),
          requestMetrics(kj::addRef(requestMetrics)) {}
    KJ_DISALLOW_COPY_AND_MOVE(SubrequestClient);
    kj::Promise<void> request(
        kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
        kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) override;
    kj::Promise<void> connect(
        kj::StringPtr host, const kj::HttpHeaders& headers, kj::AsyncIoStream& connection,
        kj::HttpService::ConnectResponse& tunnel,
        kj::HttpConnectSettings settings) override;
    void prewarm(kj::StringPtr url) override;
    kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override;
    kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime) override;
    kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override;

  private:
    kj::Own<const Isolate> constIsolate;
    kj::Own<WorkerInterface> inner;
    kj::HttpHeaderId contentEncodingHeaderId;
    kj::Own<RequestObserver> requestMetrics;
  };

  class ResponseStreamWrapper;
  class LimitedBodyWrapper;

  size_t nextRequestId = 0;
  kj::Own<jsg::AsyncContextFrame::StorageKey> traceAsyncContextKey;

  friend class Worker;
  friend class IsolateChannelImpl;
};

class Worker::ApiIsolate {
  // The "API isolate" is a wrapper around JSG which determines which APIs are available. This is
  // an abstract interface which can be customized to make the runtime support a different set of
  // APIs. All JSG wrapping/unwrapping is encapsulated within this.
  //
  // In contrast, the rest of the classes in `worker.h` are concerned more with lifecycle
  // management.
  //
  // TODO(cleanup): Find a better name for this, we have too many things called "isolate".
public:
  static const ApiIsolate& current();
  // Get the current `ApiIsolate` or throw if we're not currently executing JavaScript.
  //
  // TODO(cleanup): This is a hack thrown in quickly because IoContext::curent() dosen't work in
  //   the global scope (when no request is running). We need a better design here.

  virtual kj::Own<jsg::Lock> lock(jsg::V8StackScope& stackScope) const = 0;
  // Take a lock on the isolate.
  //
  // TODO(cleanup): Change all locking to a synchrenous callback style rather than RAII style, so
  //   that this doesn't have to allocate and so it's not possible to hold a lock while returning
  //   to the event loop.

  virtual CompatibilityFlags::Reader getFeatureFlags() const = 0;
  // Get the FeatureFlags this isolate is configured with. Returns a Reader that is owned by the
  // ApiIsolate.

  virtual jsg::JsContext<api::ServiceWorkerGlobalScope> newContext(jsg::Lock& lock) const = 0;
  // Create the context (global scope) object.

  virtual jsg::Dict<NamedExport> unwrapExports(
      jsg::Lock& lock, v8::Local<v8::Value> moduleNamespace) const = 0;
  // Given a module's export namespace, return all the top-level exports.

  struct ErrorInterface {
    // Convenience struct for accessing typical Error properties.
    jsg::Optional<kj::String> name;
    jsg::Optional<kj::String> message;
    JSG_STRUCT(name, message);
  };
  virtual const jsg::TypeHandler<ErrorInterface>&
      getErrorInterfaceTypeHandler(jsg::Lock& lock) const = 0;
  virtual const jsg::TypeHandler<api::QueueExportedHandler>& getQueueTypeHandler(
      jsg::Lock& lock) const = 0;

  virtual kj::Maybe<const api::CryptoAlgorithm&> getCryptoAlgorithm(kj::StringPtr name) const {
    // Look up crypto algorithms by case-insensitive name. This can be used to extend the set of
    // WebCrytpo algorithms supported.
    return nullptr;
  }
};

enum class UncaughtExceptionSource {
  INTERNAL,
  INTERNAL_ASYNC,
  // We catch, log, and rethrow some exceptions at these intermediate levels, in case higher-level
  // handlers fail.

  ASYNC_TASK,
  REQUEST_HANDLER,
  TRACE_HANDLER,
  ALARM_HANDLER,
};
kj::StringPtr KJ_STRINGIFY(UncaughtExceptionSource value);

class Worker::Lock {
  // A Worker may bounce between threads as it handles multiple requests, but can only actually
  // execute on one thread at a time. Each thread must therefore lock the Worker while executing
  // code.
  //
  // A Worker::Lock MUST be allocated on the stack.

public:
  class TakeSynchronously {
    // Worker locks should normally be taken asynchronously. The TakeSynchronously type can be used
    // when a synchronous lock is unavoidable. The purpose of this type is to make it easy to find
    // all the places where we take synchronous locks.

  public:
    explicit TakeSynchronously(kj::Maybe<RequestObserver&> request);
    // We don't provide a default constructor so that call sites need to think about whether they
    // have a Request& available to pass in.

    kj::Maybe<RequestObserver&> getRequest();

  private:
    RequestObserver* request = nullptr;
    // Non-null if this lock is being taken on behalf of a request.
    // HACK: The OneOf<TakeSynchronously, ...> in Worker::LockType doesn't like that
    //   Maybe<RequestObserver&> forces us to have a mutable copy constructor. I couldn't figure
    //   out how to work around it, so here we are with a raw pointer. :/
  };

  explicit Lock(const Worker& worker, LockType lockType);
  KJ_DISALLOW_COPY_AND_MOVE(Lock);
  ~Lock() noexcept(false);

  void requireNoPermanentException();

  Worker& getWorker() { return worker; }

  operator jsg::Lock&();

  v8::Isolate* getIsolate();
  v8::Local<v8::Context> getContext();

  bool isInspectorEnabled();
  void logWarning(kj::StringPtr description);
  void logWarningOnce(kj::StringPtr description);

  void logErrorOnce(kj::StringPtr description);

  void logUncaughtException(kj::StringPtr description);
  void logUncaughtException(UncaughtExceptionSource source, v8::Local<v8::Value> exception,
                            v8::Local<v8::Message> message = {});
  // Logs an exception to the debug console or trace, if active.

  void reportPromiseRejectEvent(v8::PromiseRejectMessage& message);

  void validateHandlers(ValidationErrorReporter& errorReporter);
  // Checks for problems with the registered event handlers (such as that there are none) and
  // reports them to the error reporter.

  kj::Maybe<api::ExportedHandler&> getExportedHandler(
      kj::Maybe<kj::StringPtr> entrypointName, kj::Maybe<Worker::Actor&> actor);
  // Get the ExportedHandler exported under the given name. `entrypointName` may be null to get the
  // default handler. Returns null if this is not a modules-syntax worker (but `entrypointName`
  // must be null in that case).
  //
  // If running in an actor, the name is ignored and the entrypoint originally used to construct
  // the actor is returned.

  api::ServiceWorkerGlobalScope& getGlobalScope();
  // Get the C++ object representing the global scope.

  jsg::AsyncContextFrame::StorageKey& getTraceAsyncContextKey();
  // Get the opaque storage key to use for recording trace information in async contexts.

private:
  struct Impl;

  jsg::V8StackScope stackScope;
  Worker& worker;
  kj::Own<Impl> impl;

  friend class Worker;
};

class Worker::LockType {
  // Can be initialized either from an `AsyncLock` or a `TakeSynchronously`, to indicate whether an
  // async lock is held and help us grep for places in the code that do not support async locks.
public:
  LockType(Lock::TakeSynchronously origin): origin(origin) {}
  LockType(AsyncLock& origin): origin(&origin) {}

private:
  kj::OneOf<Lock::TakeSynchronously, AsyncLock*> origin;
  friend class Worker::Isolate;
};

class Worker::AsyncLock {
  // Represents the thread's ownership of an isolate's asynchronous lock. Call `takeAsyncLock()`
  // on a `Worker` or `Worker::Isolate` to obtain this. Pass it to the constructor of
  // `Worker::Lock` (as the `lockType`) in order to indicate that the calling thread has taken
  // the async lock first.
  //
  // You must never store an `AsyncLock` long-term. Use it in a continuation and then discard it.
  // To put it another way: An `AsyncLock` instance must never outlive an `evalLast()`.
public:
  // No public methods. The only thing you can do is pass this for the `LockType` parameter of
  // `Worker::Lock`'s constructor.

  static kj::Promise<void> whenThreadIdle();
  // Waits until the thread has no async locks, is not waiting on any locks, and has finished all
  // pending events (a la `kj::evalLast()`).

private:
  kj::Own<AsyncWaiter> waiter;
  kj::Maybe<kj::Own<IsolateObserver::LockTiming>> lockTiming;

  AsyncLock(kj::Own<AsyncWaiter> waiter,
            kj::Maybe<kj::Own<IsolateObserver::LockTiming>> lockTiming)
      : waiter(kj::mv(waiter)), lockTiming(kj::mv(lockTiming)) {}

  friend class Worker::Isolate;
  friend class Worker::AsyncWaiter;
};

class Worker::Actor final: public kj::Refcounted {
  // Represents actor state within a Worker instance. This object tracks the JavaScript heap
  // objects backing `event.actorState`. Multiple `Actor`s can be created within a single `Worker`.

public:
  using MakeActorCacheFunc = kj::Function<kj::Maybe<kj::Own<ActorCacheInterface>>(
      const ActorCache::SharedLru& sharedLru, OutputGate& outputGate, ActorCache::Hooks& hooks)>;
  // Callback which constructs the `ActorCacheInterface` instance (if any) for the Actor. This
  // can be used to customize the storage implementation. This will be called synchronously in
  // the constructor.

  using MakeStorageFunc = kj::Function<jsg::Ref<api::DurableObjectStorage>(
      jsg::Lock& js, const ApiIsolate& apiIsolate, ActorCacheInterface& actorCache)>;
  // Callback which constructs the `DurableObjectStorage` instance for an actor. This can be used
  // to customize the JavaScript API.
  //
  // TODO(cleanup): Can we refactor the (internal-codebase) user of this so that it doesn't need
  //   to customize the JS API but only the underlying ActorCacheInterface?

  using Id = kj::OneOf<kj::Own<ActorIdFactory::ActorId>, kj::String>;

  class Loopback {
    // Class that allows sending requests to this actor, recreating it as needed. It is safe to hold
    // onto this for longer than a Worker::Actor is alive.
  public:
    virtual kj::Own<WorkerInterface> getWorker(IoChannelFactory::SubrequestMetadata metadata) = 0;
    // Send a request to this actor, potentially re-creating it if it is not currently active.
    // The returned kj::Own<WorkerInterface> may be held longer than Loopback, and is assumed
    // to keep the Worker::Actor alive as well.

    virtual kj::Own<Loopback> addRef() = 0;
  };

  class HibernationManager : public kj::Refcounted {
    // The HibernationManager class manages HibernatableWebSockets created by an actor.
    // The manager handles accepting new websockets, retreiving existing websockets by tag, and
    // removing websockets from its collection when they disconnect.
  public:
    virtual void acceptWebSocket(jsg::Ref<api::WebSocket> ws, kj::ArrayPtr<kj::String> tags) = 0;
    virtual kj::Vector<jsg::Ref<api::WebSocket>> getWebSockets(
        jsg::Lock& js,
        kj::Maybe<kj::StringPtr> tag) = 0;
    virtual void hibernateWebSockets(Worker::Lock& lock) = 0;
  };

  Actor(const Worker& worker, kj::Maybe<RequestTracker&> tracker, Id actorId,
        bool hasTransient, MakeActorCacheFunc makeActorCache,
        kj::Maybe<kj::StringPtr> className, MakeStorageFunc makeStorage, Worker::Lock& lock,
        kj::Own<Loopback> loopback, TimerChannel& timerChannel, kj::Own<ActorObserver> metrics,
        kj::Maybe<kj::Own<HibernationManager>> manager, kj::Maybe<uint16_t> hibernationEventType);
  // Create a new Actor hosted by this Worker. Note that this Actor object may only be manipulated
  // from the thread that created it.

  ~Actor() noexcept(false);

  void ensureConstructed(IoContext&);
  // Call when starting any new request, to ensure that the actor object's constructor has run.
  //
  // This is used only for modules-syntax actors (which most are, since that's the only format we
  // support publicly).

  void shutdown(uint16_t reasonCode, kj::Maybe<const kj::Exception&> error = nullptr);
  // Forces cancellation of all "background work" this actor is executing, i.e. work that is not
  // happening on behalf of an active request. Note that this is not a part of the dtor because
  // IoContext objects prolong the lifetime of their Actor.
  //
  // `reasonCode` is passed back to the WorkerObserver.

  void shutdownActorCache(kj::Maybe<const kj::Exception&> error);
  // Stops new work on behalf of the ActorCache. This does not cancel any ongoing flushes.
  // TODO(soon) This should probably be folded into shutdown(). We'd need a piece that converts
  // `error` to `reasonCode` in workerd to do this. There may also be opportunities to streamline
  // interactions between `onAbort` and `onShutdown` promises.

  kj::Promise<void> onShutdown();
  // Get a promise that resolves when `shutdown()` has been called.

  kj::Promise<void> onBroken();
  // Get a promise that rejects when this actor becomes broken in some way. See doc comments for
  // WorkerRuntime.makeActor() in worker.capnp for a discussion of actor brokenness.
  // Note that this doesn't cover every cause of actor brokenness -- some of them are fulfilled
  // in worker-set or process-sandbox code, in particular code updates and exceeded memory.
  // This method can only be called once.

  const Id& getId();
  Id cloneId();
  static Id cloneId(Id& id);
  kj::Maybe<jsg::Value> getTransient(Worker::Lock& lock);
  kj::Maybe<ActorCacheInterface&> getPersistent();
  kj::Own<Loopback> getLoopback();

  kj::Maybe<jsg::Ref<api::DurableObjectStorage>> makeStorageForSwSyntax(Worker::Lock& lock);
  // Make the storage object for use in Service Workers syntax. This should not be used for
  // modules-syntax worokers. (Note that Service-Workers-syntax actors are not supported publicly.)

  ActorObserver& getMetrics();

  InputGate& getInputGate();
  OutputGate& getOutputGate();

  kj::Maybe<IoContext&> getIoContext();
  // Get the IoContext which should be used for all activity in this Actor. Returns null if
  // setIoContext() hasn't been called yet.

  void setIoContext(kj::Own<IoContext> context);
  // Set the IoContext for this actor. This is called once, when starting the first request
  // to the actor.
  //
  // TODO(cleanup): Could we make it so the Worker::Actor can create the IoContext directly,
  //   rather than have WorkerEntrypoint create it on the first request? We'd have to plumb through
  //   some more information to the place where `Actor` is created, which might be uglier than it's
  //   worth.

  kj::Maybe<HibernationManager&> getHibernationManager();
  // Get the HibernationManager which should be used for all activity in this Actor. Returns null if
  // setHibernationManager() hasn't been called yet.

  void setHibernationManager(kj::Own<HibernationManager> manager);
  // Set the HibernationManager for this actor. This is called once, on the first call to
  // `acceptWebSocket`.

  kj::Maybe<uint16_t> getHibernationEventType();
  // Gets the event type ID of the hibernation event, which is defined outside of workerd.
  // Only needs to be called when allocating a HibernationManager!

  const Worker& getWorker() { return *worker; }

  void assertCanSetAlarm();
  kj::Promise<void> makeAlarmTaskForPreview(kj::Date scheduledTime);

  using AlarmResult = WorkerInterface::AlarmResult;
  kj::Maybe<kj::Promise<AlarmResult>> getAlarm(kj::Date scheduledTime);
  // If there is a scheduled or running alarm with the given `scheduledTime`, return a promise to
  // its result. This allows use to de-dupe multiple requests to a single `IoContext::run()`.

  class AlarmFulfiller {
  public:
    AlarmFulfiller(kj::Own<kj::PromiseFulfiller<AlarmResult>> fulfiller)
      : maybeFulfiller(kj::mv(fulfiller)) {}
    KJ_DISALLOW_COPY(AlarmFulfiller);
    AlarmFulfiller(AlarmFulfiller&&) = default;
    AlarmFulfiller& operator=(AlarmFulfiller&&) = default;
    ~AlarmFulfiller() noexcept(false) {
      KJ_IF_MAYBE(fulfiller, getFulfiller()) {
        fulfiller->reject(KJ_EXCEPTION(FAILED, "AlarmFulfiller destroyed without resolution"));
      }
    }
    void fulfill(const AlarmResult& result) {
      KJ_IF_MAYBE(fulfiller, getFulfiller()) {
        fulfiller->fulfill(kj::cp(result));
      }
    }
    void reject(const kj::Exception& e) {
      KJ_IF_MAYBE(fulfiller, getFulfiller()) {
        fulfiller->reject(kj::cp(e));
      }
    }
    void cancel() {
      KJ_IF_MAYBE(fulfiller, getFulfiller()) {
        fulfiller->fulfill(AlarmResult{
          .retry = false,
          .outcome = EventOutcome::CANCELED,
        });
      }
    }

  private:
    kj::Maybe<kj::Own<kj::PromiseFulfiller<AlarmResult>>> maybeFulfiller;
    kj::Maybe<kj::PromiseFulfiller<AlarmResult>&> getFulfiller() {
      KJ_IF_MAYBE(fulfiller, maybeFulfiller) {
        if (fulfiller->get()->isWaiting()) {
          return **fulfiller;
        }
      }

      return nullptr;
    }
  };
  using ScheduleAlarmResult = kj::OneOf<AlarmResult, AlarmFulfiller>;
  kj::Promise<ScheduleAlarmResult> scheduleAlarm(kj::Date scheduledTime);
  // Wait for `Date.now()` to be greater than or equal to `scheduledTime`. If the promise resolves
  // to an `AlarmFulfiller`, then the caller is responsible for invoking `fulfill()`, `reject()`, or
  // `cancel()`. Otherwise, the scheduled alarm was overriden by another call to `scheduleAlarm()`
  // and thus was cancelled. Note that callers likely want to invoke `getAlarm()` first to see if
  // there is an existing alarm at `scheduledTime` for which they want to wait (instead of
  // cancelling it).

  kj::Own<Worker::Actor> addRef() {
    KJ_IF_MAYBE(t, tracker) {
      return kj::addRef(*this).attach(t->get()->startRequest());
    } else {
      return kj::addRef(*this);
    }
  }
private:
  kj::Own<const Worker> worker;
  kj::Maybe<kj::Own<RequestTracker>> tracker;
  struct Impl;
  kj::Own<Impl> impl;

  kj::Maybe<api::ExportedHandler&> getHandler();
  friend class Worker;
};

// =======================================================================================
// inline implementation details

inline const Worker::Isolate& Worker::getIsolate() const { return *script->isolate; }

class Worker::AsyncWaiter: public kj::Refcounted {
  // Represents a thread's attempt to take an async lock. Each Isolate has a linked list of
  // `AsyncWaiter`s. A particular thread only ever owns one `AsyncWaiter` at a time.

public:
  AsyncWaiter(kj::Own<const Isolate> isolate);
  ~AsyncWaiter() noexcept;
  KJ_DISALLOW_COPY_AND_MOVE(AsyncWaiter);

private:
  const kj::Executor& executor;
  // Executor for this waiter's thread.

  kj::Own<const Isolate> isolate;
  // The isolate for which this waiter is currently waiting.

  kj::ForkedPromise<void> readyPromise = nullptr;
  kj::Own<kj::CrossThreadPromiseFulfiller<void>> readyFulfiller;
  // Promise/fulfiller to fire when the waiter reaches the front of the list for the corresponding
  // isolate.

  kj::ForkedPromise<void> releasePromise = nullptr;
  kj::Own<kj::PromiseFulfiller<void>> releaseFulfiller;
  // Promise/fulfiller to fire when the AsyncLock is finally released. This is used when a thread
  // tries to take locks on multiple different isolates concurrently, in order to serialize the
  // locks so only one is taken at a time. This is NOT a cross-thread fulfiller; it can only be
  // fulfilled by the thread that owns the waiter.

  kj::Maybe<AsyncWaiter&> next;
  kj::Maybe<AsyncWaiter&>* prev;
  // Protected by the lock on `Isolate::asyncWaiters` for the isolate identified by
  // `currentIsolate`. Must be null if `currentIsolate` is null. (All other members of `Waiter`
  // can only be accessed by the thread that created the `Waiter`.)

  static thread_local AsyncWaiter* threadCurrentWaiter;

  friend class Worker::Isolate;
  friend class Worker::AsyncLock;
};

} // namespace workerd
