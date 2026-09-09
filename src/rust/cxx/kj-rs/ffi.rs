//! The `#[cxx::bridge]` FFI island for kj-rs.
//!
//! This is the crate's dedicated `#[cxx::bridge]` file (file-top `#![allow(unsafe_code)]`): the
//! cxx bridge DSL and its generated glue are inherently unsafe (extern "C++" vtables, placement
//! new/drop, C-ABI signatures) — a genuine seam. The bridge module is re-exported as
//! `crate::ffi::*`, the path the rest of the crate (awaiter.rs, promise.rs, waker.rs) uses.
//!
//! The crate root `lib.rs` is wholly-safe. The per-type vocabulary islands
//! (`future.rs`, `awaiter.rs`, `waker.rs`, `promise.rs`, `own.rs`, `refcount.rs`, `maybe.rs`)
//! carry their own file-top `#![allow(unsafe_code)]`: they implement individual unsafe primitive
//! types, distinct from this — the crate's cxx bridge.
#![allow(unsafe_code)]

pub use bridge::*;

use crate::awaiter::RustWaker;
use crate::awaiter::WakerRef;
use crate::awaiter::clone_waker;

// SAFETY: `FutureWakerCell` is C++'s thread-safe waker cell (waker.h). Everything Rust can reach
// through a shared reference or a `KjArc` handle is safe from any thread: `addRef`/`reown` and
// handle drops are atomic refcount operations (kj::AtomicRefcounted), `wakeByRef` routes wakes
// through an owning-executor check (same-thread direct arm, cross-thread fulfiller otherwise),
// and the last handle may destroy the cell on any thread (its members — an Executor own, a
// mutexed fulfiller, and an owning-thread-only weak link that destruction does not dereference —
// all tolerate that). These impls are what let `KjArc<FutureWakerCell>` (and thus the
// `std::task::Waker` built over it in waker.rs) be `Send + Sync`, as the `Waker` contract
// requires.
unsafe impl Send for bridge::FutureWakerCell {}
// SAFETY: see the `Send` impl above.
unsafe impl Sync for bridge::FutureWakerCell {}

#[cxx::bridge(namespace = "kj_rs")]
// The cxx bridge DSL and its generated glue are inherently unsafe (extern "C++" vtables, placement
// new/drop, C-ABI signatures). This is a genuine seam.
// missing_safety_doc: the `# Safety` docs on `reown` below are for human readers only — the cxx
// macro does not forward doc comments to the generated unsafe shim, so the lint cannot be
// satisfied by documentation here.
#[expect(clippy::missing_safety_doc)]
mod bridge {

    /// Representation of a `GuardedRustPromiseAwaiter` in C++. The size of the blob should match.
    #[derive(Debug)]
    pub struct GuardedRustPromiseAwaiterRepr {
        _bindgen_opaque_blob: [u64; 16usize],
    }

    extern "Rust" {
        type WakerRef<'a>;

        /// An owned clone of a `std::task::Waker` (awaiter.rs). cxx-rs does not allow us to
        /// export types defined outside this crate, such as `Waker`, directly — so when
        /// RustPromiseAwaiter (C++) needs to hold on to the Waker it was last polled with, it
        /// owns one of these as a `rust::Box<RustWaker>` member: move-only, dropped
        /// automatically, no ownership convention to uphold by hand.
        type RustWaker;
        /// `Waker::wake_by_ref` on the owned clone; the caller drops the box right after.
        fn wake(self: &RustWaker);
        /// `Waker::will_wake`: would the owned clone wake the same task as the borrowed `waker`?
        fn will_wake(self: &RustWaker, waker: &WakerRef) -> bool;
        /// Clone the borrowed Waker into an owned handle for C++ to keep.
        #[expect(
            clippy::unnecessary_box_returns,
            reason = "cxx requires opaque Rust types to cross the bridge as Box"
        )]
        fn clone_waker(waker: &WakerRef) -> Box<RustWaker>;
    }

    unsafe extern "C++" {
        include!("kj-rs/waker.h");

        /// The stack-owned waker C++ passes to `Future::poll()`. Rust only ever borrows it; the
        /// `Waker` built from it (waker.rs) has a no-op drop and clones by taking a real strong
        /// reference to the event's `FutureWakerCell` via `clone_cell()`. Both operations are
        /// safe from any thread (`&Waker` is `Sync`).
        type PollWaker;
        #[cxx_name = "wakeByRef"]
        fn wake_by_ref(self: &PollWaker);
        #[cxx_name = "cloneCell"]
        fn clone_cell(self: &PollWaker) -> KjArc<FutureWakerCell>;

        /// The atomically-refcounted, thread-safe cell behind every retained waker; waking
        /// it arms the owning FuturePollEvent — directly on the owning loop's thread, through a
        /// cross-thread fulfiller from any other — and is a safe no-op after that event is
        /// destroyed.
        type FutureWakerCell;
        #[cxx_name = "wakeByRef"]
        fn wake_by_ref(self: &FutureWakerCell);
        #[cxx_name = "addRef"]
        fn add_ref(self: &FutureWakerCell) -> KjArc<FutureWakerCell>;
        /// Re-own a strong reference previously disowned into a `RawWaker` data slot (waker.rs's
        /// owned-cell vtable).
        ///
        /// # Safety
        ///
        /// `self` must carry exactly such a surrendered reference — this mints an owned handle
        /// without incrementing the count. Dropping the returned handle releases the reference.
        unsafe fn reown(self: &FutureWakerCell) -> KjArc<FutureWakerCell>;
    }

    unsafe extern "C++" {
        include!("kj-rs/promise.h");

        type OwnPromiseNode = crate::OwnPromiseNode;

        // Takes `&mut` (not a raw pointer): this is a placement-destruct of a live
        // `OwnPromiseNode` whose backing memory is owned by Rust and only reached through the
        // `&mut self` in `OwnPromiseNode`'s `Drop`. The reference is valid for the call; the
        // value is logically dead only after, inside `drop`, so no use-after-free is possible.
        // Expressing it as a borrow lets cxx generate a safe-to-call binding.
        fn own_promise_node_drop_in_place(node: &mut OwnPromiseNode);
    }

    unsafe extern "C++" {
        include!("kj-rs/awaiter.h");

        type GuardedRustPromiseAwaiter;

        /// Placement-new of the C++ awaiter into Rust-owned storage.
        ///
        /// # Safety
        ///
        /// - `ptr` must point to uninitialized storage of (at least) the size and alignment of
        ///   `GuardedRustPromiseAwaiterRepr`, valid for writes, and must stay pinned for the
        ///   awaiter's lifetime.
        /// - The awaiter must eventually be destroyed exactly once via
        ///   `guarded_rust_promise_awaiter_drop_in_place`.
        unsafe fn guarded_rust_promise_awaiter_new_in_place(
            ptr: *mut GuardedRustPromiseAwaiter,
            node: OwnPromiseNode,
        );
        /// Placement-destruct of the awaiter constructed by
        /// `guarded_rust_promise_awaiter_new_in_place`.
        ///
        /// # Safety
        ///
        /// `ptr` must point to a live awaiter previously constructed in that storage by
        /// `guarded_rust_promise_awaiter_new_in_place`, and the awaiter must not be used again
        /// afterwards (at most one drop per construction).
        unsafe fn guarded_rust_promise_awaiter_drop_in_place(ptr: *mut GuardedRustPromiseAwaiter);

        fn poll(self: Pin<&mut GuardedRustPromiseAwaiter>, waker: &WakerRef) -> bool;
        #[cxx_name = "pollWithPollWaker"]
        fn poll_with_poll_waker(
            self: Pin<&mut GuardedRustPromiseAwaiter>,
            waker: &WakerRef,
            poll_waker: &PollWaker,
        ) -> bool;

        #[must_use]
        fn take_own_promise_node(self: Pin<&mut GuardedRustPromiseAwaiter>) -> OwnPromiseNode;
    }
}
