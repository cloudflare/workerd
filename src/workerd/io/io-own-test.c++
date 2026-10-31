#include <workerd/io/io-context.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

#include <capnp/message.h>
#include <kj/async-io.h>
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
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("rearm-test"))});
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  auto executor = context->getCrossContextExecutor();

  uint ran = 0;
  KJ_ASSERT(executor.tryExecute([&executor, &ran](jsg::Lock&) {
    ++ran;
    // Scheduling during the run must still wake the context.
    KJ_ASSERT(executor.tryExecute([&ran](jsg::Lock&) { ++ran; }));
  }));
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 2, ran);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("cross-context actions that fail abort the IoContext, whatever details they carry") {
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("failing-action-test"))});
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);

  // An action that fails with the detail that keeps actions queued must still abort the context.
  KJ_ASSERT(context->getCrossContextExecutor().tryExecute([](jsg::Lock&) {
    auto e = KJ_EXCEPTION(FAILED, "action failed");
    e.setDetail(NO_CURRENT_INCOMING_REQUEST_DETAIL_ID, kj::heapArray<kj::byte>(0));
    kj::throwException(kj::mv(e));
  }));
  fixture.pollEventLoop();
  KJ_IF_SOME(reason, context->getAbortReason()) {
    KJ_EXPECT(reason.getDescription() == "action failed", reason);
  } else {
    KJ_FAIL_ASSERT("the IoContext wasn't aborted");
  }
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a cross-context action that fails drops the actions taken with it") {
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("failing-batch-test"))});
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  auto executor = context->getCrossContextExecutor();

  // Queued together, so one run takes all three.
  kj::Vector<kj::StringPtr> ran;
  KJ_ASSERT(executor.tryExecute([&ran](jsg::Lock&) { ran.add("first"_kj); }));
  KJ_ASSERT(executor.tryExecute(
      [](jsg::Lock&) { kj::throwException(KJ_EXCEPTION(FAILED, "second failed")); }));
  KJ_ASSERT(executor.tryExecute([&ran](jsg::Lock&) { ran.add("third"_kj); }));
  fixture.pollEventLoop();

  KJ_EXPECT(ran.size() == 1 && ran[0] == "first", kj::strArray(ran, ", "));
  KJ_EXPECT(executor.getQueuedActionCountForTest() == 0, executor.getQueuedActionCountForTest());
  KJ_IF_SOME(reason, context->getAbortReason()) {
    KJ_EXPECT(reason.getDescription() == "second failed", reason);
  } else {
    KJ_FAIL_ASSERT("the IoContext wasn't aborted");
  }
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("cross-context actions wait for an I/O continuation holding the input lock") {
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("input-lock-test"))});
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
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("older-request-test"))});
  auto context = fixture.newIoContext();
  auto older = fixture.newIncomingRequest(*context);
  auto newer = fixture.newIncomingRequest(*context);

  // Queued while the newer request is current, which then ends before the signal task wakes,
  // leaving the older one current.
  uint ran = 0;
  KJ_ASSERT(context->getCrossContextExecutor().tryExecute([&ran](jsg::Lock&) { ++ran; }));
  newer = nullptr;
  KJ_ASSERT(context->hasCurrentIncomingRequest());
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 1, ran);
  fixture.drainAndDestroy(kj::mv(older));
}

KJ_TEST(
    "a request doesn't finish draining while actions queued during it wait for the input lock") {
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("drain-wait-test"))});
  auto& waitScope = fixture.getWaitScope();
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);

  // Queued after delivery, so the signal task runs these, but the input lock holds them up until
  // the request is draining.
  auto inputLock = fixture.getActor().getInputGate().wait(nullptr).wait(waitScope);
  uint ran = 0;
  KJ_ASSERT(context->getCrossContextExecutor().tryExecute([&ran](jsg::Lock&) { ++ran; }));
  fixture.pollEventLoop();
  ExpectNoTaskFailures errorHandler;
  kj::TaskSet drains(errorHandler);
  request->drain(drains, kj::mv(request));
  fixture.pollEventLoop();
  KJ_EXPECT(context->hasCurrentIncomingRequest());
  KJ_EXPECT(ran == 0, ran);

  { auto released = kj::mv(inputLock); }
  drains.onEmpty().wait(waitScope);
  KJ_EXPECT(ran == 1, ran);
  KJ_EXPECT(!context->hasCurrentIncomingRequest());
  KJ_EXPECT(context->getAbortReason() == kj::none, context->getAbortReason());
}

KJ_TEST(
    "cross-context actions stay queued if the request ends while they wait for the input lock") {
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("input-lock-wait-test"))});
  auto& waitScope = fixture.getWaitScope();
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);

  auto inputLock = fixture.getActor().getInputGate().wait(nullptr).wait(waitScope);
  uint ran = 0;
  KJ_ASSERT(context->getCrossContextExecutor().tryExecute([&ran](jsg::Lock&) { ++ran; }));
  fixture.pollEventLoop();

  // Destroyed without draining, so nothing holds the request open for the actions' run.
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
  // Each fixture initializes the process's autogates, so both enable it.
  TestFixture fixture({.waitScope = io.waitScope,
    .autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("isolate-lock-wait-test")),
    .useRealTimers = false});
  TestFixture other({.waitScope = io.waitScope,
    .autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .useRealTimers = false});
  auto context = fixture.newIoContext();

  // The request drains until the actor shuts down.
  auto request = fixture.newIncomingRequest(*context);
  context->addWaitUntil(kj::Promise<void>(kj::NEVER_DONE));
  ExpectNoTaskFailures errorHandler;
  kj::TaskSet drains(errorHandler);
  request->drain(drains, kj::mv(request));

  // Holding another isolate's lock holds up the run, since a thread takes one at a time.
  auto asyncLock = other.takeAsyncLock();
  uint ran = 0;
  auto executor = context->getCrossContextExecutor();
  KJ_ASSERT(executor.tryExecute([&ran](jsg::Lock&) { ++ran; }));
  fixture.pollEventLoop();

  fixture.getActor().shutdown(0);
  fixture.pollEventLoop();
  KJ_EXPECT(!context->hasCurrentIncomingRequest());
  { auto released = kj::mv(asyncLock); }
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 0, ran);
  KJ_EXPECT(executor.getQueuedActionCountForTest() == 1, executor.getQueuedActionCountForTest());
  KJ_EXPECT(context->getAbortReason() == kj::none, context->getAbortReason());
}

KJ_TEST("cross-context actions wait for an idle actor's next request") {
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("idle-actor-test"))});
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

KJ_TEST("an idle actor's queued actions run before its next request's own work") {
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("idle-actor-order-test"))});
  auto context = fixture.newIoContext();
  fixture.drainAndDestroy(fixture.newIncomingRequest(*context));

  kj::Vector<kj::StringPtr> order;
  KJ_ASSERT(context->getCrossContextExecutor().tryExecute(
      [&order](jsg::Lock&) { order.add("action"_kj); }));
  fixture.pollEventLoop();
  KJ_EXPECT(order.empty(), kj::strArray(order, ", "));

  // Delivery starts the actions' run, which takes the input lock before the request's own run().
  auto request = fixture.newIncomingRequest(*context);
  context->run([&order](Worker::Lock&) { order.add("handler"_kj); }).wait(fixture.getWaitScope());
  KJ_EXPECT(order.size() == 2 && order[0] == "action" && order[1] == "handler",
      kj::strArray(order, ", "));
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a cross-context action that fails when an idle actor's next request runs it aborts the "
        "IoContext") {
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("idle-failing-action-test"))});
  auto context = fixture.newIoContext();
  fixture.drainAndDestroy(fixture.newIncomingRequest(*context));

  KJ_ASSERT(context->getCrossContextExecutor().tryExecute(
      [](jsg::Lock&) { kj::throwException(KJ_EXCEPTION(FAILED, "action failed")); }));
  fixture.pollEventLoop();
  KJ_EXPECT(context->getAbortReason() == kj::none, context->getAbortReason());

  auto request = fixture.newIncomingRequest(*context);
  fixture.pollEventLoop();
  KJ_IF_SOME(reason, context->getAbortReason()) {
    KJ_EXPECT(reason.getDescription() == "action failed", reason);
  } else {
    KJ_FAIL_ASSERT("the IoContext wasn't aborted");
  }
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("an idle actor's next request doesn't finish draining before its queued actions run") {
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("short-request-test"))});
  auto& waitScope = fixture.getWaitScope();
  auto context = fixture.newIoContext();
  fixture.drainAndDestroy(fixture.newIncomingRequest(*context));

  uint ran = 0;
  KJ_ASSERT(context->getCrossContextExecutor().tryExecute([&ran](jsg::Lock&) { ++ran; }));
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 0, ran);

  // The next request drains at once, like an event that awaits nothing, while the input lock
  // holds up the actions.
  auto inputLock = fixture.getActor().getInputGate().wait(nullptr).wait(waitScope);
  auto request = fixture.newIncomingRequest(*context);
  ExpectNoTaskFailures errorHandler;
  kj::TaskSet drains(errorHandler);
  request->drain(drains, kj::mv(request));
  fixture.pollEventLoop();
  KJ_EXPECT(context->hasCurrentIncomingRequest());
  KJ_EXPECT(ran == 0, ran);

  { auto released = kj::mv(inputLock); }
  drains.onEmpty().wait(waitScope);
  KJ_EXPECT(ran == 1, ran);
  KJ_EXPECT(!context->hasCurrentIncomingRequest());
  KJ_EXPECT(context->getAbortReason() == kj::none, context->getAbortReason());
}

KJ_TEST("an idle actor's queued actions stay queued if it shuts down before its next request "
        "runs them") {
  auto io = kj::setupAsyncIo();
  // Each fixture initializes the process's autogates, so both enable it.
  TestFixture fixture({.waitScope = io.waitScope,
    .autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("next-request-shutdown-test")),
    .useRealTimers = false});
  TestFixture other({.waitScope = io.waitScope,
    .autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .useRealTimers = false});
  auto context = fixture.newIoContext();
  fixture.drainAndDestroy(fixture.newIncomingRequest(*context));

  uint ran = 0;
  auto executor = context->getCrossContextExecutor();
  KJ_ASSERT(executor.tryExecute([&ran](jsg::Lock&) { ++ran; }));
  fixture.pollEventLoop();

  // Holding another isolate's lock holds up the run that delivering the next request starts, and
  // the request drains until the actor shuts down.
  auto asyncLock = other.takeAsyncLock();
  auto request = fixture.newIncomingRequest(*context);
  ExpectNoTaskFailures errorHandler;
  kj::TaskSet drains(errorHandler);
  request->drain(drains, kj::mv(request));
  fixture.pollEventLoop();
  KJ_EXPECT(context->hasCurrentIncomingRequest());

  fixture.getActor().shutdown(0);
  fixture.pollEventLoop();
  KJ_EXPECT(!context->hasCurrentIncomingRequest());
  { auto released = kj::mv(asyncLock); }
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 0, ran);
  KJ_EXPECT(executor.getQueuedActionCountForTest() == 1, executor.getQueuedActionCountForTest());
  KJ_EXPECT(context->getAbortReason() == kj::none, context->getAbortReason());
}

KJ_TEST("without the autogate, cross-context actions abort an idle actor") {
  TestFixture fixture({.ignoreAllAutogatesEnv = true,
    .actorId = Worker::Actor::Id(kj::str("idle-actor-gate-off-test"))});
  auto context = fixture.newIoContext();
  fixture.drainAndDestroy(fixture.newIncomingRequest(*context));

  uint ran = 0;
  KJ_ASSERT(context->getCrossContextExecutor().tryExecute([&ran](jsg::Lock&) { ++ran; }));
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 0, ran);
  KJ_IF_SOME(reason, context->getAbortReason()) {
    KJ_EXPECT(reason.getDescription().contains("no current IncomingRequest"), reason);
  } else {
    KJ_FAIL_EXPECT("the IoContext wasn't aborted");
  }
}

KJ_TEST("a stateless context's cross-context actions wait without aborting it once its request "
        "has ended") {
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS)});
  auto context = fixture.newStatelessIoContext();
  fixture.drainAndDestroy(fixture.newIncomingRequest(*context));

  uint ran = 0;
  auto executor = context->getCrossContextExecutor();
  KJ_ASSERT(executor.tryExecute([&ran](jsg::Lock&) { ++ran; }));
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 0, ran);
  KJ_EXPECT(executor.getQueuedActionCountForTest() == 1, executor.getQueuedActionCountForTest());
  KJ_EXPECT(context->getAbortReason() == kj::none, context->getAbortReason());
}

KJ_TEST("a stateless context's cross-context actions stay queued if its request ends while they "
        "wait for the isolate lock") {
  auto io = kj::setupAsyncIo();
  // Each fixture initializes the process's autogates, so both enable it.
  TestFixture fixture({.waitScope = io.waitScope,
    .autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .useRealTimers = false});
  TestFixture other({.waitScope = io.waitScope,
    .autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .useRealTimers = false});
  auto context = fixture.newStatelessIoContext();
  auto request = fixture.newIncomingRequest(*context);

  // Holding another isolate's lock holds up the run, since a thread takes one at a time.
  auto asyncLock = other.takeAsyncLock();
  uint ran = 0;
  auto executor = context->getCrossContextExecutor();
  KJ_ASSERT(executor.tryExecute([&ran](jsg::Lock&) { ++ran; }));
  fixture.pollEventLoop();

  fixture.drainAndDestroy(kj::mv(request));
  { auto released = kj::mv(asyncLock); }
  fixture.pollEventLoop();
  KJ_EXPECT(ran == 0, ran);
  KJ_EXPECT(executor.getQueuedActionCountForTest() == 1, executor.getQueuedActionCountForTest());
  KJ_EXPECT(context->getAbortReason() == kj::none, context->getAbortReason());
}

// An actor whose class publishes its `ctx`. createPromise() creates a promise for another context
// to settle with settle(). The promise's reaction uses the actor's storage, which needs a current
// request, and appends its progress to `globalThis.events`.
constexpr kj::StringPtr PROMISE_ACTOR_SOURCE = R"SCRIPT(
  import { DurableObject } from "cloudflare:workers";
  globalThis.events = "";
  export default class extends DurableObject {
    constructor(ctx, env) {
      super(ctx, env);
      globalThis.ctx = ctx;
    }
  }
  globalThis.createPromise = (key) => {
    const { promise, resolve } = Promise.withResolvers();
    globalThis.settle = resolve;
    promise.then(async (value) => {
      globalThis.events += "reaction;";
      await globalThis.ctx.storage.put(key, value);
      globalThis.events += `stored:${await globalThis.ctx.storage.get(key)};`;
    });
  };
)SCRIPT"_kj;

// Calls the global function `name` with the string `argument`.
void callGlobal(jsg::Lock& js, kj::StringPtr name, kj::StringPtr argument) {
  auto function = KJ_ASSERT_NONNULL(js.global().get(js, name).tryCast<jsg::JsFunction>());
  function.call(js, js.undefined(), js.str(argument));
}

kj::String readEvents(TestFixture& fixture) {
  kj::String events;
  fixture.enterWorkerLock([&](Worker::Lock& lock) {
    jsg::Lock& js = lock;
    js.withinHandleScope([&]() {
      v8::Context::Scope contextScope(lock.getContext());
      events = js.global().get(js, "events"_kj).toString(js);
    });
  });
  return events;
}

// Runs `body` with an actor running PROMISE_ACTOR_SOURCE that created a promise in a request that
// has since ended. A stateless context in the same isolate has settled the promise, so its
// reaction is queued on the idle actor.
void withPromiseSettledOnIdleActor(
    kj::StringPtr actorId, kj::FunctionParam<void(TestFixture&, IoContext&)> body) {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setHandleCrossRequestPromiseResolution(true);
  TestFixture fixture({.featureFlags = flags.asReader(),
    .autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .mainModuleSource = PROMISE_ACTOR_SOURCE,
    .actorId = Worker::Actor::Id(kj::str(actorId)),
    .actorClassName = "default"_kj});
  auto context = fixture.newIoContext();
  fixture.getActor().setIoContext(kj::addRef(*context));

  auto creator = fixture.newIncomingRequest(*context);
  fixture.enterContext(*creator,
      [](TestFixture::Environment& env) { callGlobal(env.js, "createPromise"_kj, "key"_kj); });
  fixture.drainAndDestroy(kj::mv(creator));

  auto statelessContext = fixture.newStatelessIoContext();
  auto settler = fixture.newIncomingRequest(*statelessContext);
  fixture.enterContext(
      *settler, [](TestFixture::Environment& env) { callGlobal(env.js, "settle"_kj, "value"_kj); });
  fixture.drainAndDestroy(kj::mv(settler));
  fixture.pollEventLoop();
  KJ_EXPECT(context->getCrossContextExecutor().getQueuedActionCountForTest() == 1);
  KJ_EXPECT(readEvents(fixture) == "", readEvents(fixture));
  KJ_EXPECT(context->getAbortReason() == kj::none, context->getAbortReason());

  body(fixture, *context);
}

KJ_TEST("a promise settled from another context waits for an idle actor's next request") {
  withPromiseSettledOnIdleActor(
      "idle-promise-test"_kj, [](TestFixture& fixture, IoContext& context) {
    auto request = fixture.newIncomingRequest(context);
    fixture.pollEventLoop();
    KJ_EXPECT(readEvents(fixture) == "reaction;stored:value;", readEvents(fixture));
    fixture.drainAndDestroy(kj::mv(request));
  });
}

KJ_TEST("an idle actor's next request doesn't finish draining before a promise settled from "
        "another context runs") {
  withPromiseSettledOnIdleActor(
      "short-request-promise-test"_kj, [](TestFixture& fixture, IoContext& context) {
    // The next request drains at once, like an event that awaits nothing.
    auto request = fixture.newIncomingRequest(context);
    ExpectNoTaskFailures errorHandler;
    kj::TaskSet drains(errorHandler);
    request->drain(drains, kj::mv(request));
    drains.onEmpty().wait(fixture.getWaitScope());
    KJ_EXPECT(readEvents(fixture) == "reaction;stored:value;", readEvents(fixture));
    KJ_EXPECT(!context.hasCurrentIncomingRequest());
  });
}

}  // namespace
}  // namespace workerd
