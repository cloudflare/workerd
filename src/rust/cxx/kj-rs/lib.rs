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
}
