//! The C++ <-> Rust bridge, and all of the crate's `unsafe` code.
#![allow(unsafe_code)]

use std::future::Future;
use std::mem::MaybeUninit;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

pub use bridge::*;
use kj::http::HeaderTable;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use tokio::io::ReadBuf;

use crate::body::BodySink;
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
use crate::tls::wrap_tls_server;
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

    /// Why a request cannot be accepted as a WebSocket (RFC 6455 section 4.2.1).
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    #[repr(u8)]
    enum WsRejection {
        NONE,
        /// No `Upgrade: websocket`: not a handshake at all.
        NOT_UPGRADE,
        NOT_GET,
        UNSUPPORTED_VERSION,
        MISSING_KEY,
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

    #[namespace = "kj::rust"]
    unsafe extern "C++" {
        include!("kj-rs-http/ffi.h");
        type HttpHeaderTable = kj::http::ffi::HttpHeaderTable;
        type HttpHeaders = kj::http::ffi::HttpHeaders;
        type HttpService = kj::http::ffi::HttpService;
        type HttpServiceResponse = kj::http::ffi::HttpServiceResponse;
        type ConnectResponse = kj::http::ffi::ConnectResponse;
        type AsyncInputStream = kj::io::ffi::AsyncInputStream;
        type AsyncIoStream = kj::io::ffi::AsyncIoStream;
    }

    #[namespace = "kj_rs_io"]
    extern "Rust" {
        type TokioStream = kj_rs_io::TokioStream;
    }

    unsafe extern "C++" {
        include!("kj-hyper/kj-hyper.h");

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
        #[cxx_name = "kjReadEndWhenWriteDisconnected"]
        fn kj_read_end_when_write_disconnected(end: &KjStreamReadEnd) -> KjOwn<KjStreamReadEnd>;
        #[cxx_name = "kjShareWhenWriteDisconnected"]
        async unsafe fn kj_share_when_write_disconnected<'a>(
            end: Pin<&'a mut KjStreamReadEnd>,
        ) -> Result<()>;
        #[cxx_name = "kjWriteEndWrite"]
        async unsafe fn kj_write_end_write<'a>(
            end: Pin<&'a mut KjStreamWriteEnd>,
            buffer: &'a [u8],
        ) -> Result<()>;
        #[cxx_name = "kjWriteEndShutdownWrite"]
        fn kj_write_end_shutdown_write(end: Pin<&mut KjStreamWriteEnd>) -> Result<()>;

        // Rust objects as kj interfaces.
        #[cxx_name = "newRustIoStream"]
        fn new_rust_io_stream(io: Box<RustIo>) -> KjOwn<AsyncIoStream>;
        #[cxx_name = "newBodyStream"]
        fn new_body_stream(body: Box<RustBody>) -> KjOwn<AsyncInputStream>;
        #[cxx_name = "newServerResponse"]
        fn new_server_response(response: Box<ServerResponse>) -> KjOwn<HttpServiceResponse>;
        #[cxx_name = "newConnectResponse"]
        fn new_connect_response(response: Box<ConnectResponder>) -> KjOwn<ConnectResponse>;

        // The C++ service (shared-reentrant: many calls may be in flight on one service).
        #[cxx_name = "serviceRequest"]
        async fn service_request(
            service: &HttpService,
            method: &[u8],
            url: &[u8],
            headers: &HttpHeaders,
            body: Pin<&mut AsyncInputStream>,
            response: Pin<&mut HttpServiceResponse>,
        ) -> Result<()>;
        #[cxx_name = "serviceConnect"]
        async fn service_connect(
            service: &HttpService,
            host: &[u8],
            headers: &HttpHeaders,
            connection: Pin<&mut AsyncIoStream>,
            response: Pin<&mut ConnectResponse>,
        ) -> Result<()>;

        // Dialing for the pooling client: a kj Network or NetworkAddress.
        type HttpConnector;
        type Dialed;
        #[cxx_name = "newDialed"]
        fn new_dialed() -> KjOwn<Dialed>;
        #[cxx_name = "connectorConnect"]
        async unsafe fn connector_connect<'a>(
            connector: &'a HttpConnector,
            authority: &'a [u8],
            https: bool,
            out: Pin<&'a mut Dialed>,
        ) -> Result<()>;
        #[cxx_name = "takeDialed"]
        fn take_dialed(dialed: Pin<&mut Dialed>) -> KjOwn<AsyncIoStream>;
    }

    extern "Rust" {
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
        async unsafe fn when_write_disconnected<'a>(self: &'a RustIo);

        type RustBody;
        async unsafe fn body_read<'a>(
            body: &'a mut RustBody,
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
        async unsafe fn abort<'a>(self: &'a RustWebSocket);
        async unsafe fn when_aborted<'a>(self: &'a RustWebSocket);
        fn sent_byte_count(self: &RustWebSocket) -> u64;
        fn received_byte_count(self: &RustWebSocket) -> u64;

        type HyperClient;
        unsafe fn new_pooled_client(
            table: &HttpHeaderTable,
            connector: &HttpConnector,
            idle_timeout_ms: u64,
            proxy: bool,
        ) -> Box<HyperClient>;
        unsafe fn new_single_client(
            table: &HttpHeaderTable,
            stream: KjOwn<AsyncIoStream>,
            lent: bool,
        ) -> Result<Box<HyperClient>>;
        async unsafe fn drive<'a>(self: &'a HyperClient);
        fn request(
            self: &HyperClient,
            method: &str,
            url: &str,
            headers: &HttpHeaders,
            length: KjMaybe<u64>,
        ) -> Result<Box<ClientRequest>>;
        fn open_websocket(
            self: &HyperClient,
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
        async unsafe fn response<'a>(self: &'a mut ClientRequest) -> Result<Box<ClientResponse>>;

        type ClientResponse;
        fn status_code(self: &ClientResponse) -> u32;
        unsafe fn status_text<'a>(self: &'a ClientResponse) -> &'a [u8];
        unsafe fn headers<'a>(self: &'a ClientResponse) -> &'a HttpHeaders;
        unsafe fn websocket_handshake_error<'a>(self: &'a ClientResponse) -> &'a str;
        fn take_body(self: &ClientResponse) -> Box<RustBody>;
        fn take_websocket(
            self: &ClientResponse,
            compression: &WsCompression,
        ) -> Result<Box<RustWebSocket>>;
        fn take_tunnel(self: &ClientResponse) -> Result<Box<RustIo>>;

        type HyperConnection;
        unsafe fn new_connection(
            table: &HttpHeaderTable,
            service: &HttpService,
            stream: KjOwn<AsyncIoStream>,
            lent: bool,
            header_timeout_ms: u64,
        ) -> Box<HyperConnection>;
        async unsafe fn serve<'a>(self: &'a HyperConnection) -> Result<()>;
        fn shutdown(self: &HyperConnection);

        type ServerResponse;
        fn send(
            self: &ServerResponse,
            status: u32,
            status_text: &[u8],
            headers: &HttpHeaders,
            length: KjMaybe<u64>,
        ) -> Result<Box<BodySink>>;
        unsafe fn websocket_extensions<'a>(self: &'a ServerResponse) -> &'a [u8];
        fn websocket_rejection(self: &ServerResponse) -> WsRejection;
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

        type TlsClientConfig;
        type TlsServerConfig;
        fn new_tls_client_config(options: &TlsOptions) -> Result<Box<TlsClientConfig>>;
        fn new_tls_server_config(options: &TlsOptions) -> Result<Box<TlsServerConfig>>;
        async unsafe fn wrap_tls_client<'a>(
            stream: KjOwn<AsyncIoStream>,
            config: &'a TlsClientConfig,
            hostname: &'a str,
        ) -> Result<Box<RustIo>>;
        fn wrap_tls_server(stream: KjOwn<AsyncIoStream>, config: &TlsServerConfig) -> Box<RustIo>;
    }
}

// =======================================================================================
// Pointers C++ guarantees outlive the object holding them.

/// The header table a client or connection allocates headers against.
#[derive(Clone, Copy)]
pub struct HeaderTablePtr(*const HeaderTable);

impl HeaderTablePtr {
    pub fn get(self) -> &'static HeaderTable {
        // SAFETY: the creating C++ code guarantees the table outlives every object holding this.
        unsafe { &*self.0 }
    }
}

/// The C++ service a connection dispatches to (shared: kj services are reentrant).
#[derive(Clone, Copy)]
pub struct ServicePtr(*const HttpService);

impl ServicePtr {
    pub fn get(self) -> &'static HttpService {
        // SAFETY: as for `HeaderTablePtr`.
        unsafe { &*self.0 }
    }
}

/// The C++ connector a pooling client dials through.
pub struct ConnectorPtr(*const HttpConnector);

// SAFETY: hyper-util's pool requires `Send + Sync` connectors, but the pointer is only
// dereferenced inside dial futures, which `OwnerThread` confines to the loop thread.
unsafe impl Send for ConnectorPtr {}
// SAFETY: as above.
unsafe impl Sync for ConnectorPtr {}

impl ConnectorPtr {
    pub fn get(&self) -> &'static HttpConnector {
        // SAFETY: as for `HeaderTablePtr`.
        unsafe { &*self.0 }
    }
}

/// Dials through the connector.
pub async fn connect(
    connector: &HttpConnector,
    authority: &[u8],
    https: bool,
) -> kj::Result<KjOwn<AsyncIoStream>> {
    let mut dialed = new_dialed();
    // SAFETY: `dialed` and the arguments outlive the promise, which this future awaits.
    unsafe { connector_connect(connector, authority, https, dialed.as_mut()) }.await?;
    Ok(take_dialed(dialed.as_mut()))
}

// =======================================================================================
// OwnerThread

/// A value bound to the thread that created it, made `Send` for hyper's transports and tasks.
///
/// It is touched and dropped only on the owning thread; elsewhere `get_mut` returns `None` and a
/// drop leaks.
pub struct OwnerThread<T> {
    value: std::mem::ManuallyDrop<T>,
    owner: std::thread::ThreadId,
}

// SAFETY: moving the bytes runs no code; every access and the drop first check the thread.
unsafe impl<T> Send for OwnerThread<T> {}

impl<T> OwnerThread<T> {
    pub fn new(value: T) -> Self {
        Self {
            value: std::mem::ManuallyDrop::new(value),
            owner: std::thread::current().id(),
        }
    }

    pub fn get_mut(&mut self) -> Option<&mut T> {
        (std::thread::current().id() == self.owner).then_some(&mut *self.value)
    }
}

impl<T> Drop for OwnerThread<T> {
    fn drop(&mut self) {
        if std::thread::current().id() == self.owner {
            // SAFETY: dropped exactly once, here.
            unsafe { std::mem::ManuallyDrop::drop(&mut self.value) };
        }
    }
}

/// Off the owning thread the future never completes (and is never polled there in practice).
impl<F: Future + Unpin> Future for OwnerThread<F> {
    type Output = F::Output;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<F::Output> {
        match self.get_mut().get_mut() {
            Some(future) => Pin::new(future).poll(cx),
            None => Poll::Pending,
        }
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
    pub fn when_write_disconnected(&mut self) -> impl Future<Output = ()> + 'static {
        let mut end = kj_read_end_when_write_disconnected(&self.0);
        async move {
            // SAFETY: `end` is owned by this future and exclusively borrowed for the call.
            let _ = unsafe { kj_share_when_write_disconnected(end.as_mut()) }.await;
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
unsafe fn rust_io_read(
    io: &RustIo,
    ptr: *mut u8,
    len: usize,
    min_bytes: usize,
) -> impl Future<Output = kj::Result<usize>> + '_ {
    // SAFETY: forwarded.
    let mut buf = unsafe { uninit(ptr, len) };
    async move { io.read(&mut buf, min_bytes).await }
}

/// # Safety
///
/// As for [`uninit`].
unsafe fn body_read(
    body: &mut RustBody,
    ptr: *mut u8,
    len: usize,
    min_bytes: usize,
) -> impl Future<Output = kj::Result<usize>> + '_ {
    // SAFETY: forwarded.
    let mut buf = unsafe { uninit(ptr, len) };
    async move { body.read(&mut buf, min_bytes).await }
}

fn body_length(body: &RustBody) -> KjMaybe<u64> {
    body.length().into()
}

/// # Safety
///
/// `table` and `connector` outlive the client, used on this thread only.
unsafe fn new_pooled_client(
    table: &HttpHeaderTable,
    connector: &HttpConnector,
    idle_timeout_ms: u64,
    proxy: bool,
) -> Box<HyperClient> {
    Box::new(HyperClient::pooled(
        HeaderTablePtr(std::ptr::from_ref(table)),
        ConnectorPtr(std::ptr::from_ref(connector)),
        idle_timeout_ms,
        proxy,
    ))
}

/// # Safety
///
/// `table` (and a lent stream) outlive the client, used on this thread only.
unsafe fn new_single_client(
    table: &HttpHeaderTable,
    stream: KjOwn<AsyncIoStream>,
    lent: bool,
) -> kj::Result<Box<HyperClient>> {
    let (io, hangup) = crate::io::kj_to_tokio(stream, lent);
    Ok(Box::new(HyperClient::single(
        HeaderTablePtr(std::ptr::from_ref(table)),
        io,
        hangup,
    )?))
}

/// # Safety
///
/// `table`, `service` (and a lent stream) outlive the connection, used on this thread only.
#[expect(clippy::unnecessary_box_returns)]
unsafe fn new_connection(
    table: &HttpHeaderTable,
    service: &HttpService,
    stream: KjOwn<AsyncIoStream>,
    lent: bool,
    header_timeout_ms: u64,
) -> Box<HyperConnection> {
    let (io, hangup) = crate::io::kj_to_tokio(stream, lent);
    Box::new(HyperConnection::new(
        HeaderTablePtr(std::ptr::from_ref(table)),
        ServicePtr(std::ptr::from_ref(service)),
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
) -> impl Future<Output = kj::Result<Box<RustIo>>> + 'a {
    crate::tls::wrap_tls_client(stream, config, hostname.to_owned())
}
