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

// Regression test for DeleteQueue::scheduleDeletion's cross-thread branch: dropping an IoOwn
// while its IoContext is not current (another request's context, the KJ event loop outside any
// context scope, or another thread) must wake the owning context to drain the deletion, the
// same way scheduleAction() wakes it; historically it was only drained when the context
// happened to run again for some other reason, pinning the object on an idle context.
KJ_TEST("dropping an IoOwn outside the IoContext promptly wakes the context to delete it") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();

  struct Tracked {
    bool& destroyed;
    Tracked(bool& destroyed): destroyed(destroyed) {}
    ~Tracked() {
      destroyed = true;
    }
  };

  bool destroyed = false;
  kj::Maybe<IoOwn<Tracked>> obj;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    obj = env.context.addObject(kj::heap<Tracked>(destroyed));
  });

  // Drop the IoOwn with no IoContext current. This takes scheduleDeletion's cross-thread
  // branch, which enqueues the deletion on the context's cross-thread delete queue.
  obj = kj::none;
  KJ_EXPECT(!destroyed);

  // The queued deletion must be drained by the delete queue's signal task alone -- no further
  // request activity happens on this context.
  fixture.pollEventLoop();
  KJ_EXPECT(destroyed, "cross-thread deletion did not wake the owning IoContext");

  fixture.drainAndDestroy(kj::mv(request));
}

}  // namespace
}  // namespace workerd
