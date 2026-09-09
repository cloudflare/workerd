// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/io-context.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd {
namespace {

struct TaskState final: public kj::Refcounted {
  uint activeTasks = 0;
  uint attempts = 0;
};

class TestActorObserver final: public ActorObserver {
 public:
  explicit TestActorObserver(kj::Own<TaskState> state): state(kj::mv(state)) {}

  kj::Own<WaitUntilTaskHandle> addedWaitUntilTask() override {
    class Handle final: public WaitUntilTaskHandle {
     public:
      explicit Handle(kj::Own<TaskState> state): state(kj::mv(state)) {
        ++this->state->activeTasks;
      }
      ~Handle() noexcept override {
        --state->activeTasks;
      }

     private:
      kj::Own<TaskState> state;
    };

    return kj::heap<Handle>(kj::addRef(*state));
  }

 private:
  kj::Own<TaskState> state;
};

class FailedTrackingActorObserver final: public ActorObserver {
 public:
  explicit FailedTrackingActorObserver(kj::Own<TaskState> state): state(kj::mv(state)) {}

  kj::Own<WaitUntilTaskHandle> addedWaitUntilTask() override {
    ++state->attempts;
    kj::throwRecoverableException(KJ_EXCEPTION(FAILED, "synthetic tracking failure"));
    KJ_UNREACHABLE;
  }

 private:
  kj::Own<TaskState> state;
};

kj::Function<kj::Own<ActorObserver>()> makeActorObserverFactory(TaskState& state) {
  return [state = kj::addRef(state)]() mutable -> kj::Own<ActorObserver> {
    return kj::refcounted<TestActorObserver>(kj::addRef(*state));
  };
}

KJ_TEST("actor wait-until task holds observer handle") {
  auto state = kj::refcounted<TaskState>();
  TestFixture fixture(TestFixture::SetupParams{
    .actorId = Worker::Actor::Id(kj::str("task-test")),
    .useRealTimers = false,
    .actorObserverFactory = makeActorObserverFactory(*state),
  });
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);

  context->addWaitUntil(kj::evalLater([]() {}));
  KJ_EXPECT(state->activeTasks == 1);

  fixture.pollEventLoop();
  KJ_EXPECT(state->activeTasks == 0);

  context->addTask(kj::evalLater([]() {}));
  KJ_EXPECT(state->activeTasks == 1);

  fixture.pollEventLoop();
  KJ_EXPECT(state->activeTasks == 0);

  kj::Canceler firstCanceler;
  kj::Canceler secondCanceler;
  context->addWaitUntil(firstCanceler.wrap(kj::Promise<void>(kj::NEVER_DONE)));
  KJ_EXPECT(state->activeTasks == 1);
  context->addWaitUntil(secondCanceler.wrap(kj::Promise<void>(kj::NEVER_DONE)));
  KJ_EXPECT(state->activeTasks == 2);

  firstCanceler.cancel("first test cancellation");
  fixture.pollEventLoop();
  KJ_EXPECT(state->activeTasks == 1);

  secondCanceler.cancel("second test cancellation");
  fixture.pollEventLoop();
  KJ_EXPECT(state->activeTasks == 0);

  context->addWaitUntil(kj::evalLater(
      []() { kj::throwRecoverableException(KJ_EXCEPTION(FAILED, "expected task failure")); }));
  KJ_EXPECT(state->activeTasks == 1);

  KJ_EXPECT_LOG(ERROR, "expected task failure");
  fixture.pollEventLoop();
  KJ_EXPECT(state->activeTasks == 0);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("actor wait-until task runs with base observer") {
  TestFixture fixture(TestFixture::SetupParams{
    .actorId = Worker::Actor::Id(kj::str("no-op-task-test")),
    .useRealTimers = false,
  });
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);

  bool taskRan = false;
  context->addWaitUntil(kj::evalLater([&taskRan]() { taskRan = true; }));
  fixture.pollEventLoop();
  KJ_EXPECT(taskRan);

  bool taskCanceled = false;
  kj::Canceler canceler;
  context->addWaitUntil(
      canceler.wrap(kj::Promise<void>(kj::NEVER_DONE)).catch_([&taskCanceled](kj::Exception&&) {
    taskCanceled = true;
  }));
  canceler.cancel("expected task cancellation");
  fixture.pollEventLoop();
  KJ_EXPECT(taskCanceled);

  context->addWaitUntil(kj::evalLater(
      []() { kj::throwRecoverableException(KJ_EXCEPTION(FAILED, "expected base task failure")); }));
  KJ_EXPECT_LOG(ERROR, "expected base task failure");
  fixture.pollEventLoop();

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("tracking failure does not affect actor wait-until task") {
  auto state = kj::refcounted<TaskState>();
  auto actorObserverFactory = kj::Function<kj::Own<ActorObserver>()>(
      [state = kj::addRef(*state)]() mutable -> kj::Own<ActorObserver> {
    return kj::refcounted<FailedTrackingActorObserver>(kj::addRef(*state));
  });
  TestFixture fixture(TestFixture::SetupParams{
    .actorId = Worker::Actor::Id(kj::str("failed-tracking-test")),
    .useRealTimers = false,
    .actorObserverFactory = kj::mv(actorObserverFactory),
  });
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);

  bool taskRan = false;
  KJ_EXPECT_LOG(ERROR, "synthetic tracking failure");
  context->addWaitUntil(kj::evalLater([&taskRan]() { taskRan = true; }));
  fixture.pollEventLoop();

  KJ_EXPECT(state->attempts == 1);
  KJ_EXPECT(taskRan);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("actor teardown releases pending task handle") {
  auto state = kj::refcounted<TaskState>();
  TestFixture fixture(TestFixture::SetupParams{
    .actorId = Worker::Actor::Id(kj::str("teardown-task-test")),
    .useRealTimers = false,
    .actorObserverFactory = makeActorObserverFactory(*state),
  });

  {
    auto& actor = fixture.getActor();
    KJ_EXPECT(&KJ_ASSERT_NONNULL(actor.tryGetMetrics()) == &actor.getMetrics());
    auto context = fixture.newIoContext();
    auto request = fixture.newIncomingRequest(*context);
    actor.setIoContext(kj::addRef(*context));
    context->addWaitUntil(kj::Promise<void>(kj::NEVER_DONE));
    KJ_EXPECT(state->activeTasks == 1);

    actor.shutdown(0);
    KJ_EXPECT(&KJ_ASSERT_NONNULL(actor.tryGetMetrics()) == &actor.getMetrics());
    fixture.drainAndDestroy(kj::mv(request));
  }

  // The actor is the context's sole owner after the request and local reference are released.
  KJ_EXPECT(state->activeTasks == 1);
  fixture.resetActor();
  KJ_EXPECT(state->activeTasks == 0);
}

}  // namespace
}  // namespace workerd
