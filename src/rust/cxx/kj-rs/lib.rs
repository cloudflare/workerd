// Safety & panic enforcement walls. Inherent-FFI crate: unsafe is
// concentrated at the bridge and every op must sit in an explicit, documented unsafe
// block; prod code returns Result/KjError rather than panicking (a panic on the async
// poll path is a process abort). Test code is exempted below.
#![deny(unsafe_op_in_unsafe_fn)]
// Quarantine unsafe into named FFI islands: deny unsafe crate-wide, then re-allow it only on the
// modules that genuinely need it (each carries its own `#![allow(unsafe_code)]`). Any module
// without that opt-in — and any newly-added module — is compiler-proven unsafe-free, and no future
// edit can smuggle unsafe into non-island code without tripping this deny.
#![deny(unsafe_code)]
#![deny(clippy::undocumented_unsafe_blocks)]
#![deny(
    clippy::unwrap_used,
    clippy::expect_used,
    clippy::panic,
    clippy::unreachable,
    clippy::todo,
    clippy::unimplemented
)]
#![cfg_attr(
    test,
    allow(
        clippy::unwrap_used,
        clippy::expect_used,
        clippy::panic,
        clippy::unreachable,
        clippy::todo,
        clippy::unimplemented
    )
)]

// The cxx bridge expands vocabulary builtins (KjRc, KjMaybe, ...) to `::kj_rs::...` paths; make
// that path resolve inside this crate itself, since ffi.rs's bridge uses them too.
extern crate self as kj_rs;

pub use awaiter::PromiseAwaiter;
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

pub use crate::ffi::FutureWakerCell;
pub use crate::ffi::PollWaker;

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
