//! KJ ffi crate.
//!
//! This crate provides bindings to common KJ classes and functions that do not require special
//! C++ bridge support.

// Safety & panic enforcement walls. Inherent-FFI crate. Test code exempted.
//
// `unsafe_code` is denied crate-wide. Every source file is either wholly-safe or a dedicated
// FFI-island file whose file-top `#![allow(unsafe_code)]` states its status. This is a thin kj
// FFI binding crate, so its two `#[cxx::bridge]` binding modules are the only islands: http.rs
// (the HttpHeaders / HttpService bridge, plus the safe getter wrappers and
// `CustomHeaderId::from_ptr_slice`) and io.rs (the async-stream bridge). All wholly-safe wrapper
// business logic lives in deny-protected files — headers.rs (`HeadersRef`/`Headers`/
// `CustomHeaderId`), service.rs (`Service`/`CxxService`/`ServiceResponse`/`ConnectResponse`),
// streams.rs (`AsyncInputStream`/`AsyncOutputStream`), codec.rs (the HTTP/1.1 text codec) —
// re-exported through the islands so the
// public `kj::http::*` / `kj::io::*` paths are unchanged. The bridges stay in http.rs / io.rs
// because the `kj::http::ffi` / `kj::io::ffi` module paths and the generated `http.rs.h` header
// are referenced across the workerd consumers. own.rs (the KjOwn disposal helper) is wholly-safe.
#![deny(unsafe_code)]
#![deny(unsafe_op_in_unsafe_fn)]
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

pub mod http;
pub mod io;
pub mod macros;

// Re-exported so that `$crate::KjError` / `$crate::KjExceptionType` resolve inside the
// macros in `macros.rs` regardless of whether the invoking crate itself depends on `cxx`.
pub use cxx::KjError;
pub use cxx::KjExceptionType;

// Wholly-safe wrapper modules (no `#![allow(unsafe_code)]`, zero `unsafe`). Their public items are
// re-exported from the `http` / `io` islands, so the crate's `kj::http::*` / `kj::io::*` public
// paths are unchanged. Kept private here to avoid introducing new top-level `kj::{codec, headers,
// service, streams}` paths.
mod codec;
mod headers;
mod service;
mod streams;

mod own;
pub use own::*;

pub type Result<T> = std::result::Result<T, cxx::KjError>;
