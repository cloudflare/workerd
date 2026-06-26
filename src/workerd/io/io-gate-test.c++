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

  kj::Promise<InputGate::Lock> promise1 = gate.wait(nullptr);
  kj::Promise<InputGate::Lock> promise2 = gate.wait(nullptr);
  kj::Promise<InputGate::Lock> promise3 = gate.wait(nullptr);

  KJ_ASSERT(promise1.poll(ws));
  KJ_EXPECT(!promise2.poll(ws));
  KJ_EXPECT(!promise3.poll(ws));

  {
    auto lock = promise1.wait(ws);

    KJ_EXPECT(!promise2.poll(ws));
    KJ_EXPECT(!promise3.poll(ws));

    auto lock2 = lock.addRef(nullptr);
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
    auto lock = gate.wait(nullptr).wait(ws);
    cs = lock.startCriticalSection();
  }

  {
    // Take the first lock.
    auto firstLock = cs->wait(nullptr).wait(ws);

    // Other locks are blocked.
    auto wait1 = cs->wait(nullptr);
    auto wait2 = cs->wait(nullptr);
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
  auto outerWait = gate.wait(nullptr);
  KJ_EXPECT(!outerWait.poll(ws));

  {
    auto lock = cs->wait(nullptr).wait(ws);
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
    auto lock = gate.wait(nullptr).wait(ws);
    cs1 = lock.startCriticalSection();
    cs2 = lock.startCriticalSection();
  }

  // Start cs1.
  cs1->wait(nullptr).wait(ws);

  // Can't start cs2 yet.
  auto cs2Wait = cs2->wait(nullptr);
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
    auto lock = gate.wait(nullptr).wait(ws);
    cs1 = lock.startCriticalSection();
  }

  {
    auto lock = cs1->wait(nullptr).wait(ws);
    cs2 = lock.startCriticalSection();
  }

  // Start cs2.
  cs2->wait(nullptr).wait(ws);

  // Can't start new tasks in cs1 until cs2 finishes.
  auto cs1Wait = cs1->wait(nullptr);
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
    auto lock = gate.wait(nullptr).wait(ws);
    cs1 = lock.startCriticalSection();
  }

  {
    auto lock = cs1->wait(nullptr).wait(ws);
    cs2 = lock.startCriticalSection();
  }

  // Start cs2.
  cs2->wait(nullptr).wait(ws);

  // Mark cs1 done. (Note that, in a real program, this probably can't happen like this, because a
  // lock would be taken on cs1 before marking it done, and that lock would wait for cs2 to
  // finish. But I want to make sure it works anyway.)
  cs1->succeeded();

  // Can't start new tasks in at root until cs2 finishes.
  auto rootWait = gate.wait(nullptr);
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
    auto lock = gate.wait(nullptr).wait(ws);
    cs1 = lock.startCriticalSection();
  }

  {
    auto lock = cs1->wait(nullptr).wait(ws);
    cs2 = lock.startCriticalSection();
  }

  {
    auto lock = cs2->wait(nullptr).wait(ws);
    cs3 = lock.startCriticalSection();
    cs4 = lock.startCriticalSection();
  }

  // Start cs2
  cs2->wait(nullptr).wait(ws);

  // Add some waiters to cs2, some of which are waiting to start more nested critical sections
  auto lock = cs2->wait(nullptr).wait(ws);
  auto waiter1 = cs2->wait(nullptr);
  auto waiter2 = cs2->wait(nullptr);

  // Both of these wait on cs2 indirectly, as they are nested under cs2
  auto waiter3 = cs3->wait(nullptr);
  auto waiter4 = cs4->wait(nullptr);

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
  auto waiter5 = cs3->wait(nullptr);
  KJ_ASSERT(!waiter5.poll(ws));

  // Can't start new tasks on the root until both cs1 and cs3 have succeeded, and all outstanding
  // tasks have either been dropped or completed.
  auto waiter6 = gate.wait(nullptr);
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
    auto lock = gate.wait(nullptr).wait(ws);
    cs = lock.startCriticalSection();
  }

  // Start critical section.
  auto lock = cs->wait(nullptr).wait(ws);
  KJ_ASSERT(lock.isFor(gate));

  // Mark it done, even though a lock is still outstanding.
  cs->succeeded();

  // Drop our reference.
  cs = nullptr;

  // Lock should have been reparented, so should still work.
  KJ_ASSERT(lock.isFor(gate));

  // Adding a ref and dropping it shouldn't cause trouble.
  lock.addRef(nullptr);

  // The gate should still be locked
  auto waiter = gate.wait(nullptr);
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
    auto lock = gate.wait(nullptr).wait(ws);
    cs1 = lock.startCriticalSection();
    cs3 = lock.startCriticalSection();
  }

  {
    auto lock = cs1->wait(nullptr).wait(ws);
    cs2 = lock.startCriticalSection();
  }

  // start cs2
  cs2->wait(nullptr).wait(ws);

  auto cs1Wait = cs1->wait(nullptr);
  KJ_EXPECT(!cs1Wait.poll(ws));

  auto cs3Wait = cs3->wait(nullptr);
  KJ_EXPECT(!cs3Wait.poll(ws));

  auto rootWait = gate.wait(nullptr);
  KJ_EXPECT(!rootWait.poll(ws));

  cs2->failed(KJ_EXCEPTION(FAILED, "foobar"));

  KJ_EXPECT_THROW_MESSAGE("foobar", cs1Wait.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("foobar", cs3Wait.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("foobar", rootWait.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("foobar", cs2->wait(nullptr).wait(ws));
  KJ_EXPECT_THROW_MESSAGE("foobar", brokenPromise.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("foobar", gate.onBroken().wait(ws));
}

KJ_TEST("InputGate deeply nested critical sections tear down without overflowing the stack") {
  // Regression test: a Worker can nest blockConcurrencyWhile() calls (and thus
  // InputGate::CriticalSections) arbitrarily deeply. Each CriticalSection owns its parent, so a
  // naive recursive teardown -- either via ~CriticalSection or via CriticalSection::failed() --
  // recurses once per nesting level and exhausts the native stack, crashing the process. Both
  // paths must be iterative.
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  InputGate gate;

  // Deep enough to reliably overflow a default (~8MB) stack if teardown were recursive.
  constexpr size_t DEPTH = 100'000;

  // Build a chain of nested, RUNNING critical sections.
  auto outerLock = gate.wait(nullptr).wait(ws);
  auto cs = outerLock.startCriticalSection();
  { auto drop = kj::mv(outerLock); }
  kj::Maybe<InputGate::Lock> lock = cs->wait(nullptr).wait(ws);

  for (size_t i = 0; i < DEPTH; i++) {
    auto child = KJ_ASSERT_NONNULL(lock).startCriticalSection();
    // Release the parent's lock so the child's initial wait() can proceed synchronously.
    lock = kj::none;
    lock = child->wait(nullptr).wait(ws);
    // Drop our reference to the parent; it stays alive via the child's owned parent link.
    cs = kj::mv(child);
  }

  // Drop the innermost lock and reference. This destroys the innermost critical section, which
  // (being RUNNING) calls failed() -- exercising the iterative failure propagation -- and then
  // tears down the owned parent chain -- exercising the iterative destruction. Either of these
  // would overflow the stack if implemented recursively.
  lock = kj::none;
  cs = nullptr;

  // Reaching here without crashing means teardown was iterative. The gate should now be broken,
  // since the innermost RUNNING critical section failed as it was destroyed.
  KJ_EXPECT_THROW_MESSAGE("deadlock", gate.wait(nullptr).wait(ws));
}

KJ_TEST("InputGate forwarding wait through deeply reparented critical sections") {
  // Regression test: when wait() is called on a REPARENTED critical section, it forwards up the
  // chain via `co_await c->wait()`. If every ancestor is also REPARENTED, this builds a chain of
  // nested coroutines, one per nesting level. Destroying that chain (e.g. when the straggler task
  // is canceled) must not recurse once per level, or it overflows the stack.
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  InputGate gate;

  constexpr size_t DEPTH = 50'000;

  // Build a chain of nested, RUNNING critical sections.
  kj::Vector<kj::Own<InputGate::CriticalSection>> css;
  {
    auto outerLock = gate.wait(nullptr).wait(ws);
    css.add(outerLock.startCriticalSection());
  }
  kj::Maybe<InputGate::Lock> lock = css[0]->wait(nullptr).wait(ws);
  for (size_t i = 1; i < DEPTH; i++) {
    auto child = KJ_ASSERT_NONNULL(lock).startCriticalSection();
    lock = kj::none;
    auto childLock = child->wait(nullptr).wait(ws);
    css.add(kj::mv(child));
    lock = kj::mv(childLock);
  }
  lock = kj::none;

  // Mark every section succeeded, inner-to-outer, so they all become REPARENTED. We keep only the
  // outermost returned lock (a lock on the root gate) to keep the gate locked; the inner returned
  // locks are dropped immediately, which is O(1) because their target section is still RUNNING (it
  // hasn't been reparented yet). Inner-to-outer also keeps each succeeded() O(1).
  kj::Maybe<InputGate::Lock> gateLock;
  for (size_t i = DEPTH; i > 0; i--) {
    auto parentLock = css[i - 1]->succeeded();
    if (i == 1) {
      gateLock = kj::mv(parentLock);
    }
  }

  // A straggler task takes a lock via the innermost (REPARENTED) section. This forwards up through
  // every REPARENTED ancestor; if forwarding were recursive it would build a chain of DEPTH nested
  // coroutines. It suspends because the root gate is locked.
  auto forwardWait = css.back()->wait(nullptr);
  KJ_EXPECT(!forwardWait.poll(ws));

  // Cancelling it destroys the coroutine(s). If forwarding built (and thus destroyed) one coroutine
  // per level, this -- or the setup above -- would overflow the stack.
  { auto drop = kj::mv(forwardWait); }

  gateLock = kj::none;
  css.clear();
}

KJ_TEST("InputGate teardown does not corrupt critical sections that outlive their child") {
  // Regression test: ~CriticalSection tears down its owned parent chain iteratively, walking up
  // the chain and clearing each link. But an ancestor may still be referenced elsewhere -- in
  // particular, while a chain of nested blockConcurrencyWhile() critical sections unwinds, each
  // ancestor is still owned by its own in-flight task. The iterative teardown must stop as soon as
  // it reaches an ancestor that has other references, rather than severing that (still-live)
  // ancestor's parent link. Otherwise the survivor is left with `parent` set to kj::None, and a
  // later parentAsInputGate() (e.g. from succeeded() or wait()) trips KJ_UNREACHABLE.
  //
  // Note this is distinct from the "tear down without overflowing the stack" test above, which
  // exercises a chain that is *exclusively* owned via the parent links (no surviving references),
  // and so never hits this case.
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  InputGate gate;

  constexpr size_t DEPTH = 1000;

  // Build a chain of nested, RUNNING critical sections, keeping a reference to every level (as an
  // in-flight task would), so that every ancestor is shared.
  kj::Vector<kj::Own<InputGate::CriticalSection>> css;
  {
    auto outerLock = gate.wait(nullptr).wait(ws);
    css.add(outerLock.startCriticalSection());
  }
  kj::Maybe<InputGate::Lock> lock = css[0]->wait(nullptr).wait(ws);
  for (size_t i = 1; i < DEPTH; i++) {
    auto child = KJ_ASSERT_NONNULL(lock).startCriticalSection();
    lock = kj::none;
    auto childLock = child->wait(nullptr).wait(ws);
    css.add(kj::mv(child));
    lock = kj::mv(childLock);
  }
  lock = kj::none;

  // The innermost section succeeds (its task resolved) and is then dropped (its task completed).
  // Dropping it runs ~CriticalSection, which walks the owned parent chain. Because every ancestor
  // is still held by `css`, the walk must stop at the first ancestor without severing parent links.
  { auto drop = css.back()->succeeded(); }
  css.removeLast();

  // Unwind the rest of the chain, inner-to-outer, exactly as the surviving tasks' success
  // continuations would. succeeded() calls parentAsInputGate() to walk to the parent gate; before
  // the fix the deepest survivor's `parent` had been set to kj::None, tripping KJ_UNREACHABLE here.
  kj::Maybe<InputGate::Lock> gateLock;
  for (size_t i = css.size(); i > 0; i--) {
    auto parentLock = css[i - 1]->succeeded();
    if (i == 1) {
      gateLock = kj::mv(parentLock);
    }
  }

  gateLock = kj::none;
  css.clear();
}

KJ_TEST("InputGate CriticalSection tracks nesting depth") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  InputGate gate;

  kj::Own<InputGate::CriticalSection> cs1;
  {
    auto lock = gate.wait(nullptr).wait(ws);
    cs1 = lock.startCriticalSection();
  }
  KJ_EXPECT(cs1->getDepth() == 1);

  kj::Own<InputGate::CriticalSection> cs2;
  {
    auto lock = cs1->wait(nullptr).wait(ws);
    cs2 = lock.startCriticalSection();
  }
  KJ_EXPECT(cs2->getDepth() == 2);

  kj::Own<InputGate::CriticalSection> cs3;
  {
    auto lock = cs2->wait(nullptr).wait(ws);
    cs3 = lock.startCriticalSection();
  }
  KJ_EXPECT(cs3->getDepth() == 3);
}

// =======================================================================================

KJ_TEST("OutputGate basics") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  OutputGate gate;

  KJ_EXPECT(gate.wait(nullptr).poll(ws));

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto blocker1 = gate.lockWhile(kj::mv(paf1.promise), nullptr);

  auto promise1 = gate.wait(nullptr);
  auto promise2 = gate.wait(nullptr);

  auto paf2 = kj::newPromiseAndFulfiller<void>();
  auto blocker2 = gate.lockWhile(kj::mv(paf2.promise), nullptr);

  auto promise3 = gate.wait(nullptr);

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

  KJ_EXPECT(gate.wait(nullptr).poll(ws));

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto blocker1 = gate.lockWhile(kj::mv(paf1.promise), nullptr);

  auto promise1 = gate.wait(nullptr);
  auto promise2 = gate.wait(nullptr);

  auto paf2 = kj::newPromiseAndFulfiller<void>();
  auto blocker2 = gate.lockWhile(kj::mv(paf2.promise), nullptr);

  auto promise3 = gate.wait(nullptr);

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

  KJ_EXPECT(gate.wait(nullptr).poll(ws));

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto blocker1 = gate.lockWhile(kj::mv(paf1.promise), nullptr);

  auto promise1 = gate.wait(nullptr);
  auto promise2 = gate.wait(nullptr);

  auto paf2 = kj::newPromiseAndFulfiller<void>();
  auto blocker2 = gate.lockWhile(kj::mv(paf2.promise), nullptr);

  auto promise3 = gate.wait(nullptr);

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

  KJ_EXPECT(gate.wait(nullptr).poll(ws));

  auto paf1 = kj::newPromiseAndFulfiller<void>();
  auto blocker1 = gate.lockWhile(kj::mv(paf1.promise), nullptr);

  auto promise1 = gate.wait(nullptr);
  auto promise2 = gate.wait(nullptr);

  auto blocker2 = gate.lockWhile(kj::Promise<void>(kj::NEVER_DONE), nullptr);

  auto promise3 = gate.wait(nullptr);

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
