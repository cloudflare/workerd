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
//! - `"loopback:name"`, once the network's [`LoopbackRegistry`] is enabled (workerd test
//!   only): connections serviced within the process (loopback.rs).
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
use tokio::net::UdpSocket;
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
use crate::loopback::LoopbackQueue;
use crate::loopback::LoopbackRegistry;
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

    fn loopback(name: &[u8]) -> Self {
        Self {
            name: name.to_vec(),
            ..Self::blank(AddressKind::Loopback)
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

pub struct TokioDatagram {
    shared: Arc<DatagramShared>,
}

struct DatagramShared {
    socket: UdpSocket,
    owner: tokio::runtime::Id,
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
    /// `"loopback:name"`: the queue the network's registry gave for the name at parse time.
    Loopback(Arc<LoopbackQueue>),
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
    async fn parse(text: &[u8], port_hint: u16, loopback: &LoopbackRegistry) -> Result<Self> {
        if let Some(queue) = loopback.parse(text) {
            return Ok(Self {
                spec: Spec::Loopback(queue?),
            });
        }
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
            Spec::Loopback(queue) => Ok(vec![SocketAddress::loopback(queue.name())]),
        }
    }

    /// `connect()` to one of [`Self::targets`]. The address itself supplies what a
    /// `SocketAddress` cannot name: the queue behind a loopback target.
    fn connect(
        &self,
        target: SocketAddress,
    ) -> impl Future<Output = Result<Box<TokioStream>>> + use<> {
        let spec = self.spec.clone();
        async move {
            match spec {
                Spec::Loopback(queue) => queue.connect(),
                _ => connect_to(target).await,
            }
        }
    }

    fn listen(&self) -> Result<Box<TokioListener>> {
        ensure_loop_thread()?;
        let backend = match &self.spec {
            Spec::Ip { addrs, wildcard } => {
                if addrs.is_empty() {
                    return Err(KjIoError::other("listen()", "no addresses to bind"));
                }
                ListenerBackend::sockets(
                    addrs
                        .iter()
                        .map(|addr| bind_tcp(*addr, *wildcard))
                        .collect::<Result<Vec<ListenerInner>>>()?,
                )
            }
            #[cfg(unix)]
            Spec::Unix(name) => ListenerBackend::sockets(vec![ListenerInner::Unix(
                UnixListener::bind_addr(&name.to_tokio()?).map_err(op("bind()"))?,
            )]),
            Spec::Loopback(queue) => ListenerBackend::Loopback(Arc::clone(queue)),
        };
        Ok(Box::new(TokioListener::new(backend)?))
    }

    fn bind_datagram(&self) -> Result<Box<TokioDatagram>> {
        ensure_loop_thread()?;
        let (addr, wildcard) = match &self.spec {
            Spec::Ip { addrs, wildcard } => (
                *addrs
                    .first()
                    .ok_or_else(|| KjIoError::other("bind()", "no addresses to bind"))?,
                *wildcard,
            ),
            #[cfg(unix)]
            Spec::Unix(_) => {
                return Err(KjIoError::other(
                    "bind()",
                    "Unix datagram sockets are not supported",
                ));
            }
            Spec::Loopback(_) => {
                return Err(KjIoError::other(
                    "bind()",
                    "loopback addresses do not support datagrams",
                ));
            }
        };
        let domain = if addr.is_ipv4() {
            socket2::Domain::IPV4
        } else {
            socket2::Domain::IPV6
        };
        let socket =
            socket2::Socket::new(domain, socket2::Type::DGRAM, Some(socket2::Protocol::UDP))
                .map_err(op("socket()"))?;
        socket
            .set_reuse_address(true)
            .map_err(op("setsockopt(SO_REUSEADDR)"))?;
        if wildcard && addr.is_ipv6() {
            socket
                .set_only_v6(false)
                .map_err(op("setsockopt(IPV6_V6ONLY)"))?;
        }
        socket.bind(&addr.into()).map_err(op("bind()"))?;
        socket.set_nonblocking(true).map_err(op("fcntl()"))?;
        let socket = UdpSocket::from_std(socket.into()).map_err(op("bindDatagramPort()"))?;
        Ok(Box::new(TokioDatagram {
            shared: Arc::new(DatagramShared {
                socket,
                owner: crate::current_loop_runtime_id()?,
            }),
        }))
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
            Spec::Loopback(queue) => [b"loopback:".as_slice(), queue.name()].concat(),
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
    Ok(Box::new(TokioStream::new(socket)?))
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
            Box::new(TokioStream::new(Socket::Unix(first))?),
            Box::new(TokioStream::new(Socket::Unix(second))?),
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
/// readiness; or a loopback queue. A handle to `Arc`-shared state; each `accept()` future owns a
/// share.
pub struct TokioListener {
    shared: Arc<ListenerShared>,
}

struct ListenerShared {
    backend: ListenerBackend,
    /// The runtime the sockets are registered with (lib.rs, `ensure_owner_loop`).
    owner: tokio::runtime::Id,
}

enum ListenerBackend {
    Sockets {
        /// Never empty.
        inners: Vec<ListenerInner>,
        /// Round-robin start index for the next `accept()` poll, so a busy first socket cannot
        /// starve the others.
        next: AtomicUsize,
    },
    Loopback(Arc<LoopbackQueue>),
}

impl ListenerBackend {
    fn sockets(inners: Vec<ListenerInner>) -> Self {
        Self::Sockets {
            inners,
            next: AtomicUsize::new(0),
        }
    }
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

impl ListenerBackend {
    fn poll_accept_any(
        inners: &[ListenerInner],
        next: &AtomicUsize,
        cx: &mut Context<'_>,
    ) -> Poll<std::io::Result<(Socket, SocketAddress)>> {
        let count = inners.len();
        let start = next.load(Ordering::Relaxed);
        for offset in 0..count {
            let index = (start + offset) % count;
            if let Poll::Ready(result) = inners[index].poll_accept(cx) {
                next.store((index + 1) % count, Ordering::Relaxed);
                return Poll::Ready(result);
            }
        }
        Poll::Pending
    }
}

impl ListenerShared {
    /// One accepted connection: KJ's transient errors are retried here; whether the peer is
    /// allowed is the adapter's decision (module docs, "Where filtering happens").
    async fn accept(&self) -> Result<PeerStream> {
        crate::ensure_owner_loop(self.owner)?;
        let (inners, next) = match &self.backend {
            ListenerBackend::Sockets { inners, next } => (inners, next),
            ListenerBackend::Loopback(queue) => {
                return Ok(PeerStream {
                    stream: queue.accept().await?,
                    peer: SocketAddress::loopback(queue.name()),
                });
            }
        };
        loop {
            let (socket, peer) =
                match std::future::poll_fn(|cx| ListenerBackend::poll_accept_any(inners, next, cx))
                    .await
                {
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
                stream: Box::new(TokioStream::new(socket)?),
                peer,
            });
        }
    }
}

impl TokioListener {
    fn new(backend: ListenerBackend) -> Result<Self> {
        Ok(Self {
            shared: Arc::new(ListenerShared {
                backend,
                owner: crate::current_loop_runtime_id()?,
            }),
        })
    }

    /// `kj::ConnectionReceiver::getPort`: the first socket's, like KJ's aggregate receiver; 0 for
    /// a loopback receiver, as for KJ's non-IP receivers.
    fn port(&self) -> Result<u16> {
        match &self.shared.backend {
            ListenerBackend::Sockets { inners, .. } => inners[0].port(),
            ListenerBackend::Loopback(_) => Ok(0),
        }
    }

    /// `kj::ConnectionReceiver::getsockname`: the first socket's.
    fn local_addr(&self) -> Result<SocketAddress> {
        match &self.shared.backend {
            ListenerBackend::Sockets { inners, .. } => inners[0].local_addr(),
            ListenerBackend::Loopback(queue) => Ok(SocketAddress::loopback(queue.name())),
        }
    }
}

// ======================================================================================
// Bridge entry points (see ffi.rs)

pub async fn parse_address(
    addr: &[u8],
    port_hint: u16,
    loopback: &LoopbackRegistry,
) -> Result<Box<TokioAddress>> {
    Ok(Box::new(
        TokioAddress::parse(addr, port_hint, loopback).await?,
    ))
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

pub fn connect_target(
    addr: &TokioAddress,
    target: SocketAddress,
) -> impl Future<Output = Result<Box<TokioStream>>> + use<> {
    addr.connect(target)
}

pub fn address_listen(addr: &TokioAddress) -> Result<Box<TokioListener>> {
    addr.listen()
}

pub fn address_bind_datagram(addr: &TokioAddress) -> Result<Box<TokioDatagram>> {
    addr.bind_datagram()
}

pub fn datagram_send(
    datagram: &TokioDatagram,
    data: &[u8],
    destination: SocketAddress,
) -> impl Future<Output = Result<usize>> + use<> {
    let shared = Arc::clone(&datagram.shared);
    let data = data.to_vec();
    async move {
        crate::ensure_owner_loop(shared.owner)?;
        shared
            .socket
            .send_to(&data, ip_socket_addr(&destination)?)
            .await
            .map_err(op("sendto()"))
    }
}

pub fn datagram_receive(
    datagram: &TokioDatagram,
    capacity: usize,
) -> impl Future<Output = Result<crate::ffi::ReceivedDatagram>> + use<> {
    let shared = Arc::clone(&datagram.shared);
    async move {
        crate::ensure_owner_loop(shared.owner)?;
        let receive_capacity = capacity
            .checked_add(1)
            .ok_or_else(|| KjIoError::other("recvfrom()", "datagram capacity is too large"))?;
        let mut data = vec![0; receive_capacity];
        let (size, source) = shared
            .socket
            .recv_from(&mut data)
            .await
            .map_err(op("recvfrom()"))?;
        let truncated = size > capacity;
        data.truncate(size.min(capacity));
        Ok(crate::ffi::ReceivedDatagram {
            data,
            source: SocketAddress::from(source),
            truncated,
        })
    }
}

pub fn datagram_port(datagram: &TokioDatagram) -> Result<u16> {
    Ok(datagram
        .shared
        .socket
        .local_addr()
        .map_err(op("getsockname()"))?
        .port())
}

#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx takes an opaque Rust type boxed"
)]
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
#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx takes an opaque Rust type boxed"
)]
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
    Ok(Box::new(TokioStream::new(socket)?))
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
    Ok(Box::new(TokioListener::new(ListenerBackend::sockets(
        vec![inner],
    ))?))
}

#[cfg(test)]
#[path = "net-test.rs"]
mod tests;
