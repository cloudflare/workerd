pub use bridge::*;

use crate::awaiter::RustWaker;
use crate::awaiter::WakerRef;
use crate::awaiter::clone_waker;

// Safety: FutureWakerCell uses atomic reference counting and routes foreign-thread wakes through
// its owning event loop's cross-thread sink.
unsafe impl Send for bridge::FutureWakerCell {}
// Safety: all shared operations on FutureWakerCell are thread-safe.
unsafe impl Sync for bridge::FutureWakerCell {}

#[cxx::bridge(namespace = "kj_rs")]
mod bridge {

    /// Representation of a `GuardedRustPromiseAwaiter` in C++. The size of the blob should match.
    #[derive(Debug)]
    pub struct GuardedRustPromiseAwaiterRepr {
        _bindgen_opaque_blob: [u64; 16usize],
    }

    extern "Rust" {
        type WakerRef<'a>;
        type RustWaker;
        fn wake(self: &RustWaker);
        fn will_wake(self: &RustWaker, waker: &WakerRef) -> bool;
        fn clone_waker(waker: &WakerRef) -> Box<RustWaker>;
    }

    unsafe extern "C++" {
        include!("kj-rs/waker.h");

        type PollWaker;
        #[cxx_name = "wakeByRef"]
        fn wake_by_ref(self: &PollWaker);
        #[cxx_name = "cloneCell"]
        fn clone_cell(self: &PollWaker) -> KjArc<FutureWakerCell>;

        type FutureWakerCell;
        #[cxx_name = "wakeByRef"]
        fn wake_by_ref(self: &FutureWakerCell);
        #[cxx_name = "addRef"]
        fn add_ref(self: &FutureWakerCell) -> KjArc<FutureWakerCell>;
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

        /// # Safety
        /// The pointers must identify valid storage and a live waker for the awaiter's lifetime.
        unsafe fn guarded_rust_promise_awaiter_new_in_place(
            ptr: *mut GuardedRustPromiseAwaiter,
            node: OwnPromiseNode,
        );
        /// # Safety
        /// `ptr` must point to an initialized guarded awaiter.
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
