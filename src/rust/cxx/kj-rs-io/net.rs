//! Tokio-backed `kj::Network` / `kj::NetworkAddress` / `kj::ConnectionReceiver` backends.
//!
//! Address-string grammar follows KJ's `SocketAddress::parse` (kj/async-io-unix.c++), the
//! formats workerd.capnp documents for `Socket.address` / `ExternalServer.address`:
//!
//! - IPv4: `"1.2.3.4"`, `"1.2.3.4:80"`
//! - IPv6: `"1234:5678::abcd"`, `"[1234:5678::abcd]:80"`
//! - Wildcard (dual-stack): `"*"`, `"*:80"`
//! - Hostnames (DNS via `tokio::net::lookup_host`, i.e. the system resolver on tokio's blocking
//!   pool; duplicates dropped like KJ does): `"example.com"`, `"example.com:80"`
//! - Unix domain: `"unix:/path/to/socket"` (Unix only)
//! - Abstract Unix domain: `"unix-abstract:name"` (Linux only, as in KJ)
//!
//! Known deviations from KJ, both erroring loudly rather than misbehaving: named service ports
//! (`"host:http"`) and IPv6 scope IDs (`"fe80::1%eth0"`) are not supported.
//!
//! Every tokio resource here is created on the KJ loop thread, inside the port's runtime context
//! (`require_loop_runtime`, checked at creation); existing resources are then polled without any
//! per-operation check, as tokio itself does. Listeners follow the ownership model described in
//! stream.rs: an `accept()` future owns a share of the listener, so destroying the receiver with
//! an accept pending is memory-safe.

use std::cell::Cell;
use std::future::Future;
use std::net::IpAddr;
use std::net::SocketAddr;
use std::rc::Rc;
use std::task::Context;
use std::task::Poll;

use kj_rs::KjOwn;
use kj_rs::KjRc;
use tokio::net::TcpListener;
use tokio::net::TcpStream;
#[cfg(unix)]
use tokio::net::UnixListener;
#[cfg(unix)]
use tokio::net::UnixStream;

use crate::error::KjIoError;
use crate::error::Result;
use crate::error::op;
use crate::ffi::AcceptedStream;
use crate::ffi::NetworkFilter;
use crate::ffi::PeerFilter;
use crate::ffi::network_filter_should_allow;
use crate::ffi::peer_filter_should_allow_parse;
use crate::ffi::sockaddr_from_bytes;
use crate::ffi::sockaddr_to_bytes;
use crate::runtime::require_loop_runtime;
use crate::stream::TokioStream;

/// KJ parity: `::listen(fd, SOMAXCONN)`.
#[cfg(unix)]
const LISTEN_BACKLOG: i32 = libc::SOMAXCONN;
/// winsock's `SOMAXCONN` ("a reasonable maximum", 0x7fffffff).
#[cfg(windows)]
const LISTEN_BACKLOG: i32 = 0x7fff_ffff;

/// A parsed network address: one or more socket addresses to try in order.
pub struct TokioAddress {
    spec: Spec,
}

#[derive(Clone)]
enum Spec {
    Ip {
        /// Resolved addresses (no duplicates), tried in order by `connect()`; `listen()` binds
        /// every one of them (KJ's aggregate receiver).
        addrs: Vec<SocketAddr>,
        /// `"*"`: listen on `[::]` with `IPV6_V6ONLY` disabled (dual-stack), reject `connect()`.
        wildcard: bool,
        /// Written as an IP literal (or built from a raw `sockaddr`) rather than resolved from a
        /// hostname. KJ applies its parse-time `restrictPeers` check to literals only -- DNS
        /// results are checked at `connect()` -- so the C++ side needs to know which this is.
        literal: bool,
    },
    #[cfg(unix)]
    Unix(UnixTarget),
}

/// A Unix-domain endpoint, in the two forms KJ's grammar names.
#[cfg(unix)]
#[derive(Clone)]
enum UnixTarget {
    /// `unix:/path`: a filesystem socket.
    Path(std::path::PathBuf),
    /// `unix-abstract:name`: Linux's abstract namespace (a leading NUL in `sun_path`).
    #[cfg(target_os = "linux")]
    Abstract(Vec<u8>),
}

/// The three shapes a `struct sockaddr_un` takes -- the distinction KJ's peer filter makes
/// (`safeUnixPath`): a pathname socket is judged by the "unix" rule, an abstract one by
/// "unix-abstract", and an unnamed one (a client that never bound, the common case) as "unix".
#[cfg(unix)]
#[derive(Clone, Copy)]
enum UnixPeer<'a> {
    Path(&'a [u8]),
    #[cfg(target_os = "linux")]
    Abstract(&'a [u8]),
    Unnamed,
}

/// Builds `struct sockaddr_un` bytes exactly as KJ's `SocketAddress::parse` does: the family
/// field set, `sun_path` = `path + NUL` (length counts the NUL), or `NUL + name` for the abstract
/// namespace (length excludes any trailing NUL), or nothing for an unnamed socket. Layout comes
/// from libc's struct (BSDs have a leading `sun_len` byte, left zero like KJ leaves it).
#[cfg(unix)]
fn sockaddr_un_bytes(peer: UnixPeer<'_>) -> Result<Vec<u8>> {
    let path_offset = std::mem::offset_of!(libc::sockaddr_un, sun_path);
    let capacity = std::mem::size_of::<libc::sockaddr_un>() - path_offset;
    let payload: Vec<u8> = match peer {
        UnixPeer::Path(path) => {
            if path.contains(&0) {
                return Err(KjIoError::other(
                    "parseAddress",
                    "Unix domain socket address contains NULL. Use 'unix-abstract:' for the \
                     abstract namespace.",
                ));
            }
            let mut p = path.to_vec();
            p.push(0);
            p
        }
        #[cfg(target_os = "linux")]
        UnixPeer::Abstract(name) => {
            let mut p = Vec::with_capacity(name.len() + 1);
            p.push(0);
            p.extend_from_slice(name);
            p
        }
        UnixPeer::Unnamed => Vec::new(),
    };
    if payload.len() > capacity {
        return Err(KjIoError::other(
            "parseAddress",
            "Unix domain socket address is too long.",
        ));
    }
    let mut bytes = vec![0u8; path_offset];
    let family = libc::sa_family_t::try_from(libc::AF_UNIX)
        .map_err(|_| KjIoError::other("sockaddr", "AF_UNIX does not fit sa_family_t"))?
        .to_ne_bytes();
    let family_offset = std::mem::offset_of!(libc::sockaddr_un, sun_family);
    bytes[family_offset..family_offset + family.len()].copy_from_slice(&family);
    bytes.extend_from_slice(&payload);
    Ok(bytes)
}

#[cfg(unix)]
impl UnixTarget {
    fn sockaddr_bytes(&self) -> Result<Vec<u8>> {
        use std::os::unix::ffi::OsStrExt;
        match self {
            Self::Path(path) => sockaddr_un_bytes(UnixPeer::Path(path.as_os_str().as_bytes())),
            #[cfg(target_os = "linux")]
            Self::Abstract(name) => sockaddr_un_bytes(UnixPeer::Abstract(name)),
        }
    }

    fn sockaddr(&self) -> Result<socket2::SockAddr> {
        sockaddr_from_bytes(&self.sockaddr_bytes()?)
    }

    fn display(&self) -> String {
        match self {
            Self::Path(path) => format!("unix:{}", path.display()),
            #[cfg(target_os = "linux")]
            Self::Abstract(name) => format!("unix-abstract:{}", String::from_utf8_lossy(name)),
        }
    }
}

/// KJ's `lookupHost` deduplicates `getaddrinfo` results ("`getaddrinfo()` can return multiple
/// copies of the same address for several reasons"); a duplicate would make `listen()` bind the
/// same endpoint twice (EADDRINUSE) and `connect()` retry a refused address. Order preserved.
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
    /// resolution produces; reserved for native Rust consumers and tests.
    #[must_use]
    pub fn from_socket_addrs(addrs: Vec<SocketAddr>) -> Self {
        Self {
            spec: Spec::Ip {
                addrs: dedup_in_order(addrs),
                wildcard: false,
                literal: false,
            },
        }
    }

    /// The `unix:` / `unix-abstract:` forms, or `None` for anything else.
    fn parse_unix(text: &str) -> Option<Result<Self>> {
        if let Some(path) = text.strip_prefix("unix:") {
            #[cfg(unix)]
            {
                let target = UnixTarget::Path(path.into());
                // KJ's length / embedded-NUL checks happen at parse time.
                return Some(target.sockaddr_bytes().map(|_| Self {
                    spec: Spec::Unix(target),
                }));
            }
            #[cfg(windows)]
            {
                let _ = path;
                return Some(Err(KjIoError::other(
                    "parseAddress",
                    "Unix domain sockets are not supported on this platform",
                )));
            }
        }
        if let Some(name) = text.strip_prefix("unix-abstract:") {
            #[cfg(target_os = "linux")]
            {
                let target = UnixTarget::Abstract(name.as_bytes().to_vec());
                return Some(target.sockaddr_bytes().map(|_| Self {
                    spec: Spec::Unix(target),
                }));
            }
            #[cfg(not(target_os = "linux"))]
            {
                let _ = name;
                return Some(Err(KjIoError::other(
                    "parseAddress",
                    "abstract Unix domain sockets exist only on Linux",
                )));
            }
        }
        None
    }

    async fn parse(text: &str, port_hint: u16) -> Result<Self> {
        if let Some(unix) = Self::parse_unix(text) {
            return unix;
        }

        // Split into address and port parts, exactly like KJ's SocketAddress::parse.
        let (addr_part, port_part) = if let Some(rest) = text.strip_prefix('[') {
            // Bracketed IPv6, optionally "[..]:port".
            let close = rest.rfind(']').ok_or_else(|| {
                KjIoError::other("parseAddress", format!("Unclosed '[' in address: {text}"))
            })?;
            let addr = &rest[..close];
            let tail = &rest[close + 1..];
            if tail.is_empty() {
                (addr, None)
            } else if let Some(port) = tail.strip_prefix(':') {
                (addr, Some(port))
            } else {
                return Err(KjIoError::other(
                    "parseAddress",
                    format!("Expected port suffix after ']': {text}"),
                ));
            }
        } else if let Some(colon) = text.find(':') {
            if text[colon + 1..].contains(':') {
                // Two or more colons, no brackets: a bare IPv6 address with no port.
                (text, None)
            } else {
                // Exactly one colon: ip4/hostname with port.
                (&text[..colon], Some(&text[colon + 1..]))
            }
        } else {
            (text, None)
        };

        let port = match port_part {
            Some(port_text) => port_text.parse::<u16>().map_err(|_| {
                // KJ falls back to getaddrinfo service-name resolution here; tokio's resolver
                // only accepts numeric ports.
                KjIoError::other(
                    "parseAddress",
                    format!("invalid port (named services are not supported): {port_text}"),
                )
            })?,
            None => port_hint,
        };

        if addr_part == "*" {
            return Ok(Self {
                spec: Spec::Ip {
                    addrs: vec![SocketAddr::new(
                        IpAddr::V6(std::net::Ipv6Addr::UNSPECIFIED),
                        port,
                    )],
                    wildcard: true,
                    // KJ resolves "*" through getaddrinfo and applies no parse-time check.
                    literal: false,
                },
            });
        }

        if let Ok(ip) = addr_part.parse::<IpAddr>() {
            return Ok(Self {
                spec: Spec::Ip {
                    addrs: vec![SocketAddr::new(ip, port)],
                    wildcard: false,
                    literal: true,
                },
            });
        }

        // Not a literal: resolve the hostname. `lookup_host` runs getaddrinfo (the system
        // resolver: NSS, /etc/hosts, ...) on the runtime's blocking pool, so it needs the loop
        // runtime; the completion wakes this future through the waker bridge. Dropping the
        // future (KJ promise cancelled) detaches the blocking call, which cannot be interrupted
        // once started -- an OS limitation KJ's own resolver thread shares.
        require_loop_runtime()?;
        let addrs = dedup_in_order(
            tokio::net::lookup_host((addr_part, port))
                .await
                .map_err(op("getaddrinfo()"))?
                .collect(),
        );
        if addrs.is_empty() {
            return Err(KjIoError::other(
                "getaddrinfo()",
                format!("no addresses found for host: {addr_part}"),
            ));
        }
        Ok(Self {
            spec: Spec::Ip {
                addrs,
                wildcard: false,
                literal: false,
            },
        })
    }

    /// Every endpoint to try, in order, copied out so the connect future owns them (see the
    /// module docs), each with its raw `sockaddr` bytes for the filter.
    fn connect_targets(&self) -> Result<Vec<(Vec<u8>, ConnectTarget)>> {
        match &self.spec {
            Spec::Ip {
                addrs, wildcard, ..
            } => {
                if *wildcard {
                    return Err(KjIoError::other(
                        "connect()",
                        "cannot connect() to a wildcard address",
                    ));
                }
                Ok(addrs
                    .iter()
                    .map(|addr| {
                        (
                            sockaddr_to_bytes(&socket2::SockAddr::from(*addr)),
                            ConnectTarget::Ip(*addr),
                        )
                    })
                    .collect())
            }
            #[cfg(unix)]
            Spec::Unix(target) => Ok(vec![(
                target.sockaddr_bytes()?,
                ConnectTarget::Unix(target.sockaddr()?),
            )]),
        }
    }

    /// KJ's parse-time restrictPeers check (`SocketAddress::parse`): a *literal* whose family
    /// the filter forbids is rejected with KJ's own message; DNS results are not checked here
    /// (KJ's `lookupHost` does not either -- they are judged at `connect()`).
    fn check_literal(&self, filter: &KjRc<PeerFilter>) -> Result<()> {
        if !self.is_literal() {
            return Ok(());
        }
        let raw = self.raw_sockaddr(0)?;
        if peer_filter_should_allow_parse(filter, &raw) {
            return Ok(());
        }
        Err(KjIoError::verbatim(match &self.spec {
            Spec::Ip { .. } => "address family blocked by restrictPeers()",
            #[cfg(unix)]
            Spec::Unix(UnixTarget::Path(_)) => "unix sockets blocked by restrictPeers()",
            #[cfg(target_os = "linux")]
            Spec::Unix(UnixTarget::Abstract(_)) => {
                "abstract unix sockets blocked by restrictPeers()"
            }
        }))
    }

    fn raw_sockaddr(&self, index: usize) -> Result<Vec<u8>> {
        match &self.spec {
            Spec::Ip { addrs, .. } => {
                let addr = addrs
                    .get(index)
                    .ok_or_else(|| KjIoError::other("sockaddr", "address index out of range"))?;
                Ok(sockaddr_to_bytes(&socket2::SockAddr::from(*addr)))
            }
            #[cfg(unix)]
            Spec::Unix(target) => {
                if index != 0 {
                    return Err(KjIoError::other("sockaddr", "address index out of range"));
                }
                target.sockaddr_bytes()
            }
        }
    }

    /// Whether this address was written as a literal (IP literal, `unix:`/`unix-abstract:`, or
    /// a raw sockaddr) rather than resolved from a hostname -- see `Spec::Ip::literal`.
    fn is_literal(&self) -> bool {
        match &self.spec {
            Spec::Ip { literal, .. } => *literal,
            #[cfg(unix)]
            Spec::Unix(_) => true,
        }
    }

    /// Binds and listens on every resolved address, like KJ's `NetworkAddressImpl::listen()`
    /// (one socket per address, combined by `newAggregateConnectionReceiver`). Each socket
    /// binds independently, so a hostname with port 0 gets a distinct ephemeral port per
    /// address and `getPort()` reports the first -- also KJ's behavior.
    fn listen(&self) -> Result<Box<TokioListener>> {
        require_loop_runtime()?;
        let inners = match &self.spec {
            Spec::Ip {
                addrs, wildcard, ..
            } => {
                if addrs.is_empty() {
                    return Err(KjIoError::other("listen()", "no addresses to bind"));
                }
                addrs
                    .iter()
                    .map(|addr| bind_tcp(*addr, *wildcard))
                    .collect::<Result<Vec<ListenerInner>>>()?
            }
            #[cfg(unix)]
            Spec::Unix(target) => vec![bind_unix(&target.sockaddr()?)?],
        };
        Ok(Box::new(TokioListener::new(inners)))
    }

    fn to_display_string(&self) -> String {
        match &self.spec {
            Spec::Ip {
                addrs, wildcard, ..
            } => {
                if *wildcard {
                    format!("*:{}", addrs[0].port())
                } else {
                    let parts: Vec<String> = addrs.iter().map(ToString::to_string).collect();
                    parts.join(",")
                }
            }
            #[cfg(unix)]
            Spec::Unix(target) => target.display(),
        }
    }
}

/// An endpoint a connect future owns outright.
enum ConnectTarget {
    Ip(SocketAddr),
    #[cfg(unix)]
    Unix(socket2::SockAddr),
}

impl ConnectTarget {
    async fn connect(self) -> Result<Box<TokioStream>> {
        match self {
            Self::Ip(addr) => {
                let stream = TcpStream::connect(addr).await.map_err(op("connect()"))?;
                // KJ parity: outbound TCP sockets get TCP_NODELAY unconditionally (a hard
                // failure in KJ's SocketAddress::socket(), and here).
                stream
                    .set_nodelay(true)
                    .map_err(op("setsockopt(TCP_NODELAY)"))?;
                Ok(Box::new(TokioStream::from_tcp(stream)))
            }
            #[cfg(unix)]
            Self::Unix(addr) => {
                let socket =
                    socket2::Socket::new(socket2::Domain::UNIX, socket2::Type::STREAM, None)
                        .map_err(op("socket()"))?;
                socket.set_nonblocking(true).map_err(op("fcntl()"))?;
                let stream = connect_unix_nonblocking(socket, &addr).await?;
                Ok(Box::new(TokioStream::from_unix(stream)))
            }
        }
    }
}

/// The non-blocking connect dance for a Unix-domain socket -- `connect(2)`, wait for
/// writability, read `SO_ERROR` -- which tokio only packages for TCP (`TcpSocket::connect`).
/// Covers pathname and abstract addresses alike, since both are just `sockaddr_un` bytes.
#[cfg(unix)]
async fn connect_unix_nonblocking(
    socket: socket2::Socket,
    addr: &socket2::SockAddr,
) -> Result<UnixStream> {
    match socket.connect(addr) {
        Ok(()) => {}
        Err(e)
            if e.raw_os_error() == Some(libc::EINPROGRESS)
                || e.kind() == std::io::ErrorKind::WouldBlock => {}
        Err(e) => return Err(op("connect()")(e)),
    }
    let stream = UnixStream::from_std(socket.into()).map_err(op("connect()"))?;
    stream.writable().await.map_err(op("connect()"))?;
    if let Some(e) = stream.take_error().map_err(op("connect()"))? {
        return Err(op("connect()")(e));
    }
    Ok(stream)
}

/// One listening TCP socket, KJ style: `SO_REUSEADDR`, dual-stack for wildcards
/// (`IPV6_V6ONLY` off), `SOMAXCONN` backlog, non-blocking, CLOEXEC (socket2's default).
fn bind_tcp(addr: SocketAddr, wildcard: bool) -> Result<ListenerInner> {
    let domain = socket2::Domain::for_address(addr);
    let socket =
        socket2::Socket::new(domain, socket2::Type::STREAM, None).map_err(op("socket()"))?;
    // KJ: "We always enable SO_REUSEADDR because having to take your server down for five
    // minutes before it can restart really sucks."
    socket
        .set_reuse_address(true)
        .map_err(op("setsockopt(SO_REUSEADDR)"))?;
    if wildcard {
        socket
            .set_only_v6(false)
            .map_err(op("setsockopt(IPV6_V6ONLY)"))?;
    }
    socket.bind(&addr.into()).map_err(op("bind()"))?;
    socket.listen(LISTEN_BACKLOG).map_err(op("listen()"))?;
    socket.set_nonblocking(true).map_err(op("fcntl()"))?;
    let listener = TcpListener::from_std(socket.into()).map_err(op("wrap listener"))?;
    Ok(ListenerInner::Tcp(listener))
}

/// One listening Unix-domain socket (pathname or abstract). Like KJ, no `unlink()`: binding an
/// existing path fails. Built through socket2 rather than std so the backlog is `SOMAXCONN`
/// (std hard-codes 128).
#[cfg(unix)]
fn bind_unix(addr: &socket2::SockAddr) -> Result<ListenerInner> {
    let socket = socket2::Socket::new(socket2::Domain::UNIX, socket2::Type::STREAM, None)
        .map_err(op("socket()"))?;
    socket.bind(addr).map_err(op("bind()"))?;
    socket.listen(LISTEN_BACKLOG).map_err(op("listen()"))?;
    socket.set_nonblocking(true).map_err(op("fcntl()"))?;
    let listener = UnixListener::from_std(socket.into()).map_err(op("wrap listener"))?;
    Ok(ListenerInner::Unix(listener))
}

/// A `kj::ConnectionReceiver` backend: one or more listening sockets (several when the address
/// resolved to several -- KJ's aggregate receiver), accepted from in round-robin order of
/// readiness. A handle to `Rc`-shared state; each `accept()` future owns a share.
pub struct TokioListener {
    shared: Rc<ListenerShared>,
}

struct ListenerShared {
    /// Never empty.
    inners: Vec<ListenerInner>,
    /// Round-robin start index for the next `accept()` poll, so a busy first socket cannot
    /// starve the others.
    next: Cell<usize>,
}

enum ListenerInner {
    Tcp(TcpListener),
    #[cfg(unix)]
    Unix(UnixListener),
}

/// What `accept(2)` handed back: the connected socket plus the peer address it reported.
enum Accepted {
    Tcp(TcpStream, SocketAddr),
    #[cfg(unix)]
    Unix(UnixStream, tokio::net::unix::SocketAddr),
}

impl ListenerInner {
    fn poll_accept(&self, cx: &mut Context<'_>) -> Poll<std::io::Result<Accepted>> {
        match self {
            Self::Tcp(listener) => listener
                .poll_accept(cx)
                .map_ok(|(stream, peer)| Accepted::Tcp(stream, peer)),
            #[cfg(unix)]
            Self::Unix(listener) => listener
                .poll_accept(cx)
                .map_ok(|(stream, peer)| Accepted::Unix(stream, peer)),
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

    #[cfg(unix)]
    fn as_borrowed_fd(&self) -> std::os::fd::BorrowedFd<'_> {
        use std::os::fd::AsFd;
        match self {
            Self::Tcp(listener) => listener.as_fd(),
            Self::Unix(listener) => listener.as_fd(),
        }
    }

    #[cfg(windows)]
    fn as_borrowed_socket(&self) -> std::os::windows::io::BorrowedSocket<'_> {
        use std::os::windows::io::AsSocket;
        match self {
            Self::Tcp(listener) => listener.as_socket(),
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

/// The accepted Unix peer's `sockaddr_un` bytes, preserving the pathname / abstract / unnamed
/// distinction the filter grammar relies on (a client bound to an abstract name must be judged
/// by the "unix-abstract" rule, not collapsed into an unnamed "unix" peer).
#[cfg(unix)]
fn unix_peer_bytes(peer: tokio::net::unix::SocketAddr) -> Result<Vec<u8>> {
    use std::os::unix::ffi::OsStrExt;
    let peer: std::os::unix::net::SocketAddr = peer.into();
    if let Some(path) = peer.as_pathname() {
        return sockaddr_un_bytes(UnixPeer::Path(path.as_os_str().as_bytes()));
    }
    #[cfg(target_os = "linux")]
    {
        use std::os::linux::net::SocketAddrExt;
        if let Some(name) = peer.as_abstract_name() {
            return sockaddr_un_bytes(UnixPeer::Abstract(name));
        }
    }
    sockaddr_un_bytes(UnixPeer::Unnamed)
}

impl ListenerShared {
    /// Polls every socket once, starting after the one that produced the previous connection.
    fn poll_accept_any(&self, cx: &mut Context<'_>) -> Poll<std::io::Result<Accepted>> {
        let count = self.inners.len();
        let start = self.next.get();
        for offset in 0..count {
            let index = (start + offset) % count;
            if let Poll::Ready(result) = self.inners[index].poll_accept(cx) {
                self.next.set((index + 1) % count);
                return Poll::Ready(result);
            }
        }
        Poll::Pending
    }

    async fn accept(&self) -> Result<AcceptedStream> {
        loop {
            let accepted = match std::future::poll_fn(|cx| self.poll_accept_any(cx)).await {
                Ok(accepted) => accepted,
                Err(e) if is_transient_accept_error(&e) => continue,
                Err(e) => return Err(op("accept()")(e)),
            };
            return Ok(match accepted {
                Accepted::Tcp(stream, peer) => {
                    if let Err(e) = stream.set_nodelay(true)
                        && !is_tolerable_nodelay_error(&e)
                    {
                        return Err(op("setsockopt(TCP_NODELAY)")(e));
                    }
                    AcceptedStream {
                        stream: Box::new(TokioStream::from_tcp(stream)),
                        peer: sockaddr_to_bytes(&socket2::SockAddr::from(peer)),
                    }
                }
                #[cfg(unix)]
                Accepted::Unix(stream, peer) => AcceptedStream {
                    stream: Box::new(TokioStream::from_unix(stream)),
                    peer: unix_peer_bytes(peer)?,
                },
            });
        }
    }
}

impl TokioListener {
    fn new(inners: Vec<ListenerInner>) -> Self {
        Self {
            shared: Rc::new(ListenerShared {
                inners,
                next: Cell::new(0),
            }),
        }
    }

    /// The first socket's port (KJ's aggregate receiver reports its first child).
    fn port(&self) -> Result<u16> {
        self.shared.inners[0].port()
    }

    /// Borrows the first listening socket's fd, for the `getsockopt`/`getsockname` passthroughs
    /// (KJ's aggregate receiver answers those from its first child).
    #[cfg(unix)]
    pub(crate) fn first_borrowed_fd(&self) -> std::os::fd::BorrowedFd<'_> {
        self.shared.inners[0].as_borrowed_fd()
    }

    /// Runs `f` on every listening socket's fd, stopping at the first error (the `setsockopt`
    /// passthrough: KJ's aggregate receiver applies it to all children).
    #[cfg(unix)]
    pub(crate) fn for_each_borrowed_fd(
        &self,
        mut f: impl FnMut(std::os::fd::BorrowedFd<'_>) -> Result<()>,
    ) -> Result<()> {
        self.shared
            .inners
            .iter()
            .try_for_each(|inner| f(inner.as_borrowed_fd()))
    }

    /// Windows counterpart of [`TokioListener::first_borrowed_fd`] (tokio's `TcpListener`
    /// implements `AsSocket`).
    #[cfg(windows)]
    pub(crate) fn first_borrowed_socket(&self) -> std::os::windows::io::BorrowedSocket<'_> {
        self.shared.inners[0].as_borrowed_socket()
    }

    /// Windows counterpart of [`TokioListener::for_each_borrowed_fd`].
    #[cfg(windows)]
    pub(crate) fn for_each_borrowed_socket(
        &self,
        mut f: impl FnMut(std::os::windows::io::BorrowedSocket<'_>) -> Result<()>,
    ) -> Result<()> {
        self.shared
            .inners
            .iter()
            .try_for_each(|inner| f(inner.as_borrowed_socket()))
    }

    /// Raw `struct sockaddr` bytes of the first socket's bound address (the `getsockname()`
    /// passthrough behind `kj::ConnectionReceiver::getsockname`).
    fn local_addr_bytes(&self) -> Result<Vec<u8>> {
        #[cfg(unix)]
        let sock = self.first_borrowed_fd();
        #[cfg(windows)]
        let sock = self.first_borrowed_socket();
        let addr = socket2::SockRef::from(&sock)
            .local_addr()
            .map_err(op("getsockname()"))?;
        Ok(sockaddr_to_bytes(&addr))
    }
}

// ======================================================================================
// Bridge entry points (see ffi.rs).

pub async fn network_parse_address(
    addr: String,
    port_hint: u16,
    filter: KjRc<PeerFilter>,
) -> Result<Box<TokioAddress>> {
    let parsed = TokioAddress::parse(&addr, port_hint).await?;
    parsed.check_literal(&filter)?;
    Ok(Box::new(parsed))
}

pub fn network_get_sockaddr(sockaddr: &[u8]) -> Result<Box<TokioAddress>> {
    let addr = sockaddr_from_bytes(sockaddr)?;
    if let Some(socket_addr) = addr.as_socket() {
        return Ok(Box::new(TokioAddress {
            spec: Spec::Ip {
                addrs: vec![socket_addr],
                wildcard: false,
                literal: true,
            },
        }));
    }
    #[cfg(unix)]
    if let Some(path) = addr.as_pathname() {
        return Ok(Box::new(TokioAddress {
            spec: Spec::Unix(UnixTarget::Path(path.into())),
        }));
    }
    #[cfg(target_os = "linux")]
    if let Some(name) = addr.as_abstract_namespace() {
        return Ok(Box::new(TokioAddress {
            spec: Spec::Unix(UnixTarget::Abstract(name.to_vec())),
        }));
    }
    Err(KjIoError::other(
        "getSockaddr",
        "unsupported sockaddr family",
    ))
}

/// KJ's `NetworkAddressImpl::connect()`: each resolved address in order; an address the filter
/// forbids contributes `connect() blocked by restrictPeers()` (KJ's exact text), a failed
/// connect its own error, and only the last address's error propagates. The future owns its
/// targets and the filter share, capturing no borrow of `addr`.
pub fn address_connect(
    addr: &TokioAddress,
    filter: KjOwn<NetworkFilter>,
) -> impl Future<Output = Result<Box<TokioStream>>> + use<> {
    let targets = addr.connect_targets();
    async move {
        require_loop_runtime()?;
        let mut filter = filter;
        let mut last_error = None;
        for (sockaddr, target) in targets? {
            if !network_filter_should_allow(filter.as_mut(), &sockaddr) {
                last_error = Some(KjIoError::verbatim("connect() blocked by restrictPeers()"));
                continue;
            }
            match target.connect().await {
                Ok(stream) => return Ok(stream),
                Err(e) => last_error = Some(e),
            }
        }
        Err(last_error
            .unwrap_or_else(|| KjIoError::other("connect()", "no addresses to connect to")))
    }
}

pub fn address_listen(addr: &TokioAddress) -> Result<Box<TokioListener>> {
    addr.listen()
}

#[expect(clippy::unnecessary_box_returns)] // Opaque cxx types must cross the bridge boxed.
pub fn address_clone(addr: &TokioAddress) -> Box<TokioAddress> {
    Box::new(TokioAddress {
        spec: addr.spec.clone(),
    })
}

pub fn address_to_string(addr: &TokioAddress) -> String {
    addr.to_display_string()
}

/// The next connection `filter` allows: disallowed peers are dropped silently and accepting
/// continues (KJ's `acceptImpl` behavior). The future owns the listener share and the filter.
pub fn listener_accept(
    listener: &TokioListener,
    filter: KjOwn<NetworkFilter>,
) -> impl Future<Output = Result<AcceptedStream>> + use<> {
    let shared = Rc::clone(&listener.shared);
    async move {
        let mut filter = filter;
        loop {
            let accepted = shared.accept().await?;
            if network_filter_should_allow(filter.as_mut(), &accepted.peer) {
                return Ok(accepted);
            }
            // Dropping `accepted` closes the disallowed connection.
        }
    }
}

pub fn listener_port(listener: &TokioListener) -> Result<u16> {
    listener.port()
}

pub fn listener_local_addr(listener: &TokioListener) -> Result<Vec<u8>> {
    listener.local_addr_bytes()
}

// ======================================================================================
// Typed socket wrapping. Raw handles never reach this module: ffi.rs converts them into owned
// `socket2::Socket`s (the C++ side having normalized KJ's TAKE_OWNERSHIP / ALREADY_CLOEXEC /
// ALREADY_NONBLOCK flags, dup'ing when not taking ownership, so every socket here is owned and
// non-blocking).

/// Wraps an owned, connected stream socket (TCP or Unix domain, detected from its family).
pub fn wrap_socket(socket: socket2::Socket) -> Result<Box<TokioStream>> {
    let local = socket.local_addr().map_err(op("getsockname()"))?;
    require_loop_runtime()?;
    match local.domain() {
        socket2::Domain::IPV4 | socket2::Domain::IPV6 => {
            let stream = TcpStream::from_std(socket.into()).map_err(op("wrapSocketFd"))?;
            Ok(Box::new(TokioStream::from_tcp(stream)))
        }
        #[cfg(unix)]
        socket2::Domain::UNIX => {
            let stream = UnixStream::from_std(socket.into()).map_err(op("wrapSocketFd"))?;
            Ok(Box::new(TokioStream::from_unix(stream)))
        }
        _ => Err(KjIoError::other(
            "wrapSocketFd",
            "unsupported socket family",
        )),
    }
}

/// Wraps an owned, bound and listening socket (TCP or Unix domain, detected from its family).
pub fn wrap_listener(socket: socket2::Socket) -> Result<Box<TokioListener>> {
    let local = socket.local_addr().map_err(op("getsockname()"))?;
    require_loop_runtime()?;
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

/// Owns an unconnected socket from the instant C++ transfers its handle. C++ constructs this
/// synchronously before asking cxx to create the connect future, so dropping that future
/// without polling it still closes the socket.
pub struct OwnedConnectingSocket {
    socket: socket2::Socket,
}

impl OwnedConnectingSocket {
    pub(crate) fn new(socket: socket2::Socket) -> Self {
        Self { socket }
    }
}

/// `kj::LowLevelAsyncIoProvider::wrapConnectingSocketFd`: connects the owned, non-blocking
/// socket to `sockaddr` and wraps the result. TCP goes through tokio's `TcpSocket::connect`;
/// Unix-domain sockets (pathname or abstract, which KJ supports here too) through
/// [`connect_unix_nonblocking`].
pub async fn wrap_connecting_socket(
    socket: OwnedConnectingSocket,
    sockaddr: &[u8],
) -> Result<Box<TokioStream>> {
    let addr = sockaddr_from_bytes(sockaddr)?;
    require_loop_runtime()?;
    let OwnedConnectingSocket { socket } = socket;
    if let Some(socket_addr) = addr.as_socket() {
        // TcpSocket::connect handles the nonblocking connect dance (EINPROGRESS, wait for
        // writability, check SO_ERROR) and registers with the I/O driver.
        let tcp_socket = tokio::net::TcpSocket::from_std_stream(socket.into());
        let stream = tcp_socket
            .connect(socket_addr)
            .await
            .map_err(op("connect()"))?;
        return Ok(Box::new(TokioStream::from_tcp(stream)));
    }
    #[cfg(unix)]
    if addr.is_unix() {
        let stream = connect_unix_nonblocking(socket, &addr).await?;
        return Ok(Box::new(TokioStream::from_unix(stream)));
    }
    Err(KjIoError::other(
        "wrapConnectingSocketFd",
        "unsupported sockaddr family",
    ))
}

#[cfg(test)]
mod tests {
    use std::net::IpAddr;
    use std::net::Ipv4Addr;
    use std::net::Ipv6Addr;
    use std::net::SocketAddr;
    use std::task::Waker;

    use cxx::KjError;
    use static_assertions::assert_not_impl_any;

    use super::*;

    // Like TokioStream: a loop-thread handle to Rc-shared state.
    assert_not_impl_any!(TokioListener: Send, Sync);

    /// `TokioAddress::parse` is `async` only because hostnames go through DNS; every literal
    /// form below resolves without ever awaiting, so a single poll with a no-op waker suffices.
    fn parse_literal(text: &str, port_hint: u16) -> Result<TokioAddress> {
        let mut fut = std::pin::pin!(TokioAddress::parse(text, port_hint));
        let mut cx = Context::from_waker(Waker::noop());
        match fut.as_mut().poll(&mut cx) {
            Poll::Ready(result) => result,
            Poll::Pending => panic!("literal address {text:?} should not await"),
        }
    }

    fn parse_ok(text: &str, port_hint: u16) -> TokioAddress {
        match parse_literal(text, port_hint) {
            Ok(addr) => addr,
            Err(e) => panic!(
                "{text:?} failed to parse: {}",
                KjError::from(e).description()
            ),
        }
    }

    fn parse_err(text: &str, port_hint: u16) -> KjError {
        match parse_literal(text, port_hint) {
            Ok(_) => panic!("{text:?} unexpectedly parsed"),
            Err(e) => KjError::from(e),
        }
    }

    fn ip_addrs(addr: &TokioAddress) -> (&[SocketAddr], bool) {
        match &addr.spec {
            Spec::Ip {
                addrs, wildcard, ..
            } => (addrs, *wildcard),
            #[cfg(unix)]
            Spec::Unix(_) => panic!("expected an IP address"),
        }
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

        // No port: the port hint fills in.
        let addr = parse_ok("1.2.3.4", 8080);
        assert_eq!(ip_addrs(&addr).0[0].port(), 8080);
    }

    #[test]
    fn ipv6_literal_forms() {
        // Bracketed with port.
        let addr = parse_ok("[::1]:443", 0);
        assert_eq!(
            ip_addrs(&addr).0,
            &[SocketAddr::new(IpAddr::V6(Ipv6Addr::LOCALHOST), 443)]
        );
        // Bracketed without port: hint applies.
        let addr = parse_ok("[::1]", 7);
        assert_eq!(ip_addrs(&addr).0[0].port(), 7);
        // Bare IPv6 (two or more colons, no brackets) means "no port" -- never "host:port".
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
    }

    #[cfg(unix)]
    #[test]
    fn unix_forms() {
        match &parse_ok("unix:/tmp/sock", 0).spec {
            Spec::Unix(UnixTarget::Path(path)) => {
                assert_eq!(path, std::path::Path::new("/tmp/sock"));
            }
            _ => panic!("expected a unix path address"),
        }
        // KJ's parse-time checks on the path.
        let too_long = format!("unix:/{}", "x".repeat(200));
        assert!(parse_err(&too_long, 0).description().contains("too long"));
        assert!(
            parse_err("unix:/tmp/a\0b", 0)
                .description()
                .contains("contains NULL")
        );
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn abstract_unix_forms() {
        let addr = parse_ok("unix-abstract:kj-rs-io-test", 0);
        match &addr.spec {
            Spec::Unix(UnixTarget::Abstract(name)) => assert_eq!(name, b"kj-rs-io-test"),
            _ => panic!("expected an abstract unix address"),
        }
        assert_eq!(addr.to_display_string(), "unix-abstract:kj-rs-io-test");
        // KJ's layout: family, then a leading NUL and the name, with no trailing NUL counted.
        let bytes = addr.raw_sockaddr(0).unwrap();
        let off = std::mem::offset_of!(libc::sockaddr_un, sun_path);
        assert_eq!(&bytes[off..], b"\0kj-rs-io-test");
        // Round trip through getSockaddr.
        let back = network_get_sockaddr(&bytes).unwrap();
        assert_eq!(back.to_display_string(), "unix-abstract:kj-rs-io-test");
    }

    #[cfg(not(target_os = "linux"))]
    #[test]
    fn abstract_unix_is_linux_only() {
        assert!(
            parse_err("unix-abstract:foo", 0)
                .description()
                .contains("Linux")
        );
    }

    /// The `sockaddr_un` bytes preserve KJ's three peer shapes, which the filter grammar tells
    /// apart: a pathname (trailing NUL counted), an abstract name (leading NUL, nothing
    /// trailing), and an unnamed peer (family only).
    #[cfg(unix)]
    #[test]
    fn sockaddr_un_bytes_match_kj_layout() {
        let off = std::mem::offset_of!(libc::sockaddr_un, sun_path);
        let path = sockaddr_un_bytes(UnixPeer::Path(b"/tmp/s")).unwrap();
        assert_eq!(&path[off..], b"/tmp/s\0");
        #[cfg(target_os = "linux")]
        {
            let abstract_name = sockaddr_un_bytes(UnixPeer::Abstract(b"nm")).unwrap();
            assert_eq!(&abstract_name[off..], b"\0nm");
            assert!(sockaddr_from_bytes(&abstract_name).unwrap().is_unix());
        }
        let unnamed = sockaddr_un_bytes(UnixPeer::Unnamed).unwrap();
        assert_eq!(unnamed.len(), off);
        for bytes in [&path, &unnamed] {
            let decoded = sockaddr_from_bytes(bytes).unwrap();
            assert!(decoded.is_unix(), "family survives the round trip");
        }
        assert_eq!(
            sockaddr_from_bytes(&path).unwrap().as_pathname(),
            Some(std::path::Path::new("/tmp/s"))
        );
    }

    #[cfg(unix)]
    #[test]
    fn get_sockaddr_rejects_unsupported_family() {
        // A zeroed sockaddr (family AF_UNSPEC) is neither AF_INET/6 nor AF_UNIX, so
        // network_get_sockaddr must reject it rather than fabricate an address. Length is a
        // valid sockaddr_in size so it passes the length check and reaches the family branch.
        let bogus = vec![0u8; core::mem::size_of::<libc::sockaddr_in>()];
        let err = match network_get_sockaddr(&bogus) {
            Ok(_) => panic!("a zeroed (AF_UNSPEC) sockaddr must be rejected"),
            Err(e) => KjError::from(e),
        };
        assert!(
            err.description().contains("unsupported sockaddr family"),
            "{}",
            err.description()
        );
    }

    #[test]
    fn literal_flag_distinguishes_literals_from_resolved_and_wildcard_addresses() {
        assert!(parse_ok("1.2.3.4:80", 0).is_literal());
        assert!(parse_ok("[::1]", 0).is_literal());
        assert!(
            !parse_ok("*:80", 0).is_literal(),
            "KJ resolves '*' through getaddrinfo and applies no parse-time filter to it"
        );
        assert!(!TokioAddress::from_socket_addrs(vec!["1.1.1.1:1".parse().unwrap()]).is_literal());
        #[cfg(unix)]
        assert!(parse_ok("unix:/tmp/sock", 0).is_literal());
    }

    /// Resolver results are deduplicated in order (KJ's `lookupHost` does the same): a
    /// duplicate would make `listen()` bind the same endpoint twice.
    #[test]
    fn duplicate_addresses_are_dropped_in_order() {
        let a: SocketAddr = "1.1.1.1:1".parse().unwrap();
        let b: SocketAddr = "[::2]:2".parse().unwrap();
        assert_eq!(dedup_in_order(vec![a, b, a, b, a]), vec![a, b]);
        let addr = TokioAddress::from_socket_addrs(vec![b, b, a, b]);
        assert_eq!(ip_addrs(&addr).0, &[b, a]);
        // ...so a duplicated endpoint still listens (one socket, not EADDRINUSE).
        let _port = kj_rs_tokio::TokioPort::new();
        let loopback: SocketAddr = "127.0.0.1:0".parse().unwrap();
        let listener = TokioAddress::from_socket_addrs(vec![loopback, loopback])
            .listen()
            .expect("duplicates collapse to one socket");
        assert_eq!(listener.shared.inners.len(), 1);
    }

    /// Randomized: the address grammar must never panic on arbitrary strings, and every literal
    /// it accepts must survive a display round trip. Hostname-shaped inputs are allowed to go
    /// Pending (they start a DNS lookup on the loop's runtime, which needs a port; dropping the
    /// future detaches it). Seeded xorshift, so a failure is reproducible.
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
        let cx_waker = Waker::noop();
        for _ in 0..20_000 {
            let len = usize::try_from(next() % 24).unwrap();
            let text: String = (0..len)
                .map(|_| ALPHABET[usize::try_from(next() % ALPHABET.len() as u64).unwrap()] as char)
                .collect();
            let hint = u16::try_from(next() & 0xffff).unwrap();
            let mut fut = std::pin::pin!(TokioAddress::parse(&text, hint));
            let mut cx = Context::from_waker(cx_waker);
            let Poll::Ready(Ok(addr)) = fut.as_mut().poll(&mut cx) else {
                continue; // rejected, or a hostname now resolving: both fine
            };
            // Round trip: what it prints must parse back to the same addresses.
            let shown = addr.to_display_string();
            let Spec::Ip {
                addrs, wildcard, ..
            } = &addr.spec
            else {
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
    fn documented_rejections() {
        // Named services: tokio's resolver only takes numeric ports.
        let err = parse_err("1.2.3.4:http", 0);
        assert!(
            err.description().contains("named services"),
            "{}",
            err.description()
        );
        // Unclosed bracket.
        let err = parse_err("[::1", 0);
        assert!(
            err.description().contains("Unclosed"),
            "{}",
            err.description()
        );
        // Junk after the closing bracket.
        let err = parse_err("[::1]x", 0);
        assert!(
            err.description().contains("Expected port suffix"),
            "{}",
            err.description()
        );
        // Out-of-range port.
        let _ = parse_err("1.2.3.4:70000", 0);
    }

    /// KJ's accept retry set: an accepted-but-already-broken connection must not take down
    /// the listener, while a real failure (EBADF: the listening socket is gone) must.
    #[cfg(unix)]
    #[test]
    fn accept_error_classification_matches_kj() {
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

        // TCP_NODELAY on a just-reset socket: tolerated where KJ tolerates it, never EBADF.
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

    /// Listening on an address that resolved to several socket addresses binds every one of
    /// them (KJ's aggregate receiver): both are connectable at the kernel level. (Accepting from
    /// each needs a driven reactor, which only the C++ tests have -- see "`listen()` on a
    /// multi-address hostname" in `tests/async-io-test.c++`.)
    #[test]
    fn listen_binds_every_resolved_address() {
        let _port = kj_rs_tokio::TokioPort::new();
        // Find a port free on both loopback families: bind v4 ephemeral, then require v6 to
        // take the same number (retry a few times if it is taken).
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
            sockaddr_from_bytes(&listener.local_addr_bytes().unwrap())
                .unwrap()
                .as_socket(),
            Some(v6),
            "getsockname() reports the first socket"
        );
        // Both families are reachable; the kernel completes the handshakes into the backlogs.
        std::net::TcpStream::connect(v4).expect("IPv4 connect");
        std::net::TcpStream::connect(v6).expect("IPv6 connect");
    }
}
