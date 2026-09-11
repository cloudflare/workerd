//! Hyper-backed HTTP client and server behind the KJ interfaces.
//!
//! - `client`: outbound HTTP/1.1 client implementing `kj::HttpService` semantics on top of a
//!   hyper 1.x client, including WebSocket upgrades and CONNECT tunnels (C++ wrapper:
//!   `newHyperHttpService()` in hyper-http.h).
//! - `server`: inbound HTTP/1.1 server dispatching every request to a C++ `kj::HttpService`,
//!   including `acceptWebSocket()` and CONNECT dispatch (C++ wrapper:
//!   `newHyperHttpConnection()` in hyper-http.h; the C++ side owns the accept loop).
//! - `ws`: a `kj::WebSocket`-semantics session (framing, auto-pong, close codes, MANUAL-mode
//!   permessage-deflate) over the hyper-upgraded byte stream, shared by both directions.
//! - `tls`: rustls client/server TLS config mapping for the subset of workerd's
//!   `config::TlsOptions` these paths use; see tls.rs for its documented divergences.
//!
//! Scope: HTTP/1.1 over TCP with keep-alive, WebSocket upgrades, CONNECT; client- and
//! server-side TLS. No h2.
//!
//! # Unsafe quarantine
//!
//! `unsafe_code` is denied crate-wide (below). Every source file is either wholly-safe —
//! compiler-proven `unsafe`-free — or the crate's single dedicated FFI-island file whose file-top
//! `#![allow(unsafe_code)]` makes its status readable at a glance. That island is `ffi.rs`: the
//! `#[cxx::bridge]` wire, its entry-point functions, and the safe wrappers over the raw C++
//! header-table/service pointers (`HeaderTablePtr` / `ServicePtr` / `SharedFilter`). The business
//! logic — client.rs, server.rs, translate.rs, ws.rs, and every other module — is wholly-safe.

// Safety & panic enforcement walls. Inherent-FFI crate; this crate runs
// in the abort-on-panic async poll path, so prod code must return Result/KjError. Test
// code exempted.
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

// The `#[cxx::bridge]` and its entry-point functions live in `ffi.rs` (the crate's single
// dedicated FFI island, which also folds in the former cxx_ptr.rs pointer wrappers) so this crate
// root stays wholly-safe. `ffi` is public so consumers and the business modules keep using
// `kj_hyper::ffi` / `crate::ffi` unchanged.
pub mod ffi;

// `client` is public so out-of-crate consumers can reuse its kj→http translation helpers
// and channel-backed request body for the *client* direction, the mirror image of what
// `translate` shares for the server direction.
pub mod client;
mod client_tls;
mod server;
mod stall;
mod tls;
pub mod translate;
mod upgraded_io;
mod ws;
mod ws_ext;
mod ws_pipe;

pub use client::HyperClient;
pub use server::HyperConnection;
pub use tls::HyperTlsClientConfig;
pub use tls::HyperTlsServerConfig;
pub use translate::HyperRequestBody;
pub use translate::HyperResponseBodySink;
