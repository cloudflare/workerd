mod await_;
pub use await_::GuardedRustPromiseAwaiter;
use await_::PtrGuardedRustPromiseAwaiter;

mod future;
use future::box_future_void_drop_in_place;
use future::box_future_void_poll;
pub use future::BoxFuture;
use future::PtrBoxFuture;

mod lazy_pin_init;

mod promise;
pub use promise::OwnPromiseNode;
use promise::PtrOwnPromiseNode;

mod test_futures;
use test_futures::new_layered_ready_future_void;
use test_futures::new_naive_select_future_void;
use test_futures::new_pending_future_void;
use test_futures::new_ready_future_void;
use test_futures::new_threaded_delay_future_void;
use test_futures::new_waking_future_void;
use test_futures::new_wrapped_waker_future_void;

mod waker;
use waker::RustWaker;

#[cxx::bridge(namespace = "workerd::rust::async")]
mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/async/waker.h");

        // Match the definition of the abstract virtual class in the C++ header.
        type CxxWaker;
        fn clone(&self) -> *const CxxWaker;
        fn wake(&self);
        fn wake_by_ref(&self);
        fn drop(&self);
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/waker.h");

        type KjWaker;
        fn is_current(&self) -> bool;
    }

    extern "Rust" {
        // We expose the Rust Waker type to C++ through this RustWaker reference wrapper. cxx-rs
        // does not allow us to export types not defined in this module directly.
        //
        // When Rust code polls a KJ promise with a Waker implemented in Rust, it uses this type to
        // pass the Waker to the `RustPromiseAwaiter` class, which is implemented in C++.
        //
        // TODO(now): Make `&mut self`.
        type RustWaker;
        fn wake(&self);
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/future.h");

        type BoxFutureVoid = crate::BoxFuture<()>;
        type PtrBoxFutureVoid = crate::PtrBoxFuture<()>;
    }

    extern "Rust" {
        fn box_future_void_poll(future: &mut BoxFutureVoid, waker: &KjWaker) -> bool;
        unsafe fn box_future_void_drop_in_place(ptr: PtrBoxFutureVoid);
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/promise.h");

        type OwnPromiseNode = crate::OwnPromiseNode;
        type PtrOwnPromiseNode = crate::PtrOwnPromiseNode;

        unsafe fn own_promise_node_drop_in_place(node: PtrOwnPromiseNode);
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/await.h");

        type GuardedRustPromiseAwaiter = crate::GuardedRustPromiseAwaiter;
        type PtrGuardedRustPromiseAwaiter = crate::PtrGuardedRustPromiseAwaiter;

        unsafe fn guarded_rust_promise_awaiter_new_in_place(
            ptr: PtrGuardedRustPromiseAwaiter,
            node: OwnPromiseNode,
        );
        unsafe fn guarded_rust_promise_awaiter_drop_in_place(ptr: PtrGuardedRustPromiseAwaiter);

        fn poll_with_kj_waker(self: Pin<&mut GuardedRustPromiseAwaiter>, waker: &KjWaker) -> bool;
        unsafe fn poll(self: Pin<&mut GuardedRustPromiseAwaiter>, waker: *const RustWaker) -> bool;
    }

    // Helper functions to create OwnPromiseNodes for testing purposes.
    unsafe extern "C++" {
        include!("workerd/rust/async/test-promises.h");

        fn new_ready_promise_node() -> OwnPromiseNode;
        fn new_pending_promise_node() -> OwnPromiseNode;
        fn new_coroutine_promise_node() -> OwnPromiseNode;
    }

    enum CloningAction {
        None,
        CloneSameThread,
        CloneBackgroundThread,
        WakeByRefThenCloneSameThread,
    }

    enum WakingAction {
        None,
        WakeByRefSameThread,
        WakeByRefBackgroundThread,
        WakeSameThread,
        WakeBackgroundThread,
    }

    // Helper functions to create BoxFutureVoids for testing purposes.
    extern "Rust" {
        fn new_pending_future_void() -> BoxFutureVoid;
        fn new_ready_future_void() -> BoxFutureVoid;
        fn new_waking_future_void(
            cloning_action: CloningAction,
            waking_action: WakingAction,
        ) -> BoxFutureVoid;
        fn new_threaded_delay_future_void() -> BoxFutureVoid;
        fn new_layered_ready_future_void() -> BoxFutureVoid;
        fn new_naive_select_future_void() -> BoxFutureVoid;
        fn new_wrapped_waker_future_void() -> BoxFutureVoid;
    }
}
