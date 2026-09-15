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
};

struct FailedTrackingState final: public kj::Refcounted {
  uint attempts = 0;
  uint rollbacks = 0;
};

class TestActorObserver final: public ActorObserver {
 public:
  explicit TestActorObserver(kj::Own<TaskState> state): state(kj::mv(state)) {}

  kj::Own<WaitUntilTaskHandle> addedWaitUntilTask() noexcept override {
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
  explicit FailedTrackingActorObserver(kj::Own<FailedTrackingState> state): state(kj::mv(state)) {}

  kj::Own<WaitUntilTaskHandle> addedWaitUntilTask() noexcept override {
    ++state->attempts;
    try {
      kj::throwRecoverableException(KJ_EXCEPTION(FAILED, "synthetic tracking failure"));
    } catch (kj::Exception&) {
      ++state->rollbacks;
      return kj::Own<WaitUntilTaskHandle>();
    }
    KJ_UNREACHABLE;
  }

 private:
  kj::Own<FailedTrackingState> state;
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

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("tracking failure does not affect actor wait-until task") {
  auto state = kj::refcounted<FailedTrackingState>();
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
  context->addWaitUntil(kj::evalLater([&taskRan]() { taskRan = true; }));
  fixture.pollEventLoop();

  KJ_EXPECT(state->attempts == 1);
  KJ_EXPECT(state->rollbacks == 1);
  KJ_EXPECT(taskRan);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("actor context teardown releases pending task handle") {
  auto state = kj::refcounted<TaskState>();
  TestFixture fixture(TestFixture::SetupParams{
    .actorId = Worker::Actor::Id(kj::str("teardown-task-test")),
    .useRealTimers = false,
    .actorObserverFactory = makeActorObserverFactory(*state),
  });

  {
    auto context = fixture.newIoContext();
    auto request = fixture.newIncomingRequest(*context);
    context->addWaitUntil(kj::Promise<void>(kj::NEVER_DONE));
    KJ_EXPECT(state->activeTasks == 1);

    fixture.getActor().shutdown(0);
    fixture.drainAndDestroy(kj::mv(request));
  }

  KJ_EXPECT(state->activeTasks == 0);
}

}  // namespace
}  // namespace workerd
