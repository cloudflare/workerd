// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "io-context.h"

#include <workerd/io/io-gate.h>
#include <workerd/io/tracer.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/util/autogate.h>
#include <workerd/util/own-util.h>
#include <workerd/util/sentry.h>
#include <workerd/util/uncaught-exception-source.h>

#include <kj/debug.h>

#include <cmath>
#include <map>

namespace workerd {

static thread_local IoContext* threadLocalRequest = nullptr;

SuppressIoContextScope::SuppressIoContextScope(): cached(threadLocalRequest) {
  threadLocalRequest = nullptr;
}

SuppressIoContextScope::~SuppressIoContextScope() noexcept(false) {
  threadLocalRequest = cached;
}

static const kj::EventLoopLocal<int> threadId;

static void* getThreadId() {
  return threadId.get();
}

class IoContext::TimeoutManagerImpl final: public TimeoutManager {
 public:
  class TimeoutState;
  using Map = std::map<TimeoutId, TimeoutState>;
  using Iterator = Map::iterator;

  TimeoutManagerImpl() = default;
  KJ_DISALLOW_COPY_AND_MOVE(TimeoutManagerImpl);

  TimeoutId setTimeout(
      IoContext& context, TimeoutId::Generator& generator, TimeoutParameters params) override {
    // Verify the generator is from the correct ServiceWorkerGlobalScope. If we have been passed a
    // different `timeoutIdGenerator`, then that means this IoContext is active at a time when
    // JavaScript in a different V8 context is executing. This _should_ be impossible, but we're
    // occasionally seeing timeout ID collision assertion failures in `addState()`, and one possible
    // explanation is that an IoContext is somehow current for a different V8 context.
    //
    // TODO(cleanup): Find a more general way to assert that the JS API surface is being used under
    //   the correct IoContext, get rid of this function's `generator` parameter, and instead rely
    //   on the IoContext to provide the generator.
    KJ_ASSERT(&generator == &context.getCurrentLock().getTimeoutIdGenerator(),
        "TimeoutId Generator mismatch - using a generator from wrong ServiceWorkerGlobalScope");

    auto [id, it] = addState(generator, kj::mv(params));
    setTimeoutImpl(context, it);
    return id;
  }

  void clearTimeout(IoContext&, TimeoutId id) override;

  size_t getTimeoutCount() const override {
    return timeoutsStarted - timeoutsFinished;
  }

  kj::Maybe<kj::Date> getNextTimeout() const override {
    if (timeoutTimes.size() == 0) {
      return kj::none;
    } else {
      return timeoutTimes.begin()->key.when;
    }
  }

  void cancelAll() override {
    timerTask = nullptr;
    timeouts.clear();
    timeoutTimes.clear();
  }

 private:
  struct IdAndIterator {
    TimeoutId id;
    Iterator it;
  };
  IdAndIterator addState(TimeoutId::Generator& generator, TimeoutParameters params);

  void setTimeoutImpl(IoContext& context, Iterator it);

  // A pair of a Date and a numeric ID, used as entry in timeoutTimes set, below.
  struct TimeoutTime {
    kj::Date when;
    uint tiebreaker;  // Unique number, in case two timeouts target same time.

    inline bool operator<(const TimeoutTime& other) const {
      if (when < other.when) return true;
      if (when > other.when) return false;
      return tiebreaker < other.tiebreaker;
    }
    inline bool operator==(const TimeoutTime& other) const {
      return when == other.when && tiebreaker == other.tiebreaker;
    }
  };

  // Tracks registered timeouts sorted by the next time the timeout is expected to fire.
  //
  // The associated fulfiller should be fulfilled when the time has been reached AND all previous
  // timeouts have completed.
  kj::TreeMap<TimeoutTime, kj::Own<kj::PromiseFulfiller<void>>> timeoutTimes;
  uint timeoutTimesTiebreakerCounter = 0;

  uint timeoutsStarted = 0;
  uint timeoutsFinished = 0;
  Map timeouts;

  // Promise that is waiting for the closest timeout, and will fulfill its fulfiller. We only ever
  // actually wait on the next timeout in `timeoutTasks`, so that we can't fulfill timer callbacks
  // out-of-order. This task gets replaced each time the lead timeout changes.
  kj::Promise<void> timerTask = nullptr;

  // Must be called any time timeoutTimes.begin() changes.
  void resetTimerTask(TimerChannel& timerChannel);
};

class IoContext::TimeoutManagerImpl::TimeoutState {
 public:
  TimeoutState(TimeoutManagerImpl& manager, TimeoutParameters params);
  ~TimeoutState();

  void trigger(Worker::Lock& lock);
  void cancel();

  TimeoutManagerImpl& manager;
  TimeoutParameters params;

  bool isCanceled = false;
  bool isRunning = false;

  kj::Maybe<kj::Promise<void>> maybePromise;
};

IoContext::IoContext(ThreadContext& thread,
    kj::Own<const Worker> workerParam,
    kj::Maybe<Worker::Actor&> actorParam,
    kj::Own<LimitEnforcer> limitEnforcerParam)
    : thread(thread),
      worker(kj::mv(workerParam)),
      actor(actorParam),
      limitEnforcer(kj::mv(limitEnforcerParam)),
      threadId(getThreadId()),
      deleteQueue(kj::arc<DeleteQueue>()),
      cachePutSerializer(kj::READY_NOW),
      waitUntilTasks(*this),
      tasks(*this),
      timeoutManager(kj::heap<TimeoutManagerImpl>()),
      deleteQueueSignalTask(startDeleteQueueSignalTask(this)) {
  kj::PromiseFulfillerPair<void> paf = kj::newPromiseAndFulfiller<void>();
  abortFulfiller = kj::mv(paf.fulfiller);
  abortPromise = paf.promise.fork();

  // Arrange to complain if execution resource limits (CPU/memory) are exceeded.
  auto makeLimitsPromise = [this]() {
    auto promise = limitEnforcer->onLimitsExceeded();
    if (isInspectorEnabled()) {
      // Arrange to report the problem to the inspector in addition to aborting.
      // TODO(cleanup): This is weird. Should it go somewhere else?
      promise = (kj::coCapture([this, promise = kj::mv(promise)]() mutable -> kj::Promise<void> {
        kj::Maybe<kj::Exception> maybeException;
        try {
          co_await promise;
        } catch (...) {
          // Just capture the exception here, we'll handle is below since we cannot have
          // a co_await in the body of a catch clause.
          maybeException = kj::getCaughtExceptionAsKj();
        }

        KJ_IF_SOME(exception, maybeException) {
          Worker::AsyncLock asyncLock = co_await worker->takeAsyncLockWithoutRequest(nullptr);
          worker->runInLockScope(asyncLock, [&](Worker::Lock& lock) {
            lock.logUncaughtException(
                jsg::extractTunneledExceptionDescription(exception.getDescription()));
            kj::throwFatalException(kj::mv(exception));
          });
        }
      }))();
    }

    return promise;
  };
  KJ_IF_SOME(cb, this->worker->getIsolate().getCpuLimitNearlyExceededCallback()) {
    limitEnforcer->setCpuLimitNearlyExceededCallback(kj::mv(cb));
  }

  // Arrange to abort when limits expire.
  abortWhen(makeLimitsPromise());

  KJ_IF_SOME(a, actor) {
    // Arrange to complain if the input gate is broken, which indicates a critical section failed
    // and the actor can no longer be used.
    abortWhen(a.getInputGate().onBroken());

    // Also complain if the output gate is broken, which indicates a critical storage failure that
    // means we cannot continue execution. (In fact, we need to retroactively pretend that previous
    // execution didn't happen, but that is taken care of elsewhere.)
    abortWhen(a.getOutputGate().onBroken());
  }
}

IoContext::IncomingRequest::IoContext_IncomingRequest(kj::Own<IoContext> contextParam,
    kj::Own<IoChannelFactory> ioChannelFactoryParam,
    kj::Own<RequestObserver> metricsParam,
    kj::Maybe<kj::Own<BaseTracer>> workerTracer,
    kj::Maybe<tracing::InvocationSpanContext> maybeTriggerInvocationSpan)
    : context(kj::mv(contextParam)),
      metrics(kj::mv(metricsParam)),
      workerTracer(kj::mv(workerTracer)),
      ioChannelFactory(kj::mv(ioChannelFactoryParam)),
      maybeTriggerInvocationSpan(kj::mv(maybeTriggerInvocationSpan)) {}

tracing::InvocationSpanContext& IoContext::IncomingRequest::getInvocationSpanContext() {
  // Creating a new InvocationSpanContext can be a bit expensive since it needs to
  // generate random IDs, so we only create it lazily when requested, which should
  // only be when tracing is enabled and we need to record spans.
  KJ_IF_SOME(ctx, invocationSpanContext) {
    return ctx;
  }

  invocationSpanContext = tracing::InvocationSpanContext::newForInvocation(
      maybeTriggerInvocationSpan.map(
          [](auto& trigger) -> tracing::InvocationSpanContext& { return trigger; }),
      context->getEntropySource());
  return KJ_ASSERT_NONNULL(invocationSpanContext);
}

// A call to delivered() implies a promise to call drain() later (or one of the other methods
// that sets waitedForWaitUntil). So, we can now safely add the request to
// context->incomingRequests, which implies taking responsibility for draining on the way out.
void IoContext::IncomingRequest::delivered(kj::SourceLocation location) {
  KJ_REQUIRE(!wasDelivered, "delivered() can only be called once");
  if (!context->incomingRequests.empty()) {
    // There is already an IncomingRequest running in this context, and we're going to make it no
    // longer current. Make sure to attribute accumulated CPU time to it.
    auto& oldFront = context->incomingRequests.front();
    context->limitEnforcer->reportMetrics(*oldFront.metrics);

    KJ_IF_SOME(f, oldFront.drainFulfiller) {
      // Allow the previous current IncomingRequest to finish draining, because the new request
      // will take over responsibility for completing any tasks that aren't done yet.
      f.get()->fulfill();
    }
  }

  context->incomingRequests.addFront(*this);
  wasDelivered = true;
  deliveredLocation = location;
  metrics->delivered();

  KJ_IF_SOME(workerTracer, workerTracer) {
    currentUserTraceSpan = workerTracer->makeUserRequestSpan();
  }

  KJ_IF_SOME(a, context->actor) {
    // Re-synchronize the timer and top up limits for every new incoming request to an actor.
    ioChannelFactory->getTimer().syncTime();
    context->limitEnforcer->topUpActor();

    // Run the Actor's constructor if it hasn't been run already.
    a.ensureConstructed(*context);

    // Record a new incoming request to actor metrics.
    a.getMetrics().startRequest();
  }
}

kj::Date IoContext::IncomingRequest::now(kj::Maybe<kj::Date> nextTimeout) {
  metrics->clockRead();
  return ioChannelFactory->getTimer().now(kj::mv(nextTimeout));
}

IoContext::IncomingRequest::~IoContext_IncomingRequest() noexcept(false) {
  if (!wasDelivered) {
    KJ_IF_SOME(w, workerTracer) {
      w->markUnused();
    }
    // Request was never added to context->incomingRequests in the first place.
    return;
  }

  // Hack: We need to report an accurate time stamps for the STW outcome event, but the timer may
  // not be available when the outcome event gets reported. Define the outcome event time as the
  // time when the incoming request shuts down.
  KJ_IF_SOME(w, workerTracer) {
    w->recordTimestamp(now());
  }

  if (&context->incomingRequests.front() == this) {
    // We're the current request, make sure to consume CPU time attribution.
    context->limitEnforcer->reportMetrics(*metrics);
    context->lastDeliveredLocation = deliveredLocation;

    if (!waitedForWaitUntil && !context->waitUntilTasks.isEmpty()) {
      KJ_LOG(WARNING, "failed to invoke drain() on IncomingRequest before destroying it",
          kj::getStackTrace());
    }
  }

  KJ_IF_SOME(a, context->actor) {
    a.getMetrics().endRequest();
  }
  context->worker->getIsolate().completedRequest();
  metrics->jsDone();

  if (context->isShared()) {
    // This context is not about to be destroyed when we drop it, but if it was aborted, we would
    // prefer for it to get cleaned up promptly.

    KJ_IF_SOME(e, context->abortException) {
      // The context was aborted. It's possible that the event ended with background work still
      // scheduled, because `drain()` ends early on abort. We should cancel that background work
      // now.
      //
      // We couldn't do this in abort() because it can be called from inside a task that could
      // be canceled, and a self-cancellation would lead to a crash.

      if (!context->canceler.isEmpty()) {
        context->canceler.cancel(e);
      }
      context->timeoutManager->cancelAll();
      context->tasks.clear();
      context->waitUntilTasks.clear();
    }
  }

  // Remove incoming request after canceling waitUntil tasks, which may have spans attached that
  // require accessing a timer from the active request.
  context->incomingRequests.remove(*this);
}

InputGate::Lock IoContext::getInputLock() {
  return KJ_ASSERT_NONNULL(currentInputLock, "no input lock available in this context")
      .addRef(getCurrentTraceSpan());
}

kj::Maybe<kj::Own<InputGate::CriticalSection>> IoContext::getCriticalSection() {
  KJ_IF_SOME(l, currentInputLock) {
    return l.getCriticalSection().map(
        [](InputGate::CriticalSection& cs) { return kj::addRef(cs); });
  } else {
    return kj::none;
  }
}

kj::Promise<void> IoContext::waitForOutputLocks() {
  KJ_IF_SOME(p, waitForOutputLocksIfNecessary()) {
    return kj::mv(p);
  } else {
    return kj::READY_NOW;
  }
}

bool IoContext::hasOutputGate() {
  return actor != kj::none;
}

kj::Maybe<kj::Promise<void>> IoContext::waitForOutputLocksIfNecessary() {
  return actor.map(
      [this](Worker::Actor& actor) { return actor.getOutputGate().wait(getCurrentTraceSpan()); });
}

kj::Maybe<IoOwn<kj::Promise<void>>> IoContext::waitForOutputLocksIfNecessaryIoOwn() {
  return waitForOutputLocksIfNecessary().map(
      [this](kj::Promise<void> promise) { return addObject(kj::heap(kj::mv(promise))); });
}

bool IoContext::isOutputGateBroken() {
  KJ_IF_SOME(a, actor) {
    return a.getOutputGate().isBroken();
  } else {
    return false;
  }
}

bool IoContext::isInspectorEnabled() {
  return worker->getIsolate().isInspectorEnabled();
}

bool IoContext::isFiddle() {
  return thread.isFiddle();
}

bool IoContext::hasWarningHandler() {
  return isInspectorEnabled() || getWorkerTracer() != kj::none ||
      ::kj::_::Debug::shouldLog(::kj::LogSeverity::INFO);
}

void IoContext::logWarning(kj::StringPtr description) {
  KJ_REQUIRE_NONNULL(currentLock).logWarning(description);
}

void IoContext::logWarningOnce(kj::StringPtr description) {
  KJ_REQUIRE_NONNULL(currentLock).logWarningOnce(description);
}

void IoContext::logErrorOnce(kj::StringPtr description) {
  KJ_REQUIRE_NONNULL(currentLock).logErrorOnce(description);
}

void IoContext::logUncaughtException(kj::StringPtr description) {
  KJ_REQUIRE_NONNULL(currentLock).logUncaughtException(description);
}

void IoContext::logUncaughtException(
    UncaughtExceptionSource source, const jsg::JsValue& exception, const jsg::JsMessage& message) {
  KJ_REQUIRE_NONNULL(currentLock).logUncaughtException(source, exception, message);
}

void IoContext::logUncaughtExceptionAsync(
    UncaughtExceptionSource source, kj::Exception&& exception) {
  if (getWorkerTracer() == kj::none && !worker->getIsolate().isInspectorEnabled()) {
    // We don't need to take the isolate lock as neither inspecting nor tracing is enabled. We
    // do still want to syslog if relevant, but we can do that without a lock.
    if (!jsg::isTunneledException(exception.getDescription()) &&
        !jsg::isDoNotLogException(exception.getDescription()) &&
        // TODO(soon): Figure out why client disconnects are getting logged here if we don't
        // ignore DISCONNECTED. If we fix that, do we still want to filter these?
        exception.getType() != kj::Exception::Type::DISCONNECTED) {
      LOG_EXCEPTION("jsgInternalError", exception);
    } else {
      KJ_LOG(INFO, "uncaught exception", exception);  // Run with --verbose to see exception logs.
    }
    return;
  }

  struct RunnableImpl: public Runnable {
    UncaughtExceptionSource source;
    kj::Exception exception;

    RunnableImpl(UncaughtExceptionSource source, kj::Exception&& exception)
        : source(source),
          exception(kj::mv(exception)) {}
    void run(Worker::Lock& lock) override {
      // TODO(soon): Add logUncaughtException to jsg::Lock.
      lock.logUncaughtException(source, kj::mv(exception));
    }
  };

  // Make sure this is logged even if another exception occurs trying to log it to the devtools inspector,
  // e.g. if `runImpl` throws before calling logUncaughtException.
  // This is useful for tests (and in fact only affects tests, since it's logged at an INFO level).
  KJ_ON_SCOPE_FAILURE({ KJ_LOG(INFO, "uncaught exception", source, exception); });
  RunnableImpl runnable(source, kj::mv(exception));
  // TODO(perf): Is it worth using an async lock here? The only case where it really matters is
  //   when a trace worker is active, but maybe they'll be more common in the future. To take an
  //   async lock here, we'll probably have to update all the call sites of this method... ick.
  kj::Maybe<RequestObserver&> metrics;
  if (!incomingRequests.empty()) metrics = getMetrics();
  runImpl(
      runnable, Worker::Lock::TakeSynchronously(metrics), kj::none, Runnable::Exceptional(true));
}

void IoContext::abort(kj::Exception&& e) {
  if (abortException != kj::none) {
    return;
  }
  abortException = kj::cp(e);
  KJ_IF_SOME(a, actor) {
    // Stop the ActorCache from flushing any scheduled write operations to prevent any unnecessary
    // or unintentional async work
    a.shutdownActorCache(kj::cp(e));
  }
  abortFulfiller->reject(kj::mv(e));
}

void IoContext::abortWhen(kj::Promise<void> promise) {
  // Unlike addTask(), abortWhen() always uses `tasks`, even in actors, because we do not want
  // these tasks to block hibernation.
  if (abortException == kj::none) {
    tasks.add(promise.catch_([this](kj::Exception&& e) { abort(kj::mv(e)); }));
  }
}

void IoContext::addTask(kj::Promise<void> promise) {
  ++addTaskCounter;

  // In Actors, we treat all tasks as wait-until tasks, because it's perfectly legit to start a
  // task under one request and then expect some other request to handle it later.
  if (actor != kj::none) {
    addWaitUntil(kj::mv(promise));
    return;
  }

  if (actor == kj::none) {
    // This metric won't work correctly in actors since it's being tracked per-request, but tasks
    // are not tied to requests in actors. So we just skip it in actors. (Actually this code path
    // is not even executed in the actor case but I'm leaving the check in just in case that ever
    // changes.)
    auto& metrics = getMetrics();
    if (metrics.getSpan().isObserved()) {
      promise = promise.attach(metrics.addedContextTask());
    }
  }

  tasks.add(kj::mv(promise));
}

void IoContext::addWaitUntil(kj::Promise<void> promise) {
  if (actor == kj::none) {
    // This metric won't work correctly in actors since it's being tracked per-request, but tasks
    // are not tied to requests in actors. So we just skip it in actors.
    auto& metrics = getMetrics();
    if (metrics.getSpan().isObserved()) {
      promise = promise.attach(metrics.addedWaitUntilTask());
    }
  }

  if (incomingRequests.empty()) {
    DEBUG_FATAL_RELEASE_LOG(WARNING, "Adding task to IoContext with no current IncomingRequest",
        lastDeliveredLocation, kj::getStackTrace());
  }

  waitUntilTasks.add(kj::mv(promise));
}

// Mark ourselves so we know that we made a best effort attempt to wait for waitUntilTasks.
kj::Promise<void> IoContext::IncomingRequest::drain() {
  waitedForWaitUntil = true;

  if (&context->incomingRequests.front() != this) {
    // A newer request was received, so draining isn't our job.
    return kj::READY_NOW;
  }

  kj::Promise<void> timeoutPromise = nullptr;
  KJ_IF_SOME(a, context->actor) {
    // For actors, all promises are canceled on actor shutdown, not on a fixed timeout,
    // because work doesn't necessarily happen on a per-request basis in actors and we don't want
    // work being unexpectedly canceled based on which request initiated it.
    timeoutPromise = a.onShutdown();

    // Also arrange to cancel the drain if a new request arrives, since it will take over
    // responsibility for background tasks.
    auto drainPaf = kj::newPromiseAndFulfiller<void>();
    drainFulfiller = kj::mv(drainPaf.fulfiller);
    timeoutPromise = timeoutPromise.exclusiveJoin(kj::mv(drainPaf.promise));
  } else {
    // For non-actor requests, apply the configured soft timeout, typically 30 seconds.
    auto timeoutLogPromise = [this]() -> kj::Promise<void> {
      return context->run([this](Worker::Lock&) {
        context->logWarning(
            "waitUntil() tasks did not complete within the allowed time after invocation end and have been cancelled. "
            "See: https://developers.cloudflare.com/workers/runtime-apis/context/#waituntil");
      });
    };
    timeoutPromise = context->limitEnforcer->limitDrain().then(kj::mv(timeoutLogPromise));
  }
  return context->waitUntilTasks.onEmpty()
      .exclusiveJoin(kj::mv(timeoutPromise))
      .exclusiveJoin(context->onAbort().catch_([](kj::Exception&&) {}));
}

kj::Promise<IoContext_IncomingRequest::FinishScheduledResult> IoContext::IncomingRequest::
    finishScheduled() {
  // TODO(someday): In principle we should be able to support delivering the "scheduled" event type
  //   to an actor, and this may be important if we open up the whole of WorkerInterface to be
  //   callable from any stub. However, the logic around async tasks would have to be different. We
  //   cannot assume that just because an async task fails while the scheduled event is running,
  //   that the scheduled event itself failed -- the failure could have been a task initiated by
  //   an unrelated concurrent event.
  KJ_ASSERT(context->actor == kj::none,
      "this code isn't designed to allow scheduled events to be delivered to actors");

  // Mark ourselves so we know that we made a best effort attempt to wait for waitUntilTasks.
  KJ_ASSERT(context->incomingRequests.size() == 1);
  context->incomingRequests.front().waitedForWaitUntil = true;

  auto timeoutPromise = context->limitEnforcer->limitScheduled().then(
      [] { return IoContext_IncomingRequest::FinishScheduledResult::TIMEOUT; });
  return context->waitUntilTasks.onEmpty()
      .then([]() { return IoContext_IncomingRequest::FinishScheduledResult::COMPLETED; })
      .exclusiveJoin(kj::mv(timeoutPromise))
      .exclusiveJoin(context->onAbort().then([] {
    return IoContext_IncomingRequest::FinishScheduledResult::ABORTED;
  }, [](kj::Exception&&) { return IoContext_IncomingRequest::FinishScheduledResult::ABORTED; }));
}

class IoContext::PendingEvent: public kj::Refcounted {
 public:
  explicit PendingEvent(IoContext& context): maybeContext(context) {}
  ~PendingEvent() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(PendingEvent);

  kj::Maybe<IoContext&> maybeContext;
};

IoContext::~IoContext() noexcept(false) {
  if (!canceler.isEmpty()) {
    KJ_IF_SOME(e, abortException) {
      // Assume the abort exception is why we are canceling.
      canceler.cancel(e);
    } else {
      canceler.cancel(JSG_KJ_EXCEPTION(
          FAILED, Error, "The execution context responding to this call was canceled."));
    }
  }

  // Detach the PendingEvent if it still exists.
  KJ_IF_SOME(pe, pendingEvent) {
    pe.maybeContext = kj::none;
  }

  // Kill the sentinel so that no weak references can refer to this IoContext anymore.
  selfRef->invalidate();
}

IoContext::PendingEvent::~PendingEvent() noexcept(false) {
  IoContext& context = KJ_UNWRAP_OR(maybeContext, {
    // IoContext must have been destroyed before the PendingEvent was.
    return;
  });

  context.pendingEvent = kj::none;

  // We can't abort just yet. We need to run the event loop to see if any queued
  // events come back into JavaScript. If registerPendingEvent() is called in the meantime, this
  // will be canceled.
  context.abortFromHangTask = Worker::AsyncLock::whenThreadIdle()
                                  .then([&context = context]() noexcept {
    // We have nothing left to do and no PendingEvent has been registered. Abort now.
    return context.worker->takeAsyncLock(context.getMetrics())
        .then([&context](Worker::AsyncLock asyncLock) { context.abortFromHang(asyncLock); });
  }).eagerlyEvaluate(nullptr);
}

kj::Own<void> IoContext::registerPendingEvent() {
  if (actor != kj::none) {
    // Actors don't use the pending event system, because different requests to the same Actor are
    // explicitly allowed to resolve each other's promises.
    return {};
  }

  KJ_IF_SOME(pe, pendingEvent) {
    return kj::addRef(pe);
  } else {
    KJ_IF_SOME(e, abortException) {
      kj::throwFatalException(kj::cp(e));
    }

    // Cancel any already-scheduled finalization.
    abortFromHangTask = kj::none;

    auto result = kj::refcounted<PendingEvent>(*this);
    pendingEvent = *result;
    return result;
  }
}

IoContext::TimeoutManagerImpl::TimeoutState::TimeoutState(
    TimeoutManagerImpl& manager, TimeoutParameters params)
    : manager(manager),
      params(kj::mv(params)) {
  ++manager.timeoutsStarted;
}

IoContext::TimeoutManagerImpl::TimeoutState::~TimeoutState() {
  KJ_ASSERT(!isRunning);
  if (!isCanceled) {
    ++manager.timeoutsFinished;
  }
}

void IoContext::TimeoutManagerImpl::TimeoutState::trigger(Worker::Lock& lock) {
  isRunning = true;
  auto cleanupGuard = kj::defer([&] { isRunning = false; });

  // Now it's safe to call the user's callback.
  KJ_IF_SOME(function, params.function) {
    (function)(lock);
  }
}

void IoContext::TimeoutManagerImpl::TimeoutState::cancel() {
  if (isCanceled) {
    return;
  }

  auto wasCanceled = isCanceled;
  isCanceled = true;

  if (!isRunning && !wasCanceled) {
    params.function = kj::none;
    maybePromise = kj::none;
  }

  ++manager.timeoutsFinished;
}

auto IoContext::TimeoutManagerImpl::addState(
    TimeoutId::Generator& generator, TimeoutParameters params) -> IdAndIterator {
  JSG_REQUIRE(getTimeoutCount() < MAX_TIMEOUTS, DOMQuotaExceededError,
      "You have exceeded the number of active timeouts you may set.",
      " max active timeouts: ", MAX_TIMEOUTS, ", current active timeouts: ", getTimeoutCount(),
      ", finished timeouts: ", timeoutsFinished);

  auto id = generator.getNext();
  auto [it, wasEmplaced] = timeouts.try_emplace(id, *this, kj::mv(params));
  if (!wasEmplaced) {
    // We shouldn't have reached here because the `TimeoutId::Generator` throws if it reaches
    // Number.MAX_SAFE_INTEGER, much less wraps around the uint64_t number space. Let's throw with
    // as many details as possible.
    auto& state = it->second;
    auto delay = state.params.msDelay;
    auto repeat = state.params.repeat;
    KJ_FAIL_ASSERT("Saw a timeout id collision", getTimeoutCount(), timeoutsStarted, id.toNumber(),
        delay, repeat);
  }

  return {id, it};
}

void IoContext::TimeoutManagerImpl::setTimeoutImpl(IoContext& context, Iterator it) {
  auto& state = it->second;

  auto stateGuard = kj::defer([&]() {
    if (state.maybePromise == kj::none) {
      // Something threw, erase the state.
      timeouts.erase(it);
    }
  });

  auto paf = kj::newPromiseAndFulfiller<void>();

  // Schedule relative to Date.now() so the delay appears exact to the application.
  auto when = context.now() + state.params.msDelay * kj::MILLISECONDS;
  // TODO(cleanup): The manual use of run() here (including carrying over the critical section) is
  //   kind of ugly, but using awaitIo() doesn't work here because we need the ability to cancel
  //   the timer, so we don't want to addTask() it, which awaitIo() does implicitly.
  auto promise =
      paf.promise.then([this, &context, it, cs = context.getCriticalSection()]() mutable {
    return context.run([this, &context, it](Worker::Lock& lock) mutable {
      auto& state = it->second;

      auto stateGuard = kj::defer([&] {
        if (state.maybePromise == kj::none) {
          // At the end of this block, there was no new timeout, so we should remove the state.
          // Note that this can happen from cancelTimeout or a non-repeating timeout.
          timeouts.erase(it);
        }
      });

      if (state.isCanceled) {
        // We've been canceled before running. Nothing more to do.
        KJ_ASSERT(state.maybePromise == kj::none);
        return;
      }

      KJ_IF_SOME(promise, state.maybePromise) {
        // We could KJ_ASSERT_NONNULL(iter->second) instead if we are sure clearTimeout() couldn't
        // race us. However, I'm not sure about that.

        // First, move our timeout promise to the task set so it's safe to call clearInterval()
        // inside the user's callback. We don't yet null out the Maybe<Promise>, because we need to
        // be able to detect whether the user does call clearInterval(). We leave the actual map
        // entry in place because this aids in reporting cross-request-context timeout cancellation
        // errors to the user.
        context.addTask(kj::mv(promise));

        // Because Promise has an underspecified move ctor, we need to explicitly nullify the Maybe
        // to indicate that we've consumed the promise.
        state.maybePromise = kj::none;

        // The user's callback might throw, but we need to at least attempt to reschedule interval
        // callbacks even if they throw. This deferred action takes care of that. Note that we don't
        // run the user's callback directly in this->run(), because that function throws a fatal
        // exception if a JS exception is thrown, which complicates our logic here.
        //
        // TODO(perf): If we can guarantee that `timeout->second = nullptr` will never throw, it
        //   might be worthwhile having an early-out path for non-interval timeouts.
        kj::UnwindDetector unwindDetector;
        KJ_DEFER(unwindDetector.catchExceptionsIfUnwinding([&] {
          if (state.isCanceled) {
            // The user's callback has called clearInterval(), nothing more to do.
            KJ_ASSERT(state.maybePromise == kj::none);
            return;
          }

          // If this is an interval task and the script has CPU time left, reschedule the task;
          // otherwise leave the dead map entry in place.
          if (state.params.repeat && context.limitEnforcer->getLimitsExceeded() == kj::none) {
            setTimeoutImpl(context, it);
          }
        }););

        state.trigger(lock);
      }
    }, kj::mv(cs));
  }, [](kj::Exception&&) {});

  promise = promise.attach(context.registerPendingEvent());

  // Add an entry to the timeoutTimes map, to track when the nearest timeout is. Arrange for it
  // to be removed when the promise completes.
  TimeoutTime timeoutTimesKey{when, timeoutTimesTiebreakerCounter++};
  timeoutTimes.insert(timeoutTimesKey, kj::mv(paf.fulfiller));
  auto deferredTimeoutTimeRemoval = kj::defer([this, &context, timeoutTimesKey]() {
    // If the promise is being destroyed due to IoContext teardown then IoChannelFactory may
    // no longer be available, but we can just skip starting a new timer in that case as it'd be
    // canceled anyway. Similarly we should skip rescheduling if the context has been aborted since
    // there's no way the events can run anyway (and we'll cause trouble if `cancelAll()` is being
    // called in ~IoContext_IncomingRequest).
    if (context.selfRef->isValid() && context.abortException == kj::none) {
      bool isNext = timeoutTimes.begin()->key == timeoutTimesKey;
      timeoutTimes.erase(timeoutTimesKey);
      if (isNext) resetTimerTask(context.getIoChannelFactory().getTimer());
    }
  });

  if (timeoutTimes.begin()->key == timeoutTimesKey) {
    resetTimerTask(context.getIoChannelFactory().getTimer());
  }
  promise = promise.attach(kj::mv(deferredTimeoutTimeRemoval));

  if (context.actor != kj::none) {
    // Add a wait-until task which resolves when this timer completes. This ensures that
    // `IncomingRequest::drain()` waits until all timers finish.
    auto paf = kj::newPromiseAndFulfiller<void>();
    promise = promise.attach(
        kj::defer([fulfiller = kj::mv(paf.fulfiller)]() mutable { fulfiller->fulfill(); }));
    context.addWaitUntil(kj::mv(paf.promise));
  }

  state.maybePromise = promise.eagerlyEvaluate(nullptr);
}

void IoContext::TimeoutManagerImpl::resetTimerTask(TimerChannel& timerChannel) {
  if (timeoutTimes.size() == 0) {
    // Not waiting for any timer, clear the existing timer task.
    timerTask = nullptr;
  } else {
    // Wait for the first timer.
    auto& entry = *timeoutTimes.begin();
    timerTask = timerChannel.atTime(entry.key.when)
                    .then([this, key = entry.key]() {
      auto& newEntry = *timeoutTimes.begin();
      KJ_ASSERT(newEntry.key == key,
          "front of timeoutTimes changed without calling resetTimerTask(), we probably missed "
          "a timeout!");
      newEntry.value->fulfill();
    }).eagerlyEvaluate([](kj::Exception&& e) { KJ_LOG(ERROR, e); });
  }
}

void IoContext::TimeoutManagerImpl::clearTimeout(IoContext& context, TimeoutId timeoutId) {
  auto timeout = timeouts.find(timeoutId);
  if (timeout == timeouts.end()) {
    // We can't find this timeout, thus we act as if it was already canceled.
    return;
  }

  // Cancel the timeout.
  timeout->second.cancel();
}

TimeoutId IoContext::setTimeoutImpl(
    TimeoutId::Generator& generator, bool repeat, jsg::Function<void()> function, double msDelay) {
  static constexpr int64_t max = 3153600000000;  // Milliseconds in 100 years
  // Clamp the range on timers to [0, 3153600000000] (inclusive). The specs
  // do not indicate a clear maximum range for setTimeout/setInterval so the
  // limit here is fairly arbitrary. 100 years max should be plenty safe.
  int64_t delay = msDelay <= 0 || std::isnan(msDelay) ? 0
      : msDelay >= static_cast<double>(max)           ? max
                                                      : static_cast<int64_t>(msDelay);
  auto params = TimeoutManager::TimeoutParameters(repeat, delay, kj::mv(function));
  return timeoutManager->setTimeout(*this, generator, kj::mv(params));
}

void IoContext::clearTimeoutImpl(TimeoutId id) {
  timeoutManager->clearTimeout(*this, id);
}

size_t IoContext::getTimeoutCount() {
  return timeoutManager->getTimeoutCount();
}

kj::Date IoContext::now(IncomingRequest& incomingRequest) {
  if (getWorker().getScript().getIsolate().getApi().getFeatureFlags().getPreciseTimers()) {
    auto now = kj::systemPreciseCalendarClock().now();
    // Round to 3ms granularity
    int64_t ms = (now - kj::UNIX_EPOCH) / kj::MILLISECONDS;
    int64_t roundedMs = (ms / 3) * 3;
    return kj::UNIX_EPOCH + roundedMs * kj::MILLISECONDS;
  }

  // Let TimerChannel decide whether to clamp to the next timeout time. This is how Spectre
  // mitigations ensure Date.now() inside a callback returns exactly the scheduled time.
  return incomingRequest.now(timeoutManager->getNextTimeout());
}

kj::Date IoContext::now() {
  return now(getCurrentIncomingRequest());
}

kj::Rc<ExternalPusherImpl> IoContext::getExternalPusher() {
  KJ_IF_SOME(ep, externalPusher) {
    return ep.addRef();
  } else {
    return externalPusher.emplace(kj::rc<ExternalPusherImpl>(getByteStreamFactory())).addRef();
  }
}

kj::Own<WorkerInterface> IoContext::getSubrequestNoChecks(
    kj::FunctionParam<kj::Own<WorkerInterface>(TraceContext&, IoChannelFactory&)> func,
    SubrequestOptions options) {
  TraceContext tracing;
  KJ_IF_SOME(n, options.operationName) {
    tracing = makeUserTraceSpan(n.clone());
  }

  kj::Own<WorkerInterface> ret;
  KJ_IF_SOME(existing, options.existingTraceContext) {
    ret = func(existing, getIoChannelFactory());
  } else {
    ret = func(tracing, getIoChannelFactory());
  }

  if (options.wrapMetrics) {
    auto& metrics = getMetrics();
    ret = metrics.wrapSubrequestClient(kj::mv(ret));
    ret = worker->getIsolate().wrapSubrequestClient(
        kj::mv(ret), getHeaderIds().contentEncoding, metrics);
  }

  if (tracing.isObserved()) {
    auto ioOwnedSpan = addObject(kj::heap(kj::mv(tracing)));
    ret = ret.attach(kj::mv(ioOwnedSpan));
  }

  // Subrequests use a lot of unaccounted C++ memory, so we adjust V8's external memory counter to
  // pressure the GC and protect against OOMs. When the autogate is enabled, we apply this
  // adjustment to ALL subrequests (not just fetch). We only apply this when the JS lock is held
  // (i.e., when JS code initiated the subrequest); infrastructure paths that bypass JS don't need
  // it.
  if (util::Autogate::isEnabled(util::AutogateKey::INCREASE_EXTERNAL_MEMORY_ADJUSTMENT_FOR_FETCH)) {
    KJ_IF_SOME(lock, currentLock) {
      jsg::Lock& js = lock;
      ret = ret.attach(js.getExternalMemoryAdjustment(8 * 1024));
    }
  }

  return kj::mv(ret);
}

kj::Own<WorkerInterface> IoContext::getSubrequest(
    kj::FunctionParam<kj::Own<WorkerInterface>(TraceContext&, IoChannelFactory&)> func,
    SubrequestOptions options) {
  limitEnforcer->newSubrequest(options.inHouse);
  return getSubrequestNoChecks(kj::mv(func), kj::mv(options));
}

kj::Own<WorkerInterface> IoContext::getSubrequestChannel(
    uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson, kj::ConstString operationName) {
  return getSubrequest(
      [&](TraceContext& tracing, IoChannelFactory& channelFactory) {
    return getSubrequestChannelImpl(
        channel, isInHouse, kj::mv(cfBlobJson), tracing, channelFactory);
  },
      SubrequestOptions{
        .inHouse = isInHouse,
        .wrapMetrics = !isInHouse,
        .operationName = kj::mv(operationName),
      });
}

kj::Own<WorkerInterface> IoContext::getSubrequestChannel(
    uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson, TraceContext& traceContext) {
  return getSubrequest(
      [&](TraceContext& tracing, IoChannelFactory& channelFactory) {
    return getSubrequestChannelImpl(
        channel, isInHouse, kj::mv(cfBlobJson), tracing, channelFactory);
  },
      SubrequestOptions{
        .inHouse = isInHouse,
        .wrapMetrics = !isInHouse,
        .existingTraceContext = traceContext,
      });
}

kj::Own<WorkerInterface> IoContext::getSubrequestChannelNoChecks(uint channel,
    bool isInHouse,
    kj::Maybe<kj::String> cfBlobJson,
    kj::Maybe<kj::ConstString> operationName) {
  return getSubrequestNoChecks(
      [&](TraceContext& tracing, IoChannelFactory& channelFactory) {
    return getSubrequestChannelImpl(
        channel, isInHouse, kj::mv(cfBlobJson), tracing, channelFactory);
  },
      SubrequestOptions{
        .inHouse = isInHouse,
        .wrapMetrics = !isInHouse,
        .operationName = kj::mv(operationName),
      });
}

kj::Own<WorkerInterface> IoContext::getSubrequestChannelImpl(uint channel,
    bool isInHouse,
    kj::Maybe<kj::String> cfBlobJson,
    TraceContext& tracing,
    IoChannelFactory& channelFactory) {
  IoChannelFactory::SubrequestMetadata metadata{
    .cfBlobJson = kj::mv(cfBlobJson),
    .parentSpan = tracing.getInternalSpanParent(),
    .featureFlagsForFl = mapCopyString(worker->getIsolate().getFeatureFlagsForFl()),
  };

  auto client = channelFactory.startSubrequest(channel, kj::mv(metadata));

  return client;
}

kj::Own<kj::HttpClient> IoContext::getHttpClient(
    uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson, kj::ConstString operationName) {
  return asHttpClient(
      getSubrequestChannel(channel, isInHouse, kj::mv(cfBlobJson), kj::mv(operationName)));
}

kj::Own<kj::HttpClient> IoContext::getHttpClient(
    uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson, TraceContext& traceContext) {
  return asHttpClient(getSubrequestChannel(channel, isInHouse, kj::mv(cfBlobJson), traceContext));
}

kj::Own<CacheClient> IoContext::getCacheClient() {
  // TODO(someday): Should Cache API requests be considered in-house? They are already not counted
  //   as subrequests in metrics and logs (like in-house requests aren't), but historically the
  //   subrequest limit still applied. Since I can't currently think of a use case for more than 50
  //   cache API requests per request, I'm leaving it as-is for now.
  limitEnforcer->newSubrequest(false);
  auto ret = getIoChannelFactory().getCache();

  // Apply external memory adjustment for Cache API subrequests when autogate is enabled (same as
  // other subrequests in getSubrequestNoChecks).
  if (util::Autogate::isEnabled(util::AutogateKey::INCREASE_EXTERNAL_MEMORY_ADJUSTMENT_FOR_FETCH)) {
    KJ_IF_SOME(lock, currentLock) {
      jsg::Lock& js = lock;
      ret = ret.attach(js.getExternalMemoryAdjustment(8 * 1024));
    }
  }

  return kj::mv(ret);
}

jsg::AsyncContextFrame::StorageScope IoContext::makeAsyncTraceScope(
    Worker::Lock& lock, kj::Maybe<SpanParent> spanParentOverride) {
  static const SpanParent dummySpanParent = nullptr;

  jsg::Lock& js = lock;
  kj::Own<SpanParent> spanParent;
  KJ_IF_SOME(spo, kj::mv(spanParentOverride)) {
    spanParent = kj::heap(kj::mv(spo));
  } else {
    // TODO(cleanup): Can we also elide the other memory allocations for the (unused) storage
    // scope if tracing is disabled?
    SpanParent metricsSpan = getMetrics().getSpan();
    if (!metricsSpan.isObserved()) {
      // const_cast is ok: There's no state that could be changed in a non-observed span parent.
      spanParent = kj::Own<SpanParent>(
          &const_cast<SpanParent&>(dummySpanParent), kj::NullDisposer::instance);
    } else {
      spanParent = kj::heap(kj::mv(metricsSpan));
    }
  }
  auto ioOwnSpanParent = IoContext::current().addObject(kj::mv(spanParent));
  auto spanHandle = jsg::wrapOpaque(js.v8Context(), kj::mv(ioOwnSpanParent));
  return jsg::AsyncContextFrame::StorageScope(
      js, lock.getTraceAsyncContextKey(), js.v8Ref(spanHandle));
}

SpanParent IoContext::getCurrentTraceSpan() {
  // If called while lock is held, try to use the trace info stored in the async context.
  KJ_IF_SOME(lock, currentLock) {
    KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(lock)) {
      KJ_IF_SOME(value, frame.get(lock.getTraceAsyncContextKey())) {
        auto handle = value.getHandle(lock);
        jsg::Lock& js = lock;
        auto& spanParent = jsg::unwrapOpaqueRef<IoOwn<SpanParent>>(js.v8Isolate, handle);
        return spanParent->addRef();
      }
    }
  }

  // If async context is unavailable (unset, or JS lock is not held), fall back to heuristic of
  // using the trace info from the most recent active request.
  return getMetrics().getSpan();
}

SpanParent IoContext::getCurrentUserTraceSpan() {
  if (incomingRequests.empty()) {
    return SpanParent(nullptr);
  } else {
    return getCurrentIncomingRequest().getCurrentUserTraceSpan();
  }
}

SpanParent IoContext_IncomingRequest::getCurrentUserTraceSpan() {
  return currentUserTraceSpan.addRef();
}

SpanBuilder IoContext::makeTraceSpan(kj::ConstString operationName) {
  return getCurrentTraceSpan().newChild(kj::mv(operationName));
}

TraceContext IoContext::makeUserTraceSpan(kj::ConstString operationName) {
  auto span = makeTraceSpan(operationName.clone());
  auto userSpan = getCurrentUserTraceSpan().newChild(kj::mv(operationName));
  return TraceContext(kj::mv(span), kj::mv(userSpan));
}

void IoContext::taskFailed(kj::Exception&& exception) {
  if (waitUntilStatusValue == EventOutcome::OK) {
    KJ_IF_SOME(status, limitEnforcer->getLimitsExceeded()) {
      waitUntilStatusValue = status;
    } else {
      waitUntilStatusValue = EventOutcome::EXCEPTION;
    }
  }

  // If `taskFailed()` throws the whole event loop blows up... let's be careful not to let that
  // happen.
  KJ_IF_SOME(e, kj::runCatchingExceptions([&]() {
    logUncaughtExceptionAsync(UncaughtExceptionSource::ASYNC_TASK, kj::mv(exception));
  })) {
    KJ_LOG(ERROR, "logUncaughtExceptionAsync() threw an exception?", e);
  }
}

void IoContext::requireCurrent() {
  KJ_REQUIRE(threadLocalRequest == this, "request is not current in this thread");
}

void IoContext::checkFarGet(const DeleteQueue& expectedQueue, const std::type_info& type) {
  requireCurrent();

  if (&expectedQueue == deleteQueue.queue.get()) {
    // same request or same actor, success
  } else {
    throwNotCurrentJsError(type);
  }
}

Worker::Actor& IoContext::getActorOrThrow() {
  return KJ_ASSERT_NONNULL(actor, "not an actor request");
}

void IoContext::runInContextScope(Worker::LockType lockType,
    kj::Maybe<InputGate::Lock> inputLock,
    kj::Function<void(Worker::Lock&)> func) {
  // The previously-current context, before we entered this scope. We have to allow opening
  // multiple nested scopes especially to support destructors: destroying objects related to a
  // subrequest in one worker could transitively destroy resources belonging to the next worker in
  // the pipeline. We can't delay destruction to a future turn of the event loop because it's
  // common for child objects to contain pointers back to stuff owned by the parent that could
  // then be dangling.
  KJ_REQUIRE(threadId == getThreadId(), "IoContext cannot switch threads");
  SuppressIoContextScope previousRequest;
  threadLocalRequest = this;

  worker->runInLockScope(lockType, [&](Worker::Lock& lock) {
    KJ_REQUIRE(currentInputLock == kj::none);
    KJ_REQUIRE(currentLock == kj::none);
    KJ_DEFER(currentLock = kj::none; currentInputLock = kj::none);
    currentInputLock = kj::mv(inputLock);
    currentLock = lock;

    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](jsg::Lock& js) {
      v8::Isolate::PromiseContextScope promiseContextScope(
          lock.getIsolate(), getPromiseContextTag(lock));

      {
        // Handle any pending deletions that arrived while the worker was processing a different
        // request.
        auto l = deleteQueue.queue->crossThreadDeleteQueue.lockExclusive();
        auto& state = KJ_ASSERT_NONNULL(*l);
        for (auto& object: state.queue) {
          OwnedObjectList::unlink(*object);
        }
        state.queue.clear();
      }

      func(lock);
    });
  });
}

void IoContext::runImpl(Runnable& runnable,
    Worker::LockType lockType,
    kj::Maybe<InputGate::Lock> inputLock,
    Runnable::Exceptional exceptional) {
  KJ_IF_SOME(l, inputLock) {
    KJ_REQUIRE(l.isFor(KJ_ASSERT_NONNULL(actor).getInputGate()));
  }

  getIoChannelFactory().getTimer().syncTime();

  runInContextScope(lockType, kj::mv(inputLock), [&](Worker::Lock& workerLock) {
    kj::Own<void> event;
    if (!exceptional) {
      workerLock.requireNoPermanentException();
      // Prevent prematurely detecting a hang while we're still executing JavaScript.
      // TODO(cleanup): Is this actually still needed or is this vestigial? Seems like it should
      //   not be necessary.
      event = registerPendingEvent();
    }

    auto limiterScope = limitEnforcer->enterJs(workerLock, *this);

    bool gotTermination = false;

    KJ_DEFER({
      // Always clear out all pending V8 events before leaving the scope. This ensures that
      // there's never any unfinished work waiting to run when we return to the event loop.
      //
      // Alternatively, we could use kj::evalLater() to queue a callback which runs the microtasks.
      // This would perhaps prevent a microtask loop from blocking incoming I/O events. However,
      // in practice this seems like a dubious scenario. A script that does while(1) will always
      // block I/O, so why should a script in a promise loop not? If scripts want to use 100% of
      // CPU but also receive I/O as it arrives, we should offer some API to explicitly request
      // polling for I/O.
      jsg::Lock& js = workerLock;

      if (gotTermination) {
        // We already consumed the termination pseudo-exception, so if we call RunMicrotasks() now,
        // they will run with no limit. But if we call terminateNextExecution() again now, it will
        // conveniently cause RunMicrotasks() to terminate _right after_ dequeuing the contents of
        // the task queue, which is perfect, because it effectively cancels them all.
        js.terminateNextExecution();
      }

      // Run microtask checkpoint with an active IoContext
      {
        // Running the microtask queue can itself trigger a pending exception in the isolate.
        v8::TryCatch tryCatch(workerLock.getIsolate());

        js.runMicrotasks();

        if (tryCatch.HasCaught()) {
          // It really shouldn't be possible for microtasks to throw regular exceptions.
          // so if we got here it should be a terminal condition.
          KJ_ASSERT(tryCatch.HasTerminated());
          // If we do not reset here we end up with a dangling exception in the isolate that
          // leads to an assert in v8 when the Lock is destroyed.
          tryCatch.Reset();
          // Ensure we don't pump the message loop in this case
          gotTermination = true;
        }
      }

      // Run FinalizationRegistry cleanup tasks without an IoContext
      {
        SuppressIoContextScope noIoCtxt;
        while (!gotTermination && js.pumpMsgLoop()) {
          // Check if FinalizationRegistry cleanup callbacks have not breached our limits
          if (limitEnforcer->getLimitsExceeded() != kj::none) {
            // We can potentially log this, but due to a lack of IoContext we cannot notify
            // the worker
            break;
          }

          // It is possible that a microtask got enqueued during pumpMsgLoop execution
          // Microtasks enqueued by FinalizationRegistry cleanup tasks should also run
          // without an active IoContext
          v8::TryCatch tryCatch(workerLock.getIsolate());

          js.runMicrotasks();

          if (tryCatch.HasCaught()) {
            // It really shouldn't be possible for microtasks to throw regular exceptions.
            // so if we got here it should be a terminal condition.
            KJ_ASSERT(tryCatch.HasTerminated());
            // If we do not reset here we end up with a dangling exception in the isolate that
            // leads to an assert in v8 when the Lock is destroyed.
            tryCatch.Reset();
            // Ensure we don't pump the message loop in this case
            gotTermination = true;
          }
        }
      }
    });

    v8::TryCatch tryCatch(workerLock.getIsolate());
    try {
      runnable.run(workerLock);
    } catch (const jsg::JsExceptionThrown&) {
      if (tryCatch.HasTerminated()) {
        gotTermination = true;
        limiterScope = nullptr;

        // Check if we hit a limit.
        limitEnforcer->requireLimitsNotExceeded();

        // Check if we were aborted. TerminateExecution() may be called after abort() in order
        // to prevent any more JavaScript from executing.
        KJ_IF_SOME(e, abortException) {
          kj::throwFatalException(kj::cp(e));
        }

        // That should have thrown, so we shouldn't get here.
        KJ_FAIL_ASSERT("script terminated for unknown reasons");
      } else {
        if (tryCatch.Message().IsEmpty()) {
          // Should never happen, but check for it because otherwise V8 will crash.
          KJ_LOG(ERROR, "tryCatch.Message() was empty even when not HasTerminated()??",
              kj::getStackTrace());
          JSG_FAIL_REQUIRE(Error, "(JavaScript exception with no message)");
        } else {
          auto jsException = tryCatch.Exception();

          // TODO(someday): We log "uncaught exception" here whenever throwing from JS to C++.
          //   However, the C++ code calling us may still catch the exception and do its own logging,
          //   or may even tunnel it back to JavaScript, making this log line redundant or maybe even
          //   wrong (if the exception is in fact caught later). But, it's difficult to be sure that
          //   all C++ consumers log properly, and even if they do, the stack trace is lost once the
          //   exception has been tunneled into a KJ exception, so the later logging won't be as
          //   useful. We should improve the tunneling to include stack traces and ensure that all
          //   consumers do in fact log exceptions, then we can remove this.
          workerLock.logUncaughtException(UncaughtExceptionSource::INTERNAL,
              jsg::JsValue(jsException), jsg::JsMessage(tryCatch.Message()));

          jsg::throwTunneledException(workerLock.getIsolate(), jsException);
        }
      }
    }
  });
}

static constexpr auto kAsyncIoErrorMessage =
    "Disallowed operation called within global scope. Asynchronous I/O "
    "(ex: fetch() or connect()), setting a timeout, and generating random "
    "values are not allowed within global scope. To fix this error, perform this "
    "operation within a handler. "
    "https://developers.cloudflare.com/workers/runtime-apis/handlers/";

IoContext& IoContext::current() {
  if (threadLocalRequest == nullptr) {
    v8::Isolate* isolate = v8::Isolate::TryGetCurrent();
    KJ_REQUIRE(isolate != nullptr, "there is no current request on this thread");
    isolate->ThrowError(jsg::v8StrIntern(isolate, kAsyncIoErrorMessage));
    throw jsg::JsExceptionThrown();
  } else {
    return *threadLocalRequest;
  }
}

kj::Maybe<IoContext&> IoContext::tryCurrent() {
  if (threadLocalRequest == nullptr) {
    return kj::none;
  } else {
    return *threadLocalRequest;
  }
}

bool IoContext::hasCurrent() {
  return threadLocalRequest != nullptr;
}

bool IoContext::isCurrent() {
  return this == threadLocalRequest;
}

auto IoContext::tryGetWeakRefForCurrent() -> kj::Maybe<kj::Own<WeakRef>> {
  KJ_IF_SOME(ioContext, tryCurrent()) {
    return ioContext.getWeakRef();
  } else {
    return kj::none;
  }
}

void IoContext::abortFromHang(Worker::AsyncLock& asyncLock) {
  KJ_ASSERT(actor == kj::none);  // we don't perform hang detection on actor requests

  // Don't bother aborting if limits were exceeded because in that case the abort promise will be
  // fulfilled shortly anyway.
  if (limitEnforcer->getLimitsExceeded() == kj::none) {
    abort(JSG_KJ_EXCEPTION(FAILED, Error,
        "The Workers runtime canceled this request because it detected that your Worker's code "
        "had hung and would never generate a response. Refer to: "
        "https://developers.cloudflare.com/workers/observability/errors/"));
  }
}

namespace {

class CacheSerializedInputStream final: public kj::AsyncInputStream {
 public:
  CacheSerializedInputStream(
      kj::Own<kj::AsyncInputStream> inner, kj::Own<kj::PromiseFulfiller<void>> fulfiller)
      : inner(kj::mv(inner)),
        fulfiller(kj::mv(fulfiller)) {}

  ~CacheSerializedInputStream() noexcept(false) {
    fulfiller->fulfill();
  }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return inner->tryRead(buffer, minBytes, maxBytes);
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return inner->tryGetLength();
  }

  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    return inner->pumpTo(output, amount);
  }

 private:
  kj::Own<kj::AsyncInputStream> inner;
  kj::Own<kj::PromiseFulfiller<void>> fulfiller;
};

}  // namespace

jsg::Promise<IoOwn<kj::AsyncInputStream>> IoContext::makeCachePutStream(
    jsg::Lock& js, kj::Own<kj::AsyncInputStream> stream) {
  auto paf = kj::newPromiseAndFulfiller<void>();

  KJ_DEFER(cachePutSerializer = kj::mv(paf.promise));

  return awaitIo(js,
      cachePutSerializer.then(
          [fulfiller = kj::mv(paf.fulfiller),
              stream = kj::mv(stream)]() mutable -> kj::Own<kj::AsyncInputStream> {
    if (stream->tryGetLength() != kj::none) {
      // PUT with Content-Length. We can just return immediately, allowing the next PUT to start.
      KJ_DEFER(fulfiller->fulfill());
      return kj::mv(stream);
    } else {
      // TODO(later): With Cache streams no longer having a size limit enforced by the runtime,
      // explore if we can clean up stream serialization too.
      // PUT with Transfer-Encoding: chunked. We have no idea how big this request body is going to
      // be, so wrap the stream that only unblocks the next PUT after this one is complete.
      return kj::heap<CacheSerializedInputStream>(kj::mv(stream), kj::mv(fulfiller));
    }
  }),
      [this](
          jsg::Lock&, kj::Own<kj::AsyncInputStream> result) { return addObject(kj::mv(result)); });
}

void IoContext::writeLogfwdr(
    uint channel, kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage) {
  addWaitUntil(getIoChannelFactory()
                   .writeLogfwdr(channel, kj::mv(buildMessage))
                   .attach(registerPendingEvent()));
}

void IoContext::requireCurrentOrThrowJs() {
  if (!isCurrent()) {
    throwNotCurrentJsError();
  }
}

void IoContext::requireCurrentOrThrowJs(WeakRef& weak) {
  KJ_IF_SOME(ctx, weak.tryGet()) {
    if (ctx.isCurrent()) {
      return;
    }
  }
  throwNotCurrentJsError();
}

void IoContext::throwNotCurrentJsError(kj::Maybe<const std::type_info&> maybeType) {
  auto type = maybeType
                  .map([](const std::type_info& type) {
    return kj::str(" (I/O type: ", jsg::typeName(type), ")");
  }).orDefault(kj::String());

  if (threadLocalRequest != nullptr && threadLocalRequest->actor != kj::none) {
    JSG_FAIL_REQUIRE(Error,
        kj::str(
            "Cannot perform I/O on behalf of a different Durable Object. I/O objects "
            "(such as streams, request/response bodies, and others) created in the context of one "
            "Durable Object cannot be accessed from a different Durable Object in the same isolate. "
            "This is a limitation of Cloudflare Workers which allows us to improve overall "
            "performance.",
            type));
  } else {
    JSG_FAIL_REQUIRE(Error,
        kj::str(
            "Cannot perform I/O on behalf of a different request. I/O objects (such as "
            "streams, request/response bodies, and others) created in the context of one request "
            "handler cannot be accessed from a different request's handler. This is a limitation "
            "of Cloudflare Workers which allows us to improve overall performance.",
            type));
  }
}

jsg::JsObject IoContext::getPromiseContextTag(jsg::Lock& js) {
  if (promiseContextTag == kj::none) {
    auto deferral = kj::heap<IoCrossContextExecutor>(deleteQueue.queue.addRef());
    promiseContextTag = jsg::JsRef(js, js.opaque(kj::mv(deferral)));
  }
  return KJ_REQUIRE_NONNULL(promiseContextTag).getHandle(js);
}

kj::Promise<void> IoContext::startDeleteQueueSignalTask(IoContext* context) {
  // The promise that is returned is held by the IoContext itself, so when the
  // IoContext is destroyed, the promise will be canceled and the loop will
  // end. On each iteration of the loop we want to reset the cross thread
  // signal in the delete queue, then wait on the promise. Once the promise
  // is fulfilled, we will run an empty task to prompt the IoContext to drain
  // the DeleteQueue.
  try {
    for (;;) {
      co_await context->deleteQueue.queue->resetCrossThreadSignal();
      co_await context->run([](auto& lock) {
        auto& context = IoContext::current();
        auto l = context.deleteQueue.queue->crossThreadDeleteQueue.lockExclusive();
        auto& state = KJ_ASSERT_NONNULL(*l);
        for (auto& action: state.actions) {
          action(lock);
        }
        state.actions.clear();
      });
    }
  } catch (...) {
    context->abort(kj::getCaughtExceptionAsKj());
  }
}
}  // namespace workerd
