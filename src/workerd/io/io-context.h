// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "actor-id.h"
#include "io-channels.h"
#include "io-gate.h"
#include "io-own.h"
#include "io-timers.h"
#include "io-thread-context.h"
#include "limit-enforcer.h"
#include "trace.h"
#include "worker.h"
#include <workerd/api/deferred-proxy.h>
#include <workerd/jsg/async-context.h>
#include <workerd/jsg/jsg.h>
#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/mutex.h>
#include <kj/function.h>
#include <capnp/dynamic.h>
#include <workerd/util/weak-refs.h>
#include <workerd/io/limit-enforcer.h>
#include <workerd/io/io-channels.h>
#include <workerd/util/perfetto-tracing.h>
#include <workerd/util/uncaught-exception-source.h>

namespace capnp { class HttpOverCapnpFactory; }

namespace workerd {

[[noreturn]] void throwExceededMemoryLimit(bool isActor);

class IoContext;

// Represents one incoming request being handled by a IoContext. In non-actor scenarios,
// there is only ever one IncomingRequest per IoContext, but with actors there could be many.
//
// This should normally be referenced as IoContext::IncomingRequest, but it has been pulled
// out of the nested scope to allow forward-declaration.
//
// The purpose of tracking IncomingRequests at all is so that we can perform metrics, logging,
// and tracing on a "per-request basis", e.g. we can log that a particular incoming request
// generated N subrequests, and traces can trace through them. But this concept falls apart
// a bit when actors are in play, because we can't really say which incoming request "caused"
// any particular subrequest, especially when multiple incoming requests overlap. As a
// heuristic approximation, we attribute each subrequest (and all other forms of resource
// usage) to the "current" incoming request, which is defined as the newest request that hasn't
// already completed.
class IoContext_IncomingRequest {
public:
  IoContext_IncomingRequest(kj::Own<IoContext> context,
                            kj::Own<IoChannelFactory> ioChannelFactory,
                            kj::Own<RequestObserver> metrics,
                            kj::Maybe<kj::Own<WorkerTracer>> workerTracer);
  KJ_DISALLOW_COPY_AND_MOVE(IoContext_IncomingRequest);
  ~IoContext_IncomingRequest() noexcept(false);

  IoContext& getContext() { return *context; }

  // Invoked when the request is actually delivered.
  //
  // If, for some reason, this is not invoked before the object is destroyed, this indicate that
  // the event was canceled for some reason before delivery. No JavaScript was invoked.
  //
  // This method invokes metrics->delivered() and also makes this IncomingRequest "current" for
  // the IoContext.
  //
  // If delivered() is never called, then drain() need not be called.
  void delivered();

  // Waits until the request is "done". For non-actor requests this means waiting until
  // all "waitUntil" tasks finish, applying the "soft timeout" time limit from WorkerLimits.
  //
  // For actor requests, this means waiting until either all tasks have finished (not just
  // waitUntil, all tasks), or a new incoming request has been received (which then takes over
  // responsibility for waiting for tasks), or the actor is shut down.
  kj::Promise<void> drain();

  // Waits for all "waitUntil" tasks to finish, up to the time limit for scheduled events, as
  // defined by `scheduledTimeoutMs` in `WorkerLimits`. Returns a bool indicating `true` if the
  // event completed successfully, or `false`, if it was canceled early.
  //
  // Note that, while this is similar in some ways to `drain()`, `finishScheduled()` is intended
  // to be called synchronously during request handling, i.e. where a client is waiting for the
  // result, and the operation will be canceled if the client disconnects. `drain()` is intended
  // to be called after the client has received a response or disconnected.
  //
  // This method is also used by some custom event handlers (see WorkerInterface::CustomEvent) that
  // need similar behavior, as well as the test handler. TODO(cleanup): Rename to something more
  // generic?
  kj::Promise<bool> finishScheduled();

  RequestObserver& getMetrics() { return *metrics; }

  kj::Maybe<WorkerTracer&> getWorkerTracer() { return workerTracer; }

private:
  kj::Own<IoContext> context;
  kj::Own<RequestObserver> metrics;
  kj::Maybe<kj::Own<WorkerTracer>> workerTracer;
  kj::Own<IoChannelFactory> ioChannelFactory;

  bool wasDelivered = false;

  // Used for debugging, tracks whether we properly called drain() or some other mechanism to
  // wait for waitUntil tasks.
  bool waitedForWaitUntil = false;

  // If drain() was already called, this is non-null and fulfilling it will cancel the drain.
  // This is used in particular when a new IncomingRequest starts while the drain is being
  // awaited.
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> drainFulfiller;

  // Used by IoContext::incomingRequests.
  kj::ListLink<IoContext_IncomingRequest> link;

  friend class IoContext;
};

// IoContext holds state associated with a single I/O context. For stateless requests, each
// incoming request runs in a unique I/O context. For actors, each actor runs in a unique I/O
// context (but all requests received by that actor run in the same context).
//
// The IoContext serves as a bridge between JavaScript objects and I/O objects. I/O
// objects are strongly tied to the KJ event loop, and thus must live on a single thread. The
// JS isolate, however, can move between threads, bringing all garbage-collected heap objects
// with it. So, when a GC'd object holds a reference to I/O objects or tasks (KJ promises), it
// needs help from IoContext manage this.
//
// Whenever JavaScript is executing, the current IoContext can be obtained via
// `IoContext::current()`, and this can then be used to manage I/O, such as outgoing
// subrequests. When the IoContext is destroyed, all outstanding I/O objects and tasks
// created through it are destroyed immediately, even if objects on the JS heap still refer to
// them. Any attempt to access an I/O object from the wrong context will throw.
//
// This has an observable side-effect for workers: if a worker saves the request objects
// associated with one request into its global state and then attempts to access those objects
// within callbacks associated with some other request, an exception will be thrown. We actually
// like this. We don't want people leaking heavy objects or allowing simultaneous requests to
// interfere with each other.
class IoContext final: public kj::Refcounted, private kj::TaskSet::ErrorHandler {
public:
  class TimeoutManagerImpl;

  // Construct a new IoContext. Before using it, you must also create an IncomingRequest.
  IoContext(ThreadContext& thread,
                 kj::Own<const Worker> worker, kj::Maybe<Worker::Actor&> actor,
                 kj::Own<LimitEnforcer> limitEnforcer);

  // On destruction, all outstanding tasks associated with this request are canceled.
  ~IoContext() noexcept(false);

  using IncomingRequest = IoContext_IncomingRequest;

  const Worker& getWorker() { return *worker; }
  Worker::Lock& getCurrentLock() { return KJ_REQUIRE_NONNULL(currentLock); }

  kj::Maybe<Worker::Actor&> getActor() {
    return actor;
  }

  // Gets the actor, throwing if there isn't one.
  Worker::Actor& getActorOrThrow();

  RequestObserver& getMetrics() {
    return *getCurrentIncomingRequest().metrics;
  }

  const kj::Maybe<WorkerTracer&> getWorkerTracer() {
    if (incomingRequests.empty()) return kj::none;
    return getCurrentIncomingRequest().getWorkerTracer();
  }

  LimitEnforcer& getLimitEnforcer() { return *limitEnforcer; }

  // Get the current input lock. Throws an exception if no input lock is held (e.g. because this is
  // not an actor request).
  InputGate::Lock getInputLock();

  // Get the current CriticalSection, if there is one, or returns null if not.
  kj::Maybe<kj::Own<InputGate::CriticalSection>> getCriticalSection();

  // Runs `callback` within its own critical section, returning its final result. If `callback`
  // throws, the input lock will break, resetting the actor.
  //
  // This can only be called when I/O gates are active, i.e. in an actor.
  template <typename Func>
  jsg::PromiseForResult<Func, void, true> blockConcurrencyWhile(jsg::Lock& js, Func&& callback);

  // Returns true if output lock gating is necessary.
  // Can be used in optimizations to bypass wait* calls altogether.
  bool hasOutputGate();

  // Wait until all outstanding output locks have been unlocked. Does not wait for future output
  // locks, even if they are created before past locks are unlocked.
  //
  // This is used in actors to block output while some storage writes are uncommitted. For
  // non-actor requests, this always completes immediately.
  kj::Promise<void> waitForOutputLocks();

  // Like waitForOutputLocks() but, as an optimization, returns null in (some) cases where no
  // wait is needed, such as when the request is not an actor request.
  //
  // Use the ...IoOwn() overload if you need to store this promise in a JS API object.
  kj::Maybe<kj::Promise<void>> waitForOutputLocksIfNecessary();
  kj::Maybe<IoOwn<kj::Promise<void>>> waitForOutputLocksIfNecessaryIoOwn();

  // Check if the output gate (only used by actors) is currently broken. This indicates that there
  // was a problem with committing storage writes.
  //
  // For non-actor requests, this always returns false.
  bool isOutputGateBroken();

  // Lock output until the given promise completes.
  //
  // It is an error to call this outside of actors.
  template <typename T>
  kj::Promise<T> lockOutputWhile(kj::Promise<T> promise);

  bool isInspectorEnabled();
  bool isFiddle();

  // Log a warning to the inspector. This is a no-op if the inspector is not enabled.
  void logWarning(kj::StringPtr description);

  // Log a warning to the inspector. This is a no-op if the inspector is not enabled. Deduplicates
  // warning messages such that a single unique message will only be logged once for the lifetime of
  // an isolate.
  void logWarningOnce(kj::StringPtr description);

  // Log an internal error message. Deduplicates log messages such that a single unique message will
  // only be logged once for the lifetime of an isolate.
  void logErrorOnce(kj::StringPtr description);

  void logUncaughtException(kj::StringPtr description);
  void logUncaughtException(UncaughtExceptionSource source,
                            const jsg::JsValue& exception,
                            const jsg::JsMessage& message = jsg::JsMessage());

  // Log an uncaught exception from an asynchronous context, i.e. when the IoContext is not
  // "current".
  void logUncaughtExceptionAsync(UncaughtExceptionSource source, kj::Exception&& e);

  void reportPromiseRejectEvent(v8::PromiseRejectMessage& message);

  // Returns a promise that will reject with an exception if and when the request should be
  // aborted, e.g. because its CPU time expired. This should be joined with any promises for
  // incoming tasks.
  kj::Promise<void> onAbort() { return abortPromise.addBranch(); }

  // Force context abort now.
  void abort(kj::Exception&& e) { abortFulfiller->reject(kj::mv(e)); }

  // Has event.passThroughOnException() been called?
  bool isFailOpen() { return failOpen; }

  // Called by event.passThroughOnException().
  void setFailOpen() { failOpen = true; }

  // -----------------------------------------------------------------
  // Tracking thread-local request

  // Asynchronously execute a callback inside the context.
  //
  // We don't use a "scope" class because this might actually switch to a larger stack for the
  // duration of the callback.
  //
  // If `inputLock` is not provided, and this is an actor context, an input lock will be obtained
  // before executing the callback.
  template <typename Func>
  kj::PromiseForResult<Func, Worker::Lock&> run(
      Func&& func, kj::Maybe<InputGate::Lock> inputLock = kj::none)
      KJ_WARN_UNUSED_RESULT;

  // Like run() but executes within the given critical section, if it is non-null. If
  // `criticalSection` is null, then this just forwards to the other run() (with null inputLock).
  template <typename Func>
  kj::PromiseForResult<Func, Worker::Lock&> run(
      Func&& func, kj::Maybe<kj::Own<InputGate::CriticalSection>> criticalSection)
      KJ_WARN_UNUSED_RESULT;

  // Returns the current IoContext for the thread.
  // Throws an exception if there is no current context (see hasCurrent() below).
  static IoContext& current();

  // True if there is a current IoContext for the thread (current() will not throw).
  static bool hasCurrent();

  // True if this is the IoContext for the current thread (same as `hasCurrent() && tcx == current()`).
  bool isCurrent();

  // Like requireCurrent() but throws a JS error if this IoContext is not the current.
  void requireCurrentOrThrowJs();

  // A WeakRef is a weak reference to a IoContext. Note that because IoContext is not
  // itself ref-counted, we cannot follow the usual pattern of a weak reference that potentially
  // converts to a strong reference. Instead, intended usage looks like so:
  // ```
  // auto& context = IoContext::current();
  // return canOutliveContext().then([contextWeakRef = context.getWeakRef()]() mutable {
  //   auto hadContext = contextWeakRef.runIfAlive([&](IoContext& context){
  //     useContextFinally(context);
  //   });
  //   if (!hadContext) {
  //     doWhatMustBeDone();
  //   }
  // });
  // ```
  using WeakRef = workerd::WeakRef<IoContext>;

  kj::Own<WeakRef> getWeakRef() {
    return kj::addRef(*selfRef);
  }

  // If there is a current IoContext, return its WeakRef.
  static kj::Maybe<kj::Own<WeakRef>> tryGetWeakRefForCurrent();

  // Like requireCurrentOrThrowJs() but works on a WeakRef.
  static void requireCurrentOrThrowJs(WeakRef& weak);

  // Just throw the error that requireCurrentOrThrowJs() would throw on failure.
  [[noreturn]] static void throwNotCurrentJsError();

  // -----------------------------------------------------------------
  // Task scheduling and object storage

  // Arrange for the given promise to execute as part of this request. It will be canceled if the
  // request is canceled.
  void addTask(kj::Promise<void> promise);

  template <typename T, typename Func>
  jsg::PromiseForResult<Func, T, true> awaitIo(
      jsg::Lock& js, kj::Promise<T> promise, Func&& func);

  // Waits for some background I/O to complete, then executes `func` on the result, returning a
  // JavaScript promise for the result of that. If no `func` is provided, no transformation is
  // applied.
  //
  // If the IoContext is canceled, the I/O promise will be canceled, `func` will be destroyed
  // without being called, and the JS promise will never resolve.
  //
  // You might wonder why this function takes a continuation function as a parameter, rather than
  // taking a single `kj::Promise<T>`, returning `jsg::Promise<T>`, and leaving it up to you to
  // call `.then()` on the result. The answer is that `func` provides stronger guarantees about the
  // context where it runs, which avoids the need for `IoOwn`s:
  // - `func` itself can safely capture I/O objects without IoOwn, because the function itself
  //   is attached to the IoContext. (If the IoContext is canceled, `func` is destroyed.)
  // - Similarly, the result of `promise` can be an I/O object without needing to be wrapped in
  //   IoOwn, because `func` is guaranteed to be called in this IoContext.
  //
  // Conversely, you might wonder why you wouldn't use `awaitIo(promise.then(func))` instead, which
  // would also avoid the need for `IoOwn` since `func` would run as part of the KJ event loop.
  // But, in this version, `func` cannot access any JavaScript objects, because it would not run
  // with the isolate lock.
  //
  // Historically, we solved this with something called `capctx`. You would write something like:
  // `awaitIo(promise.then(capctx(func)))`. This provided both properties: `func()` ran both in
  // the KJ event loop and with the isolate lock held. However, this had the problem that it
  // required returning to the KJ event loop between running func() and runnning whatever
  // JavaScript code was waiting on it. This implies releasing the isolate lock just to
  // immediately acquire it again, which was wasteful. Passing `func` as a parameter to `awaitIo()`
  // allows it to run under the same isolate lock that then runs the awaiting JavaScript.
  //
  // Note that awaitIo() automatically implies registering a pending event while waiting for the
  // promise (no need to call registerPendingEvent()).
  template <typename T>
  jsg::Promise<T> awaitIo(jsg::Lock& js, kj::Promise<T> promise);

  // Waits for the given I/O while holding the input lock, so that all other I/O is blocked from
  // completing in the meantime (unless it is also holding the same input lock).
  template <typename T>
  jsg::Promise<T> awaitIoWithInputLock(jsg::Lock& js, kj::Promise<T> promise);

  template <typename T, typename Func>
  jsg::PromiseForResult<Func, T, true> awaitIoWithInputLock(jsg::Lock& js,
                                                             kj::Promise<T> promise,
                                                             Func&& func);

  // DEPRECATED: Like awaitIo() but:
  // - Does not have a continuation function, so suffers from the problems described in
  //   `awaitIo()`'s doc comment.
  // - Does not automatically register a pending event.
  //
  // This is used to implement the historical KJ-oriented PromiseWrapper behavior in terms of the
  // new `awaitIo()` implementation. This should go away once all API implementations are
  // refactored to use `awaitIo()`.
  template <typename T>
  jsg::Promise<T> awaitIoLegacy(jsg::Lock& js, kj::Promise<T> promise);

  // DEPRECATED: Like awaitIo() but:
  // - Does not have a continuation function, so suffers from the problems described in
  //   `awaitIo()`'s doc comment.
  // - Does not automatically register a pending event.
  //
  // This is used to implement the historical KJ-oriented PromiseWrapper behavior in terms of the
  // new `awaitIo()` implementation. This should go away once all API implementations are
  // refactored to use `awaitIo()`.
  template <typename T>
  jsg::Promise<T> awaitIoLegacyWithInputLock(jsg::Lock& js, kj::Promise<T> promise);

  // Returns a KJ promise that resolves when a particular JavaScript promise completes.
  //
  // The JS promise must complete within this IoContext. The KJ promise will reject
  // immediately if any of these happen:
  // - The JS promise is GC'd without resolving.
  // - The JS promise is resolved from the wrong context.
  // - The system detects that no further process will be made in this context (because there is no
  //   more JavaScript to run, and there is no outstanding I/O scheduled with awaitIo()).
  //
  // If `T` is `IoOwn<U>`, it will be unwrapped to just `U` in the result. If `U` is in turn
  // `kj::Promise<V>`, then the promises will be chained as usual, so the final result is
  // `kj::Promise<V>`.
  template <typename T>
  kj::_::ReducePromises<RemoveIoOwn<T>> awaitJs(jsg::Lock& js, jsg::Promise<T> promise);

  // Returns the number of times addTask() has been called (even if the tasks have completed).
  uint taskCount() { return addTaskCounter; }

  // Indicates that the script has requested that it stay active until the given promise resolves.
  // drain() waits until all such promises have completed.
  void addWaitUntil(kj::Promise<void> promise);

  // Returns the status of waitUntil promises. If a promise fails, this sets the status to the
  // one corresponding to the exception type.
  EventOutcome waitUntilStatus() const { return waitUntilStatusValue; }

  // DO NOT USE, use `addWaitUntil()` instead.
  kj::TaskSet& getWaitUntilTasks() {
    // TODO(cleanup): This is only needed for use with RpcWorkerInterface, but we can eliminate
    //   that class's need for waitUntilTasks if we change the signature of sendTraces() to return
    //   a promise, I think.
    return waitUntilTasks;
  }

  // Wraps a reference in a wrapper which:
  // 1. Will throw an exception if dereferenced while the IoContext is not current for the
  //    thread.
  // 2. Can be safely destroyed from any thread.
  // 3. Invalidates itself when the request ends (such that dereferencing throws).
  template <typename T> IoOwn<T> addObject(kj::Own<T> obj);

  // Wraps a reference in a wrapper which:
  // 1. Will throw an exception if dereferenced while the IoContext is not current for the
  //    thread.
  // 2. Can be safely destroyed from any thread.
  // 3. Invalidates itself when the request ends (such that dereferencing throws).
  template <typename T> IoPtr<T> addObject(T& obj);

  // Like addObject() but takes a functor, returning a functor with the same signature but which
  // holds the original functor under a `IoOwn`, and so will stop working if the IoContext
  // is no longer valid. This is particularly useful for passing to `jsg::Promise::then()` when
  // you need the continuation to run in the correct context.
  template <typename Func>
  auto addFunctor(Func&& func);

  // Call this to indicate that the caller expects to call into JavaScript in this IoContext
  // at some point in the future, in response to some *external* event that the caller is waiting
  // for. Then, hold on to the returned handle until that time. This prevents finalizers from being
  // called in the meantime.
  kj::Own<void> registerPendingEvent();
  // TODO(cleanup): awaitIo() automatically applies this. Is the public method needed anymore?

  // When you want to perform a task that returns Promise<DeferredProxy<T>> and the application
  // JavaScript is waiting for the result, use `context.waitForDeferredProxy(promise)` to turn it
  // into a regular `Promise<T>`, including registering pending events as needed.
  template <typename T>
  kj::Promise<T> waitForDeferredProxy(kj::Promise<api::DeferredProxy<T>>&& promise) {
    return promise.then([this](api::DeferredProxy<T> deferredProxy) {
      return deferredProxy.proxyTask.attach(registerPendingEvent());
    });
  }

  // Like awaitIo(), but handles the specific case of Promise<DeferredProxy>. This is special
  // becaues the convention is that the outer promise is NOT treated as a pending I/O event; it
  // may actually be waiting for something to happen in JavaScript land. Once the outer promise
  // resolves, the inner promise (the DeferredProxy<T>) is treated as external I/O.
  template <typename T>
  jsg::Promise<T> awaitDeferredProxy(jsg::Lock& js, kj::Promise<api::DeferredProxy<T>>&& promise) {
    return awaitIoImpl(js, waitForDeferredProxy(kj::mv(promise)), getCriticalSection(),
                       IdentityFunc<T>());
  }

  bool isFinalized() {
    return ownedObjects.isFinalized();
  }

  // Called by ScheduledEvent
  void setNoRetryScheduled() { retryScheduled = false; }

  // Called by ServiceWorkerGlobalScope::runScheduled
  bool shouldRetryScheduled() { return retryScheduled; }

  // -----------------------------------------------------------------
  // Access to I/O

  // Used to implement setTimeout(). We don't expose the timer directly because the
  // promises it returns need to live in this I/O context, anyway.
  TimeoutId setTimeoutImpl(
    TimeoutId::Generator& generator,
    bool repeat,
    jsg::Function<void()> function,
    double msDelay);

  // Used to implement clearTimeout(). We don't expose the timer directly because the
  // promises it returns need to live in this I/O context, anyway.
  void clearTimeoutImpl(TimeoutId key);

  size_t getTimeoutCount();

  // Access the event loop's current time point. This will remain constant between ticks.
  kj::Date now(IncomingRequest& incomingRequest);

  // Access the event loop's current time point. This will remain constant between ticks.
  kj::Date now();

  // Returns a promise that resolves once `now() >= when`.
  kj::Promise<void> atTime(kj::Date when) { return getIoChannelFactory().getTimer().atTime(when); }

  // Returns a promise that resolves after some time. This is intended to be used for implementing
  // time limits on some sort of operation, not for implementing application-driven timing, as it
  // does not maintain consistency with the clock as observed through Date.now(), e.g. when it
  // comes to spectre mitigations.
  kj::Promise<void> afterLimitTimeout(kj::Duration t) {
    return getIoChannelFactory().getTimer().afterLimitTimeout(t);
  }

  // Provide access to the system CSPRNG.
  kj::EntropySource& getEntropySource() { return thread.getEntropySource(); }

  capnp::HttpOverCapnpFactory& getHttpOverCapnpFactory() {
    return thread.getHttpOverCapnpFactory();
  }

  capnp::ByteStreamFactory& getByteStreamFactory() {
    return thread.getByteStreamFactory();
  }

  const kj::HttpHeaderTable& getHeaderTable() { return thread.getHeaderTable(); }
  const ThreadContext::HeaderIdBundle& getHeaderIds() { return thread.getHeaderIds(); }

  // Subrequest channel numbers for the two special channels.
  // NULL = The channel used by global fetch() when the Request has no fetcher attached.
  // NEXT = DEPRECATED: The fetcher attached to Requests delivered by a FetchEvent, so that we can
  //     detect when an incoming request is passed through to `fetch()` (perhaps with rewrites)
  //     and treat that case differently. In practice this has proven too confusing, so we don't
  //     plan to treat NEXT and NULL differently going forward.
  static constexpr uint NULL_CLIENT_CHANNEL = 0;
  static constexpr uint NEXT_CLIENT_CHANNEL = 1;

  // Number of subrequest channels that have special meaning (and so won't appear in any binding).
  static constexpr uint SPECIAL_SUBREQUEST_CHANNEL_COUNT = 2;

  struct SubrequestOptions final {
    // When inHouse is true, the subrequest is to an API provided internally. For example calls
    // to KV. This primarily affects metrics and limits.
    bool inHouse;

    // When true, the client is wrapped by metrics.wrapSubrequestClient() ensuring appropriate
    // metrics collection.
    bool wrapMetrics;

    // The name to use for the request's span if tracing is turned on.
    kj::Maybe<kj::ConstString> operationName;
  };

  kj::Own<WorkerInterface> getSubrequestNoChecks(
      kj::FunctionParam<kj::Own<WorkerInterface>(SpanBuilder&, IoChannelFactory&)> func,
      SubrequestOptions options);

  // If creating a new subrequest is permitted, calls the given factory function synchronously to
  // create one.
  kj::Own<WorkerInterface> getSubrequest(
      kj::FunctionParam<kj::Own<WorkerInterface>(SpanBuilder&, IoChannelFactory&)> func,
      SubrequestOptions options);

  // Get WorkerInterface objects to use for subrequests.
  //
  // `channel` specifies which outgoing channel to use. The special channel 0 refers to the "null"
  // binding (used for fetches where `request.fetcher` is not set), and channel 1 refers to the
  // "next" binding (used when request.fetcher is carried over from the incoming request).
  // Named bindings, e.g. Worker2Worker bindings, will have indices starting from 2. Fetcher
  // bindings declared via Worker::Global::Fetcher have a corresponding `channel` property to refer
  // to these outgoing bindings.
  //
  // `isInHouse` is true if this client represents an "in house" endpoint, i.e. some API provided
  // by the Workers platform. For example, KV namespaces are in-house. This primarily affects
  // metrics and limits:
  // - In-house requests do not count as "subrequests" for metrics and logging purposes.
  // - In-house requests are not subject to the same limits on the number of subrequests per
  //   request.
  // - In preview, in-house requests do not show up in the network tab.
  //
  // `operationName` is the name to use for the request's span, if tracing is turned on.
  kj::Own<WorkerInterface> getSubrequestChannel(
      uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson, kj::ConstString operationName);

  // Like getSubrequestChannel() but doesn't enforce limits. Use for trusted paths only.
  kj::Own<WorkerInterface> getSubrequestChannelNoChecks(
      uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson,
      kj::Maybe<kj::ConstString> operationName = kj::none);

  // Convenience methods that call getSubrequest*() and adapt the returned WorkerInterface objects
  // to HttpClient.
  kj::Own<kj::HttpClient> getHttpClient(
      uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson, kj::ConstString operationName);

  // Convenience methods that call getSubrequest*() and adapt the returned WorkerInterface objects
  // to HttpClient.
  kj::Own<kj::HttpClient> getHttpClientNoChecks(
      uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson,
      kj::Maybe<kj::ConstString> operationName = kj::none);
  // TODO(cleanup): Make it the caller's job to call asHttpClient() on the result of
  //   getSubrequest*().

  capnp::Capability::Client getCapnpChannel(uint channel) {
    return getIoChannelFactory().getCapability(channel);
  }

  kj::Own<IoChannelFactory::ActorChannel> getGlobalActorChannel(
      uint channel, const ActorIdFactory::ActorId& id, kj::Maybe<kj::String> locationHint,
      ActorGetMode mode, SpanParent parentSpan) {
    return getIoChannelFactory().getGlobalActor(channel, id, kj::mv(locationHint), mode,
        kj::mv(parentSpan));
  }
  kj::Own<IoChannelFactory::ActorChannel> getColoLocalActorChannel(uint channel, kj::StringPtr id,
      SpanParent parentSpan) {
    return getIoChannelFactory().getColoLocalActor(channel, id, kj::mv(parentSpan));
  }

  void abortAllActors() {
    return getIoChannelFactory().abortAllActors();
  }

  // Get an HttpClient to use for Cache API subrequests.
  kj::Own<CacheClient> getCacheClient();

  // Returns an object that ensures an async JS operation started in the current scope captures the
  // given trace span, or the current request's trace span, if no span is given.
  jsg::AsyncContextFrame::StorageScope makeAsyncTraceScope(
      Worker::Lock& lock, kj::Maybe<SpanParent> spanParent = kj::none) KJ_WARN_UNUSED_RESULT;

  // Returns the current span being recorded.  If called while the JS lock is held, uses the trace
  // information from the current async context, if available.
  SpanParent getCurrentTraceSpan();

  // Returns a builder for recording tracing spans (or a no-op builder if tracing is inactive).
  // If called while the JS lock is held, uses the trace information from the current async
  // context, if available.
  SpanBuilder makeTraceSpan(kj::ConstString operationName);

  // Implement per-IoContext rate limiting for Cache.put(). Pass the body of a Cache API PUT
  // request and get a possibly wrapped stream back.
  //
  // The returned promise is fulfilled with kj::none if the Cache API PUT quota is already exceeded,
  // or if the passed stream would cause it to be exceeded. If the stream has an unknown length, you
  // will get a wrapped stream back that tracks how many bytes are read/pumped out of the stream,
  // then decrements the per-IoContext quota on destruction.
  jsg::Promise<kj::Maybe<IoOwn<kj::AsyncInputStream>>> makeCachePutStream(
      jsg::Lock& js, kj::Own<kj::AsyncInputStream> stream);
  // TODO(cleanup): Factor this into getCacheClient() somehow so it's not opt-in.

  // Gets a CapabilityServerSet representing the capnp capabilities hosted by this request or
  // actor context. This allows us to implement the CapnpCapability::unwrap() method on
  // capabilities which allows the application to get at the underlying server object, when the
  // capability points to a local object.
  capnp::CapabilityServerSet<capnp::DynamicCapability>& getLocalCapSet() {
    return localCapSet;
  }

  void writeLogfwdr(uint channel, kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage);

  jsg::JsObject getPromiseContextTag(jsg::Lock& js);

private:
  ThreadContext& thread;

  kj::Own<WeakRef> selfRef = kj::refcounted<WeakRef>(kj::Badge<IoContext>(), *this);

  kj::Own<const Worker> worker;
  kj::Maybe<Worker::Actor&> actor;
  kj::Own<LimitEnforcer> limitEnforcer;

  // List of active IncomingRequests, ordered from most-recently-started to least-recently-started.
  kj::List<IncomingRequest, &IncomingRequest::link> incomingRequests;

  capnp::CapabilityServerSet<capnp::DynamicCapability> localCapSet;

  bool failOpen = false;

  // For debug checks.
  void* threadId;

  // For scheduled workers noRetry calls
  bool retryScheduled = true;

  kj::Maybe<Worker::Lock&> currentLock;
  kj::Maybe<InputGate::Lock> currentInputLock;

  DeleteQueuePtr deleteQueue;

  kj::Own<kj::PromiseFulfiller<void>> abortFulfiller;
  kj::ForkedPromise<void> abortPromise = nullptr;

  class PendingEvent;

  kj::Maybe<PendingEvent&> pendingEvent;
  kj::Maybe<kj::Promise<void>> runFinalizersTask;

  // Objects pointed to by IoOwn<T>s.
  // NOTE: This must live below `deleteQueue`, as some of these OwnedObjects may own attachctx()'ed
  //   objects which reference `deleteQueue` in their destructors.
  OwnedObjectList ownedObjects;

  // Implementation detail of makeCachePutStream().

  constexpr static size_t GB = 1 << 30;
  constexpr static size_t MAX_TOTAL_PUT_SIZE = 5 * GB;
  kj::Promise<size_t> cachePutQuota = MAX_TOTAL_PUT_SIZE;

  kj::TaskSet waitUntilTasks;
  EventOutcome waitUntilStatusValue = EventOutcome::OK;

  void setTimeoutImpl(TimeoutId timeoutId, bool repeat, jsg::V8Ref<v8::Function> function,
    double msDelay, kj::Array<jsg::Value> args);

  uint addTaskCounter = 0;
  kj::Maybe<kj::TaskSet> tasks;

  // The timeout manager needs to live below `deleteQueue` because the promises may refer to
  // objects in the queue.

  // ATTENTION: `tasks` and `timeoutManager` MUST be destructed before any other member.
  // If any other member is destructed after (is declared later in the class than) these two
  // members, then there is a possibility that callbacks will attempt to use a partially or fully
  // destructed IoContext object. For the same reason, any promises stored outside of the
  // IoContext (e.g. in the ActorContext) MUST be canceled when the IoContext is
  // destructed.
  kj::Own<TimeoutManager> timeoutManager;

  kj::Own<WorkerInterface> getSubrequestChannelImpl(
      uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson,
      SpanBuilder& span, IoChannelFactory& channelFactory);

  friend class IoContext_IncomingRequest;
  template <typename T> friend class IoOwn;
  template <typename T> friend class IoPtr;

  void taskFailed(kj::Exception&& exception) override;
  void requireCurrent();
  void checkFarGet(const DeleteQueue* expectedQueue);

  kj::Maybe<jsg::JsRef<jsg::JsObject>> promiseContextTag;

  class Runnable {
  public:
    virtual void run(Worker::Lock& lock) = 0;
  };
  void runImpl(Runnable& runnable, bool takePendingEvent, Worker::LockType lockType,
               kj::Maybe<InputGate::Lock> inputLock, bool allowPermanentException);

  void runFinalizers(Worker::AsyncLock& asyncLock);

  template <typename T>
  struct IdentityFunc {
    inline T operator()(jsg::Lock&, T&& value) const {
      return kj::mv(value);
    }
  };
  template <>
  struct IdentityFunc<void> {
    inline void operator()(jsg::Lock&) const {}
  };

  template <typename T>
  struct ExceptionOr_ {
    using Type = kj::OneOf<T, kj::Exception>;
  };
  template <>
  struct ExceptionOr_<void> {
    using Type = kj::Maybe<kj::Exception>;
  };
  template <typename T>
  using ExceptionOr = typename ExceptionOr_<T>::Type;

  template <typename T, typename InputLockOrMaybeCriticalSection, typename Func>
  jsg::PromiseForResult<Func, T, true> awaitIoImpl(
      jsg::Lock& js, kj::Promise<T> promise, InputLockOrMaybeCriticalSection ilOrCs, Func&& func);

  // The IncomingRequest that is currently considered "current". This is always the
  // latest-starting request that hasn't yet completed.
  //
  // For stateless requests, there is only ever one IncomingRequest per IoContext. For
  // actors, there is one IoContext per actor, and each incoming request to the actor
  // creates a new  IncomingRequest.
  //
  // The current request is tracked for metrics, logging, and tracing purposes. Any resource
  // usage on the part of the actor, including outgoing subrequests, is attributed to the current
  // request for logging and tracing. This is a hack, we don't actually know which request
  // "caused" any particular resource usage, so this is merely our best guess.
  //
  // The IoChannelFactory must also be accessed through the currentIncomingRequest because it has
  // some tracing context built in.
  IncomingRequest& getCurrentIncomingRequest() {
    KJ_REQUIRE(!incomingRequests.empty(), "the IoContext has no current IncomingRequest");
    return incomingRequests.front();
  }
  IoChannelFactory& getIoChannelFactory() {
    return *getCurrentIncomingRequest().ioChannelFactory;
  }

  // Run the given callback within the scope of this IoContext. This encapsultes the
  // setup of a number of scopes that must be entered prior to running within the
  // context, including entering the V8StackScope and acquiring the Worker::Lock.
  void runInContextScope(Worker::LockType lockType,
                         kj::Maybe<InputGate::Lock> inputLock,
                         kj::Function<void(Worker::Lock&)> func);

  friend class Finalizeable;
  friend class DeleteQueue;
};

// =======================================================================================
// inline implementation details

template <typename T>
kj::Promise<T> IoContext::lockOutputWhile(kj::Promise<T> promise) {
  return getActorOrThrow().getOutputGate().lockWhile(kj::mv(promise));
}

template <typename Func>
kj::PromiseForResult<Func, Worker::Lock&> IoContext::run(
    Func&& func, kj::Maybe<kj::Own<InputGate::CriticalSection>> criticalSection) {
  KJ_IF_SOME(cs, criticalSection) {
    return cs.get()->wait()
        .then([this,func=kj::fwd<Func>(func)](InputGate::Lock&& inputLock) mutable {
      return run(kj::fwd<Func>(func), kj::mv(inputLock));
    });
  } else {
    return run(kj::fwd<Func>(func));
  }
}

template <typename Func>
kj::PromiseForResult<Func, Worker::Lock&> IoContext::run(
    Func&& func, kj::Maybe<InputGate::Lock> inputLock) {
  kj::Promise<Worker::AsyncLock> asyncLockPromise = nullptr;
  KJ_IF_SOME(a, actor) {
    if (inputLock == kj::none) {
      return a.getInputGate().wait()
          .then([this,func=kj::fwd<Func>(func)](InputGate::Lock&& inputLock) mutable {
        return run(kj::fwd<Func>(func), kj::mv(inputLock));
      });
    }

    asyncLockPromise = worker->takeAsyncLockWhenActorCacheReady(now(), a, getMetrics());
  } else {
    asyncLockPromise = worker->takeAsyncLock(getMetrics());
  }

  return asyncLockPromise
      .then([this,inputLock=kj::mv(inputLock),func=kj::fwd<Func>(func)]
            (Worker::AsyncLock lock) mutable {
    typedef decltype(func(kj::instance<Worker::Lock&>())) Result;
    if constexpr (kj::isSameType<Result, void>()) {
      struct RunnableImpl: public Runnable {
        Func func;

        RunnableImpl(Func&& func): func(kj::fwd<Func>(func)) {}
        void run(Worker::Lock& lock) override {
          func(lock);
        }
      };

      RunnableImpl runnable(kj::fwd<Func>(func));
      runImpl(runnable, true, lock, kj::mv(inputLock), false);
    } else {
      struct RunnableImpl: public Runnable {
        Func func;
        kj::Maybe<Result> result;

        RunnableImpl(Func&& func): func(kj::fwd<Func>(func)) {}
        void run(Worker::Lock& lock) override {
          result = func(lock);
        }
      };

      RunnableImpl runnable { kj::fwd<Func>(func) };
      runImpl(runnable, true, lock, kj::mv(inputLock), false);
      KJ_IF_SOME(r, runnable.result) {
        return kj::mv(r);
      } else {
        KJ_UNREACHABLE;
      }
    }
  });
}

template <typename T, typename Func>
jsg::PromiseForResult<Func, T, true> IoContext::awaitIo(
    jsg::Lock& js, kj::Promise<T> promise, Func&& func) {
  return awaitIoImpl(js, promise.attach(registerPendingEvent()), getCriticalSection(),
                     kj::fwd<Func>(func));
}

template <typename T>
jsg::Promise<T> IoContext::awaitIo(jsg::Lock& js, kj::Promise<T> promise) {
  return awaitIoImpl(js, promise.attach(registerPendingEvent()), getCriticalSection(),
                     IdentityFunc<T>());
}

template <typename T, typename Func>
jsg::PromiseForResult<Func, T, true> IoContext::awaitIoWithInputLock(
    jsg::Lock& js, kj::Promise<T> promise, Func&& func) {
  return awaitIoImpl(js, promise.attach(registerPendingEvent()), getInputLock(),
                     kj::fwd<Func>(func));
}

template <typename T>
jsg::Promise<T> IoContext::awaitIoWithInputLock(jsg::Lock& js, kj::Promise<T> promise) {
  return awaitIoImpl(js, promise.attach(registerPendingEvent()), getInputLock(),
                     IdentityFunc<T>());
}

template <typename T>
jsg::Promise<T> IoContext::awaitIoLegacy(jsg::Lock& js, kj::Promise<T> promise) {
  return awaitIoImpl(js, kj::mv(promise), getCriticalSection(), IdentityFunc<T>());
}

template <typename T>
jsg::Promise<T> IoContext::awaitIoLegacyWithInputLock(jsg::Lock& js, kj::Promise<T> promise) {
  return awaitIoImpl(js, kj::mv(promise), getInputLock(), IdentityFunc<T>());
}

template <typename T, typename InputLockOrMaybeCriticalSection, typename Func>
jsg::PromiseForResult<Func, T, true> IoContext::awaitIoImpl(
    jsg::Lock& js, kj::Promise<T> promise, InputLockOrMaybeCriticalSection ilOrCs, Func&& func) {
  // WARNING: The fact that `promise` has been passed by value whereas `func` is by reference is
  // actually important, because this means that if we throw an exception here in the function
  // body, `promise` will be destroyed first, before `func`. That's important as often `func`
  // holds ownership of objects that `promise` depends on.

  requireCurrent();

  // `T` is the type produced by the input promise. `Result` is the type of the final output
  // promise. `Func` transforms from `T` to `Result`.
  typedef jsg::ReturnType<Func, T, true> Result;

  // It is necessary for us to grab a reference to the jsg::AsyncContextFrame here
  // and pass it into the then(). If the promise is rejected, and there is no rejection
  // handler attached to it, an unhandledrejection event will be scheduled, and scheduling
  // that event needs to be done within the appropriate frame to propagate the correct context.

  // We need to catch exceptions from KJ and merge them into the result, so that they can propagate
  // to JavaScript.
  kj::Promise<ExceptionOr<T>> promiseForExceptionOrT = [&]() {
    if constexpr (jsg::isVoid<T>()) {
      return promise.then([]() -> ExceptionOr<T> {
        return kj::none;
      }, [](kj::Exception&& exception) -> ExceptionOr<T> {
        return kj::mv(exception);
      });
    } else {
      return promise.then([](T&& result) -> ExceptionOr<T> {
        return kj::mv(result);
      }, [](kj::Exception&& exception) -> ExceptionOr<T> {
        return kj::mv(exception);
      });
    }
  }();

  // Reminder: This can throw JsExceptionThrown if the execution context has been termintaed.
  // it's important in that case that `promiseForExceptionOrT` will be destroyed before `func`.
  auto [ jsPromise, resolver ] = js.newPromiseAndResolver<ExceptionOr<Result>>();

  addTask(promiseForExceptionOrT
      .then([this, resolver = kj::mv(resolver), ilOrCs = kj::mv(ilOrCs),
            maybeAsyncContext = jsg::AsyncContextFrame::currentRef(js),
            // Reminder: It's important that `func` gets attached to the promise before the whole
            // thing is passed to `addTask()`, so that it's impossible for `func` to be destroyed
            // before the inner promise.
            func = kj::fwd<Func>(func)]
            (ExceptionOr<T>&& exceptionOrT) mutable {
    return run([resolver = kj::mv(resolver),
                exceptionOrT = kj::mv(exceptionOrT),
                maybeAsyncContext = kj::mv(maybeAsyncContext),
                func = kj::fwd<Func>(func)](Worker::Lock& lock) mutable {
      jsg::AsyncContextFrame::Scope asyncScope(lock, maybeAsyncContext);
      jsg::Lock& js = lock;

      if constexpr (jsg::isVoid<T>()) {
        KJ_IF_SOME(e, exceptionOrT) {
          // We don't use `resolver.reject()` here because if we convert the kj::Exception into
          // a JS Error here, it won't have a useful stack trace. V8 can generate a good stack
          // trace as long as we construct the Error inside of a promise continuation, so we use
          // a `.then()` below that acutally extracts the kj::Exception and turn it into a JS
          // Error.
          resolver.resolve(kj::mv(e));
        } else {
          try {
            js.tryCatch([&]() {
              if constexpr (jsg::isVoid<Result>()) {
                func(js);
                resolver.resolve(js, kj::none);
              } else {
                resolver.resolve(js, func(js));
              }
            }, [&](jsg::Value error) {
              // Here we can just `resolver.reject` because we already have a JS exception.
              resolver.reject(js, error.getHandle(js));
            });
          } catch (...) {
            // Again, pass along the KJ exception so we can convert it later in the right context.
            resolver.resolve(js, kj::getCaughtExceptionAsKj());
          }
        }
      } else {
        // T is not void.
        KJ_SWITCH_ONEOF(exceptionOrT) {
          KJ_CASE_ONEOF(exception, kj::Exception) {
            // Again, pass along the KJ exception so we can convert it later in the right context.
            resolver.resolve(js, kj::mv(exception));
          }
          KJ_CASE_ONEOF(result, T) {
            try {
              js.tryCatch([&]() {
                if constexpr (jsg::isVoid<Result>()) {
                  func(js, kj::mv(result));
                  resolver.resolve(js, kj::none);
                } else {
                  // Here we can just `resolver.reject` because we already have a JS exception.
                  resolver.resolve(js, func(js, kj::mv(result)));
                }
              }, [&](jsg::Value error) {
                resolver.reject(js, error.getHandle(js));
              });
            } catch (...) {
              // Again, pass along the KJ exception so we can convert it later in the right context.
              resolver.resolve(js, kj::getCaughtExceptionAsKj());
            }
          }
        }
      }
    }, kj::mv(ilOrCs));
  }));

  // Reminder: This can throw JsExceptionThrown if the execution context has been termintaed. We
  // have already disowned `promise` and `func` by this point, though, so teardown order is no
  // longer our concern.
  return jsPromise.then(js, [](jsg::Lock& js, ExceptionOr<Result>&& exceptionOrResult) -> Result {
    if constexpr (jsg::isVoid<Result>()) {
      KJ_IF_SOME(e, exceptionOrResult) {
        // Now that we're in a promise continuation, we can convert the error and get a good stack
        // trace.
        js.throwException(kj::mv(e));
      }
      return;
    } else {
      KJ_SWITCH_ONEOF(exceptionOrResult) {
        KJ_CASE_ONEOF(e, kj::Exception) {
          // Now that we're in a promise continuation, we can convert the error and get a good stack
          // trace.
          js.throwException(kj::mv(e));
        }
        KJ_CASE_ONEOF(result, Result) {
          return kj::mv(result);
        }
      }
      KJ_UNREACHABLE;
    }
  });
}

template <typename T>
kj::_::ReducePromises<RemoveIoOwn<T>> IoContext::awaitJs(jsg::Lock& js, jsg::Promise<T> jsPromise) {
  auto paf = kj::newPromiseAndFulfiller<RemoveIoOwn<T>>();
  struct RefcountedFulfiller: public Finalizeable, public kj::Refcounted {
    kj::Own<kj::PromiseFulfiller<RemoveIoOwn<T>>> fulfiller;
    bool isDone = false;

    RefcountedFulfiller(kj::Own<kj::PromiseFulfiller<RemoveIoOwn<T>>> fulfiller)
        : fulfiller(kj::mv(fulfiller)) {}

    ~RefcountedFulfiller() noexcept(false) {
      if (!isDone) {
        // The JavaScript resolver was garbage collected, i.e. JavaScript will never resolve
        // this promise.
        fulfiller->reject(
            JSG_KJ_EXCEPTION(FAILED, Error, "Promise will never complete."));
      }
    }

  private:
    kj::Maybe<kj::StringPtr> finalize() override {
      if (!isDone) {
        fulfiller->reject(JSG_KJ_EXCEPTION(FAILED,
            Error, "Promise will never complete."));
        isDone = true;
        return "A hanging Promise was canceled. This happens when the worker runtime is waiting "
                "for a Promise from JavaScript to resolve, but has detected that the Promise "
                "cannot possibly ever resolve because all code and events related to the "
                "Promise's I/O context have already finished."_kj;
      } else {
        return kj::none;
      }
    }
  };
  auto fulfiller = kj::refcounted<RefcountedFulfiller>(kj::mv(paf.fulfiller));

  auto errorHandler =
      [fulfiller = addObject(kj::addRef(*fulfiller))]
      (jsg::Lock& js, jsg::Value jsExceptionRef) mutable {
    // Note: `context` can possibly be different than the one that started the wait, if the
    // promise resolved from a different context. In that case the use of `fulfiller` will
    // throw later on. But it's OK to use the wrong context up until that point.
    auto& context = IoContext::current();

    auto isolate = context.getCurrentLock().getIsolate();
    auto jsException = jsExceptionRef.getHandle(js);

    // TODO(someday): We log an "uncaught exception" here whenever a promise returned from JS to
    //   C++ rejects. However, the C++ code waiting on the promise may do its own logging (e.g.
    //   event.respondWith() does), in which case this is redundant. But, it's difficult to be
    //   sure that all C++ consumers log properly, and even if they do, the stack trace is lost
    //   once the exception has been tunneled into a KJ exception, so the later logging won't be
    //   as useful. We should improve the tunneling to include stack traces and ensure that all
    //   consumers do in fact log exceptions, then we can remove this.
    context.logUncaughtException(UncaughtExceptionSource::INTERNAL_ASYNC,
                                 jsg::JsValue(jsException));

    fulfiller->fulfiller->reject(jsg::createTunneledException(isolate, jsException));
    fulfiller->isDone = true;
  };

  if constexpr (jsg::isVoid<T>()) {
    jsPromise.then(js, [fulfiller = addObject(kj::mv(fulfiller))](jsg::Lock&) mutable {
      fulfiller->fulfiller->fulfill();
      fulfiller->isDone = true;
    }, kj::mv(errorHandler));
  } else {
    jsPromise.then(js, [fulfiller = addObject(kj::mv(fulfiller))]
                    (jsg::Lock&, T&& result) mutable {
      if constexpr (isIoOwn<T>()) {
        fulfiller->fulfiller->fulfill(kj::mv(*result));
      } else {
        fulfiller->fulfiller->fulfill(kj::mv(result));
      }
      fulfiller->isDone = true;
    }, kj::mv(errorHandler));
  }

  return kj::mv(paf.promise);
}

template <typename T>
inline IoOwn<T> IoContext::addObject(kj::Own<T> obj) {
  requireCurrent();
  return deleteQueue->addObject(kj::mv(obj), ownedObjects);
}

template <typename T>
inline IoPtr<T> IoContext::addObject(T& obj) {
  requireCurrent();
  return IoPtr<T>(kj::atomicAddRef(*deleteQueue), &obj);
}

template <typename Func>
auto IoContext::addFunctor(Func&& func) {
  if constexpr (kj::isReference<Func>()) {
    return [func = addObject(func)](auto&&... params) mutable {
      return (*func)(kj::fwd<decltype(params)>(params)...);
    };
  } else {
    return [func = addObject(kj::heap(kj::mv(func)))](auto&&... params) mutable {
      return (*func)(kj::fwd<decltype(params)>(params)...);
    };
  }
}

template <typename Func>
jsg::PromiseForResult<Func, void, true> IoContext::blockConcurrencyWhile(
    jsg::Lock& js, Func&& callback) {
  auto lock = getInputLock();
  auto cs = lock.startCriticalSection();
  auto cs2 = kj::addRef(*cs);

  typedef jsg::RemovePromise<jsg::PromiseForResult<Func, void, true>> T;
  auto [result, resolver] = js.newPromiseAndResolver<T>();

  addTask(cs->wait().then([this, callback = kj::mv(callback),
                           maybeAsyncContext = jsg::AsyncContextFrame::currentRef(js)]
                          (InputGate::Lock inputLock) mutable {
    return run([this, callback = kj::mv(callback),
                maybeAsyncContext = kj::mv(maybeAsyncContext)](Worker::Lock& lock) mutable {
      jsg::AsyncContextFrame::Scope scope(lock, maybeAsyncContext);
      auto cb = kj::mv(callback);

      // Remember that this can throw synchronously, and it's important that we catch such throws
      // and call cs->failed().
      auto promise = cb(lock);

      // Arrange to time out if the critical section runs more than 30 seconds, so that objects
      // won't be hung forever if they have a critical section that deadlocks.
      auto timeout = afterLimitTimeout(30 * kj::SECONDS)
        .then([]() -> T {
          kj::throwFatalException(JSG_KJ_EXCEPTION(
              OVERLOADED, Error,
              "A call to blockConcurrencyWhile() in a Durable Object waited for "
              "too long. The call was canceled and the Durable Object was reset."));
      });

      return awaitJs(lock, kj::mv(promise)).exclusiveJoin(kj::mv(timeout));
    }, kj::mv(inputLock));
  }).then([this, cs = kj::mv(cs), resolver = kj::mv(resolver),
           maybeAsyncContext = jsg::AsyncContextFrame::currentRef(js)](T&& value) mutable {
    auto inputLock = cs->succeeded();
    return run([value = kj::mv(value),resolver = kj::mv(resolver),
                maybeAsyncContext = kj::mv(maybeAsyncContext)](Worker::Lock& lock) mutable {
      jsg::AsyncContextFrame::Scope scope(lock, maybeAsyncContext);
      resolver.resolve(kj::mv(value));
    }, kj::mv(inputLock));
  }, [cs = kj::mv(cs2)]
     (kj::Exception&& e) mutable {
    // Annotate as broken for periodic metrics.
    auto msg = e.getDescription();
    if (!msg.startsWith("broken."_kj) && !msg.startsWith("remote.broken."_kj)) {
      // If we already set up a brokeness reason, we shouldn't override it.

      auto description = jsg::annotateBroken(msg, "broken.inputGateBroken");
      e.setDescription(kj::mv(description));
    }

    // Note that on failure, no further InputLocks will be obtainable and the actor will
    // shut down, so don't worry about holding a lock until we get back to application code --
    // we won't! In fact, we don't even bother calling resolver.reject() because it's meaningless
    // at this point.
    cs->failed(e);

    kj::throwFatalException(kj::mv(e));
  }));

  return kj::mv(result);
}

}  // namespace workerd
