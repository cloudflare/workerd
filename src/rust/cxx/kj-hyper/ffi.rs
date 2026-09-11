//! The `#[cxx::bridge]` FFI island for kj-hyper.
//!
//! This is the crate's single dedicated FFI-island file (file-top `#![allow(unsafe_code)]`). It
//! holds the crate's `#[cxx::bridge] mod bridge` (the C++ <-> Rust wire, re-exported as
//! `crate::ffi::*`), the bridge entry-point functions, and the safe pointer wrappers
//! (`HeaderTablePtr` / `ServicePtr` / `SharedFilter`, folded in from the former `cxx_ptr.rs`).
//!
//! The entry-point functions discharge each declared `# Safety` contract once — wrapping the raw
//! C++ header-table/service pointers in those safe wrappers — so the business modules (client.rs,
//! server.rs, translate.rs, ws.rs) receive only safe types and are compiler-proven `unsafe`-free
//! by the crate-root `#![deny(unsafe_code)]`.
#![allow(unsafe_code)]

use std::net::SocketAddr;

pub use bridge::*;
use cxx::UniquePtr;
use kj::http::HeaderTable;

use crate::client::ClientResponseOutcome;
use crate::client::HyperClient;
use crate::client::PendingHttpRequest;
use crate::client::RequestBodySink;
use crate::client::TunnelOutcome;
use crate::client::WsUpgradeOutcome;
use crate::client_tls::RustlsConn;
use crate::client_tls::new_rustls_client_conn;
use crate::client_tls::new_rustls_server_conn;
use crate::server::HyperConnectResponder;
use crate::server::HyperConnection;
use crate::server::HyperResponseSender;
use crate::tls;
use crate::tls::HyperTlsClientConfig;
use crate::tls::HyperTlsServerConfig;
use crate::translate::HyperRequestBody;
use crate::translate::HyperResponseBodySink;
use crate::translate::ServeRequestBody;
use crate::upgraded_io::HyperTunnel;
use crate::ws_pipe::WsPipeEnd;
use crate::ws_pipe::new_websocket_pipe;

/// Bridge entry for the pipe's pumpTo: wraps the foreign socket in a [`WsPtr`] (discharging
/// its lifetime contract once, here) and runs the safe state machine.
async fn ws_pipe_pump_to(end: &WsPipeEnd, other: &bridge::WebSocket) -> kj::Result<()> {
    // SAFETY: kj's pump contract -- `other` outlives the returned promise; loop-thread
    // confined; the pipe clears every stored copy when the pump settles or drops.
    let ws = unsafe { WsPtr::new(other) };
    end.pump_to(ws).await
}

/// Bridge entry for the pipe's tryPumpFrom (see [`ws_pipe_pump_to`]).
async fn ws_pipe_pump_from(end: &WsPipeEnd, other: &bridge::WebSocket) -> kj::Result<()> {
    // SAFETY: as above.
    let ws = unsafe { WsPtr::new(other) };
    end.pump_from(ws).await
}

/// Bridge entry for `kj::WebSocket::pumpTo()`'s default fallback loop (the workerd kj-http
/// shim's interface-default definition, kj's `pumpWebSocketLoop`): wraps both foreign sockets
/// in [`WsPtr`]s (discharging their lifetime contract once, here) and runs the safe loop.
async fn websocket_default_pump(
    from: &bridge::WebSocket,
    to: &bridge::WebSocket,
) -> kj::Result<()> {
    // SAFETY: kj's pump contract -- both sockets outlive the returned promise; loop-thread
    // confined; nothing is stored beyond the returned future.
    let (from, to) = unsafe { (WsPtr::new(from), WsPtr::new(to)) };
    crate::ws_pipe::default_pump(from, to).await
}
use crate::ws;
use crate::ws::WsSession;

// needless_lifetimes: cxx requires explicit lifetimes on reference-returning bridge fns.
#[expect(clippy::needless_lifetimes)]
#[cxx::bridge(namespace = "workerd::rust::kj_hyper")]
mod bridge {
    /// The kind of WebSocket message in a `WsMessage` (mirrors `kj::WebSocket::Message`'s
    /// alternatives).
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    #[repr(u8)]
    enum WsMessageKind {
        TEXT,
        BINARY,
        CLOSE,
    }

    /// One received WebSocket message. For TEXT the data is the (not necessarily valid UTF-8,
    /// matching kj's non-validation) text bytes; for BINARY the payload; for CLOSE the reason
    /// bytes plus `close_code`.
    struct WsMessage {
        kind: WsMessageKind,
        data: Vec<u8>,
        close_code: u16,
    }

    /// Result of `WsSession::preferred_extensions()`: a `kj::Maybe<kj::String>` shape.
    struct WsPreferredExtensions {
        is_some: bool,
        value: String,
    }

    /// `config::TlsOptions::Version` (workerd.capnp), the minimum-TLS-version knob. rustls
    /// implements only TLS 1.2/1.3: SSL3/TLS1_0/TLS1_1 are rejected with a clear error when
    /// building a `HyperTlsClientConfig` (see tls.rs).
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    #[repr(u8)]
    enum TlsMinVersion {
        GOOD_DEFAULT,
        SSL3,
        TLS1_0,
        TLS1_1,
        TLS1_2,
        TLS1_3,
    }

    /// The subset of `config::TlsOptions` workerd's *outbound* TLS paths use, pre-extracted by
    /// the C++ side (which owns the capnp schema). See tls.rs for the rustls mapping.
    struct TlsClientOptions {
        /// `trustBrowserCas` (kj `useSystemTrustStore`): trust the platform trust store.
        trust_system_roots: bool,
        /// `trustedCertificates`: PEM text, one or more certificates per entry.
        trusted_certificates: Vec<String>,
        /// `keypair.certificateChain` (PEM), the client-certificate identity; empty = no
        /// client auth.
        certificate_chain: String,
        /// `keypair.privateKey` (PEM). Only meaningful when `certificate_chain` is non-empty.
        private_key: String,
        min_version: TlsMinVersion,
        /// `cipherList` (OpenSSL syntax); empty = rustls defaults.
        cipher_list: String,
    }

    /// The subset of `config::TlsOptions` workerd's *inbound* (https socket) path uses,
    /// pre-extracted by the C++ side. See `HyperTlsServerConfig` in tls.rs for the mapping.
    struct TlsServerOptions {
        /// `keypair.certificateChain` (PEM) -- required for a server.
        certificate_chain: String,
        /// `keypair.privateKey` (PEM).
        private_key: String,
        /// `requireClientCerts` (kj `verifyClients`).
        require_client_certs: bool,
        /// `trustBrowserCas`: include the platform trust store among the CAs client
        /// certificates may chain to.
        trust_system_roots: bool,
        /// `trustedCertificates`: PEM text, one or more certificates per entry.
        trusted_certificates: Vec<String>,
        min_version: TlsMinVersion,
        /// `cipherList` (OpenSSL syntax); empty = rustls defaults.
        cipher_list: String,
    }

    #[namespace = "kj::rust"]
    unsafe extern "C++" {
        include!("kj-rs-http/ffi.h");
        type HttpHeaderTable = kj::http::ffi::HttpHeaderTable;
        type HttpHeaders = kj::http::ffi::HttpHeaders;
        type HttpMethod = kj::http::ffi::HttpMethod;
        type HttpService = kj::http::ffi::HttpService;
        type HttpServiceResponse = kj::http::ffi::HttpServiceResponse;
        type ConnectResponse = kj::http::ffi::ConnectResponse;
        type AsyncInputStream = kj::io::ffi::AsyncInputStream;
        type AsyncIoStream = kj::io::ffi::AsyncIoStream;
    }

    #[namespace = "kj"]
    unsafe extern "C++" {
        include!("kj/compat/http.h");
        type WebSocket;
    }

    unsafe extern "C++" {
        include!("kj-hyper/peer-filter.h");

        /// The restrictPeers network ACL, a wrapper over `kj_rs_io::PeerFilter` (KJ's
        /// `kj::_::NetworkFilter` port). Because the hyper client dials `host:port` on tokio
        /// itself, the filter kj would otherwise apply inside `connect()` must be re-applied
        /// here, per connection, at the resolved IP actually being dialed. Immutable after
        /// construction; built on the C++ side via peer-filter.h.
        type HyperPeerFilter;

        /// Whether a resolved peer address is permitted (the same rules kj applies inside
        /// `connect()`; see client.rs `dial_filtered`). `addr` holds the 4 (IPv4) or 16 (IPv6)
        /// network-order bytes of the IP; `port` is host order.
        #[cxx_name = "shouldAllow"]
        fn should_allow(self: &HyperPeerFilter, is_ipv6: bool, addr: &[u8], port: u16) -> bool;
    }

    // =====================================================================================
    // WebSocket pipe (kj::newWebSocketPipe()'s Rust replacement; see ws_pipe.rs)

    unsafe extern "C++" {
        include!("kj-hyper/ws-pipe-ffi.h");

        /// Received-message holder for `websocket_receive_into` (see ws-pipe-ffi.h; kind 0 =
        /// text, 1 = binary, 2 = close).
        type PipeWsMessage;
        fn kind(self: &PipeWsMessage) -> u8;
        unsafe fn data<'a>(self: &'a PipeWsMessage) -> &'a [u8];
        #[cxx_name = "closeCode"]
        fn close_code(self: &PipeWsMessage) -> u16;
        fn new_pipe_ws_message() -> KjOwn<PipeWsMessage>;

        /// Foreign kj::WebSocket operations, all shared-receiver (see ws-pipe-ffi.h's aliasing
        /// contract): the pipe's pump-adoption states drive real sockets the pipe is pumped
        /// to/from.
        ///
        /// # Safety
        /// Each `&WebSocket` must be a live socket on this thread's KJ event loop, outliving
        /// the returned promise (kj's pump contract); discharged by `WsPtr` below.
        async unsafe fn websocket_send_text<'a>(ws: &'a WebSocket, text: &'a [u8]) -> Result<()>;
        async unsafe fn websocket_send_binary<'a>(ws: &'a WebSocket, data: &'a [u8]) -> Result<()>;
        async unsafe fn websocket_close<'a>(
            ws: &'a WebSocket,
            code: u16,
            reason: &'a [u8],
        ) -> Result<()>;
        unsafe fn websocket_disconnect(ws: &WebSocket);
        async unsafe fn websocket_when_aborted<'a>(ws: &'a WebSocket) -> Result<()>;
        async unsafe fn websocket_pump_to<'a>(from: &'a WebSocket, to: &'a WebSocket)
        -> Result<()>;
        async unsafe fn websocket_receive_into<'a>(
            ws: &'a WebSocket,
            max_size: usize,
            out: Pin<&'a mut PipeWsMessage>,
        ) -> Result<()>;
        unsafe fn websocket_received_byte_count(ws: &WebSocket) -> u64;
        unsafe fn websocket_get_preferred_extensions(
            ws: &WebSocket,
            is_request_context: bool,
            out: &mut String,
        ) -> bool;
    }

    extern "Rust" {
        /// One end of the in-memory WebSocket pipe; see `newWebSocketPipe()` in hyper-http.h
        /// for the C++ kj::WebSocket wrapper.
        type WsPipeEnd;

        async unsafe fn send_text<'a>(self: &'a WsPipeEnd, text: &'a [u8]) -> Result<()>;
        async unsafe fn send_binary<'a>(self: &'a WsPipeEnd, data: &'a [u8]) -> Result<()>;
        async unsafe fn close<'a>(self: &'a WsPipeEnd, code: u16, reason: &'a [u8]) -> Result<()>;
        fn disconnect(self: &WsPipeEnd);
        fn abort(self: &WsPipeEnd);
        async unsafe fn when_aborted<'a>(self: &'a WsPipeEnd);
        async unsafe fn receive<'a>(self: &'a WsPipeEnd, max_size: usize) -> Result<WsMessage>;
        /// kj::WebSocket::pumpTo()/tryPumpFrom() with the pipe's adoption semantics.
        ///
        /// # Safety
        /// `other` must outlive the returned promise (kj's pump contract).
        async unsafe fn ws_pipe_pump_to<'a>(end: &'a WsPipeEnd, other: &'a WebSocket)
        -> Result<()>;
        async unsafe fn ws_pipe_pump_from<'a>(
            end: &'a WsPipeEnd,
            other: &'a WebSocket,
        ) -> Result<()>;

        /// kj::WebSocket::pumpTo()'s default fallback loop (kj's pumpWebSocketLoop, defined by
        /// the workerd kj-http shim's WebSocket::pumpTo() under the rust backend): receive each
        /// message from `from` and forward it into `to`; forwarding a Close completes the pump;
        /// any receive/send error disconnect()s `to` and propagates as the pump result. The
        /// shim wraps this in kj's exact whenAborted/abort cancellation race.
        ///
        /// # Safety
        /// `from` and `to` must outlive the returned promise (kj's pump contract).
        async unsafe fn websocket_default_pump<'a>(
            from: &'a WebSocket,
            to: &'a WebSocket,
        ) -> Result<()>;
        fn sent_byte_count(self: &WsPipeEnd) -> u64;
        fn received_byte_count(self: &WsPipeEnd) -> u64;
        /// The peer end's active pump target's getPreferredExtensions, if any (see the spec:
        /// reads the shared out-direction's recorded pump destinations, To before From).
        fn peer_preferred_extensions(
            self: &WsPipeEnd,
            is_request_context: bool,
            out: &mut String,
        ) -> bool;

        /// Create the two entangled ends.
        fn new_websocket_pipe() -> WsPipePair;
    }

    /// The two ends of a freshly-created WebSocket pipe (kj::WebSocketPipe's shape).
    struct WsPipePair {
        end1: Box<WsPipeEnd>,
        end2: Box<WsPipeEnd>,
    }

    // =====================================================================================
    // Outbound client

    extern "Rust" {
        /// Outbound HTTP/1.1 client for one upstream `host:port`, backed by hyper with its
        /// connection tasks on the calling thread's kj-rs-tokio loop runtime. See
        /// `newHyperHttpService()` in hyper-http.h for the C++ wrapper.
        type HyperClient;

        /// Create a client. The calling thread must own a `kj_rs_tokio::TokioEventPort` by the
        /// time a request is made.
        ///
        /// # Safety
        /// `table` must outlive the returned client (the same contract as
        /// `kj::newHttpClient(timer, table, addr, ...)`); response headers are allocated
        /// against it.
        /// `jsgify_websocket_errors` renders WebSocket protocol errors the way workerd's
        /// JsgifyWebSocketErrors handler does (the kj default rendering prefixed with
        /// "jsg.Error: "), mirroring kj's pluggable `webSocketErrorHandler` setting.
        /// `filter` is the restrictPeers ACL applied to every connection this client dials (see
        /// `HyperPeerFilter`); pass `newAllowAllHyperPeerFilter()` for an unrestricted client.
        #[expect(clippy::unnecessary_box_returns)]
        unsafe fn new_hyper_http_client(
            table: &HttpHeaderTable,
            host: &str,
            port: u16,
            jsgify_websocket_errors: bool,
            filter: UniquePtr<HyperPeerFilter>,
        ) -> Box<HyperClient>;

        /// A rustls client configuration built from workerd `TlsOptions` (see
        /// `TlsClientOptions`), shareable across per-host HTTPS clients. Fails with a clear
        /// `kj::Exception` for configurations rustls cannot honor (bad PEM, minVersion below
        /// TLS 1.2, cipherList with no supported cipher).
        type HyperTlsClientConfig;

        fn new_hyper_tls_client_config(
            options: &TlsClientOptions,
        ) -> Result<Box<HyperTlsClientConfig>>;

        /// Like `new_hyper_http_client`, but connections to `host`:`port` are wrapped in TLS
        /// using `tls_config`. `expected_server_hostname` is the name the server certificate
        /// must be valid for (also sent as SNI) — kj's `expectedServerHostname`; pass the
        /// dialed host when there is no separate certificateHost. Fails if the name is not a
        /// valid DNS name or IP address.
        ///
        /// # Safety
        /// Same contract as `new_hyper_http_client`; `tls_config` is only read during the
        /// call (its configuration is shared by refcount afterwards).
        unsafe fn new_hyper_https_client(
            table: &HttpHeaderTable,
            host: &str,
            port: u16,
            jsgify_websocket_errors: bool,
            tls_config: &HyperTlsClientConfig,
            expected_server_hostname: &str,
            filter: UniquePtr<HyperPeerFilter>,
        ) -> Result<Box<HyperClient>>;

        /// Like `new_hyper_http_client`, but over one pre-connected stream the caller hands
        /// over (the `kj::newHttpClient(table, stream)` single-connection client shape): no
        /// dialing, no TLS, no peer filter — the stream carries whatever the caller
        /// established. Once that connection closes, further requests fail DISCONNECTED.
        ///
        /// # Safety
        /// `table` must outlive the returned client (the same contract as
        /// `new_hyper_http_client`).
        #[expect(clippy::unnecessary_box_returns)]
        unsafe fn new_hyper_stream_http_client(
            table: &HttpHeaderTable,
            stream: KjOwn<AsyncIoStream>,
            jsgify_websocket_errors: bool,
        ) -> Box<HyperClient>;

        /// Corresponds to `kj::HttpService::request()` for plain (non-upgrade) requests.
        async unsafe fn request<'a>(
            self: &'a HyperClient,
            method: HttpMethod,
            url: &'a [u8],
            headers: &'a HttpHeaders,
            request_body: Pin<&'a mut AsyncInputStream>,
            response: Pin<&'a mut HttpServiceResponse>,
        ) -> Result<()>;

        /// Drives the stream-tier byte pump for a stream client without a native socket; the
        /// C++ client wrapper owns the returned promise as a member so the pump — and its
        /// bridged kj promises — are cancelled synchronously when the client is destroyed
        /// (before its borrowed stream dies, and while the KJ event loop still exists).
        /// Resolves immediately for clients without a pump.
        async unsafe fn drive_stream_pump<'a>(self: &'a HyperClient);

        /// Performs a WebSocket upgrade handshake (kj `openWebSocket()` semantics). The outcome
        /// is either an established WebSocket or a regular (non-101) response.
        async unsafe fn open_websocket<'a>(
            self: &'a HyperClient,
            url: &'a [u8],
            headers: &'a HttpHeaders,
        ) -> Result<Box<WsUpgradeOutcome>>;

        /// Issues a CONNECT request (kj `connect()` semantics): 2xx yields a tunnel, anything
        /// else a rejection with an error body.
        async unsafe fn connect_tunnel<'a>(
            self: &'a HyperClient,
            host: &'a [u8],
            headers: &'a HttpHeaders,
        ) -> Result<Box<TunnelOutcome>>;

        /// Sends a request asking for an HTTP/1.1 protocol upgrade (`Connection: Upgrade` plus
        /// the caller's `Upgrade:` header, e.g. Docker's connection-hijacking `Upgrade: tcp`):
        /// a 101 yields the raw upgraded byte stream as the tunnel, anything else the regular
        /// response with its body. `body`, when non-empty, is sent complete with an exact
        /// Content-Length.
        async unsafe fn upgrade_request<'a>(
            self: &'a HyperClient,
            method: HttpMethod,
            url: &'a [u8],
            headers: &'a HttpHeaders,
            body: &'a [u8],
        ) -> Result<Box<TunnelOutcome>>;

        type WsUpgradeOutcome;
        fn status_code(self: &WsUpgradeOutcome) -> u32;
        unsafe fn status_text<'a>(self: &'a WsUpgradeOutcome) -> &'a [u8];
        unsafe fn response_headers<'a>(self: &'a WsUpgradeOutcome) -> &'a HttpHeaders;
        fn is_websocket(self: &WsUpgradeOutcome) -> bool;
        fn expected_body_size(self: &WsUpgradeOutcome) -> KjMaybe<u64>;
        fn take_websocket(self: &mut WsUpgradeOutcome) -> Result<Box<WsSession>>;
        fn take_body(self: &mut WsUpgradeOutcome) -> Result<Box<HyperRequestBody>>;

        type TunnelOutcome;
        fn status_code(self: &TunnelOutcome) -> u32;
        unsafe fn status_text<'a>(self: &'a TunnelOutcome) -> &'a [u8];
        unsafe fn response_headers<'a>(self: &'a TunnelOutcome) -> &'a HttpHeaders;
        fn is_accepted(self: &TunnelOutcome) -> bool;
        fn expected_body_size(self: &TunnelOutcome) -> KjMaybe<u64>;
        fn take_tunnel(self: &mut TunnelOutcome) -> Result<Box<HyperTunnel>>;
        fn take_body(self: &mut TunnelOutcome) -> Result<Box<HyperRequestBody>>;

        /// A new handle to the same client (shared `Rc` inner: same upstream, keep-alive pool,
        /// and header table). The composition seam that keeps the
        /// `kj::newHttpClient(HttpService&)` / `kj::newHttpService(HttpClient&)` fast paths
        /// inside Rust: the service and client shapes become two views of one client.
        #[expect(clippy::unnecessary_box_returns)]
        fn clone_client(client: &HyperClient) -> Box<HyperClient>;

        /// Whether this client can accept another request without failing DISCONNECTED — the
        /// analog of kj `HttpClientImpl::canReuse()`, for connection-pooling wrappers (workerd's
        /// rust-backend kj-http shim pools single-connection stream clients the way kj's
        /// `NetworkAddressHttpClient` pools its `HttpClientImpl`s). For a single-connection
        /// stream client this is true while the stream is still unconsumed, or while the
        /// connection made from it is alive and not taken over by an upgrade; dialing clients
        /// (which reconnect internally) are always reusable.
        fn can_reuse(client: &HyperClient) -> bool;

        /// Begin a plain request in the `kj::HttpClient::request()` shape (see
        /// `HyperHttpClient` in hyper-http.h): translates the head and creates the body channel
        /// synchronously — kj clients may destroy `url`/`headers` as soon as `request()`
        /// returns — and may fail like kj's serializeRequest (invalid URL/header). Framing
        /// follows `kj::HttpClient::request()`: `expected_body_size` Some(n) → Content-Length n
        /// (framing header first), none → chunked.
        fn start_request(
            client: &HyperClient,
            method: HttpMethod,
            url: &[u8],
            headers: &HttpHeaders,
            expected_body_size: KjMaybe<u64>,
        ) -> Result<Box<PendingHttpRequest>>;

        /// One in-flight `kj::HttpClient::request()`.
        type PendingHttpRequest;

        /// The request-body writer (`kj::HttpClient::Request::body`); may be taken once.
        /// Dropping it ends the body (kj's drop-to-finish contract).
        fn take_body_sink(self: &PendingHttpRequest) -> Result<Box<RequestBodySink>>;

        /// Send the request and await the response head (`kj::HttpClient::Request::response`);
        /// may be called once.
        ///
        /// # Safety
        /// The pending request must outlive the returned promise (the C++ wrapper's coroutine
        /// owns the Box for the promise's duration).
        async unsafe fn response<'a>(
            self: &'a PendingHttpRequest,
        ) -> Result<Box<ClientResponseOutcome>>;

        type RequestBodySink;

        /// Corresponds to `kj::AsyncOutputStream::write()`. The buffer is only read through the
        /// returned future; dropping the future cancels the write.
        async unsafe fn write<'a>(self: &'a mut RequestBodySink, buffer: &'a [u8]) -> Result<()>;

        /// Corresponds to `kj::AsyncOutputStream::whenWriteDisconnected()`.
        async unsafe fn when_write_disconnected<'a>(self: &'a RequestBodySink);

        type ClientResponseOutcome;
        fn status_code(self: &ClientResponseOutcome) -> u32;
        unsafe fn status_text<'a>(self: &'a ClientResponseOutcome) -> &'a [u8];
        unsafe fn response_headers<'a>(self: &'a ClientResponseOutcome) -> &'a HttpHeaders;
        fn expected_body_size(self: &ClientResponseOutcome) -> KjMaybe<u64>;
        fn take_body(self: &mut ClientResponseOutcome) -> Result<Box<HyperRequestBody>>;
    }

    // =====================================================================================
    // Raw-socket TLS: a synchronous rustls state machine driven by the C++ `RustlsStream`
    // (workerd tls-network.c++) over an already-connected plaintext `kj::AsyncIoStream`.
    // Client-side connections back the sockets API's startTls and TLS-from-the-start
    // connects; server-side connections back the rustls SecureNetworkWrapper's
    // wrapPort()/wrapServer() (TLS listeners). See client_tls.rs for the orchestration
    // contract.

    extern "Rust" {
        /// A synchronous rustls connection (client- or server-side; see the constructors).
        /// Holds no I/O handles; the C++ side performs all wire I/O on the underlying
        /// plaintext stream and feeds this machine.
        type RustlsConn;

        /// Build a client TLS connection verifying (and SNI-advertising)
        /// `expected_server_hostname`, using `config` (the same rustls client config the HTTPS
        /// client uses). Errors on an invalid server name or unusable config.
        fn new_rustls_client_conn(
            config: &HyperTlsClientConfig,
            expected_server_hostname: &str,
        ) -> Result<Box<RustlsConn>>;

        /// Build a server-side TLS connection using `config` (the same rustls server config
        /// the hyper https listener uses). The handshake is driven by the first reads: the
        /// peer speaks first (ClientHello).
        fn new_rustls_server_conn(config: &HyperTlsServerConfig) -> Result<Box<RustlsConn>>;

        /// Whether the TLS handshake flight is still in progress.
        fn is_handshaking(self: &RustlsConn) -> bool;

        /// Whether rustls has ciphertext queued to send on the wire.
        fn wants_tls_write(self: &RustlsConn) -> bool;

        /// Drain all queued outgoing ciphertext (must be sent before the next call; the C++
        /// side serializes this with the wire write so record order is preserved).
        fn take_tls_out(self: &mut RustlsConn) -> Result<Vec<u8>>;

        /// Feed ciphertext read from the wire; processes completed records (errors carry
        /// kj-parity certificate-verification text).
        fn feed_tls_in(self: &mut RustlsConn, data: &[u8]) -> Result<()>;

        /// Drain decrypted application data into `buf`: returns bytes read, `0` for clean EOF,
        /// `-1` when no plaintext is available yet (read more ciphertext and feed it).
        fn read_plaintext(self: &mut RustlsConn, buf: &mut [u8]) -> Result<i64>;

        /// Queue application data for encryption (emitted by the next `take_tls_out`).
        fn write_plaintext(self: &mut RustlsConn, data: &[u8]) -> Result<()>;

        /// Queue a `close_notify` alert (kj `shutdownWrite()`).
        fn send_close_notify(self: &mut RustlsConn);
    }

    // =====================================================================================
    // WebSocket sessions and CONNECT tunnels (shared by client and server directions)

    extern "Rust" {
        /// The Rust side of a `kj::WebSocket` (wrapped by `RustWebSocket`, see
        /// hyper-server-ffi.c++). All methods take `&self`: kj allows one send() and one
        /// receive() concurrently (plus whenAborted()), coordinated internally.
        type WsSession;

        /// Corresponds to both `kj::WebSocket::send()` overloads. The buffer must stay valid
        /// until the returned promise resolves (kj's contract).
        async unsafe fn send<'a>(
            self: &'a WsSession,
            is_text: bool,
            message: &'a [u8],
        ) -> Result<()>;

        /// Corresponds to `kj::WebSocket::close()`.
        async unsafe fn close<'a>(self: &'a WsSession, code: u16, reason: &'a [u8]) -> Result<()>;

        /// Corresponds to `kj::WebSocket::receive(maxSize)`.
        async unsafe fn receive<'a>(self: &'a WsSession, max_size: usize) -> Result<WsMessage>;

        /// Corresponds to `kj::WebSocket::disconnect()`.
        fn disconnect(self: &WsSession);

        /// Corresponds to `kj::WebSocket::abort()`.
        fn abort(self: &WsSession);

        /// Corresponds to `kj::WebSocket::whenAborted()`.
        async unsafe fn when_aborted<'a>(self: &'a WsSession);

        fn sent_byte_count(self: &WsSession) -> u64;
        fn received_byte_count(self: &WsSession) -> u64;

        /// Corresponds to `kj::WebSocket::getPreferredExtensions()`.
        fn get_preferred_extensions(
            self: &WsSession,
            request_context: bool,
        ) -> WsPreferredExtensions;

        /// The Rust side of a `kj::AsyncIoStream` over a CONNECT tunnel (wrapped by
        /// `RustTunnelStream`, see hyper-server-ffi.c++).
        type HyperTunnel;

        /// Corresponds to `kj::AsyncInputStream::tryRead(buffer, min_bytes, buffer.len())`.
        async unsafe fn read<'a>(
            self: &'a HyperTunnel,
            buffer: &'a mut [u8],
            min_bytes: usize,
        ) -> Result<usize>;

        /// Corresponds to `kj::AsyncOutputStream::write()`.
        async unsafe fn write<'a>(self: &'a HyperTunnel, buffer: &'a [u8]) -> Result<()>;

        /// Corresponds to `kj::AsyncIoStream::shutdownWrite()`.
        fn shutdown_write(self: &HyperTunnel);

        /// Corresponds to `kj::AsyncIoStream::abortRead()`.
        fn abort_read(self: &HyperTunnel);

        /// Corresponds to `kj::AsyncOutputStream::whenWriteDisconnected()`.
        async unsafe fn when_write_disconnected<'a>(self: &'a HyperTunnel);

        /// Computes the `Sec-WebSocket-Accept` value for a key (RFC 6455). Exposed for tests.
        fn websocket_accept_key(key: &[u8]) -> String;
    }

    // =====================================================================================
    // Inbound server

    unsafe extern "C++" {
        include!("kj-hyper/hyper-server-ffi.h");

        /// Wraps the Rust side of an inbound request body (or client-side response/rejection
        /// body) as a `kj::AsyncInputStream`.
        fn new_hyper_request_body_stream(body: Box<HyperRequestBody>) -> KjOwn<AsyncInputStream>;

        /// Wraps the hyper serve path's inline (same-task, pump-free) request body as a
        /// `kj::AsyncInputStream` — see `ServeRequestBody` (Stage 4).
        fn new_serve_request_body_stream(body: Box<ServeRequestBody>) -> KjOwn<AsyncInputStream>;

        /// Wraps the Rust side of an inbound response as a `kj::HttpService::Response` for the
        /// C++ service.
        fn new_hyper_response(sender: Box<HyperResponseSender>) -> KjOwn<HttpServiceResponse>;

        /// Wraps a Rust WebSocket session as a `kj::WebSocket`.
        fn new_rust_websocket(session: Box<WsSession>) -> KjOwn<WebSocket>;

        /// Wraps a Rust CONNECT tunnel as a `kj::AsyncIoStream`.
        fn new_tunnel_stream(tunnel: Box<HyperTunnel>) -> KjOwn<AsyncIoStream>;

        /// Wraps the Rust side of an inbound CONNECT response as a
        /// `kj::HttpService::ConnectResponse` for the C++ service.
        fn new_hyper_connect_response(
            responder: Box<HyperConnectResponder>,
        ) -> KjOwn<ConnectResponse>;

        /// Drives `kj::HttpService::request()` on a **shared** service reference. kj services are
        /// shared-reentrant — `kj::HttpServer` dispatches concurrent `request()` calls through one
        /// `kj::HttpService&` (a shared C++ reference) — and the inbound hyper server multiplexes
        /// many keep-alive requests (across connections and pipelined on one connection) onto ONE
        /// workerd `HttpService`, so several of these futures may be in flight on one service at
        /// once, each holding its own `&HttpService`. Backed by `hyper_service_request`
        /// (hyper-server-ffi.c++), which performs the (non-const) kj call on the shared object; the
        /// Rust side never forms an exclusive `Pin<&mut>` that two multiplexed requests would alias
        /// (the shared-receiver soundness rule; see hyper-server-ffi.h).
        async fn hyper_service_request(
            service: &HttpService,
            method: HttpMethod,
            url: &[u8],
            headers: &HttpHeaders,
            request_body: Pin<&mut AsyncInputStream>,
            response: Pin<&mut HttpServiceResponse>,
        ) -> Result<()>;

        /// Drives `kj::HttpService::connect()` on a **shared** service reference (see
        /// [`hyper_service_request`] for the shared-reentrant rationale). `kj::HttpServer` dispatches
        /// CONNECT with default `HttpConnectSettings` — no TLS, no `tlsStarter` — so nothing but the
        /// shared service and the CONNECT head crosses; the shim hardcodes those defaults. Backed by
        /// `hyper_service_connect` (hyper-server-ffi.c++).
        async fn hyper_service_connect(
            service: &HttpService,
            host: &[u8],
            headers: &HttpHeaders,
            connection: Pin<&mut AsyncIoStream>,
            response: Pin<&mut ConnectResponse>,
        ) -> Result<()>;
    }

    extern "Rust" {
        /// A single externally-accepted connection served by hyper (workerd's rust-I/O-backend
        /// inbound path): the C++ side accepts the connection itself — keeping
        /// PeerIdentity/cf-blob handling in C++ — and hands over the accepted
        /// `kj::AsyncIoStream`, whose socket is taken natively via kj-rs-io's
        /// `take_kj_socket` (unwrap for kj-rs-io streams, fd dup otherwise).
        type HyperConnection;

        /// Take the socket out of the accepted stream (`kj_rs_io::take_kj_socket`: kj-rs-io
        /// streams give up their native tokio socket, leaving the wrapper hollow; other
        /// fd-backed streams have their fd duplicated) and prepare to serve it with `service`.
        /// Ownership of the stream transfers here; it is destroyed by the tier that consumed
        /// it (or with the error, if no tier applies). Errors for streams that are neither
        /// (e.g. in-memory test transports) — the caller is expected to route only plain TCP
        /// sockets here.
        ///
        /// # Safety
        /// `stream` must have no I/O promises in flight (the usual kj rule for destroying a
        /// stream), and must be a *plain stream socket* (its fd, if any, carries the stream's
        /// own bytes — not a TLS or otherwise byte-transforming wrapper). `table` and
        /// `service` must outlive the returned object, and it must only be driven from the
        /// thread owning the KJ event loop.
        unsafe fn new_hyper_http_connection(
            table: &HttpHeaderTable,
            service: Pin<&mut HttpService>,
            stream: KjOwn<AsyncIoStream>,
            jsgify_websocket_errors: bool,
        ) -> Result<Box<HyperConnection>>;

        /// A rustls *server* configuration built from workerd `TlsOptions` (see
        /// `TlsServerOptions`), shareable across the connections of an https socket. Fails
        /// with a clear `kj::Exception` for configurations rustls cannot honor (no/bad
        /// keypair PEM, minVersion below TLS 1.2, cipherList with no supported cipher,
        /// requireClientCerts without usable trusted certificates).
        type HyperTlsServerConfig;

        fn new_hyper_tls_server_config(
            options: &TlsServerOptions,
        ) -> Result<Box<HyperTlsServerConfig>>;

        /// Like `new_hyper_http_connection`, but the socket is an https connection: the
        /// server-side TLS handshake (rustls, per `tls_config`) runs first, and hyper serves
        /// HTTP/1.1 over the TLS stream. A failed handshake surfaces from `serve()` (except
        /// peer disconnects, which resolve cleanly).
        ///
        /// # Safety
        /// Same contract as `new_hyper_http_connection` (in particular, `stream` must carry
        /// the raw — not yet TLS-wrapped — bytes; the handshake happens here); `tls_config`
        /// is only read during the call (its configuration is shared by refcount afterwards).
        unsafe fn new_hyper_http_connection_tls(
            table: &HttpHeaderTable,
            service: Pin<&mut HttpService>,
            stream: KjOwn<AsyncIoStream>,
            jsgify_websocket_errors: bool,
            tls_config: &HyperTlsServerConfig,
        ) -> Result<Box<HyperConnection>>;

        /// Serve the connection until it closes. Dropping the returned promise aborts the
        /// connection and cancels the in-flight service call.
        async unsafe fn serve<'a>(self: &'a HyperConnection) -> Result<()>;

        /// Begin a graceful shutdown; `serve()` resolves once in-flight work finishes.
        fn shutdown(self: &HyperConnection);

        type HyperRequestBody;

        /// Corresponds to `kj::AsyncInputStream::tryRead(buffer, min_bytes, buffer.len())`.
        /// The buffer is only written through the returned future; dropping the future cancels
        /// the read, after which the buffer is no longer accessed.
        async unsafe fn read<'a>(
            self: &'a mut HyperRequestBody,
            buffer: &'a mut [u8],
            min_bytes: usize,
        ) -> Result<usize>;

        /// Corresponds to `kj::AsyncInputStream::tryGetLength()`.
        fn try_get_length(self: &HyperRequestBody) -> KjMaybe<u64>;

        type ServeRequestBody;

        /// Corresponds to `kj::AsyncInputStream::tryRead(buffer, min_bytes, buffer.len())` for the
        /// hyper serve path's inline request body. Same buffer contract as `HyperRequestBody::read`.
        async unsafe fn read<'a>(
            self: &'a mut ServeRequestBody,
            buffer: &'a mut [u8],
            min_bytes: usize,
        ) -> Result<usize>;

        /// Corresponds to `kj::AsyncInputStream::tryGetLength()`.
        fn try_get_length(self: &ServeRequestBody) -> KjMaybe<u64>;

        type HyperResponseSender;

        /// Corresponds to `kj::HttpService::Response::send()`; applies kj::HttpServer's framing
        /// rules and returns the response body sink.
        fn send(
            self: &HyperResponseSender,
            status_code: u32,
            status_text: &[u8],
            headers: &HttpHeaders,
            expected_body_size: KjMaybe<u64>,
        ) -> Result<Box<HyperResponseBodySink>>;

        /// Corresponds to `kj::HttpService::Response::acceptWebSocket()`: validates the
        /// handshake, sends the 101, and yields the server-side WebSocket session.
        fn accept_websocket(
            self: &HyperResponseSender,
            headers: &HttpHeaders,
        ) -> Result<Box<WsSession>>;

        type HyperResponseBodySink;

        /// Corresponds to `kj::AsyncOutputStream::write()`.
        async unsafe fn write<'a>(
            self: &'a mut HyperResponseBodySink,
            buffer: &'a [u8],
        ) -> Result<()>;

        /// Corresponds to `kj::AsyncOutputStream::whenWriteDisconnected()`.
        async unsafe fn when_write_disconnected<'a>(self: &'a HyperResponseBodySink);

        type HyperConnectResponder;

        /// Corresponds to `kj::HttpService::ConnectResponse::accept()`.
        fn accept(
            self: &HyperConnectResponder,
            status_code: u32,
            status_text: &[u8],
            headers: &HttpHeaders,
        ) -> Result<()>;

        /// Corresponds to `kj::HttpService::ConnectResponse::reject()`; returns the rejection
        /// body sink.
        fn reject(
            self: &HyperConnectResponder,
            status_code: u32,
            status_text: &[u8],
            headers: &HttpHeaders,
            expected_body_size: KjMaybe<u64>,
        ) -> Result<Box<HyperResponseBodySink>>;
    }
}

#[expect(clippy::unnecessary_box_returns)]
unsafe fn new_hyper_http_client(
    table: &HttpHeaderTable,
    host: &str,
    port: u16,
    jsgify_websocket_errors: bool,
    filter: cxx::UniquePtr<HyperPeerFilter>,
) -> Box<HyperClient> {
    // SAFETY: the caller guarantees `table` outlives the returned client.
    let table = unsafe { HeaderTablePtr::new(std::ptr::from_ref(table)) };
    Box::new(HyperClient::new(
        table,
        host.to_owned(),
        port,
        jsgify_websocket_errors,
        filter,
    ))
}

fn new_hyper_tls_client_config(
    options: &TlsClientOptions,
) -> kj::Result<Box<HyperTlsClientConfig>> {
    Ok(Box::new(HyperTlsClientConfig::new(options)?))
}

unsafe fn new_hyper_https_client(
    table: &HttpHeaderTable,
    host: &str,
    port: u16,
    jsgify_websocket_errors: bool,
    tls_config: &HyperTlsClientConfig,
    expected_server_hostname: &str,
    filter: cxx::UniquePtr<HyperPeerFilter>,
) -> kj::Result<Box<HyperClient>> {
    let tls = tls::TlsParams::new(tls_config, expected_server_hostname)?;
    // SAFETY: the caller guarantees `table` outlives the returned client.
    let table = unsafe { HeaderTablePtr::new(std::ptr::from_ref(table)) };
    Ok(Box::new(HyperClient::new_with_tls(
        table,
        host.to_owned(),
        port,
        jsgify_websocket_errors,
        tls,
        filter,
    )))
}

unsafe fn new_hyper_stream_http_client(
    table: &HttpHeaderTable,
    stream: kj_rs::KjOwn<AsyncIoStream>,
    jsgify_websocket_errors: bool,
) -> Box<HyperClient> {
    // SAFETY: same alias transmute as take_connection_socket — `AsyncIoStream` and
    // `kj_rs_io::KjAsyncIoStream` are two bridge aliases of the same C++ type.
    let stream: kj_rs::KjOwn<kj_rs_io::KjAsyncIoStream> = unsafe { std::mem::transmute(stream) };
    // serve_kj_stream, not take_kj_socket: the client accepts any stream shape (native tokio
    // socket where one can be taken, duplex + pump otherwise).
    let served = kj_rs_io::serve_kj_stream(stream);
    // SAFETY: the caller guarantees `table` outlives the returned client.
    let table = unsafe { HeaderTablePtr::new(std::ptr::from_ref(table)) };
    Box::new(HyperClient::new_with_stream(
        table,
        served,
        jsgify_websocket_errors,
    ))
}

#[expect(clippy::unnecessary_box_returns)]
fn clone_client(client: &HyperClient) -> Box<HyperClient> {
    Box::new(client.clone_handle())
}

fn can_reuse(client: &HyperClient) -> bool {
    client.can_reuse()
}

fn start_request(
    client: &HyperClient,
    method: kj::http::Method,
    url: &[u8],
    headers: &kj::http::ffi::HttpHeaders,
    expected_body_size: kj_rs::KjMaybe<u64>,
) -> kj::Result<Box<PendingHttpRequest>> {
    Ok(Box::new(client.start_request(
        method,
        url,
        kj::http::HeadersRef::from(headers),
        expected_body_size.into(),
    )?))
}

/// Shared front half of the two connection constructors: take ownership of the accepted stream
/// and adapt it for hyper — its socket taken natively through kj-rs-io (unwrap tier for
/// kj-rs-io streams, fd-dup tier for other fd-backed streams), or, for streams with no usable
/// socket (in-memory transports, tunnel streams), a duplex bridged by a pump that
/// `HyperConnection::serve()` drives. The consumed stream is destroyed by the tier that took
/// it (the pump destroys it when it settles or is dropped).
fn take_connection_socket(
    stream: kj_rs::KjOwn<AsyncIoStream>,
) -> (kj_rs_io::ServeIo, Option<kj_rs_io::StreamPump>) {
    // SAFETY: `AsyncIoStream` (kj::io::ffi, via kj-rs-http) and `kj_rs_io::KjAsyncIoStream` are
    // two bridge aliases of the SAME C++ type, `kj::AsyncIoStream`; `KjOwn`'s repr is
    // {disposer, ptr} regardless of the pointee alias, so this transmute only renames the
    // pointee type. It replaces the raw-pointer cast the two aliases otherwise force.
    let stream: kj_rs::KjOwn<kj_rs_io::KjAsyncIoStream> = unsafe { std::mem::transmute(stream) };
    match kj_rs_io::take_kj_socket(stream) {
        Ok(io) => (io, None),
        Err(e) => {
            // No native socket: bridge through serve_kj_stream's duplex pump tier.
            let served = kj_rs_io::serve_kj_stream(e.stream);
            (served.io, served.pump)
        }
    }
}

unsafe fn new_hyper_http_connection(
    table: &HttpHeaderTable,
    service: std::pin::Pin<&mut HttpService>,
    stream: kj_rs::KjOwn<AsyncIoStream>,
    jsgify_websocket_errors: bool,
) -> kj::Result<Box<HyperConnection>> {
    let (io, pump) = take_connection_socket(stream);
    // SAFETY: forwarded caller contract (see the bridge declaration); `table` and `service`
    // outlive the returned connection. The service is held as a *shared* `*const` (vended as
    // `&HttpService`): kj services are shared-reentrant, so the (non-const) call is made on the C++
    // side and Rust never forms an exclusive `&mut` to it (the F1b fix; see `ServicePtr`).
    let (table, service) = unsafe {
        let table = HeaderTablePtr::new(std::ptr::from_ref(table));
        let service = ServicePtr::new(
            std::ptr::from_mut(std::pin::Pin::into_inner_unchecked(service)).cast_const(),
        );
        (table, service)
    };
    Ok(Box::new(HyperConnection::new(
        table,
        service,
        io,
        pump,
        jsgify_websocket_errors,
        None,
    )))
}

fn new_hyper_tls_server_config(
    options: &TlsServerOptions,
) -> kj::Result<Box<HyperTlsServerConfig>> {
    Ok(Box::new(HyperTlsServerConfig::new(options)?))
}

unsafe fn new_hyper_http_connection_tls(
    table: &HttpHeaderTable,
    service: std::pin::Pin<&mut HttpService>,
    stream: kj_rs::KjOwn<AsyncIoStream>,
    jsgify_websocket_errors: bool,
    tls_config: &HyperTlsServerConfig,
) -> kj::Result<Box<HyperConnection>> {
    let (io, pump) = take_connection_socket(stream);
    // SAFETY: forwarded caller contract (see the bridge declaration); `table` and `service`
    // outlive the returned connection. The service is held as a *shared* `*const` (vended as
    // `&HttpService`): kj services are shared-reentrant, so the (non-const) call is made on the C++
    // side and Rust never forms an exclusive `&mut` to it (the F1b fix; see `ServicePtr`).
    let (table, service) = unsafe {
        let table = HeaderTablePtr::new(std::ptr::from_ref(table));
        let service = ServicePtr::new(
            std::ptr::from_mut(std::pin::Pin::into_inner_unchecked(service)).cast_const(),
        );
        (table, service)
    };
    Ok(Box::new(HyperConnection::new(
        table,
        service,
        io,
        pump,
        jsgify_websocket_errors,
        Some(tls_config.acceptor()),
    )))
}

fn websocket_accept_key(key: &[u8]) -> String {
    ws::websocket_accept_key(key)
}

// =====================================================================================
// Safe wrappers over the raw C++ pointers the bridge hands over (folded in from cxx_ptr.rs).
//
// The bridge entry points above construct each wrapper once, discharging the caller's
// outlives/thread `# Safety` contract at that single point; client.rs / server.rs then use only
// the safe accessors, and stay compiler-proven unsafe-free under the crate-root deny.

/// A borrowed C++ `kj::HttpHeaderTable`, carried as a raw pointer across the bridge.
///
/// The creator of the client/connection guarantees the table outlives every borrow this hands
/// out — the same contract as `kj::newHttpClient(timer, table, addr, ...)`; response and request
/// header objects are allocated against it. `Copy` so the per-request dispatch can thread it
/// through without reborrowing.
#[derive(Clone, Copy)]
pub(crate) struct HeaderTablePtr(*const HeaderTable);

impl HeaderTablePtr {
    /// Wrap the borrowed table pointer.
    ///
    /// # Safety
    /// `table` must remain valid for the lifetime of this wrapper and of every reference
    /// [`get`](Self::get) returns.
    pub(crate) unsafe fn new(table: *const HeaderTable) -> Self {
        Self(table)
    }

    /// Borrow the table. Sound by the construction-time contract (see [`new`](Self::new)).
    pub(crate) fn get(self) -> &'static HeaderTable {
        // SAFETY: the creator guarantees the table outlives this wrapper (the `new` contract).
        unsafe { &*self.0 }
    }
}

/// A borrowed C++ `kj::HttpService`, carried as a raw pointer across the bridge and surfaced as a
/// *shared* `&'static HttpService`.
///
/// kj interface objects are **shared-reentrant**: `kj::HttpServer` dispatches concurrent
/// `request()`/`connect()` calls through one `HttpService&`, and the inbound hyper server
/// multiplexes many keep-alive requests (across connections, and pipelined on one connection) onto
/// the single workerd `HttpService`, so two or more calls hold a reference to the same object at
/// once. A C++ `HttpService&` is a *shared* reference (C++ `&` carries no exclusivity); modeling it
/// as a Rust `Pin<&mut>` (exclusive / `noalias`) would be UB the moment two requests are
/// multiplexed. So [`get`](Self::get) hands out a shared `&HttpService` and the (non-const) kj call
/// is performed on the C++ side (`hyper_service_request` / `hyper_service_connect`) — Rust never
/// forms a `&mut` to the service. This is the F1b fix (the inbound-hyper twin of F1): the former
/// `Pin<&mut>`-vending `pin()` laundered the shared service into an exclusive borrow, aliasing under
/// ordinary concurrent inbound traffic. `Copy` so the per-request dispatch can thread it through.
#[derive(Clone, Copy)]
pub(crate) struct ServicePtr(*const HttpService);

impl ServicePtr {
    /// Wrap the borrowed service pointer.
    ///
    /// # Safety
    /// `service` must be non-null, outlive this wrapper and every `&HttpService` [`get`](Self::get)
    /// yields, and only ever be used from the KJ event-loop thread that owns it — `kj::HttpServer`'s
    /// reentrant-service discipline: many concurrent calls, one thread, shared access.
    pub(crate) unsafe fn new(service: *const HttpService) -> Self {
        Self(service)
    }

    /// Borrow the service as a *shared* reference to dispatch one call. Multiple live
    /// `&HttpService` may coexist (the reentrant, concurrent-inbound-request case); none is ever a
    /// `&mut`, so multiplexed in-flight calls never alias exclusively. Sound by the
    /// construction-time contract (see [`new`](Self::new)).
    pub(crate) fn get(self) -> &'static HttpService {
        // SAFETY: the creator guarantees the service outlives this wrapper and is only touched from
        // its owning KJ event-loop thread (the `new` contract). Handing out a *shared* `&T` matches
        // kj::HttpServer sharing one `HttpService&` across concurrent calls; the (non-const) call is
        // made on the C++ side, so Rust never forms an aliasing `&mut`.
        unsafe { &*self.0 }
    }
}

/// A foreign `kj::WebSocket` held by the pipe's pump-adoption states (ws_pipe.rs), as a
/// *shared* pointer — kj::WebSocket is legitimately used concurrently across directions (send
/// while a receive is in flight, whenAborted while pumping), so no exclusive borrow may ever
/// exist (the same shared-receiver rule as [`ServicePtr`]). All operations go through the
/// `websocket_*` shims in ws-pipe-ffi.h, which recover the non-const reference on the C++ side.
#[derive(Clone, Copy)]
pub(crate) struct WsPtr(*const bridge::WebSocket);

impl WsPtr {
    /// Wrap the borrowed socket pointer.
    ///
    /// # Safety
    /// `ws` must be non-null, outlive this wrapper and every operation made through it, and
    /// only ever be used from the KJ event-loop thread that owns it. Discharged at the pipe's
    /// pump entry points: kj's pump contract guarantees the pumped-to/from socket outlives the
    /// pump promise, and the pipe clears every stored `WsPtr` when that promise settles or
    /// drops.
    pub(crate) unsafe fn new(ws: &bridge::WebSocket) -> Self {
        Self(std::ptr::from_ref(ws))
    }

    fn get(self) -> &'static bridge::WebSocket {
        // SAFETY: the `new` contract (socket outlives the wrapper; loop-thread confined;
        // shared access only).
        unsafe { &*self.0 }
    }

    pub(crate) async fn send_text(self, text: &[u8]) -> kj::Result<()> {
        // SAFETY: `new` contract; shared receiver.
        unsafe { bridge::websocket_send_text(self.get(), text) }
            .await
            .map_err(cxx::KjError::from)
    }
    pub(crate) async fn send_binary(self, data: &[u8]) -> kj::Result<()> {
        // SAFETY: `new` contract; shared receiver.
        unsafe { bridge::websocket_send_binary(self.get(), data) }
            .await
            .map_err(cxx::KjError::from)
    }
    pub(crate) async fn close(self, code: u16, reason: &[u8]) -> kj::Result<()> {
        // SAFETY: `new` contract; shared receiver.
        unsafe { bridge::websocket_close(self.get(), code, reason) }
            .await
            .map_err(cxx::KjError::from)
    }
    pub(crate) fn disconnect(self) {
        // SAFETY: `new` contract; shared receiver.
        unsafe { bridge::websocket_disconnect(self.get()) }
    }
    pub(crate) async fn when_aborted(self) {
        // whenAborted() cannot reject; a bridged error would be a kj invariant violation.
        // SAFETY: `new` contract; shared receiver.
        let _ = unsafe { bridge::websocket_when_aborted(self.get()) }.await;
    }
    pub(crate) async fn pump_to(self, to: WsPtr) -> kj::Result<()> {
        // SAFETY: `new` contract on both; shared receivers.
        unsafe { bridge::websocket_pump_to(self.get(), to.get()) }
            .await
            .map_err(cxx::KjError::from)
    }
    /// Receive one message (deep-copied into the bridge's `WsMessage` shape).
    pub(crate) async fn receive(self, max_size: usize) -> kj::Result<bridge::WsMessage> {
        let mut holder = bridge::new_pipe_ws_message();
        // SAFETY: `new` contract; shared receiver; the holder is exclusively ours.
        unsafe { bridge::websocket_receive_into(self.get(), max_size, holder.as_mut()) }
            .await
            .map_err(cxx::KjError::from)?;
        let holder = holder.as_ref();
        let kind = holder.kind();
        Ok(bridge::WsMessage {
            kind: match kind {
                0 => bridge::WsMessageKind::TEXT,
                1 => bridge::WsMessageKind::BINARY,
                _ => bridge::WsMessageKind::CLOSE,
            },
            // SAFETY: copied out before the holder drops.
            data: unsafe { holder.data() }.to_vec(),
            // closeCode() may only be read on a close message (unchecked OneOf::get in the shim).
            close_code: if kind == 2 { holder.close_code() } else { 0 },
        })
    }
    pub(crate) fn received_byte_count(self) -> u64 {
        // SAFETY: `new` contract; shared receiver.
        unsafe { bridge::websocket_received_byte_count(self.get()) }
    }
    /// Forward getPreferredExtensions; returns the preference, if the socket expressed one.
    pub(crate) fn preferred_extensions(self, is_request_context: bool) -> Option<String> {
        let mut out = String::new();
        // SAFETY: `new` contract; shared receiver.
        if unsafe {
            bridge::websocket_get_preferred_extensions(self.get(), is_request_context, &mut out)
        } {
            Some(out)
        } else {
            None
        }
    }
}

/// The restrictPeers ACL, shared into each dial task. Wraps the C++ [`HyperPeerFilter`] (a
/// `kj_rs_io::PeerFilter` port). The filter is immutable after construction and `should_allow`
/// only reads, so sharing it across the dial tasks is sound.
pub(crate) struct SharedFilter(UniquePtr<HyperPeerFilter>);

// SAFETY: `HyperPeerFilter` is immutable after construction; `shouldAllow` performs read-only
// CIDR matching with no interior mutability, so `&HyperPeerFilter` is safe to use from any
// thread. (In practice the dial task runs on the same thread's loop runtime.)
unsafe impl Send for SharedFilter {}
// SAFETY: see the `Send` impl above — `HyperPeerFilter` is immutable after construction and
// `shouldAllow` is read-only, so `&HyperPeerFilter` is safe to share across threads.
unsafe impl Sync for SharedFilter {}

impl SharedFilter {
    /// Wrap the restrictPeers ACL handed over by the bridge.
    pub(crate) fn new(filter: UniquePtr<HyperPeerFilter>) -> Self {
        Self(filter)
    }

    /// Whether the peer filter permits a *resolved* address. Mirrors the per-address check kj
    /// performs inside `connect()` (kj_rs_io::async-io.c++ `TokioNetworkAddress::connectImpl`).
    pub(crate) fn should_allow(&self, addr: &SocketAddr) -> bool {
        match addr {
            SocketAddr::V4(a) => self.0.should_allow(false, &a.ip().octets(), a.port()),
            SocketAddr::V6(a) => self.0.should_allow(true, &a.ip().octets(), a.port()),
        }
    }
}
