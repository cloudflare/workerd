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
#include <workerd/util/weak-refs.h>
#include <workerd/util/thread-scopes.h>

namespace v8 { class Isolate; }

namespace workerd {

namespace jsg {
  // TODO(cleanup): While this isn't used directly in worker.h or worker.c++, it is
  // used transitively by internal repo source files. Those will need to be updated
  // to import or define jsg::V8System where appropriate before we can remove this
  // declaration.
  class V8System;
}

namespace api {
  class DurableObjectState;
  class DurableObjectStorage;
  class ServiceWorkerGlobalScope;
  struct ExportedHandler;
  struct CryptoAlgorithm;
  struct QueueExportedHandler;
  class Socket;
  class WebSocket;
  class WebSocketRequestResponsePair;
}

class ThreadContext;
class IoContext;
class InputGate;
class OutputGate;

// Type signature of a durable object implementation class.
typedef jsg::Constructor<api::ExportedHandler(
      jsg::Ref<api::DurableObjectState> durableObject, jsg::Value env)>
    DurableObjectConstructor;

// The type of a top-level export -- either a simple handler or a durable object class.
typedef kj::OneOf<DurableObjectConstructor, api::ExportedHandler> NamedExport;

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
class Worker: public kj::AtomicRefcounted {
public:
  class Script;
  class Isolate;
  class Api;
  using ApiIsolate [[deprecated("Use Workerd::Api")]] = Api;

  class ValidationErrorReporter {
  public:
    virtual void addError(kj::String error) = 0;
    virtual void addHandler(kj::Maybe<kj::StringPtr> exportName, kj::StringPtr type) = 0;
  };

  class LockType;

  enum class ConsoleMode {
    // Only send `console.log`s to the inspector. Default, production behaviour.
    INSPECTOR_ONLY,
    // Send `console.log`s to the inspector and stdout/err. Behaviour running `workerd` locally.
    STDOUT,
  };

  explicit Worker(kj::Own<const Script> script,
                  kj::Own<WorkerObserver> metrics,
                  kj::FunctionParam<void(
                      jsg::Lock& lock, const Api& api,
                      v8::Local<v8::Object> target)> compileBindings,
                  IsolateObserver::StartType startType,
                  SpanParent parentSpan, LockType lockType,
                  kj::Maybe<ValidationErrorReporter&> errorReporter = kj::none);
  // `compileBindings()` is a callback that constructs all of the bindings and adds them as
  // properties to `target`.

  ~Worker() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Worker);

  const Script& getScript() const { return *script; }
  const Isolate& getIsolate() const;

  const WorkerObserver& getMetrics() const { return *metrics; }

  class Lock;

  class AsyncLock;

  // Places this thread into the queue of threads which are interested in locking this isolate,
  // and returns when it is this thread's turn. The thread must still obtain a `Worker::Lock`, but
  // by obtaining an `AsyncLock` first, the thread ensures that it is not fighting over the lock
  // with many other threads, and all interested threads get their fair turn.
  kj::Promise<AsyncLock> takeAsyncLockWithoutRequest(SpanParent parentSpan) const;

  // Places this thread into the queue of threads which are interested in locking this isolate,
  // and returns when it is this thread's turn. The thread must still obtain a `Worker::Lock`, but
  // by obtaining an `AsyncLock` first, the thread ensures that it is not fighting over the lock
  // with many other threads, and all interested threads get their fair turn.
  //
  // The version accepting a `request` metrics object accumulates lock timing data and reports the
  // data via `request`'s trace span.
  kj::Promise<AsyncLock> takeAsyncLock(RequestObserver& request) const;

  class Actor;

  // Like takeAsyncLock(), but also takes care of actor cache time-based eviction and backpressure.
  kj::Promise<AsyncLock> takeAsyncLockWhenActorCacheReady(kj::Date now, Actor& actor,
      RequestObserver& request) const;

  // Track a set of address->callback overrides for which the connect(address) behavior should be
  // overridden via callbacks rather than using the default Socket connect() logic.
  // This is useful for allowing generic client libraries to connect to private local services using
  // just a provided address (rather than requiring them to support being passed a binding to call
  // binding.connect() on).
  typedef kj::Function<jsg::Ref<api::Socket>(jsg::Lock&)> ConnectFn;
  void setConnectOverride(kj::String networkAddress, ConnectFn connectFn);
  kj::Maybe<ConnectFn&> getConnectOverride(kj::StringPtr networkAddress);

  // TODO(cleanup): This is currently used in exactly one place in the internal
  // repo. Once that is updated to use workerd::WarnAboutIsolateLockScope directly
  // this alias can be removed.
  using WarnAboutIsolateLockScope = workerd::WarnAboutIsolateLockScope;

private:
  kj::Own<const Script> script;

  kj::Own<WorkerObserver> metrics;

  // metrics needs to be first to be destroyed last to correctly capture destruction timing.
  // it needs script to report destruction time, so it comes right after that.
  TeardownFinishedGuard<WorkerObserver&> teardownGuard { *metrics };

  struct Impl;
  kj::Own<Impl> impl;

  kj::HashMap<kj::String, ConnectFn> connectOverrides;

  class InspectorClient;
  class AsyncWaiter;
  friend constexpr bool _kj_internal_isPolymorphic(AsyncWaiter*);

  static void handleLog(jsg::Lock& js, ConsoleMode mode, LogLevel level,
                          const v8::Global<v8::Function>& original,
                          const v8::FunctionCallbackInfo<v8::Value>& info);
};

// A compiled script within an Isolate, but which hasn't been instantiated into a particular
// context (Worker).
class Worker::Script: public kj::AtomicRefcounted {
public:
  ~Script() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Script);

  inline kj::StringPtr getId() const { return id; }
  const Isolate& getIsolate() const { return *isolate; }

  bool isModular() const { return modular; }

  struct CompiledGlobal {
    jsg::V8Ref<v8::String> name;
    jsg::V8Ref<v8::Value> value;
  };

  struct ScriptSource {
    // Content of the script (JavaScript). Pointer is valid only until the Script constructor
    // returns.
    kj::StringPtr mainScript;

    // Name of the script, used as the script origin for stack traces. Pointer is valid only until
    // the Script constructor returns.
    kj::StringPtr mainScriptName;

    // Callback which will compile the script-level globals, returning a list of them.
    kj::Function<kj::Array<CompiledGlobal>(jsg::Lock& lock, const Api& api, const jsg::CompilationObserver& observer)>
        compileGlobals;
  };
  struct ModulesSource {
    // Path to the main module, which can be looked up in the module registry. Pointer is valid
    // only until the Script constructor returns.
    kj::StringPtr mainModule;

    // Callback which will construct the module registry and load all the modules into it.
    kj::Function<void(jsg::Lock& lock, const Api& api)> compileModules;
  };
  using Source = kj::OneOf<ScriptSource, ModulesSource>;

private:
  kj::Own<const Isolate> isolate;
  kj::String id;
  bool modular;

  struct Impl;
  kj::Own<Impl> impl;

  friend class Worker;

public:  // pretend this is private (needs to be public because allocated through template)
  explicit Script(kj::Own<const Isolate> isolate, kj::StringPtr id, Source source,
                  IsolateObserver::StartType startType, bool logNewScript,
                  kj::Maybe<ValidationErrorReporter&> errorReporter);
};

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
class Worker::Isolate: public kj::AtomicRefcounted {
public:
  // Determines whether a devtools inspector client can be attached.
  enum class InspectorPolicy {
    DISALLOW,
    ALLOW_UNTRUSTED,
    ALLOW_FULLY_TRUSTED,
  };

  // Creates an isolate with the given ID. The ID only matters for metrics-reporting purposes.
  // Usually it matches the script ID. An exception is preview isolates: there, each preview
  // session has one isolate which may load many iterations of the script (this allows the
  // inspector session to stay open across them).
  explicit Isolate(kj::Own<Api> api,
                   kj::Own<IsolateObserver>&& metrics,
                   kj::StringPtr id,
                   kj::Own<IsolateLimitEnforcer> limitEnforcer,
                   InspectorPolicy inspectorPolicy,
                   ConsoleMode consoleMode = ConsoleMode::INSPECTOR_ONLY);

  ~Isolate() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Isolate);

  const IsolateObserver& getMetrics() const { return *metrics; }

  inline kj::StringPtr getId() const { return id; }

  // Parses the given code to create a new script object and returns it.
  kj::Own<const Worker::Script> newScript(
      kj::StringPtr id, Script::Source source,
      IsolateObserver::StartType startType, bool logNewScript = false,
      kj::Maybe<ValidationErrorReporter&> errorReporter = kj::none) const;

  const IsolateLimitEnforcer& getLimitEnforcer() const { return *limitEnforcer; }

  const Api& getApi() const { return *api; }
  [[deprecated("use getApi()")]] const Api& getApiIsolate() const { return *api; }

  // Returns the number of threads currently blocked trying to lock this isolate's mutex (using
  // takeAsyncLock()).
  uint getCurrentLoad() const;

  // Returns a count that is incremented upon every successful lock.
  uint getLockSuccessCount() const;

  // Accepts a connection to the V8 inspector and handles requests until the client disconnects.
  kj::Promise<void> attachInspector(
      kj::Timer& timer,
      kj::Duration timerOffset,
      kj::HttpService::Response& response,
      const kj::HttpHeaderTable& headerTable,
      kj::HttpHeaderId controlHeaderId) const;

  kj::Promise<void> attachInspector(
      kj::Timer& timer,
      kj::Duration timerOffset,
      kj::WebSocket& webSocket) const;

  // Log a warning to the inspector if attached, and log an INFO severity message.
  void logWarning(kj::StringPtr description, Worker::Lock& lock);

  // logWarningOnce() only logs the warning if it has not already been logged for this
  // worker instance.
  void logWarningOnce(kj::StringPtr description, Worker::Lock& lock);

  // Log an ERROR severity message, if it has not already been logged for this worker instance.
  void logErrorOnce(kj::StringPtr description);

  // Wrap an HttpClient to report subrequests to inspector.
  kj::Own<WorkerInterface> wrapSubrequestClient(
      kj::Own<WorkerInterface> client,
      kj::HttpHeaderId contentEncodingHeaderId,
      RequestObserver& requestMetrics) const;

  kj::Maybe<kj::StringPtr> getFeatureFlagsForFl() const {
    return featureFlagsForFl;
  }

  // Called after each completed request. Does not require a lock.
  void completedRequest() const;

  // See Worker::takeAsyncLock().
  kj::Promise<AsyncLock> takeAsyncLockWithoutRequest(SpanParent parentSpan) const;

  // See Worker::takeAsyncLock().
  kj::Promise<AsyncLock> takeAsyncLock(RequestObserver&) const;

  bool isInspectorEnabled() const;

  // Represents a weak reference back to the isolate that code within the isolate can use as an
  // indirect pointer when they want to be able to race destruction safely. A caller wishing to
  // use a weak reference to the isolate should acquire a strong reference to weakIsolateRef.
  // That will ensure it's always safe to invoke `tryAddStrongRef` to try to obtain a strong
  // reference of the underlying isolate. This is because the Isolate's destructor will explicitly
  // clear the underlying pointer that would be dereferenced by `tryAddStrongRef`. This means that
  // after the refcount reaches 0, `tryAddStrongRef` is always still safe to invoke even if the
  // underlying Isolate memory has been deallocated (provided ownership of the weak isolate
  // reference is retained).
  using WeakIsolateRef = AtomicWeakRef<Isolate>;

  kj::Own<const WeakIsolateRef> getWeakRef() const;

private:
  kj::Promise<AsyncLock> takeAsyncLockImpl(
      kj::Maybe<kj::Own<IsolateObserver::LockTiming>> lockTiming) const;

  kj::String id;
  kj::Own<IsolateLimitEnforcer> limitEnforcer;
  kj::Own<Api> api;
  ConsoleMode consoleMode;

  // If non-null, a serialized JSON object with a single "flags" property, which is a list of
  // compatibility enable-flags that are relevant to FL.
  kj::Maybe<kj::String> featureFlagsForFl;

  kj::Own<IsolateObserver> metrics;
  TeardownFinishedGuard<IsolateObserver&> teardownGuard { *metrics };

  struct Impl;
  kj::Own<Impl> impl;

  // This is a weak reference that can be used to safely (in a multi-threaded context) try to
  // acquire a strong reference to the isolate. To do that add a strong reference to the
  // weakIsolateRef while it's safe and then call tryAddStrongRef which will return a strong
  // reference if the object isn't being destroyed (it's safe to call this even if the destructor
  // has already run).
  kj::Own<const WeakIsolateRef> weakIsolateRef;

  class InspectorChannelImpl;
  kj::Maybe<InspectorChannelImpl&> currentInspectorSession;

  struct AsyncWaiterList {
    kj::Maybe<AsyncWaiter&> head = kj::none;
    kj::Maybe<AsyncWaiter&>* tail = &head;

    ~AsyncWaiterList() noexcept;
  };

  // Mutex-guarded linked list of threads waiting for an async lock on this worker. The lock
  // protects the `AsyncWaiterList` as well as the next/prev pointers in each `AsyncWaiter` that
  // is currently in the list.
  kj::MutexGuarded<AsyncWaiterList> asyncWaiters;
  // TODO(perf): Use a lock-free list? Tricky to get right. `asyncWaiters` should only be locked
  //   briefly so there's probably not that much to gain.

  friend class Worker::AsyncLock;

  void disconnectInspector();

  // Log a message as if with console.{log,warn,error,etc}. `type` must be one of the cdp::LogType
  // enum, which unfortunately we cannot forward-declare, ugh.
  void logMessage(jsg::Lock& js, uint16_t type, kj::StringPtr description);

  class SubrequestClient;
  class ResponseStreamWrapper;
  class LimitedBodyWrapper;

  size_t nextRequestId = 0;
  kj::Own<jsg::AsyncContextFrame::StorageKey> traceAsyncContextKey;

  friend class Worker;
};

// The "API isolate" is a wrapper around JSG which determines which APIs are available. This is
// an abstract interface which can be customized to make the runtime support a different set of
// APIs. All JSG wrapping/unwrapping is encapsulated within this.
//
// In contrast, the rest of the classes in `worker.h` are concerned more with lifecycle
// management.
class Worker::Api {
public:
  // Get the current `Api` or throw if we're not currently executing JavaScript.
  static const Api& current();
  // TODO(cleanup): This is a hack thrown in quickly because IoContext::current() doesn't work in
  //   the global scope (when no request is running). We need a better design here.

  // Take a lock on the isolate.
  virtual kj::Own<jsg::Lock> lock(jsg::V8StackScope& stackScope) const = 0;
  // TODO(cleanup): Change all locking to a synchronous callback style rather than RAII style, so
  //   that this doesn't have to allocate and so it's not possible to hold a lock while returning
  //   to the event loop.

  // Get the FeatureFlags this isolate is configured with. Returns a Reader that is owned by the
  // Api.
  virtual CompatibilityFlags::Reader getFeatureFlags() const = 0;

  // Create the context (global scope) object.
  virtual jsg::JsContext<api::ServiceWorkerGlobalScope> newContext(jsg::Lock& lock) const = 0;

  // Given a module's export namespace, return all the top-level exports.
  virtual jsg::Dict<NamedExport> unwrapExports(
      jsg::Lock& lock, v8::Local<v8::Value> moduleNamespace) const = 0;

  // Convenience struct for accessing typical Error properties.
  struct ErrorInterface {
    jsg::Optional<kj::String> name;
    jsg::Optional<kj::String> message;
    JSG_STRUCT(name, message);
  };
  virtual const jsg::TypeHandler<ErrorInterface>&
      getErrorInterfaceTypeHandler(jsg::Lock& lock) const = 0;
  virtual const jsg::TypeHandler<api::QueueExportedHandler>& getQueueTypeHandler(
      jsg::Lock& lock) const = 0;

  // Look up crypto algorithms by case-insensitive name. This can be used to extend the set of
  // WebCrypto algorithms supported.
  virtual kj::Maybe<const api::CryptoAlgorithm&> getCryptoAlgorithm(kj::StringPtr name) const {
    return kj::none;
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

// A Worker may bounce between threads as it handles multiple requests, but can only actually
// execute on one thread at a time. Each thread must therefore lock the Worker while executing
// code.
//
// A Worker::Lock MUST be allocated on the stack.
class Worker::Lock {
public:
  // Worker locks should normally be taken asynchronously. The TakeSynchronously type can be used
  // when a synchronous lock is unavoidable. The purpose of this type is to make it easy to find
  // all the places where we take synchronous locks.
  class TakeSynchronously {
  public:
    // We don't provide a default constructor so that call sites need to think about whether they
    // have a Request& available to pass in.
    explicit TakeSynchronously(kj::Maybe<RequestObserver&> request);

    kj::Maybe<RequestObserver&> getRequest();

  private:
    // Non-null if this lock is being taken on behalf of a request.
    RequestObserver* request = nullptr;
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

  // Logs an exception to the debug console or trace, if active.
  void logUncaughtException(kj::StringPtr description);

  // Logs an exception to the debug console or trace, if active.
  void logUncaughtException(UncaughtExceptionSource source,
                            const jsg::JsValue& exception,
                            const jsg::JsMessage& message = jsg::JsMessage());

  void reportPromiseRejectEvent(v8::PromiseRejectMessage& message);

  // Checks for problems with the registered event handlers (such as that there are none) and
  // reports them to the error reporter.
  void validateHandlers(ValidationErrorReporter& errorReporter);

  // Get the ExportedHandler exported under the given name. `entrypointName` may be null to get the
  // default handler. Returns null if this is not a modules-syntax worker (but `entrypointName`
  // must be null in that case).
  //
  // If running in an actor, the name is ignored and the entrypoint originally used to construct
  // the actor is returned.
  kj::Maybe<api::ExportedHandler&> getExportedHandler(
      kj::Maybe<kj::StringPtr> entrypointName, kj::Maybe<Worker::Actor&> actor);

  // Get the C++ object representing the global scope.
  api::ServiceWorkerGlobalScope& getGlobalScope();

  // Get the opaque storage key to use for recording trace information in async contexts.
  jsg::AsyncContextFrame::StorageKey& getTraceAsyncContextKey();

private:
  struct Impl;

  jsg::V8StackScope stackScope;
  Worker& worker;
  kj::Own<Impl> impl;

  friend class Worker;
};

// Can be initialized either from an `AsyncLock` or a `TakeSynchronously`, to indicate whether an
// async lock is held and help us grep for places in the code that do not support async locks.
class Worker::LockType {
public:
  LockType(Lock::TakeSynchronously origin): origin(origin) {}
  LockType(AsyncLock& origin): origin(&origin) {}

private:
  kj::OneOf<Lock::TakeSynchronously, AsyncLock*> origin;
  friend class Worker::Isolate;
};

// Represents the thread's ownership of an isolate's asynchronous lock. Call `takeAsyncLock()`
// on a `Worker` or `Worker::Isolate` to obtain this. Pass it to the constructor of
// `Worker::Lock` (as the `lockType`) in order to indicate that the calling thread has taken
// the async lock first.
//
// You must never store an `AsyncLock` long-term. Use it in a continuation and then discard it.
// To put it another way: An `AsyncLock` instance must never outlive an `evalLast()`.
class Worker::AsyncLock {
public:
  // Waits until the thread has no async locks, is not waiting on any locks, and has finished all
  // pending events (a la `kj::evalLast()`).
  static kj::Promise<void> whenThreadIdle();

private:
  kj::Own<AsyncWaiter> waiter;
  kj::Maybe<kj::Own<IsolateObserver::LockTiming>> lockTiming;

  AsyncLock(kj::Own<AsyncWaiter> waiter,
            kj::Maybe<kj::Own<IsolateObserver::LockTiming>> lockTiming)
      : waiter(kj::mv(waiter)), lockTiming(kj::mv(lockTiming)) {}

  friend class Worker::Isolate;
  friend class Worker::AsyncWaiter;
};

// Represents actor state within a Worker instance. This object tracks the JavaScript heap
// objects backing `event.actorState`. Multiple `Actor`s can be created within a single `Worker`.
class Worker::Actor final: public kj::Refcounted {
public:
  // Callback which constructs the `ActorCacheInterface` instance (if any) for the Actor. This
  // can be used to customize the storage implementation. This will be called synchronously in
  // the constructor.
  using MakeActorCacheFunc = kj::Function<kj::Maybe<kj::Own<ActorCacheInterface>>(
      const ActorCache::SharedLru& sharedLru, OutputGate& outputGate, ActorCache::Hooks& hooks)>;

  // Callback which constructs the `DurableObjectStorage` instance for an actor. This can be used
  // to customize the JavaScript API.
  using MakeStorageFunc = kj::Function<jsg::Ref<api::DurableObjectStorage>(
      jsg::Lock& js, const Api& api, ActorCacheInterface& actorCache)>;
  // TODO(cleanup): Can we refactor the (internal-codebase) user of this so that it doesn't need
  //   to customize the JS API but only the underlying ActorCacheInterface?

  using Id = kj::OneOf<kj::Own<ActorIdFactory::ActorId>, kj::String>;

  // Class that allows sending requests to this actor, recreating it as needed. It is safe to hold
  // onto this for longer than a Worker::Actor is alive.
  class Loopback {
  public:
    // Send a request to this actor, potentially re-creating it if it is not currently active.
    // The returned kj::Own<WorkerInterface> may be held longer than Loopback, and is assumed
    // to keep the Worker::Actor alive as well.
    virtual kj::Own<WorkerInterface> getWorker(IoChannelFactory::SubrequestMetadata metadata) = 0;

    virtual kj::Own<Loopback> addRef() = 0;
  };

  // The HibernationManager class manages HibernatableWebSockets created by an actor.
  // The manager handles accepting new WebSockets, retrieving existing WebSockets by tag, and
  // removing WebSockets from its collection when they disconnect.
  class HibernationManager : public kj::Refcounted {
  public:
    virtual void acceptWebSocket(jsg::Ref<api::WebSocket> ws, kj::ArrayPtr<kj::String> tags) = 0;
    virtual kj::Vector<jsg::Ref<api::WebSocket>> getWebSockets(
        jsg::Lock& js,
        kj::Maybe<kj::StringPtr> tag) = 0;
    virtual void hibernateWebSockets(Worker::Lock& lock) = 0;
    virtual void setWebSocketAutoResponse(kj::Maybe<kj::StringPtr> request,
        kj::Maybe<kj::StringPtr> response) = 0;
    virtual kj::Maybe<jsg::Ref<api::WebSocketRequestResponsePair>> getWebSocketAutoResponse() = 0;
    virtual void setTimerChannel(TimerChannel& timerChannel) = 0;
    virtual kj::Own<HibernationManager> addRef() = 0;
  };

  // Create a new Actor hosted by this Worker. Note that this Actor object may only be manipulated
  // from the thread that created it.
  Actor(const Worker& worker, kj::Maybe<RequestTracker&> tracker, Id actorId,
        bool hasTransient, MakeActorCacheFunc makeActorCache,
        kj::Maybe<kj::StringPtr> className, MakeStorageFunc makeStorage, Worker::Lock& lock,
        kj::Own<Loopback> loopback, TimerChannel& timerChannel, kj::Own<ActorObserver> metrics,
        kj::Maybe<kj::Own<HibernationManager>> manager, kj::Maybe<uint16_t> hibernationEventType);

  ~Actor() noexcept(false);

  // Call when starting any new request, to ensure that the actor object's constructor has run.
  //
  // This is used only for modules-syntax actors (which most are, since that's the only format we
  // support publicly).
  void ensureConstructed(IoContext&);

  // Forces cancellation of all "background work" this actor is executing, i.e. work that is not
  // happening on behalf of an active request. Note that this is not a part of the dtor because
  // IoContext objects prolong the lifetime of their Actor.
  //
  // `reasonCode` is passed back to the WorkerObserver.
  void shutdown(uint16_t reasonCode, kj::Maybe<const kj::Exception&> error = kj::none);

  // Stops new work on behalf of the ActorCache. This does not cancel any ongoing flushes.
  // TODO(soon) This should probably be folded into shutdown(). We'd need a piece that converts
  // `error` to `reasonCode` in workerd to do this. There may also be opportunities to streamline
  // interactions between `onAbort` and `onShutdown` promises.
  void shutdownActorCache(kj::Maybe<const kj::Exception&> error);

  // Get a promise that resolves when `shutdown()` has been called.
  kj::Promise<void> onShutdown();

  // Get a promise that rejects when this actor becomes broken in some way. See doc comments for
  // WorkerRuntime.makeActor() in worker.capnp for a discussion of actor brokenness.
  // Note that this doesn't cover every cause of actor brokenness -- some of them are fulfilled
  // in worker-set or process-sandbox code, in particular code updates and exceeded memory.
  // This method can only be called once.
  kj::Promise<void> onBroken();

  const Id& getId();
  Id cloneId();
  static Id cloneId(Id& id);
  kj::Maybe<jsg::JsRef<jsg::JsValue>> getTransient(Worker::Lock& lock);
  kj::Maybe<ActorCacheInterface&> getPersistent();
  kj::Own<Loopback> getLoopback();

  // Make the storage object for use in Service Workers syntax. This should not be used for
  // modules-syntax workers. (Note that Service-Workers-syntax actors are not supported publicly.)
  kj::Maybe<jsg::Ref<api::DurableObjectStorage>> makeStorageForSwSyntax(Worker::Lock& lock);

  ActorObserver& getMetrics();

  InputGate& getInputGate();
  OutputGate& getOutputGate();

  // Get the IoContext which should be used for all activity in this Actor. Returns null if
  // setIoContext() hasn't been called yet.
  kj::Maybe<IoContext&> getIoContext();

  // Set the IoContext for this actor. This is called once, when starting the first request
  // to the actor.
  void setIoContext(kj::Own<IoContext> context);
  // TODO(cleanup): Could we make it so the Worker::Actor can create the IoContext directly,
  //   rather than have WorkerEntrypoint create it on the first request? We'd have to plumb through
  //   some more information to the place where `Actor` is created, which might be uglier than it's
  //   worth.

  // Get the HibernationManager which should be used for all activity in this Actor. Returns null if
  // setHibernationManager() hasn't been called yet.
  kj::Maybe<HibernationManager&> getHibernationManager();

  // Set the HibernationManager for this actor. This is called once, on the first call to
  // `acceptWebSocket`.
  void setHibernationManager(kj::Own<HibernationManager> manager);

  // Gets the event type ID of the hibernation event, which is defined outside of workerd.
  // Only needs to be called when allocating a HibernationManager!
  kj::Maybe<uint16_t> getHibernationEventType();

  const Worker& getWorker() { return *worker; }

  void assertCanSetAlarm();
  kj::Promise<void> makeAlarmTaskForPreview(kj::Date scheduledTime);

  // If there is a scheduled or running alarm with the given `scheduledTime`, return a promise to
  // its result. This allows use to de-dupe multiple requests to a single `IoContext::run()`.
  kj::Maybe<kj::Promise<WorkerInterface::AlarmResult>> getAlarm(kj::Date scheduledTime);

  // Wait for `Date.now()` to be greater than or equal to `scheduledTime`. If the promise resolves
  // to an `AlarmFulfiller`, then the caller is responsible for invoking `fulfill()`, `reject()`, or
  // `cancel()`. Otherwise, the scheduled alarm was overriden by another call to `scheduleAlarm()`
  // and thus was cancelled. Note that callers likely want to invoke `getAlarm()` first to see if
  // there is an existing alarm at `scheduledTime` for which they want to wait (instead of
  // cancelling it).
  kj::Promise<WorkerInterface::ScheduleAlarmResult> scheduleAlarm(kj::Date scheduledTime);

  kj::Own<Worker::Actor> addRef() {
    KJ_IF_SOME(t, tracker) {
      return kj::addRef(*this).attach(t.get()->startRequest());
    } else {
      return kj::addRef(*this);
    }
  }
private:
  kj::Promise<WorkerInterface::ScheduleAlarmResult> handleAlarm(kj::Date scheduledTime);

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

KJ_DECLARE_NON_POLYMORPHIC(Worker::AsyncWaiter);

// An implementation of Worker::ValidationErrorReporter that collects errors into
// a kj::Vector<kj::String>.
struct SimpleWorkerErrorReporter final : public Worker::ValidationErrorReporter {
  void addError(kj::String error) override { errors.add(kj::mv(error)); }
  void addHandler(kj::Maybe<kj::StringPtr> exportName,
                  kj::StringPtr type) override {
    KJ_UNREACHABLE;
  }

  SimpleWorkerErrorReporter() = default;
  KJ_DISALLOW_COPY_AND_MOVE(SimpleWorkerErrorReporter);
  kj::Vector<kj::String> errors;
};

} // namespace workerd
