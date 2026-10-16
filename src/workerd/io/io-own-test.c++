#include <workerd/io/io-context.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd {
namespace {

class TrackedObject {
 public:
  TrackedObject(uint& destructionCount, uint value)
      : value(value),
        destructionCount(destructionCount) {}
  ~TrackedObject() noexcept(false) {
    ++destructionCount;
  }

  uint value;

 private:
  uint& destructionCount;
};

KJ_TEST("ReverseIoOwn supports dereferencing, moves, and early destruction") {
  TestFixture fixture;
  uint destructionCount = 0;
  auto context = fixture.newIoContext();
  auto object = context->addObjectReverse(kj::heap<TrackedObject>(destructionCount, 123));

  KJ_EXPECT(object->value == 123);
  KJ_EXPECT((*object).value == 123);

  // Checking that each move empties its source is the behavior under test.
  auto moved = kj::mv(object);
  KJ_EXPECT(object.tryGet() == kj::none);  // NOLINT(workerd-use-after-move)
  KJ_EXPECT(moved.tryGet() != kj::none);

  object = kj::mv(moved);
  KJ_EXPECT(moved.tryGet() == kj::none);  // NOLINT(workerd-use-after-move)
  KJ_EXPECT(object.tryGet() != kj::none);

  object = nullptr;
  KJ_EXPECT(destructionCount == 1);
  KJ_EXPECT(object.tryGet() == kj::none);
}

KJ_TEST("ReverseIoOwn can transfer a live object out of its IoContext") {
  TestFixture fixture;
  uint destructionCount = 0;
  kj::Own<TrackedObject> owned;

  {
    auto context = fixture.newIoContext();
    auto object = context->addObjectReverse(kj::heap<TrackedObject>(destructionCount, 123));

    // Checking that the transfer empties the source is the behavior under test.
    owned = kj::mv(object);
    KJ_EXPECT(object.tryGet() == kj::none);  // NOLINT(workerd-use-after-move)
    KJ_EXPECT(owned->value == 123);
  }

  KJ_EXPECT(destructionCount == 0);
  owned = nullptr;
  KJ_EXPECT(destructionCount == 1);
}

KJ_TEST("ReverseIoOwn remains safe after its IoContext is destroyed") {
  TestFixture fixture;
  uint destructionCount = 0;
  ReverseIoOwn<TrackedObject> object = nullptr;

  {
    auto context = fixture.newIoContext();
    object = context->addObjectReverse(kj::heap<TrackedObject>(destructionCount, 123));
  }

  KJ_EXPECT(destructionCount == 1);
  KJ_EXPECT(object.tryGet() == kj::none);
  KJ_EXPECT_THROW_MESSAGE("execution context has ended", object->value);

  {
    auto context = fixture.newIoContext();
    object = context->addObjectReverse(kj::heap<TrackedObject>(destructionCount, 456));
    KJ_EXPECT(object.tryGet() != kj::none);
    KJ_EXPECT(object->value == 456);
  }

  KJ_EXPECT(destructionCount == 2);
  KJ_EXPECT(object.tryGet() == kj::none);

  {
    auto destroyedAfterInvalidation = kj::mv(object);
    KJ_EXPECT(destroyedAfterInvalidation.tryGet() == kj::none);
  }
}

class ExpectNoTaskFailures final: public kj::TaskSet::ErrorHandler {
 public:
  void taskFailed(kj::Exception&& exception) override {
    KJ_FAIL_EXPECT(exception);
  }
};

KJ_TEST("cross-context actions scheduled while actions run still wake the IoContext") {
  TestFixture fixture({.actorId = Worker::Actor::Id(kj::str("rearm-test"))});
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  auto executor = context->getCrossContextExecutor();

  uint ran = 0;
  KJ_ASSERT(executor.tryExecute([&executor, &ran](jsg::Lock&) {
    ++ran;
    // Another IoContext can schedule work at any point while the actions run. Consuming the signal
    // without installing a fresh one first would leave this action queued with nothing outstanding
    // to wake the context.
    KJ_ASSERT(executor.tryExecute([&ran](jsg::Lock&) { ++ran; }));
  }));
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 2, ran);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("cross-context actions wait for an I/O continuation holding the input lock") {
  TestFixture fixture({.actorId = Worker::Actor::Id(kj::str("input-lock-test"))});
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);

  kj::Vector<kj::StringPtr> order;
  auto io = kj::newPromiseAndFulfiller<void>();
  fixture.enterContext(*request, [&](TestFixture::Environment& env) {
    env.context
        .awaitIoWithInputLock(env.js, kj::mv(io.promise), [&order](jsg::Lock&) {
      order.add("continuation"_kj);
    }).markAsHandled(env.js);
    KJ_ASSERT(env.context.getCrossContextExecutor().tryExecute(
        [&order](jsg::Lock&) { order.add("action"_kj); }));
  });
  fixture.pollEventLoop();
  KJ_EXPECT(order.empty(), kj::strArray(order, ", "));

  io.fulfiller->fulfill();
  fixture.pollEventLoop();
  KJ_EXPECT(order.size() == 2 && order[0] == "continuation" && order[1] == "action",
      kj::strArray(order, ", "));
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("cross-context actions run while an older request outlives a newer one") {
  TestFixture fixture({.actorId = Worker::Actor::Id(kj::str("older-request-test"))});
  auto context = fixture.newIoContext();
  auto older = fixture.newIncomingRequest(*context);
  auto newer = fixture.newIncomingRequest(*context);

  // The newer request finishes in the turn just before the signal task wakes, so the signal task
  // sees it draining with no waitUntil tasks left, and the older request takes over afterwards.
  ExpectNoTaskFailures errorHandler;
  kj::TaskSet tasks(errorHandler);
  auto finished = kj::newPromiseAndCrossThreadFulfiller<void>();
  tasks.add(finished.promise.then([&tasks, &newer]() { newer->drain(tasks, kj::mv(newer)); }));

  uint ran = 0;
  finished.fulfiller->fulfill();
  KJ_ASSERT(context->getCrossContextExecutor().tryExecute([&ran](jsg::Lock&) { ++ran; }));
  fixture.pollEventLoop();
  KJ_EXPECT(newer.get() == nullptr);
  KJ_EXPECT(ran == 1, ran);
  fixture.drainAndDestroy(kj::mv(older));
}

KJ_TEST(
    "cross-context actions stay queued if the request ends while they wait for the input lock") {
  TestFixture fixture({.actorId = Worker::Actor::Id(kj::str("input-lock-wait-test"))});
  auto& waitScope = fixture.getWaitScope();
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);

  auto inputLock = fixture.getActor().getInputGate().wait(nullptr).wait(waitScope);
  uint ran = 0;
  KJ_ASSERT(context->getCrossContextExecutor().tryExecute([&ran](jsg::Lock&) { ++ran; }));
  fixture.pollEventLoop();

  request = nullptr;
  { auto released = kj::mv(inputLock); }
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 0, ran);
  KJ_EXPECT(context->getAbortReason() == kj::none, context->getAbortReason());

  auto next = fixture.newIncomingRequest(*context);
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 1, ran);
  fixture.drainAndDestroy(kj::mv(next));
}

KJ_TEST("cross-context actions stay queued if the actor shuts down while they wait for a lock") {
  auto io = kj::setupAsyncIo();
  TestFixture fixture({.waitScope = io.waitScope,
    .actorId = Worker::Actor::Id(kj::str("isolate-lock-wait-test")),
    .useRealTimers = false});
  TestFixture other({.waitScope = io.waitScope, .useRealTimers = false});
  auto context = fixture.newIoContext();

  // The request drains until the actor shuts down.
  auto request = fixture.newIncomingRequest(*context);
  context->addWaitUntil(kj::Promise<void>(kj::NEVER_DONE));
  ExpectNoTaskFailures errorHandler;
  kj::TaskSet drains(errorHandler);
  request->drain(drains, kj::mv(request));

  // A thread waits for one isolate lock at a time, so holding another isolate's lock holds up the
  // actions' run.
  auto otherContext = other.newIoContext();
  auto asyncLock =
      otherContext->getWorker().takeAsyncLockWithoutRequest(nullptr).wait(io.waitScope);
  uint ran = 0;
  KJ_ASSERT(context->getCrossContextExecutor().tryExecute([&ran](jsg::Lock&) { ++ran; }));
  fixture.pollEventLoop();

  fixture.getActor().shutdown(0);
  fixture.pollEventLoop();
  KJ_EXPECT(!context->hasCurrentIncomingRequest());
  { auto released = kj::mv(asyncLock); }
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 0, ran);
  KJ_EXPECT(context->getAbortReason() == kj::none, context->getAbortReason());
}

KJ_TEST("cross-context actions wait for an idle actor's next request") {
  TestFixture fixture({.actorId = Worker::Actor::Id(kj::str("idle-actor-test"))});
  auto context = fixture.newIoContext();
  fixture.drainAndDestroy(fixture.newIncomingRequest(*context));

  uint ran = 0;
  KJ_ASSERT(context->getCrossContextExecutor().tryExecute([&ran](jsg::Lock&) { ++ran; }));
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 0, ran);
  KJ_EXPECT(context->getAbortReason() == kj::none, context->getAbortReason());

  // Delivering the next request is enough; it doesn't have to run anything itself.
  auto request = fixture.newIncomingRequest(*context);
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 1, ran);
  fixture.drainAndDestroy(kj::mv(request));
}

}  // namespace
}  // namespace workerd
