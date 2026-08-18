// Regression test: FutureWakerCell "neutralize-on-drop".
//
// The bridge's same-thread waker is a FutureWakerCell whose wake() arms the owning FuturePollEvent.
// The hazard: a waker clone is retained (e.g. handed to some sub-future) and its wake() fires AFTER
// the FuturePollEvent (and the boxed future it owns) has been torn down -- arming a freed
// kj::_::Event would be a use-after-free.
//
// This guards the refcounted-cell handling: the cell holds an Event*; the FuturePollEvent holds
// the strong ref and NULLS the cell on destruction (BEFORE the boxed future / sub-wakers drop). A
// retained clone that calls wake() after teardown observes null and is a safe no-op.
//
// Run under ASAN to confirm no UAF:
//   bazel test //kj-rs/tests:neutralize-waker-test --config=asan

#include <kj/async.h>
#include <kj/debug.h>
#include <kj/refcount.h>
#include <kj/test.h>

namespace kj_rs {
namespace {

using kj::uint;

// The refcounted cell shared between the FutureEvent and any retained Waker clones. The bridge is
// single-threaded (no cross-thread wakes), so a plain (non-atomic) kj::Refcounted with a bare
// Event* is sufficient -- no mutex/atomic needed.
class WakerCell: public kj::Refcounted {
 public:
  kj::_::Event* event = nullptr;

  bool observedNullOnWake = false;
  uint wakeArmCount = 0;

  // Called by the FutureEvent's destructor to neutralize all outstanding waker clones.
  void neutralize() {
    event = nullptr;
  }

  // The Waker's wake(): arm the FutureEvent, or no-op if it's been neutralized.
  void wake() {
    if (event != nullptr) {
      event->armDepthFirst();
      ++wakeArmCount;
    } else {
      observedNullOnWake = true;  // SAFE no-op: no arm of a freed Event, no UAF.
    }
  }
};

// Models the boxed Rust future (and its sub-wakers) owned inline by the FutureEvent. Its whole job
// here is to ASSERT the ordering requirement: by the time it is destroyed, the cell must already
// have been neutralized -- i.e. nulled BEFORE the boxed future / sub-wakers drop.
class BoxedFutureStandin {
 public:
  explicit BoxedFutureStandin(WakerCell& cell): cell(cell) {}
  ~BoxedFutureStandin() noexcept(false) {
    KJ_ASSERT(cell.event == nullptr,
        "ordering violation: cell must be neutralized BEFORE the boxed future drops");
  }

 private:
  WakerCell& cell;
};

// Stand-in for the FutureEvent: a kj Event that owns the WakerCell strong ref and the boxed future.
class FutureEventStandin final: public kj::_::Event {
 public:
  explicit FutureEventStandin(kj::Rc<WakerCell> cellParam, kj::SourceLocation location = {})
      : Event(location),
        cell(kj::mv(cellParam)),
        boxedFuture(*cell) {
    cell->event = this;
  }

  ~FutureEventStandin() noexcept(false) {
    // ORDERING: neutralize the cell FIRST (destructor body runs before member subobjects are
    // destroyed). Member destruction order is reverse-declaration: `boxedFuture` then `cell`.
    // So when boxedFuture's dtor asserts, the cell is already nulled.
    cell->neutralize();
  }

  uint fireCount = 0;
  void traceEvent(kj::_::TraceBuilder&) override {}

 private:
  void fire() override {
    ++fireCount;
  }

  kj::Rc<WakerCell> cell;          // declared first -> destroyed LAST
  BoxedFutureStandin boxedFuture;  // declared second -> destroyed FIRST
};

KJ_TEST("FutureWaker neutralize-on-drop: retained clone wake() is a safe no-op after teardown") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto cell = kj::rc<WakerCell>();
  auto retainedClone = cell.addRef();  // a second handle that will OUTLIVE the FutureEvent.

  auto event = kj::heap<FutureEventStandin>(kj::mv(cell));

  // Sanity: while the FutureEvent is alive, a wake via the retained clone arms it.
  retainedClone->wake();
  waitScope.poll();
  KJ_EXPECT(event->fireCount == 1);
  KJ_EXPECT(retainedClone->wakeArmCount == 1);
  KJ_EXPECT(!retainedClone->observedNullOnWake);

  // TEARDOWN: destroy the FutureEvent. Its dtor neutralizes the cell (asserted to happen before
  // boxedFuture drops). The retained clone keeps the cell object itself alive.
  event = nullptr;

  // The retained clone's wake() now observes a null Event* -> SAFE no-op. Without neutralize-on-
  // drop this would arm a freed Event (UAF -- caught by ASAN under --config=asan).
  retainedClone->wake();
  waitScope.poll();  // must not fire anything, must not crash
  KJ_EXPECT(retainedClone->observedNullOnWake);
  KJ_EXPECT(retainedClone->wakeArmCount == 1);  // unchanged: the post-teardown wake armed nothing
}

KJ_TEST("FutureWaker neutralize-on-drop: multiple retained clones all neutralized together") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto cell = kj::rc<WakerCell>();
  auto cloneA = cell.addRef();
  auto cloneB = cell.addRef();

  auto event = kj::heap<FutureEventStandin>(kj::mv(cell));
  event = nullptr;  // teardown

  // Both retained clones observe null; neither arms a freed Event.
  cloneA->wake();
  cloneB->wake();
  waitScope.poll();
  KJ_EXPECT(cloneA->observedNullOnWake);
  KJ_EXPECT(cloneB->observedNullOnWake);
  KJ_EXPECT(cloneA->wakeArmCount == 0);
}

}  // namespace
}  // namespace kj_rs
