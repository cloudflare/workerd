// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Cap'n Proto RPC from Rust over KJ's C++ RPC system.
//!
//! The code capnpc-rust generates (`Client` types and `Server` traits) runs unchanged: this
//! crate implements capnp-rust's capability hooks over C++ `capnp::ClientHook`s. There is one
//! RPC system in the process, KJ's. A Rust server is wrapped in a C++ `capnp::Capability::Server`
//! as soon as it becomes a client, so every capability Rust holds is a KJ capability, and KJ
//! does promise resolution, embargoes, pipelining and local calls.
//!
//! - [`new_client`] turns a Rust server into a client, as `capnp_rpc::new_client` does.
//! - [`client_from_kj`] and [`client_into_kj`] move a capability between a C++
//!   `kj::Own<capnp::ClientHook>` and a typed Rust client.

mod client;
mod error;
pub mod ffi;
mod message;
mod server;

use std::rc::Rc;

use capnp::capability::FromClientHook;
use capnp::capability::FromServer;
use kj_rs::KjOwn;

pub use crate::error::from_kj;
pub use crate::error::to_kj;
pub use crate::ffi::bridge::ClientHook;
pub use crate::server::Cancellation;

/// Returns a client for `server`, served through KJ. Its calls run to completion even if their
/// callers cancel them, as a C++ server's do without `$Cxx.allowCancellation`.
pub fn new_client<C, S>(server: S) -> C
where
    C: FromServer<S>,
{
    new_client_with_cancellation(server, Cancellation::RunToCompletion)
}

/// Like [`new_client`], with the server's cancellation behaviour stated. Use
/// [`Cancellation::Allow`] for an interface annotated `$Cxx.allowCancellation`.
pub fn new_client_with_cancellation<C, S>(server: S, cancellation: Cancellation) -> C
where
    C: FromServer<S>,
{
    let dispatch = C::from_server(Rc::new(server));
    let hook = ffi::bridge::new_rust_server_client(Box::new(server::RustServer::new(
        dispatch,
        cancellation,
    )));
    client_from_kj(hook)
}

/// Wraps a C++ capability as a typed Rust client.
pub fn client_from_kj<C: FromClientHook>(hook: KjOwn<ClientHook>) -> C {
    C::new(Box::new(client::KjClient::new(hook)))
}

/// Returns the C++ capability behind a typed Rust client.
pub fn client_into_kj<C: FromClientHook>(client: C) -> KjOwn<ClientHook> {
    client::KjClient::into_kj(client.into_client_hook())
}
