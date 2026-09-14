//! Tokio-backed `kj::Network` / `kj::NetworkAddress` / `kj::ConnectionReceiver` backends.
//!
//! # Address grammar: KJ's, for what workerd uses
//!
//! `parse` follows `SocketAddress::parse` and `lookupHost` in kj/async-io-unix.c++, so every
//! address string workerd.capnp documents for `Socket.address` / `ExternalServer.address` means
//! the same thing here as under KJ's own backend:
//!
//! - IPv4 / IPv6 literals: `"1.2.3.4"`, `"1.2.3.4:80"`, `"1234:5678::abcd"`, `"[::1]:80"`.
//! - Wildcard (dual-stack): `"*"`, `"*:80"`.
//! - Ports are decimal; a port text that is not a decimal number (`"http"`) is a *service
//!   name*, resolved by `getaddrinfo`. (KJ's `strtoul(..., 0)` grammar -- octal `"010"`, hex
//!   `"0x50"` -- is not reproduced: no configuration relies on it.)
//! - Hostnames go to `getaddrinfo` with KJ's hints (`AF_UNSPEC`, `AI_V4MAPPED | AI_ADDRCONFIG`),
//!   so a host with no IPv6 configured gets no AAAA results and IPv6 scope IDs (`fe80::1%eth0`)
//!   resolve; results are deduplicated in the resolver's order (KJ's `std::set` re-sort is not
//!   reproduced: it is not something any workerd behavior depends on).
//! - Unix domain: `"unix:/path/to/socket"` (the path is arbitrary bytes) and, on Linux,
//!   `"unix-abstract:name"` for the abstract namespace -- both documented for workerd's
//!   `Socket.address` / `ExternalServer.address`.
//!
//! # Typed addresses
//!
//! An address crosses the bridge as [`SocketAddress`] (ffi.rs): a family tag plus the fields
//! that family has. On this side it is built from and read back into `std::net::SocketAddr`
//! and `std::os::unix::net::SocketAddr`, which is all safe code; the C++ adapter is the only
//! place a `struct sockaddr` is ever decoded or encoded, at the two KJ interfaces that speak
//! raw sockaddrs (`getSockaddr`, `getsockname` / `getpeername`) and for KJ's own
//! `NetworkFilter`. Internally an IP address is a `SocketAddr` list and a Unix address is a
//! [`UnixName`]; tokio binds and connects them through its `bind_addr` / `connect_addr`
//! constructors. Nothing is re-derived from a printed form.
//!
//! # Where filtering happens
//!
//! `restrictPeers()` policy is KJ's own `kj::_::NetworkFilter`, applied by the C++ adapter
//! (async-io.c++) exactly where KJ applies it: to each target before `connect()` tries it, and
//! to each accepted peer. This module therefore has no filter type: [`address_targets`] lists
//! the endpoints in order for the adapter to filter and [`connect_target`] connects one;
//! [`listener_accept`] reports the peer with the stream. (KJ also rejects a filtered *literal*
//! at parse time and in `getSockaddr`; that only changes the moment the same error surfaces
//! and is not reproduced.)
//!
//! # Runtime and ownership
//!
//! Every bridged operation here starts with [`ensure_loop_thread`] (lib.rs, "The tokio
//! runtime"): the sockets `poll_accept` materializes register with the runtime the accept future
//! is polled under, as do connects, binds and the resolver's blocking task. Listeners follow the
//! ownership model described in stream.rs: an `accept()` future owns a share of the listener, so
//! destroying the receiver with an accept pending is memory-safe.

use std::future::Future;
use std::net::IpAddr;
use std::net::SocketAddr;
#[cfg(unix)]
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;
use std::task::Context;
use std::task::Poll;

use tokio::net::TcpListener;
use tokio::net::TcpSocket;
use tokio::net::TcpStream;
#[cfg(unix)]
use tokio::net::UnixListener;
#[cfg(unix)]
use tokio::net::UnixStream;

use crate::ensure_loop_thread;
use crate::error::KjIoError;
use crate::error::Result;
use crate::error::op;
use crate::ffi::AddressKind;
use crate::ffi::PeerStream;
use crate::ffi::SocketAddress;
use crate::stream::Socket;
use crate::stream::TokioStream;

/// KJ parity: `::listen(fd, SOMAXCONN)`.
#[cfg(unix)]
const LISTEN_BACKLOG: u32 = libc::SOMAXCONN.unsigned_abs();
/// winsock's `SOMAXCONN` ("a reasonable maximum", 0x7fffffff).
#[cfg(windows)]
const LISTEN_BACKLOG: u32 = 0x7fff_ffff;

// ======================================================================================
// Typed addresses (see the module docs)

impl SocketAddress {
    fn blank(kind: AddressKind) -> Self {
        Self {
            kind,
            ip: [0; 16],
            port: 0,
            flowinfo: 0,
            scope_id: 0,
            name: Vec::new(),
        }
    }
}

impl From<SocketAddr> for SocketAddress {
    fn from(addr: SocketAddr) -> Self {
        match addr {
            SocketAddr::V4(v4) => {
                let mut out = Self::blank(AddressKind::Ipv4);
                out.ip[..4].copy_from_slice(&v4.ip().octets());
                out.port = v4.port();
                out
            }
            SocketAddr::V6(v6) => Self {
                kind: AddressKind::Ipv6,
                ip: v6.ip().octets(),
                port: v6.port(),
                flowinfo: v6.flowinfo(),
                scope_id: v6.scope_id(),
                name: Vec::new(),
            },
        }
    }
}

/// The `SocketAddr` of an IP [`SocketAddress`]; an error for the unix kinds.
fn ip_socket_addr(addr: &SocketAddress) -> Result<SocketAddr> {
    match addr.kind {
        AddressKind::Ipv4 => {
            let octets: [u8; 4] = addr.ip[..4].try_into().map_or([0; 4], |octets| octets);
            Ok(SocketAddr::V4(std::net::SocketAddrV4::new(
                octets.into(),
                addr.port,
            )))
        }
        AddressKind::Ipv6 => Ok(SocketAddr::V6(std::net::SocketAddrV6::new(
            addr.ip.into(),
            addr.port,
            addr.flowinfo,
            addr.scope_id,
        ))),
        _ => Err(KjIoError::other("address", "not an IP socket address")),
    }
}

#[cfg(unix)]
impl From<&std::os::unix::net::SocketAddr> for SocketAddress {
    fn from(addr: &std::os::unix::net::SocketAddr) -> Self {
        use std::os::unix::ffi::OsStrExt;
        if let Some(path) = addr.as_pathname() {
            let mut out = Self::blank(AddressKind::UnixPath);
            out.name = path.as_os_str().as_bytes().to_vec();
            return out;
        }
        #[cfg(target_os = "linux")]
        {
            use std::os::linux::net::SocketAddrExt;
            if let Some(name) = addr.as_abstract_name() {
                let mut out = Self::blank(AddressKind::UnixAbstract);
                out.name = name.to_vec();
                return out;
            }
        }
        Self::blank(AddressKind::UnixUnnamed)
    }
}

/// The name a unix [`SocketAddress`] binds or connects: a pathname, or (Linux) an abstract name.
#[cfg(unix)]
#[derive(Clone, PartialEq, Eq, Debug)]
enum UnixName {
    Path(PathBuf),
    #[cfg(target_os = "linux")]
    Abstract(Vec<u8>),
}

#[cfg(unix)]
impl UnixName {
    /// `unix:` + the path or `unix-abstract:` + the name, byte for byte (a unix path need not be
    /// UTF-8, and neither is a `kj::String`).
    fn display_bytes(&self) -> Vec<u8> {
        use std::os::unix::ffi::OsStrExt;
        match self {
            Self::Path(path) => [b"unix:".as_slice(), path.as_os_str().as_bytes()].concat(),
            #[cfg(target_os = "linux")]
            Self::Abstract(name) => [b"unix-abstract:".as_slice(), name].concat(),
        }
    }

    fn to_socket_address(&self) -> SocketAddress {
        use std::os::unix::ffi::OsStrExt;
        match self {
            Self::Path(path) => {
                let mut out = SocketAddress::blank(AddressKind::UnixPath);
                out.name = path.as_os_str().as_bytes().to_vec();
                out
            }
            #[cfg(target_os = "linux")]
            Self::Abstract(name) => {
                let mut out = SocketAddress::blank(AddressKind::UnixAbstract);
                out.name.clone_from(name);
                out
            }
        }
    }

    /// The typed name behind a unix [`SocketAddress`]; unnamed peers, which nothing can bind or
    /// connect to, and (off Linux) abstract names are errors.
    fn from_socket_address(addr: &SocketAddress) -> Result<Self> {
        use std::os::unix::ffi::OsStrExt;
        match addr.kind {
            AddressKind::UnixPath => Ok(Self::Path(PathBuf::from(std::ffi::OsStr::from_bytes(
                &addr.name,
            )))),
            #[cfg(target_os = "linux")]
            AddressKind::UnixAbstract => Ok(Self::Abstract(addr.name.clone())),
            #[cfg(not(target_os = "linux"))]
            AddressKind::UnixAbstract => Err(KjIoError::other(
                "address",
                "Unix domain socket abstract namespace is only supported on Linux",
            )),
            AddressKind::UnixUnnamed => Err(KjIoError::other(
                "address",
                "an unnamed unix socket address cannot be connected to or listened on",
            )),
            _ => Err(KjIoError::other("address", "not a unix socket address")),
        }
    }

    /// The address tokio's `bind_addr` / `connect_addr` take, built by `std`. `std` enforces the
    /// `sun_path` limit (108 bytes, NUL included for a pathname) and rejects interior NULs, so
    /// nothing is truncated silently.
    fn to_tokio(&self) -> Result<tokio::net::unix::SocketAddr> {
        let addr = match self {
            Self::Path(path) => {
                use std::os::unix::ffi::OsStrExt;
                if path.as_os_str().as_bytes().contains(&0) {
                    // KJ's message.
                    return Err(KjIoError::other(
                        "parseAddress",
                        "Unix domain socket address contains NULL.",
                    ));
                }
                std::os::unix::net::SocketAddr::from_pathname(path).map_err(op("parseAddress"))?
            }
            #[cfg(target_os = "linux")]
            Self::Abstract(name) => {
                use std::os::linux::net::SocketAddrExt;
                std::os::unix::net::SocketAddr::from_abstract_name(name)
                    .map_err(op("parseAddress"))?
            }
        };
        Ok(tokio::net::unix::SocketAddr::from(addr))
    }
}

/// A parsed network address: one or more socket addresses to try in order.
pub struct TokioAddress {
    spec: Spec,
}

#[derive(Clone)]
enum Spec {
    Ip {
        addrs: Vec<SocketAddr>,
        /// `"*"` / `"*:port"`: `listen()` binds dual-stack, `connect()` is an error.
        wildcard: bool,
    },
    #[cfg(unix)]
    Unix(UnixName),
}

// ======================================================================================
// Parsing

/// `SocketAddress::parse`'s split into address and port text: bracketed IPv6 (with an optional
/// `:port` after the *last* `]`), one colon means host:port, two or more colons and no brackets
/// mean a bare IPv6 address with no port.
fn split_host_port(text: &str) -> Result<(&str, Option<&str>)> {
    if text.starts_with('[') {
        let close = text.rfind(']').ok_or_else(|| {
            KjIoError::other(
                "parseAddress",
                format!("Unclosed '[' in address string. {text}"),
            )
        })?;
        let addr = &text[1..close];
        let tail = &text[close + 1..];
        if tail.is_empty() {
            return Ok((addr, None));
        }
        let port = tail.strip_prefix(':').ok_or_else(|| {
            KjIoError::other(
                "parseAddress",
                format!("Expected port suffix after ']'. {text}"),
            )
        })?;
        return Ok((addr, Some(port)));
    }
    match text.find(':') {
        Some(colon) if !text[colon + 1..].contains(':') => {
            Ok((&text[..colon], Some(&text[colon + 1..])))
        }
        _ => Ok((text, None)),
    }
}

/// `getaddrinfo` as KJ's `SocketAddress::lookupHost` calls it: `ai_family = AF_UNSPEC`, no
/// socket type, `ai_flags = AI_V4MAPPED | AI_ADDRCONFIG` (`AI_ADDRCONFIG` alone where
/// `AI_V4MAPPED` breaks the resolver, as on Android), `host` `None` for the wildcard, and
/// `service` either a name to look up or `None` (the caller patches the port in). Blocking:
/// call it on the loop runtime's blocking pool. Failures carry the resolver's own description
/// (KJ's "DNS lookup failed." with `gai_strerror`).
///
/// Why `dns-lookup` and not `tokio::net::lookup_host`: tokio's resolver is `std`'s
/// `ToSocketAddrs` (getaddrinfo with no hints) on the same blocking pool. It has no
/// `AI_ADDRCONFIG`, so `listen()` on a hostname in a container without IPv6 would resolve
/// `::1` and fail the bind instead of skipping it, and it has no service names (`"host:http"`),
/// which workerd's address grammar allows. Both are what workerd relies on KJ for.
fn getaddrinfo(host: Option<&str>, service: Option<&str>) -> Result<Vec<SocketAddr>> {
    #[cfg(all(unix, not(target_os = "android")))]
    let flags = libc::AI_V4MAPPED | libc::AI_ADDRCONFIG;
    #[cfg(target_os = "android")]
    let flags = libc::AI_ADDRCONFIG;
    // ws2def.h: AI_ADDRCONFIG = 0x0400, AI_V4MAPPED = 0x0800.
    #[cfg(windows)]
    let flags = 0x0400 | 0x0800;
    let hints = dns_lookup::AddrInfoHints {
        flags,
        ..dns_lookup::AddrInfoHints::default() // AF_UNSPEC, any socket type, any protocol
    };
    let host_text = host.unwrap_or("*");
    let service_text = service.unwrap_or("(none)");
    let results = dns_lookup::getaddrinfo(host, service, Some(hints)).map_err(|e| {
        KjIoError::other(
            "getaddrinfo()",
            format!("DNS lookup failed. host = {host_text}; service = {service_text}; {e}"),
        )
    })?;
    Ok(results
        .filter_map(std::result::Result::ok)
        .map(|info| info.sockaddr)
        .collect())
}

/// Deduplicates an address list, keeping its order (a duplicate would make `listen()` bind the
/// same endpoint twice and `connect()` retry a refused address). getaddrinfo's own order is
/// kept: the resolver sorts by RFC 6724 destination preference, which is what every tokio and
/// std program connects in.
fn dedup_in_order(addrs: Vec<SocketAddr>) -> Vec<SocketAddr> {
    let mut seen = std::collections::HashSet::with_capacity(addrs.len());
    addrs
        .into_iter()
        .filter(|addr| seen.insert(*addr))
        .collect()
}

impl TokioAddress {
    /// An address that resolves to exactly `addrs` (deduplicated, order kept), tried in that
    /// order by `connect()` and all bound by `listen()`. The programmatic counterpart of what DNS
    /// resolution produces; used by the tests as a deterministic multi-result lookup.
    #[must_use]
    pub fn from_socket_addrs(addrs: Vec<SocketAddr>) -> Self {
        Self::ip(dedup_in_order(addrs), false)
    }

    const fn ip(addrs: Vec<SocketAddr>, wildcard: bool) -> Self {
        Self {
            spec: Spec::Ip { addrs, wildcard },
        }
    }

    fn wildcard(port: u16) -> Self {
        Self::ip(
            vec![SocketAddr::new(
                IpAddr::V6(std::net::Ipv6Addr::UNSPECIFIED),
                port,
            )],
            true,
        )
    }

    /// A unix address as *parsed*: validated now (length, NUL), like KJ's `parseAddress`, so a
    /// bad configuration string fails at parse time rather than at `listen()`.
    #[cfg(unix)]
    fn parsed_unix(name: UnixName) -> Result<Self> {
        name.to_tokio()?;
        Ok(Self {
            spec: Spec::Unix(name),
        })
    }

    /// The `unix:` and `unix-abstract:` forms (byte-wise: a path need not be UTF-8), or `None`
    /// for anything else.
    fn parse_unix(text: &[u8]) -> Option<Result<Self>> {
        #[cfg(unix)]
        {
            use std::os::unix::ffi::OsStrExt;
            if let Some(path) = text.strip_prefix(b"unix:") {
                let path = PathBuf::from(std::ffi::OsStr::from_bytes(path));
                return Some(Self::parsed_unix(UnixName::Path(path)));
            }
            if let Some(name) = text.strip_prefix(b"unix-abstract:") {
                #[cfg(target_os = "linux")]
                return Some(Self::parsed_unix(UnixName::Abstract(name.to_vec())));
                #[cfg(not(target_os = "linux"))]
                {
                    let _ = name;
                    return Some(Err(KjIoError::other(
                        "parseAddress",
                        "Unix domain socket abstract namespace is only supported on Linux",
                    )));
                }
            }
            None
        }
        #[cfg(windows)]
        {
            if text.starts_with(b"unix:") || text.starts_with(b"unix-abstract:") {
                return Some(Err(KjIoError::other(
                    "parseAddress",
                    "Unix domain sockets are not supported on this platform",
                )));
            }
            None
        }
    }

    /// `SocketAddress::parse` (kj/async-io-unix.c++), see the module docs.
    async fn parse(text: &[u8], port_hint: u16) -> Result<Self> {
        if let Some(unix) = Self::parse_unix(text) {
            return unix;
        }
        let text = std::str::from_utf8(text)
            .map_err(|_| KjIoError::other("parseAddress", "address is not valid UTF-8"))?;
        let (addr_part, port_part) = split_host_port(text)?;

        // A decimal port, or a service name for getaddrinfo (everything, host included, goes
        // there then, as in KJ).
        let port = match port_part {
            Some(port_text)
                if !port_text.is_empty() && port_text.bytes().all(|b| b.is_ascii_digit()) =>
            {
                port_text
                    .parse::<u16>()
                    .map_err(|_| KjIoError::other("parseAddress", "Port number too large."))?
            }
            Some(service) => return Self::lookup_host(addr_part, Some(service), port_hint).await,
            None => port_hint,
        };

        if addr_part == "*" {
            return Ok(Self::wildcard(port));
        }

        // KJ tries inet_pton first; what it rejects (hostnames, IPv6 scope IDs) falls back to
        // DNS. Rust's `IpAddr` parser accepts the forms glibc's inet_pton accepts (dotted quad
        // without leading zeros, RFC 4291 IPv6 text); a libc whose inet_pton is more lenient
        // would differ only on such malformed literals, which go to getaddrinfo here as they do
        // for KJ on glibc.
        if let Ok(ip) = addr_part.parse::<IpAddr>() {
            return Ok(Self::ip(vec![SocketAddr::new(ip, port)], false));
        }
        Self::lookup_host(addr_part, None, port).await
    }

    /// `SocketAddress::lookupHost`: `getaddrinfo` with KJ's hints on the loop runtime's
    /// blocking pool (KJ uses a detached thread per lookup). With a `service` the results carry
    /// getaddrinfo's ports; without one, `port` is patched in. Host `"*"` becomes a wildcard
    /// address keeping only the resolved port. Dropping the future (KJ promise cancelled)
    /// detaches the blocking call, which cannot be interrupted once started -- an OS limitation
    /// KJ's resolver thread shares; a runtime shutting down waits for it (kj-rs-tokio's teardown
    /// policy).
    async fn lookup_host(host: &str, service: Option<&str>, port: u16) -> Result<Self> {
        let host_text = host.to_owned();
        let service_text = service.map(str::to_owned);
        ensure_loop_thread()?;
        let resolved = tokio::task::spawn_blocking(move || {
            let host = (host_text != "*").then_some(host_text.as_str());
            getaddrinfo(host, service_text.as_deref())
        })
        .await
        .map_err(|_| KjIoError::other("getaddrinfo()", "resolver task failed"))??;
        let addrs: Vec<SocketAddr> = resolved
            .into_iter()
            .map(|mut addr| {
                if service.is_none() {
                    addr.set_port(port);
                }
                addr
            })
            .collect();
        let addrs = dedup_in_order(addrs);
        if addrs.is_empty() {
            return Err(KjIoError::other(
                "getaddrinfo()",
                format!("DNS lookup failed. host = {host}; no addresses"),
            ));
        }
        if host == "*" {
            return Ok(Self::wildcard(addrs[0].port()));
        }
        Ok(Self::ip(addrs, false))
    }

    /// Every endpoint `connect()` would try, in order, for the adapter to filter and connect.
    fn targets(&self) -> Result<Vec<SocketAddress>> {
        match &self.spec {
            Spec::Ip { wildcard: true, .. } => Err(KjIoError::other(
                "connect()",
                "cannot connect() to a wildcard address",
            )),
            Spec::Ip { addrs, .. } => Ok(addrs.iter().copied().map(SocketAddress::from).collect()),
            #[cfg(unix)]
            Spec::Unix(name) => Ok(vec![name.to_socket_address()]),
        }
    }

    fn listen(&self) -> Result<Box<TokioListener>> {
        ensure_loop_thread()?;
        let inners = match &self.spec {
            Spec::Ip { addrs, wildcard } => {
                if addrs.is_empty() {
                    return Err(KjIoError::other("listen()", "no addresses to bind"));
                }
                addrs
                    .iter()
                    .map(|addr| bind_tcp(*addr, *wildcard))
                    .collect::<Result<Vec<ListenerInner>>>()?
            }
            #[cfg(unix)]
            Spec::Unix(name) => vec![ListenerInner::Unix(
                UnixListener::bind_addr(&name.to_tokio()?).map_err(op("bind()"))?,
            )],
        };
        Ok(Box::new(TokioListener::new(inners)))
    }

    /// `kj::NetworkAddress::toString`, byte for byte like KJ's: `"ip:port"`, `"[v6]:port"`,
    /// comma-separated for several results, `"*:port"` for a wildcard, `"unix:path"`.
    fn to_display_bytes(&self) -> Vec<u8> {
        match &self.spec {
            Spec::Ip { addrs, wildcard } => {
                if *wildcard {
                    format!("*:{}", addrs[0].port()).into_bytes()
                } else {
                    let parts: Vec<String> = addrs.iter().map(ToString::to_string).collect();
                    parts.join(",").into_bytes()
                }
            }
            #[cfg(unix)]
            Spec::Unix(name) => name.display_bytes(),
        }
    }
}

// ======================================================================================
// Connecting

/// tokio's connect (`connect(2)`, wait for writability, read `SO_ERROR`), then KJ's
/// unconditional `TCP_NODELAY` on outbound TCP sockets (a hard failure in KJ's
/// `SocketAddress::socket()`, and here).
async fn connect_to(target: SocketAddress) -> Result<Box<TokioStream>> {
    ensure_loop_thread()?;
    let socket = match target.kind {
        AddressKind::Ipv4 | AddressKind::Ipv6 => {
            let stream = TcpStream::connect(ip_socket_addr(&target)?)
                .await
                .map_err(op("connect()"))?;
            stream
                .set_nodelay(true)
                .map_err(op("setsockopt(TCP_NODELAY)"))?;
            Socket::Tcp(stream)
        }
        #[cfg(unix)]
        AddressKind::UnixPath | AddressKind::UnixAbstract | AddressKind::UnixUnnamed => {
            let name = UnixName::from_socket_address(&target)?.to_tokio()?;
            Socket::Unix(
                UnixStream::connect_addr(&name)
                    .await
                    .map_err(op("connect()"))?,
            )
        }
        _ => {
            return Err(KjIoError::other(
                "connect()",
                "not a socket address this platform can connect to",
            ));
        }
    };
    Ok(Box::new(TokioStream::new(socket)))
}

// ======================================================================================
// Socket pairs (kj::AsyncIoProvider::newTwoWayPipe)

#[cfg(any(windows, test))]
fn accept_socket_pair_peer(
    listener: &std::net::TcpListener,
    client: &std::net::TcpStream,
) -> std::io::Result<std::net::TcpStream> {
    let expected = client.local_addr()?;
    loop {
        let (stream, peer) = listener.accept()?;
        // The loopback listener is visible to other local processes. Only the connection made
        // by our client belongs to this socket pair.
        if peer == expected {
            return Ok(stream);
        }
    }
}

/// A connected pair of stream sockets, both registered with the loop runtime: an `AF_UNIX`
/// socketpair on unix; on Windows a loopback TCP connection, the way kj's own win32 provider
/// builds its pipes (`newOsSocketpair`).
pub fn socket_pair() -> Result<(Box<TokioStream>, Box<TokioStream>)> {
    #[cfg(unix)]
    {
        ensure_loop_thread()?;
        let (first, second) = UnixStream::pair().map_err(op("socketpair()"))?;
        Ok((
            Box::new(TokioStream::new(Socket::Unix(first))),
            Box::new(TokioStream::new(Socket::Unix(second))),
        ))
    }
    #[cfg(windows)]
    {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").map_err(op("bind()"))?;
        let addr = listener.local_addr().map_err(op("getsockname()"))?;
        let client = std::net::TcpStream::connect(addr).map_err(op("connect()"))?;
        let server = accept_socket_pair_peer(&listener, &client).map_err(op("accept()"))?;
        let (first, second) = (socket2::Socket::from(client), socket2::Socket::from(server));
        first.set_nonblocking(true).map_err(op("fcntl()"))?;
        second.set_nonblocking(true).map_err(op("fcntl()"))?;
        Ok((wrap_socket(first)?, wrap_socket(second)?))
    }
}

// ======================================================================================
// Listening

/// One listening TCP socket, KJ style: `SO_REUSEADDR` (KJ: "We always enable `SO_REUSEADDR`
/// because having to take your server down for five minutes before it can restart really
/// sucks"), dual-stack for wildcards (`IPV6_V6ONLY` off -- the one option tokio's `TcpSocket`
/// has no setter for, hence the socket2 borrow), `SOMAXCONN` backlog.
fn bind_tcp(addr: SocketAddr, wildcard: bool) -> Result<ListenerInner> {
    let socket = match addr {
        SocketAddr::V4(_) => TcpSocket::new_v4(),
        SocketAddr::V6(_) => TcpSocket::new_v6(),
    }
    .map_err(op("socket()"))?;
    socket
        .set_reuseaddr(true)
        .map_err(op("setsockopt(SO_REUSEADDR)"))?;
    if wildcard {
        socket2::SockRef::from(&socket)
            .set_only_v6(false)
            .map_err(op("setsockopt(IPV6_V6ONLY)"))?;
    }
    socket.bind(addr).map_err(op("bind()"))?;
    let listener = socket.listen(LISTEN_BACKLOG).map_err(op("listen()"))?;
    Ok(ListenerInner::Tcp(listener))
}

/// A `kj::ConnectionReceiver` backend: one or more listening sockets (several when the address
/// resolved to several -- KJ's aggregate receiver), accepted from in round-robin order of
/// readiness. A handle to `Arc`-shared state; each `accept()` future owns a share.
pub struct TokioListener {
    shared: Arc<ListenerShared>,
}

struct ListenerShared {
    /// Never empty.
    inners: Vec<ListenerInner>,
    /// Round-robin start index for the next `accept()` poll, so a busy first socket cannot
    /// starve the others.
    next: AtomicUsize,
}

enum ListenerInner {
    Tcp(TcpListener),
    #[cfg(unix)]
    Unix(UnixListener),
}

impl ListenerInner {
    /// One `accept(2)`: the connected socket plus the peer address it reported.
    fn poll_accept(&self, cx: &mut Context<'_>) -> Poll<std::io::Result<(Socket, SocketAddress)>> {
        match self {
            Self::Tcp(listener) => listener
                .poll_accept(cx)
                .map_ok(|(stream, peer)| (Socket::Tcp(stream), SocketAddress::from(peer))),
            #[cfg(unix)]
            Self::Unix(listener) => listener.poll_accept(cx).map_ok(|(stream, peer)| {
                let peer = std::os::unix::net::SocketAddr::from(peer);
                (Socket::Unix(stream), SocketAddress::from(&peer))
            }),
        }
    }

    fn port(&self) -> Result<u16> {
        match self {
            Self::Tcp(listener) => Ok(listener.local_addr().map_err(op("getsockname()"))?.port()),
            // KJ returns 0 for non-IP listeners.
            #[cfg(unix)]
            Self::Unix(_) => Ok(0),
        }
    }

    fn local_addr(&self) -> Result<SocketAddress> {
        match self {
            Self::Tcp(listener) => Ok(SocketAddress::from(
                listener.local_addr().map_err(op("getsockname()"))?,
            )),
            #[cfg(unix)]
            Self::Unix(listener) => {
                let addr = listener.local_addr().map_err(op("getsockname()"))?;
                Ok(SocketAddress::from(&std::os::unix::net::SocketAddr::from(
                    addr,
                )))
            }
        }
    }
}

/// KJ's `acceptImpl` retry set (kj/async-io-unix.c++): errors `accept(2)` reports about the
/// *accepted* connection being already broken, which must not take down the listener. KJ's own
/// comment: "it's hard to say exactly what errors are such network errors and which ones are
/// permanent errors. We've made a guess here."
fn is_transient_accept_error(error: &std::io::Error) -> bool {
    #[cfg(unix)]
    {
        if let Some(errno) = error.raw_os_error() {
            #[cfg(not(target_os = "openbsd"))]
            let eproto = errno == libc::EPROTO;
            #[cfg(target_os = "openbsd")]
            let eproto = false;
            return eproto
                || matches!(
                    errno,
                    libc::EINTR
                        | libc::ENETDOWN
                        | libc::EHOSTDOWN
                        | libc::EHOSTUNREACH
                        | libc::ENETUNREACH
                        | libc::ECONNABORTED
                        | libc::ETIMEDOUT
                );
        }
        // XNU can return an accepted-but-already-dead socket with addrlen == 0 (KJ discards
        // it and retries, citing the kernel bug). mio surfaces that zero-length, family-less
        // address as InvalidInput; there is no fd to leak, mio closed it.
        cfg!(target_os = "macos") && error.kind() == std::io::ErrorKind::InvalidInput
    }
    #[cfg(windows)]
    {
        matches!(
            error.kind(),
            std::io::ErrorKind::ConnectionAborted
                | std::io::ErrorKind::ConnectionReset
                | std::io::ErrorKind::Interrupted
        )
    }
}

/// Failures of `setsockopt(TCP_NODELAY)` on a freshly accepted socket that KJ tolerates
/// (kj/async-io-unix.c++ `acceptImpl`): the option is not supported on this socket kind, or
/// (macOS / FreeBSD) `EINVAL` because the peer already reset the connection between accept and
/// here. Anything else is a real failure, in KJ and here.
fn is_tolerable_nodelay_error(error: &std::io::Error) -> bool {
    #[cfg(unix)]
    {
        let Some(errno) = error.raw_os_error() else {
            return false;
        };
        let einval_dead_socket =
            cfg!(any(target_os = "macos", target_os = "freebsd")) && errno == libc::EINVAL;
        einval_dead_socket || matches!(errno, libc::EOPNOTSUPP | libc::ENOPROTOOPT)
    }
    #[cfg(windows)]
    {
        use crate::error::win32;
        matches!(
            error.raw_os_error(),
            Some(win32::WSAEOPNOTSUPP | win32::WSAENOPROTOOPT)
        )
    }
}

impl ListenerShared {
    fn poll_accept_any(
        &self,
        cx: &mut Context<'_>,
    ) -> Poll<std::io::Result<(Socket, SocketAddress)>> {
        let count = self.inners.len();
        let start = self.next.load(Ordering::Relaxed);
        for offset in 0..count {
            let index = (start + offset) % count;
            if let Poll::Ready(result) = self.inners[index].poll_accept(cx) {
                self.next.store((index + 1) % count, Ordering::Relaxed);
                return Poll::Ready(result);
            }
        }
        Poll::Pending
    }

    /// One accepted connection: KJ's transient errors are retried here; whether the peer is
    /// allowed is the adapter's decision (module docs, "Where filtering happens").
    async fn accept(&self) -> Result<PeerStream> {
        ensure_loop_thread()?;
        loop {
            let (socket, peer) = match std::future::poll_fn(|cx| self.poll_accept_any(cx)).await {
                Ok(accepted) => accepted,
                Err(e) if is_transient_accept_error(&e) => continue,
                Err(e) => return Err(op("accept()")(e)),
            };
            if let Socket::Tcp(stream) = &socket
                && let Err(e) = stream.set_nodelay(true)
                && !is_tolerable_nodelay_error(&e)
            {
                return Err(op("setsockopt(TCP_NODELAY)")(e));
            }
            return Ok(PeerStream {
                stream: Box::new(TokioStream::new(socket)),
                peer,
            });
        }
    }
}

impl TokioListener {
    fn new(inners: Vec<ListenerInner>) -> Self {
        Self {
            shared: Arc::new(ListenerShared {
                inners,
                next: AtomicUsize::new(0),
            }),
        }
    }

    /// `kj::ConnectionReceiver::getPort`: the first socket's, like KJ's aggregate receiver.
    fn port(&self) -> Result<u16> {
        self.shared.inners[0].port()
    }

    /// `kj::ConnectionReceiver::getsockname`: the first socket's.
    fn local_addr(&self) -> Result<SocketAddress> {
        self.shared.inners[0].local_addr()
    }
}

// ======================================================================================
// Bridge entry points (see ffi.rs)

pub async fn parse_address(addr: &[u8], port_hint: u16) -> Result<Box<TokioAddress>> {
    Ok(Box::new(TokioAddress::parse(addr, port_hint).await?))
}

/// `kj::Network::getSockaddr`: the C++ adapter decoded the caller's `struct sockaddr` into a
/// typed address. Not validated here: KJ accepts, prints and stores whatever `sockaddr` a caller
/// hands over (a pathname filling `sun_path` with no NUL included); what cannot be bound or
/// connected fails there, as it does under KJ.
pub fn network_address_from(addr: &SocketAddress) -> Result<Box<TokioAddress>> {
    match addr.kind {
        AddressKind::Ipv4 | AddressKind::Ipv6 => Ok(Box::new(TokioAddress::ip(
            vec![ip_socket_addr(addr)?],
            false,
        ))),
        #[cfg(unix)]
        AddressKind::UnixPath | AddressKind::UnixAbstract | AddressKind::UnixUnnamed => {
            Ok(Box::new(TokioAddress {
                spec: Spec::Unix(UnixName::from_socket_address(addr)?),
            }))
        }
        _ => Err(KjIoError::other(
            "getSockaddr",
            "not a socket address this platform supports",
        )),
    }
}

pub fn address_targets(addr: &TokioAddress) -> Result<Vec<SocketAddress>> {
    addr.targets()
}

pub fn connect_target(target: SocketAddress) -> impl Future<Output = Result<Box<TokioStream>>> {
    connect_to(target)
}

pub fn address_listen(addr: &TokioAddress) -> Result<Box<TokioListener>> {
    addr.listen()
}

#[expect(clippy::unnecessary_box_returns)]
pub fn address_clone(addr: &TokioAddress) -> Box<TokioAddress> {
    Box::new(TokioAddress {
        spec: addr.spec.clone(),
    })
}

pub fn address_to_string(addr: &TokioAddress) -> Vec<u8> {
    addr.to_display_bytes()
}

pub fn listener_accept(
    listener: &TokioListener,
) -> impl Future<Output = Result<PeerStream>> + use<> {
    let shared = Arc::clone(&listener.shared);
    async move { shared.accept().await }
}

/// Another handle to the same listener, for an accept loop to own (the C++ receiver may be
/// destroyed while its `accept()` is pending; the loop's share keeps the sockets alive).
#[expect(clippy::unnecessary_box_returns)]
pub fn listener_clone(listener: &TokioListener) -> Box<TokioListener> {
    Box::new(TokioListener {
        shared: Arc::clone(&listener.shared),
    })
}

pub fn listener_port(listener: &TokioListener) -> Result<u16> {
    listener.port()
}

pub fn listener_local_addr(listener: &TokioListener) -> Result<SocketAddress> {
    listener.local_addr()
}

/// A socket handed over by `wrapSocketFd` (ffi.rs turned the raw handle into this owned
/// `socket2::Socket`, KJ's fd flags applied): registered with the loop runtime as the tokio
/// stream type of its family.
pub fn wrap_socket(socket: socket2::Socket) -> Result<Box<TokioStream>> {
    let local = socket.local_addr().map_err(op("getsockname()"))?;
    ensure_loop_thread()?;
    let socket = match local.domain() {
        socket2::Domain::IPV4 | socket2::Domain::IPV6 => {
            Socket::Tcp(TcpStream::from_std(socket.into()).map_err(op("wrapSocketFd"))?)
        }
        #[cfg(unix)]
        socket2::Domain::UNIX => {
            Socket::Unix(UnixStream::from_std(socket.into()).map_err(op("wrapSocketFd"))?)
        }
        _ => {
            return Err(KjIoError::other(
                "wrapSocketFd",
                "unsupported socket family",
            ));
        }
    };
    Ok(Box::new(TokioStream::new(socket)))
}

/// `wrapListenSocketFd`'s counterpart of [`wrap_socket`].
pub fn wrap_listener(socket: socket2::Socket) -> Result<Box<TokioListener>> {
    let local = socket.local_addr().map_err(op("getsockname()"))?;
    ensure_loop_thread()?;
    let inner = match local.domain() {
        socket2::Domain::IPV4 | socket2::Domain::IPV6 => ListenerInner::Tcp(
            TcpListener::from_std(socket.into()).map_err(op("wrapListenSocketFd"))?,
        ),
        #[cfg(unix)]
        socket2::Domain::UNIX => ListenerInner::Unix(
            UnixListener::from_std(socket.into()).map_err(op("wrapListenSocketFd"))?,
        ),
        _ => {
            return Err(KjIoError::other(
                "wrapListenSocketFd",
                "unsupported socket family",
            ));
        }
    };
    Ok(Box::new(TokioListener::new(vec![inner])))
}

#[cfg(test)]
mod tests {
    use std::net::IpAddr;
    use std::net::Ipv4Addr;
    use std::net::Ipv6Addr;
    use std::net::SocketAddr;
    use std::task::Waker;

    use cxx::KjError;
    use static_assertions::assert_impl_all;

    use super::*;

    assert_impl_all!(TokioListener: Send, Sync);
    assert_impl_all!(TokioAddress: Send, Sync);

    fn parse_once(text: &[u8], port_hint: u16) -> Option<Result<TokioAddress>> {
        let mut fut = std::pin::pin!(TokioAddress::parse(text, port_hint));
        let mut cx = Context::from_waker(Waker::noop());
        match fut.as_mut().poll(&mut cx) {
            Poll::Ready(result) => Some(result),
            Poll::Pending => None,
        }
    }

    fn parse_ok(text: &str, port_hint: u16) -> TokioAddress {
        match parse_once(text.as_bytes(), port_hint) {
            Some(Ok(addr)) => addr,
            Some(Err(e)) => panic!(
                "{text:?} failed to parse: {}",
                KjError::from(e).description()
            ),
            None => panic!("{text:?} unexpectedly went to DNS"),
        }
    }

    fn parse_err(text: &str, port_hint: u16) -> KjError {
        match parse_once(text.as_bytes(), port_hint) {
            Some(Ok(_)) => panic!("{text:?} unexpectedly parsed"),
            Some(Err(e)) => KjError::from(e),
            None => panic!("{text:?} unexpectedly went to DNS"),
        }
    }

    fn err_of<T>(result: Result<T>) -> KjError {
        match result {
            Ok(_) => panic!("expected an error"),
            Err(e) => KjError::from(e),
        }
    }

    fn ip_addrs(addr: &TokioAddress) -> (&[SocketAddr], bool) {
        match &addr.spec {
            Spec::Ip { addrs, wildcard } => (addrs, *wildcard),
            #[cfg(unix)]
            Spec::Unix(_) => panic!("expected an IP address"),
        }
    }

    #[test]
    fn socket_pair_accepts_only_its_own_client() {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let foreign = std::net::TcpStream::connect(addr).unwrap();
        let client = std::net::TcpStream::connect(addr).unwrap();

        let server = accept_socket_pair_peer(&listener, &client).unwrap();
        assert_eq!(server.peer_addr().unwrap(), client.local_addr().unwrap());

        drop(foreign);
    }

    #[test]
    fn ipv4_literal_with_and_without_port() {
        let addr = parse_ok("1.2.3.4:80", 0);
        let (addrs, wildcard) = ip_addrs(&addr);
        assert_eq!(
            addrs,
            &[SocketAddr::new(IpAddr::V4(Ipv4Addr::new(1, 2, 3, 4)), 80)]
        );
        assert!(!wildcard);

        let addr = parse_ok("1.2.3.4", 8080);
        assert_eq!(ip_addrs(&addr).0[0].port(), 8080);
    }

    #[test]
    fn ipv6_literal_forms() {
        let addr = parse_ok("[::1]:443", 0);
        assert_eq!(
            ip_addrs(&addr).0,
            &[SocketAddr::new(IpAddr::V6(Ipv6Addr::LOCALHOST), 443)]
        );
        let addr = parse_ok("[::1]", 7);
        assert_eq!(ip_addrs(&addr).0[0].port(), 7);
        let addr = parse_ok("fe80::1", 9);
        assert_eq!(
            ip_addrs(&addr).0[0],
            SocketAddr::new("fe80::1".parse::<IpAddr>().unwrap(), 9)
        );
    }

    #[test]
    fn wildcard_forms() {
        let addr = parse_ok("*", 1234);
        let (addrs, wildcard) = ip_addrs(&addr);
        assert!(wildcard);
        assert_eq!(
            addrs,
            &[SocketAddr::new(IpAddr::V6(Ipv6Addr::UNSPECIFIED), 1234)]
        );
        let addr = parse_ok("*:80", 0);
        let (addrs, wildcard) = ip_addrs(&addr);
        assert!(wildcard);
        assert_eq!(addrs[0].port(), 80);
        assert!(
            KjError::from(addr.targets().unwrap_err())
                .description()
                .contains("wildcard")
        );
    }

    #[test]
    fn port_text_is_decimal_or_a_service_name() {
        assert_eq!(ip_addrs(&parse_ok("127.0.0.1:80", 0)).0[0].port(), 80);
        assert_eq!(ip_addrs(&parse_ok("127.0.0.1:0", 5)).0[0].port(), 0);
        assert_eq!(ip_addrs(&parse_ok("127.0.0.1", 5)).0[0].port(), 5);
        for too_large in ["127.0.0.1:65536", "127.0.0.1:99999999999999999999"] {
            assert!(
                parse_err(too_large, 0)
                    .description()
                    .contains("Port number too large."),
                "{too_large}"
            );
        }
        let _port = kj_rs_tokio::TokioPort::new();
        for service in [
            "127.0.0.1:http",
            "127.0.0.1:0x50",
            "127.0.0.1:-1",
            "127.0.0.1:",
        ] {
            assert!(
                parse_once(service.as_bytes(), 0).is_none(),
                "{service}: not decimal, so a service name for getaddrinfo"
            );
        }
    }

    /// Every `SocketAddress` kind survives the round trip through a `TokioAddress` (what
    /// `getSockaddr` followed by `toString` / `connect` sees) and back to a typed address.
    #[test]
    fn typed_addresses_round_trip() {
        for text in ["1.2.3.4:80", "[::1]:443", "[fe80::1%7]:0"] {
            let addr: SocketAddr = text.parse().unwrap();
            let typed = SocketAddress::from(addr);
            assert_eq!(ip_socket_addr(&typed).unwrap(), addr, "{text}");
            let back = network_address_from(&typed).unwrap();
            assert_eq!(ip_addrs(&back).0, &[addr]);
            assert_eq!(back.targets().unwrap(), vec![typed]);
        }
        let unnamed = SocketAddress::blank(AddressKind::UnixUnnamed);
        #[cfg(unix)]
        let expected = "unnamed";
        #[cfg(windows)]
        let expected = "not a socket address this platform supports";
        assert!(
            err_of(network_address_from(&unnamed))
                .description()
                .contains(expected),
            "an unnamed peer is not an address one can connect to"
        );
        assert!(ip_socket_addr(&unnamed).is_err());
    }

    #[cfg(unix)]
    #[test]
    fn unix_forms() {
        use std::os::unix::ffi::OsStrExt;
        let addr = parse_ok("unix:/tmp/sock", 0);
        assert_eq!(addr.to_display_bytes(), b"unix:/tmp/sock");
        assert!(
            matches!(&addr.spec, Spec::Unix(UnixName::Path(path)) if path == std::path::Path::new("/tmp/sock"))
        );
        let too_long = format!("unix:/{}", "x".repeat(200));
        assert!(
            parse_err(&too_long, 0)
                .description()
                .contains("parseAddress")
        );
        assert!(
            parse_err("unix:/tmp/a\0b", 0)
                .description()
                .contains("contains NULL")
        );
        let raw = b"unix:/tmp/\xff\xfe";
        let Some(Ok(addr)) = parse_once(raw, 0) else {
            panic!("a non-UTF-8 unix path must parse");
        };
        assert!(matches!(
            &addr.spec,
            Spec::Unix(UnixName::Path(path)) if path.as_os_str().as_bytes() == b"/tmp/\xff\xfe"
        ));
        assert_eq!(addr.to_display_bytes(), raw);

        // Typed round trip, including a path that fills sun_path (KJ allows the unterminated
        // form; std refuses to bind or connect it, which is where that surfaces).
        let targets = addr.targets().unwrap();
        assert_eq!(targets.len(), 1);
        assert_eq!(targets[0].kind, AddressKind::UnixPath);
        assert_eq!(targets[0].name, b"/tmp/\xff\xfe");
        let back = network_address_from(&targets[0]).unwrap();
        assert_eq!(back.to_display_bytes(), addr.to_display_bytes());
        let mut full = SocketAddress::blank(AddressKind::UnixPath);
        full.name = vec![b'x'; 108];
        full.name[0] = b'/';
        let whole = network_address_from(&full).unwrap();
        assert_eq!(
            whole.to_display_bytes().len(),
            5 + 108,
            "printed whole, like KJ"
        );
        assert!(
            err_of(
                whole
                    .targets()
                    .and_then(|t| UnixName::from_socket_address(&t[0])?.to_tokio())
            )
            .description()
            .contains("parseAddress"),
            "108 bytes leaves no room for std's NUL: connect/listen is where it fails"
        );
        let peer = std::os::unix::net::SocketAddr::from_pathname("/tmp/peer").unwrap();
        assert_eq!(
            SocketAddress::from(&peer),
            UnixName::Path(PathBuf::from("/tmp/peer")).to_socket_address()
        );
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn abstract_unix_forms() {
        use std::os::linux::net::SocketAddrExt;
        let addr = parse_ok("unix-abstract:kj-rs-io", 0);
        assert_eq!(addr.to_display_bytes(), b"unix-abstract:kj-rs-io");
        let targets = addr.targets().unwrap();
        assert_eq!(targets[0].kind, AddressKind::UnixAbstract);
        assert_eq!(targets[0].name, b"kj-rs-io");
        let back = network_address_from(&targets[0]).unwrap();
        assert_eq!(back.to_display_bytes(), addr.to_display_bytes());
        let too_long = format!("unix-abstract:{}", "x".repeat(200));
        assert!(
            parse_err(&too_long, 0)
                .description()
                .contains("parseAddress")
        );
        let std_addr = std::os::unix::net::SocketAddr::from_abstract_name(b"peer").unwrap();
        assert_eq!(
            SocketAddress::from(&std_addr),
            UnixName::Abstract(b"peer".to_vec()).to_socket_address()
        );
        let unnamed = std::os::unix::net::SocketAddr::from_pathname("").unwrap();
        assert_eq!(SocketAddress::from(&unnamed).kind, AddressKind::UnixUnnamed);
    }

    #[cfg(all(unix, not(target_os = "linux")))]
    #[test]
    fn abstract_unix_names_are_linux_only() {
        assert!(
            parse_err("unix-abstract:kj-rs-io", 0)
                .description()
                .contains("only supported on Linux")
        );
        let mut typed = SocketAddress::blank(AddressKind::UnixAbstract);
        typed.name = b"peer".to_vec();
        assert!(
            err_of(network_address_from(&typed))
                .description()
                .contains("only supported on Linux")
        );
    }

    #[test]
    fn address_lists_dedup_in_order() {
        let v4a: SocketAddr = "10.0.0.2:1".parse().unwrap();
        let v6: SocketAddr = "[::2]:2".parse().unwrap();
        assert_eq!(dedup_in_order(vec![v4a, v6, v4a, v6, v4a]), vec![v4a, v6]);
        let addr = TokioAddress::from_socket_addrs(vec![v6, v6, v4a, v6]);
        assert_eq!(ip_addrs(&addr).0, &[v6, v4a]);
        let _port = kj_rs_tokio::TokioPort::new();
        let loopback: SocketAddr = "127.0.0.1:0".parse().unwrap();
        let listener = TokioAddress::from_socket_addrs(vec![loopback, loopback])
            .listen()
            .expect("duplicates collapse to one socket");
        assert_eq!(listener.shared.inners.len(), 1);
    }

    #[test]
    fn rejects_non_utf8_non_unix_text() {
        let Some(Err(e)) = parse_once(b"\xff\xfe:80", 0) else {
            panic!("non-UTF-8 host text must be rejected");
        };
        assert!(KjError::from(e).description().contains("UTF-8"));
    }

    #[test]
    fn empty_address_lists_are_errors() {
        let _port = kj_rs_tokio::TokioPort::new();
        let addr = TokioAddress::from_socket_addrs(Vec::new());
        assert!(
            KjError::from(addr.listen().err().expect("listen on nothing fails"))
                .description()
                .contains("no addresses to bind")
        );
        assert!(addr.targets().unwrap().is_empty());
    }

    #[test]
    fn parse_never_panics_on_random_input_and_literals_round_trip() {
        const ALPHABET: &[u8] = b"0123456789abcdef:.[]*%-/xu";
        let _port = kj_rs_tokio::TokioPort::new();
        let mut state: u64 = 0x2545_f491_4f6c_dd1d;
        let mut next = move || {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state
        };
        for _ in 0..20_000 {
            let len = usize::try_from(next() % 24).unwrap();
            let text: Vec<u8> = (0..len)
                .map(|_| ALPHABET[usize::try_from(next() % ALPHABET.len() as u64).unwrap()])
                .collect();
            let hint = u16::try_from(next() & 0xffff).unwrap();
            let Some(Ok(addr)) = parse_once(&text, hint) else {
                continue; // rejected, or resolving: both fine
            };
            let shown = String::from_utf8(addr.to_display_bytes()).unwrap();
            let Spec::Ip { addrs, wildcard } = &addr.spec else {
                continue;
            };
            if *wildcard {
                assert!(shown.starts_with("*:"), "{text:?} -> {shown:?}");
                continue;
            }
            let expected = addrs[0];
            let reparsed = parse_ok(&shown, 0);
            assert_eq!(ip_addrs(&reparsed).0[0], expected, "{text:?} -> {shown:?}");
        }
    }

    #[test]
    fn bracket_errors_carry_kj_messages() {
        let err = parse_err("[::1", 0);
        assert!(
            err.description().contains("Unclosed '['"),
            "{}",
            err.description()
        );
        let err = parse_err("[::1]x", 0);
        assert!(
            err.description().contains("Expected port suffix after ']'"),
            "{}",
            err.description()
        );
    }

    #[cfg(unix)]
    #[test]
    fn accept_and_connect_error_classification_matches_kj() {
        for errno in [
            libc::ECONNABORTED,
            libc::EPROTO,
            libc::ENETDOWN,
            libc::EHOSTUNREACH,
            libc::ETIMEDOUT,
            libc::EINTR,
        ] {
            assert!(
                is_transient_accept_error(&std::io::Error::from_raw_os_error(errno)),
                "errno {errno} should be retried"
            );
        }
        for errno in [libc::EBADF, libc::EMFILE, libc::ENOTSOCK] {
            assert!(
                !is_transient_accept_error(&std::io::Error::from_raw_os_error(errno)),
                "errno {errno} must surface"
            );
        }

        assert!(is_tolerable_nodelay_error(
            &std::io::Error::from_raw_os_error(libc::EOPNOTSUPP)
        ));
        assert!(is_tolerable_nodelay_error(
            &std::io::Error::from_raw_os_error(libc::ENOPROTOOPT)
        ));
        assert!(!is_tolerable_nodelay_error(
            &std::io::Error::from_raw_os_error(libc::EBADF)
        ));
        assert_eq!(
            is_tolerable_nodelay_error(&std::io::Error::from_raw_os_error(libc::EINVAL)),
            cfg!(any(target_os = "macos", target_os = "freebsd"))
        );
    }

    /// `accept()` is a bridged operation like any other: on a thread without a `TokioEventPort`
    /// it fails with a `kj::Exception` rather than waiting on a driver that never turns.
    #[test]
    fn accept_off_the_loop_thread_is_refused() {
        let _port = kj_rs_tokio::TokioPort::new();
        let listener = TokioAddress::from_socket_addrs(vec!["127.0.0.1:0".parse().unwrap()])
            .listen()
            .unwrap();
        let accept = listener_accept(&listener);
        let err = std::thread::spawn(move || {
            let mut accept = Box::pin(accept);
            let mut cx = Context::from_waker(Waker::noop());
            match accept.as_mut().poll(&mut cx) {
                Poll::Ready(Err(e)) => KjError::from(e),
                _ => panic!("accept off the loop thread must fail at once"),
            }
        })
        .join()
        .unwrap();
        assert!(err.description().contains("no TokioEventPort"));
    }

    #[test]
    fn listen_binds_every_resolved_address_or_fails_as_a_whole() {
        let _port = kj_rs_tokio::TokioPort::new();
        let (v4, v6) = (0..16)
            .find_map(|_| {
                let probe = std::net::TcpListener::bind("127.0.0.1:0").ok()?;
                let port = probe.local_addr().ok()?.port();
                drop(probe);
                let v4: SocketAddr = SocketAddr::new(Ipv4Addr::LOCALHOST.into(), port);
                let v6: SocketAddr = SocketAddr::new(Ipv6Addr::LOCALHOST.into(), port);
                std::net::TcpListener::bind(v6).ok().map(|l| {
                    drop(l);
                    (v4, v6)
                })
            })
            .expect("a port free on both loopback families");

        let listener = TokioAddress::from_socket_addrs(vec![v6, v4])
            .listen()
            .expect("listen on both addresses");
        assert_eq!(
            listener.shared.inners.len(),
            2,
            "one socket per resolved address"
        );
        assert_eq!(
            listener.port().unwrap(),
            v6.port(),
            "getPort() reports the first socket"
        );
        assert_eq!(
            ip_socket_addr(&listener.local_addr().unwrap()).unwrap(),
            v6,
            "getsockname() reports the first socket"
        );
        std::net::TcpStream::connect(v4).expect("IPv4 connect");
        std::net::TcpStream::connect(v6).expect("IPv6 connect");

        // winsock's SO_REUSEADDR lets a second socket bind a port that is already listened on, so
        // the "one bind fails the whole listen" half is a unix check.
        #[cfg(unix)]
        {
            let taken = v4;
            let free: SocketAddr = "127.0.0.1:0".parse().unwrap();
            let err = TokioAddress::from_socket_addrs(vec![free, taken])
                .listen()
                .err()
                .expect("the second bind must fail the listen");
            assert!(
                KjError::from(err).description().contains("bind()"),
                "reported as the bind failure"
            );
        }
    }
}
