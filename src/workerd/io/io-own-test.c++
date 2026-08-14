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

  auto moved = kj::mv(object);
  KJ_EXPECT(object.tryGet() == kj::none);
  KJ_EXPECT(moved.tryGet() != kj::none);

  object = kj::mv(moved);
  KJ_EXPECT(moved.tryGet() == kj::none);
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

    owned = kj::mv(object);
    KJ_EXPECT(object.tryGet() == kj::none);
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

// Schedules `action` on `queue`, which requires a jsg::Lock but not an IoContext.
void scheduleAction(
    TestFixture& fixture, const DeleteQueue& queue, kj::Function<void(jsg::Lock&)>&& action) {
  fixture.enterWorkerLockSynchronously(
      [&](Worker::Lock& lock) { queue.scheduleAction(lock, kj::mv(action)); });
}

// Runs everything currently queued on `queue`.
uint runActions(TestFixture& fixture, const DeleteQueue& queue) {
  auto actions = queue.takeActions();
  fixture.enterWorkerLockSynchronously([&](Worker::Lock& lock) {
    for (auto& action: actions) {
      action(lock);
    }
  });
  return actions.size();
}

KJ_TEST("DeleteQueue actions are taken out of the queue before they run") {
  TestFixture fixture;
  auto queue = kj::arc<DeleteQueue>();
  auto signal = queue->resetCrossThreadSignal();

  // Settling a promise can run application JavaScript, which is free to settle another promise
  // belonging to this same queue. That would deadlock if the queue's lock were still held while
  // running actions.
  uint ran = 0;
  scheduleAction(fixture, *queue, [&queue, &ran](jsg::Lock& js) {
    ++ran;
    queue->scheduleAction(js, [&ran](jsg::Lock&) { ++ran; });
  });

  KJ_EXPECT(runActions(fixture, *queue) == 1);
  KJ_EXPECT(ran == 1);

  KJ_EXPECT(runActions(fixture, *queue) == 1);
  KJ_EXPECT(ran == 2);

  KJ_EXPECT(runActions(fixture, *queue) == 0);
}

KJ_TEST("DeleteQueue signals its owner for actions scheduled while it is draining") {
  TestFixture fixture;
  auto queue = kj::arc<DeleteQueue>();

  auto signal = queue->resetCrossThreadSignal();
  KJ_EXPECT(!signal.poll(fixture.getWaitScope()));

  scheduleAction(fixture, *queue, [](jsg::Lock&) {});
  KJ_EXPECT(signal.poll(fixture.getWaitScope()));

  // Re-arming installs a new signal, so an action scheduled after the previous one was consumed
  // fulfills the new signal rather than being left queued with none outstanding.
  signal = queue->resetCrossThreadSignal();
  KJ_EXPECT(!signal.poll(fixture.getWaitScope()));

  scheduleAction(fixture, *queue, [](jsg::Lock&) {});
  KJ_EXPECT(signal.poll(fixture.getWaitScope()));
  KJ_EXPECT(runActions(fixture, *queue) == 2);
}

// Returns the handle another IoContext uses to push work onto `context`'s delete queue. This is the
// same object the promise cross-context resolve callback unwraps from a promise's context tag.
IoCrossContextExecutor& getCrossContextExecutor(jsg::Lock& js, IoContext& context) {
  return *jsg::unwrapOpaqueRef<kj::Own<IoCrossContextExecutor>>(
      js.v8Isolate, context.getPromiseContextTag(js));
}

KJ_TEST("cross-context actions scheduled during a drain still wake the IoContext") {
  TestFixture fixture({.actorId = Worker::Actor::Id(kj::str("drain-rearm-test"))});

  auto context = fixture.newIoContext();
  // Draining runs application JavaScript, so it needs a request to run under.
  auto request = fixture.newIncomingRequest(*context);

  uint ran = 0;
  fixture.enterContext(*request, [&](TestFixture::Environment& env) {
    // runImpl() already drained the queue on the way in, so this action stays queued until the
    // signal task picks it up.
    getCrossContextExecutor(env.js, *context).execute(env.js, [&context, &ran](jsg::Lock& js) {
      ++ran;
      // Schedule a second action from within the first, modelling another IoContext scheduling work
      // while this drain is already in progress. Consuming the signal without installing a fresh
      // one first would leave this action queued with nothing outstanding to wake the context.
      getCrossContextExecutor(js, *context).execute(js, [&ran](jsg::Lock&) { ++ran; });
    });
  });

  for (uint i = 0; i < 100 && ran < 2; ++i) {
    fixture.pollEventLoop();
  }
  KJ_EXPECT(ran == 2, ran);
}

}  // namespace
}  // namespace workerd
