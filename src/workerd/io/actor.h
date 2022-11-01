#include "worker.h"
#include "actor-cache.h"
#include "worker-interface.h"
#include "io-gate.h"
#include <workerd/api/global-scope.h>

namespace workerd {

// TODO(now): Rename to Worker::Actor
struct Worker::ActorImpl final: public kj::TaskSet::ErrorHandler {
  // Represents actor state within a Worker instance. This object tracks the JavaScript heap
  // objects backing `event.actorState`. Multiple `Actor`s can be created within a single `Worker`.
  kj::Own<const Worker> worker;
  Actor::Id actorId;
  using MakeStorageFunc = kj::Function<jsg::Ref<api::DurableObjectStorage>(
      jsg::Lock& js, const ApiIsolate& apiIsolate, ActorCache& actorCache)>;
  MakeStorageFunc makeStorage;

  kj::Own<ActorObserver> metrics;

  kj::Maybe<jsg::Value> transient;
  kj::Maybe<ActorCache> actorCache;

  struct NoClass {};
  struct Initializing {};

  kj::OneOf<
    NoClass,                         // not class-based
    DurableObjectConstructor*,       // constructor not run yet
    Initializing,                    // constructor currently running
    api::ExportedHandler,            // fully constructed
    kj::Exception                    // constructor threw
  > classInstance;
  // If the actor is backed by a class, this field tracks the instance through its stages. The
  // instance is constructed as part of the first request to be delivered.

  static kj::OneOf<NoClass, DurableObjectConstructor*>
      buildClassInstance(Worker& worker, kj::Maybe<kj::StringPtr> className);
    // Used by the Worker::ActorImpl constructor to initialize the classInstance.

  class HooksImpl: public InputGate::Hooks, public OutputGate::Hooks {
  public:
    HooksImpl(TimerChannel& timerChannel, ActorObserver& metrics)
        : timerChannel(timerChannel), metrics(metrics) {}

    void inputGateLocked() override { metrics.inputGateLocked(); }
    void inputGateReleased() override { metrics.inputGateReleased(); }
    void inputGateWaiterAdded() override { metrics.inputGateWaiterAdded(); }
    void inputGateWaiterRemoved() override { metrics.inputGateWaiterRemoved(); }
    // Implements InputGate::Hooks.

    kj::Promise<void> makeTimeoutPromise() override {
      return timerChannel.afterLimitTimeout(10 * kj::SECONDS)
          .then([]() -> kj::Promise<void> {
        return KJ_EXCEPTION(FAILED,
            "broken.outputGateBroken; jsg.Error: Durable Object storage operation exceeded "
            "timeout which caused object to be reset.");
      });
    }

    void outputGateLocked() override { metrics.outputGateLocked(); }
    void outputGateReleased() override { metrics.outputGateReleased(); }
    void outputGateWaiterAdded() override { metrics.outputGateWaiterAdded(); }
    void outputGateWaiterRemoved() override { metrics.outputGateWaiterRemoved(); }
    // Implements OutputGate::Hooks.

  private:
    TimerChannel& timerChannel;    // only for afterLimitTimeout()
    ActorObserver& metrics;
  };

  HooksImpl hooks;

  InputGate inputGate;
  // Handles both input locks and request locks.

  OutputGate outputGate;
  // Handles output locks.

  kj::Maybe<kj::Own<IoContext>> ioContext;
  // `ioContext` is initialized upon delivery of the first request.
  // TODO(cleanup): Rename IoContext to IoContext.

  kj::Maybe<kj::Own<kj::PromiseFulfiller<kj::Promise<void>>>> abortFulfiller;
  // If onBroken() is called while `ioContext` is still null, this is initialized. When
  // `ioContext` is constructed, this will be fulfilled with `ioContext.onAbort()`.

  kj::Maybe<kj::Promise<void>> metricsFlushLoopTask;
  // Task which periodically flushes metrics. Initialized after `ioContext` is initialized.

  TimerChannel& timerChannel;

  kj::ForkedPromise<void> shutdownPromise;
  kj::Own<kj::PromiseFulfiller<void>> shutdownFulfiller;

  kj::PromiseFulfillerPair<void> constructorFailedPaf = kj::newPromiseAndFulfiller<void>();

  struct Alarm {
    kj::Promise<void> alarmTask;
    kj::ForkedPromise<WorkerInterface::AlarmResult> alarm;
    kj::Own<kj::PromiseFulfiller<WorkerInterface::AlarmResult>> fulfiller;
    kj::Date scheduledTime;
  };

  struct RunningAlarm : public Alarm {
    kj::Maybe<Alarm> queuedAlarm;
  };

  kj::TaskSet deletedAlarmTasks;
  kj::Maybe<RunningAlarm> runningAlarm;
  // Used to handle deduplication of alarm requests

  bool hasCalledOnBroken = false;

  ActorImpl(Worker::Lock& lock, Actor::Id actorId,
       bool hasTransient, kj::Maybe<rpc::ActorStorage::Stage::Client> persistent,
       kj::Maybe<kj::StringPtr> className,
       MakeStorageFunc makeStorage, TimerChannel& timerChannel,
       kj::Own<ActorObserver> metricsParam,
       kj::PromiseFulfillerPair<void> paf = kj::newPromiseAndFulfiller<void>());

  void taskFailed(kj::Exception&& e) override {
    LOG_EXCEPTION("deletedAlarmTaskFailed", e);
  }

  kj::Promise<void> onBroken() {
    // Get a promise that rejects when this actor becomes broken in some way. See doc comments for
    // WorkerRuntime.makeActor() in worker.capnp for a discussion of actor brokenness.
    // Note that this doesn't cover every cause of actor brokenness -- some of them are fulfilled
    // in worker-set or process-sandbox code, in particular code updates and exceeded memory.
    // This method can only be called once.
    // TODO(soon): Detect and report other cases of brokenness, as described in worker.capnp.

    KJ_REQUIRE(!hasCalledOnBroken, "onBroken() can only be called once!");
    kj::Promise<void> abortPromise = nullptr;

    KJ_IF_MAYBE(rc, ioContext) {
      abortPromise = rc->get()->onAbort();
    } else {
      auto paf = kj::newPromiseAndFulfiller<kj::Promise<void>>();
      abortPromise = kj::mv(paf.promise);
      abortFulfiller = kj::mv(paf.fulfiller);
    }

    return abortPromise
      // inputGate.onBroken() is covered by IoContext::onAbort(), but outputGate.onBroken() is
      // not.
      .exclusiveJoin(outputGate.onBroken())
      .exclusiveJoin(kj::mv(constructorFailedPaf.promise));
  }
};

}; // namespace workerd
