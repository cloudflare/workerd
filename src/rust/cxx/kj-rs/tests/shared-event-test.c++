// Regression test: one shared kj Event as the onReady target of MANY concurrent pending nodes.
//
// The bridge registers the SAME FuturePollEvent as the onReady target of every kj::Promise a Rust
// future is `.await`ing (many nodes -> one event, kj's native mechanism). This guards the
// kj Event/onReady properties that relies on:
//   (a) idempotent arming: fulfilling several nodes in the SAME turn arms the one event once
//       (Event::armDepthFirst's `if (prev == nullptr)` guard, async.c++:2201),
//   (b) re-poll: fire() re-polls, ready nodes are consumed, not-yet-ready nodes stay registered
//       and re-arm the event when THEY later resolve,
//   (c) arm-while-firing: fulfilling a node DURING the shared event's own fire() safely re-arms
//       it for the next turn (no "Promise callback destroyed itself" abort async.c++:2188, no
//       lost wake) -- because turn() unlinks the event before fire(), so prev==nullptr during
//       fire and armDepthFirst re-inserts it.
//
// Two test groups:
//   GROUP A -- "pure kj primitive": N PromiseNodes each call node->onReady(&sharedEvent) DIRECTLY.
//              Proves kj's OnReadyEvent::arm() -> Event::armDepthFirst() coalescing on one shared
//              target. Readiness bookkeeping is test-driven (kj exposes no per-node readiness
//              query): the point of Group A is the wake/arm coalescing path.
//   GROUP B -- "faithful future model": each leaf is a trivial ~6-line per-leaf arm Event whose
//              fire() sets ready=true and arms ONE shared re-poll event, and readiness here is
//              genuinely kj-detected.

#include <kj/async.h>
#include <kj/debug.h>
#include <kj/function.h>
#include <kj/test.h>
#include <kj/vector.h>

namespace kj_rs {
namespace {

using kj::uint;
using kj::_::Event;
using kj::_::ExceptionOr;
using kj::_::OwnPromiseNode;
using kj::_::PromiseNode;
using kj::_::Void;

// A stable slot holding a fulfiller/node pair so we can call setSelfPointer() and later get().
struct Slot {
  kj::Own<kj::PromiseFulfiller<void>> fulfiller;
  OwnPromiseNode node;
  bool fulfilled = false;  // test-side bookkeeping (Group A)
  bool consumed = false;

  static kj::Own<Slot> make() {
    auto paf = kj::newPromiseAndFulfiller<void>();
    auto self = kj::heap<Slot>();
    self->fulfiller = kj::mv(paf.fulfiller);
    self->node = PromiseNode::from(kj::mv(paf.promise));
    self->node->setSelfPointer(&self->node);
    return self;
  }

  void consume() {
    ExceptionOr<Void> output;
    node->get(output);
    KJ_ASSERT(output.exception == kj::none);
    consumed = true;
  }
};

// =======================================================================================
// GROUP A -- N nodes -> ONE shared event as direct onReady target.

class SharedRepollEvent final: public Event {
 public:
  SharedRepollEvent(kj::ArrayPtr<kj::Own<Slot>> slots, kj::SourceLocation location = {})
      : Event(location),
        slots(slots) {}

  uint fireCount = 0;
  uint consumedCount = 0;

  // If set, invoked once during the NEXT fire() -- models a sub-future resolving mid-poll.
  kj::Function<void()>* armWhileFiringHook = nullptr;

  void traceEvent(kj::_::TraceBuilder&) override {}

 private:
  kj::ArrayPtr<kj::Own<Slot>> slots;

  void fire() override {
    ++fireCount;

    // Model "the rust future re-polls all its sub-futures": consume every ready (fulfilled),
    // not-yet-consumed node; leave the rest registered.
    for (auto& slot: slots) {
      if (slot->fulfilled && !slot->consumed) {
        slot->consume();
        ++consumedCount;
      }
    }

    // ARM-WHILE-FIRING: fulfill another node during our own fire(). Its onReady points at us;
    // arming us here must be safe (we were unlinked before fire, so prev==nullptr -> re-inserts).
    if (armWhileFiringHook != nullptr) {
      auto* hook = armWhileFiringHook;
      armWhileFiringHook = nullptr;
      (*hook)();
    }
    // NOTE: we intentionally do NOT self-destruct; a FutureEvent lives until its future resolves.
  }
};

KJ_TEST("SharedEvent(A): fulfilling several nodes in one turn arms the shared event idempotently") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto slots = kj::heapArray<kj::Own<Slot>>(4);
  for (auto& s: slots) s = Slot::make();

  SharedRepollEvent shared(slots);
  for (auto& s: slots) s->node->onReady(&shared);

  // Fulfill 3 of the 4 in the SAME turn (no loop run in between).
  for (uint i: {0u, 1u, 2u}) {
    slots[i]->fulfiller->fulfill();
    slots[i]->fulfilled = true;
  }

  // Exactly one fire should happen: the 3 same-turn arms coalesced into a single armed event.
  // (If they had NOT coalesced, fireCount would be 3.)
  waitScope.poll();
  KJ_EXPECT(shared.fireCount == 1);
  KJ_EXPECT(shared.consumedCount == 3);

  // (b) The 4th node was never fulfilled: it stays registered on the shared event. Fulfilling it
  //     now must re-arm the (idle) shared event and fire again.
  slots[3]->fulfiller->fulfill();
  slots[3]->fulfilled = true;
  waitScope.poll();
  KJ_EXPECT(shared.fireCount == 2);
  KJ_EXPECT(shared.consumedCount == 4);
}

KJ_TEST("SharedEvent(A): arm-while-firing re-arms safely for the next turn (no lost wake)") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto slots = kj::heapArray<kj::Own<Slot>>(3);
  for (auto& s: slots) s = Slot::make();

  SharedRepollEvent shared(slots);
  for (auto& s: slots) s->node->onReady(&shared);

  // During the shared event's FIRST fire(), fulfill slot[2]. Its onReady == &shared, so this arms
  // `shared` while `shared` is mid-fire. Must not abort; must re-arm for the next turn.
  kj::Function<void()> hook = [&]() {
    slots[2]->fulfiller->fulfill();
    slots[2]->fulfilled = true;
  };
  shared.armWhileFiringHook = &hook;

  // Kick off: fulfill slots 0 and 1 in this turn.
  for (uint i: {0u, 1u}) {
    slots[i]->fulfiller->fulfill();
    slots[i]->fulfilled = true;
  }

  // A single poll() drains: fire #1 consumes 0,1 and (via the hook) fulfills slot[2], which arms
  // `shared` mid-fire; fire #2 (the re-arm) consumes slot[2]. Proves no abort + no lost wake.
  waitScope.poll();
  KJ_EXPECT(shared.fireCount == 2);
  KJ_EXPECT(shared.consumedCount == 3);
}

// =======================================================================================
// GROUP B -- faithful future model: trivial per-leaf arm events + ONE shared re-poll event.

class FutureEvent;

// A leaf `.await` of a kj::Promise. ~6 lines of real logic: on its promise's readiness, record
// ready and arm the shared FutureEvent.
class LeafAwaiter final: public Event {
 public:
  LeafAwaiter(OwnPromiseNode nodeParam, FutureEvent& futureEvent, kj::SourceLocation location = {});
  ~LeafAwaiter() noexcept(false) {
    node = nullptr;
  }

  bool ready = false;
  bool consumed = false;

  void consume() {
    KJ_ASSERT(ready && !consumed);
    ExceptionOr<Void> output;
    node->get(output);
    KJ_ASSERT(output.exception == kj::none);
    consumed = true;
  }

  void traceEvent(kj::_::TraceBuilder&) override {}

 private:
  OwnPromiseNode node;
  FutureEvent& futureEvent;
  void fire() override;  // defined after FutureEvent
};

// The ONE event that "is the future": its fire() re-polls the whole future (== consume any ready
// leaves, leave the rest). Every leaf arms THIS single event.
class FutureEvent final: public Event {
 public:
  FutureEvent(kj::SourceLocation location = {}): Event(location) {}

  uint fireCount = 0;

  // If set, invoked once during the NEXT fire() -- models a sub-future resolving mid-poll.
  kj::Function<void()>* armWhileFiringHook = nullptr;

  void addLeaf(kj::Own<LeafAwaiter> leaf) {
    leaves.add(kj::mv(leaf));
  }

  bool allConsumed() const {
    for (auto& l: leaves)
      if (!l->consumed) return false;
    return true;
  }
  uint consumedCount() const {
    uint n = 0;
    for (auto& l: leaves)
      if (l->consumed) ++n;
    return n;
  }

  void traceEvent(kj::_::TraceBuilder&) override {}

 private:
  kj::Vector<kj::Own<LeafAwaiter>> leaves;

  void fire() override {
    ++fireCount;
    for (auto& l: leaves) {
      if (l->ready && !l->consumed) l->consume();
    }
    if (armWhileFiringHook != nullptr) {
      auto* hook = armWhileFiringHook;
      armWhileFiringHook = nullptr;
      (*hook)();  // fulfills another leaf -> its LeafAwaiter arms `this` mid-fire
    }
  }
};

LeafAwaiter::LeafAwaiter(OwnPromiseNode nodeParam, FutureEvent& fe, kj::SourceLocation location)
    : Event(location),
      node(kj::mv(nodeParam)),
      futureEvent(fe) {
  node->setSelfPointer(&node);
  node->onReady(this);
}

void LeafAwaiter::fire() {
  ready = true;
  futureEvent.armDepthFirst();  // arm the ONE shared future event (idempotent across leaves)
}

KJ_TEST(
    "SharedEvent(B): many leaves arm one FutureEvent; ready consumed, pending stay registered") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  FutureEvent future;
  kj::Vector<kj::Own<kj::PromiseFulfiller<void>>> fulfillers;

  for (uint i = 0; i < 4; ++i) {
    auto paf = kj::newPromiseAndFulfiller<void>();
    fulfillers.add(kj::mv(paf.fulfiller));
    future.addLeaf(kj::heap<LeafAwaiter>(PromiseNode::from(kj::mv(paf.promise)), future));
  }

  // Fulfill 3 leaves in one turn -> 3 leaf Events fire (each arms `future`), then `future` fires
  // ONCE (coalesced). turn() runs the leaf events + the single future event.
  for (uint i: {0u, 1u, 2u}) fulfillers[i]->fulfill();

  loop.run(64);
  KJ_EXPECT(future.fireCount >= 1);
  KJ_EXPECT(future.consumedCount() == 3);
  KJ_EXPECT(!future.allConsumed());

  uint fireCountAfter3 = future.fireCount;

  // 4th leaf stays registered; fulfilling it re-arms the future event.
  fulfillers[3]->fulfill();
  loop.run(64);
  KJ_EXPECT(future.fireCount > fireCountAfter3);
  KJ_EXPECT(future.allConsumed());
}

KJ_TEST(
    "SharedEvent(B): leaf resolving during the future's fire re-arms the future (no lost wake)") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  FutureEvent future;
  kj::Vector<kj::Own<kj::PromiseFulfiller<void>>> fulfillers;

  for (uint i = 0; i < 3; ++i) {
    auto paf = kj::newPromiseAndFulfiller<void>();
    fulfillers.add(kj::mv(paf.fulfiller));
    future.addLeaf(kj::heap<LeafAwaiter>(PromiseNode::from(kj::mv(paf.promise)), future));
  }

  // During the future's FIRST fire(), fulfill leaf 2. Its LeafAwaiter event will arm `future`
  // while `future` is still mid-fire -> must safely re-arm for the next turn.
  kj::Function<void()> hook = [&]() { fulfillers[2]->fulfill(); };
  future.armWhileFiringHook = &hook;

  // Kick off with leaves 0 and 1.
  fulfillers[0]->fulfill();
  fulfillers[1]->fulfill();

  loop.run(64);
  KJ_EXPECT(future.fireCount >= 2);  // at least: fire consuming 0/1, then the re-armed fire for 2
  KJ_EXPECT(future.allConsumed());   // leaf 2 (fulfilled during a fire) was not lost
}

}  // namespace
}  // namespace kj_rs
