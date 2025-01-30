mod await_;
pub use await_::GuardedRustPromiseAwaiter;
use await_::PtrGuardedRustPromiseAwaiter;

mod future;
use future::box_future_drop_in_place_fallible_i32;
use future::box_future_drop_in_place_fallible_void;
use future::box_future_drop_in_place_void;
use future::box_future_poll_fallible_i32;
use future::box_future_poll_fallible_void;
use future::box_future_poll_void;
use future::box_future_poll_with_co_await_waker_fallible_i32;
use future::box_future_poll_with_co_await_waker_fallible_void;
use future::box_future_poll_with_co_await_waker_void;
pub use future::BoxFuture;
use future::PtrBoxFuture;

mod lazy_pin_init;

mod promise;
pub use promise::OwnPromiseNode;
pub use promise::Promise;
use promise::PtrOwnPromiseNode;
use promise::PtrPromise;

mod test_futures;
use test_futures::new_error_handling_future_void;
use test_futures::new_errored_future_fallible_void;
use test_futures::new_layered_ready_future_void;
use test_futures::new_naive_select_future_void;
use test_futures::new_pending_future_void;
use test_futures::new_ready_future_void;
use test_futures::new_threaded_delay_future_void;
use test_futures::new_waking_future_void;
use test_futures::new_wrapped_waker_future_void;

mod waker;
use waker::RustWaker;

type CxxResult<T> = std::result::Result<T, cxx::Exception>;

type Result<T> = std::io::Result<T>;
type Error = std::io::Error;

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
        include!("workerd/rust/async/await.h");

        type CoAwaitWaker;
        fn is_current(&self) -> bool;
    }

    extern "Rust" {
        // We expose the Rust Waker type to C++ through this RustWaker reference wrapper. cxx-rs
        // does not allow us to export types defined outside this crate, such as Waker, directly.
        //
        // `LazyRustPromiseAwaiter` (the implementation of `.await` syntax/the IntoFuture trait),
        // stores a RustWaker immediately after `GuardedRustPromiseAwaiter` in declaration order.
        // pass the Waker to the `RustPromiseAwaiter` class, which is implemented in C++
        type RustWaker;
        fn is_some(&self) -> bool;
        fn is_none(&self) -> bool;
        fn wake(&mut self);
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/future.h");

        // TODO(now): Generate boilerplate with a macro.
        type BoxFutureVoid = crate::BoxFuture<()>;
        type PtrBoxFutureVoid = crate::PtrBoxFuture<()>;

        // TODO(now): Generate boilerplate with a macro.
        type BoxFutureFallibleVoid = crate::BoxFuture<crate::Result<()>>;
        type PtrBoxFutureFallibleVoid = crate::PtrBoxFuture<crate::Result<()>>;
    }

    extern "Rust" {
        // TODO(now): Generate boilerplate with a macro.
        fn box_future_poll_void(
            future: &mut BoxFutureVoid,
            waker: &CxxWaker,
            fulfiller: Pin<&mut BoxFutureFulfillerVoid>,
        ) -> bool;
        fn box_future_poll_with_co_await_waker_void(
            future: &mut BoxFutureVoid,
            waker: &CoAwaitWaker,
        ) -> bool;
        unsafe fn box_future_drop_in_place_void(ptr: PtrBoxFutureVoid);

        // TODO(now): Generate boilerplate with a macro.
        fn box_future_poll_fallible_void(
            future: &mut BoxFutureFallibleVoid,
            waker: &CxxWaker,
            fulfiller: Pin<&mut BoxFutureFulfillerFallibleVoid>,
        ) -> Result<bool>;
        fn box_future_poll_with_co_await_waker_fallible_void(
            future: &mut BoxFutureFallibleVoid,
            waker: &CoAwaitWaker,
            fulfiller: Pin<&mut BoxFutureFulfillerFallibleVoid>,
        ) -> Result<bool>;
        unsafe fn box_future_drop_in_place_fallible_void(ptr: PtrBoxFutureFallibleVoid);

        fn box_future_poll_fallible_i32(
            future: &mut BoxFutureFallibleI32,
            waker: &CxxWaker,
            fulfiller: Pin<&mut BoxFutureFulfillerFallibleI32>,
        ) -> Result<bool>;
        fn box_future_poll_with_co_await_waker_fallible_i32(
            future: &mut BoxFutureFallibleI32,
            waker: &CoAwaitWaker,
            fulfiller: Pin<&mut BoxFutureFulfillerFallibleI32>,
        ) -> Result<bool>;
        unsafe fn box_future_drop_in_place_fallible_i32(ptr: PtrBoxFutureFallibleI32);
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
            rust_waker_ptr: *mut RustWaker,
            node: OwnPromiseNode,
        );
        unsafe fn guarded_rust_promise_awaiter_drop_in_place(ptr: PtrGuardedRustPromiseAwaiter);

        // TODO(now): Reduce to just one function.
        fn poll_with_co_await_waker(
            self: Pin<&mut GuardedRustPromiseAwaiter>,
            waker: &CoAwaitWaker,
        ) -> bool;
        fn poll(self: Pin<&mut GuardedRustPromiseAwaiter>) -> bool;

        fn take_own_promise_node(self: Pin<&mut GuardedRustPromiseAwaiter>) -> OwnPromiseNode;
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/promise-boilerplate.h");

        // TODO(now): Generate boilerplate with a macro.
        type PromiseVoid = crate::Promise<()>;
        type PtrPromiseVoid = crate::PtrPromise<()>;
        fn own_promise_node_unwrap_void(node: OwnPromiseNode) -> Result<()>;
        unsafe fn promise_drop_in_place_void(promise: PtrPromiseVoid);
        fn promise_into_own_promise_node_void(promise: PromiseVoid) -> OwnPromiseNode;
    }
    // -----------------------------------------------------
    // Test functions

    // Helper functions to create Promises for testing purposes.
    unsafe extern "C++" {
        include!("workerd/rust/async/test-promises.h");

        fn new_ready_promise_void() -> PromiseVoid;
        fn new_pending_promise_void() -> PromiseVoid;
        fn new_coroutine_promise_void() -> PromiseVoid;

        fn new_errored_promise_void() -> PromiseVoid;
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
        fn new_layered_ready_future_void() -> BoxFutureFallibleVoid;

        fn new_naive_select_future_void() -> BoxFutureFallibleVoid;
        fn new_wrapped_waker_future_void() -> BoxFutureFallibleVoid;

        fn new_errored_future_fallible_void() -> BoxFutureFallibleVoid;
        fn new_error_handling_future_void() -> BoxFutureVoid;
    }
}
