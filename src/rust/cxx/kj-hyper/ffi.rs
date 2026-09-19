//! The C++ <-> Rust bridge, and all of the crate's `unsafe` code.
#![allow(unsafe_code)]

use std::future::Future;
use std::mem::MaybeUninit;

pub use bridge::*;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use tokio::io::ReadBuf;

use crate::body::BodySink;
use crate::body::Head;
use crate::body::HeaderBlock;
use crate::body::RustBody;
use crate::client::ClientRequest;
use crate::client::ClientResponse;
use crate::client::HyperClient;
use crate::io::RustIo;
use crate::server::ConnectResponder;
use crate::server::HyperConnection;
use crate::server::ServerResponse;
use crate::tls::TlsClientConfig;
use crate::tls::TlsServerConfig;
use crate::tls::new_tls_client_config;
use crate::tls::new_tls_server_config;
use crate::ws::RustWebSocket;

#[expect(
    clippy::needless_lifetimes,
    reason = "cxx requires explicit lifetimes on async fns"
)]
#[expect(
    clippy::missing_safety_doc,
    reason = "cxx-generated wrappers do not carry the declarations' docs"
)]
#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx passes opaque Rust types by Box"
)]
#[cxx::bridge(namespace = "workerd::rust::kj_hyper")]
mod bridge {
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    #[repr(u8)]
    enum WsMessageKind {
        TEXT,
        BINARY,
        CLOSE,
        // `data` is the description, `close_code` the status (kj's `WebSocket::ProtocolError`).
        PROTOCOL_ERROR,
    }

    struct WsMessage {
        kind: WsMessageKind,
        data: Vec<u8>,
        close_code: u16,
    }

    /// Agreed permessage-deflate parameters, from this endpoint's view (kj's
    /// `CompressionParameters`); a window of 0 means the default (15).
    struct WsCompression {
        enabled: bool,
        outbound_no_context_takeover: bool,
        inbound_no_context_takeover: bool,
        outbound_max_window_bits: u8,
        inbound_max_window_bits: u8,
    }

    /// How a pooling client's connection to an authority is dialed.
    #[derive(Debug, PartialEq, Eq, Hash, Copy, Clone)]
    #[repr(u8)]
    enum Scheme {
        HTTP,
        HTTPS,
    }

    /// The subset of workerd's `config::TlsOptions` this backend uses.
    struct TlsOptions {
        trust_system_roots: bool,
        trusted_certificates: Vec<String>,
        certificate_chain: String,
        private_key: String,
        require_client_certs: bool,
        /// `minVersion = tls1Dot3`.
        min_tls13: bool,
        cipher_list: String,
    }

    #[namespace = "kj_rs_io"]
    extern "Rust" {
        type TokioStream = kj_rs_io::TokioStream;
    }

    unsafe extern "C++" {
        include!("kj-hyper/kj-hyper.h");

        // kj's types, aliased into this namespace (a nested class has no cxx path of its own).
        type HttpHeaders;
        type AsyncIoStream;

        // Taking streams apart (io.rs).
        #[cxx_name = "isTokioStream"]
        fn is_tokio_stream(stream: &AsyncIoStream) -> bool;
        #[cxx_name = "releaseTokioStream"]
        fn release_tokio_stream(stream: KjOwn<AsyncIoStream>) -> Box<TokioStream>;
        #[cxx_name = "wrapTokioStream"]
        fn wrap_tokio_stream(stream: Box<TokioStream>) -> KjOwn<AsyncIoStream>;
        #[cxx_name = "isReleasableRustIo"]
        fn is_releasable_rust_io(stream: &AsyncIoStream) -> bool;
        #[cxx_name = "releaseRustIo"]
        fn release_rust_io(stream: KjOwn<AsyncIoStream>) -> Box<RustIo>;

        // The directions of a foreign kj stream (io.rs's KjIo).
        type KjStreamReadEnd;
        type KjStreamWriteEnd;
        #[cxx_name = "kjStreamReadEnd"]
        fn kj_stream_read_end(stream: KjOwn<AsyncIoStream>) -> KjOwn<KjStreamReadEnd>;
        #[cxx_name = "kjStreamWriteEnd"]
        fn kj_stream_write_end(read: Pin<&mut KjStreamReadEnd>) -> KjOwn<KjStreamWriteEnd>;
        #[cxx_name = "kjReadEndTryRead"]
        async unsafe fn kj_read_end_try_read<'a>(
            end: Pin<&'a mut KjStreamReadEnd>,
            buffer: &'a mut [u8],
            min_bytes: usize,
        ) -> Result<usize>;
        #[cxx_name = "kjReadEndShare"]
        fn kj_read_end_share(end: Pin<&mut KjStreamReadEnd>) -> KjOwn<KjStreamReadEnd>;
        #[cxx_name = "kjReadEndWhenWriteDisconnected"]
        async unsafe fn kj_read_end_when_write_disconnected<'a>(
            end: Pin<&'a mut KjStreamReadEnd>,
        ) -> Result<()>;
        #[cxx_name = "kjWriteEndWrite"]
        async unsafe fn kj_write_end_write<'a>(
            end: Pin<&'a mut KjStreamWriteEnd>,
            buffer: &'a [u8],
        ) -> Result<()>;
        #[cxx_name = "kjWriteEndShutdownWrite"]
        fn kj_write_end_shutdown_write(end: Pin<&mut KjStreamWriteEnd>) -> Result<()>;

        // kj headers, one at a time into `head` (Head::append).
        #[cxx_name = "forEachHeader"]
        fn for_each_header(headers: &HttpHeaders, head: &mut Head);

        // A connection's per-request context: kj's HttpServer around the application (its error
        // handlers and WebSocket settings), shared by every request on the connection. Headers
        // arrive packed (body.rs's HeaderBlock) and are copied before the call returns.
        type HttpDispatcher;
        #[cxx_name = "dispatchRequest"]
        async unsafe fn dispatcher_request<'a>(
            dispatcher: &'a HttpDispatcher,
            method: &'a [u8],
            url: &'a [u8],
            header_arena: &'a [u8],
            header_lens: &'a [u32],
            body: Box<RustBody>,
            response: Box<ServerResponse>,
        ) -> Result<()>;
        #[cxx_name = "dispatchConnect"]
        async unsafe fn dispatcher_connect<'a>(
            dispatcher: &'a HttpDispatcher,
            host: &'a [u8],
            header_arena: &'a [u8],
            header_lens: &'a [u32],
            tunnel: Box<RustIo>,
            response: Box<ConnectResponder>,
        ) -> Result<()>;

        // Dialing for the pooling client: a kj Network or NetworkAddress. Connectors are shared:
        // many dials may be in flight on one.
        type HttpConnector;
        #[cxx_name = "connectorConnect"]
        async unsafe fn connector_connect<'a>(
            connector: &'a HttpConnector,
            authority: &'a [u8],
            scheme: Scheme,
        ) -> Result<KjOwn<AsyncIoStream>>;
    }

    extern "Rust" {
        type Head;
        fn append(self: &mut Head, name: &[u8], value: &[u8]);

        type RustIo;
        async unsafe fn rust_io_read<'a>(
            io: &'a RustIo,
            buffer: *mut u8,
            len: usize,
            min_bytes: usize,
        ) -> Result<usize>;
        async unsafe fn write<'a>(self: &'a RustIo, data: &'a [u8]) -> Result<()>;
        async unsafe fn shutdown_write<'a>(self: &'a RustIo) -> Result<()>;
        fn abort_read(self: &RustIo);
        fn can_release(self: &RustIo) -> bool;
        async unsafe fn rust_io_when_write_disconnected<'a>(io: &'a RustIo) -> Result<()>;

        type RustBody;
        async unsafe fn body_read<'a>(
            body: &'a RustBody,
            buffer: *mut u8,
            len: usize,
            min_bytes: usize,
        ) -> Result<usize>;
        fn body_length(body: &RustBody) -> KjMaybe<u64>;

        type BodySink;
        async unsafe fn write<'a>(self: &'a BodySink, data: &'a [u8]) -> Result<()>;
        async unsafe fn when_write_disconnected<'a>(self: &'a BodySink);

        type RustWebSocket;
        async unsafe fn send<'a>(
            self: &'a RustWebSocket,
            is_text: bool,
            data: &'a [u8],
        ) -> Result<()>;
        async unsafe fn close<'a>(
            self: &'a RustWebSocket,
            code: u16,
            reason: &'a [u8],
        ) -> Result<()>;
        async unsafe fn receive<'a>(self: &'a RustWebSocket, max_size: u64) -> Result<WsMessage>;
        async unsafe fn disconnect<'a>(self: &'a RustWebSocket);
        fn abort(self: &RustWebSocket);
        async unsafe fn when_aborted<'a>(self: &'a RustWebSocket) -> Result<()>;
        fn sent_byte_count(self: &RustWebSocket) -> u64;
        fn received_byte_count(self: &RustWebSocket) -> u64;

        type HyperClient;
        fn new_pooled_client(
            connector: KjOwn<HttpConnector>,
            idle_timeout_ms: u64,
        ) -> Box<HyperClient>;
        fn new_single_client(stream: KjOwn<AsyncIoStream>) -> Box<HyperClient>;
        fn new_single_client_over_lent_stream(stream: KjOwn<AsyncIoStream>) -> Box<HyperClient>;
        async unsafe fn drive<'a>(self: &'a HyperClient);
        fn request(
            self: &HyperClient,
            authority: &str,
            scheme: Scheme,
            method: &str,
            url: &str,
            headers: &HttpHeaders,
            length: KjMaybe<u64>,
        ) -> Result<Box<ClientRequest>>;
        fn open_websocket(
            self: &HyperClient,
            authority: &str,
            scheme: Scheme,
            url: &str,
            headers: &HttpHeaders,
            key: &str,
            extensions: &[u8],
        ) -> Result<Box<ClientRequest>>;
        fn connect(
            self: &HyperClient,
            host: &str,
            headers: &HttpHeaders,
        ) -> Result<Box<ClientRequest>>;

        type ClientRequest;
        fn take_body_sink(self: &mut ClientRequest) -> Result<Box<BodySink>>;
        async unsafe fn response<'a>(self: &'a ClientRequest) -> Result<Box<ClientResponse>>;

        type ClientResponse;
        fn status_code(self: &ClientResponse) -> u32;
        unsafe fn status_text<'a>(self: &'a ClientResponse) -> &'a [u8];
        unsafe fn header_arena<'a>(self: &'a ClientResponse) -> &'a [u8];
        unsafe fn header_lens<'a>(self: &'a ClientResponse) -> &'a [u32];
        unsafe fn websocket_handshake_error<'a>(self: &'a ClientResponse) -> &'a str;
        fn take_body(self: &ClientResponse) -> Box<RustBody>;
        fn take_websocket(
            self: &ClientResponse,
            compression: &WsCompression,
        ) -> Result<Box<RustWebSocket>>;
        fn take_tunnel(self: &ClientResponse) -> Result<Box<RustIo>>;

        type HyperConnection;
        fn new_connection(
            stream: KjOwn<AsyncIoStream>,
            header_timeout_ms: u64,
        ) -> Box<HyperConnection>;
        fn new_connection_over_lent_stream(
            stream: KjOwn<AsyncIoStream>,
            header_timeout_ms: u64,
        ) -> Box<HyperConnection>;
        async unsafe fn serve<'a>(
            self: &'a HyperConnection,
            dispatcher: &'a HttpDispatcher,
        ) -> Result<()>;
        fn shutdown(self: &HyperConnection);

        type ServerResponse;
        fn send(
            self: &ServerResponse,
            status: u32,
            status_text: &[u8],
            headers: &HttpHeaders,
            length: KjMaybe<u64>,
        ) -> Result<Box<BodySink>>;
        fn accept_websocket(
            self: &ServerResponse,
            headers: &HttpHeaders,
            extensions: &[u8],
            compression: &WsCompression,
        ) -> Result<Box<RustWebSocket>>;
        fn close_after_send(self: &ServerResponse);

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
        fn error_response(self: &ConnectResponder) -> Box<ServerResponse>;

        type TlsClientConfig;
        type TlsServerConfig;
        fn new_tls_client_config(options: &TlsOptions) -> Result<Box<TlsClientConfig>>;
        fn new_tls_server_config(options: &TlsOptions) -> Result<Box<TlsServerConfig>>;
        async unsafe fn wrap_tls_client<'a>(
            stream: KjOwn<AsyncIoStream>,
            config: &'a TlsClientConfig,
            hostname: &'a str,
        ) -> Result<Box<RustIo>>;
        async unsafe fn wrap_tls_server<'a>(
            stream: KjOwn<AsyncIoStream>,
            config: &'a TlsServerConfig,
        ) -> Result<Box<RustIo>>;
    }
}

// =======================================================================================
// Foreign kj stream directions

pub struct KjStreamReadHalf(KjOwn<KjStreamReadEnd>);
pub struct KjStreamWriteHalf(KjOwn<KjStreamWriteEnd>);

pub fn split_kj_stream(stream: KjOwn<AsyncIoStream>) -> (KjStreamReadHalf, KjStreamWriteHalf) {
    let mut read = kj_stream_read_end(stream);
    let write = kj_stream_write_end(read.as_mut());
    (KjStreamReadHalf(read), KjStreamWriteHalf(write))
}

impl KjStreamReadHalf {
    /// The stream's `whenWriteDisconnected`, owning its share of the stream.
    pub fn when_write_disconnected(&mut self) -> impl Future<Output = crate::Result<()>> + 'static {
        let mut end = kj_read_end_share(self.0.as_mut());
        async move {
            // SAFETY: `end` is owned by this future and exclusively borrowed for the call.
            Ok(unsafe { kj_read_end_when_write_disconnected(end.as_mut()) }.await?)
        }
    }

    pub async fn try_read(
        &mut self,
        buf: &mut [u8],
        min_bytes: usize,
    ) -> Result<usize, cxx::KjException> {
        // SAFETY: the end and the buffer are exclusively borrowed for the whole call.
        unsafe { kj_read_end_try_read(self.0.as_mut(), buf, min_bytes) }.await
    }
}

impl KjStreamWriteHalf {
    pub async fn write(&mut self, buf: &[u8]) -> Result<(), cxx::KjException> {
        // SAFETY: as for `try_read`.
        unsafe { kj_write_end_write(self.0.as_mut(), buf) }.await
    }

    pub fn shutdown_write(&mut self) -> Result<(), cxx::KjException> {
        kj_write_end_shutdown_write(self.0.as_mut())
    }
}

/// Dispatches a request to the connection's service. The arguments are borrowed for as long as
/// the returned future, which owns the promise.
pub fn dispatch_request<'a>(
    dispatcher: &'a HttpDispatcher,
    method: &'a [u8],
    url: &'a [u8],
    headers: &'a HeaderBlock,
    body: Box<RustBody>,
    response: Box<ServerResponse>,
) -> impl Future<Output = Result<(), cxx::KjException>> + 'a {
    // SAFETY: as the function docs.
    unsafe {
        dispatcher_request(
            dispatcher,
            method,
            url,
            &headers.arena,
            &headers.lens,
            body,
            response,
        )
    }
}

/// Dispatches a CONNECT request to the connection's service, as [`dispatch_request`].
pub fn dispatch_connect<'a>(
    dispatcher: &'a HttpDispatcher,
    host: &'a [u8],
    headers: &'a HeaderBlock,
    tunnel: Box<RustIo>,
    response: Box<ConnectResponder>,
) -> impl Future<Output = Result<(), cxx::KjException>> + 'a {
    // SAFETY: as for `dispatch_request`.
    unsafe {
        dispatcher_connect(
            dispatcher,
            host,
            &headers.arena,
            &headers.lens,
            tunnel,
            response,
        )
    }
}

/// Dials through a connector the caller keeps alive for as long as the returned future.
pub async fn connect(
    connector: &HttpConnector,
    authority: &[u8],
    scheme: Scheme,
) -> crate::Result<KjOwn<AsyncIoStream>> {
    // SAFETY: the arguments are borrowed for as long as the promise this future awaits.
    Ok(unsafe { connector_connect(connector, authority, scheme) }.await?)
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

fn rust_io_when_write_disconnected(io: &RustIo) -> crate::io::Hangup {
    io.when_write_disconnected()
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

#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx passes opaque Rust types by Box"
)]
fn new_pooled_client(connector: KjOwn<HttpConnector>, idle_timeout_ms: u64) -> Box<HyperClient> {
    Box::new(HyperClient::pooled(connector, idle_timeout_ms))
}

#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx passes opaque Rust types by Box"
)]
fn new_single_client(stream: KjOwn<AsyncIoStream>) -> Box<HyperClient> {
    Box::new(HyperClient::single(crate::io::kj_to_tokio(stream)))
}

/// The stream (a non-owning `kj::Own`) must outlive the client.
#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx passes opaque Rust types by Box"
)]
fn new_single_client_over_lent_stream(stream: KjOwn<AsyncIoStream>) -> Box<HyperClient> {
    Box::new(HyperClient::single(crate::io::drive_kj_stream(stream)))
}

#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx passes opaque Rust types by Box"
)]
fn new_connection(stream: KjOwn<AsyncIoStream>, header_timeout_ms: u64) -> Box<HyperConnection> {
    let (io, hangup) = crate::io::kj_to_tokio(stream);
    Box::new(HyperConnection::new(
        io,
        hangup,
        std::time::Duration::from_millis(header_timeout_ms),
    ))
}

/// The stream (a non-owning `kj::Own`) must outlive the connection.
#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx passes opaque Rust types by Box"
)]
fn new_connection_over_lent_stream(
    stream: KjOwn<AsyncIoStream>,
    header_timeout_ms: u64,
) -> Box<HyperConnection> {
    let (io, hangup) = crate::io::drive_kj_stream(stream);
    Box::new(HyperConnection::new(
        io,
        hangup,
        std::time::Duration::from_millis(header_timeout_ms),
    ))
}

/// # Safety
///
/// `config` and `hostname` outlive the future.
unsafe fn wrap_tls_client<'a>(
    stream: KjOwn<AsyncIoStream>,
    config: &'a TlsClientConfig,
    hostname: &'a str,
) -> impl Future<Output = crate::Result<Box<RustIo>>> + 'a {
    crate::tls::wrap_tls_client(stream, config, hostname.to_owned())
}

/// # Safety
///
/// `config` outlives the future.
unsafe fn wrap_tls_server(
    stream: KjOwn<AsyncIoStream>,
    config: &TlsServerConfig,
) -> impl Future<Output = crate::Result<Box<RustIo>>> + '_ {
    crate::tls::wrap_tls_server(stream, config)
}
