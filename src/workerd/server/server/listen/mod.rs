// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The sockets of the config and the listeners over them: HTTP and HTTPS (hyper, through
//! kj-hyper), TCP and UDP (`connect()` events), and the debug port.
//!
//! Sockets are kj-rs-io's (`TokioAddress`: KJ's address grammar, resolver, socket options and
//! accept retries over tokio) but for a `loopback:name` address, which names a queue of this
//! server's registry (`listen::loopback`). They are bound before the services start so that a
//! worker can learn its own address. A listener is one future: its accept loop and every
//! connection it accepted, so that `run()` can wait for the connections to finish once draining
//! has stopped the accept loop. A failure of the accept loop itself is the listener's failure,
//! and fatal to the server; a failure of one connection is logged and ends that connection only.

pub mod loopback;
pub mod udp;

use std::collections::HashMap;
use std::future::Future;
use std::net::SocketAddr;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::Arc;

use cxx::KjExceptionType;
use futures::FutureExt;
use futures::StreamExt;
use futures::TryFutureExt;
use futures::future::Either;
use futures::stream::FuturesUnordered;
use kj_hyper::Hangup;
use kj_hyper::server::Connect;
use kj_hyper::server::Handler;
use kj_hyper::server::ServerSettings;
use kj_hyper::server::Shutdown;
use kj_rs::KjMaybe;
use kj_rs_io::Socket;
use kj_rs_io::TokioAddress;
use tokio::net::UdpSocket;
use tokio::sync::watch;
use worker::ConnectResponse;
use worker::ConnectSettings;
use worker::CxxWorkerInterface;
use worker::Headers;
use worker::HeadersRef;
use worker::Method;
use worker::Service;
use worker::ServiceResponse;
use workerd_capnp::config;
use workerd_capnp::socket;

use crate::Result;
use crate::bridge;
use crate::bridge::ffi;
use crate::channels::AbortReason;
use crate::channels::Channel;
use crate::channels::SubrequestChannel;
use crate::config::Factory;
use crate::config::Reporter;
use crate::config::capnp_error;
use crate::config::text;
use crate::listen::loopback::Loopback;
use crate::listen::loopback::LoopbackListener;
use crate::listen::loopback::LoopbackStream;
use crate::services::header_table;
use crate::services::rewriter::HttpRewriter;
use crate::services::send_error;

// =====================================================================================
// Addresses

/// The host of a listen address, without its port, for a worker's inbound listener info and the
/// authority of a TCP socket's `connect()`. Unix addresses are returned whole.
#[must_use]
pub fn host_of_address(address: &str) -> String {
    if address.starts_with("unix:") {
        return address.to_owned();
    }
    if let Some(colon) = address.rfind(':')
        && (address.starts_with('[') || address.find(':') == Some(colon))
    {
        // A bare IPv6 literal without brackets contains colons but no port.
        return address[..colon].to_owned();
    }
    address.to_owned()
}

fn default_port_for(sock: socket::Reader<'_>) -> u16 {
    match sock.which() {
        Ok(socket::Which::Http(_)) => 80,
        Ok(socket::Which::Https(_)) => 443,
        _ => 0,
    }
}

// =====================================================================================
// Sockets

/// A connection-oriented listening socket: kj-rs-io's over the sockets the address names, or
/// the queue of a `loopback:` name.
pub enum StreamListener {
    Socket(Box<kj_rs_io::TokioListener>),
    Loopback(LoopbackListener),
}

/// An accepted connection and, of a TCP one, who it came from.
pub enum Accepted {
    Socket(Socket, Option<SocketAddr>),
    Loopback(LoopbackStream),
}

impl StreamListener {
    /// The port of the first socket; 0 for a Unix or loopback socket, as KJ reports it.
    pub fn port(&self) -> Result<u16> {
        match self {
            Self::Socket(listener) => Ok(listener.port()?),
            Self::Loopback(_) => Ok(0),
        }
    }

    /// The next connection. Dropping the future accepts nothing.
    pub async fn accept(&self) -> Result<Accepted> {
        match self {
            Self::Socket(listener) => {
                let (socket, peer) = listener.accept().await?;
                Ok(Accepted::Socket(socket, peer))
            }
            Self::Loopback(listener) => Ok(Accepted::Loopback(listener.accept().await?)),
        }
    }
}

/// A socket of the config, bound.
pub enum BoundSocket {
    Stream(StreamListener),
    Datagram(UdpSocket),
}

/// Listens on `address`; a loopback address takes its name's listener from `loopback`.
pub async fn listen(
    address: &str,
    default_port: u16,
    loopback: &Loopback,
) -> Result<StreamListener> {
    if let Some(name) = address.strip_prefix("loopback:") {
        return loopback.listen(name).map(StreamListener::Loopback);
    }
    let address = TokioAddress::parse_str(address, default_port).await?;
    Ok(StreamListener::Socket(address.listen()?))
}

/// Binds the datagram socket `address` names.
async fn bind_udp(address: &str, default_port: u16) -> Result<UdpSocket> {
    let address = TokioAddress::parse_str(address, default_port).await?;
    Ok(address.bind_udp()?)
}

/// A listening socket inherited from the CLI (`--socket-fd`) as a listener; its family decides
/// what it accepts.
fn wrap_inherited(socket: socket2::Socket) -> Result<StreamListener> {
    socket
        .set_nonblocking(true)
        .map_err(|e| kj::failed!("--socket-fd: {e}"))?;
    Ok(StreamListener::Socket(kj_rs_io::wrap_listener(socket)?))
}

// =====================================================================================
// Binding

/// The config's sockets, bound. `sockets[i]` is `None` for a socket that could not be bound: the
/// error was reported.
pub struct BoundSockets {
    pub sockets: Vec<Option<(BoundSocket, String)>>,
    /// The TCP sockets by the service they serve, for `Worker::Api::getInboundListeners()`.
    pub inbound: HashMap<String, Vec<ffi::InboundListener>>,
}

/// Binds every socket of the config, taking the CLI's overrides for it out of `addresses` and
/// `inherited` (what is left in them matched no socket). Bind failures are config errors.
#[expect(
    clippy::implicit_hasher,
    reason = "the maps are the command line's, with the default hasher"
)]
pub async fn bind_sockets(
    config: config::Reader<'_>,
    addresses: &mut HashMap<String, String>,
    inherited: &mut HashMap<String, socket2::Socket>,
    loopback: &Loopback,
    report: &Reporter,
) -> Result<BoundSockets> {
    let mut bound = BoundSockets {
        sockets: Vec::new(),
        inbound: HashMap::new(),
    };
    for sock in config.get_sockets().map_err(capnp_error)? {
        let name = text(sock.get_name())?;
        let is_udp = matches!(sock.which(), Ok(socket::Which::Udp(_)));
        let inherited_listener = inherited.remove(&name);
        let address = if let Some(address) = addresses.remove(&name) {
            address
        } else if inherited_listener.is_some() {
            String::new()
        } else if sock.has_address() {
            text(sock.get_address())?
        } else {
            report.error(format!(
                "Socket \"{name}\" has no address in the config, so must be specified on the \
                 command line with `--socket-addr`."
            ));
            bound.sockets.push(None);
            continue;
        };

        if is_udp {
            if inherited_listener.is_some() {
                report.error(format!(
                    "Socket \"{name}\" is a UDP socket; --socket-fd overrides (which pass a \
                     listening connection-oriented socket) are not supported for it."
                ));
                bound.sockets.push(None);
                continue;
            }
            let socket = match bind_udp(&address, default_port_for(sock)).await {
                Ok(socket) => socket,
                Err(e) => {
                    report.error(format!("Socket \"{name}\": {}", e.description()));
                    bound.sockets.push(None);
                    continue;
                }
            };
            bound
                .sockets
                .push(Some((BoundSocket::Datagram(socket), address)));
            continue;
        }

        let listener = match inherited_listener {
            Some(socket) => wrap_inherited(socket),
            None => listen(&address, default_port_for(sock), loopback).await,
        };
        let listener = match listener {
            Ok(listener) => listener,
            Err(e) => {
                report.error(format!("Socket \"{name}\": {}", e.description()));
                bound.sockets.push(None);
                continue;
            }
        };

        // A loopback socket is not a listener another process could reach.
        let designator = sock.get_service().map_err(capnp_error)?;
        if matches!(sock.which(), Ok(socket::Which::Tcp(_)))
            && !matches!(listener, StreamListener::Loopback(_))
            && designator.has_name()
        {
            let service = text(designator.get_name())?;
            bound
                .inbound
                .entry(service)
                .or_default()
                .push(ffi::InboundListener {
                    protocol: "tcp".to_owned(),
                    address: host_of_address(&address),
                    port: listener.port()?,
                });
        }
        bound
            .sockets
            .push(Some((BoundSocket::Stream(listener), address)));
    }
    Ok(bound)
}

/// A bound socket's port, for the control channel's `listen` event.
pub fn bound_port(socket: &BoundSocket) -> Result<u16> {
    match socket {
        BoundSocket::Stream(listener) => listener.port(),
        BoundSocket::Datagram(socket) => socket
            .local_addr()
            .map(|addr| addr.port())
            .map_err(|e| kj::failed!("getsockname: {e}")),
    }
}

// =====================================================================================
// Connections

/// Any tokio byte stream hyper can serve.
trait Io: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin + Send {}
impl<T: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin + Send + ?Sized> Io for T {}

/// A connection's transport once TLS (if any) is done: the tokio stream the listener accepted,
/// or the TLS stream over it.
type Transport = Pin<Box<dyn Io>>;

/// The cf blob describing a peer that sent none itself: its address for a network peer, its
/// process credentials for a local one. A loopback peer has no identity to describe.
fn peer_cf_blob(accepted: &Accepted) -> Option<String> {
    match accepted {
        Accepted::Socket(_, Some(peer)) => {
            Some(serde_json::json!({ "clientIp": peer.to_string() }).to_string())
        }
        #[cfg(unix)]
        Accepted::Socket(Socket::Unix(stream), None) => {
            let mut blob = serde_json::Map::new();
            if let Ok(cred) = stream.peer_cred() {
                if let Some(pid) = cred.pid() {
                    blob.insert("clientPid".to_owned(), pid.into());
                }
                blob.insert("clientUid".to_owned(), cred.uid().into());
            }
            Some(serde_json::Value::Object(blob).to_string())
        }
        Accepted::Socket(..) | Accepted::Loopback(_) => None,
    }
}

/// Wraps the connection in TLS when the socket has it, and boxes the transport. Draining drops
/// a connection that is still shaking hands, as it drops one not yet accepted.
async fn transport(
    accepted: Accepted,
    tls: Option<&Arc<rustls::ServerConfig>>,
    context: &ListenContext,
) -> Result<Transport> {
    let io: Transport = match accepted {
        Accepted::Socket(Socket::Tcp(stream), _) => Box::pin(stream),
        #[cfg(unix)]
        Accepted::Socket(Socket::Unix(stream), _) => Box::pin(stream),
        Accepted::Loopback(stream) => Box::pin(stream),
    };
    let Some(config) = tls else {
        return Ok(io);
    };
    let handshake = std::pin::pin!(kj_hyper::tls::accept(io, Arc::clone(config)));
    match futures::future::select(handshake, std::pin::pin!(context.drained())).await {
        Either::Left((stream, _)) => Ok(Box::pin(stream?)),
        Either::Right(_) => Err(kj::disconnected!("the server is draining")),
    }
}

/// Resolves once the peer of `accepted` is gone, as `whenWriteDisconnected()` of kj's own stream
/// over it would: a socket hung up or failed (not a peer that shut down its side), a loopback
/// connection's other end dropped. A socket's costs a descriptor for as long as the
/// connection is served (kj-rs-io/stream.rs, "whenWriteDisconnected costs a descriptor").
fn hangup(accepted: &Accepted) -> Hangup {
    match accepted {
        Accepted::Socket(Socket::Tcp(stream), _) => {
            kj_rs_io::when_write_disconnected(stream).err_into().boxed()
        }
        #[cfg(unix)]
        Accepted::Socket(Socket::Unix(stream), _) => {
            kj_rs_io::when_write_disconnected(stream).err_into().boxed()
        }
        Accepted::Loopback(stream) => stream.hangup(),
    }
}

/// Logs a connection's failure the way workerd's `handleApplicationError` does: a peer that went
/// away is not news.
fn log_connection_error(what: &str, error: &crate::Error) {
    if error.exception_type() != KjExceptionType::Disconnected {
        tracing::error!("{what}: {}", error.description());
    }
}

/// Logs a failed HTTP call the way workerd's `handleApplicationError` does, the error as KJ
/// prints an exception; the client gets kj-hyper's bare 500.
fn log_uncaught(error: &crate::Error) {
    if error.exception_type() != KjExceptionType::Disconnected {
        let error = ffi::exception_text(&AbortReason(Some(error.clone())));
        tracing::error!("Uncaught exception: {error}");
    }
}

/// What every listener shares: the factory (for the header table and the RPC bootstrap), and
/// the drain signal. `draining` flips once, when the server stops accepting connections.
pub struct ListenContext {
    pub factory: Rc<Factory>,
    pub draining: watch::Receiver<bool>,
}

impl ListenContext {
    /// Resolves once draining begins.
    async fn drained(&self) {
        let mut draining = self.draining.clone();
        let _ = draining.wait_for(|draining| *draining).await;
    }
}

/// Runs a listener: accepts until draining begins, then waits for the connections it accepted.
/// `accept` yields one connection per call and accepts nothing when dropped unfinished; its
/// failure is the listener's. A connection's failure is logged as `what`.
async fn accept_loop<A, C>(
    context: &ListenContext,
    what: &str,
    mut accept: impl FnMut() -> A,
    on_drain: impl FnOnce(),
) -> Result<()>
where
    A: Future<Output = Result<C>>,
    C: Future<Output = Result<()>> + 'static,
{
    let log = |result: Result<()>| {
        if let Err(e) = result {
            log_connection_error(what, &e);
        }
    };
    let mut connections = FuturesUnordered::new();
    let mut drained = std::pin::pin!(context.drained().fuse());
    loop {
        let mut accepting = std::pin::pin!(accept().fuse());
        futures::select_biased! {
            () = drained => break,
            result = connections.select_next_some() => log(result),
            connection = accepting => connections.push(connection?.boxed_local()),
        }
    }
    on_drain();
    // Every connection finishes on its own once told to shut down.
    while let Some(result) = connections.next().await {
        log(result);
    }
    Ok(())
}

// =====================================================================================
// HTTP

/// One socket's HTTP settings, shared by its connections.
pub struct HttpSocket {
    pub channel: Rc<dyn Channel>,
    pub rewriter: Rc<HttpRewriter>,
    pub physical_protocol: &'static str,
    pub tls: Option<Arc<rustls::ServerConfig>>,
}

/// Serves HTTP on `listener` until the server drains.
pub async fn listen_http(
    context: Rc<ListenContext>,
    listener: StreamListener,
    socket: Rc<HttpSocket>,
) -> Result<()> {
    // The application negotiates WebSocket compression itself.
    let settings = Rc::new(ServerSettings {
        websocket_errors: Some(ffi::new_jsgify_websocket_errors()),
        websocket_compression: kj_hyper::WebSocketCompression::MANUAL,
        ..ServerSettings::default()
    });
    let shutdown = Rc::new(Shutdown::new());
    let serve = |accepted: Accepted| {
        let context = Rc::clone(&context);
        let socket = Rc::clone(&socket);
        let settings = Rc::clone(&settings);
        let shutdown = Rc::clone(&shutdown);
        async move {
            let cf_blob = (!socket.rewriter.has_cf_blob_header())
                .then(|| peer_cf_blob(&accepted))
                .flatten();
            let hangup = hangup(&accepted);
            let io = transport(accepted, socket.tls.as_ref(), &context).await?;
            let handler: Rc<dyn Handler> = Rc::new(HttpConnection {
                context: Rc::clone(&context),
                socket,
                cf_blob,
            });
            let table = header_table(&context.factory);
            kj_hyper::server::serve_connection(io, hangup, table, settings, handler, &shutdown)
                .await
        }
    };
    accept_loop(
        &context,
        "HTTP connection failed",
        || listener.accept().map_ok(&serve),
        || shutdown.shutdown(),
    )
    .await
}

/// One HTTP connection's handler: the socket's service, with the socket's rewriting and the
/// peer's cf blob applied to each request.
struct HttpConnection {
    context: Rc<ListenContext>,
    socket: Rc<HttpSocket>,
    /// Built from the peer identity when the socket's options name no cf blob header.
    cf_blob: Option<String>,
}

impl HttpConnection {
    fn start_request(&self, cf_blob: Option<&str>) -> Result<CxxWorkerInterface> {
        let metadata = ffi::new_request_metadata(cf_blob.into(), KjMaybe::None);
        Ok(CxxWorkerInterface::new(
            self.socket.channel.start_request(metadata)?,
        ))
    }
}

#[async_trait::async_trait(?Send)]
impl Handler for HttpConnection {
    async fn request<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        body: Pin<&'a mut worker::AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        async {
            let table = header_table(&self.context.factory);
            let rewriter = &self.socket.rewriter;
            let url =
                std::str::from_utf8(url).map_err(|_| kj::failed!("request URL is not UTF-8"))?;
            let Some((rewritten, cf_blob)) = rewriter.rewrite_incoming_request(
                table,
                url,
                self.socket.physical_protocol,
                headers,
            )?
            else {
                return send_error(response, 400, "Bad Request", &Headers::new(table)).await;
            };
            let cf_blob = cf_blob.or_else(|| self.cf_blob.clone());
            let mut worker = self.start_request(cf_blob.as_deref())?;
            let headers = rewritten
                .headers
                .as_deref()
                .map_or(headers, HeadersRef::from);
            let mut response =
                bridge::rewriting_response(response.into_ffi(), table, rewriter.response_edits())?;
            worker
                .request(
                    method,
                    rewritten.url.as_bytes(),
                    headers,
                    body,
                    ServiceResponse::from(response.as_mut()),
                )
                .await
        }
        .await
        .inspect_err(log_uncaught)
    }

    async fn connect<'a>(
        &'a self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connect: Connect,
    ) -> Result<()> {
        async {
            let table = header_table(&self.context.factory);
            if let Some(capnp_host) = self.socket.rewriter.capnp_connect_host()
                && capnp_host.as_bytes() == host
            {
                // The client is opening a capnp session.
                let empty = Headers::new(table);
                let tunnel = connect.accept(200, "OK", HeadersRef::from(&empty))?;
                let stream = tunnel.into_kj();
                let target = SubrequestChannel::new(Rc::clone(&self.socket.channel));
                let factory = self.context.factory.raw();
                return Ok(ffi::factory_accept_bootstrap(factory, stream, target).await?);
            }
            let mut worker = self.start_request(self.cf_blob.as_deref())?;
            let (mut tunnel, mut response) = connect.into_kj();
            worker
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
        .await
        .inspect_err(log_uncaught)
    }
}

// =====================================================================================
// TCP

/// Serves raw TCP on `listener`: each connection is a `connect()` event on the socket's service,
/// addressed to `authority` (the socket as bound), until the server drains.
pub async fn listen_tcp(
    context: Rc<ListenContext>,
    listener: StreamListener,
    channel: Rc<dyn Channel>,
    tls: Option<Arc<rustls::ServerConfig>>,
    authority: String,
) -> Result<()> {
    let authority = Rc::new(authority);
    let serve = |accepted: Accepted| {
        let context = Rc::clone(&context);
        let channel = Rc::clone(&channel);
        let tls = tls.clone();
        let authority = Rc::clone(&authority);
        async move {
            let peer = match &accepted {
                Accepted::Socket(_, peer) => peer.map(|peer| peer.to_string()),
                Accepted::Loopback(_) => None,
            };
            let metadata = ffi::new_request_metadata(KjMaybe::None, peer.as_deref().into());
            let mut worker = CxxWorkerInterface::new(channel.start_request(metadata)?);
            let hangup = hangup(&accepted);
            let io = transport(accepted, tls.as_ref(), &context).await?;
            let mut stream = kj_hyper::into_kj_stream_with(io, Some(hangup.shared()));
            let mut response = ffi::new_null_connect_response();
            let headers = Headers::new(header_table(&context.factory));
            worker
                .connect(
                    authority.as_bytes(),
                    HeadersRef::from(&headers),
                    stream.as_mut(),
                    ConnectResponse::from(response.as_mut()),
                    ConnectSettings {
                        use_tls: false,
                        tls_starter: KjMaybe::None,
                    },
                )
                .await
        }
    };
    accept_loop(
        &context,
        "TCP connect() handler threw",
        || listener.accept().map_ok(&serve),
        || {},
    )
    .await
}

// =====================================================================================
// Debug port

/// Serves the workerd debug port on `listener`; the factory resolves its requests through the
/// server. A connection lasts for as long as its client keeps it, so draining drops them all.
pub async fn listen_debug_port(context: Rc<ListenContext>, listener: StreamListener) -> Result<()> {
    let serve = |accepted: Accepted| {
        let context = Rc::clone(&context);
        async move {
            let stream = kj_hyper::into_kj_stream(transport(accepted, None, &context).await?);
            Ok(ffi::factory_accept_debug_port(context.factory.raw(), stream).await?)
        }
    };
    let what = "debug port connection failed";
    let serving = accept_loop(&context, what, || listener.accept().map_ok(&serve), || {});
    let drained = std::pin::pin!(context.drained());
    match futures::future::select(std::pin::pin!(serving), drained).await {
        Either::Left((result, _)) => result,
        Either::Right(_) => Ok(()),
    }
}

#[cfg(test)]
#[path = "mod-test.rs"]
mod tests;
