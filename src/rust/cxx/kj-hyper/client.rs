//! HTTP/1.1 clients over hyper's connection API: a pool of connections dialed through a C++
//! connector, or one connection over a kj stream.
//!
//! Connections are driven by tasks the C++ client runs ([`HyperClient::drive`]), so they are
//! cancelled with it. A request checks out a connection (idle, or dialed for it), and the
//! connection goes back once hyper is ready for another request on it. Upgrades are taken over by
//! hand: once hyper finishes HTTP on an upgraded connection, its task hands the transport to the
//! waiting WebSocket or tunnel.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::collections::VecDeque;
use std::future::Future;
use std::rc::Rc;
use std::rc::Weak;
use std::task::Poll;
use std::task::Waker;
use std::time::Duration;

use cxx::KjError;
use cxx::KjExceptionType;
use futures::FutureExt;
use futures::StreamExt;
use futures::future::LocalBoxFuture;
use futures::stream::FuturesUnordered;
use hyper::body::Incoming;
use hyper::client::conn::http1;
use hyper_util::rt::TokioIo;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use tokio::io::AsyncWriteExt;
use tokio::sync::oneshot;
use tokio::time::Instant;

use crate::body::Abandoned;
use crate::body::BodySink;
use crate::body::ChannelBody;
use crate::body::Head;
use crate::body::HeaderBlock;
use crate::body::RustBody;
use crate::ffi::HttpConnector;
use crate::ffi::HttpHeaders;
use crate::ffi::Scheme;
use crate::ffi::WsCompression;
use crate::io::BoxIo;
use crate::io::Hangup;
use crate::io::RustIo;
use crate::io::Upgraded;
use crate::io::rewound;
use crate::ws::Role;
use crate::ws::RustWebSocket;

fn failed(what: impl std::fmt::Display) -> KjError {
    KjError::new(KjExceptionType::Failed, what.to_string())
}

fn disconnected(what: impl std::fmt::Display) -> KjError {
    KjError::new(KjExceptionType::Disconnected, what.to_string())
}

/// A request's failure: the kj exception behind it (a failed dial or stream) when there is one.
fn client_error(error: impl std::error::Error + 'static) -> KjError {
    crate::io::KjIoError::find(&error).unwrap_or_else(|| disconnected(error))
}

// =======================================================================================
// Driver

type Task = LocalBoxFuture<'static, ()>;

/// The tasks a client runs: connection drivers, pool bookkeeping.
#[derive(Default)]
struct Driver {
    spawned: RefCell<Vec<Task>>,
    waker: RefCell<Option<Waker>>,
}

impl Driver {
    fn spawn(&self, task: impl Future<Output = ()> + 'static) {
        self.spawned.borrow_mut().push(task.boxed_local());
        if let Some(waker) = self.waker.borrow().as_ref() {
            waker.wake_by_ref();
        }
    }

    /// Runs the tasks until the returned future is dropped.
    fn run(self: &Rc<Self>) -> impl Future<Output = ()> + use<> {
        let driver = Rc::downgrade(self);
        let mut running = FuturesUnordered::new();
        std::future::poll_fn(move |cx| {
            let Some(driver) = driver.upgrade() else {
                return Poll::Ready(());
            };
            *driver.waker.borrow_mut() = Some(cx.waker().clone());
            loop {
                running.extend(driver.spawned.borrow_mut().drain(..));
                while running.poll_next_unpin(cx) == Poll::Ready(Some(())) {}
                if driver.spawned.borrow().is_empty() {
                    return Poll::Pending;
                }
            }
        })
    }
}

// =======================================================================================
// Connections

/// What the task driving a connection does with its transport once hyper is done with it.
enum Upgrade {
    /// No upgrade expected: shut it down.
    None,
    /// A request that may upgrade the connection awaits its response: keep it.
    Expected,
    /// The response upgraded the connection: deliver it.
    Waiting(oneshot::Sender<BoxIo>),
    /// Hyper finished before the response was taken: it waits here.
    Ready(BoxIo),
}

type UpgradeSlot = Rc<RefCell<Upgrade>>;

/// A connection ready for a request.
struct Conn {
    sender: http1::SendRequest<ChannelBody>,
    hangup: Hangup,
    upgrade: UpgradeSlot,
}

/// Handshakes a connection, its task running on `driver`.
async fn handshake(driver: &Driver, io: BoxIo, hangup: Hangup) -> crate::Result<Conn> {
    let (sender, connection) = http1::Builder::new()
        .title_case_headers(true)
        .preserve_header_case(true)
        .max_headers(crate::body::MAX_HEADERS)
        .handshake(TokioIo::new(io))
        .await
        .map_err(client_error)?;
    let upgrade: UpgradeSlot = Rc::new(RefCell::new(Upgrade::None));
    let slot = upgrade.clone();
    driver.spawn(async move {
        let Ok(parts) = connection.without_shutdown().await else {
            return;
        };
        let mut io = rewound(parts.io.into_inner(), parts.read_buf);
        let state = std::mem::replace(&mut *slot.borrow_mut(), Upgrade::None);
        match state {
            Upgrade::Waiting(tx) => drop(tx.send(io)),
            Upgrade::Expected => *slot.borrow_mut() = Upgrade::Ready(io),
            Upgrade::None | Upgrade::Ready(_) => {
                let _ = io.shutdown().await;
            }
        }
    });
    Ok(Conn {
        sender,
        hangup,
        upgrade,
    })
}

/// What a request may upgrade its connection to.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Expect {
    Response,
    WebSocket,
    Tunnel,
}

/// A response, its connection's hangup, and the transport it upgraded to, if it did.
struct Exchanged {
    response: http::Response<Incoming>,
    hangup: Hangup,
    upgraded: Option<Upgraded>,
}

type ResponseFuture = LocalBoxFuture<'static, crate::Result<Exchanged>>;

/// Sends `request` on `conn`: the response future, and the connection itself once hyper is ready
/// for another request on it (or `None` once it can take no more).
fn exchange(
    mut conn: Conn,
    request: http::Request<ChannelBody>,
    expect: Expect,
) -> (
    impl Future<Output = crate::Result<Exchanged>> + use<>,
    impl Future<Output = Option<Conn>> + use<>,
) {
    if expect != Expect::Response {
        *conn.upgrade.borrow_mut() = Upgrade::Expected;
    }
    let response = conn.sender.send_request(request);
    let hangup = conn.hangup.clone();
    let slot = conn.upgrade.clone();
    let returned = async move { conn.sender.ready().await.is_ok().then_some(conn) };
    let response = async move {
        let response = response.await.map_err(client_error)?;
        let status = response.status();
        let is_upgrade = match expect {
            Expect::Response => false,
            Expect::WebSocket => status == http::StatusCode::SWITCHING_PROTOCOLS,
            Expect::Tunnel => status.is_success(),
        };
        let upgraded = if is_upgrade {
            let (tx, rx) = oneshot::channel();
            let state = std::mem::replace(&mut *slot.borrow_mut(), Upgrade::None);
            match state {
                Upgrade::Ready(io) => drop(tx.send(io)),
                _ => *slot.borrow_mut() = Upgrade::Waiting(tx),
            }
            Some(Upgraded::new(rx))
        } else {
            *slot.borrow_mut() = Upgrade::None;
            None
        };
        Ok(Exchanged {
            response,
            hangup,
            upgraded,
        })
    };
    (response, returned)
}

// =======================================================================================
// Pool

/// Where a pool's connections go.
type Key = (String, Scheme);

struct Idle {
    conn: Conn,
    since: Instant,
}

struct Pool {
    connector: KjOwn<HttpConnector>,
    idle_timeout: Duration,
    idle: RefCell<HashMap<Key, Vec<Idle>>>,
    /// An eviction task is running.
    evicting: Cell<bool>,
    driver: Rc<Driver>,
}

impl Pool {
    fn take_idle(&self, key: &Key) -> Option<Conn> {
        let mut idle = self.idle.borrow_mut();
        let conns = idle.get_mut(key)?;
        while let Some(entry) = conns.pop() {
            if entry.conn.sender.is_ready() && entry.since.elapsed() < self.idle_timeout {
                return Some(entry.conn);
            }
        }
        None
    }

    fn put(self: &Rc<Self>, key: Key, conn: Conn) {
        self.idle.borrow_mut().entry(key).or_default().push(Idle {
            conn,
            since: Instant::now(),
        });
        if !self.evicting.replace(true) {
            self.driver.spawn(Self::evict(Rc::downgrade(self)));
        }
    }

    /// Closes connections idle for `idle_timeout`, until none are left.
    async fn evict(pool: Weak<Self>) {
        loop {
            let deadline = {
                let Some(pool) = pool.upgrade() else { return };
                let idle = pool.idle.borrow();
                let Some(oldest) = idle.values().flatten().map(|entry| entry.since).min() else {
                    pool.evicting.set(false);
                    return;
                };
                oldest + pool.idle_timeout
            };
            tokio::time::sleep_until(deadline).await;
            let Some(pool) = pool.upgrade() else { return };
            let now = Instant::now();
            // Dropping a connection's sender closes it.
            pool.idle.borrow_mut().retain(|_, conns| {
                conns.retain(|entry| now < entry.since + pool.idle_timeout);
                !conns.is_empty()
            });
        }
    }

    async fn checkout(self: Rc<Self>, key: &Key) -> crate::Result<Conn> {
        if let Some(conn) = self.take_idle(key) {
            return Ok(conn);
        }
        let stream = crate::ffi::connect(&self.connector, key.0.as_bytes(), key.1).await?;
        let (io, hangup) = crate::io::kj_to_tokio(stream);
        handshake(&self.driver, io, hangup).await
    }

    fn send(
        self: &Rc<Self>,
        key: Key,
        request: http::Request<ChannelBody>,
        expect: Expect,
    ) -> ResponseFuture {
        let pool = Rc::downgrade(self);
        Box::pin(async move {
            // The pool is held only while a connection is dialed.
            let pool = pool
                .upgrade()
                .ok_or_else(|| disconnected("the HTTP client was destroyed"))?;
            let conn = pool.clone().checkout(&key).await?;
            let (response, returned) = exchange(conn, request, expect);
            let returning = Rc::downgrade(&pool);
            pool.driver.spawn(async move {
                if let Some(conn) = returned.await
                    && let Some(pool) = returning.upgrade()
                {
                    pool.put(key, conn);
                }
            });
            drop(pool);
            response.await
        })
    }
}

// =======================================================================================
// SingleConnection

enum SingleState {
    /// The stream, until the first request makes the connection.
    Unconnected(BoxIo, Hangup),
    Idle(Conn),
    /// A request holds the connection.
    Busy,
    Closed,
}

struct SingleConnection {
    state: RefCell<SingleState>,
    /// Requests waiting for the connection to come back.
    waiters: RefCell<VecDeque<oneshot::Sender<crate::Result<Conn>>>>,
    /// Set once an application drops a response body it has not read to the end, which leaves
    /// the connection unusable.
    abandoned: Abandoned,
    driver: Rc<Driver>,
}

impl SingleConnection {
    /// Why the connection can't be used any more, as kj reports it.
    fn closed_error(&self) -> KjError {
        if self.abandoned.get() {
            failed(
                "application did not finish reading previous HTTP response body; can't read next \
                 pipelined request/response",
            )
        } else {
            disconnected("connection closed")
        }
    }

    async fn checkout(&self) -> crate::Result<Conn> {
        let state = std::mem::replace(&mut *self.state.borrow_mut(), SingleState::Busy);
        match state {
            SingleState::Unconnected(io, hangup) => handshake(&self.driver, io, hangup).await,
            SingleState::Idle(conn) if conn.sender.is_ready() => Ok(conn),
            SingleState::Idle(_) | SingleState::Closed => {
                *self.state.borrow_mut() = SingleState::Closed;
                Err(self.closed_error())
            }
            SingleState::Busy => {
                let (tx, rx) = oneshot::channel();
                self.waiters.borrow_mut().push_back(tx);
                rx.await.map_err(|_| self.closed_error())?
            }
        }
    }

    /// The connection is ready again (or gone): to the next waiter, or idle.
    fn put(&self, conn: Option<Conn>) {
        let Some(mut conn) = conn else {
            *self.state.borrow_mut() = SingleState::Closed;
            let waiters = std::mem::take(&mut *self.waiters.borrow_mut());
            for waiter in waiters {
                let _ = waiter.send(Err(self.closed_error()));
            }
            return;
        };
        loop {
            let waiter = self.waiters.borrow_mut().pop_front();
            let Some(waiter) = waiter else { break };
            match waiter.send(Ok(conn)) {
                Ok(()) | Err(Err(_)) => return,
                Err(Ok(returned)) => conn = returned,
            }
        }
        *self.state.borrow_mut() = SingleState::Idle(conn);
    }

    fn send(
        self: &Rc<Self>,
        request: http::Request<ChannelBody>,
        expect: Expect,
    ) -> ResponseFuture {
        let single = Rc::downgrade(self);
        Box::pin(async move {
            let single = single
                .upgrade()
                .ok_or_else(|| disconnected("the HTTP client was destroyed"))?;
            let conn = single.checkout().await?;
            let (response, returned) = exchange(conn, request, expect);
            let returning = Rc::downgrade(&single);
            single.driver.spawn(async move {
                let conn = returned.await;
                if let Some(single) = returning.upgrade() {
                    single.put(conn);
                }
            });
            drop(single);
            response.await
        })
    }
}

// =======================================================================================
// HyperClient

enum Upstream {
    Pooled(Rc<Pool>),
    Single(Rc<SingleConnection>),
}

/// See the module docs.
pub struct HyperClient {
    upstream: Upstream,
    driver: Rc<Driver>,
}

impl HyperClient {
    /// A pool whose connections come from `connector`, closed after `idle_timeout_ms` idle.
    pub fn pooled(connector: KjOwn<HttpConnector>, idle_timeout_ms: u64) -> Self {
        let driver = Rc::default();
        Self {
            upstream: Upstream::Pooled(Rc::new(Pool {
                connector,
                idle_timeout: Duration::from_millis(idle_timeout_ms),
                idle: RefCell::default(),
                evicting: Cell::new(false),
                driver: Rc::clone(&driver),
            })),
            driver,
        }
    }

    /// One connection over a stream; requests wait their turn for it.
    pub fn single((io, hangup): (BoxIo, Hangup)) -> Self {
        let driver = Rc::default();
        Self {
            upstream: Upstream::Single(Rc::new(SingleConnection {
                state: RefCell::new(SingleState::Unconnected(io, hangup)),
                waiters: RefCell::default(),
                abandoned: Rc::default(),
                driver: Rc::clone(&driver),
            })),
            driver,
        }
    }

    /// Runs the client's connections until the client is destroyed.
    pub fn drive(&self) -> impl Future<Output = ()> + use<> {
        self.driver.run()
    }

    fn send(
        &self,
        authority: &str,
        scheme: Scheme,
        request: http::Request<ChannelBody>,
        expect: Expect,
    ) -> (ResponseFuture, Option<Abandoned>) {
        match &self.upstream {
            Upstream::Pooled(pool) => (
                pool.send((authority.to_owned(), scheme), request, expect),
                None,
            ),
            Upstream::Single(single) => {
                (single.send(request, expect), Some(single.abandoned.clone()))
            }
        }
    }

    fn build(
        method: &str,
        url: &str,
        head: Head,
        body: ChannelBody,
    ) -> crate::Result<http::Request<ChannelBody>> {
        let mut parts = http::Request::new(()).into_parts().0;
        parts.method = http::Method::from_bytes(method.as_bytes()).map_err(failed)?;
        parts.uri = url.parse().map_err(failed)?;
        head.apply(&mut parts.headers, &mut parts.extensions);
        Ok(http::Request::from_parts(parts, body))
    }

    /// `authority` and `scheme` choose a pool's connection; `url` goes out as given.
    pub fn request(
        &self,
        authority: &str,
        scheme: Scheme,
        method: &str,
        url: &str,
        headers: &HttpHeaders,
        length: KjMaybe<u64>,
    ) -> crate::Result<Box<ClientRequest>> {
        let length: Option<u64> = length.into();
        let (sink, body) = crate::body::channel(length);
        // As kj: an empty GET or HEAD carries no Content-Length.
        let declared = length.filter(|&n| n != 0 || !matches!(method, "GET" | "HEAD"));
        let head = Head::new(headers).with_length(declared);
        let request = Self::build(method, url, head, body)?;
        let (response, abandoned) = self.send(authority, scheme, request, Expect::Response);
        Ok(Box::new(ClientRequest {
            sink: Some(Box::new(sink)),
            response: RefCell::new(Some(response)),
            abandoned,
            expected_accept: None,
        }))
    }

    /// `key`: the `Sec-WebSocket-Key`, from the client's entropy source; `extensions`: the
    /// `Sec-WebSocket-Extensions` offer to send, if any.
    pub fn open_websocket(
        &self,
        authority: &str,
        scheme: Scheme,
        url: &str,
        headers: &HttpHeaders,
        key: &str,
        extensions: &[u8],
    ) -> crate::Result<Box<ClientRequest>> {
        let expected_accept = crate::ws::accept_key(key.as_bytes());
        let mut head = Head::new(headers);
        head.remove(&http::header::SEC_WEBSOCKET_EXTENSIONS);
        if !extensions.is_empty() {
            head.set(
                http::header::SEC_WEBSOCKET_EXTENSIONS,
                "Sec-WebSocket-Extensions",
                http::HeaderValue::from_bytes(extensions).map_err(failed)?,
            );
        }
        head.set(
            http::header::CONNECTION,
            "Connection",
            http::HeaderValue::from_static("Upgrade"),
        );
        head.set(
            http::header::UPGRADE,
            "Upgrade",
            http::HeaderValue::from_static("websocket"),
        );
        head.set(
            http::header::SEC_WEBSOCKET_VERSION,
            "Sec-WebSocket-Version",
            http::HeaderValue::from_static("13"),
        );
        head.set(
            http::header::SEC_WEBSOCKET_KEY,
            "Sec-WebSocket-Key",
            http::HeaderValue::try_from(key).map_err(failed)?,
        );
        let request = Self::build("GET", url, head, ChannelBody::empty())?;
        let (response, abandoned) = self.send(authority, scheme, request, Expect::WebSocket);
        Ok(Box::new(ClientRequest {
            sink: None,
            response: RefCell::new(Some(response)),
            abandoned,
            expected_accept: Some(expected_accept),
        }))
    }

    pub fn connect(&self, host: &str, headers: &HttpHeaders) -> crate::Result<Box<ClientRequest>> {
        let request = Self::build("CONNECT", host, Head::new(headers), ChannelBody::empty())?;
        let (response, abandoned) = self.send("", Scheme::HTTP, request, Expect::Tunnel);
        Ok(Box::new(ClientRequest {
            sink: None,
            response: RefCell::new(Some(response)),
            abandoned,
            expected_accept: None,
        }))
    }
}

/// A request in flight: its body sink (plain requests) and its response.
pub struct ClientRequest {
    sink: Option<Box<BodySink>>,
    response: RefCell<Option<ResponseFuture>>,
    abandoned: Option<Abandoned>,
    /// For a WebSocket request: the `Sec-WebSocket-Accept` the server must answer with.
    expected_accept: Option<String>,
}

impl ClientRequest {
    pub fn take_body_sink(&mut self) -> crate::Result<Box<BodySink>> {
        self.sink.take().ok_or_else(|| failed("no request body"))
    }

    pub fn response(&self) -> impl Future<Output = crate::Result<Box<ClientResponse>>> + use<> {
        let taken = self.response.borrow_mut().take();
        let abandoned = self.abandoned.clone();
        let expected_accept = self.expected_accept.clone();
        async move {
            let exchanged = taken
                .ok_or_else(|| failed("response already taken"))?
                .await?;
            let (mut parts, body) = exchanged.response.into_parts();
            let reason = parts.extensions.remove::<hyper::ext::ReasonPhrase>();
            let handshake_error = expected_accept
                .as_ref()
                .filter(|_| parts.status == http::StatusCode::SWITCHING_PROTOCOLS)
                .and_then(|expected| websocket_handshake_error(&parts.headers, expected));
            Ok(Box::new(ClientResponse {
                status: parts.status,
                reason,
                hangup: exchanged.hangup,
                handshake_error,
                headers: HeaderBlock::new(&parts.headers, &parts.extensions),
                body: RefCell::new(Some(body)),
                abandoned,
                upgraded: RefCell::new(exchanged.upgraded),
            }))
        }
    }
}

/// kj's checks of a server's WebSocket handshake, with kj's messages.
fn websocket_handshake_error(headers: &http::HeaderMap, expected_accept: &str) -> Option<String> {
    let upgrade = headers
        .get(http::header::UPGRADE)
        .map(http::HeaderValue::as_bytes);
    if !upgrade.is_some_and(|u| u.eq_ignore_ascii_case(b"websocket")) {
        return Some(match upgrade {
            Some(actual) => format!(
                "Server failed WebSocket handshake: incorrect Upgrade header: expected \
                 'websocket', got '{}'.",
                String::from_utf8_lossy(actual)
            ),
            None => "Server failed WebSocket handshake: missing Upgrade header.".to_owned(),
        });
    }
    match headers.get(http::header::SEC_WEBSOCKET_ACCEPT) {
        Some(actual) if actual.as_bytes() == expected_accept.as_bytes() => None,
        Some(actual) => Some(format!(
            "Server failed WebSocket handshake: incorrect Sec-WebSocket-Accept header: expected \
             '{expected_accept}', got '{}'.",
            String::from_utf8_lossy(actual.as_bytes())
        )),
        None => Some(
            "Server failed WebSocket handshake: missing Sec-WebSocket-Accept header.".to_owned(),
        ),
    }
}

/// A response: head, then exactly one of body, WebSocket or tunnel.
pub struct ClientResponse {
    status: http::StatusCode,
    reason: Option<hyper::ext::ReasonPhrase>,
    hangup: Hangup,
    /// A WebSocket response that fails kj's handshake checks.
    handshake_error: Option<String>,
    headers: HeaderBlock,
    body: RefCell<Option<Incoming>>,
    abandoned: Option<Abandoned>,
    upgraded: RefCell<Option<Upgraded>>,
}

impl ClientResponse {
    pub fn status_code(&self) -> u32 {
        u32::from(self.status.as_u16())
    }

    pub fn status_text(&self) -> &[u8] {
        self.reason.as_ref().map_or_else(
            || self.status.canonical_reason().unwrap_or("").as_bytes(),
            hyper::ext::ReasonPhrase::as_bytes,
        )
    }

    pub fn header_arena(&self) -> &[u8] {
        &self.headers.arena
    }

    pub fn header_lens(&self) -> &[u32] {
        &self.headers.lens
    }

    /// Why this WebSocket response fails the handshake, empty if it doesn't.
    pub fn websocket_handshake_error(&self) -> &str {
        self.handshake_error.as_deref().unwrap_or("")
    }

    #[expect(
        clippy::unnecessary_box_returns,
        reason = "cxx passes opaque Rust types by Box"
    )]
    pub fn take_body(&self) -> Box<RustBody> {
        Box::new(
            self.body
                .borrow_mut()
                .take()
                .map_or_else(RustBody::empty, |body| {
                    RustBody::new(body, self.abandoned.clone())
                }),
        )
    }

    fn take_upgraded(&self) -> crate::Result<BoxIo> {
        let upgraded = self
            .upgraded
            .borrow_mut()
            .take()
            .ok_or_else(|| failed("not an upgrade"))?;
        Ok(Box::new(upgraded))
    }

    pub fn take_websocket(&self, compression: &WsCompression) -> crate::Result<Box<RustWebSocket>> {
        Ok(Box::new(RustWebSocket::new(
            self.take_upgraded()?,
            Role::Client,
            compression,
            self.hangup.clone(),
        )))
    }

    pub fn take_tunnel(&self) -> crate::Result<Box<RustIo>> {
        Ok(Box::new(RustIo::new(
            self.take_upgraded()?,
            self.hangup.clone(),
        )))
    }
}
