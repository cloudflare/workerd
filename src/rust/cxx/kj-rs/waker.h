#pragma once

#include <kj/async.h>
#include <kj/debug.h>
#include <kj/mutex.h>
#include <kj/refcount.h>
#include <kj/vector.h>

#include <atomic>

namespace kj_rs {

class FuturePollEvent;
class FutureWakerCell;

// =======================================================================================
// CrossThreadWakeSink

// The one cross-thread doorway into a KJ event loop for Rust wakers: one per loop (a
// kj::EventLoopLocal, created on first use and destroyed with the loop), shared by every
// FutureWakerCell created on that loop.
//
// A waker woken from a foreign thread cannot arm its event directly (KJ events are single-loop),
// so it hands its cell to the loop's sink: enqueue() queues a strong reference and fulfills the
// sink's one kj::CrossThreadPromiseFulfiller, whose promise side is a drain coroutine running on
// the owning loop. The drain wakes each queued cell from the owning thread -- now the ordinary
// same-thread path: arm the event, or no-op if it was neutralized -- and re-arms a fresh
// fulfiller. Repeated foreign wakes before the drain runs coalesce, as the Waker contract permits.
//
// One fulfiller per loop rather than one per awaited future: a cross-thread fulfiller is a heap
// allocation plus an executor reference, and paying that on every `co_await` of a Rust future
// (the overwhelming majority of which are never woken cross-thread) measured as the single
// largest cost of the bridge. Cells hold the sink by kj::Arc, so a cell retained by Rust past the
// loop's lifetime still has a valid sink to talk to; the sink is closed when its loop is
// destroyed (enqueue then drops the reference and does nothing), which is the cross-thread
// flavor of neutralize-on-drop.
//
// The drain runs as a detached ("daemon") task of the loop -- see Holder in waker.c++ for why
// that ordering is required at loop teardown. The one consequence: kj::WaitScope::
// cancelAllDetached() (a documented KJ hack, unused in workerd) kills the drain too. That is
// tolerated rather than fatal: wakes are always queued in `pending` regardless, and the drain is
// restarted lazily by ensureDrain() -- called whenever a cell is created on the loop and at the
// top of every poll of a bridged future -- whose first arm() finds the queue non-empty and fires
// at once, so such wakes are delayed until the loop next touches a bridged future, never lost.
class CrossThreadWakeSink final: public kj::AtomicRefcounted {
 public:
  // Owning thread only: the sink of the current event loop, with its drain running.
  static kj::Arc<CrossThreadWakeSink> forCurrentLoop();

  // Any thread: queue `cell` for a same-thread wake on the owning loop and wake that loop. Once
  // the loop is gone, drops the reference and does nothing. Const because cells reach the sink
  // through a shared handle; the state is mutex-guarded.
  void enqueue(kj::Arc<FutureWakerCell> cell) const;

  // Owning thread only: (re)start the drain daemon if it is not running (it is cancelled by
  // ~EventLoop, where restarting is impossible and unnecessary, or by cancelAllDetached()).
  // One relaxed atomic load when it is.
  void ensureDrain() const;

 private:
  // The per-loop owner: holds the sink and closes it when the loop destroys it. Defined in
  // waker.c++.
  struct Holder;

  // Owning thread, drain coroutine only.
  static kj::Promise<void> drainLoop(kj::Arc<CrossThreadWakeSink> sink);
  void arm(kj::Own<const kj::CrossThreadPromiseFulfiller<void>> fulfiller) const;
  kj::Vector<kj::Arc<FutureWakerCell>> takePending() const;
  void close() const;

  // Whether a drain coroutine currently exists for this sink. Written only on the owning thread
  // (set by ensureDrain(), cleared by the coroutine frame's destruction), read by ensureDrain()
  // on the same thread; atomic so the sink stays trivially shareable, not for cross-thread use.
  mutable std::atomic<bool> drainRunning{false};

  struct State {
    kj::Vector<kj::Arc<FutureWakerCell>> pending;
    // The current fulfiller, consumed by the first enqueue() after it is armed; the drain
    // installs the next one. Never dropped by close(): its promise side is owned by the drain
    // coroutine, which dies first (cancelling it), so a later destruction of the fulfiller is a
    // no-op wherever it happens.
    kj::Maybe<kj::Own<const kj::CrossThreadPromiseFulfiller<void>>> fulfiller;
    bool closed = false;
  };
  kj::MutexGuarded<State> state;
};

// =======================================================================================
// FutureWakerCell

// FutureWakerCell is the thread-safe "waker cell" behind every Rust waker that outlives a single
// `Future::poll()` call. `std::task::Waker` is `Send + Sync`, so safe Rust may clone, wake, and
// drop it from any thread; the cell upholds that contract:
//
//   - clone/drop are atomic refcount operations (kj::AtomicRefcounted, handed across the FFI as
//     `kj::Arc<FutureWakerCell>`).
//   - wakeByRef() checks whether it is running on the owning event loop's thread. On the owning
//     thread — the overwhelmingly common case, e.g. every tokio I/O readiness event under
//     kj-rs-tokio — it arms the owning FuturePollEvent's `kj::_::Event` directly via
//     `Event::armDepthFirst()`, which is idempotent across same-turn arms and safe to call from
//     within the event's own `fire()` (turn() unlinks the event before firing). This is the same
//     way a RustPromiseAwaiter leaf arms the FuturePollEvent when its Promise becomes ready — a
//     wake is just another leaf arming the same event. From any other thread (including threads
//     with no KJ event loop at all, e.g. tokio's blocking pool), wakeByRef() hands the cell to
//     its loop's CrossThreadWakeSink (above), whose drain repeats the wake from the owning
//     thread, where the weak event link below is safe to read.
//
// Neutralize-on-drop: the cell's link to the Event is weak — a `kj::Maybe<Event&>` invalidated
// (structurally, by the owning FuturePollEvent's RAII guard; see awaiter.h) when that event is
// destroyed. The link is only ever read or written on the owning thread (same-thread wakes,
// neutralize()), so it needs no synchronization; a cell reference that Rust retains past the
// Future's lifetime (e.g. parked in a channel's AtomicWaker) observes a dead link — or, cross-
// thread, a sink closed with its loop — and the wake is a safe no-op rather than arming a freed
// Event.
//
// Ownership only ever crosses the FFI as real `kj::Arc<FutureWakerCell>` handles (PollWaker::
// cloneCell(), addRef()). Rust's RawWakerVTable island (waker.rs) carries its handle in the
// RawWaker data slot, disowning/reowning it at the vtable edge — the one place `std::task::Waker`
// forces a raw pointer.
class FutureWakerCell final: public kj::AtomicRefcounted {
 public:
  explicit FutureWakerCell(FuturePollEvent& event)
      : executor(kj::getCurrentThreadExecutor().addRef()),
        sink(CrossThreadWakeSink::forCurrentLoop()),
        event(event) {}

  // Called by `~FuturePollEvent` (owning thread) to neutralize this cell and every outstanding
  // Rust reference to it, making any subsequent wake a safe no-op (a cross-thread wake is
  // replayed on the owning thread by the sink's drain and lands here too). Const because
  // `kj::Arc` (like the FFI) only hands out const access; `event` is owning-thread-only interior
  // state.
  //
  // `event` is unsynchronized, so this write must be on the owning thread -- the thread whose
  // wakeByRef() reads it -- or on a thread with no event loop at all after the owning loop has
  // been torn down (then no thread can take the same-thread read path any more). KJ promises are
  // single-loop, which makes this true by construction; the check turns that into a debug-time
  // guarantee rather than a comment.
  void neutralize() const {
    KJ_DREQUIRE(executor->isCurrent() || kj::tryGetCurrentThreadExecutor() == kj::none,
        "FutureWakerCell neutralized off its owning thread");
    event = kj::none;
    // Let foreign threads see the death too (below), so their wakes stop travelling to a loop
    // that has nothing to do with them. A stale `true` is harmless: the replay finds `event`
    // gone and does nothing.
    alive.store(false, std::memory_order_release);
  }

  // Owning thread only (called at the top of each poll, by PollWaker): make sure the owning
  // loop's cross-thread drain is running. See CrossThreadWakeSink::ensureDrain().
  void ensureCrossThreadDrain() const {
    sink->ensureDrain();
  }

  // Wake from any thread: arm the owning FuturePollEvent (directly on the owning thread, via the
  // loop's CrossThreadWakeSink otherwise), or no-op if it has been neutralized. Const because Rust
  // reaches it through `&self`. Defined in waker.c++, where FuturePollEvent is a complete type.
  void wakeByRef() const;

  // The owning FuturePollEvent, if the current thread's kj::Executor is the one which owns it
  // (`event` is owning-thread-only state) and the event is still alive. Lets RustPromiseAwaiter
  // arm the event directly instead of going through a waker, when possible.
  kj::Maybe<FuturePollEvent&> tryGetFuturePollEvent() const {
    if (executor->isCurrent()) {
      return event;
    }
    return kj::none;
  }

  // Hand out a new strong reference. Const + const_cast because Rust reaches it through `&self`:
  // cells are always heap-allocated non-const (kj::arc in FuturePollEvent), and the atomic
  // refcount bump is safe from any thread.
  kj::Arc<FutureWakerCell> addRef() const {
    return const_cast<FutureWakerCell&>(*this).addRefToThis();
  }

  // Re-own a strong reference previously surrendered to a raw pointer: waker.rs disowns the
  // kj::Arc it parks in a RawWaker data slot, and its vtable's drop calls this to reclaim it.
  // Exposed to Rust as an `unsafe fn`: `this` must carry exactly such a surrendered reference,
  // and dropping the returned handle releases it.
  kj::Arc<FutureWakerCell> reown() const {
    return kj::Arc<FutureWakerCell>::reown(this);
  }

 private:
  // The owning event loop's executor, used to route wakes: captured at construction (which
  // happens on the owning thread), immutable afterwards, safe to read from any thread. Owned via
  // addRef() so a cell retained by Rust past loop teardown still has a valid Executor to ask
  // (isCurrent() then reports false and the wake takes the — closed, no-op — sink path).
  kj::Own<const kj::Executor> executor;

  // The owning loop's cross-thread doorway (see CrossThreadWakeSink), captured at construction.
  // Shared ownership so a cell retained by Rust past the loop's lifetime still has a valid sink
  // to hand itself to; the sink is closed by then, and enqueue() is a no-op.
  kj::Arc<CrossThreadWakeSink> sink;

  // False once neutralize() ran: the thread-safe shadow of `event`'s liveness, so a foreign
  // thread can skip the sink for a waker whose future is already gone (a retained dead waker
  // parked in a busy channel would otherwise wake the loop for nothing on every wake).
  mutable std::atomic<bool> alive{true};

  // Weak, owner-invalidated reference to the owning FuturePollEvent: non-owning (the event lives
  // in the promise graph; the cell must observe its death, never extend its life) and nulled by
  // `neutralize()` when that event is destroyed.
  //
  // Deliberately NOT a kj::Weak, even though the event is a kj::PtrTarget and every other weak
  // edge to it uses one (see awaiter.h): kj::Weak is a single-threaded type (its shared WeakCell
  // refcount is not atomic, and `Weak<const T>` is explicitly unimplemented), while this cell is
  // dropped on whatever thread releases the last kj::Arc reference — a ~Weak there would race the
  // owning thread's ~PtrTarget on the WeakCell refcount. This hand-rolled link is the
  // thread-compatible equivalent: only read and written on the owning thread (so it needs no
  // synchronization), with invalidation tied to the event's destruction by ~NeutralizeGuard, and
  // destruction on foreign threads touching nothing but the Maybe's bits. Mutable because the
  // cell is only ever reached const (kj::Arc / FFI `&self`).
  mutable kj::Maybe<FuturePollEvent&> event;
};

// =======================================================================================
// PollWaker

// PollWaker is the waker C++ passes to `Future::poll()`. It lives on the stack / in a coroutine
// frame for the duration of a single poll, and Rust only ever borrows it (waker.rs wraps it in a
// Waker whose drop is a no-op).
//
//   - wakeByRef() delegates to the event's FutureWakerCell, which handles both the same-thread
//     (synchronous same-turn re-poll) and foreign-thread (sink) cases — `&Waker` is Sync, so even
//     the borrowed waker may legally be woken from another thread during the poll.
//   - cloneCell() is how Rust retains a waker past the poll: it hands out a strong reference to
//     the event's FutureWakerCell, so a later wake from any thread arms the same event.
//   - tryGetFuturePollEvent() lets RustPromiseAwaiter (which helps Rust `.await` KJ Promises)
//     arm the event directly instead of going through a waker, when possible (owning thread
//     only).
class PollWaker final {
 public:
  // `futurePollEvent` is the FuturePollEvent responsible for calling `Future::poll()`, and must
  // outlive this PollWaker. Construction happens on the owning thread at the top of each poll.
  explicit PollWaker(FuturePollEvent& futurePollEvent);
  ~PollWaker() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(PollWaker);

  // Wake from any thread: arm the associated FuturePollEvent so it (re-)polls.
  void wakeByRef() const;

  // Hand out a new strong reference to the event's FutureWakerCell, for Rust to retain and wake
  // later. Safe from any thread (atomic refcount).
  kj::Arc<FutureWakerCell> cloneCell() const;

  // The FuturePollEvent whose poll() this waker was created for, if the current thread's
  // kj::Executor is the one which owns it. Delegates to the cell's weak, neutralize-on-drop
  // event link.
  kj::Maybe<FuturePollEvent&> tryGetFuturePollEvent() const;

 private:
  // A strong reference to the event's cell — the PollWaker's only state. The cell handles every
  // operation: wakes route by thread through it, cloneCell() hands out more references to it, and
  // its weak event link answers tryGetFuturePollEvent(). It works from any thread and cannot
  // dangle no matter how long this PollWaker lives. (In practice a PollWaker is a stack object
  // scoped to a single poll; the atomic refcount pair per poll is noise next to the poll itself.)
  kj::Arc<FutureWakerCell> cell;
};

}  // namespace kj_rs
