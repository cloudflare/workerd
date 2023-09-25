// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "io-gate.h"
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("InputGate basics") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  InputGate gate;

  kj::Promise<InputGate::Lock> promise1 = gate.wait();
  kj::Promise<InputGate::Lock> promise2 = gate.wait();
  kj::Promise<InputGate::Lock> promise3 = gate.wait();

  KJ_ASSERT(promise1.poll(ws));
  KJ_EXPECT(!promise2.poll(ws));
  KJ_EXPECT(!promise3.poll(ws));

  {
    auto lock = promise1.wait(ws);

    KJ_EXPECT(!promise2.poll(ws));
    KJ_EXPECT(!promise3.poll(ws));

    auto lock2 = lock.addRef();
    { auto drop = kj::mv(lock); }

    KJ_EXPECT(!promise2.poll(ws));
    KJ_EXPECT(!promise3.poll(ws));
  }

  KJ_EXPECT(promise2.poll(ws));
  KJ_EXPECT(!promise3.poll(ws));  // we'll cancel this waiter to make sure that works

  KJ_EXPECT(!gate.onBroken().poll(ws));
}

KJ_TEST("InputGate critical section") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  InputGate gate;

  kj::Own<InputGate::CriticalSection> cs;

  {
    auto lock = gate.wait().wait(ws);
    cs = lock.startCriticalSection();
  }

  {
    // Take the first lock.
    auto firstLock = cs->wait().wait(ws);

    // Other locks are blocked.
    auto wait1 = cs->wait();
    auto wait2 = cs->wait();
    KJ_EXPECT(!wait1.poll(ws));
    KJ_EXPECT(!wait2.poll(ws));

    // Drop it.
    { auto drop = kj::mv(firstLock); }

    // Now other locks make progress.
    {
      auto lock = wait1.wait(ws);
      KJ_EXPECT(!wait2.poll(ws));
    }
    wait2.wait(ws);
  }

  // Can't lock the top-level gate while CriticalSection still exists.
  auto outerWait = gate.wait();
  KJ_EXPECT(!outerWait.poll(ws));

  {
    auto lock = cs->wait().wait(ws);
    cs->succeeded();
    KJ_EXPECT(!outerWait.poll(ws));
  }

  outerWait.wait(ws);
}

KJ_TEST("InputGate multiple critical sections start together") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  InputGate gate;

  kj::Own<InputGate::CriticalSection> cs1;
  kj::Own<InputGate::CriticalSection> cs2;

  {
    auto lock = gate.wait().wait(ws);
    cs1 = lock.startCriticalSection();
    cs2 = lock.startCriticalSection();
  }

  // Start cs1.
  cs1->wait().wait(ws);

  // Can't start cs2 yet.
  auto cs2Wait = cs2->wait();
  KJ_EXPECT(!cs2Wait.poll(ws));

  cs1->succeeded();

  cs2Wait.wait(ws);
}

KJ_TEST("InputGate nested critical sections") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  InputGate gate;

  kj::Own<InputGate::CriticalSection> cs1;
  kj::Own<InputGate::CriticalSection> cs2;

  {
    auto lock = gate.wait().wait(ws);
    cs1 = lock.startCriticalSection();
  }

  {
    auto lock = cs1->wait().wait(ws);
    cs2 = lock.startCriticalSection();
  }

  // Start cs2.
  cs2->wait().wait(ws);

  // Can't start new tasks in cs1 until cs2 finishes.
  auto cs1Wait = cs1->wait();
  KJ_EXPECT(!cs1Wait.poll(ws));

  cs2->succeeded();

  cs1Wait.wait(ws);
}

KJ_TEST("InputGate nested critical section outlives parent") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  InputGate gate;

  kj::Own<InputGate::CriticalSection> cs1;
  kj::Own<InputGate::CriticalSection> cs2;

  {
    auto lock = gate.wait().wait(ws);
    cs1 = lock.startCriticalSection();
  }

  {
    auto lock = cs1->wait().wait(ws);
    cs2 = lock.startCriticalSection();
  }

  // Start cs2.
  cs2->wait().wait(ws);

  // Mark cs1 done. (Note that, in a real program, this probably can't happen like this, because a
  // lock would be taken on cs1 before marking it done, and that lock would wait for cs2 to
  // finish. But I want to make sure it works anyway.)
  cs1->succeeded();

  // Can't start new tasks in at root until cs2 finishes.
  auto rootWait = gate.wait();
  KJ_EXPECT(!rootWait.poll(ws));

  cs2->succeeded();

  rootWait.wait(ws);
}

KJ_TEST("InputGate deeply nested critical sections") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  InputGate gate;

  kj::Own<InputGate::CriticalSection> cs1;
  kj::Own<InputGate::CriticalSection> cs2;
  kj::Own<InputGate::CriticalSection> cs3;
  kj::Own<InputGate::CriticalSection> cs4;

  {
    auto lock = gate.wait().wait(ws);
    cs1 = lock.startCriticalSection();
  }

  {
    auto lock = cs1->wait().wait(ws);
    cs2 = lock.startCriticalSection();
  }

  {
    auto lock = cs2->wait().wait(ws);
    cs3 = lock.startCriticalSection();
    cs4 = lock.startCriticalSection();
  }

  // Start cs2
  cs2->wait().wait(ws);

  // Add some waiters to cs2, some of which are waiting to start more nested critical sections
  auto lock = cs2->wait().wait(ws);
  auto waiter1 = cs2->wait();
  auto waiter2 = cs2->wait();

  // Both of these wait on cs2 indirectly, as they are nested under cs2
  auto waiter3 = cs3->wait();
  auto waiter4 = cs4->wait();

  KJ_EXPECT(!waiter1.poll(ws));
  KJ_EXPECT(!waiter2.poll(ws));
  KJ_EXPECT(!waiter3.poll(ws));
  KJ_EXPECT(!waiter4.poll(ws));

  // Mark cs2 as complete with outstanding waiters, and drop our reference to it.
  cs2->succeeded();
  cs2 = nullptr;

  // Our waiters should still be outstanding as we have not released the lock
  KJ_EXPECT(!waiter1.poll(ws));
  KJ_EXPECT(!waiter2.poll(ws));
  KJ_EXPECT(!waiter3.poll(ws));
  KJ_EXPECT(!waiter4.poll(ws));

  // Drop some outstanding waiters
  { auto drop = kj::mv(waiter2); }
  { auto drop = kj::mv(waiter4); }

  // Release the lock on cs2
  { auto drop = kj::mv(lock); }

  // cs3 should have started
  KJ_ASSERT(!waiter1.poll(ws));
  KJ_ASSERT(waiter3.poll(ws));
  auto lock2 = waiter3.wait(ws);

  // Add a waiter on cs3
  auto waiter5 = cs3->wait();
  KJ_ASSERT(!waiter5.poll(ws));

  // Can't start new tasks on the root until both cs1 and cs3 have succeeded, and all outstanding
  // tasks have either been dropped or completed.
  auto waiter6 = gate.wait();
  KJ_ASSERT(!waiter6.poll(ws));

  cs1->succeeded();
  cs3->succeeded();

  // drop waiter5
  { auto drop = kj::mv(waiter5); }

  // Release the lock on cs3
  { auto drop = kj::mv(lock2); }

  // Our root task should be ready now.
  KJ_ASSERT(waiter6.poll(ws));
  waiter6.wait(ws);
}

KJ_TEST("InputGate critical section lock outlives critical section") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  InputGate gate;

  kj::Own<InputGate::CriticalSection> cs;

  {
    auto lock = gate.wait().wait(ws);
    cs = lock.startCriticalSection();
  }

  // Start critical section.
  auto lock = cs->wait().wait(ws);
  KJ_ASSERT(lock.isFor(gate));

  // Mark it done, even though a lock is still outstanding.
  cs->succeeded();

  // Drop our reference.
  cs = nullptr;

  // Lock should have been reparented, so should still work.
  KJ_ASSERT(lock.isFor(gate));

  // Adding a ref and dropping it shouldn't cause trouble.
  lock.addRef();

  // The gate should still be locked
  auto waiter = gate.wait();
  KJ_EXPECT(!waiter.poll(ws));

  // Drop the outstanding lock
  { auto drop = kj::mv(lock); }

  // Our waiter should resolve now
  KJ_ASSERT(waiter.poll(ws));
  KJ_EXPECT(waiter.wait(ws).isFor(gate));
}

KJ_TEST("InputGate broken") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  InputGate gate;

  auto brokenPromise = gate.onBroken();

  kj::Own<InputGate::CriticalSection> cs1;
  kj::Own<InputGate::CriticalSection> cs2;
  kj::Own<InputGate::CriticalSection> cs3;

  {
    auto lock = gate.wait().wait(ws);
    cs1 = lock.startCriticalSection();
    cs3 = lock.startCriticalSection();
  }

  {
    auto lock = cs1->wait().wait(ws);
    cs2 = lock.startCriticalSection();
  }

  // start cs2
  cs2->wait().wait(ws);

  auto cs1Wait = cs1->wait();
  KJ_EXPECT(!cs1Wait.poll(ws));

  auto cs3Wait = cs3->wait();
  KJ_EXPECT(!cs3Wait.poll(ws));

  auto rootWait = gate.wait();
  KJ_EXPECT(!rootWait.poll(ws));

  cs2->failed(KJ_EXCEPTION(FAILED, "foobar"));

  KJ_EXPECT_THROW_MESSAGE("foobar", cs1Wait.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("foobar", cs3Wait.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("foobar", rootWait.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("foobar", cs2->wait().wait(ws));
  KJ_EXPECT_THROW_MESSAGE("foobar", brokenPromise.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("foobar", gate.onBroken().wait(ws));
}

// =======================================================================================

KJ_TEST("OutputGate basics") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  OutputGate gate;

  KJ_EXPECT(gate.wait().poll(ws));

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto blocker1 = gate.lockWhile(kj::mv(paf1.promise));

  auto promise1 = gate.wait();
  auto promise2 = gate.wait();

  auto paf2 = kj::newPromiseAndFulfiller<void>();
  auto blocker2 = gate.lockWhile(kj::mv(paf2.promise));

  auto promise3 = gate.wait();

  KJ_EXPECT(!promise1.poll(ws));
  KJ_EXPECT(!promise2.poll(ws));
  KJ_EXPECT(!promise3.poll(ws));

  KJ_EXPECT(!blocker1.poll(ws));
  paf1.fulfiller->fulfill();
  KJ_EXPECT(blocker1.poll(ws));
  blocker1.wait(ws);

  KJ_EXPECT(promise1.poll(ws));
  promise1.wait(ws);
  KJ_EXPECT(promise2.poll(ws));
  promise2.wait(ws);
  KJ_EXPECT(!promise3.poll(ws));

  KJ_EXPECT(!blocker2.poll(ws));
  paf2.fulfiller->fulfill();
  KJ_EXPECT(blocker2.poll(ws));
  blocker2.wait(ws);

  KJ_EXPECT(promise3.poll(ws));
  promise3.wait(ws);

  KJ_EXPECT(!gate.onBroken().poll(ws));
}

KJ_TEST("OutputGate out-of-order") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  OutputGate gate;

  KJ_EXPECT(gate.wait().poll(ws));

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto blocker1 = gate.lockWhile(kj::mv(paf1.promise));

  auto promise1 = gate.wait();
  auto promise2 = gate.wait();

  auto paf2 = kj::newPromiseAndFulfiller<void>();
  auto blocker2 = gate.lockWhile(kj::mv(paf2.promise));

  auto promise3 = gate.wait();

  KJ_EXPECT(!promise1.poll(ws));
  KJ_EXPECT(!promise2.poll(ws));
  KJ_EXPECT(!promise3.poll(ws));

  // Fulfill second blocker first.
  KJ_EXPECT(!blocker2.poll(ws));
  paf2.fulfiller->fulfill();
  KJ_EXPECT(blocker2.poll(ws));
  blocker2.wait(ws);

  // Everything is still blocked.
  KJ_EXPECT(!promise1.poll(ws));
  KJ_EXPECT(!promise2.poll(ws));
  KJ_EXPECT(!promise3.poll(ws));

  // Fulfill the first one.
  KJ_EXPECT(!blocker1.poll(ws));
  paf1.fulfiller->fulfill();
  KJ_EXPECT(blocker1.poll(ws));
  blocker1.wait(ws);

  // Everything unblocked.
  KJ_EXPECT(promise1.poll(ws));
  promise1.wait(ws);
  KJ_EXPECT(promise2.poll(ws));
  promise2.wait(ws);
  KJ_EXPECT(promise3.poll(ws));
  promise3.wait(ws);

  KJ_EXPECT(!gate.onBroken().poll(ws));
}

KJ_TEST("OutputGate exception") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  OutputGate gate;
  auto onBroken = gate.onBroken();

  KJ_EXPECT(gate.wait().poll(ws));

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto blocker1 = gate.lockWhile(kj::mv(paf1.promise));

  auto promise1 = gate.wait();
  auto promise2 = gate.wait();

  auto paf2 = kj::newPromiseAndFulfiller<void>();
  auto blocker2 = gate.lockWhile(kj::mv(paf2.promise));

  auto promise3 = gate.wait();

  KJ_EXPECT(!promise1.poll(ws));
  KJ_EXPECT(!promise2.poll(ws));
  KJ_EXPECT(!promise3.poll(ws));

  // Let's have the second blocker fail first.
  paf2.fulfiller->reject(KJ_EXCEPTION(FAILED, "foo"));
  KJ_EXPECT(blocker2.poll(ws));
  KJ_EXPECT_THROW_MESSAGE("foo", blocker2.wait(ws));

  // Promises are all still waiting. TECHNICALLY, it would be OK to fail-fast the third promise,
  // but for now we don't.
  KJ_EXPECT(!promise1.poll(ws));
  KJ_EXPECT(!promise2.poll(ws));
  KJ_EXPECT(!promise3.poll(ws));

  // We are marked broken at this point, though.
  KJ_ASSERT(onBroken.poll(ws));
  KJ_EXPECT_THROW_MESSAGE("foo", onBroken.wait(ws));

  // Fulfill the first blocker (normally, not with an exception).
  KJ_EXPECT(!blocker1.poll(ws));
  paf1.fulfiller->fulfill();
  KJ_EXPECT(blocker1.poll(ws));
  blocker1.wait(ws);

  // Everything unblocked, but only the third promise fails.
  KJ_EXPECT(promise1.poll(ws));
  promise1.wait(ws);
  KJ_EXPECT(promise2.poll(ws));
  promise2.wait(ws);
  KJ_EXPECT(promise3.poll(ws));
  KJ_EXPECT_THROW_MESSAGE("foo", promise3.wait(ws));

  // Still broken.
  onBroken = gate.onBroken();
  KJ_ASSERT(onBroken.poll(ws));
  KJ_EXPECT_THROW_MESSAGE("foo", onBroken.wait(ws));
}

KJ_TEST("OutputGate canceled") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  OutputGate gate;
  auto onBroken = gate.onBroken();

  KJ_EXPECT(gate.wait().poll(ws));

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto blocker1 = gate.lockWhile(kj::mv(paf1.promise));

  auto promise1 = gate.wait();
  auto promise2 = gate.wait();

  auto blocker2 = gate.lockWhile(kj::Promise<void>(kj::NEVER_DONE));

  auto promise3 = gate.wait();

  KJ_EXPECT(!promise1.poll(ws));
  KJ_EXPECT(!promise2.poll(ws));
  KJ_EXPECT(!promise3.poll(ws));

  // Let's cancel the second blocker first.
  blocker2 = nullptr;

  // Promises are all still waiting. TECHNICALLY, it would be OK to fail-fast the third promise,
  // but for now we don't.
  KJ_EXPECT(!promise1.poll(ws));
  KJ_EXPECT(!promise2.poll(ws));
  KJ_EXPECT(!promise3.poll(ws));

  // We are marked broken at this point, though.
  KJ_ASSERT(onBroken.poll(ws));
  KJ_EXPECT_THROW_MESSAGE("output lock was canceled before completion", onBroken.wait(ws));

  // Fulfill the first blocker (normally, not with an exception).
  KJ_EXPECT(!blocker1.poll(ws));
  paf1.fulfiller->fulfill();
  KJ_EXPECT(blocker1.poll(ws));
  blocker1.wait(ws);

  // Everything unblocked, but only the third promise fails.
  KJ_EXPECT(promise1.poll(ws));
  promise1.wait(ws);
  KJ_EXPECT(promise2.poll(ws));
  promise2.wait(ws);
  KJ_EXPECT(promise3.poll(ws));
  KJ_EXPECT_THROW_MESSAGE("output lock was canceled before completion", promise3.wait(ws));

  // Still broken.
  onBroken = gate.onBroken();
  KJ_ASSERT(onBroken.poll(ws));
  KJ_EXPECT_THROW_MESSAGE("output lock was canceled before completion", onBroken.wait(ws));
}

}  // namespace
}  // namespace workerd
