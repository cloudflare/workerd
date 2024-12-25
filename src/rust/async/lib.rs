mod waker;

mod future;
use future::box_future_void_drop_in_place;
use future::box_future_void_poll;
pub use future::BoxFuture;
use future::PtrBoxFuture;

mod promise;
pub use promise::OwnPromiseNode;
pub use promise::OwnPromiseNodeFuture;
use promise::PtrOwnPromiseNode;

mod test_futures;
use test_futures::new_layered_ready_future_void;
use test_futures::new_pending_future_void;
use test_futures::new_ready_future_void;
use test_futures::new_threaded_delay_future_void;
use test_futures::new_waking_future_void;

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

        type AwaitWaker;
        fn is_current(&self) -> bool;
        fn wake_after(&self, node: Pin<&mut OwnPromiseNode>);
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/future.h");

        type BoxFutureVoid = crate::BoxFuture<()>;
        type PtrBoxFutureVoid = crate::PtrBoxFuture<()>;
    }

    extern "Rust" {
        fn box_future_void_poll(future: &mut BoxFutureVoid, cx: &AwaitWaker) -> bool;
        unsafe fn box_future_void_drop_in_place(ptr: PtrBoxFutureVoid);
    }

    unsafe extern "C++" {
        include!("workerd/rust/async/promise.h");

        type OwnPromiseNode = crate::OwnPromiseNode;
        type PtrOwnPromiseNode = crate::PtrOwnPromiseNode;

        unsafe fn own_promise_node_drop_in_place(node: PtrOwnPromiseNode);
    }

    // Helper functions to create OwnPromiseNodes for testing purposes.
    unsafe extern "C++" {
        fn new_ready_promise_node() -> OwnPromiseNode;
        fn new_coroutine_promise_node() -> OwnPromiseNode;
    }

    // Helper functions to create BoxFutureVoids for testing purposes.
    extern "Rust" {
        fn new_pending_future_void() -> BoxFutureVoid;
        fn new_ready_future_void() -> BoxFutureVoid;
        fn new_waking_future_void() -> BoxFutureVoid;
        fn new_threaded_delay_future_void() -> BoxFutureVoid;
        fn new_layered_ready_future_void() -> BoxFutureVoid;
    }
}
