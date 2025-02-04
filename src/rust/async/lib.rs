mod awaiter;
pub use awaiter::GuardedRustPromiseAwaiter;
use awaiter::OptionWaker;
use awaiter::PtrGuardedRustPromiseAwaiter;
use awaiter::WakerRef;

mod future;
pub use future::BoxFuture;
use future::PtrBoxFuture;

mod future_boilerplate;
use future_boilerplate::*;

mod lazy_pin_init;

mod promise;
pub use promise::OwnPromiseNode;
pub use promise::Promise;
use promise::PtrOwnPromiseNode;
use promise::PtrPromise;

mod promise_boilerplate;

mod test_futures;
use test_futures::*;

mod waker;

type CxxResult<T> = std::result::Result<T, cxx::Exception>;

type Result<T> = std::io::Result<T>;
type Error = std::io::Error;

#[cxx::bridge(namespace = "workerd::rust::async")]
mod ffi {
    extern "Rust" {
        type WakerRef<'a>;
    }

    extern "Rust" {
        // We expose the Rust Waker type to C++ through this OptionWaker reference wrapper. cxx-rs
        // does not allow us to export types defined outside this crate, such as Waker, directly.
        //
        // `LazyRustPromiseAwaiter` (the implementation of `.await` syntax/the IntoFuture trait),
        // stores a OptionWaker immediately after `GuardedRustPromiseAwaiter` in declaration order.
        // pass the Waker to the `RustPromiseAwaiter` class, which is implemented in C++
        type OptionWaker;
        fn set(&mut self, waker: &WakerRef);
        fn set_none(&mut self);
        fn wake(&mut self);
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/waker.h");

        // Match the definition of the abstract virtual class in the C++ header.
        type KjWaker;
        fn clone(&self) -> *const KjWaker;
        fn wake(&self);
        fn wake_by_ref(&self);
        fn drop(&self);
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/promise.h");

        type OwnPromiseNode = crate::OwnPromiseNode;
        type PtrOwnPromiseNode = crate::PtrOwnPromiseNode;

        unsafe fn own_promise_node_drop_in_place(node: PtrOwnPromiseNode);
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/awaiter.h");

        type GuardedRustPromiseAwaiter = crate::GuardedRustPromiseAwaiter;
        type PtrGuardedRustPromiseAwaiter = crate::PtrGuardedRustPromiseAwaiter;

        unsafe fn guarded_rust_promise_awaiter_new_in_place(
            ptr: PtrGuardedRustPromiseAwaiter,
            rust_waker_ptr: *mut OptionWaker,
            node: OwnPromiseNode,
        );
        unsafe fn guarded_rust_promise_awaiter_drop_in_place(ptr: PtrGuardedRustPromiseAwaiter);

        // TODO(now): Safety comment.
        unsafe fn poll(
            self: Pin<&mut GuardedRustPromiseAwaiter>,
            waker: &WakerRef,
            maybe_kj_waker: *const KjWaker,
        ) -> bool;

        fn take_own_promise_node(self: Pin<&mut GuardedRustPromiseAwaiter>) -> OwnPromiseNode;
    }

    // -----------------------------------------------------
    // Boilerplate

    extern "Rust" {
        // TODO(now): Generate boilerplate with a macro.
        fn box_future_poll_void(
            future: Pin<&mut BoxFutureVoid>,
            waker: &KjWaker,
            fulfiller: Pin<&mut BoxFutureFulfillerVoid>,
        ) -> bool;
        unsafe fn box_future_drop_in_place_void(ptr: PtrBoxFutureVoid);

        // TODO(now): Generate boilerplate with a macro.
        fn box_future_poll_fallible_void(
            future: Pin<&mut BoxFutureFallibleVoid>,
            waker: &KjWaker,
            fulfiller: Pin<&mut BoxFutureFulfillerFallibleVoid>,
        ) -> Result<bool>;
        unsafe fn box_future_drop_in_place_fallible_void(ptr: PtrBoxFutureFallibleVoid);

        fn box_future_poll_fallible_i32(
            future: Pin<&mut BoxFutureFallibleI32>,
            waker: &KjWaker,
            fulfiller: Pin<&mut BoxFutureFulfillerFallibleI32>,
        ) -> Result<bool>;
        unsafe fn box_future_drop_in_place_fallible_i32(ptr: PtrBoxFutureFallibleI32);
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/future-boilerplate.h");

        // TODO(now): Generate boilerplate with a macro.
        type BoxFutureVoid = crate::BoxFuture<()>;
        type PtrBoxFutureVoid = crate::PtrBoxFuture<()>;
        type BoxFutureFulfillerVoid;
        fn fulfill(self: Pin<&mut BoxFutureFulfillerVoid>);

        // TODO(now): Generate boilerplate with a macro.
        type BoxFutureFallibleVoid = crate::BoxFuture<crate::Result<()>>;
        type PtrBoxFutureFallibleVoid = crate::PtrBoxFuture<crate::Result<()>>;
        type BoxFutureFulfillerFallibleVoid;
        fn fulfill(self: Pin<&mut BoxFutureFulfillerFallibleVoid>);

        type BoxFutureFallibleI32 = crate::BoxFuture<crate::Result<i32>>;
        type PtrBoxFutureFallibleI32 = crate::PtrBoxFuture<crate::Result<i32>>;
        type BoxFutureFulfillerFallibleI32;
        fn fulfill(self: Pin<&mut BoxFutureFulfillerFallibleI32>, value: i32);
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/promise-boilerplate.h");

        // TODO(now): Generate boilerplate with a macro.
        type PromiseVoid = crate::Promise<()>;
        type PtrPromiseVoid = crate::PtrPromise<()>;
        fn own_promise_node_unwrap_void(node: OwnPromiseNode) -> Result<()>;
        unsafe fn promise_drop_in_place_void(promise: PtrPromiseVoid);
        fn promise_into_own_promise_node_void(promise: PromiseVoid) -> OwnPromiseNode;

        type PromiseI32 = crate::Promise<i32>;
        type PtrPromiseI32 = crate::PtrPromise<i32>;
        fn own_promise_node_unwrap_i32(node: OwnPromiseNode) -> Result<i32>;
        unsafe fn promise_drop_in_place_i32(promise: PtrPromiseI32);
        fn promise_into_own_promise_node_i32(promise: PromiseI32) -> OwnPromiseNode;
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
        fn new_ready_promise_i32(value: i32) -> PromiseI32;
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

        fn new_awaiting_future_i32() -> BoxFutureVoid;
        fn new_ready_future_fallible_i32(value: i32) -> BoxFutureFallibleI32;
    }
}
