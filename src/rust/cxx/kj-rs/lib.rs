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
