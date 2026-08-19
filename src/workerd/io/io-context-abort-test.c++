// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests of IoContext::abort() behavior.

#include <workerd/io/io-context.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("JS queued before abort() does not run after it") {
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  auto& context = request->getContext();
  auto& waitScope = fixture.getWaitScope();

  bool firstRan = false;
  bool secondRan = false;

  // Simulate outstanding I/O, as in a real request that has pending subrequests, streams, etc.
  // (e.g. awaitIo() attaches a pending event to every promise it awaits). Without this, the
  // registerPendingEvent() call in runImpl() happens to throw the abort exception as a side
  // effect of hang detection, masking the gap this test covers. With an existing PendingEvent
  // alive -- or in an actor context, which doesn't use pending events at all -- it performs no
  // abort check.
  auto pendingEvent = context.registerPendingEvent();

  // Make two back-to-back calls to run(). Both pass the abort check at the top of runSingle()
  // before either has acquired the isolate lock and entered JS. The first run aborts the context,
  // so the second must be refused rather than executed.
  auto first = context.run([&firstRan](Worker::Lock&, IoContext& context) {
    firstRan = true;
    context.abort(KJ_EXCEPTION(FAILED, "jsg.Error: test abort reason"));
  });
  auto second = context.run([&secondRan](Worker::Lock&) { secondRan = true; });

  first.wait(waitScope);
  KJ_EXPECT(firstRan);

  KJ_EXPECT_THROW_MESSAGE("test abort reason", second.wait(waitScope));
  KJ_EXPECT(!secondRan);

  fixture.drainAndDestroy(kj::mv(request));
}

}  // namespace
}  // namespace workerd
