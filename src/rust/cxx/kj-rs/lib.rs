// Let cxx vocabulary types resolve this crate by its generated `::kj_rs` path.
extern crate self as kj_rs;

use awaiter::OptionWaker;
pub use awaiter::PromiseAwaiter;
use awaiter::WakerRef;
pub use date::KjDate;
pub use future::FuturePollStatus;
pub use future::map_err;
pub use maybe::repr::KjMaybe;
pub use own::repr::KjOwn;
pub use promise::KjPromise;
pub use promise::KjPromiseNodeImpl;
pub use promise::OwnPromiseNode;
pub use promise::PromiseFuture;
pub use promise::new_callbacks_promise_future;
pub use refcount::repr::KjArc;
pub use refcount::repr::KjRc;

pub use crate::ffi::KjWaker;

mod awaiter;
mod date;
mod ffi;
mod future;
pub mod maybe;
mod own;
mod promise;
pub mod refcount;
mod waker;

pub mod repr {
    pub use crate::future::repr::*;
    pub use crate::maybe::repr::*;
    pub use crate::own::repr::*;
    pub use crate::refcount::repr::*;
}

pub type Result<T> = std::io::Result<T>;
pub type Error = std::io::Error;

pub trait JsgStruct {}

#[cfg(any())]
#[cxx::bridge(namespace = "kj_rs")]
mod legacy_ffi {

    /// Representation of a `GuardedRustPromiseAwaiter` in C++. The size of the blob should match.
    #[derive(Debug)]
    pub struct GuardedRustPromiseAwaiterRepr {
        _bindgen_opaque_blob: [u64; 14usize],
    }

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
        fn wake_if_some(&mut self);
    }

    unsafe extern "C++" {
        include!("kj-rs/waker.h");

        // Match the definition of the abstract virtual class in the C++ header.
        type KjWaker;
        #[cxx_name = "clone"]
        fn clone_kj_waker(self: &KjWaker) -> *const KjWaker;
        fn wake(self: &KjWaker);
        fn wake_by_ref(self: &KjWaker);
        fn drop(self: &KjWaker);

        type PollWaker;
        #[cxx_name = "wakeByRef"]
        fn poll_waker_wake_by_ref(self: &PollWaker);
        #[cxx_name = "cloneCell"]
        fn clone_cell(self: &PollWaker) -> KjMaybe<KjRc<FutureWakerCell>>;

        type FutureWakerCell;
        #[cxx_name = "wakeByRef"]
        fn cell_wake_by_ref(self: &FutureWakerCell);
        #[cxx_name = "addRef"]
        fn add_ref(self: &FutureWakerCell) -> KjRc<FutureWakerCell>;
        /// Reclaims a strong reference previously surrendered to a raw waker data pointer.
        unsafe fn reown(self: &FutureWakerCell) -> KjRc<FutureWakerCell>;
    }

    unsafe extern "C++" {
        include!("kj-rs/promise.h");

        type OwnPromiseNode = crate::OwnPromiseNode;

        /// # Safety
        /// `node` must point to a live `OwnPromiseNode`.
        unsafe fn own_promise_node_drop_in_place(node: *mut OwnPromiseNode);
    }
}
