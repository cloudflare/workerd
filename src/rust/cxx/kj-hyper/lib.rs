//! hyper, rustls and tungstenite's frame codec behind workerd's kj HTTP interfaces.
//!
//! - `io`: kj streams as tokio streams (native sockets taken out, foreign streams driven
//!   directly) and tokio streams as kj streams.
//! - `body`: HTTP message bodies in both directions, and header conversion.
//! - `server`: one accepted connection served by hyper, dispatching to a C++ `kj::HttpService`.
//! - `client`: hyper-util's pooling client (dialing through a C++ connector) and a
//!   single-connection client over one kj stream.
//! - `ws`: `kj::WebSocket` on a stream: kj's frame loop semantics over tungstenite's frame codec.
//! - `deflate`: permessage-deflate codecs for `ws`.
//! - `tls`: rustls configs from workerd's `TlsOptions`, and TLS over a kj stream.
//!
//! All `unsafe` code lives in `ffi.rs`.

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
mod client;
mod deflate;
pub mod ffi;
mod io;
mod server;
mod tls;
mod ws;
