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

use crate::awaiter::OptionWaker;
use crate::awaiter::WakerRef;

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
