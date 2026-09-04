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
  kj::Own<BaseTracer> tracer = kj::refcounted<WorkerTracer>(
      kj::none, kj::mv(trace), PipelineLogLevel::FULL, kj::none, kj::none);
  auto request = fixture.newUndeliveredIncomingRequest(*context, kj::mv(tracer));

  KJ_EXPECT(request->now() == kj::UNIX_EPOCH);
  KJ_ASSERT_NONNULL(request->getWorkerTracer()).setEventInfo(*request, tracing::CustomEventInfo());
  KJ_EXPECT(traceRef->eventTimestamp == timer.getWallTime());

  request->delivered();
  fixture.drainAndDestroy(kj::mv(request));
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

// abandonTasksForActorShutdown() is what the onEvict hook uses instead of drain(): it
// must cancel leftover I/O while the request is still installed, so nothing resumes on an
// IoContext with no current request (that path Sentry-reports via taskFailed). The end of the
// hook means no more code may run in the actor, so this includes work belonging to other,
// still-live IncomingRequests (an embedder may deliver the hook to an actor that is still
// serving requests): those requests must not be allowed to finish.
KJ_TEST("abandonTasksForActorShutdown() cancels leftover work of all requests") {
  TestFixture fixture({.actorId = Worker::Actor::Id(kj::str("abandon-test"))});

  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);

  bool lastRequestWaitUntilCanceled = false;
  context->addWaitUntil(kj::Promise<void>(kj::NEVER_DONE)
          .attach(kj::defer(
              [&lastRequestWaitUntilCanceled]() { lastRequestWaitUntilCanceled = true; })));
  bool lastRequestTaskCanceled = false;
  context->addTask(kj::Promise<void>(kj::NEVER_DONE)
          .attach(kj::defer([&lastRequestTaskCanceled]() { lastRequestTaskCanceled = true; })));

  request->abandonTasksForActorShutdown();
  request = nullptr;
  KJ_EXPECT(lastRequestWaitUntilCanceled, "sole request's leftover waitUntil must be canceled");
  KJ_EXPECT(lastRequestTaskCanceled, "sole request's leftover task must be canceled");

  auto first = fixture.newIncomingRequest(*context);
  auto second = fixture.newIncomingRequest(*context);

  bool racingWaitUntilCanceled = false;
  context->addWaitUntil(kj::Promise<void>(kj::NEVER_DONE)
          .attach(kj::defer([&racingWaitUntilCanceled]() { racingWaitUntilCanceled = true; })));
  bool racingTaskCanceled = false;
  context->addTask(kj::Promise<void>(kj::NEVER_DONE)
          .attach(kj::defer([&racingTaskCanceled]() { racingTaskCanceled = true; })));

  first->abandonTasksForActorShutdown();
  first = nullptr;
  KJ_EXPECT(racingWaitUntilCanceled,
      "leftover waitUntil must be canceled even while another IncomingRequest is live");
  KJ_EXPECT(racingTaskCanceled,
      "leftover task must be canceled even while another IncomingRequest is live");

  fixture.drainAndDestroy(kj::mv(second));
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
