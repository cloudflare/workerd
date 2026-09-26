//! The C++ <-> Rust bridge, and all of the crate's `unsafe` code.
//!
//! kj's types are reused from the `kj` crate's bridges, so a `KjOwn<HttpHeaders>` built here is
//! the same type a `kj::http::HeadersRef` borrows.
#![allow(unsafe_code)]

use std::future::Future;
use std::marker::PhantomData;
use std::mem::MaybeUninit;
use std::ops::Deref;
use std::pin::Pin;

pub use bridge::AsyncInputStream;
pub use bridge::AsyncIoStream;
pub use bridge::ConnectResponse;
pub use bridge::HttpHeaderTable;
pub use bridge::HttpHeaders;
pub use bridge::HttpMethod;
pub use bridge::HttpServiceResponse;
pub use bridge::TlsStarterCallback;
pub use bridge::WebSocket;
pub use bridge::WebSocketCompression;
pub use bridge::WebSocketErrorHandler;
pub use bridge::WsCompression;
pub use bridge::WsOffer;
pub use bridge::for_each_header;
pub use bridge::new_body_stream;
pub use bridge::new_connect_response;
pub use bridge::new_rust_io_stream;
pub use bridge::parse_method;
pub use bridge::pump_tunnel;
pub use bridge::tls_starter_set;
pub use bridge::websocket_agreement;
pub use bridge::websocket_offer;
pub use bridge::wrap_tokio_stream;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use kj_rs::OwnTarget;
use tokio::io::ReadBuf;

use crate::body::BodySink;
use crate::body::Head;
use crate::body::RustBody;
use crate::io::RustIo;
use crate::server::ConnectResponder;
use crate::server::ServerResponse;
use crate::starttls::TlsStarter;

#[cxx::bridge(namespace = "workerd::rust::kj_hyper")]
mod bridge {
    /// Agreed permessage-deflate parameters, from this endpoint's view (kj's
    /// `CompressionParameters`); a window of 0 means the default (15).
    #[derive(Debug, Clone)]
    struct WsCompression {
        enabled: bool,
        outbound_no_context_takeover: bool,
        inbound_no_context_takeover: bool,
        outbound_max_window_bits: u8,
        inbound_max_window_bits: u8,
    }

    /// A client's permessage-deflate offer: the `Sec-WebSocket-Extensions` to send (empty for
    /// none) and, when `has_offer`, the parameters of its first offer, which an agreement is
    /// checked against.
    struct WsOffer {
        extensions: String,
        has_offer: bool,
        offer: WsCompression,
    }

    /// How permessage-deflate is negotiated (kj's `WebSocketCompressionMode`): not at all, from
    /// the application's own `Sec-WebSocket-Extensions` header, or offered and agreed by the
    /// library with default parameters.
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    #[repr(u8)]
    enum WebSocketCompression {
        NONE,
        MANUAL,
        AUTOMATIC,
    }

    #[namespace = "kj_rs_io"]
    extern "Rust" {
        type TokioStream = kj_rs_io::TokioStream;
    }

    // kj's types, as the `kj` crate's bridges declare them.
    #[namespace = "kj::rust"]
    unsafe extern "C++" {
        include!("workerd/rust/kj/ffi.h");

        type HttpHeaders = kj::http::ffi::HttpHeaders;
        type HttpHeaderTable = kj::http::ffi::HttpHeaderTable;
        type HttpMethod = kj::http::ffi::HttpMethod;
        type HttpServiceResponse = kj::http::ffi::HttpServiceResponse;
        type ConnectResponse = kj::http::ffi::ConnectResponse;
        type WebSocket = kj::http::ffi::WebSocket;
        type AsyncInputStream = kj::io::ffi::AsyncInputStream;
        type AsyncIoStream = kj::io::ffi::AsyncIoStream;
        type TlsStarterCallback = kj::http::ffi::TlsStarterCallback;
    }

    unsafe extern "C++" {
        include!("kj-hyper/kj-hyper.h");

        /// `kj::WebSocketErrorHandler`: turns a WebSocket protocol error into the exception the
        /// application sees.
        type WebSocketErrorHandler;

        // --- Headers and methods.

        /// Every header, in kj's order, appended to `head`.
        fn for_each_header(headers: &HttpHeaders, head: &mut Head);
        /// `kj::HttpHeaders` from a packed block (body.rs's `HeaderBlock`), checked against the
        /// block's bounds.
        ///
        /// # Safety
        ///
        /// The headers keep a reference to `table`, which the `KjOwn` does not carry: the
        /// caller keeps `table` alive for as long as they exist ([`Borrowing`] does).
        unsafe fn headers_from_block(
            table: &HttpHeaderTable,
            arena: &[u8],
            lens: &[u32],
        ) -> Result<KjOwn<HttpHeaders>>;
        /// kj's parse of a request method; false for one kj has no name for.
        fn parse_method(name: &[u8], method: &mut HttpMethod) -> bool;

        // --- Rust objects behind kj interfaces.

        fn wrap_tokio_stream(stream: Box<TokioStream>) -> KjOwn<AsyncIoStream>;
        fn new_rust_io_stream(io: Box<RustIo>) -> KjOwn<AsyncIoStream>;
        fn new_body_stream(body: Box<RustBody>) -> KjOwn<AsyncInputStream>;
        /// `kj::HttpService::Response` for a request of `method` with `headers`, which decide
        /// whether it is a WebSocket handshake and whether that handshake is acceptable.
        ///
        /// # Safety
        ///
        /// The response, and a WebSocket it accepts, keep the reference in `errors`, which the
        /// `KjOwn` does not carry: the caller keeps the handler alive for as long as they exist
        /// ([`Borrowing`] does).
        unsafe fn new_server_response(
            response: Box<ServerResponse>,
            method: HttpMethod,
            headers: &HttpHeaders,
            compression: WebSocketCompression,
            errors: KjMaybe<&WebSocketErrorHandler>,
        ) -> KjOwn<HttpServiceResponse>;
        fn new_connect_response(response: Box<ConnectResponder>) -> KjOwn<ConnectResponse>;
        /// kj's `WebSocket` for the client end of an upgraded connection, masking its frames
        /// and running the agreed compression.
        ///
        /// # Safety
        ///
        /// The WebSocket keeps the reference in `errors`, which the `KjOwn` does not carry: the
        /// caller keeps the handler alive for as long as it exists ([`Borrowing`] does).
        unsafe fn new_client_websocket(
            stream: KjOwn<AsyncIoStream>,
            compression: &WsCompression,
            errors: KjMaybe<&WebSocketErrorHandler>,
        ) -> KjOwn<WebSocket>;

        // --- kj's own plumbing, for the client.

        /// Pumps messages both ways until either side closes, as kj's client adapter does.
        async fn pump_websockets(a: KjOwn<WebSocket>, b: KjOwn<WebSocket>) -> Result<()>;
        /// Pumps bytes both ways, shutting each write side down when the other side ends.
        async fn pump_tunnel(
            connection: Pin<&mut AsyncIoStream>,
            tunnel: KjOwn<AsyncIoStream>,
        ) -> Result<()>;
        /// Fills a connect's `kj::TlsStarterCallback` (an output of `connect()`) with `starter`.
        fn tls_starter_set(starter: Pin<&mut TlsStarterCallback>, start: Box<TlsStarter>);
        /// The permessage-deflate offer a client sends, in the given mode (kj's `HttpClient`).
        fn websocket_offer(headers: &HttpHeaders, mode: WebSocketCompression) -> WsOffer;
        /// What a server's `Sec-WebSocket-Extensions` answer agrees to, checked against the
        /// offer; fails on an agreement the offer does not cover.
        fn websocket_agreement(
            offer: &WsOffer,
            response: &HttpHeaders,
            mode: WebSocketCompression,
        ) -> Result<WsCompression>;
    }

    // Held as `KjOwn<WebSocketErrorHandler>` in the settings, outside the bridge's signatures.
    impl KjOwn<WebSocketErrorHandler> {}

    extern "Rust" {
        type Head;
        fn append(self: &mut Head, name: &[u8], value: &[u8]);

        type RustIo;
        async unsafe fn rust_io_read(
            io: &RustIo,
            buffer: *mut u8,
            len: usize,
            min_bytes: usize,
        ) -> Result<usize>;
        async unsafe fn write<'a>(self: &'a RustIo, data: &'a [u8]) -> Result<()>;
        async fn shutdown_write(self: &RustIo) -> Result<()>;
        fn abort_read(self: &RustIo);

        type RustBody;
        async unsafe fn body_read(
            body: &RustBody,
            buffer: *mut u8,
            len: usize,
            min_bytes: usize,
        ) -> Result<usize>;
        fn body_length(body: &RustBody) -> KjMaybe<u64>;

        type BodySink;
        async fn write(self: &BodySink, data: &[u8]) -> Result<()>;
        async fn when_write_disconnected(self: &BodySink);

        /// Random bytes for kj's frame masks (its `EntropySource`).
        fn fill_random(buffer: &mut [u8]);

        type ServerResponse;
        fn send(
            self: &ServerResponse,
            status: u32,
            status_text: &[u8],
            headers: &HttpHeaders,
            length: KjMaybe<u64>,
        ) -> Result<Box<BodySink>>;
        fn send_error(
            self: &ServerResponse,
            status: u32,
            status_text: &[u8],
            message: &[u8],
        ) -> Result<()>;
        /// Answers the handshake and hands back the upgraded connection, for kj's `WebSocket`.
        fn accept_websocket(
            self: &ServerResponse,
            headers: &HttpHeaders,
            extensions: &[u8],
        ) -> Result<Box<RustIo>>;

        type TlsStarter;
        async fn start(self: &TlsStarter, expected_server_hostname: &[u8]) -> Result<()>;

        type ConnectResponder;
        fn accept(
            self: &ConnectResponder,
            status: u32,
            status_text: &[u8],
            headers: &HttpHeaders,
        ) -> Result<()>;
        fn reject(
            self: &ConnectResponder,
            status: u32,
            status_text: &[u8],
            headers: &HttpHeaders,
            length: KjMaybe<u64>,
        ) -> Result<Box<BodySink>>;
    }
}

// =======================================================================================
// kj objects that borrow

/// A `kj::Own<T>` whose object holds a reference into something Rust owns for `'a`.
///
/// That is the `kj::HttpHeaderTable` behind `kj::HttpHeaders`, or the [`WebSocketErrorHandler`]
/// behind a server response or a WebSocket. The bridge functions that build such objects are
/// `unsafe`, because a bare `KjOwn` does not carry that borrow; this module's wrappers are their
/// only callers, and hand the object out bounded by it.
pub struct Borrowing<'a, T: OwnTarget> {
    own: KjOwn<T>,
    borrow: PhantomData<&'a ()>,
}

impl<T: OwnTarget> Borrowing<'_, T> {
    pub fn as_mut(&mut self) -> Pin<&mut T> {
        self.own.as_mut()
    }
}

impl<T: OwnTarget> Deref for Borrowing<'_, T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.own
    }
}

/// `kj::HttpHeaders` over `table` from a packed block ([`crate::body::HeaderBlock`]).
///
/// # Errors
///
/// A block whose lengths do not describe its bytes.
pub fn headers_from_block<'t>(
    table: &'t HttpHeaderTable,
    arena: &[u8],
    lens: &[u32],
) -> crate::Result<Borrowing<'t, HttpHeaders>> {
    // SAFETY: the result is bounded by `'t`, the borrow of `table` the headers keep.
    let own = unsafe { bridge::headers_from_block(table, arena, lens) }?;
    Ok(Borrowing {
        own,
        borrow: PhantomData,
    })
}

/// `kj::HttpService::Response` for a request of `method` with `headers`.
///
/// The headers decide whether the request is a WebSocket handshake and whether that handshake is
/// acceptable. A WebSocket the response accepts reports protocol errors through `errors`.
pub fn new_server_response<'e>(
    response: Box<ServerResponse>,
    method: HttpMethod,
    headers: &HttpHeaders,
    compression: WebSocketCompression,
    errors: KjMaybe<&'e WebSocketErrorHandler>,
) -> Borrowing<'e, HttpServiceResponse> {
    // SAFETY: the result is bounded by `'e`, the borrow of `errors` the response keeps. The
    // WebSocket it accepts keeps the same borrow and reaches the application only through the
    // response (`kj::HttpService::Response::acceptWebSocket`), within the call that has it.
    let own =
        unsafe { bridge::new_server_response(response, method, headers, compression, errors) };
    Borrowing {
        own,
        borrow: PhantomData,
    }
}

/// kj's `WebSocket` for the client end of an upgraded connection, masking its frames and running
/// the agreed compression; it reports protocol errors through `errors`.
pub fn new_client_websocket<'e>(
    stream: KjOwn<AsyncIoStream>,
    compression: &WsCompression,
    errors: KjMaybe<&'e WebSocketErrorHandler>,
) -> Borrowing<'e, WebSocket> {
    // SAFETY: the result is bounded by `'e`, the borrow of `errors` the WebSocket keeps.
    let own = unsafe { bridge::new_client_websocket(stream, compression, errors) };
    Borrowing {
        own,
        borrow: PhantomData,
    }
}

/// Pumps messages both ways until either side closes, as kj's client adapter does. The pump
/// owns both sockets, so `ours` lives no longer than its borrow allows.
///
/// # Errors
///
/// Whatever fails either socket.
pub async fn pump_websockets(
    ours: Borrowing<'_, WebSocket>,
    theirs: KjOwn<WebSocket>,
) -> crate::Result<()> {
    Ok(bridge::pump_websockets(ours.own, theirs).await?)
}

// =======================================================================================
// Bridge entry points

/// # Safety
///
/// `ptr` must be valid for `len` bytes, untouched by anyone else, until the future settles
/// (kj's `tryRead` contract).
unsafe fn uninit<'a>(ptr: *mut u8, len: usize) -> ReadBuf<'a> {
    if len == 0 {
        return ReadBuf::uninit(&mut []);
    }
    // SAFETY: per the contract; `MaybeUninit<u8>` has no validity requirement.
    ReadBuf::uninit(unsafe { std::slice::from_raw_parts_mut(ptr.cast::<MaybeUninit<u8>>(), len) })
}

/// # Safety
///
/// As for [`uninit`].
unsafe fn rust_io_read<'a>(
    io: &RustIo,
    ptr: *mut u8,
    len: usize,
    min_bytes: usize,
) -> impl Future<Output = crate::Result<usize>> + 'a {
    // SAFETY: forwarded.
    io.read(unsafe { uninit(ptr, len) }, min_bytes)
}

/// # Safety
///
/// As for [`uninit`].
unsafe fn body_read<'a>(
    body: &RustBody,
    ptr: *mut u8,
    len: usize,
    min_bytes: usize,
) -> impl Future<Output = crate::Result<usize>> + 'a {
    // SAFETY: forwarded.
    body.read(unsafe { uninit(ptr, len) }, min_bytes)
}

fn body_length(body: &RustBody) -> KjMaybe<u64> {
    body.length().into()
}

fn fill_random(buffer: &mut [u8]) {
    crate::handshake::fill_random(buffer);
}
