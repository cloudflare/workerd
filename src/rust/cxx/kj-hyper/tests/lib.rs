//! Rust helpers for kj-hyper's C++ contract tests (kj-hyper-test.c++): a server serving a C++
//! `kj::HttpService` through kj-hyper, and kj-hyper's client behind a `kj::HttpService`, both
//! over loopback TCP or an in-process pipe, on the test's tokio-backed kj event loop.
#![allow(clippy::missing_errors_doc)]
#![allow(clippy::must_use_candidate)]
#![allow(clippy::missing_panics_doc)]
// cxx bridge functions return Box<T> by contract.
#![allow(clippy::unnecessary_box_returns)]
// Test support: a failed helper is a failed test, and an `unwrap()` reports it where it happened.
#![allow(clippy::unwrap_used, clippy::expect_used)]

mod harness;

pub use harness::TestClient;
pub use harness::TestServer;
pub use harness::TlsPipe;
use harness::new_client;
use harness::new_pipe_client;
use harness::start_server;

type Result<T> = kj::Result<T>;

fn websocket_accept(key: &[u8]) -> String {
    kj_hyper::accept_key(key)
}

#[cxx::bridge(namespace = "kj_hyper_test")]
mod ffi {
    #[namespace = "kj::rust"]
    unsafe extern "C++" {
        include!("workerd/rust/kj/ffi.h");

        type HttpHeaders = kj::http::ffi::HttpHeaders;
        type HttpHeaderTable = kj::http::ffi::HttpHeaderTable;
        type HttpMethod = kj::http::ffi::HttpMethod;
        type HttpService = kj::http::ffi::HttpService;
        type HttpServiceResponse = kj::http::ffi::HttpServiceResponse;
        type ConnectResponse = kj::http::ffi::ConnectResponse;
        type AsyncInputStream = kj::io::ffi::AsyncInputStream;
        type AsyncIoStream = kj::io::ffi::AsyncIoStream;
    }

    unsafe extern "C++" {
        include!("kj-hyper-test/test-helpers.h");

        unsafe fn share_service(service: *mut HttpService) -> KjOwn<HttpService>;
    }

    extern "Rust" {
        /// The `Sec-WebSocket-Accept` for a client's `Sec-WebSocket-Key`.
        fn websocket_accept(key: &[u8]) -> String;

        /// A kj-hyper server of a C++ `kj::HttpService`, listening on a fresh loopback port
        /// until drained.
        type TestServer;
        /// # Safety
        ///
        /// `service` and `table` outlive the server and every connection it served.
        unsafe fn start_server(
            service: *mut HttpService,
            table: *const HttpHeaderTable,
            manual_compression: bool,
        ) -> Box<TestServer>;
        fn port(self: &TestServer) -> u16;
        /// Connections accepted so far, on the port and over pipes.
        fn accepted(self: &TestServer) -> u32;
        fn drain(self: &TestServer);
        /// Resolves once the listener has stopped after `drain()`.
        async unsafe fn listening<'a>(self: &'a TestServer) -> Result<()>;
        /// A connection served over an in-process pipe; the peer's end, as a kj stream.
        fn serve_pipe(self: &TestServer) -> KjOwn<AsyncIoStream>;
        /// A connection served inside TLS over an in-process pipe (`TlsPipe`).
        fn serve_tls_pipe(self: &TestServer) -> Box<TlsPipe>;

        type TlsPipe;
        /// Both handshakes, with kj-hyper's test certificates (the server is example.com).
        async unsafe fn handshake<'a>(self: &'a TlsPipe) -> Result<()>;
        /// The client's end after `handshake()`, as a kj stream.
        fn take_stream(self: &mut TlsPipe) -> KjOwn<AsyncIoStream>;

        /// kj-hyper's client as a `kj::HttpService`: of a loopback port, or of one in-process
        /// pipe whose other end the test drives.
        type TestClient;
        /// # Safety
        ///
        /// `table` outlives the client.
        unsafe fn new_client(
            table: *const HttpHeaderTable,
            port: u16,
            manual_compression: bool,
        ) -> Box<TestClient>;
        /// # Safety
        ///
        /// As for `new_client`.
        unsafe fn new_pipe_client(
            table: *const HttpHeaderTable,
            manual_compression: bool,
        ) -> Box<TestClient>;
        /// The pipe's other end (a pipe client only), taken once.
        fn take_peer(self: &mut TestClient) -> KjOwn<AsyncIoStream>;
        async unsafe fn request<'a>(
            self: &'a TestClient,
            method: HttpMethod,
            url: &'a [u8],
            headers: &'a HttpHeaders,
            request_body: Pin<&'a mut AsyncInputStream>,
            response: Pin<&'a mut HttpServiceResponse>,
        ) -> Result<()>;
        async unsafe fn connect<'a>(
            self: &'a TestClient,
            host: &'a [u8],
            headers: &'a HttpHeaders,
            connection: Pin<&'a mut AsyncIoStream>,
            response: Pin<&'a mut ConnectResponse>,
        ) -> Result<()>;
    }
}
