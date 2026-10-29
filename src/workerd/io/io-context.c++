// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "io-context.h"

#include <workerd/io/access-info.h>
#include <workerd/io/async-trace-perfetto.h>
#include <workerd/io/io-gate.h>
#include <workerd/io/timer-queue.h>
#include <workerd/io/tracer.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/util/sentry.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/uncaught-exception-source.h>
#include <workerd/util/use-perfetto-categories.h>

#include <kj/debug.h>

#include <atomic>
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

class IoContext::TimeoutManagerImpl final: public TimeoutManager, public kj::PtrTarget {
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
    KJ_IF_SOME(tracker, context.tryGetAsyncTracker()) {
      startAsyncTrace(tracker, it);
    }
    setTimeoutImpl(context, it);
    return id;
  }

  void clearTimeout(IoContext&, TimeoutId id) override;

  size_t getTimeoutCount() const override {
    return timeoutsStarted - timeoutsFinished;
  }

  // Earliest deadline on the active input-gate scope, including fired timers whose callbacks
  // have not completed.
  kj::Maybe<kj::Date> getClockClamp(IoContext& context) const {
    KJ_IF_SOME(queue, findQueue(currentSection(context))) {
      return queue.getClockClamp();
    }
    return kj::none;
  }

  void adoptTimersFromCompletedSection(IoContext& context, InputGate::CriticalSection& section);

  void cancelAll() override {
    timeouts.clear();
    sectionScopes.clear();
    rootQueue.clear();
  }

 private:
  struct IdAndIterator {
    TimeoutId id;
    Iterator it;
  };
  IdAndIterator addState(TimeoutId::Generator& generator, TimeoutParameters params);

  void setTimeoutImpl(IoContext& context, Iterator it);

  void startAsyncTrace(const AsyncTracker& tracker, Iterator it);

  using TimeoutKey = TimerQueue::Key;

  struct SectionScope {
    explicit SectionScope(kj::Rc<InputGate::CriticalSection> section): section(kj::mv(section)) {}

    // Keeps the map key alive while the section has timers.
    kj::Rc<InputGate::CriticalSection> section;
    TimerQueue timers;
  };

  TimerQueue rootQueue;
  // Section records are removed when their final timer is removed or adopted by the parent.
  kj::HashMap<const InputGate::CriticalSection*, kj::Own<SectionScope>> sectionScopes;
  uint timeoutTiebreakerCounter = 0;

  static kj::Maybe<InputGate::CriticalSection&> currentSection(
      IoContext& context KJ_LIFETIMEBOUND) {
    KJ_IF_SOME(lock, context.currentInputLock) {
      return lock.getCriticalSection();
    }
    return kj::none;
  }

  static kj::Maybe<InputGate::CriticalSection&> resolveSection(
      kj::Maybe<InputGate::CriticalSection&> section KJ_LIFETIMEBOUND) {
    KJ_IF_SOME(s, section) {
      return s.getActive();
    }
    return kj::none;
  }

  // Reading the clock and removing a timer must not allocate a scope.
  kj::Maybe<TimerQueue&> findQueue(
      kj::Maybe<InputGate::CriticalSection&> section) KJ_LIFETIMEBOUND {
    KJ_IF_SOME(active, resolveSection(section)) {
      return sectionScopes.find(&active).map(
          [](auto& entry) -> TimerQueue& { return entry->timers; });
    }
    return rootQueue;
  }

  kj::Maybe<const TimerQueue&> findQueue(
      kj::Maybe<InputGate::CriticalSection&> section) const KJ_LIFETIMEBOUND {
    KJ_IF_SOME(active, resolveSection(section)) {
      return sectionScopes.find(&active).map(
          [](auto& entry) -> const TimerQueue& { return entry->timers; });
    }
    return rootQueue;
  }

  TimerQueue& getOrCreateQueue(kj::Maybe<InputGate::CriticalSection&> section) KJ_LIFETIMEBOUND {
    KJ_IF_SOME(active, resolveSection(section)) {
      return sectionScopes
          .findOrCreate(&active, [&] {
        return decltype(sectionScopes)::Entry{&active, kj::heap<SectionScope>(active.clone())};
      })->timers;
    }
    return rootQueue;
  }

  void removeTimeout(
      kj::Maybe<InputGate::CriticalSection&> section, TimeoutKey key, TimerChannel& timerChannel);

  uint timeoutsStarted = 0;
  uint timeoutsFinished = 0;
  Map timeouts;
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

  // Settles when a one-shot timer fires; an interval stays unsettled until cleared. Each firing
  // causes a turn. Declared before `maybePromise`, so it is released after the timer's promise.
  AsyncResource asyncTraceResource;

  kj::Maybe<kj::Promise<void>> maybePromise;
};

namespace {

// Creates the context's tracker if the isolate has async tracing enabled and there is at least one
// sink: the embedder's, Perfetto's if it is recording "workerd.async", or the inspector's if a
// DevTools session is connected to the isolate.
kj::Maybe<kj::Arc<AsyncTracker>> makeAsyncTracker(
    const Worker& worker, kj::Maybe<Worker::Actor&> actor) {
  auto& isolate = worker.getIsolate();
  KJ_IF_SOME(traceIsolate, isolate.getAsyncTraceIsolate()) {
    kj::Maybe<kj::String> actorId = actor.map([](Worker::Actor& a) -> kj::String {
      KJ_SWITCH_ONEOF(a.getId()) {
        KJ_CASE_ONEOF(id, kj::Own<ActorIdFactory::ActorId>) {
          return id->toString();
        }
        KJ_CASE_ONEOF(name, kj::String) {
          return kj::str(name);
        }
      }
      KJ_UNREACHABLE;
    });
    auto actorIdPtr = actorId.map([](kj::String& id) -> kj::StringPtr { return id; });
    AsyncTraceSinks sinks;
    isolate.getMetrics().addAsyncTraceSinks(sinks, isolate.getId(), actorIdPtr);
    if (isAsyncTracePerfettoEnabled()) {
      sinks.add(newAsyncTracePerfettoSink(isolate.getId(), actorIdPtr));
    }
    KJ_IF_SOME(sink, isolate.newAsyncTraceInspectorSink()) {
      sinks.add(kj::mv(sink));
    }
    return AsyncTracker::tryCreate(traceIsolate, kj::mv(sinks), isolate.getId(), actorIdPtr);
  }
  return kj::none;
}

}  // namespace

IoContext::IoContext(ThreadContext& thread,
    kj::Own<const Worker> workerParam,
    kj::Maybe<Worker::Actor&> actorParam,
    kj::Own<LimitEnforcer> limitEnforcerParam)
    : thread(thread),
      worker(kj::mv(workerParam)),
      opentelemetryApiEnabled(
          worker->getIsolate().getApi().getFeatureFlags().getOpentelemetryApi()),
      actor(actorParam),
      limitEnforcer(kj::mv(limitEnforcerParam)),
      asyncTracker(makeAsyncTracker(*worker, actor)),
      id(nextId()),
      threadId(getThreadId()),
      deleteQueue(kj::arc<DeleteQueue>()),
      reverseIoOwnValidity(kj::arc<ReverseIoOwnValidity>()),
      timeoutManager(kj::heap<TimeoutManagerImpl>()),
      waitUntilTasks(*this),
      tasks(*this),
      deleteQueueSignalTask(startDeleteQueueSignalTask(this)) {
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
            kj::throwException(kj::mv(exception));
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
    kj::Rc<IoChannelFactory> ioChannelFactoryParam,
    kj::Own<RequestObserver> metricsParam,
    kj::Maybe<kj::Rc<BaseTracer>> workerTracer,
    kj::Maybe<tracing::InvocationSpanContext> maybeTriggerInvocationSpan,
    kj::Maybe<kj::Own<AccessInfo>> accessInfo,
    kj::Maybe<kj::Rc<IoChannelFactory::SelfTokenFactory>> selfTokenFactory)
    : context(kj::mv(contextParam)),
      metrics(kj::mv(metricsParam)),
      workerTracer(kj::mv(workerTracer)),
      ioChannelFactory(kj::mv(ioChannelFactoryParam)),
      accessInfo(kj::mv(accessInfo)),
      selfTokenFactory(kj::mv(selfTokenFactory)),
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

tracing::InvocationSpanContext IoContext::IncomingRequest::getInvocationSpanContextForUserSpan(
    SpanParent& userSpan) {
  auto& base = getInvocationSpanContext();
  if (context->isOpentelemetryApiEnabled()) {
    KJ_IF_SOME(identity, userSpan.toSpanContext()) {
      KJ_IF_SOME(id, identity.getSpanId()) {
        if (id != tracing::SpanId::nullId) {
          return tracing::InvocationSpanContext(identity.getTraceId(), base.getInvocationId(), id,
              identity.getTraceFlags(), identity.cloneTraceState());
        }
      }
    }
  }
  tracing::SpanId sid = userSpan.getSpanId();
  if (sid != tracing::SpanId::nullId) {
    return tracing::InvocationSpanContext(base.getTraceId(), base.getInvocationId(), sid,
        base.getTraceFlags(), base.cloneTraceState());
  }
  return base.clone();
}

// A call to delivered() implies a promise to call drain() later (or one of the other methods
// that sets waitedForWaitUntil). So, we can now safely add the request to
// context->incomingRequests, which implies taking responsibility for draining on the way out.
void IoContext::IncomingRequest::delivered(kj::SourceLocation location) {
  delivered(location.function, location);
}

void IoContext::IncomingRequest::delivered(kj::StringPtr eventType, kj::SourceLocation location) {
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
  tracerLess = workerTracer == kj::none;
  deliveredLocation = location;
  metrics->delivered();

  KJ_IF_SOME(tracker, context->tryGetAsyncTracker()) {
    asyncTraceResource = tracker.create(AsyncKind::REQUEST, eventType);
    // Delivered synchronously from another context's turn (e.g. a service binding fetch), the
    // request records what caused it there.
    tracker.linkFromCallerTurn(asyncTraceResource.getId());
  }

  // Create the root user trace span once per request. Stale references to the span (e.g. from
  // AsyncContextFrame storage via IoOwn, which for actors can outlive this request via the
  // IoContext's delete queue) are safe: user-tracing SpanSubmitters hold only a
  // BaseTracer::WeakRef, so they cannot extend tracer lifetime.
  KJ_IF_SOME(workerTracer, workerTracer) {
    auto& invCtx = getInvocationSpanContext();
    if (context->isOpentelemetryApiEnabled()) {
      // Experimental JS-only policy: absent and exported means sampled. This local policy
      // still needs product sign-off. Cache it once per invocation, never in native flags.
      localSampled = invCtx.getTraceFlags().map([](auto flags) {
        return flags.isSampled();
      }).orDefault([&] { return workerTracer->exportsUserSpans(); });
    }
    if (context->isOpentelemetryApiEnabled() &&
        invCtx.getTraceFlags().map([](auto flags) {
      return !flags.isSampled();
    }).orDefault(false)) {
      rootUserTraceSpan =
          SpanParent(kj::rc<PropagationOnlySpanObserver>(tracing::SpanContext(invCtx.getTraceId(),
              invCtx.getSpanId(), invCtx.getTraceFlags(), invCtx.cloneTraceState())));
    } else {
      rootUserTraceSpan = workerTracer->makeUserRequestSpan(
          invCtx.getTraceId(), invCtx.getTraceFlags(), invCtx.cloneTraceState());
    }
  } else if (context->isOpentelemetryApiEnabled() && maybeTriggerInvocationSpan != kj::none) {
    auto& invCtx = getInvocationSpanContext();
    // A trigger can be adopted HTTP or native dispatch. Without a sink, retain its identity
    // for JS and egress, but do not invent a context for an otherwise untraced invocation.
    KJ_IF_SOME(trigger, invCtx.getParent()) {
      rootUserTraceSpan =
          SpanParent(kj::rc<PropagationOnlySpanObserver>(tracing::SpanContext(invCtx.getTraceId(),
              trigger.getSpanId(), invCtx.getTraceFlags(), invCtx.cloneTraceState())));
    }
  }

  KJ_IF_SOME(a, context->actor) {
    // Re-synchronize the timer and top up limits for every new incoming request to an actor.
    // Note: when tracing is active this resync may have already happened via nowForTraceOnset(),
    // which reads the onset timestamp before delivered() runs. Syncing again here is harmless (the
    // two calls are back-to-back) and keeps the resync unconditional for the non-traced path.
    ioChannelFactory->getTimer().syncTime();
    context->limitEnforcer->topUpActor();

    // Run the Actor's constructor if it hasn't been run already.
    a.ensureConstructed(*context);

    // Record a new incoming request to actor metrics.
    a.getMetrics().startRequest();
  }
}

kj::Date IoContext::IncomingRequest::now(kj::Maybe<kj::Date> clockClamp) {
  metrics->clockRead();
  return ioChannelFactory->getTimer().now(kj::mv(clockClamp));
}

kj::Date IoContext::IncomingRequest::nowForTraceOnset() {
  if (context->actor != kj::none) {
    ioChannelFactory->getTimer().syncTime();
  }
  return now();
}

IoContext::IncomingRequest::~IoContext_IncomingRequest() noexcept(false) {
  if (!wasDelivered) {
    KJ_IF_SOME(w, workerTracer) {
      w->markUnused();
    }
    // Request was never added to context->incomingRequests in the first place.
    return;
  }

  asyncTraceResource.settle(AsyncOutcome::OK);

  bool hadUndrainedWaitUntilTasks = !waitedForWaitUntil && !context->waitUntilTasks.isEmpty();
  kj::Maybe<kj::Exception> cancellationException;

  if (!context->isShared()) {
    // Reentry callbacks may have spans attached to their pending promises. Cancel them while the
    // request is still current so those spans close before the request outcome is reported.
    cancellationException = context->cancelPendingWork([&]() -> kj::Exception {
      KJ_IF_SOME(e, context->abortException) {
        return e.clone();
      }
      return JSG_KJ_EXCEPTION(
          FAILED, Error, "The execution context responding to this call was canceled.");
    }, ClearTasks::YES);
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

    if (hadUndrainedWaitUntilTasks) {
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
      //
      // The abort has already been reported, so a destructor that throws during this cleanup
      // adds nothing: it is dropped.
      context->cancelPendingWork([&]() { return e.clone(); }, ClearTasks::YES);
    }
  }

  // Remove incoming request after canceling waitUntil tasks, which may have spans attached that
  // require accessing a timer from the active request.
  context->incomingRequests.remove(*this);

  KJ_IF_SOME(exception, cancellationException) {
    unwindDetector.catchExceptionsIfUnwinding(
        [&]() { kj::throwExceptionUnlessUnwinding(kj::mv(exception)); });
  }
}

kj::Maybe<kj::Exception> IoContext::cancelPendingWork(
    kj::FunctionParam<kj::Exception()> makeReason, ClearTasks clearTasks) {
  kj::Maybe<kj::Exception> firstException;
  auto runStep = [&](auto&& step) {
    KJ_IF_SOME(exception, kj::runCatchingExceptions(step)) {
      if (firstException == kj::none) {
        firstException = kj::mv(exception);
      }
    }
  };
  // Canceler unlinks each callback before destroying its promise, so retrying makes progress
  // after a destructor throws.
  while (!canceler.isEmpty()) {
    runStep([&]() { canceler.cancel(makeReason()); });
  }
  runStep([&]() { timeoutManager->cancelAll(); });
  if (clearTasks) {
    runStep([&]() { tasks.clear(); });
  }
  runStep([&]() { waitUntilTasks.clear(); });
  return firstException;
}

void IoContext::IncomingRequest::cancelPendingWorkForHibernation() {
  // This request must still be current: canceled work may have spans that read its timer.
  waitedForWaitUntil = true;
  context->reentryDisabled = true;
  // Cross-context promise reactions queued for this context would run on its next turn. The
  // context is about to be destroyed, which drops them anyway; drop them now. The task only runs
  // on its own event-loop turns, never within this call, so it is safe to destroy here.
  context->deleteQueueSignalTask = nullptr;
  KJ_IF_SOME(exception, context->cancelPendingWork([]() {
    return JSG_KJ_EXCEPTION(FAILED, Error, "The Durable Object is hibernating.");
  }, ClearTasks::NO)) {
    LOG_EXCEPTION("onHibernateCleanup", exception);
  }
}

InputGate::Lock IoContext::getInputLock() {
  return KJ_ASSERT_NONNULL(currentInputLock, "no input lock available in this context")
      .addRef(getCurrentTraceSpan());
}

kj::Maybe<kj::Rc<InputGate::CriticalSection>> IoContext::getCriticalSection() {
  KJ_IF_SOME(l, currentInputLock) {
    return l.getCriticalSection().clone();
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

bool IoContext::hasWarningHandler() {
  if (isInspectorEnabled() || ::kj::_::Debug::shouldLog(::kj::LogSeverity::INFO)) return true;
  KJ_IF_SOME(lock, currentLock) {
    jsg::Lock& js = lock;
    return js.withinHandleScope([&] { return getWorkerTracer() != kj::none; });
  }
  return getWorkerTracer() != kj::none;
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

void IoContext::logUncaughtExceptionAsync(UncaughtExceptionSource source,
    kj::Exception&& exception,
    kj::Maybe<IncomingRequest&> incomingRequest) {
  kj::Maybe<BaseTracer&> tracer;
  kj::Maybe<RequestObserver&> metrics;
  KJ_IF_SOME(request, incomingRequest) {
    KJ_DASSERT(&request.getContext() == this);
    tracer = request.getWorkerTracer();
    metrics = request.getMetrics();
  } else if (!incomingRequests.empty()) {
    tracer = getWorkerTracer();
    metrics = getMetrics();
  }

  if (tracer == kj::none && !worker->getIsolate().isInspectorEnabled()) {
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
    kj::Maybe<IncomingRequest&> incomingRequest;

    RunnableImpl(UncaughtExceptionSource source,
        kj::Exception&& exception,
        kj::Maybe<IncomingRequest&> incomingRequest)
        : source(source),
          exception(kj::mv(exception)),
          incomingRequest(incomingRequest) {}
    void run(Worker::Lock& lock) override {
      // TODO(soon): Add logUncaughtException to jsg::Lock.
      lock.logUncaughtException(source, kj::mv(exception), incomingRequest);
    }
  };

  // Make sure this is logged even if another exception occurs trying to log it to the devtools inspector,
  // e.g. if `runImpl` throws before calling logUncaughtException.
  // This is useful for tests (and in fact only affects tests, since it's logged at an INFO level).
  KJ_ON_SCOPE_FAILURE({ KJ_LOG(INFO, "uncaught exception", source, exception); });
  RunnableImpl runnable(source, kj::mv(exception), incomingRequest);
  // TODO(perf): Is it worth using an async lock here? The only case where it really matters is
  //   when a trace worker is active, but maybe they'll be more common in the future. To take an
  //   async lock here, we'll probably have to update all the call sites of this method... ick.
  runImpl(
      runnable, Worker::Lock::TakeSynchronously(metrics), kj::none, Runnable::Exceptional(true));
}

void IoContext::abort(kj::Exception&& e) {
  if (abortException != kj::none) {
    return;
  }
  abortException = e.clone();
  KJ_IF_SOME(a, actor) {
    // Stop the ActorCache from flushing any scheduled write operations to prevent any unnecessary
    // or unintentional async work
    a.shutdownActorCache(e.clone());
  }
  abortFlag.reject(kj::mv(e));
}

void IoContext::abortIsolate(kj::StringPtr reason) {
  getIoChannelFactory().abortIsolate(reason);
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

  if (incomingRequests.empty()) {
    // A task can be scheduled after the last IncomingRequest has been unlinked (see
    // ~IoContext_IncomingRequest) while the IoContext itself is still alive -- an RPC stream
    // adapter dropped during teardown, for instance, schedules the stream's abort algorithm from
    // its destructor. There is no request to attribute the task to, so the metric below is
    // skipped; the task is still queued, and runs if the IoContext gets far enough to run it.
    // This is a state worth knowing about, hence the log, but it is recoverable and must not be
    // treated as a precondition violation.
    DEBUG_FATAL_RELEASE_LOG(WARNING, "Adding task to IoContext with no current IncomingRequest",
        lastDeliveredLocation, kj::getStackTrace());
  } else if (actor == kj::none) {
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
  kj::Own<Worker::Actor::WaitUntilTaskHandle> handle;
  KJ_IF_SOME(a, actor) {
    KJ_IF_SOME(e, kj::runCatchingExceptions([&]() { handle = a.addedWaitUntilTask(); })) {
      KJ_LOG(ERROR, "Actor::addedWaitUntilTask() threw an exception", e);
    }
  }
  if (handle.get() != nullptr) {
    promise = promise.attach(kj::mv(handle));
  }

  if (incomingRequests.empty()) {
    DEBUG_FATAL_RELEASE_LOG(WARNING, "Adding task to IoContext with no current IncomingRequest",
        lastDeliveredLocation, kj::getStackTrace());
  } else if (actor == kj::none) {
    // This metric won't work correctly in actors since it's being tracked per-request, but tasks
    // are not tied to requests in actors. So we just skip it in actors.
    auto& metrics = getMetrics();
    if (metrics.getSpan().isObserved()) {
      promise = promise.attach(metrics.addedWaitUntilTask());
    }
  }

  waitUntilTasks.add(kj::mv(promise));
}

namespace {

#ifdef KJ_DEBUG

void requestGc(const Worker& worker) {
  TRACE_EVENT("workerd", "Debug: requestGc()");
  jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
    auto& isolate = worker.getIsolate();
    auto lock = isolate.getApi().lock(stackScope);
    lock->requestGcForTesting();
  });
}

template <typename T>
kj::Promise<T> addGcPassForTest(IoContext& context, kj::Promise<T> promise) {
  TRACE_EVENT("workerd", "Debug: addGcPassForTest");
  auto worker = kj::atomicAddRef(context.getWorker());
  if constexpr (kj::isSameType<T, void>()) {
    co_await promise;
    requestGc(*worker);
  } else {
    auto ret = co_await promise;
    requestGc(*worker);
    co_return kj::mv(ret);
  }
}

#endif  // KJ_DEBUG

}  // namespace

template <typename T>
kj::Promise<T> IoContext::IncomingRequest::maybeAddGcPassForTest(kj::Promise<T> promise) {
#ifdef KJ_DEBUG
  if (isPredictableModeForTest()) {
    return addGcPassForTest(*context, kj::mv(promise));
  }
#endif
  return kj::mv(promise);
}

// Mark ourselves so we know that we made a best effort attempt to wait for waitUntilTasks.
void IoContext::IncomingRequest::drain(
    kj::TaskSet& waitUntilTasks, kj::Own<IoContext_IncomingRequest>&& self) {
  // Passing by rvalue reference keeps the call-site evaluation order safe, but does not itself
  // consume the caller's owner. Move immediately so early returns still drop the request.
  auto ownedSelf = kj::mv(self);
  waitedForWaitUntil = true;

  if (&context->incomingRequests.front() != this) {
    // A newer request was received, so draining isn't our job.
    return;
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
      return context->run([](Worker::Lock&, IoContext& context) {
        context.logWarning(
            "waitUntil() tasks did not complete within the allowed time after invocation end and have been cancelled. "
            "See: https://developers.cloudflare.com/workers/runtime-apis/context/#waituntil");
      });
    };
    timeoutPromise = context->limitEnforcer->limitDrain().then(kj::mv(timeoutLogPromise));
  }
  auto result = context->waitUntilTasks.onEmpty()
                    .exclusiveJoin(kj::mv(timeoutPromise))
                    .exclusiveJoin(context->onAbort());

  result = result.attach(kj::mv(ownedSelf));

  KJ_IF_SOME(a, context->actor) {
    // Make sure the drain is canceled and the IncomingRequest dropped on actor abort.
    result = a.getAbortCanceler().wrap(kj::mv(result));
  }

  // We actually don't want the promise we put in `waitUntilTasks` to report errors when aborted.
  // Abort errors are already propagated to any connected clients and other places. Note that
  // `waitUntilTasks.onEmpty()` never throws, and `timeoutPromise` as constructed above also never
  // throws, so this is just squelching abort errors.
  result = result.catch_([](kj::Exception&&) {});

  waitUntilTasks.add(maybeAddGcPassForTest(kj::mv(result)));
}

kj::Promise<WorkerInterface::ScheduledResult> IoContext::IncomingRequest::finishScheduled(
    kj::Own<IoContext_IncomingRequest>&& self) {
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
      [] { return EventOutcome::EXCEEDED_WALL_TIME; });
  auto outcome = context->waitUntilTasks.onEmpty()
                     .then([weakThis = addWeakToThis()]() {
    return KJ_REQUIRE_NONNULL(weakThis.tryGet(), "IncomingRequest destroyed before tasks finished")
        .context->waitUntilStatus();
  })
                     .exclusiveJoin(kj::mv(timeoutPromise))
                     .exclusiveJoin(context->onAbort().then([] {
    // The abort flag should only ever be rejected instead of being fulfilled, return an
    // internalError outcome if it does happen
    return EventOutcome::INTERNAL_ERROR;
  }, [](kj::Exception&& e) {
    KJ_LOG(INFO, "execution context aborted", e);  // for tests
    return RequestObserver::outcomeFromException(e);
  }));

  auto result = outcome.then([weakThis = addWeakToThis()](EventOutcome outcome) {
    return WorkerInterface::ScheduledResult{
      .retry = KJ_REQUIRE_NONNULL(weakThis.tryGet(), "IncomingRequest destroyed before outcome")
                   .context->shouldRetryScheduled(),
      .outcome = outcome,
    };
  });

  return maybeAddGcPassForTest(result.attach(kj::mv(self)));
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

  // Invalidate ReverseIoOwn instances before destroying their backing OwnedObjects.
  reverseIoOwnValidity->invalidate();

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
  context.abortFromHangTask =
      Worker::AsyncLock::whenThreadIdle()
          .then([weakContext = context.getWeakRef()]() mutable noexcept -> kj::Promise<void> {
    // Reject rather than throw: a throw would terminate the process from this `noexcept` lambda.
    auto& ctx = KJ_UNWRAP_OR(weakContext->tryGet(),
        { return KJ_EXCEPTION(FAILED, "IoContext destroyed before hang abort"); });

    // We have nothing left to do and no PendingEvent has been registered. Abort now.
    //
    // By the time the thread goes idle the IncomingRequest may already be gone, even though the
    // IoContext lives on (e.g. waitUntil work is still outstanding). getMetrics() requires a
    // current IncomingRequest, and this continuation is `noexcept`, so reaching for it in that
    // state would terminate the process instead of aborting the context. Fall back to the
    // request-less lock, which only costs us the lock-timing metrics.
    auto lockPromise = ctx.incomingRequests.empty()
        ? ctx.worker->takeAsyncLockWithoutRequest(nullptr)
        : ctx.worker->takeAsyncLock(ctx.getMetrics());
    return lockPromise.then([weakContext = kj::mv(weakContext)](Worker::AsyncLock asyncLock) {
      KJ_REQUIRE_NONNULL(weakContext->tryGet(), "IoContext destroyed before hang abort")
          .abortFromHang(asyncLock);
    });
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
      kj::throwException(e.clone());
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
  KJ_DTOR_ASSERT(!isRunning);
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

  // Reports `destroy` now, unless the timer already fired (and settled).
  asyncTraceResource.release();

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

void IoContext::TimeoutManagerImpl::startAsyncTrace(const AsyncTracker& tracker, Iterator it) {
  auto& state = it->second;
  state.asyncTraceResource = tracker.create(AsyncKind::TIMER, state.params.asyncTraceName);
  state.asyncTraceResource.annotate("delayMs"_kj, kj::str(state.params.msDelay));
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
  TimeoutKey timeoutKey{when, timeoutTiebreakerCounter++};
  // TODO(cleanup): The manual use of run() here (including carrying over the critical section) is
  //   kind of ugly, but using awaitIo() doesn't work here because we need the ability to cancel
  //   the timer, so we don't want to addTask() it, which awaitIo() does implicitly.
  auto promise = paf.promise.then(
      [weakThis = addWeakToThis(), weakContext = context.getWeakRef(), it, when,
          cs = context.getCriticalSection()]() mutable {
    auto& firing = it->second;
    if (!firing.params.repeat) {
      firing.asyncTraceResource.settle(AsyncOutcome::OK);
    }
    auto& liveContext =
        KJ_REQUIRE_NONNULL(weakContext->tryGet(), "IoContext destroyed before timeout fired");
    return liveContext.run(
        [weakThis = kj::mv(weakThis), it, when](Worker::Lock& lock, IoContext& context) mutable {
      auto& state = it->second;

      auto stateGuard = kj::defer([&] {
        if (state.maybePromise == kj::none) {
          // At the end of this block, there was no new timeout, so we should remove the state.
          // Note that this can happen from cancelTimeout or a non-repeating timeout.
          KJ_REQUIRE_NONNULL(weakThis.tryGet(), "TimeoutManager destroyed before timeout fired")
              .timeouts.erase(it);
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

        state.asyncTraceResource.enterAsTurnCause();

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
            KJ_REQUIRE_NONNULL(weakThis.tryGet(), "TimeoutManager destroyed before timeout fired")
                .setTimeoutImpl(context, it);
          }
        }););

        // An adopted callback can precede earlier deadlines in its new queue. Observe its own
        // deadline using the channel's clock policy, and retain that time through its microtasks.
        context.observeTime(context.getCurrentIncomingRequest().now(when));
        state.trigger(lock);
      }
    }, kj::mv(cs));
  },
      [](kj::Exception&&) {});

  promise = promise.attach(context.registerPendingEvent());

  // The critical section may have completed and handed its timers to an enclosing scope.
  auto deferredTimeoutRemoval =
      kj::defer([this, &context, timeoutKey, section = context.getCriticalSection()]() mutable {
    // If the promise is being destroyed due to IoContext teardown then IoChannelFactory may
    // no longer be available, but we can just skip starting a new timer in that case as it'd be
    // canceled anyway. Similarly we should skip rescheduling if the context has been aborted since
    // there's no way the events can run anyway (and we'll cause trouble if `cancelAll()` is being
    // called in ~IoContext_IncomingRequest).
    if (context.selfRef->isValid() && context.abortException == kj::none) {
      removeTimeout(section.map([](auto& cs) -> InputGate::CriticalSection& { return *cs; }),
          timeoutKey, context.getIoChannelFactory().getTimer());
    }
  });

  auto& queue = getOrCreateQueue(currentSection(context));
  queue.insert(timeoutKey, kj::mv(paf.fulfiller), context.getIoChannelFactory().getTimer());
  promise = promise.attach(kj::mv(deferredTimeoutRemoval));

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

void IoContext::TimeoutManagerImpl::adoptTimersFromCompletedSection(
    IoContext& context, InputGate::CriticalSection& section) {
  KJ_IF_SOME(active, section.getActive()) {
    KJ_ASSERT(&active != &section, "timer adoption requires a successful critical section");
  }
  KJ_IF_SOME(scope, sectionScopes.find(&section)) {
    auto source = kj::mv(scope);
    sectionScopes.erase(&section);
    // InputGate resolves this completed section to the nearest enclosing active section.
    auto& destination = getOrCreateQueue(section);
    destination.adoptFrom(source->timers, context.getIoChannelFactory().getTimer());
  }
}

void IoContext::TimeoutManagerImpl::removeTimeout(
    kj::Maybe<InputGate::CriticalSection&> section, TimeoutKey key, TimerChannel& timerChannel) {
  // A registered timer's effective scope exists until its queue entry is removed.
  auto& queue = KJ_ASSERT_NONNULL(findQueue(section));
  queue.erase(key, timerChannel);
  if (queue.empty()) {
    KJ_IF_SOME(active, resolveSection(section)) {
      sectionScopes.erase(&active);
    }
  }
}

InputGate::Lock IoContext::finishCriticalSection(InputGate::CriticalSection& section) {
  auto inputLock = section.succeeded();
  timeoutManager->adoptTimersFromCompletedSection(*this, section);
  return inputLock;
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

TimeoutId IoContext::setTimeoutImpl(TimeoutId::Generator& generator,
    bool repeat,
    jsg::Function<void()> function,
    double msDelay,
    kj::Maybe<kj::StringPtr> asyncTraceName) {
  static constexpr int64_t max = 3153600000000;  // Milliseconds in 100 years
  // Clamp the range on timers to [0, 3153600000000] (inclusive). The specs
  // do not indicate a clear maximum range for setTimeout/setInterval so the
  // limit here is fairly arbitrary. 100 years max should be plenty safe.
  int64_t delay = msDelay <= 0 || std::isnan(msDelay) ? 0
      : msDelay >= static_cast<double>(max)           ? max
                                                      : static_cast<int64_t>(msDelay);
  auto params = TimeoutManager::TimeoutParameters(repeat, delay, kj::mv(function));
  params.asyncTraceName = asyncTraceName.orDefault(repeat ? "setInterval"_kj : "setTimeout"_kj);
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

  // Only timers on the active input-gate scope clamp observable time. Returning to a parent
  // scope must not rewind a clock that advanced inside a critical section.
  return observeTime(incomingRequest.now(timeoutManager->getClockClamp(*this)));
}

kj::Date IoContext::observeTime(kj::Date time) {
  KJ_IF_SOME(previous, lastObservedTime) {
    time = kj::max(time, previous);
  }
  // Only reads under an input lock select the caller's scope. Internal pre-entry reads, such as
  // actor-cache maintenance, see the root scope and must not override a nested timer's clamp.
  if (currentInputLock != kj::none) {
    lastObservedTime = time;
  }
  return time;
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
    SubrequestOptions options,
    CountSubrequest countSubrequest) {
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
    ret = metrics.wrapSubrequestClient(kj::mv(ret), countSubrequest);
    ret = worker->getIsolate().wrapSubrequestClient(
        kj::mv(ret), getHeaderIds().contentEncoding, metrics);
  }

  // The span lives as long as the client, not just until the caller's next awaitIo(), which may
  // await something else; unless the caller says that awaitIo() awaits this subrequest.
  bool keepForAsyncTrace = false;
  if (options.spanAwaitedNext == SpanAwaitedNext::NO) {
    tracing.detachAsync();
  } else {
    // Unobserved spans are dropped here, which would settle the operation before awaitIo() can
    // adopt it; keep it with the client, as an observed span would be.
    keepForAsyncTrace = tryGetAsyncTracker() != kj::none;
  }
  if (tracing.isObserved() || keepForAsyncTrace) {
    auto ioOwnedSpan = addObject(kj::heap(kj::mv(tracing)));
    ret = ret.attach(kj::mv(ioOwnedSpan));
  }

  // Subrequests use a lot of unaccounted C++ memory, so we adjust V8's external memory counter to
  // pressure the GC and protect against OOMs. We apply this adjustment to ALL subrequests (not
  // just fetch). We only apply this when the JS lock is held (i.e., when JS code initiated the
  // subrequest); infrastructure paths that bypass JS don't need it.
  KJ_IF_SOME(lock, currentLock) {
    jsg::Lock& js = lock;
    ret = ret.attach(js.getExternalMemoryAdjustment(8 * 1024));
  }

  return kj::mv(ret);
}

kj::Own<WorkerInterface> IoContext::getSubrequest(
    kj::FunctionParam<kj::Own<WorkerInterface>(TraceContext&, IoChannelFactory&)> func,
    SubrequestOptions options) {
  limitEnforcer->newSubrequest(options.inHouse);
  return getSubrequestNoChecks(kj::mv(func), kj::mv(options), CountSubrequest::YES);
}

kj::Own<WorkerInterface> IoContext::getSubrequestChannel(uint channel,
    bool isInHouse,
    kj::Maybe<kj::String> cfBlobJson,
    kj::ConstString operationName,
    SpanAwaitedNext spanAwaitedNext) {
  return getSubrequest(
      [&](TraceContext& tracing, IoChannelFactory& channelFactory) {
    return getSubrequestChannelImpl(
        channel, isInHouse, kj::mv(cfBlobJson), tracing, channelFactory);
  },
      SubrequestOptions{
        .inHouse = isInHouse,
        .wrapMetrics = !isInHouse,
        .operationName = kj::mv(operationName),
        .spanAwaitedNext = spanAwaitedNext,
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

kj::Own<WorkerInterface> IoContext::getSubrequestChannel(uint channel,
    bool isInHouse,
    kj::Maybe<kj::String> cfBlobJson,
    TraceContext& traceContext,
    SpanParent userSpanParent) {
  return getSubrequest(
      [&](TraceContext& tracing, IoChannelFactory& channelFactory) {
    return getSubrequestChannelImpl(
        channel, isInHouse, kj::mv(cfBlobJson), tracing, channelFactory, kj::mv(userSpanParent));
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
      },
      CountSubrequest::YES);
}

kj::Own<WorkerInterface> IoContext::getSubrequestChannelImpl(uint channel,
    bool isInHouse,
    kj::Maybe<kj::String> cfBlobJson,
    TraceContext& tracing,
    IoChannelFactory& channelFactory,
    kj::Maybe<SpanParent> userSpanParent) {
  auto propagatedUserSpanParent = tracing.getUserSpanParent();
  KJ_IF_SOME(parent, userSpanParent) {
    propagatedUserSpanParent = kj::mv(parent);
  }
  IoChannelFactory::SubrequestMetadata metadata{
    .cfBlobJson = kj::mv(cfBlobJson),
    .parentSpan = tracing.getInternalSpanParent(),
    .userSpanParent = kj::mv(propagatedUserSpanParent),
    .featureFlagsForFl = worker->getIsolate().getFeatureFlagsForFl().clone(),
  };

  auto client = channelFactory.startSubrequest(channel, kj::mv(metadata));

  return client;
}

kj::Own<kj::HttpClient> IoContext::getHttpClient(uint channel,
    bool isInHouse,
    kj::Maybe<kj::String> cfBlobJson,
    kj::ConstString operationName,
    SpanAwaitedNext spanAwaitedNext) {
  return asHttpClient(getSubrequestChannel(
      channel, isInHouse, kj::mv(cfBlobJson), kj::mv(operationName), spanAwaitedNext));
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

  // Apply external memory adjustment for Cache API subrequests (same as other subrequests in
  // getSubrequestNoChecks).
  KJ_IF_SOME(lock, currentLock) {
    jsg::Lock& js = lock;
    ret = ret.attach(js.getExternalMemoryAdjustment(8 * 1024));
  }

  return kj::mv(ret);
}

jsg::AsyncContextFrame::StorageScope IoContext::makeAsyncTraceScope(
    Worker::Lock& lock, kj::Maybe<SpanParent> spanParentOverride) {
  return jsg::AsyncContextFrame::StorageScope(
      lock, lock.getTraceAsyncContextKey(), makeTraceScopeValue(lock, kj::mv(spanParentOverride)));
}

jsg::AsyncContextFrame::StorageScope IoContext::makeUserAsyncTraceScope(
    Worker::Lock& lock, kj::Maybe<SpanParent> userSpanOverride) {
  return jsg::AsyncContextFrame::StorageScope(lock, lock.getUserTraceAsyncContextKey(),
      makeUserTraceScopeValue(lock, kj::mv(userSpanOverride)));
}

jsg::AsyncContextFrame::StorageScope IoContext::makeRequestTraceScope(Worker::Lock& lock) {
  jsg::AsyncContextFrame::StorageEntry entries[] = {
    {lock.getTraceAsyncContextKey(), makeTraceScopeValue(lock, kj::none)},
    {lock.getUserTraceAsyncContextKey(), makeUserTraceScopeValue(lock, kj::none)},
  };
  return jsg::AsyncContextFrame::StorageScope(lock, entries);
}

jsg::Value IoContext::makeTraceScopeValue(
    Worker::Lock& lock, kj::Maybe<SpanParent> spanParentOverride) {
  auto& ioContext = IoContext::current();
  jsg::Lock& js = lock;
  auto spanParent = kj::mv(spanParentOverride).orDefault([&]() { return getMetrics().getSpan(); });
  if (!spanParent.isObserved()) {
    return js.v8Ref(jsg::wrapOpaque(js.v8Context(), TraceScopeState(ioContext.getId())));
  }
  auto ioOwnSpanParent = ioContext.addObject(kj::heap(kj::mv(spanParent)));
  auto spanHandle = jsg::wrapOpaque(js.v8Context(), TraceScopeState(kj::mv(ioOwnSpanParent)));
  return js.v8Ref(spanHandle);
}

jsg::Value IoContext::makeUserTraceScopeValue(
    Worker::Lock& lock, kj::Maybe<SpanParent> userSpanOverride) {
  auto& ioContext = IoContext::current();
  jsg::Lock& js = lock;
  SpanParent userSpan(nullptr);
  SpanParent invocationRoot(nullptr);
  kj::Maybe<tracing::InvocationSpanContext> invocationSpanContext;
  kj::Maybe<kj::Own<workerd::WeakRef<BaseTracer>>> tracer;
  bool otel = opentelemetryApiEnabled;
  bool inherited = false;
  bool localSampled = false;
  bool rootObserved = false;
  bool tracerLess = true;
  KJ_IF_SOME(sp, kj::mv(userSpanOverride)) {
    userSpan = kj::mv(sp);
    if (otel) {
      inherited = hasCurrentUserTraceOwner();
      KJ_IF_SOME(owner, getCurrentUserTraceAsyncContext()) {
        invocationRoot = owner.getInvocationRoot();
        localSampled = owner.getLocalSampled();
        rootObserved = owner.isRootObserved();
        tracerLess = owner.isTracerLess();
        invocationSpanContext =
            owner.getInvocationSpanContext().map([](auto& ctx) { return ctx.clone(); });
        KJ_IF_SOME(value, owner.getTracer()) {
          tracer = value.addRef();
        }
      }
    }
  } else {
    userSpan = getRootUserTraceSpan();
  }

  // Fresh request scopes and overrides without a captured owner inherit the current request.
  // RPC, constructor, restore and hibernatable WebSocket entry points install request scopes.
  // A captured owner without a tracer is deliberately not replaced.
  if ((!otel || !inherited) && !incomingRequests.empty()) {
    if (otel) invocationRoot = getRootUserTraceSpan();
    if (!otel && !userSpan.isObserved()) {
      return js.v8Ref(jsg::wrapOpaque(js.v8Context(), UserTraceScopeState(ioContext.getId())));
    }
    KJ_IF_SOME(value, getCurrentIncomingRequest().getWorkerTracer()) {
      tracer = value.getWeakRef();
    }
    localSampled = getCurrentIncomingRequest().localSampled;
    rootObserved = getCurrentIncomingRequest().getRootUserTraceSpan().isObserved();
    tracerLess = getCurrentIncomingRequest().tracerLess;
    if (!otel) {
      auto& base = getCurrentIncomingRequest().getInvocationSpanContext();
      auto id = userSpan.getSpanId();
      invocationSpanContext = id != tracing::SpanId::nullId
          ? tracing::InvocationSpanContext(base.getTraceId(), base.getInvocationId(), id,
                base.getTraceFlags(), base.cloneTraceState())
          : base.clone();
    } else if (tracer != kj::none ||
        getCurrentIncomingRequest().maybeTriggerInvocationSpan != kj::none) {
      invocationSpanContext = getCurrentIncomingRequest().getInvocationSpanContext().clone();
    }
  }

  if (!userSpan.isObserved() && tracer == kj::none && invocationSpanContext == kj::none) {
    return js.v8Ref(jsg::wrapOpaque(js.v8Context(), UserTraceScopeState(ioContext.getId())));
  }

  auto asyncContext = kj::heap<UserTraceAsyncContext>(kj::mv(userSpan), kj::mv(tracer),
      kj::mv(invocationSpanContext), tracing::TraceFlags(localSampled ? 1 : 0),
      tracerLess ? UserTraceAsyncContext::TracerPresence::ABSENT
                 : UserTraceAsyncContext::TracerPresence::PRESENT,
      rootObserved ? RootUserSpanObserved::YES : RootUserSpanObserved::NO, kj::mv(invocationRoot));
  auto ioOwnAsyncContext = ioContext.addObject(kj::mv(asyncContext));
  auto contextHandle =
      jsg::wrapOpaque(js.v8Context(), UserTraceScopeState(kj::mv(ioOwnAsyncContext)));
  return js.v8Ref(contextHandle);
}

bool IoContext::hasCurrentUserTraceOwner() {
  KJ_IF_SOME(lock, currentLock) {
    KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(lock)) {
      KJ_IF_SOME(value, frame.get(*lock.getUserTraceAsyncContextKey())) {
        jsg::Lock& js = lock;
        auto& state = jsg::unwrapOpaqueRef<UserTraceScopeState>(js.v8Isolate, value.getHandle(js));
        KJ_SWITCH_ONEOF(state) {
          KJ_CASE_ONEOF(id, Id) {
            return id.isCurrent();
          }
          KJ_CASE_ONEOF(owner, IoOwn<UserTraceAsyncContext>) {
            return owner.tryGet() != kj::none;
          }
        }
      }
    }
  }
  return false;
}

kj::Maybe<UserTraceAsyncContext&> IoContext::getCurrentUserTraceAsyncContext() {
  KJ_IF_SOME(lock, currentLock) {
    KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(lock)) {
      KJ_IF_SOME(value, frame.get(*lock.getUserTraceAsyncContextKey())) {
        jsg::Lock& js = lock;
        auto& state = jsg::unwrapOpaqueRef<UserTraceScopeState>(js.v8Isolate, value.getHandle(js));
        KJ_SWITCH_ONEOF(state) {
          KJ_CASE_ONEOF(id, Id) {
            return kj::none;
          }
          KJ_CASE_ONEOF(owner, IoOwn<UserTraceAsyncContext>) {
            return owner.tryGet();
          }
        }
      }
    }
  }
  return kj::none;
}

kj::Maybe<BaseTracer&> IoContext::getCurrentUserTracer() {
  KJ_IF_SOME(owner, getCurrentUserTraceAsyncContext()) {
    KJ_IF_SOME(weak, owner.getTracer()) {
      // Synchronous single-threaded use is safe until re-entering user JS. Callers that do
      // re-enter JS must clone the tracer first so property getters cannot invalidate it.
      return weak.tryGet();
    }
  }
  return kj::none;
}

kj::Maybe<BaseTracer&> IoContext::getWorkerTracer() {
  if (incomingRequests.empty()) return kj::none;
  if (!opentelemetryApiEnabled) {
    return getCurrentIncomingRequest().getWorkerTracer();
  }
  if (hasCurrentUserTraceOwner()) {
    // Logs, diagnostics and exceptions reported while the frame is entered use its span sink.
    // Uncaught task failures (including timers) are reported after the frame unwinds and still
    // use the newest request. An entered stale/untraced frame must not borrow that tracer.
    return getCurrentUserTracer();
  }
  return getCurrentIncomingRequest().getWorkerTracer();
}

bool IoContext::isRootUserTraceObserved() {
  KJ_IF_SOME(owner, getCurrentUserTraceAsyncContext()) return owner.isRootObserved();
  if (hasCurrentUserTraceOwner() || incomingRequests.empty()) return false;
  return getRootUserTraceSpan().isObserved();
}

SpanParent IoContext::getUserTraceInvocationRoot() {
  if (opentelemetryApiEnabled) {
    KJ_IF_SOME(owner, getCurrentUserTraceAsyncContext()) return owner.getInvocationRoot();
    if (hasCurrentUserTraceOwner()) return SpanParent(nullptr);
  }
  return getRootUserTraceSpan();
}

kj::Maybe<tracing::InvocationSpanContext> IoContext::getUserTraceInvocationContext() {
  KJ_IF_SOME(owner, getCurrentUserTraceAsyncContext()) {
    return owner.getInvocationSpanContext().map([](auto& context) { return context.clone(); });
  }
  if (opentelemetryApiEnabled && hasCurrentUserTraceOwner()) return kj::none;
  if (incomingRequests.empty()) return kj::none;
  if (opentelemetryApiEnabled && getCurrentIncomingRequest().getWorkerTracer() == kj::none &&
      getCurrentIncomingRequest().maybeTriggerInvocationSpan == kj::none)
    return kj::none;
  return getCurrentIncomingRequest().getInvocationSpanContext().clone();
}

kj::Maybe<tracing::TraceFlags> IoContext::getUserTraceInvocationFlags() {
  KJ_IF_SOME(owner, getCurrentUserTraceAsyncContext()) {
    KJ_IF_SOME(context, owner.getInvocationSpanContext()) return context.getTraceFlags();
    return kj::none;
  }
  if (opentelemetryApiEnabled && hasCurrentUserTraceOwner()) return kj::none;
  if (incomingRequests.empty()) return kj::none;
  if (opentelemetryApiEnabled && getCurrentIncomingRequest().getWorkerTracer() == kj::none &&
      getCurrentIncomingRequest().maybeTriggerInvocationSpan == kj::none)
    return kj::none;
  return getCurrentIncomingRequest().getInvocationSpanContext().getTraceFlags();
}

kj::Maybe<tracing::InvocationSpanContext> IoContext::getUserTraceInvocationIdentity() {
  if (!opentelemetryApiEnabled) return getUserTraceInvocationContext();
  auto context = getUserTraceInvocationContext();
  bool tracerLess = true;
  KJ_IF_SOME(owner, getCurrentUserTraceAsyncContext()) {
    tracerLess = owner.isTracerLess();
  } else if (!incomingRequests.empty()) {
    tracerLess = getCurrentIncomingRequest().tracerLess;
  }
  if (tracerLess) {
    KJ_IF_SOME(root, context) {
      KJ_IF_SOME(trigger, root.getParent()) {
        return tracing::InvocationSpanContext(root.getTraceId(), root.getInvocationId(),
            trigger.getSpanId(), root.getTraceFlags(), root.cloneTraceState());
      }
      return kj::none;
    }
  }
  return context;
}

kj::Maybe<tracing::SpanContext> IoContext::getUserTraceWebSocketContext() {
  // Detached traces belong to one recording session, not the socket's later invocations.
  auto parent = getCurrentUserTraceSpan();
  KJ_IF_SOME(active, parent.toSpanContext()) {
    bool detached =
        parent.getObserver().map([](auto& observer) {
      return observer.isDetachedTrace();
    }).orDefault(false);
    // Only rewrapped identities need membership under this request's key. A retained native
    // observer carries its original provenance even after its creator's tracer has expired.
    detached = detached ||
        getWorkerTracer()
            .map([&](auto& tracer) {
      return tracer.isDetachedTraceId(active.getTraceId());
    }).orDefault(false);
    KJ_IF_SOME(id, active.getSpanId()) {
      if (active.getTraceId() != nullptr && id != nullptr && !detached) {
        return tracing::SpanContext(active.getTraceId(), id);
      }
    }
  }
  KJ_IF_SOME(identity, getUserTraceInvocationIdentity()) {
    return tracing::SpanContext(identity.getTraceId(), identity.getSpanId());
  }
  return kj::none;
}

kj::Maybe<tracing::TraceFlags> IoContext::getDefaultJsTraceFlags() {
  if (!opentelemetryApiEnabled) return kj::none;
  KJ_IF_SOME(owner, getCurrentUserTraceAsyncContext()) {
    return tracing::TraceFlags(owner.getLocalSampled() ? 1 : 0);
  }
  if (hasCurrentUserTraceOwner()) return tracing::TraceFlags(0);
  return tracing::TraceFlags(
      !incomingRequests.empty() && getCurrentIncomingRequest().localSampled ? 1 : 0);
}

tracing::InvocationSpanContext IoContext::getInvocationSpanContext() {
  if (opentelemetryApiEnabled) {
    KJ_IF_SOME(owner, getCurrentUserTraceAsyncContext()) {
      KJ_IF_SOME(context, owner.getSpanInvocationContext()) {
        return kj::mv(context);
      }
    }
  }
  auto userSpan = getCurrentUserTraceSpan();
  return getCurrentIncomingRequest().getInvocationSpanContextForUserSpan(userSpan);
}

SpanParent IoContext::getCurrentTraceSpan() {
  // If called while lock is held, try to use the trace info stored in the async context.
  KJ_IF_SOME(lock, currentLock) {
    KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(lock)) {
      KJ_IF_SOME(value, frame.get(*lock.getTraceAsyncContextKey())) {
        auto handle = value.getHandle(lock);
        jsg::Lock& js = lock;
        auto& state = jsg::unwrapOpaqueRef<TraceScopeState>(js.v8Isolate, handle);
        // The frame may have been captured by a different request (e.g. via
        // AsyncLocalStorage.snapshot()), whose span this request can't use.
        KJ_SWITCH_ONEOF(state) {
          KJ_CASE_ONEOF(id, Id) {
            if (id.isCurrent()) return SpanParent(nullptr);
          }
          KJ_CASE_ONEOF(spanParent, IoOwn<SpanParent>) {
            KJ_IF_SOME(span, spanParent.tryGet()) {
              return span.addRef();
            }
          }
        }
      }
    }
  }

  // If async context is unavailable (unset, from another request, or JS lock is not held), fall
  // back to heuristic of using the trace info from the most recent active request.
  return getMetrics().getSpan();
}

SpanParent IoContext::getCurrentUserTraceSpan() {
  if (incomingRequests.empty()) {
    return SpanParent(nullptr);
  }

  // If called while lock is held, try to use the trace info stored in the async context.
  KJ_IF_SOME(lock, currentLock) {
    KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(lock)) {
      KJ_IF_SOME(value, frame.get(*lock.getUserTraceAsyncContextKey())) {
        auto handle = value.getHandle(lock);
        jsg::Lock& js = lock;
        auto& state = jsg::unwrapOpaqueRef<UserTraceScopeState>(js.v8Isolate, handle);
        // As in getCurrentTraceSpan(), the frame may belong to a different request, in which case
        // fall back to this request's root span.
        KJ_SWITCH_ONEOF(state) {
          KJ_CASE_ONEOF(id, Id) {
            if (id.isCurrent()) return SpanParent(nullptr);
          }
          KJ_CASE_ONEOF(asyncContext, IoOwn<UserTraceAsyncContext>) {
            KJ_IF_SOME(context, asyncContext.tryGet()) {
              return context.getSpan();
            }
          }
        }
      }
    }
  }
  return getCurrentIncomingRequest().getRootUserTraceSpan();
}

SpanBuilder IoContext::makeTraceSpan(kj::ConstString operationName) {
  return getCurrentTraceSpan().newChild(kj::mv(operationName));
}

TraceContext IoContext::makeUserTraceSpan(kj::ConstString operationName) {
  TraceContextParent parents(getCurrentTraceSpan(), getCurrentUserTraceSpan());
  return parents.newChild(kj::mv(operationName), tryGetAsyncTracker());
}

void IoContext::taskFailed(kj::Exception&& exception) {
  if (waitUntilStatusValue == EventOutcome::OK) {
    KJ_IF_SOME(status, limitEnforcer->getLimitsExceeded()) {
      waitUntilStatusValue = status;
    } else {
      waitUntilStatusValue = RequestObserver::outcomeFromException(exception);
    }
  }

  // If `taskFailed()` throws the whole event loop blows up... let's be careful not to let that
  // happen.
  KJ_IF_SOME(e, kj::runCatchingExceptions([&]() {
    // There may no longer be an incoming request at task fail time – provide kj::none to indicate
    // that the tracer and metrics should be taken from the current request, if available
    logUncaughtExceptionAsync(UncaughtExceptionSource::ASYNC_TASK, kj::mv(exception), kj::none);
  })) {
    KJ_LOG(ERROR, "logUncaughtExceptionAsync() threw an exception?", e);
  }
}

void IoContext::requireCurrent() {
  KJ_REQUIRE(isCurrent(), "request is not current in this isolate");
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

  // Spans the lock wait, the JavaScript, and the microtask drain, and ends before the lock is
  // released. Without a better-known cause (set by whatever resumed JavaScript), the turn is
  // attributed to the current request.
  AsyncTracker::TurnScope turnScope(tryGetAsyncTracker(),
      incomingRequests.empty() ? 0 : incomingRequests.front().getAsyncTraceId());

  runInContextScope(lockType, kj::mv(inputLock), [&](Worker::Lock& workerLock) {
    turnScope.locked();
    // Declared before the microtask drain's KJ_DEFER below, so it runs after it, still under the
    // lock.
    KJ_DEFER(turnScope.end());
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

      // With --gc-stress, force a full GC after microtasks run. This catches objects that
      // became unreachable during JS execution / microtask processing (e.g., a
      // ReadableStreamDefaultReader with no JS variable binding whose closed promise is
      // still pending).
      if (isGcStressModeForTest()) {
        workerLock.getIsolate()->RequestGarbageCollectionForTesting(
            v8::Isolate::kFullGarbageCollection);
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

    // With --gc-stress, force a full GC before each awaitIo continuation. This helps detect
    // KJ async objects (promises, streams, etc.) stored on the JS heap without IoOwn
    // wrapping. Such objects crash under DISALLOW_KJ_IO_DESTRUCTORS_SCOPE when collected
    // by GC, but normally the timing window is too brief to hit. Forcing GC at every
    // continuation makes these bugs deterministic.
    if (isGcStressModeForTest()) {
      workerLock.getIsolate()->RequestGarbageCollectionForTesting(
          v8::Isolate::kFullGarbageCollection);
    }

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
          kj::throwException(e.clone());
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
  if (!hasCurrent()) {
    v8::Isolate* isolate = v8::Isolate::TryGetCurrent();
    KJ_REQUIRE(isolate != nullptr, "there is no current request on this thread");
    isolate->ThrowError(jsg::v8StrIntern(isolate, kAsyncIoErrorMessage));
    throw jsg::JsExceptionThrown();
  } else {
    return *threadLocalRequest;
  }
}

kj::Maybe<IoContext&> IoContext::tryCurrent() {
  if (hasCurrent()) {
    return *threadLocalRequest;
  } else {
    return kj::none;
  }
}

bool IoContext::hasCurrent() {
  return threadLocalRequest != nullptr && threadLocalRequest->isCurrent();
}

bool IoContext::isCurrent() {
  if (this != threadLocalRequest) return false;

  // An IoContext is only current while its isolate is entered.
  KJ_IF_SOME(lock, currentLock) {
    return lock.getIsolate() == v8::Isolate::TryGetCurrent();
  }
  return false;
}

IoContext::Id IoContext::nextId() {
  // Monotonically increasing, never reused. See getId().
  static std::atomic<uint64_t> counter{0};
  return Id(counter.fetch_add(1, std::memory_order_relaxed));
}

bool IoContext::Id::isCurrent() const {
  KJ_IF_SOME(ctx, IoContext::tryCurrent()) {
    return ctx.getId() == *this;
  }
  return false;
}

void IoContext::setEntrypointHandler(jsg::Lock& js, jsg::JsObject handler) {
  KJ_IF_SOME(_, entrypointHandler) {
    KJ_FAIL_REQUIRE("entrypoint handler has already been set");
  }
  entrypointHandler = jsg::JsRef<jsg::JsObject>(js, handler);
}

jsg::JsObject IoContext::getEntrypointHandler(jsg::Lock& js) {
  return KJ_REQUIRE_NONNULL(entrypointHandler, "entrypoint handler has not been set").getHandle(js);
}

kj::Maybe<kj::Rc<IoChannelFactory::SelfTokenFactory>> IoContext::getSelfTokenFactory() {
  // Scan all IncomingRequests and return the first self-token factory we find. Scanning them all,
  // rather that just using the "current" IncomingRequest, avoids unpredictable behavior in the
  // presence of concurrent requests where some requests came in through a path that doesn't have a
  // self-token factory available. In practice, though, applications ought to be consistent and
  // either always use ctx.restore() or never use it for a particular dynamic worker or facet.
  for (auto& req: incomingRequests) {
    KJ_IF_SOME(s, req.selfTokenFactory) {
      return s.clone();
    }
  }
  return kj::none;
}

auto IoContext::tryGetWeakRefForCurrent() -> kj::Maybe<kj::Own<WeakRef>> {
  KJ_IF_SOME(ioContext, tryCurrent()) {
    return ioContext.getWeakRef();
  } else {
    return kj::none;
  }
}

kj::Maybe<IoContext::Id> IoContext::tryGetCurrentId() {
  return tryCurrent().map([](IoContext& ioContext) { return ioContext.getId(); });
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

void IoContext::requireCurrentOrThrowJs(Id id) {
  if (!id.isCurrent()) {
    throwNotCurrentJsError();
  }
}

void IoContext::throwNotCurrentJsError(kj::Maybe<const std::type_info&> maybeType) {
  auto type = maybeType
                  .map([](const std::type_info& type) {
    return kj::str(" (I/O type: ", jsg::typeName(type), ")");
  }).orDefault(kj::String());

  auto isActor = tryCurrent().map([](IoContext& context) {
    return context.actor != kj::none;
  }).orDefault(false);
  if (isActor) {
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
