//! HTTP/1.1, `WebSocket` and TLS for a Rust server whose handlers are kj HTTP interfaces: hyper
//! and rustls on the Rust side, `kj::HttpService` and friends on the C++ side.
//!
//! - [`server`]: `serve_connection(io, Hangup, &HeaderTable, Rc<ServerSettings>,
//!   Rc<dyn Handler>, &Shutdown)` serves one accepted tokio connection with hyper, each request
//!   dispatched to a [`server::Handler`] with kj-typed arguments: [`server::Handler::request`]
//!   takes exactly what `kj::http::Service::request` takes (`kj::http::HeadersRef`,
//!   `kj::io::AsyncInputStream`, `kj::http::ServiceResponse`), so it can be forwarded straight to
//!   a C++ `WorkerInterface`, and [`server::Handler::connect`] takes a [`server::Connect`] that is
//!   answered in Rust ([`server::Connect::accept`] hands back the tunnel as tokio I/O,
//!   [`server::Connect::reject`]) or handed to C++ ([`server::Connect::into_kj`]). A call that
//!   fails unanswered gets a bare 500.
//! - [`client`]: [`client::Client`], a pooled HTTP/1.1 client over hyper-util's legacy client
//!   implementing `kj::http::Service`, so C++ can make outbound requests through it.
//!   `Client::new(table, settings, peer, dial)` pools connections to one peer through a dialer
//!   and asks it for paths ([`client::Peer::Origin`]) or, of an HTTP proxy, for absolute URLs
//!   sent whole ([`client::Peer::Proxy`]), and its `tunnel(host)` is an HTTP CONNECT through that
//!   peer, handed to C++ as a `kj::AsyncIoStream`; `Client::internet(table, settings, tls,
//!   connect)` reaches whatever authority a request's URL names through `connect(host, port)`,
//!   the caller's connection to it (`connect_allowed(host, port, allow)` is the usual one:
//!   resolve, then the first address `allow` admits that accepts).
//! - `body`: message bodies in both directions, and outgoing headers laid out as kj writes them.
//! - [`tls`]: rustls configurations from workerd's `TlsOptions`, and the handshakes.
//! - `handshake`: the WebSocket handshake's computed header values. The `WebSocket` itself is
//!   kj's (`kj::newWebSocket`) over the upgraded connection, built on the C++ side.
//! - [`into_kj_stream`] / [`into_kj_stream_with`]: tokio streams handed to C++ as
//!   `kj::AsyncIoStream`.
//!
//! kj-hyper is used from Rust; its C++ (kj-hyper.c++) exists only to implement kj interfaces over
//! Rust objects, and all of its `unsafe` code lives in `ffi.rs`.
//!
//! # Threads
//!
//! Everything is single-threaded state (`Rc`, `RefCell`, `Cell`), bound to the tokio runtime
//! thread it was created on, as kj's own HTTP objects are bound to their event loop's thread.
//! Transports must be `Send` (hyper's upgrade path and the legacy client's connection tasks
//! require it); handlers, settings and header tables need not be. hyper's timers (header timeout,
//! pool idle eviction) and the client's connection tasks (`tokio::spawn`) need the thread's tokio
//! runtime entered.
//!
//! # Lifetimes
//!
//! A `HeaderTable` and [`server::ServerSettings`]/[`client::ClientSettings`] outlive the futures
//! made with them (the server borrows the table; a [`WebSocketErrorHandler`] in the settings is
//! borrowed by every response object built while a call runs). What a handler call borrows from
//! C++ -- a `tryRead` buffer, header slices -- is tied to the future's lifetime on the bridge;
//! Rust objects behind kj interfaces (`RustIo`, `RustBody`) are `Rc`s whose operations own a
//! share, so a `kj::Own` dropped mid-operation dangles nothing.
//!
//! # Dropped kj settings
//!
//! No `HttpServerErrorHandler` or `HttpClientErrorHandler`, no caller-supplied `EntropySource`
//! (handshake keys and frame masks come from `rand`), no pipeline timeout or cancelled-upload
//! grace period, and no cipher list (`TlsOptions.cipherList` is not applied; see [`tls`]); hyper
//! answers unparsable requests itself (see [`server`]).

#![deny(clippy::absolute_paths)]

mod body;
pub mod client;
pub mod ffi;
mod handshake;
mod io;
pub mod server;
mod starttls;
pub mod tls;

use std::result;

pub use body::HeaderBlock;
use cxx::KjError;
pub use ffi::WebSocketCompression;
pub use ffi::WebSocketErrorHandler;
pub use handshake::accept_key;
pub use io::Hangup;
pub use io::into_kj_stream;
pub use io::into_kj_stream_with;
pub use io::io_kj_error;

/// `kj::Result` unless an error type is given, so it can stand in for `std::result::Result`.
pub type Result<T, E = KjError> = result::Result<T, E>;
