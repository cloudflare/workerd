// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "canceler.h"

#include <kj/async.h>
#include <kj/test.h>

namespace workerd {
namespace {

kj::Exception testException() {
  return KJ_EXCEPTION(DISCONNECTED, "canceled for testing");
}

KJ_TEST("ReleasingCanceler cancels wrapped promises with the given exception") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  ReleasingCanceler canceler;
  KJ_EXPECT(!canceler.isCanceled());

  auto promise = canceler.wrap(kj::Promise<void>(kj::NEVER_DONE));

  canceler.cancel(testException());
  KJ_EXPECT(canceler.isCanceled());
  KJ_EXPECT_THROW_MESSAGE("canceled for testing", promise.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("canceled for testing", canceler.throwIfCanceled());
}

KJ_TEST("ReleasingCanceler wrap after cancellation rejects immediately") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  ReleasingCanceler canceler;
  canceler.cancel(testException());

  auto promise = canceler.wrap(kj::Promise<void>(kj::READY_NOW));
  KJ_EXPECT_THROW_MESSAGE("canceled for testing", promise.wait(ws));
}

KJ_TEST("ReleasingCanceler constructed pre-canceled behaves as canceled") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  ReleasingCanceler canceler(testException());
  KJ_EXPECT(canceler.isCanceled());

  auto promise = canceler.wrap(kj::Promise<void>(kj::READY_NOW));
  KJ_EXPECT_THROW_MESSAGE("canceled for testing", promise.wait(ws));
}

KJ_TEST("ReleasingCanceler releases (does not cancel) wrapped promises on drop") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto paf = kj::newPromiseAndFulfiller<int>();
  kj::Promise<int> wrapped = nullptr;
  {
    ReleasingCanceler canceler;
    wrapped = canceler.wrap(kj::mv(paf.promise));
  }

  // The canceler is gone but the wrapped promise was released, not canceled: it still
  // completes through its original path.
  KJ_EXPECT(!wrapped.poll(ws));
  paf.fulfiller->fulfill(123);
  KJ_EXPECT(wrapped.wait(ws) == 123);
}

KJ_TEST("ReleasingCanceler fires listeners exactly once on cancellation") {
  ReleasingCanceler canceler;

  int fired = 0;
  ReleasingCanceler::Listener listener(canceler, [&fired]() { ++fired; });
  KJ_EXPECT(fired == 0);

  canceler.cancel(testException());
  KJ_EXPECT(fired == 1);

  // A second cancellation is a no-op.
  canceler.cancel(KJ_EXCEPTION(FAILED, "some other reason"));
  KJ_EXPECT(fired == 1);
  KJ_EXPECT_THROW_MESSAGE("canceled for testing", canceler.throwIfCanceled());
}

KJ_TEST("ReleasingCanceler fires a late listener immediately and never links it") {
  ReleasingCanceler canceler;
  canceler.cancel(testException());

  int fired = 0;
  ReleasingCanceler::Listener listener(canceler, [&fired]() { ++fired; });
  KJ_EXPECT(fired == 1);
}

KJ_TEST("ReleasingCanceler does not fire a listener destroyed before cancellation") {
  ReleasingCanceler canceler;

  int fired = 0;
  {
    ReleasingCanceler::Listener listener(canceler, [&fired]() { ++fired; });
  }

  canceler.cancel(testException());
  KJ_EXPECT(fired == 0);
}

KJ_TEST("ReleasingCanceler listeners may outlive the canceler once fired") {
  int fired = 0;
  kj::Maybe<ReleasingCanceler::Listener> listener;
  {
    ReleasingCanceler canceler;
    listener.emplace(canceler, [&fired]() { ++fired; });
    canceler.cancel(testException());
    KJ_EXPECT(fired == 1);
    // The canceler is destroyed here, before the (already fired, and therefore unlinked)
    // listener; the listener's destructor must not touch it.
  }
  listener = kj::none;
}

KJ_TEST("ReleasingCanceler listener callbacks may destroy other listeners") {
  ReleasingCanceler canceler;

  int fired = 0;
  kj::Maybe<ReleasingCanceler::Listener> second;
  ReleasingCanceler::Listener first(canceler, [&second]() { second = kj::none; });
  second.emplace(canceler, [&fired]() { ++fired; });

  // The first listener destroys the second while the cancellation is being delivered; the
  // second must simply not fire.
  canceler.cancel(testException());
  KJ_EXPECT(fired == 0);
}

KJ_TEST("ReleasingCanceler listener callbacks may register new listeners") {
  ReleasingCanceler canceler;

  // A listener registered from within a callback observes the already-canceled state: it
  // fires immediately (and is never linked) rather than being picked up by the drain.
  int fired = 0;
  kj::Maybe<ReleasingCanceler::Listener> late;
  ReleasingCanceler::Listener first(canceler, [&canceler, &late, &fired]() {
    late.emplace(canceler, [&fired]() { ++fired; });
    KJ_EXPECT(fired == 1);
  });

  canceler.cancel(testException());
  KJ_EXPECT(fired == 1);
}

}  // namespace
}  // namespace workerd
