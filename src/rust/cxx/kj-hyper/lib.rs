//! HTTP/1.1, `WebSocket` and TLS for a Rust server whose handlers are kj HTTP interfaces: hyper
//! and rustls on the Rust side, `kj::HttpService` and friends on the C++ side.
//!
//! - [`server`]: one accepted tokio connection served by hyper, each request dispatched to a
//!   [`server::Handler`] with kj-typed arguments (`kj::http::HeadersRef`,
//!   `kj::io::AsyncInputStream`, `kj::http::ServiceResponse`), so it can be forwarded straight to
//!   a C++ `WorkerInterface`.
//! - [`client`]: a pooled HTTP/1.1 client implementing `kj::http::Service`, so C++ can make
//!   outbound requests through it: to one fixed peer through a dialer, or to whatever authority a
//!   request's URL names.
//! - [`tls`]: rustls configurations from workerd's `TlsOptions`, and the handshakes.
//! - `handshake`: the WebSocket handshake's computed header values. The `WebSocket` itself is
//!   kj's (`kj::newWebSocket`) over the upgraded connection, built on the C++ side.
//! - [`into_kj_stream`] / [`tcp_into_kj_stream`]: tokio streams handed to C++ as
//!   `kj::AsyncIoStream`.
//!
//! All `unsafe` code lives in `ffi.rs`. The threading, lifetime and cancellation rules are in
//! src/rust/cxx/AGENTS.md.

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

mod body;
pub mod client;
pub mod ffi;
mod handshake;
mod io;
pub mod server;
mod starttls;
pub mod tls;

pub use body::Head;
pub use body::HeaderBlock;
pub use ffi::WebSocketCompression;
pub use ffi::WebSocketErrorHandler;
pub use handshake::accept_key;
pub use io::into_kj_stream;
pub use io::socket_into_kj_stream;
pub use io::tcp_into_kj_stream;

pub type Result<T> = kj::Result<T>;
