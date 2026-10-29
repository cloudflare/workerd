// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests for IoContext::IncomingRequest lifecycle behavior.

#include <workerd/io/io-context.h>
#include <workerd/io/trace-stream.h>
#include <workerd/io/tracer.h>
#include <workerd/io/worker.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/async.h>
#include <kj/test.h>

namespace workerd {
namespace {

class ErrorHandlerImpl: public kj::TaskSet::ErrorHandler {
 public:
  void taskFailed(kj::Exception&& exception) override {
    KJ_FAIL_EXPECT(exception);
  }
};

class ThrowOnDestruction: private kj::UnwindDetector {
 public:
  ~ThrowOnDestruction() noexcept(false) {
    catchExceptionsIfUnwinding([]() { KJ_FAIL_REQUIRE("test cancellation failure"); });
  }
};

class FrozenTimerChannel final: public TimerChannel {
 public:
  void syncTime() override {
    currentTime = wallTime;
  }

  kj::Date now(kj::Maybe<kj::Date>) override {
    return currentTime;
  }

  kj::Promise<void> atTime(kj::Date) override {
    return kj::NEVER_DONE;
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration) override {
    return kj::NEVER_DONE;
  }

  void advance(kj::Duration duration) {
    wallTime += duration;
  }

  kj::Date getWallTime() const {
    return wallTime;
  }

 private:
  kj::Date wallTime = kj::UNIX_EPOCH;
  kj::Date currentTime = kj::UNIX_EPOCH;
};

SpanParent traceScopeParent(uint64_t id) {
  return SpanParent::fromSpanContext(
      tracing::SpanContext(tracing::TraceId(1, 2), tracing::SpanId(id)));
}

uint64_t traceScopeSpanId(SpanParent parent) {
  auto context = parent.toSpanContext();
  return KJ_ASSERT_NONNULL(KJ_ASSERT_NONNULL(context).getSpanId()).getId();
}

class TraceScopeRequestObserver final: public RequestObserver {
 public:
  SpanParent getSpan() override {
    return traceScopeParent(11);
  }
};

KJ_TEST("request trace scopes capture both roots and restore enclosing tracing and storage") {
  for (bool observeUser: {false, true}) {
    TestFixture fixture({
      .useRealTimers = false,
      .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
          []() -> kj::Own<RequestObserver> { return kj::refcounted<TraceScopeRequestObserver>(); }),
    });
    auto context = fixture.newIoContext();
    kj::Maybe<kj::Rc<BaseTracer>> tracer;
    if (observeUser) {
      auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
          kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS);
      auto workerTracer =
          kj::rc<WorkerTracer>(kj::none, kj::mv(trace), PipelineLogLevel::NONE, kj::none, kj::none);
      workerTracer->setMakeUserRequestSpanContextFunc(
          [](tracing::SpanContext) { return traceScopeParent(22); });
      tracer = kj::Rc<BaseTracer>(kj::mv(workerTracer));
    }
    auto request = fixture.newUndeliveredIncomingRequest(*context, kj::mv(tracer));
    request->delivered();
    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      auto key = kj::arc<jsg::AsyncContextFrame::StorageKey>();
      jsg::AsyncContextFrame::StorageScope storageScope(
          env.js, key.addRef(), env.js.v8Ref(v8::Integer::New(env.isolate, 55).As<v8::Value>()));
      auto internalScope = context->makeAsyncTraceScope(env.lock, traceScopeParent(33));
      auto userScope = context->makeUserAsyncTraceScope(env.lock, traceScopeParent(44));
      auto& prior = KJ_ASSERT_NONNULL(jsg::AsyncContextFrame::current(env.js));
      kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> captured;
      auto checkRequestScope = [&]() {
        KJ_EXPECT(traceScopeSpanId(context->getCurrentTraceSpan()) == 11);
        auto userParent = context->getCurrentUserTraceSpan();
        KJ_EXPECT(userParent.isObserved() == observeUser);
        if (observeUser) {
          KJ_EXPECT(traceScopeSpanId(kj::mv(userParent)) == 22);
        }
        auto& current = KJ_ASSERT_NONNULL(jsg::AsyncContextFrame::current(env.js));
        auto value = KJ_ASSERT_NONNULL(current.get(*key)).getHandle(env.js);
        KJ_EXPECT(value.As<v8::Int32>()->Value() == 55);
      };
      {
        auto scope = context->makeRequestTraceScope(env.lock);
        checkRequestScope();
        captured = jsg::AsyncContextFrame::currentRef(env.js);
      }
      KJ_EXPECT(&KJ_ASSERT_NONNULL(jsg::AsyncContextFrame::current(env.js)) == &prior);
      KJ_EXPECT(traceScopeSpanId(context->getCurrentTraceSpan()) == 33);
      KJ_EXPECT(traceScopeSpanId(context->getCurrentUserTraceSpan()) == 44);
      {
        jsg::AsyncContextFrame::Scope scope(env.js, captured);
        checkRequestScope();
      }
    });
    fixture.drainAndDestroy(kj::mv(request));
  }
}

KJ_TEST("trace onset synchronizes an idle actor's clock before reading it") {
  FrozenTimerChannel timer;
  kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)> makeChannelFactory =
      [&timer](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timer);
  };
  TestFixture fixture({
    .actorId = Worker::Actor::Id(kj::str("trace-timing-test")),
    .useRealTimers = false,
    .ioChannelFactory = kj::mv(makeChannelFactory),
  });

  auto context = fixture.newIoContext();
  auto previousRequest = fixture.newIncomingRequest(*context);
  fixture.drainAndDestroy(kj::mv(previousRequest));

  timer.advance(3 * kj::SECONDS);

  auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
      kj::Array<kj::String>(), kj::none, ExecutionModel::DURABLE_OBJECT);
  auto traceRef = kj::addRef(*trace);
  kj::Rc<BaseTracer> tracer =
      kj::rc<WorkerTracer>(kj::none, kj::mv(trace), PipelineLogLevel::FULL, kj::none, kj::none);
  auto request = fixture.newUndeliveredIncomingRequest(*context, kj::mv(tracer));

  KJ_EXPECT(request->now() == kj::UNIX_EPOCH);
  KJ_ASSERT_NONNULL(request->getWorkerTracer()).setEventInfo(*request, tracing::CustomEventInfo());
  KJ_EXPECT(traceRef->eventTimestamp == timer.getWallTime());

  request->delivered();
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("cloning an incoming request tracer keeps it alive until the clone is dropped") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
      kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS);
  auto tracer =
      kj::rc<WorkerTracer>(kj::none, kj::mv(trace), PipelineLogLevel::FULL, kj::none, kj::none);
  auto complete = tracer->onComplete();
  auto request = fixture.newUndeliveredIncomingRequest(*context, kj::mv(tracer));
  KJ_ASSERT_NONNULL(request->getWorkerTracer()).setEventInfo(*request, tracing::CustomEventInfo());
  request->delivered();

  auto clone = request->getWorkerTracer().clone();
  KJ_ASSERT_NONNULL(clone);
  fixture.drainAndDestroy(kj::mv(request));
  KJ_EXPECT(!complete.poll(fixture.getWaitScope()));

  clone = kj::none;
  KJ_ASSERT(complete.poll(fixture.getWaitScope()));
  complete.wait(fixture.getWaitScope());
}

KJ_TEST("final request cancels context tasks before it stops being current") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  auto& context = request->getContext();

  bool taskWasCanceledWhileRequestWasCurrent = false;
  context.addTask(kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&]() {
    taskWasCanceledWhileRequestWasCurrent = context.hasCurrentIncomingRequest();
  })));

  fixture.drainAndDestroy(kj::mv(request));

  KJ_EXPECT(taskWasCanceledWhileRequestWasCurrent);
}

KJ_TEST(
    "request owning the final IoContext reference cancels reentry before it stops being current") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  auto& context = request->getContext();
  auto& waitScope = fixture.getWaitScope();

  // Value-initialized to false.
  kj::Pin<bool> callbackWasCanceledWhileRequestWasCurrent;
  kj::Function<kj::Promise<void>()> callback;
  context
      .run([&](Worker::Lock&, IoContext& context) {
    callback = context.makeReentryCallback(
        [canceled = callbackWasCanceledWhileRequestWasCurrent.addWeak()](
            Worker::Lock&, IoContext& context) {
      return kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&context, canceled]() {
        KJ_REQUIRE_NONNULL(canceled.tryGet(), "test ended before callback was canceled") =
            context.hasCurrentIncomingRequest();
      }));
    });
  }).wait(waitScope);
  auto pendingTask = callback();
  KJ_EXPECT(!pendingTask.poll(waitScope));

  fixture.drainAndDestroy(kj::mv(request));

  KJ_EXPECT(*callbackWasCanceledWhileRequestWasCurrent);
  KJ_EXPECT_THROW_MESSAGE(
      "The execution context responding to this call was canceled", pendingTask.wait(waitScope));
}

KJ_TEST("request owning the final IoContext reference preserves its abort reason") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  auto& context = request->getContext();
  auto& waitScope = fixture.getWaitScope();

  kj::Function<kj::Promise<void>()> callback;
  context
      .run([&callback](Worker::Lock&, IoContext& context) {
    callback = context.makeReentryCallback(
        [](Worker::Lock&, IoContext&) { return kj::Promise<void>(kj::NEVER_DONE); });
  }).wait(waitScope);
  auto pendingTask = callback();
  KJ_EXPECT(!pendingTask.poll(waitScope));
  context.abort(KJ_EXCEPTION(FAILED, "jsg.Error: test abort reason"));

  fixture.drainAndDestroy(kj::mv(request));

  KJ_EXPECT_THROW_MESSAGE("test abort reason", pendingTask.wait(waitScope));
}

KJ_TEST("final request cancels wait-until tasks before it stops being current") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  auto& context = request->getContext();

  bool waitUntilWasCanceledWhileRequestWasCurrent = false;
  context.addWaitUntil(kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&]() {
    waitUntilWasCanceledWhileRequestWasCurrent = context.hasCurrentIncomingRequest();
  })));
  context.abort(KJ_EXCEPTION(FAILED, "test abort reason"));

  fixture.drainAndDestroy(kj::mv(request));

  KJ_EXPECT(waitUntilWasCanceledWhileRequestWasCurrent);
}

KJ_TEST("final request cancels pending timers before it stops being current") {
  // Counts the timers it arms, and those canceled while the context still had a current request.
  struct RecordingTimerChannel final: public TimerChannel {
    void syncTime() override {}
    kj::Date now(kj::Maybe<kj::Date>) override {
      return kj::UNIX_EPOCH;
    }
    kj::Promise<void> atTime(kj::Date) override {
      ++armed;
      return kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([this]() {
        if (KJ_ASSERT_NONNULL(context).hasCurrentIncomingRequest())
          ++canceledWhileRequestWasCurrent;
      }));
    }
    kj::Promise<void> afterLimitTimeout(kj::Duration) override {
      return kj::NEVER_DONE;
    }

    kj::Maybe<IoContext&> context;
    uint armed = 0;
    uint canceledWhileRequestWasCurrent = 0;
  };

  RecordingTimerChannel timer;
  kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)> makeChannelFactory =
      [&timer](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timer);
  };
  TestFixture fixture({.useRealTimers = false, .ioChannelFactory = kj::mv(makeChannelFactory)});
  auto request = fixture.newIncomingRequest();
  timer.context = request->getContext();

  fixture.enterContext(*request, [](const TestFixture::Environment& env) {
    for (auto delayMs: {1000.0, 2000.0}) {
      env.context.setTimeoutImpl(env.lock.getTimeoutIdGenerator(), false,
          jsg::Function<void()>([](jsg::Lock&) {}), delayMs);
    }
  });
  KJ_ASSERT(timer.armed > 0);
  KJ_ASSERT(timer.canceledWhileRequestWasCurrent == 0);

  fixture.drainAndDestroy(kj::mv(request));

  KJ_EXPECT(timer.canceledWhileRequestWasCurrent == timer.armed, timer.armed,
      timer.canceledWhileRequestWasCurrent);
}

KJ_TEST("final request completes cleanup after cancellation throws") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  auto& context = request->getContext();
  auto& waitScope = fixture.getWaitScope();

  // Value-initialized to false.
  kj::Pin<bool> callbackWasCanceledWhileRequestWasCurrent;
  bool taskWasCanceledWhileRequestWasCurrent = false;
  kj::Function<kj::Promise<void>()> callback;
  kj::Function<kj::Promise<void>()> throwingCallback;
  context
      .run([&](Worker::Lock&, IoContext& context) {
    callback = context.makeReentryCallback(
        [canceled = callbackWasCanceledWhileRequestWasCurrent.addWeak()](
            Worker::Lock&, IoContext& context) {
      return kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&context, canceled]() {
        KJ_REQUIRE_NONNULL(canceled.tryGet(), "test ended before callback was canceled") =
            context.hasCurrentIncomingRequest();
      }));
    });
    throwingCallback = context.makeReentryCallback([](Worker::Lock&, IoContext&) {
      return kj::Promise<void>(kj::NEVER_DONE).attach(kj::heap<ThrowOnDestruction>());
    });
  }).wait(waitScope);
  auto pendingTask = callback();
  auto throwingPendingTask = throwingCallback();
  KJ_EXPECT(!pendingTask.poll(waitScope));
  KJ_EXPECT(!throwingPendingTask.poll(waitScope));

  context.addTask(kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&]() {
    taskWasCanceledWhileRequestWasCurrent = context.hasCurrentIncomingRequest();
  })));

  KJ_EXPECT_THROW_MESSAGE("test cancellation failure", [&]() { request = nullptr; }());

  KJ_EXPECT(*callbackWasCanceledWhileRequestWasCurrent);
  KJ_EXPECT(taskWasCanceledWhileRequestWasCurrent);
}

KJ_TEST("request on an aborted shared context completes cleanup after cancellation throws") {
  TestFixture fixture;
  auto& waitScope = fixture.getWaitScope();
  bool callbackCanceled = false;
  bool taskCanceled = false;
  bool waitUntilCanceled = false;
  // The fixture's own reference keeps the context shared after the request is destroyed.
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);

  kj::Function<kj::Promise<void>()> callback;
  kj::Function<kj::Promise<void>()> throwingCallback;
  context
      ->run([&](Worker::Lock&, IoContext& context) {
    callback = context.makeReentryCallback([&](Worker::Lock&, IoContext&) {
      return kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&]() {
        callbackCanceled = true;
      }));
    });
    throwingCallback = context.makeReentryCallback([](Worker::Lock&, IoContext&) {
      return kj::Promise<void>(kj::NEVER_DONE).attach(kj::heap<ThrowOnDestruction>());
    });
  }).wait(waitScope);
  // The canceler cancels the most recent callback first, so the throwing one goes first.
  auto pendingTask = callback();
  auto throwingPendingTask = throwingCallback();
  KJ_EXPECT(!pendingTask.poll(waitScope));
  KJ_EXPECT(!throwingPendingTask.poll(waitScope));
  context->addTask(
      kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&]() { taskCanceled = true; })));
  context->addWaitUntil(
      kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&]() { waitUntilCanceled = true; })));

  context->abort(KJ_EXCEPTION(FAILED, "test abort reason"));
  request = nullptr;

  KJ_EXPECT(callbackCanceled);
  KJ_EXPECT(taskCanceled);
  KJ_EXPECT(waitUntilCanceled);
}

// Regression test: two IncomingRequests share a single actor IoContext, as happens when a Durable
// Object receives overlapping requests. Draining the older, superseded request hits drain()'s
// "a newer request has taken over" early return.
//
// drain() takes ownership of the request via an rvalue-reference `self` parameter, but it must
// actually consume `self` on *every* return path — including the early return. Otherwise the
// caller's owner lingers for as long as the caller holds its kj::Own. In production the caller is
// a hibernatable WebSocket's deferred-proxy task, whose owner lives for the entire connection, so
// failing to release it here pins the IncomingRequest (and the actor ActiveRequest it carries) and
// prevents the Durable Object from ever hibernating.
KJ_TEST("IoContext::IncomingRequest::drain() releases a superseded (non-front) request") {
  TestFixture fixture({.actorId = Worker::Actor::Id(kj::str("drain-test"))});

  // One IoContext (the actor) with two IncomingRequests delivered against it. delivered() adds the
  // request to the front of the list, so `second` becomes the front and `first` is superseded.
  auto context = fixture.newIoContext();
  auto first = fixture.newIncomingRequest(*context);
  auto second = fixture.newIncomingRequest(*context);

  ErrorHandlerImpl errorHandler;
  kj::TaskSet waitUntilTasks(errorHandler);

  // Drain the superseded request. `kj::mv(first)` only casts to an rvalue reference; the Own is
  // not cleared unless drain() moves out of it. So `first` being null afterwards proves that
  // drain() took ownership on the early-return path.
  first->drain(waitUntilTasks, kj::mv(first));
  KJ_EXPECT(first.get() == nullptr,
      "drain() must consume `self` even when a newer request has already taken over");

  // The early-return path schedules no background work.
  KJ_EXPECT(waitUntilTasks.isEmpty());

  // Tidy up the still-live front request so its destructor doesn't warn about undrained tasks.
  fixture.drainAndDestroy(kj::mv(second));
}

KJ_TEST("cancelPendingWorkForHibernation() cancels pending work but keeps abort watchers armed") {
  TestFixture fixture({.actorId = Worker::Actor::Id(kj::str("hibernate-test"))});
  auto& waitScope = fixture.getWaitScope();
  // Declared before the context, whose destructor cancels the watcher.
  bool taskCanceled = false;
  bool watcherCanceled = false;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);

  context->addTask(
      kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&]() { taskCanceled = true; })));

  kj::Function<kj::Promise<void>()> callback;
  context
      ->run([&callback](Worker::Lock&, IoContext& context) {
    callback = context.makeReentryCallback(
        [](Worker::Lock&, IoContext&) { return kj::Promise<void>(kj::NEVER_DONE); });
  }).wait(waitScope);
  auto pendingCallback = callback();
  KJ_EXPECT(!pendingCallback.poll(waitScope));

  context->abortWhen(
      kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&]() { watcherCanceled = true; })));

  request->cancelPendingWorkForHibernation();

  KJ_EXPECT(taskCanceled);
  KJ_EXPECT_THROW_MESSAGE("The Durable Object is hibernating.", pendingCallback.wait(waitScope));
  KJ_EXPECT(!watcherCanceled);

  // A callback that is still held, e.g. by an RPC target, can't start new work either.
  KJ_EXPECT_THROW_MESSAGE("The Durable Object is hibernating.", callback());
}

KJ_TEST("cancelPendingWorkForHibernation() drops cross-context actions") {
  TestFixture fixture({.actorId = Worker::Actor::Id(kj::str("hibernate-cross-context-test"))});
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  auto executor = context->getCrossContextExecutor();

  // Cross-context promise resolution delivers reactions this way, on the context's next turn.
  bool ranBefore = false;
  KJ_ASSERT(executor.tryExecute([&](jsg::Lock&) { ranBefore = true; }));
  fixture.pollEventLoop();
  KJ_EXPECT(ranBefore);

  request->cancelPendingWorkForHibernation();

  bool ranAfter = false;
  KJ_ASSERT(executor.tryExecute([&](jsg::Lock&) { ranAfter = true; }));
  fixture.pollEventLoop();
  KJ_EXPECT(!ranAfter);
}

KJ_TEST("cancelPendingWorkForHibernation() logs a throwing destructor and keeps canceling") {
  TestFixture fixture({.actorId = Worker::Actor::Id(kj::str("hibernate-throw-test"))});
  auto& waitScope = fixture.getWaitScope();
  bool callbackCanceled = false;
  bool waitUntilCanceled = false;
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);

  kj::Function<kj::Promise<void>()> callback;
  kj::Function<kj::Promise<void>()> throwingCallback;
  context
      ->run([&](Worker::Lock&, IoContext& context) {
    callback = context.makeReentryCallback([&](Worker::Lock&, IoContext&) {
      return kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&]() {
        callbackCanceled = true;
      }));
    });
    throwingCallback = context.makeReentryCallback([](Worker::Lock&, IoContext&) {
      return kj::Promise<void>(kj::NEVER_DONE).attach(kj::heap<ThrowOnDestruction>());
    });
  }).wait(waitScope);
  // The canceler cancels the most recent callback first, so the throwing one goes first.
  auto pendingCallback = callback();
  auto throwingPendingCallback = throwingCallback();
  KJ_EXPECT(!pendingCallback.poll(waitScope));
  KJ_EXPECT(!throwingPendingCallback.poll(waitScope));

  context->addWaitUntil(
      kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&]() { waitUntilCanceled = true; })));

  KJ_EXPECT_LOG(ERROR, "test cancellation failure");
  request->cancelPendingWorkForHibernation();

  KJ_EXPECT(callbackCanceled);
  KJ_EXPECT(waitUntilCanceled);
}

// Constructs the fixture's actor from `source`, a Durable Object class with an onHibernate
// handler that appends to `globalThis.events`, then calls `body`.
void withOnHibernateActor(kj::StringPtr source,
    kj::StringPtr actorId,
    kj::FunctionParam<void(TestFixture&, IoContext&, Worker::Actor&)> body,
    bool flushActorStorage = false) {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setWorkerdExperimental(true);
  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = source,
    .actorId = Worker::Actor::Id(kj::str(actorId)),
    .actorClassName = "default"_kj,
    .useRealTimers = false,
    .flushActorStorage = flushActorStorage,
  });
  auto& actor = fixture.getActor();
  auto context = fixture.newIoContext();
  actor.setIoContext(kj::addRef(*context));

  // Delivering a request constructs the class instance, which reads the handler.
  auto constructRequest = fixture.newIncomingRequest(*context);
  fixture.pollEventLoop();
  fixture.drainAndDestroy(kj::mv(constructRequest));
  KJ_ASSERT(actor.hasOnHibernateHandler());

  body(fixture, *context, actor);
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

// Records the outcome that runOnHibernate() reports.
struct OnHibernateObserver final: public RequestObserver {
  kj::Maybe<EventOutcome> outcome;
  void reportOnHibernateOutcome(EventOutcome reported) override {
    outcome = reported;
  }
};

kj::Promise<void> runOnHibernate(TestFixture& fixture,
    Worker::Actor& actor,
    kj::Own<RequestObserver> observer,
    kj::Maybe<TimerChannel&> timer = kj::none) {
  return actor.runOnHibernate(Worker::Actor::HibernationReason::INACTIVE,
      kj::rc<TestFixture::DummyIoChannelFactory>(timer.orDefault(fixture.getTimerChannel())),
      kj::mv(observer), kj::none);
}

// An actor whose onHibernate handler never settles, leaving a timer behind.
constexpr kj::StringPtr PENDING_ON_HIBERNATE_SOURCE = R"SCRIPT(
  import { DurableObject } from "cloudflare:workers";
  import { onHibernate } from "cloudflare:durable-objects";
  globalThis.events = "";
  export default class extends DurableObject {
    async [onHibernate]() {
      globalThis.events += "onHibernate;";
      setTimeout(() => { globalThis.events += "timer;"; }, 1000);
      await new Promise(() => {});
    }
  }
)SCRIPT"_kj;

// Starts the onHibernate handler of an actor running PENDING_ON_HIBERNATE_SOURCE, then calls
// `body` while the handler is pending.
void withPendingOnHibernate(kj::StringPtr actorId,
    kj::FunctionParam<void(TestFixture&, IoContext&, Worker::Actor&, kj::Promise<void>&)> body) {
  withOnHibernateActor(PENDING_ON_HIBERNATE_SOURCE, actorId,
      [&](TestFixture& fixture, IoContext& context, Worker::Actor& actor) {
    auto promise = runOnHibernate(fixture, actor, kj::refcounted<RequestObserver>());
    KJ_EXPECT(!promise.poll(fixture.getWaitScope()));
    auto events = readEvents(fixture);
    KJ_ASSERT(events == "onHibernate;", events);

    body(fixture, context, actor, promise);
  });
}

KJ_TEST("runOnHibernate() runs the handler with the reason and resolves when it settles") {
  constexpr kj::StringPtr source = R"SCRIPT(
    import { DurableObject } from "cloudflare:workers";
    import { onHibernate } from "cloudflare:durable-objects";
    globalThis.events = "";
    export default class extends DurableObject {
      async [onHibernate](info) {
        globalThis.events += `onHibernate:${info.reason}:${Object.isFrozen(info)};`;
      }
    }
  )SCRIPT"_kj;
  withOnHibernateActor(source, "on-hibernate-success-test"_kj,
      [](TestFixture& fixture, IoContext&, Worker::Actor& actor) {
    auto observer = kj::refcounted<OnHibernateObserver>();
    runOnHibernate(fixture, actor, kj::addRef(*observer)).wait(fixture.getWaitScope());

    KJ_EXPECT(observer->outcome == EventOutcome::OK);
    KJ_EXPECT(readEvents(fixture) == "onHibernate:inactive:true;");
  });
}

KJ_TEST("runOnHibernate() resolves when the handler throws, reporting the failure") {
  constexpr kj::StringPtr source = R"SCRIPT(
    import { DurableObject } from "cloudflare:workers";
    import { onHibernate } from "cloudflare:durable-objects";
    globalThis.events = "";
    export default class extends DurableObject {
      async [onHibernate]() {
        throw new Error("handler failed");
      }
    }
  )SCRIPT"_kj;
  withOnHibernateActor(source, "on-hibernate-throw-test"_kj,
      [](TestFixture& fixture, IoContext&, Worker::Actor& actor) {
    auto observer = kj::refcounted<OnHibernateObserver>();
    runOnHibernate(fixture, actor, kj::addRef(*observer)).wait(fixture.getWaitScope());

    KJ_EXPECT(observer->outcome == EventOutcome::EXCEPTION);
  });
}

// A TimerChannel whose limit timeouts have already expired.
struct ExpiredLimitTimerChannel final: public TimerChannel {
  explicit ExpiredLimitTimerChannel(TimerChannel& inner): inner(inner) {}
  void syncTime() override {
    inner.syncTime();
  }
  kj::Date now(kj::Maybe<kj::Date> clockClamp) override {
    return inner.now(clockClamp);
  }
  kj::Promise<void> atTime(kj::Date when) override {
    return inner.atTime(when);
  }
  kj::Promise<void> afterLimitTimeout(kj::Duration) override {
    return kj::READY_NOW;
  }
  TimerChannel& inner;
};

KJ_TEST("runOnHibernate() resolves when the handler exceeds its budget") {
  withOnHibernateActor(PENDING_ON_HIBERNATE_SOURCE, "on-hibernate-timeout-test"_kj,
      [](TestFixture& fixture, IoContext& context, Worker::Actor& actor) {
    ExpiredLimitTimerChannel timer(fixture.getTimerChannel());
    auto observer = kj::refcounted<OnHibernateObserver>();
    runOnHibernate(fixture, actor, kj::addRef(*observer), timer).wait(fixture.getWaitScope());

    KJ_EXPECT(observer->outcome == EventOutcome::EXCEEDED_WALL_TIME);
    KJ_EXPECT(readEvents(fixture) == "onHibernate;");
    KJ_EXPECT(context.getTimeoutCount() == 0);
  });
}

KJ_TEST("runOnHibernate() rejects when the handler's writes can't be flushed") {
  // The fixture's actor storage is read-only, so the flush of the write fails.
  constexpr kj::StringPtr source = R"SCRIPT(
    import { DurableObject } from "cloudflare:workers";
    import { onHibernate } from "cloudflare:durable-objects";
    globalThis.events = "";
    export default class extends DurableObject {
      async [onHibernate]() {
        this.ctx.storage.put("key", "value");
      }
    }
  )SCRIPT"_kj;
  withOnHibernateActor(source, "on-hibernate-flush-test"_kj,
      [](TestFixture& fixture, IoContext&, Worker::Actor& actor) {
    auto observer = kj::refcounted<OnHibernateObserver>();
    auto promise = runOnHibernate(fixture, actor, kj::addRef(*observer));

    KJ_EXPECT_LOG(ERROR, "actorCacheFlush");
    KJ_EXPECT_THROW_MESSAGE("broken.outputGateBroken", promise.wait(fixture.getWaitScope()));
    KJ_EXPECT(observer->outcome == EventOutcome::OK);
  }, /*flushActorStorage=*/true);
}

KJ_TEST("canceling runOnHibernate() cancels pending work but keeps abort watchers armed") {
  // These outlive the fixture, whose actor owns the context that holds the task and watcher.
  bool taskCanceled = false;
  bool watcherCanceled = false;
  withPendingOnHibernate("on-hibernate-cancel-test"_kj,
      [&](TestFixture&, IoContext& context, Worker::Actor&, kj::Promise<void>& promise) {
    context.addTask(
        kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&]() { taskCanceled = true; })));
    context.abortWhen(
        kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([&]() { watcherCanceled = true; })));
    KJ_EXPECT(context.getTimeoutCount() == 1);

    promise = nullptr;

    KJ_EXPECT(taskCanceled);
    KJ_EXPECT(context.getTimeoutCount() == 0);
    KJ_EXPECT(!watcherCanceled);
  });
}

KJ_TEST("aborting the actor cancels a pending runOnHibernate()") {
  withPendingOnHibernate("on-hibernate-abort-test"_kj,
      [](TestFixture& fixture, IoContext& context, Worker::Actor& actor,
          kj::Promise<void>& promise) {
    actor.abort(KJ_EXCEPTION(FAILED, "test abort"));
    // abort() destroys the handler's IncomingRequest synchronously, so it can tear down the
    // IoContext.
    KJ_EXPECT(!context.hasCurrentIncomingRequest());
    KJ_EXPECT_THROW_MESSAGE("test abort", promise.wait(fixture.getWaitScope()));
  });
}

KJ_TEST("ambient IoContext is hidden while another V8 isolate is entered") {
  auto io = kj::setupAsyncIo();
  TestFixture outer({.waitScope = io.waitScope, .useRealTimers = false});
  TestFixture inner({.waitScope = io.waitScope, .useRealTimers = false});

  outer.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(IoContext::hasCurrent());
    KJ_EXPECT(&KJ_ASSERT_NONNULL(IoContext::tryCurrent()) == &env.context);
    KJ_EXPECT(env.context.isCurrent());
    KJ_EXPECT(env.context.getId().isCurrent());

    inner.enterWorkerLockSynchronously([&](Worker::Lock&) {
      KJ_EXPECT(!IoContext::hasCurrent());
      KJ_EXPECT(IoContext::tryCurrent() == kj::none);
      KJ_EXPECT(!env.context.isCurrent());
      KJ_EXPECT(!env.context.getId().isCurrent());
    });

    KJ_EXPECT(IoContext::hasCurrent());
    KJ_EXPECT(&KJ_ASSERT_NONNULL(IoContext::tryCurrent()) == &env.context);
    KJ_EXPECT(env.context.isCurrent());
    KJ_EXPECT(env.context.getId().isCurrent());
  });
}

}  // namespace
}  // namespace workerd
