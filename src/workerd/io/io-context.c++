// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "io-context.h"
#include <workerd/io/io-gate.h>
#include <workerd/io/worker.h>
#include <kj/threadlocal.h>
#include <kj/debug.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/sentry.h>
#include <map>

namespace workerd {

KJ_THREADLOCAL_PTR(IoContext) threadLocalRequest = nullptr;
KJ_THREADLOCAL_PTR(void) threadId = nullptr;

static void* getThreadId() {
  if (threadId == nullptr) threadId = new int;
  return threadId;
}

void IoContext::DeleteQueue::scheduleDeletion(OwnedObject* object) const {
  if (threadLocalRequest != nullptr && threadLocalRequest->deleteQueue.get() == this) {
    // Deletion from same thread. No need to enqueue.
    kj::AllowAsyncDestructorsScope scope;
    OwnedObjectList::unlink(*object);
  } else {
    auto lock = crossThreadDeleteQueue.lockExclusive();
    KJ_IF_MAYBE(state, *lock) {
      state->queue.add(object);
    }
  }
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
      return nullptr;
    } else {
      return timeoutTimes.begin()->key.when;
    }
  }

private:
  struct IdAndIterator {
    TimeoutId id;
    Iterator it;
  };
  IdAndIterator addState(TimeoutId::Generator& generator, TimeoutParameters params);

  void setTimeoutImpl(IoContext& context, Iterator it);

  struct TimeoutTime {
    // A pair of a Date and a numeric ID, used as entry in timeoutTimes set, below.

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

  kj::TreeMap<TimeoutTime, kj::Own<kj::PromiseFulfiller<void>>> timeoutTimes;
  uint timeoutTimesTiebreakerCounter = 0;
  // Tracks registered timeouts sorted by the next time the timeout is expected to fire.
  //
  // The associated fulfiller should be fulfilled when the time has been reached AND all previous
  // timeouts have completed.

  uint timeoutsStarted = 0;
  uint timeoutsFinished = 0;
  Map timeouts;

  kj::Promise<void> timerTask = nullptr;
  // Promise that is waiting for the closest timeout, and will fulfill its fulfiller. We only ever
  // actually wait on the next timeout in `timeoutTasks`, so that we can't fulfill timer callbacks
  // out-of-order. This task gets replaced each time the lead timeout changes.

  void resetTimerTask(TimerChannel& timerChannel);
  // Must be called any time timeoutTimes.begin() changes.
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

ThreadContext::HeaderIdBundle::HeaderIdBundle(kj::HttpHeaderTable::Builder& builder)
    : table(builder.getFutureTable()),
      contentEncoding(builder.add("Content-Encoding")),
      cfCacheStatus(builder.add("CF-Cache-Status")),
      cacheControl(builder.add("Cache-Control")),
      cfCacheNamespace(builder.add("CF-Cache-Namespace")),
      cfKvMetadata(builder.add("CF-KV-Metadata")),
      cfR2ErrorHeader(builder.add("CF-R2-Error")),
      cfBlobMetadataSize(builder.add("CF-R2-Metadata-Size")),
      cfBlobRequest(builder.add("CF-R2-Request")),
      authorization(builder.add("Authorization")),
      secWebSocketProtocol(builder.add("Sec-WebSocket-Protocol")) {}

ThreadContext::ThreadContext(
    kj::Timer& timer, kj::EntropySource& entropySource,
    HeaderIdBundle headerIds, capnp::HttpOverCapnpFactory& httpOverCapnpFactory, capnp::ByteStreamFactory& byteStreamFactory, bool fiddle)
    : timer(timer),
      entropySource(entropySource),
      headerIds(headerIds),
      httpOverCapnpFactory(httpOverCapnpFactory),
      byteStreamFactory(byteStreamFactory),
      fiddle(fiddle) {}

IoContext::IoContext(ThreadContext& thread,
                               kj::Own<const Worker> workerParam,
                               kj::Maybe<Worker::Actor&> actorParam,
                               kj::Own<LimitEnforcer> limitEnforcerParam)
    : thread(thread),
      worker(kj::mv(workerParam)),
      actor(actorParam),
      limitEnforcer(kj::mv(limitEnforcerParam)),
      threadId(getThreadId()),
      deleteQueue(kj::atomicRefcounted<DeleteQueue>()),
      waitUntilTasks(*this),
      timeoutManager(kj::heap<TimeoutManagerImpl>()) {
  kj::PromiseFulfillerPair<void> paf = kj::newPromiseAndFulfiller<void>();
  abortFulfiller = kj::mv(paf.fulfiller);
  auto localAbortPromise = kj::mv(paf.promise);

  // Arrange to complain if execution resource limits (CPU/memory) are exceeded.
  auto makeLimitsPromise = [this](){
    auto promise = limitEnforcer->onLimitsExceeded();
    if (isInspectorEnabled()) {
      // Arrange to report the problem to the inspector in addition to aborting.
      promise = promise.catch_([this](kj::Exception&& exception) {
        return worker->takeAsyncLockWithoutRequest(nullptr)
            .then([this, exception = kj::mv(exception)]
                  (Worker::AsyncLock asyncLock) mutable -> kj::Promise<void> {
          Worker::Lock lock(*worker, asyncLock);
          lock.logUncaughtException(
              jsg::extractTunneledExceptionDescription(exception.getDescription()));
          return kj::mv(exception);
        });
      });
    }

    return promise;
  };

  localAbortPromise = localAbortPromise.exclusiveJoin(makeLimitsPromise());

  KJ_IF_MAYBE(a, actor) {
    // Arrange to complain if the input gate is broken, which indicates a critical section failed
    // and the actor can no longer be used.
    localAbortPromise = localAbortPromise.exclusiveJoin(a->getInputGate().onBroken());

    // Stop the ActorCache from flushing any scheduled write operations to prevent any unnecessary
    // or unintentional async work
    localAbortPromise = localAbortPromise.catch_([this](kj::Exception&& e) -> kj::Promise<void> {
      KJ_IF_MAYBE(a, actor) {
        a->shutdownActorCache(e);
      }

      return kj::mv(e);
    });
  }

  // Abort when the time limit expires, the isolate is terminated, the input gate is broken, or
  // `abortFulfiller` is fulfilled for some other reason.
  abortPromise = localAbortPromise.fork();

  // We don't construct `tasks` for actor requests because we put all tasks into `waitUntilTasks`
  // in that case.
  if (actor == nullptr) {
    kj::TaskSet::ErrorHandler& errorHandler = *this;
    tasks.emplace(errorHandler);
  }
}

IoContext::IncomingRequest::IoContext_IncomingRequest(
    kj::Own<IoContext> contextParam,
    kj::Own<IoChannelFactory> ioChannelFactoryParam,
    kj::Own<RequestObserver> metricsParam,
    kj::Maybe<kj::Own<WorkerTracer>> workerTracer)
    : context(kj::mv(contextParam)),
      metrics(kj::mv(metricsParam)),
      workerTracer(kj::mv(workerTracer)),
      ioChannelFactory(kj::mv(ioChannelFactoryParam)) {}

void IoContext::IncomingRequest::delivered() {
  KJ_REQUIRE(!wasDelivered, "delivered() can only be called once");

  // A call to delivered() implies a promise to call drain() later (or one of the other methods
  // that sets waitedForWaitUntil). So, we can now safely add the request to
  // context->incomingRequests, which implies taking responsibility for draining on the way out.

  if (!context->incomingRequests.empty()) {
    // There is already an IncomingRequest running in this context, and we're going to make it no
    // longer current. Make sure to attribute accumulated CPU time to it.
    auto& oldFront = context->incomingRequests.front();
    context->limitEnforcer->reportMetrics(*oldFront.metrics);

    KJ_IF_MAYBE(f, oldFront.drainFulfiller) {
      // Allow the previous current IncomingRequest to finish draining, because the new request
      // will take over responsibility for completing any tasks that aren't done yet.
      f->get()->fulfill();
    }
  }

  context->incomingRequests.addFront(*this);
  wasDelivered = true;
  metrics->delivered();

  KJ_IF_MAYBE(a, context->actor) {
    // Re-synchronize the timer and top up limits for every new incoming request to an actor.
    ioChannelFactory->getTimer().syncTime();
    context->limitEnforcer->topUpActor();

    // Run the Actor's constructor if it hasn't been run already.
    a->ensureConstructed(*context);

    // Record a new incoming request to actor metrics.
    a->getMetrics().startRequest();
  }
}

IoContext::IncomingRequest::~IoContext_IncomingRequest() noexcept(false) {
  if (!wasDelivered) {
    // Request was never added to context->incomingRequests in the first place.
    return;
  }

  if (&context->incomingRequests.front() == this) {
    // We're the current request, make sure to consume CPU time attribution.
    context->limitEnforcer->reportMetrics(*metrics);

    if (!waitedForWaitUntil && !context->waitUntilTasks.isEmpty()) {
      KJ_LOG(WARNING, "failed to invoke drain() on IncomingRequest before destroying it",
          kj::getStackTrace());
    }
  }

  context->incomingRequests.remove(*this);

  KJ_IF_MAYBE(a, context->actor) {
    a->getMetrics().endRequest();
  }
  context->worker->getIsolate().completedRequest();
  metrics->jsDone();
}

IoContext::~IoContext() noexcept(false) {
  // Kill the sentinel so that no weak references can refer to this IoContext anymore.
  selfRef->kill();
}

InputGate::Lock IoContext::getInputLock() {
  return KJ_ASSERT_NONNULL(currentInputLock,
      "no input lock available in this context").addRef();
}

kj::Maybe<kj::Own<InputGate::CriticalSection>> IoContext::getCriticalSection() {
  KJ_IF_MAYBE(l, currentInputLock) {
    return l->getCriticalSection()
        .map([](InputGate::CriticalSection& cs) { return kj::addRef(cs); });
  } else {
    return nullptr;
  }
}

kj::Promise<void> IoContext::waitForOutputLocks() {
  KJ_IF_MAYBE(p, waitForOutputLocksIfNecessary()) {
    return kj::mv(*p);
  } else {
    return kj::READY_NOW;
  }
}

bool IoContext::hasOutputGate() {
  return actor != nullptr;
}

kj::Maybe<kj::Promise<void>> IoContext::waitForOutputLocksIfNecessary() {
  return actor.map([](Worker::Actor& actor) {
    return actor.getOutputGate().wait();
  });
}

kj::Maybe<IoOwn<kj::Promise<void>>> IoContext::waitForOutputLocksIfNecessaryIoOwn() {
  return waitForOutputLocksIfNecessary().map([this](kj::Promise<void> promise) {
    return addObject(kj::heap(kj::mv(promise)));
  });
}

bool IoContext::isOutputGateBroken() {
  KJ_IF_MAYBE(a, actor) {
    return a->getOutputGate().isBroken();
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

void IoContext::logUncaughtException(UncaughtExceptionSource source,
                                          v8::Local<v8::Value> exception,
                                          v8::Local<v8::Message> message) {
  KJ_REQUIRE_NONNULL(currentLock).logUncaughtException(source, exception, message);
}

void IoContext::logUncaughtExceptionAsync(UncaughtExceptionSource source,
                                               kj::Exception&& exception) {
  if (getWorkerTracer() == nullptr &&
      !worker->getIsolate().isInspectorEnabled()) {
    // We don't need to take the isolate lock as neither inspecting nor tracing is enabled. We
    // do still want to syslog if relevant, but we can do that without a lock.
    if (!jsg::isTunneledException(exception.getDescription()) &&
        !jsg::isDoNotLogException(exception.getDescription()) &&
        // TODO(soon): Figure out why client disconncects are getting logged here if we don't
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
        : source(source), exception(kj::mv(exception)) {}
    void run(Worker::Lock& lock) override {
      jsg::Lock& js = lock;
      auto error = js.exceptionToJs(kj::mv(exception));
      // TODO(soon): Add logUncaughtException to jsg::Lock.
      lock.logUncaughtException(source, error.getHandle(js));
    }
  };

  // Make sure this is logged even if another exception occurs trying to log it to the devtools inspector,
  // e.g. if `runImpl` throws before calling logUncaughtException.
  // This is useful for tests (and in fact only affects tests, since it's logged at an INFO level).
  KJ_ON_SCOPE_FAILURE({
    KJ_LOG(INFO, "uncaught exception", source, exception);
  });
  RunnableImpl runnable(source, kj::mv(exception));
  // TODO(perf): Is it worth using an async lock here? The only case where it really matters is
  //   when a trace worker is active, but maybe they'll be more common in the future. To take an
  //   async lock here, we'll probably have to update all the call sites of this method... ick.
  kj::Maybe<RequestObserver&> metrics;
  if (!incomingRequests.empty()) metrics = getMetrics();
  runImpl(runnable, false, Worker::Lock::TakeSynchronously(metrics), nullptr, true);
}

void IoContext::reportPromiseRejectEvent(v8::PromiseRejectMessage& message) {
  KJ_REQUIRE_NONNULL(currentLock).reportPromiseRejectEvent(message);
}

void IoContext::addTask(kj::Promise<void> promise) {
  ++addTaskCounter;

  // In Actors, we treat all tasks as wait-until tasks, because it's perfectly legit to start a
  // task under one request and then expect some other request to handle it later.
  if (actor != nullptr) {
    addWaitUntil(kj::mv(promise));
    return;
  }

  auto& tasks = KJ_ASSERT_NONNULL(this->tasks, "I/O context finalized");

  if (actor == nullptr) {
    // This metric won't work correctly in actors since it's being tracked per-request, but tasks
    // are not tied to requests in actors. So we just skip it in actors. (Actually this code path
    // is not even executed in the actor case but I'm leaving the check in just in case that ever
    // changes.)
    auto& metrics = getMetrics();
    if (metrics.getSpan().isObserved()) {
      metrics.addedContextTask();
      promise = promise.attach(kj::defer([metrics = kj::addRef(metrics)]() mutable {
        metrics->finishedContextTask();
      }));
    }
  }

  tasks.add(kj::mv(promise));
}

void IoContext::addWaitUntil(kj::Promise<void> promise) {
  if (actor == nullptr) {
    // This metric won't work correctly in actors since it's being tracked per-request, but tasks
    // are not tied to requests in actors. So we just skip it in actors.
    auto& metrics = getMetrics();
    if (metrics.getSpan().isObserved()) {
      metrics.addedWaitUntilTask();
      promise = promise.attach(kj::defer([metrics = kj::addRef(metrics)]() mutable {
        metrics->finishedWaitUntilTask();
      }));
    }
  }

  waitUntilTasks.add(kj::mv(promise));
}

kj::Promise<void> IoContext::IncomingRequest::drain() {
  // Mark ourselves so we know that we made a best effort attempt to wait for waitUntilTasks.
  waitedForWaitUntil = true;

  if (&context->incomingRequests.front() != this) {
    // A newer request was received, so draining isn't our job.
    return kj::READY_NOW;
  }

  kj::Promise<void> timeoutPromise = nullptr;
  KJ_IF_MAYBE(a, context->actor) {
    // For actors, all promises are canceled on actor shutdown, not on a fixed timeout,
    // because work doesn't necessarily happen on a per-request basis in actors and we don't want
    // work being unexpectedly canceled based on which request initiated it.
    timeoutPromise = a->onShutdown();

    // Also arrange to cancel the drain if a new request arrives, since it will take over
    // responsibility for background tasks.
    auto drainPaf = kj::newPromiseAndFulfiller<void>();
    drainFulfiller = kj::mv(drainPaf.fulfiller);
    timeoutPromise = timeoutPromise.exclusiveJoin(kj::mv(drainPaf.promise));
  } else {
    // For non-actor requests, apply the configured soft timeout, typically 30 seconds.
    timeoutPromise = context->limitEnforcer->limitDrain();
  }
  return context->waitUntilTasks.onEmpty().exclusiveJoin(kj::mv(timeoutPromise))
      .exclusiveJoin(context->abortPromise.addBranch().then([]{}, [](kj::Exception&&){}));
}

kj::Promise<bool> IoContext::IncomingRequest::finishScheduled() {
  // TODO(someday): In principle we should be able to support delivering the "scheduled" event type
  //   to an actor, and this may be important if we open up the whole of WorkerInterface to be
  //   callable from any stub. However, the logic around async tasks would have to be different. We
  //   cannot assume that just because an async task fails while the scheduled event is running,
  //   that the scheduled event itself failed -- the failure could have been a task initiated by
  //   an unrelated concurrent event.
  KJ_ASSERT(context->actor == nullptr,
      "this code isn't designed to allow scheduled events to be delivered to actors");

  // Mark ourselves so we know that we made a best effort attempt to wait for waitUntilTasks.
  KJ_ASSERT(context->incomingRequests.size() == 1);
  context->incomingRequests.front().waitedForWaitUntil = true;

  auto timeoutPromise = context->limitEnforcer->limitScheduled().then([] { return false; });
  return context->waitUntilTasks.onEmpty()
      .then([]() { return true; })
      .exclusiveJoin(kj::mv(timeoutPromise))
      .exclusiveJoin(context->abortPromise.addBranch()
          .then([] { return false; }, [](kj::Exception&&) { return false; }));
}

class IoContext::PendingEvent: public kj::Refcounted {
public:
  explicit PendingEvent(IoContext& context): context(context) {}
  ~PendingEvent() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(PendingEvent);

  IoContext& context;
};

IoContext::PendingEvent::~PendingEvent() noexcept(false) {
  context.pendingEvent = nullptr;

  // We can't execute finalizers just yet. We need to run the event loop to see if any queued
  // events come back into JavaScript. If registerPendingEvent() is called in the meantime, this
  // will be canceled.
  context.runFinalizersTask = Worker::AsyncLock::whenThreadIdle()
      .then([&context = context]() noexcept {
    // We have nothing left to do and no PendingEvent has been registered. Run finalizers now.
    return context.worker->takeAsyncLock(context.getMetrics()).then(
        [&context](Worker::AsyncLock asyncLock) {
      context.runFinalizers(asyncLock);
    });
  }).eagerlyEvaluate(nullptr);
}

kj::Own<void> IoContext::registerPendingEvent() {
  if (actor != nullptr) {
    // Actors don't use the pending event system, because different requests to the same Actor are
    // explicitly allowed to resolve each other's promises.
    return {};
  }

  KJ_IF_MAYBE(pe, pendingEvent) {
    return kj::addRef(*pe);
  } else {
    KJ_REQUIRE(!isFinalized(), "request has already been finalized");

    // Cancel any already-scheduled finalization.
    runFinalizersTask = nullptr;

    auto result = kj::refcounted<PendingEvent>(*this);
    pendingEvent = *result;
    return result;
  }
}

TimeoutId TimeoutId::Generator::getNext() {
  // The maximum integer value that we can represent as a double to convey to jsg.
  constexpr ValueType MAX_SAFE_INTEGER = (1ull << 53) - 1;

  auto id = nextId++;
  if (nextId > MAX_SAFE_INTEGER) {
    KJ_LOG(WARNING, "Unable to set timeout because there are no more unique ids");
    JSG_FAIL_REQUIRE(Error, "Unable to set timeout because there are no more unique ids "
        "less than Number.MAX_SAFE_INTEGER.");
  }
  return TimeoutId(id);
}

IoContext::TimeoutManagerImpl::TimeoutState::TimeoutState(
    TimeoutManagerImpl& manager, TimeoutParameters params)
    : manager(manager), params(kj::mv(params)) {
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
  auto cleanupGuard = kj::defer([&]{
    isRunning = false;
  });

  // Now it's safe to call the user's callback.
  KJ_IF_MAYBE(function, params.function) {
    (*function)(lock);
  }
}

void IoContext::TimeoutManagerImpl::TimeoutState::cancel() {
  if (isCanceled) {
    return;
  }

  auto wasCanceled = isCanceled;
  isCanceled = true;

  if (!isRunning && !wasCanceled) {
    params.function = nullptr;
    maybePromise = nullptr;
  }

  ++manager.timeoutsFinished;
}

auto IoContext::TimeoutManagerImpl::addState(
    TimeoutId::Generator& generator, TimeoutParameters params) -> IdAndIterator {
  JSG_REQUIRE(getTimeoutCount() < MAX_TIMEOUTS, DOMQuotaExceededError,
              "You have exceeded the number of timeouts you may set.",
              MAX_TIMEOUTS);

  auto id = generator.getNext();
  auto [it, wasEmplaced] = timeouts.try_emplace(id, *this, kj::mv(params));
  if (!wasEmplaced) {
    // We shouldn't have reached here because the `TimeoutId::Generator` throws if it reaches
    // Number.MAX_SAFE_INTEGER, much less wraps around the uint64_t number space. Let's throw with
    // as many details as possible.
    auto& state = it->second;
    auto delay = state.params.msDelay;
    auto repeat = state.params.repeat;
    KJ_FAIL_ASSERT("Saw a timeout id collision", getTimeoutCount(), id.toNumber(), delay, repeat);
  }

  return { id, it };
}

void IoContext::TimeoutManagerImpl::setTimeoutImpl(IoContext& context, Iterator it) {
  auto& state = it->second;

  auto stateGuard = kj::defer([&]() {
    if (state.maybePromise == nullptr) {
      // Something threw, erase the state.
      timeouts.erase(it);
    }
  });

  auto paf = kj::newPromiseAndFulfiller<void>();

  // Always schedule the timeout relative to what Date.now() currently returns, so that the delay
  // appear exact. Otherwise, the delay could reveal non-determinism containing side channels.
  auto when = context.now() + state.params.msDelay * kj::MILLISECONDS;
  // TODO(cleanup): The manual use of run() here (including carrying over the critical section) is
  //   kind of ugly, but using awaitIo() doesn't work here because we need the ability to cancel
  //   the timer, so we don't want to addTask() it, which awaitIo() does implicitly.
  auto promise = paf.promise
      .then([this, &context, it, cs = context.getCriticalSection()]() mutable {
    return context.run([this, &context, it] (Worker::Lock& lock) mutable {
      auto& state = it->second;

      auto stateGuard = kj::defer([&] {
        if (state.maybePromise == nullptr) {
          // At the end of this block, there was no new timeout, so we should remove the state.
          // Note that this can happen from cancelTimeout or a non-repeating timeout.
          timeouts.erase(it);
        }
      });

      if (state.isCanceled) {
        // We've been canceled before running. Nothing more to do.
        KJ_ASSERT(state.maybePromise == nullptr);
        return;
      }

      KJ_IF_MAYBE(promise, state.maybePromise) {
        // We could KJ_ASSERT_NONNULL(iter->second) instead if we are sure clearTimeout() couldn't
        // race us. However, I'm not sure about that.

        // First, move our timeout promise to the task set so it's safe to call clearInterval()
        // inside the user's callback. We don't yet null out the Maybe<Promise>, because we need to
        // be able to detect whether the user does call clearInterval(). We leave the actual map
        // entry in place because this aids in reporting cross-request-context timeout cancellation
        // errors to the user.
        context.addTask(kj::mv(*promise));

        // Because Promise has an underspecified move ctor, we need to explicitly nullify the Maybe
        // to indicate that we've consumed the promise.
        state.maybePromise = nullptr;

        // The user's callback might throw, but we need to at least attempt to reschedule interval
        // callbacks even if they throw. This deferred action takes care of that. Note that we don't
        // run the user's callback directly in this->run(), because that function throws a fatal
        // exception if a JS exception is thrown, which complicates our logic here.
        //
        // TODO(perf): If we can guarantee that `timeout->second = nullptr` will never throw, it
        //   might be worthwhile having an early-out path for non-interval timeouts.
        kj::UnwindDetector unwindDetector;
        KJ_DEFER(
          unwindDetector.catchExceptionsIfUnwinding([&] {
            if (state.isCanceled) {
              // The user's callback has called clearInterval(), nothing more to do.
              KJ_ASSERT(state.maybePromise == nullptr);
              return;
            }

            // If this is an interval task and the script has CPU time left, reschedule the task;
            // otherwise leave the dead map entry in place.
            if (state.params.repeat && context.limitEnforcer->getLimitsExceeded() == nullptr) {
              setTimeoutImpl(context, it);
            }
          });
        );

        state.trigger(lock);
      }
    }, kj::mv(cs));
  }, [](kj::Exception&&) {});

  promise = promise.attach(context.registerPendingEvent());

  // Add an entry to the timeoutTimes map, to track when the nearest timeout is. Arrange for it
  // to be removed when the promise completes.
  TimeoutTime timeoutTimesKey { when, timeoutTimesTiebreakerCounter++ };
  timeoutTimes.insert(timeoutTimesKey, kj::mv(paf.fulfiller));
  auto deferredTimeoutTimeRemoval = kj::defer(
      [this, &context, timeoutTimesKey]() {
    // If the promise is being destroyed due to IoContext teardown then IoChannelFactory may
    // no longer be available, but we can just skip starting a new timer in that case as it'd be
    // canceled anyway.
    if (context.selfRef->maybeContext != nullptr) {
      bool isNext = timeoutTimes.begin()->key == timeoutTimesKey;
      timeoutTimes.erase(timeoutTimesKey);
      if (isNext) resetTimerTask(context.getIoChannelFactory().getTimer());
    }
  });

  if (timeoutTimes.begin()->key == timeoutTimesKey) {
    resetTimerTask(context.getIoChannelFactory().getTimer());
  }
  promise = promise.attach(kj::mv(deferredTimeoutTimeRemoval));

  if (context.actor != nullptr) {
    // Add a wait-until task which resolves when this timer completes. This ensures that
    // `IncomingRequest::drain()` waits until all timers finish.
    auto paf = kj::newPromiseAndFulfiller<void>();
    promise = promise.attach(kj::defer([fulfiller = kj::mv(paf.fulfiller)]() mutable {
      fulfiller->fulfill();
    }));
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
    }).eagerlyEvaluate([](kj::Exception&& e) {
      KJ_LOG(ERROR, e);
    });
  }
}

void IoContext::TimeoutManagerImpl::clearTimeout(
    IoContext& context, TimeoutId timeoutId) {
  auto timeout = timeouts.find(timeoutId);
  if(timeout == timeouts.end()) {
    // We can't find this timeout, thus we act as if it was already canceled.
    return;
  }

  // Cancel the timeout.
  timeout->second.cancel();
}

TimeoutId IoContext::setTimeoutImpl(
    TimeoutId::Generator& generator,
    bool repeat,
    jsg::Function<void()> function,
    double msDelay) {
  static constexpr int64_t max = 3153600000000;  // Milliseconds in 100 years
  // Clamp the range on timers to [0, 3153600000000] (inclusive). The specs
  // do not indicate a clear maximum range for setTimeout/setInterval so the
  // limit here is fairly arbitrary. 100 years max should be plenty safe.
  int64_t delay =
      msDelay <= 0 ? 0 :
      msDelay >= static_cast<double>(max) ? max : static_cast<int64_t>(msDelay);
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
  kj::Date adjustedTime = incomingRequest.ioChannelFactory->getTimer().now();

  KJ_IF_MAYBE (maybeNextTimeout, timeoutManager->getNextTimeout()) {
    // Don't return a time beyond when the next setTimeout() callback is intended to run. This
    // ensures that Date.now() inside the callback itself always returns exactly the time at which
    // the callback was scheduled (hiding non-determinism which could contain side channels), and
    // that the time returned by Date.now() never goes backwards.
    return kj::min(adjustedTime, *maybeNextTimeout);
  } else {
    return adjustedTime;
  }
}

kj::Date IoContext::now() {
  return now(getCurrentIncomingRequest());
}

kj::Own<WorkerInterface> IoContext::getSubrequestNoChecks(
    kj::FunctionParam<kj::Own<WorkerInterface>(SpanBuilder&, IoChannelFactory&)> func,
    SubrequestOptions options) {
  SpanBuilder span = nullptr;
  KJ_IF_MAYBE(n, options.operationName) {
    span = makeTraceSpan(kj::mv(*n));
  }

  auto ret = func(span, getIoChannelFactory());

  if (options.wrapMetrics) {
    auto& metrics = getMetrics();
    ret = metrics.wrapSubrequestClient(kj::mv(ret));
    ret = worker->getIsolate().wrapSubrequestClient(
        kj::mv(ret), getHeaderIds().contentEncoding, metrics);
  }

  if (span.isObserved()) {
    ret = ret.attach(kj::mv(span));
  }

  return kj::mv(ret);
}

kj::Own<WorkerInterface> IoContext::getSubrequest(
    kj::FunctionParam<kj::Own<WorkerInterface>(SpanBuilder&, IoChannelFactory&)> func,
    SubrequestOptions options) {
  limitEnforcer->newSubrequest(options.inHouse);
  return getSubrequestNoChecks(kj::mv(func), kj::mv(options));
}

kj::Own<WorkerInterface> IoContext::getSubrequestChannel(
    uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson, kj::ConstString operationName) {
  return getSubrequest([&](SpanBuilder& span, IoChannelFactory& channelFactory) {
    return getSubrequestChannelImpl(
        channel, isInHouse, kj::mv(cfBlobJson), span, channelFactory);
  }, SubrequestOptions {
    .inHouse = isInHouse,
    .wrapMetrics = !isInHouse,
    .operationName = kj::mv(operationName),
  });
}

kj::Own<WorkerInterface> IoContext::getSubrequestChannelNoChecks(
    uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson,
    kj::Maybe<kj::ConstString> operationName) {
  return getSubrequestNoChecks([&](SpanBuilder& span,
                                   IoChannelFactory& channelFactory) {
    return getSubrequestChannelImpl(
        channel, isInHouse, kj::mv(cfBlobJson), span, channelFactory);
  }, SubrequestOptions {
    .inHouse = isInHouse,
    .wrapMetrics = !isInHouse,
    .operationName = kj::mv(operationName),
  });
}

kj::Own<WorkerInterface> IoContext::getSubrequestChannelImpl(
    uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson,
    SpanBuilder& span, IoChannelFactory& channelFactory) {
  IoChannelFactory::SubrequestMetadata metadata {
    .cfBlobJson = kj::mv(cfBlobJson),
    .parentSpan = span,
    .featureFlagsForFl = worker->getIsolate().getFeatureFlagsForFl(),
  };

  auto client = channelFactory.startSubrequest(channel, kj::mv(metadata));

  return client;
}

kj::Own<kj::HttpClient> IoContext::getHttpClient(
    uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson, kj::ConstString operationName) {
  return asHttpClient(getSubrequestChannel(
      channel, isInHouse, kj::mv(cfBlobJson), kj::mv(operationName)));
}

kj::Own<kj::HttpClient> IoContext::getHttpClientNoChecks(
    uint channel, bool isInHouse, kj::Maybe<kj::String> cfBlobJson,
    kj::Maybe<kj::ConstString> operationName) {
  return asHttpClient(getSubrequestChannelNoChecks(channel, isInHouse, kj::mv(cfBlobJson),
                                                   kj::mv(operationName)));
}

kj::Own<CacheClient> IoContext::getCacheClient() {
  // TODO(someday): Should Cache API requests be considered in-house? They are already not counted
  //   as subrequests in metrics and logs (like in-house requests aren't), but historically the
  //   subrequest limit still applied. Since I can't currently think of a use case for more than 50
  //   cache API requests per request, I'm leaving it as-is for now.
  limitEnforcer->newSubrequest(false);
  return getIoChannelFactory().getCache();
}

jsg::AsyncContextFrame::StorageScope IoContext::makeAsyncTraceScope(
    Worker::Lock& lock, kj::Maybe<SpanParent> spanParentOverride) {
  jsg::Lock& js = lock;
  kj::Own<SpanParent> spanParent;
  KJ_IF_MAYBE (spo, kj::mv(spanParentOverride)) {
    spanParent = kj::heap(kj::mv(*spo));
  } else {
    spanParent = kj::heap(getMetrics().getSpan());
  }
  auto ioOwnSpanParent = IoContext::current().addObject(kj::mv(spanParent));
  auto spanHandle = jsg::wrapOpaque(js.v8Context(), kj::mv(ioOwnSpanParent));
  return jsg::AsyncContextFrame::StorageScope(
      js, lock.getTraceAsyncContextKey(), js.v8Ref(spanHandle));
}

SpanParent IoContext::getCurrentTraceSpan() {
  // If called while lock is held, try to use the trace info stored in the async context.
  KJ_IF_MAYBE (lock, currentLock) {
    KJ_IF_MAYBE (frame, jsg::AsyncContextFrame::current(*lock)) {
      KJ_IF_MAYBE (value, frame->get(lock->getTraceAsyncContextKey())) {
        auto handle = value->getHandle(*lock);
        jsg::Lock& js = *lock;
        auto& spanParent = jsg::unwrapOpaqueRef<IoOwn<SpanParent>>(js.v8Isolate, handle);
        return spanParent->addRef();
      }
    }
  }

  // If async context is unavailable (unset, or JS lock is not held), fall back to heuristic of
  // using the trace info from the most recent active request.
  return getMetrics().getSpan();
}

SpanBuilder IoContext::makeTraceSpan(kj::ConstString operationName) {
  return getCurrentTraceSpan().newChild(kj::mv(operationName));
}

void IoContext::taskFailed(kj::Exception&& exception) {
  if (waitUntilStatusValue == EventOutcome::OK) {
    KJ_IF_MAYBE(status, limitEnforcer->getLimitsExceeded()) {
      waitUntilStatusValue = *status;
    } else {
      waitUntilStatusValue = EventOutcome::EXCEPTION;
    }
  }

  // If `taskFailed()` throws the whole event loop blows up... let's be careful not to let that
  // happen.
  KJ_IF_MAYBE(e, kj::runCatchingExceptions([&]() {
    logUncaughtExceptionAsync(UncaughtExceptionSource::ASYNC_TASK, kj::mv(exception));
  })) {
    KJ_LOG(ERROR, "logUncaughtExceptionAsync() threw an exception?", *e);
  }
}

void IoContext::requireCurrent() {
  KJ_REQUIRE(threadLocalRequest == this, "request is not current in this thread");
}

void IoContext::checkFarGet(const DeleteQueue* expectedQueue) {
  KJ_ASSERT(expectedQueue);
  requireCurrent();

  if (expectedQueue == deleteQueue.get()) {
    // same request or same actor, success
  } else if (actor != nullptr) {
    JSG_FAIL_REQUIRE(
        Error, "Cannot perform I/O on behalf of a different Durable Object. I/O objects "
        "(such as streams, request/response bodies, and others) created in the context of one "
        "Durable Object cannot be accessed from a different Durable Object in the same isolate. "
        "This is a limitation of Cloudflare Workers which allows us to improve overall "
        "performance.");
  } else {
    JSG_FAIL_REQUIRE(
        Error, "Cannot perform I/O on behalf of a different request. I/O objects (such as "
        "streams, request/response bodies, and others) created in the context of one request "
        "handler cannot be accessed from a different request's handler. This is a limitation "
        "of Cloudflare Workers which allows us to improve overall performance.");
  }
}

IoContext::OwnedObjectList::~OwnedObjectList() noexcept(false) {
  while (head != nullptr) {
    // We want to have the same order of operations as the recursive destructor here. Without this
    // optimization, `~SpecificOwnedObject<T>` is invoked first, then `~OwnedObject()` which
    // destructs the next node which continues the process. The key takeway is that we destroy each
    // node's `T` before we move onto the next node. This duplicates that behavior by unlinking
    // forward through the list, which hopefully should keep our stack size low no matter how many
    // `OwnedObject` instances we have.
    unlink(*KJ_ASSERT_NONNULL(head));
  }
}

void IoContext::OwnedObjectList::unlink(OwnedObject& object) {
  KJ_IF_MAYBE(next, object.next) {
    next->get()->prev = object.prev;
  }
  *object.prev = kj::mv(object.next);
}

void IoContext::OwnedObjectList::link(kj::Own<OwnedObject> object) {
  KJ_IF_MAYBE(f, object->finalizer) {
    if (finalizersRan) {
      KJ_LOG(ERROR, "somehow new objects are being added after finalizers already ran",
             kj::getStackTrace());
      f->finalize();
    }
  }

  object->next = kj::mv(head);
  KJ_IF_MAYBE(next, object->next) {
    next->get()->prev = &object->next;
  }
  object->prev = &head;
  head = kj::mv(object);
}

kj::Vector<kj::StringPtr> IoContext::OwnedObjectList::finalize() {
  KJ_ASSERT(!finalizersRan);
  finalizersRan = true;

  kj::Vector<kj::StringPtr> warnings;
  auto* link = &head;
  while (*link != nullptr) {
    auto& l = KJ_ASSERT_NONNULL(*link);
    KJ_IF_MAYBE(f, l->finalizer) {
      KJ_IF_MAYBE(w, f->finalize()) {
        warnings.add(*w);
      }
#ifdef KJ_DEBUG
      f->finalized = true;
#endif
    }
    link = &l->next;
  }

  return warnings;
}

Worker::Actor& IoContext::getActorOrThrow() {
  return KJ_ASSERT_NONNULL(actor, "not an actor request");
}

class IoContext::ThreadScope {
  // Encapsulates making the IoContext current for the thread within some scope.

public:
  ThreadScope(IoContext& context)
      : previousRequest(threadLocalRequest) {
    KJ_REQUIRE(context.threadId == getThreadId(), "IoContext cannot switch threads");
    threadLocalRequest = &context;
  }

  ~ThreadScope() {
    threadLocalRequest = previousRequest;
  }

private:
  IoContext* previousRequest;
  // The previously-current context, before we entered this scope. We have to allow opening
  // multiple nested scopes especially to support destructors: destroying objects related to a
  // subrequest in one worker could transitively destroy resources belonging to the next worker in
  // the pipeline. We can't delay destruction to a future turn of the event loop because it's
  // common for child objects to contain pointers back to stuff owned by the parent that could
  // then be dangling.
};

class IoContext::Scope {
  // Encapsulates all the different scopes we need to set up to enter the I/O context.

public:
  Scope(IoContext& context, Worker::LockType lockType, kj::Maybe<InputGate::Lock> inputLock)
      : context(context),
        threadScope(context),
        workerLock(*context.worker, lockType),
        handleScope(workerLock.getIsolate()),
        jsContextScope(workerLock.getContext()),
        promiseContextScope(workerLock.getIsolate(), context.getPromiseContextTag(workerLock)) {
    KJ_REQUIRE(context.currentInputLock == nullptr);
    KJ_REQUIRE(context.currentLock == nullptr);
    context.currentInputLock = kj::mv(inputLock);
    context.currentLock = workerLock;

    KJ_ON_SCOPE_FAILURE(context.currentLock = nullptr; context.currentInputLock = nullptr);

    {
      // Handle any pending deletions that arrived while the worker was processing a different
      // request.
      auto lock = context.deleteQueue->crossThreadDeleteQueue.lockExclusive();
      auto& state = KJ_ASSERT_NONNULL(*lock);
      for (auto& object: state.queue) {
        OwnedObjectList::unlink(*object);
      }
      state.queue.clear();
    }
  }
  ~Scope() {
    context.currentLock = nullptr;
    context.currentInputLock = nullptr;
  }

  Worker::Lock& getWorkerLock() { return workerLock; }

private:

  IoContext& context;
  ThreadScope threadScope;
  Worker::Lock workerLock;
  v8::HandleScope handleScope;
  v8::Context::Scope jsContextScope;
  v8::Isolate::PromiseContextScope promiseContextScope;
};

void IoContext::runImpl(Runnable& runnable, bool takePendingEvent,
                             Worker::LockType lockType, kj::Maybe<InputGate::Lock> inputLock,
                             bool allowPermanentException) {
  KJ_IF_MAYBE(l, inputLock) {
    KJ_REQUIRE(l->isFor(KJ_ASSERT_NONNULL(actor).getInputGate()));
  }

  getIoChannelFactory().getTimer().syncTime();

  Scope scope(*this, lockType, kj::mv(inputLock));
  auto& workerLock = scope.getWorkerLock();

  if (!allowPermanentException) {
    workerLock.requireNoPermanentException();
  }

  kj::Own<void> event;
  if (takePendingEvent) {
    // Prevent finalizers from running while we're still executing JavaScript.
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
      // they will run with no limit. But if we call TerminateExecution() again now, it will
      // conveniently cause RunMicrotasks() to terminate _right after_ dequeuing the contents of
      // the task queue, which is perfect, because it effectively cancels them all.
      js.terminateExecution();
    }

    js.runMicrotasks();
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

      // That should have thrown, so we shouldn't get here.
      KJ_FAIL_ASSERT("script terminated for unknown reasons");
    } else {
      if (tryCatch.Message().IsEmpty()) {
        // Should never happen, but check for it because otherwise V8 will crash.
        KJ_LOG(ERROR, "tryCatch.Message() was empty even when not HasTerminated()??");
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
        workerLock.logUncaughtException(UncaughtExceptionSource::INTERNAL, jsException,
                                        tryCatch.Message());

        jsg::throwTunneledException(workerLock.getIsolate(), jsException);
      }
    }
  }
}

static constexpr auto kAsyncIoErrorMessage =
    "Some functionality, such as asynchronous I/O, timeouts, and "
    "generating random values, can only be performed while handling a "
    "request.";

IoContext& IoContext::current() {
  if (threadLocalRequest == nullptr) {
    v8::Isolate* isolate = v8::Isolate::TryGetCurrent();
    if (isolate == nullptr) {
      KJ_FAIL_REQUIRE("there is no current request on this thread");
    } else {
      isolate->ThrowError(jsg::v8StrIntern(isolate, kAsyncIoErrorMessage));
      throw jsg::JsExceptionThrown();
    }
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
  if (hasCurrent()) {
    return IoContext::current().getWeakRef();
  } else {
    return nullptr;
  }
}

void IoContext::runFinalizers(Worker::AsyncLock& asyncLock) {
  KJ_ASSERT(actor == nullptr);  // we don't finalize actor requests

  tasks = nullptr;
  // Tasks typically have callbacks that dereference IoOwns. Since those callbacks
  // will throw after request finalization, we should cancel them now.

  if (abortFulfiller->isWaiting()) {
    // Don't bother fullfilling `abortFulfiller` if limits were exceeded because in that case the
    // abort promise will be fulfilled shortly anyway.
    if (limitEnforcer->getLimitsExceeded() == nullptr) {
      abortFulfiller->reject(JSG_KJ_EXCEPTION(FAILED, Error,
          "The script will never generate a response."));
    }
  }

  if (auto warnings = ownedObjects.finalize(); !warnings.empty()) {
    // Log all the warnings.
    //
    // Logging a warning calls console.log() which could be maliciously overridden, so we must use
    // run() here (as opposed to just constructing a Scope). But we don't want it to try to create
    // a new PendingEvent, so we have to call runImpl() directly to pass false for the second
    // parameter.
    struct RunnableImpl: public Runnable {
      IoContext& context;
      kj::Vector<kj::StringPtr> warnings;

      RunnableImpl(IoContext& context, kj::Vector<kj::StringPtr> warnings)
          : context(context), warnings(kj::mv(warnings)) {}
      void run(Worker::Lock& lock) override {
        for (auto warning: warnings) {
          context.logWarning(warning);
        }
      }
    };

    RunnableImpl runnable(*this, kj::mv(warnings));
    runImpl(runnable, false, asyncLock, nullptr, true);
  }

  promiseContextTag = nullptr;
}

#ifdef KJ_DEBUG

IoContext::Finalizeable::Finalizeable(): context(IoContext::current()) {
  if (context.actor != nullptr) {
    // Disable the destructor check because finalization doesn't apply to actors.
    finalized = true;
  }
}
IoContext::Finalizeable::~Finalizeable() noexcept(false) {
  KJ_ASSERT(finalized || !context.isFinalized(),
      "Finalizeable object survived request finalization without being finalized. This usually "
      "means it was not passed to IoContext::addObject<T>() as a Finalizeable T.");
}

#endif

namespace {

class CacheQuotaInputStream final: public kj::AsyncInputStream {
public:
  CacheQuotaInputStream(
      kj::Own<kj::AsyncInputStream> inner, size_t quota,
      kj::Own<kj::PromiseFulfiller<size_t>> fulfiller)
      : inner(kj::mv(inner)), quota(quota), fulfiller(kj::mv(fulfiller)) {}

  ~CacheQuotaInputStream() noexcept(false) {
    fulfiller->fulfill(kj::cp(quota));
  }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return inner->tryRead(buffer, minBytes, maxBytes).then([this](size_t amount) {
      quota -= amount > quota ? quota : amount;
      return amount;
    });
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return inner->tryGetLength();
  }

  kj::Promise<uint64_t> pumpTo(
      kj::AsyncOutputStream& output, uint64_t amount) override {
    return inner->pumpTo(output, amount).then([this](uint64_t amount) {
      quota -= amount > quota ? quota : amount;
      return amount;
    });
  }

private:
  kj::Own<kj::AsyncInputStream> inner;
  size_t quota;
  kj::Own<kj::PromiseFulfiller<size_t>> fulfiller;
};

}  // namespace

jsg::Promise<kj::Maybe<IoOwn<kj::AsyncInputStream>>> IoContext::makeCachePutStream(
    jsg::Lock& js, kj::Own<kj::AsyncInputStream> stream) {
  KJ_IF_MAYBE(length, stream->tryGetLength()) {
    if (*length > MAX_INDIVIDUAL_PUT_SIZE) {
      // This Cache API subrequest would exceed the individual PUT quota.
      return js.resolvedPromise(kj::Maybe<IoOwn<kj::AsyncInputStream>>(nullptr));
    }
  }

  auto paf = kj::newPromiseAndFulfiller<size_t>();

  KJ_DEFER(cachePutQuota = kj::mv(paf.promise));

  return awaitIo(js, cachePutQuota.then(
      [fulfiller = kj::mv(paf.fulfiller), stream = kj::mv(stream)](size_t quota) mutable
          -> kj::Maybe<kj::Own<kj::AsyncInputStream>> {
    if (quota == 0) {
      fulfiller->fulfill(kj::cp(quota));
      // Total PUT quota already exceeded.
      return nullptr;
    }

    KJ_IF_MAYBE(length, stream->tryGetLength()) {
      // PUT with Content-Length. We rely on kj-http to enforce that the expected length is
      // respected, and can just return the new quota immediately, allowing the next PUT to start.
      KJ_DASSERT(*length <= MAX_INDIVIDUAL_PUT_SIZE);
      KJ_DEFER(fulfiller->fulfill(kj::cp(quota)));
      if (quota >= *length) {
        quota -= *length;
        return kj::mv(stream);
      } else {
        // This Cache API subrequest would exceed the total PUT quota.
        return nullptr;
      }
    } else {
      // PUT with Transfer-Encoding: chunked. We have no idea how big this request body is going to
      // be, so wrap the stream in a byte counter which deducts the total amount from the quota, and
      // unblocks the next PUT, only after this one is complete.
      //
      // Note that a single chunked PUT could still blow our quota by a long shot: a script could
      // make a number of PUTs totalling 499MB, then send a chunked PUT with an infinite body. It's
      // up to the cache to stop reading from the stream in this case, but at the very least we
      // will refuse to allow the initiation of any further PUT requests.
      return kj::heap<CacheQuotaInputStream>(kj::mv(stream), quota, kj::mv(fulfiller));
    }
  }), [this](jsg::Lock&, kj::Maybe<kj::Own<kj::AsyncInputStream>> result) {
    return kj::mv(result).map(
        [&](kj::Own<kj::AsyncInputStream>&& obj) { return addObject(kj::mv(obj)); });
  });
}

void IoContext::writeLogfwdr(uint channel,
    kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage) {
  addWaitUntil(getIoChannelFactory().writeLogfwdr(channel, kj::mv(buildMessage))
      .attach(registerPendingEvent()));
}

void IoContext::requireCurrentOrThrowJs() {
  JSG_REQUIRE(isCurrent(),
      Error,
      "Cannot perform I/O on behalf of a different request. I/O objects (such as "
      "streams, request/response bodies, and others) created in the context of one request "
      "handler cannot be accessed from a different request's handler. This is a limitation "
      "of Cloudflare Workers which allows us to improve overall performance.");
}

v8::Local<v8::Object> IoContext::getPromiseContextTag(jsg::Lock& js) {
  if (promiseContextTag == nullptr) {
    promiseContextTag = js.v8Ref(v8::Object::New(js.v8Isolate));
  }
  return KJ_REQUIRE_NONNULL(promiseContextTag).getHandle(js);
}

}  // namespace workerd
