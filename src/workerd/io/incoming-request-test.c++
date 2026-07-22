// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests for IoContext::IncomingRequest lifecycle behavior.

#include <workerd/io/io-context.h>
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

}  // namespace
}  // namespace workerd
