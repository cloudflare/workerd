//! See lib.rs.

use std::cell::Cell;
use std::cell::RefCell;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::Mutex;

use cxx::KjError;
use cxx::KjExceptionType;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeaderTable;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::Service;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_hyper::WebSocketCompression;
use kj_hyper::client::Client;
use kj_hyper::client::ClientSettings;
use kj_hyper::server::Connect;
use kj_hyper::server::Handler;
use kj_hyper::server::ServerSettings;
use kj_hyper::server::Shutdown;
use kj_hyper::server::serve_connection;
use kj_hyper::tls;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::DuplexStream;
use tokio::sync::watch;

use crate::Result;
use crate::ffi;

fn failed(what: impl std::fmt::Display) -> KjError {
    KjError::new(KjExceptionType::Failed, what.to_string())
}

fn compression(manual: bool) -> WebSocketCompression {
    if manual {
        WebSocketCompression::MANUAL
    } else {
        WebSocketCompression::NONE
    }
}

// As kj-hyper's tls.rs tests: an EC P-256 CA, an example.com certificate it signed, and the key.
const CA_CERT: &str = "-----BEGIN CERTIFICATE-----
MIIBmzCCAUGgAwIBAgIUFtVfWCEoNJw9tRYkhBCqWkPYQAkwCgYIKoZIzj0EAwIw
GjEYMBYGA1UEAwwPd29ya2VyZCB0ZXN0IENBMCAXDTI2MDkxODEzNDcwMVoYDzIx
MjYwODI1MTM0NzAxWjAaMRgwFgYDVQQDDA93b3JrZXJkIHRlc3QgQ0EwWTATBgcq
hkjOPQIBBggqhkjOPQMBBwNCAAR0c/eq28LGrosC4Jp0m5O6/xS5vvetDh6lDWNG
LfwBXbM3O4yoeSz9pUKY4cChCSL4BMldwTbepDKMCBmVMfJDo2MwYTAdBgNVHQ4E
FgQU1HLxzohU0eGQqgstH3O0chw0cVIwHwYDVR0jBBgwFoAU1HLxzohU0eGQqgst
H3O0chw0cVIwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAgQwCgYIKoZI
zj0EAwIDSAAwRQIgYtV4qsw7p1xhZ1OSOWmRmjyc4LzQplblEr4jZmZO6BECIQCx
Al0TgxgxuWjJ4FuakSJ5qfCA2BiIliBop+phth7LKw==
-----END CERTIFICATE-----
";

const HOST_CERT: &str = "-----BEGIN CERTIFICATE-----
MIIBwzCCAWmgAwIBAgIUPPhG/ycBgb1qLtsWGGJiMhxtewAwCgYIKoZIzj0EAwIw
GjEYMBYGA1UEAwwPd29ya2VyZCB0ZXN0IENBMCAXDTI2MDkxODEzNDcwMVoYDzIx
MjYwODI1MTM0NzAxWjAWMRQwEgYDVQQDDAtleGFtcGxlLmNvbTBZMBMGByqGSM49
AgEGCCqGSM49AwEHA0IABCc5+7lyl50H3MHWYyEAgNbxnIhMc6TBtR7Wvpp6XOBg
7CzaOCZFwix4Mj8KXPoyhi7xgNQVKAgE1maTCPVPlB2jgY4wgYswDAYDVR0TAQH/
BAIwADAOBgNVHQ8BAf8EBAMCB4AwEwYDVR0lBAwwCgYIKwYBBQUHAwEwFgYDVR0R
BA8wDYILZXhhbXBsZS5jb20wHQYDVR0OBBYEFOFpqeWqxNaPtlBncznOXK34Slsh
MB8GA1UdIwQYMBaAFNRy8c6IVNHhkKoLLR9ztHIcNHFSMAoGCCqGSM49BAMCA0gA
MEUCIGkJrUe5mthCxcMYy8zUKtbDuURGIS1OqeT9xqypm2nlAiEA5jYVU9d8NUV8
WBIiMYUVDYKLwUf1elA4zib/Qnms8DQ=
-----END CERTIFICATE-----
";

const HOST_KEY: &str = "-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgb9K+RSaSGrigmGFU
Ucyf1+0zBpj3gEnQ9LCKN9gIFhqhRANCAAQnOfu5cpedB9zB1mMhAIDW8ZyITHOk
wbUe1r6aelzgYOws2jgmRcIseDI/Clz6MoYu8YDUFSgIBNZmkwj1T5Qd
-----END PRIVATE KEY-----
";

// =======================================================================================
// Server

/// The C++ service behind every connection; each call takes its own share of it.
struct Forwarding {
    service: *mut ffi::HttpService,
}

impl Forwarding {
    fn service(&self) -> kj::http::CxxService<'static> {
        // SAFETY: `start_server`'s contract: the service outlives the server.
        kj::http::CxxService::from(unsafe { ffi::share_service(self.service) })
    }
}

#[async_trait::async_trait(?Send)]
impl Handler for Forwarding {
    async fn request<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        self.service()
            .request(method, url, headers, body, response)
            .await
    }

    async fn connect<'a>(
        &'a self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connect: Connect,
    ) -> Result<()> {
        let (mut tunnel, mut response) = connect.into_kj();
        self.service()
            .connect(
                host,
                headers,
                tunnel.as_mut(),
                ConnectResponse::from(response.as_mut()),
                ConnectSettings {
                    use_tls: false,
                    tls_starter: KjMaybe::None,
                },
            )
            .await
    }
}

struct Served {
    table: &'static HeaderTable,
    settings: Rc<ServerSettings>,
    handler: Rc<dyn Handler>,
    shutdown: Rc<Shutdown>,
    /// `true` once drained; stops the accept loop.
    draining: watch::Sender<bool>,
    accepted: Cell<u32>,
}

impl Served {
    /// Serves one accepted transport alongside the test, logging nothing: a failed connection
    /// is the test's to observe from the peer.
    fn serve<IO: AsyncRead + AsyncWrite + Unpin + Send + 'static>(self: &Rc<Self>, io: IO) {
        self.accepted.set(self.accepted.get() + 1);
        let this = Rc::clone(self);
        drop(kj_rs_tokio::spawn(async move {
            let _ = serve_connection(
                io,
                this.table,
                Rc::clone(&this.settings),
                Rc::clone(&this.handler),
                &this.shutdown,
            )
            .await;
        }));
    }
}

pub struct TestServer {
    served: Rc<Served>,
    port: u16,
    /// `false` until the accept loop has stopped.
    stopped: watch::Receiver<bool>,
}

/// # Safety
///
/// See the bridge.
pub unsafe fn start_server(
    service: *mut ffi::HttpService,
    table: *const ffi::HttpHeaderTable,
    manual_compression: bool,
) -> Box<TestServer> {
    // SAFETY: the caller's contract.
    let table: &'static HeaderTable = unsafe { &*table };
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    listener.set_nonblocking(true).unwrap();
    let port = listener.local_addr().unwrap().port();
    let listener = tokio::net::TcpListener::from_std(listener).unwrap();
    let served = Rc::new(Served {
        table,
        settings: Rc::new(ServerSettings {
            websocket_compression: compression(manual_compression),
            ..ServerSettings::default()
        }),
        handler: Rc::new(Forwarding { service }),
        shutdown: Rc::new(Shutdown::new()),
        draining: watch::channel(false).0,
        accepted: Cell::new(0),
    });
    let (stopped_tx, stopped) = watch::channel(false);
    let accepting = Rc::clone(&served);
    drop(kj_rs_tokio::spawn(async move {
        let mut draining = accepting.draining.subscribe();
        let mut drained = std::pin::pin!(draining.wait_for(|draining| *draining));
        loop {
            match futures::future::select(std::pin::pin!(listener.accept()), drained.as_mut()).await
            {
                futures::future::Either::Left((Ok((stream, _)), _)) => accepting.serve(stream),
                futures::future::Either::Left((Err(_), _)) => {}
                futures::future::Either::Right(_) => break,
            }
        }
        stopped_tx.send_replace(true);
    }));
    Box::new(TestServer {
        served,
        port,
        stopped,
    })
}

impl TestServer {
    pub fn port(&self) -> u16 {
        self.port
    }

    pub fn accepted(&self) -> u32 {
        self.served.accepted.get()
    }

    pub fn drain(&self) {
        self.served.shutdown.shutdown();
        self.served.draining.send_replace(true);
    }

    pub async fn listening(&self) -> Result<()> {
        let mut stopped = self.stopped.clone();
        stopped.wait_for(|stopped| *stopped).await.map_err(failed)?;
        Ok(())
    }

    pub fn serve_pipe(&self) -> KjOwn<AsyncIoStream> {
        let (ours, peer) = tokio::io::duplex(1 << 16);
        self.served.serve(ours);
        kj_hyper::into_kj_stream(peer)
    }

    pub fn serve_tls_pipe(&self) -> Box<TlsPipe> {
        let (ours, peer) = tokio::io::duplex(1 << 16);
        Box::new(TlsPipe {
            served: Rc::clone(&self.served),
            ends: RefCell::new(Some((ours, peer))),
            client: RefCell::new(None),
        })
    }
}

/// A connection served inside TLS over a pipe, handshaken by [`TlsPipe::handshake`].
pub struct TlsPipe {
    served: Rc<Served>,
    ends: RefCell<Option<(DuplexStream, DuplexStream)>>,
    client: RefCell<Option<KjOwn<AsyncIoStream>>>,
}

impl TlsPipe {
    pub async fn handshake(&self) -> Result<()> {
        let (ours, peer) = self
            .ends
            .borrow_mut()
            .take()
            .ok_or_else(|| failed("handshaken"))?;
        let server = tls::server_config(&tls::TlsOptions {
            keypair: Some(tls::Keypair {
                certificate_chain: HOST_CERT.to_owned(),
                private_key: HOST_KEY.to_owned(),
            }),
            ..tls::TlsOptions::default()
        })?;
        let client = tls::client_config(&tls::TlsOptions {
            trusted_certificates: vec![CA_CERT.to_owned()],
            ..tls::TlsOptions::default()
        })?;
        let (accepted, connected) = futures::join!(
            tls::accept(ours, server),
            tls::connect(peer, client, "example.com")
        );
        self.served.serve(accepted?);
        *self.client.borrow_mut() = Some(kj_hyper::into_kj_stream(connected?));
        Ok(())
    }

    pub fn take_stream(&mut self) -> KjOwn<AsyncIoStream> {
        self.client.borrow_mut().take().unwrap()
    }
}

// =======================================================================================
// Client

/// kj-hyper's client, cloned for each call (the trait takes `&mut self`; the pool is shared).
pub struct TestClient {
    client: Client<'static>,
    peer: RefCell<Option<KjOwn<AsyncIoStream>>>,
}

fn settings(manual_compression: bool) -> ClientSettings {
    ClientSettings {
        websocket_compression: compression(manual_compression),
        ..ClientSettings::default()
    }
}

/// # Safety
///
/// See the bridge.
pub unsafe fn new_client(
    table: *const ffi::HttpHeaderTable,
    port: u16,
    manual_compression: bool,
) -> Box<TestClient> {
    // SAFETY: the caller's contract.
    let table: &'static HeaderTable = unsafe { &*table };
    let client = Client::new(table, settings(manual_compression), move || {
        tokio::net::TcpStream::connect((std::net::Ipv4Addr::LOCALHOST, port))
    });
    Box::new(TestClient {
        client,
        peer: RefCell::new(None),
    })
}

/// # Safety
///
/// See the bridge.
pub unsafe fn new_pipe_client(
    table: *const ffi::HttpHeaderTable,
    manual_compression: bool,
) -> Box<TestClient> {
    // SAFETY: the caller's contract.
    let table: &'static HeaderTable = unsafe { &*table };
    let (ours, peer) = tokio::io::duplex(1 << 16);
    let dialed = Arc::new(Mutex::new(Some(ours)));
    let client = Client::new(table, settings(manual_compression), move || {
        let io = dialed
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .take();
        async move { io.ok_or_else(|| std::io::Error::other("the pipe was dialed twice")) }
    });
    Box::new(TestClient {
        client,
        peer: RefCell::new(Some(kj_hyper::into_kj_stream(peer))),
    })
}

impl TestClient {
    pub fn take_peer(&mut self) -> KjOwn<AsyncIoStream> {
        self.peer.borrow_mut().take().unwrap()
    }

    pub async fn request(
        &self,
        method: Method,
        url: &[u8],
        headers: &ffi::HttpHeaders,
        request_body: Pin<&mut AsyncInputStream>,
        response: Pin<&mut ffi::HttpServiceResponse>,
    ) -> Result<()> {
        self.client
            .clone()
            .request(
                method,
                url,
                HeadersRef::from(headers),
                request_body,
                ServiceResponse::from(response),
            )
            .await
    }

    pub async fn connect(
        &self,
        host: &[u8],
        headers: &ffi::HttpHeaders,
        connection: Pin<&mut AsyncIoStream>,
        response: Pin<&mut ffi::ConnectResponse>,
    ) -> Result<()> {
        self.client
            .clone()
            .connect(
                host,
                HeadersRef::from(headers),
                connection,
                ConnectResponse::from(response),
                ConnectSettings {
                    use_tls: false,
                    tls_starter: KjMaybe::None,
                },
            )
            .await
    }
}
