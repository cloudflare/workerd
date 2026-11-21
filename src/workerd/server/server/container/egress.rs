// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A container's egress: the mappings a Worker installs (`interceptOutboundHttp()` and friends)
//! and the listener the egress sidecar (`proxy-everything`) sends the container's outbound
//! connections to.
//!
//! The sidecar intercepts every connection the container opens and sends it here as an HTTP
//! CONNECT for the original destination, with the TLS SNI (`X-Tls-Sni`) or the HTTP `Host`
//! (`X-Hostname`) it saw as a hint. A connection that matches a mapping is accepted with `200`
//! and the HTTP inside the tunnel is served here, each request forwarded to the mapping's
//! channel; one that matches a TCP mapping is accepted with `202` and handed whole to the
//! channel's `connect()`; anything else is accepted with `202`, which tells the sidecar to send
//! bytes as they are, and passed through to the destination when the container may reach the
//! internet (else closed).
//!
//! An HTTP mapping matches by hostname glob or by IP range; it is looked up again for every
//! request in a tunnel, so a mapping replaced while the tunnel is open takes effect at once.

use std::cell::Cell;
use std::cell::RefCell;
use std::future::pending;
use std::net::IpAddr;
use std::net::Ipv4Addr;
use std::net::Ipv6Addr;
use std::pin::Pin;
use std::pin::pin;
use std::rc::Rc;

use cxx::KjExceptionType;
use futures::FutureExt;
use futures::StreamExt;
use futures::TryFutureExt;
use futures::future::Either;
use futures::future::LocalBoxFuture;
use futures::future::RemoteHandle;
use futures::future::select;
use futures::stream::FuturesUnordered;
use ipnet::IpNet;
use kj::http::HeaderId;
use kj_hyper::io_kj_error;
use kj_hyper::server::Connect;
use kj_hyper::server::Handler;
use kj_hyper::server::ServerSettings;
use kj_hyper::server::Shutdown;
use kj_hyper::server::Upgrading;
use kj_rs::KjOwn;
use kj_rs_io::Socket;
use kj_rs_io::TokioAddress;
use kj_rs_io::TokioListener;
use tokio::io::AsyncWriteExt;
use worker::AsyncInputStream;
use worker::CxxWorkerInterface;
use worker::Headers;
use worker::HeadersRef;
use worker::Method;
use worker::Service;
use worker::ServiceResponse;

use crate::Error;
use crate::Result;
use crate::bridge::ffi;
use crate::config::Factory;
use crate::services::header_table;
use crate::services::send_error;

/// How an egress mapping proxies the connections it matches.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Protocol {
    /// HTTP inside the CONNECT tunnel, each request forwarded to the channel's `request()`.
    Http,
    /// The same, with TLS intercepted by the sidecar (whose CA certificate the container trusts).
    Https,
    /// Raw bytes, forwarded to the channel's `connect()`.
    Tcp,
}

/// What a mapping matches: destination IPs in a range, or HTTP hostnames / TLS SNIs by glob.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Destination {
    Cidr(IpNet),
    /// Lowercased.
    Hostname(String),
}

/// A mapping's channel: the one a decoded channel token names.
pub type Channel = Rc<KjOwn<ffi::EgressChannel>>;

struct Mapping<C> {
    destination: Destination,
    /// Zero matches every port.
    port: u16,
    protocol: Protocol,
    channel: C,
}

/// A container's egress mappings, in the order they were first installed.
pub struct Mappings<C>(Vec<Mapping<C>>);

impl<C> Default for Mappings<C> {
    fn default() -> Self {
        Self(Vec::new())
    }
}

impl<C: Clone> Mappings<C> {
    /// Installs a mapping, replacing the channel of one with the same destination, port and
    /// protocol.
    pub fn upsert(&mut self, destination: Destination, port: u16, protocol: Protocol, channel: C) {
        if let Some(existing) = self
            .0
            .iter_mut()
            .find(|m| m.port == port && m.protocol == protocol && m.destination == destination)
        {
            existing.channel = channel;
            return;
        }
        self.0.push(Mapping {
            destination,
            port,
            protocol,
            channel,
        });
    }

    /// Whether any mapping intercepts TLS, so the container must trust the sidecar's CA.
    #[must_use]
    pub fn has_https(&self) -> bool {
        self.0.iter().any(|m| m.protocol == Protocol::Https)
    }

    /// The hostnames the sidecar's DNS resolves for the container: every hostname glob of a
    /// mapping, once each, and `*` once a mapping by IP range comes up (the destination of such
    /// a mapping can have any name).
    #[must_use]
    pub fn dns_allow_hostnames(&self) -> Vec<String> {
        let mut result: Vec<String> = Vec::new();
        for mapping in &self.0 {
            match &mapping.destination {
                Destination::Cidr(_) => {
                    result.push("*".to_owned());
                    return result;
                }
                Destination::Hostname(glob) => {
                    if !result.contains(glob) {
                        result.push(glob.clone());
                    }
                }
            }
        }
        result
    }

    /// The channel of the first mapping of `protocol` that matches `dest_addr` (`host:port`;
    /// `default_port` when it names none) or `hostname`.
    ///
    /// # Errors
    ///
    /// A `hostname` that is not one, or a range mapping that comes up for a host that is not an
    /// IP address.
    pub fn find(
        &self,
        dest_addr: &str,
        default_port: u16,
        hostname: Option<&str>,
        protocol: Protocol,
    ) -> Result<Option<C>> {
        let (host, port) = strip_port(dest_addr)?;
        let port = port.unwrap_or(default_port);
        let hostname = hostname.map(normalize_hostname).transpose()?;
        for mapping in &self.0 {
            if mapping.protocol != protocol || (mapping.port != 0 && mapping.port != port) {
                continue;
            }
            let matched = match &mapping.destination {
                Destination::Cidr(range) => cidr_matches(range, &host)?,
                Destination::Hostname(glob) => hostname
                    .as_deref()
                    .is_some_and(|hostname| hostname_glob_matches(glob, hostname)),
            };
            if matched {
                return Ok(Some(mapping.channel.clone()));
            }
        }
        Ok(None)
    }

    /// What to do with a CONNECT from the sidecar to `dest`, given its `X-Tls-Sni` and
    /// `X-Hostname` hints.
    ///
    /// The TLS hint goes first, then the HTTP one: a hint that a mapping of its protocol
    /// matches intercepts the tunnel, and a hint that none matches passes it through. Without
    /// either, a TCP mapping of the address and port takes the tunnel (raw TCP carries no
    /// hostname); with no mapping at all, it passes through.
    ///
    /// # Errors
    ///
    /// As [`Self::find`].
    pub fn route(
        &self,
        dest: &str,
        tls_sni: Option<&str>,
        hostname: Option<&str>,
    ) -> Result<Route<C>> {
        for (hint, default_port, protocol) in [
            (tls_sni, 443, Protocol::Https),
            (hostname, 80, Protocol::Http),
        ] {
            if self.find(dest, default_port, hint, protocol)?.is_some() {
                return Ok(Route::Intercept {
                    protocol,
                    default_port,
                    hostname: hint.map(str::to_owned),
                });
            }
            if hint.is_some() {
                return Ok(Route::PassThrough);
            }
        }
        Ok(match self.find(dest, 0, None, Protocol::Tcp)? {
            Some(channel) => Route::Tcp(channel),
            None => Route::PassThrough,
        })
    }
}

/// What the egress listener does with one CONNECT ([`Mappings::route`]).
#[derive(Debug, PartialEq, Eq)]
pub enum Route<C> {
    /// Serve the tunnel's HTTP, each request going to the channel of the mapping of `protocol`
    /// that matches when it arrives.
    Intercept {
        protocol: Protocol,
        /// The port of a destination without one.
        default_port: u16,
        hostname: Option<String>,
    },
    /// Hand the tunnel to the channel's `connect()`.
    Tcp(C),
    /// Connect the tunnel to the destination itself, if the container may reach the internet.
    PassThrough,
}

impl<C> Route<C> {
    /// The answer to the CONNECT. 200 makes the sidecar speak HTTP into the tunnel (decrypted
    /// TLS, for HTTPS); 202 makes it send the bytes as they are, which may carry TLS.
    #[must_use]
    pub fn status(&self) -> (u32, &'static str) {
        match self {
            Self::Intercept { .. } => (200, "OK"),
            Self::Tcp(_) | Self::PassThrough => (202, "Accepted"),
        }
    }
}

/// A container's egress state, shared by its client and its egress listener.
#[derive(Default)]
pub struct Egress {
    pub mappings: RefCell<Mappings<Channel>>,
    /// Whether the container may reach the internet, once a `start()` has said.
    pub internet: Cell<Option<bool>>,
}

impl Egress {
    fn find(
        &self,
        dest_addr: &str,
        default_port: u16,
        hostname: Option<&str>,
        protocol: Protocol,
    ) -> Result<Option<Channel>> {
        self.mappings
            .borrow()
            .find(dest_addr, default_port, hostname, protocol)
    }
}

// =======================================================================================
// Addresses

/// `host[:port]`: a bracketed IPv6 host loses its brackets; a host with more than one colon and
/// no brackets is IPv6 without a port.
///
/// # Errors
///
/// An unclosed bracket, or a bad port after a bracketed host.
pub fn strip_port(text: &str) -> Result<(String, Option<u16>)> {
    if text.starts_with('[') {
        let Some(close) = text.rfind(']') else {
            kj::fail_require!("Unclosed '[' in address string.; str = {text}");
        };
        let host = &text[1..close];
        let after = &text[close + 1..];
        if after.is_empty() {
            return Ok((host.to_owned(), None));
        }
        let Some(port) = after.strip_prefix(':') else {
            kj::fail_require!("Expected port suffix after ']'.; str = {text}");
        };
        let Ok(port) = port.parse::<u16>() else {
            kj::fail_require!("Invalid port number.; str = {text}");
        };
        return Ok((host.to_owned(), Some(port)));
    }
    if let Some(colon) = text.rfind(':')
        && let Ok(port) = text[colon + 1..].parse::<u16>()
        && !text[..colon].contains(':')
    {
        return Ok((text[..colon].to_owned(), Some(port)));
    }
    Ok((text.to_owned(), None))
}

/// The range `host` names, as kj's `CidrRange` parses one: `addr/bits`, or one address (`/32`
/// or `/128` by whether it has a colon). Bits past the prefix are cleared.
fn parse_cidr(host: &str) -> Option<IpNet> {
    let (address, bits) = match host.split_once('/') {
        Some((address, bits)) => (address, Some(bits.parse::<u8>().ok()?)),
        None => (host, None),
    };
    let net = if host.contains(':') {
        let address: Ipv6Addr = address.parse().ok()?;
        IpNet::new(address.into(), bits.unwrap_or(128)).ok()?
    } else {
        let address: Ipv4Addr = address.parse().ok()?;
        IpNet::new(address.into(), bits.unwrap_or(32)).ok()?
    };
    Some(net.trunc())
}

/// A mapping's destination and port, from the `host[:port]` a Worker gave: an IP address or
/// range (IPv6 bracketed when a port follows), else a hostname glob, lowercased.
///
/// # Errors
///
/// An empty host, or a malformed address (see [`strip_port`]).
pub fn parse_host_port(text: &str) -> Result<(Destination, Option<u16>)> {
    let (host, port) = strip_port(text)?;
    if host.is_empty() {
        kj::fail_require!("Host must not be empty.; str = {text}");
    }
    let destination = parse_cidr(&host).map_or_else(
        || Destination::Hostname(host.to_ascii_lowercase()),
        Destination::Cidr,
    );
    Ok((destination, port))
}

/// Whether `range` holds the address `host`. An IPv4 range holds the IPv4-mapped IPv6 form of its
/// addresses too.
fn cidr_matches(range: &IpNet, host: &str) -> Result<bool> {
    let Ok(address) = host.parse::<IpAddr>() else {
        kj::fail_require!("Invalid IP address; addr = {host}");
    };
    Ok(match (range, address) {
        (IpNet::V4(range), IpAddr::V4(address)) => range.contains(&address),
        (IpNet::V4(range), IpAddr::V6(address)) => address
            .to_ipv4_mapped()
            .is_some_and(|address| range.contains(&address)),
        (IpNet::V6(range), IpAddr::V6(address)) => range.contains(&address),
        (IpNet::V6(_), IpAddr::V4(_)) => false,
    })
}

/// A hostname hint as a URL's host normalizes it (lowercase, IDNA, no port).
///
/// # Errors
///
/// A hint that is not a host.
pub fn normalize_hostname(hostname: &str) -> Result<String> {
    url::Url::parse(&format!("http://{hostname}"))
        .ok()
        .and_then(|url| url.host_str().map(str::to_owned))
        .ok_or_else(|| kj::failed!("Invalid X-Hostname URL hint.; hostname = {hostname}"))
}

/// Whether `hostname` (normalized) matches `pattern`, where `*` matches any run of characters:
/// `cloudflare.*.com`, `cloudflare.com`, `*`.
#[must_use]
pub fn hostname_glob_matches(pattern: &str, hostname: &str) -> bool {
    let (pattern, hostname) = (pattern.as_bytes(), hostname.as_bytes());
    let (mut p, mut h) = (0, 0);
    let mut restart = 0;
    let mut star: Option<usize> = None;
    while h < hostname.len() {
        if p < pattern.len() && pattern[p] == b'*' {
            star = Some(p);
            p += 1;
            restart = h;
            continue;
        }
        if p < pattern.len() && pattern[p] == hostname[h] {
            p += 1;
            h += 1;
            continue;
        }
        if let Some(star) = star {
            p = star + 1;
            restart += 1;
            h = restart;
            continue;
        }
        return false;
    }
    while p < pattern.len() && pattern[p] == b'*' {
        p += 1;
    }
    p == pattern.len()
}

// =======================================================================================
// The listener

/// The egress listener of a container, which the sidecar connects to. Dropping it closes the
/// listener and every tunnel through it.
pub struct Listener {
    pub port: u16,
    _serve: RemoteHandle<()>,
}

/// Starts the egress listener on `address:port` (`port` 0 for one the system picks).
///
/// The address is the Docker bridge's gateway, which is not always an address of the host: under
/// WSL with Docker Desktop it lives in Docker's VM. Where it cannot be bound, the listener falls
/// back to loopback, which the sidecar reaches through `host-gateway`.
///
/// # Errors
///
/// An address that does not parse, or nothing to bind.
pub async fn listen(
    factory: &Rc<Factory>,
    egress: Rc<Egress>,
    address: &str,
    port: u16,
) -> Result<Listener> {
    let parsed = TokioAddress::parse_str(&format!("{address}:{port}"), 0)
        .await
        .map_err(Error::from)?;
    let listener = match parsed.listen() {
        Ok(listener) => listener,
        Err(e) => {
            tracing::warn!(
                "Could not bind egress listener to gateway address, falling back to loopback; \
                 listenAddress = {address}; e = {}",
                Error::from(e).description()
            );
            TokioAddress::parse_str(&format!("127.0.0.1:{port}"), 0)
                .await
                .map_err(Error::from)?
                .listen()
                .map_err(Error::from)?
        }
    };
    let port = listener.port().map_err(Error::from)?;
    let serve = factory.spawn(serve(Rc::clone(factory), egress, listener));
    Ok(Listener {
        port,
        _serve: serve,
    })
}

/// Accepts connections until the listener fails, serving each alongside.
async fn serve(factory: Rc<Factory>, egress: Rc<Egress>, listener: Box<TokioListener>) {
    let settings = Rc::new(ServerSettings::default());
    let mut connections = FuturesUnordered::<LocalBoxFuture<'static, ()>>::new();
    loop {
        let accepted = {
            let accept = pin!(listener.accept());
            let finish = pin!(async {
                if connections.is_empty() {
                    pending::<()>().await;
                }
                connections.next().await;
            });
            match select(accept, finish).await {
                Either::Left((accepted, _)) => Some(accepted),
                Either::Right(_) => None,
            }
        };
        match accepted {
            Some(Ok((socket, _))) => {
                let handler: Rc<dyn Handler> = Rc::new(EgressConnection {
                    factory: Rc::clone(&factory),
                    egress: Rc::clone(&egress),
                });
                connections.push(serve_socket(
                    Rc::clone(&factory),
                    socket,
                    Rc::clone(&settings),
                    handler,
                ));
            }
            Some(Err(e)) => {
                tracing::error!(
                    "Workerd could not listen in the TCP port to proxy traffic off the docker \
                     container: {}",
                    Error::from(e).description()
                );
                return;
            }
            None => {}
        }
    }
}

fn serve_socket(
    factory: Rc<Factory>,
    socket: Socket,
    settings: Rc<ServerSettings>,
    handler: Rc<dyn Handler>,
) -> LocalBoxFuture<'static, ()> {
    async move {
        let table = header_table(&factory);
        let shutdown = Shutdown::default();
        let result = match socket {
            Socket::Tcp(stream) => {
                let hangup = kj_rs_io::when_write_disconnected(&stream)
                    .err_into()
                    .boxed();
                kj_hyper::server::serve_connection(
                    stream, hangup, table, settings, handler, &shutdown,
                )
                .await
            }
            #[cfg(unix)]
            Socket::Unix(stream) => {
                let hangup = kj_rs_io::when_write_disconnected(&stream)
                    .err_into()
                    .boxed();
                kj_hyper::server::serve_connection(
                    stream, hangup, table, settings, handler, &shutdown,
                )
                .await
            }
        };
        if let Err(e) = result {
            log_failure("egress connection failed", &e);
        }
    }
    .boxed_local()
}

/// Logs a failure unless the peer just went away.
fn log_failure(what: &str, error: &Error) {
    if error.exception_type() != KjExceptionType::Disconnected {
        tracing::error!("{what}: {}", error.description());
    }
}

/// One connection from the sidecar, which sends nothing but CONNECTs.
struct EgressConnection {
    factory: Rc<Factory>,
    egress: Rc<Egress>,
}

#[async_trait::async_trait(?Send)]
impl Handler for EgressConnection {
    async fn request<'a>(
        &'a self,
        _method: Method,
        _url: &'a [u8],
        _headers: HeadersRef<'a>,
        _body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let headers = Headers::new(header_table(&self.factory));
        send_error(response, 405, "Method Not Allowed", &headers).await
    }

    async fn connect<'a>(
        &'a self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connect: Connect,
    ) -> Result<()> {
        let dest = String::from_utf8_lossy(host).into_owned();
        let hint = |name| {
            headers
                .get_by_name(name)
                .map(|value| String::from_utf8_lossy(value).into_owned())
        };
        let route = self.egress.mappings.borrow().route(
            &dest,
            hint("X-Tls-Sni").as_deref(),
            hint("X-Hostname").as_deref(),
        )?;
        let (status, text) = route.status();
        let tunnel = connect.accept(
            status,
            text,
            HeadersRef::from(&Headers::new(header_table(&self.factory))),
        )?;
        match route {
            Route::Intercept {
                protocol,
                default_port,
                hostname,
            } => {
                self.serve_tunnel(tunnel, &dest, protocol, default_port, hostname)
                    .await
            }
            Route::Tcp(channel) => {
                ffi::egress_channel_connect(self.factory.raw(), &channel, &dest, tunnel.into_kj())
                    .await?;
                Ok(())
            }
            Route::PassThrough => self.pass_through(&dest, tunnel).await,
        }
    }
}

impl EgressConnection {
    /// Serves the HTTP inside an intercepted tunnel.
    async fn serve_tunnel(
        &self,
        tunnel: Upgrading,
        dest: &str,
        protocol: Protocol,
        default_port: u16,
        hostname: Option<String>,
    ) -> Result<()> {
        let hangup = tunnel.hangup().boxed();
        let handler: Rc<dyn Handler> = Rc::new(TunnelRequests {
            egress: Rc::clone(&self.egress),
            dest: dest.to_owned(),
            default_port,
            hostname,
            protocol,
        });
        kj_hyper::server::serve_connection(
            tunnel,
            hangup,
            header_table(&self.factory),
            Rc::new(ServerSettings::default()),
            handler,
            &Shutdown::default(),
        )
        .await?;
        Ok(())
    }

    /// The tunnel connected to `dest` itself if the container may reach the internet, else
    /// closed.
    async fn pass_through(&self, dest: &str, mut tunnel: Upgrading) -> Result<()> {
        if !self.egress.internet.get().unwrap_or(false) {
            return tunnel.shutdown().await.map_err(|e| io_kj_error(&e));
        }
        let address = TokioAddress::parse_str(dest, 0)
            .await
            .map_err(Error::from)?;
        let copied = match address.connect_first().await.map_err(Error::from)? {
            Socket::Tcp(mut stream) => {
                tokio::io::copy_bidirectional(&mut tunnel, &mut stream).await
            }
            #[cfg(unix)]
            Socket::Unix(mut stream) => {
                tokio::io::copy_bidirectional(&mut tunnel, &mut stream).await
            }
        };
        copied.map(|_| ()).map_err(|e| io_kj_error(&e))
    }
}

/// The HTTP requests inside one intercepted tunnel, each forwarded to the channel of the mapping
/// that matches when it arrives.
struct TunnelRequests {
    egress: Rc<Egress>,
    dest: String,
    default_port: u16,
    hostname: Option<String>,
    protocol: Protocol,
}

impl TunnelRequests {
    /// The URL the Worker sees: a request target that is only a path is resolved against the
    /// `Host` header, or the destination address without one.
    fn url_for_worker(&self, target: &str, headers: HeadersRef<'_>) -> String {
        if !target.starts_with('/') {
            return target.to_owned();
        }
        let scheme = if self.protocol == Protocol::Https {
            "https://"
        } else {
            "http://"
        };
        let base = match headers.get(HeaderId::HOST) {
            Some(host) => format!("{scheme}{}", String::from_utf8_lossy(host)),
            None => format!("{scheme}{}", self.dest),
        };
        url::Url::parse(&base)
            .and_then(|parsed| parsed.join(target))
            .map_or_else(|_| format!("{base}{target}"), String::from)
    }
}

#[async_trait::async_trait(?Send)]
impl Handler for TunnelRequests {
    async fn request<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        // Looked up for each request: a mapping replaced while the tunnel is open applies.
        let channel = self
            .egress
            .find(
                &self.dest,
                self.default_port,
                self.hostname.as_deref(),
                self.protocol,
            )?
            .ok_or_else(|| kj::failed!("egress mapping disappeared during active tunnel"))?;
        let target = String::from_utf8_lossy(url);
        let url = self.url_for_worker(&target, headers);
        let mut worker = CxxWorkerInterface::new(ffi::egress_channel_start_request(&channel)?);
        // The handler fails with a `KjError`, which keeps the exception's type, description,
        // location and details, but not its context.
        worker
            .request(method, url.as_bytes(), headers, body, response)
            .await?;
        Ok(())
    }

    async fn connect<'a>(
        &'a self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connect: Connect,
    ) -> Result<()> {
        Err(kj::not_implemented!(
            "CONNECT is not implemented by this HttpService."
        ))
    }
}

#[cfg(test)]
#[path = "egress-test.rs"]
mod tests;
