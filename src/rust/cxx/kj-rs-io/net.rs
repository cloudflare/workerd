//! Tokio-backed `kj::Network` / `kj::NetworkAddress` / `kj::ConnectionReceiver` backends.
//!
//! Address-string grammar follows KJ's `SocketAddress::parse` (kj/async-io-unix.c++) for the
//! subset workerd feeds it:
//!
//! - IPv4: `"1.2.3.4"`, `"1.2.3.4:80"`
//! - IPv6: `"1234:5678::abcd"`, `"[1234:5678::abcd]:80"`
//! - Wildcard (dual-stack): `"*"`, `"*:80"`
//! - Hostnames (DNS via blocking `getaddrinfo` on tokio's blocking pool, its completion delivered
//!   same-thread through a tokio runtime task — see [`resolve_host`]): `"example.com"`,
//!   `"example.com:80"`
//! - Unix domain: `"unix:/path/to/socket"` (Unix only)
//!
//! Known deviations from KJ, all erroring loudly rather than misbehaving: named services
//! (`"host:http"`), `unix-abstract:` addresses, and IPv6 scope IDs (`"fe80::1%eth0"`) are not
//! supported.

use std::net::IpAddr;
use std::net::SocketAddr;
use std::net::ToSocketAddrs;

use tokio::net::TcpListener;
use tokio::net::TcpStream;
#[cfg(unix)]
use tokio::net::UnixListener;
#[cfg(unix)]
use tokio::net::UnixStream;

use crate::error::KjIoError;
use crate::error::Result;
use crate::error::op;
use crate::runtime::runtime_handle;
use crate::runtime::with_runtime;
use crate::stream::TokioStream;

const LISTEN_BACKLOG: i32 = 1024;

/// A parsed network address: one or more socket addresses to try in order.
pub struct TokioAddress {
    spec: Spec,
}

#[derive(Clone)]
enum Spec {
    Ip {
        /// Resolved addresses, tried in order by `connect()`; `listen()` binds the first one
        /// (mirroring KJ, which also only listens on the first result).
        addrs: Vec<SocketAddr>,
        /// `"*"`: listen on `[::]` with `IPV6_V6ONLY` disabled (dual-stack), reject `connect()`.
        wildcard: bool,
    },
    #[cfg(unix)]
    Unix { path: std::path::PathBuf },
}

impl TokioAddress {
    async fn parse(text: &str, port_hint: u16) -> Result<Self> {
        if let Some(path) = text.strip_prefix("unix:") {
            #[cfg(unix)]
            {
                return Ok(Self {
                    spec: Spec::Unix { path: path.into() },
                });
            }
            #[cfg(not(unix))]
            {
                let _ = path;
                return Err(KjIoError::other(
                    "parseAddress",
                    "Unix domain sockets are not supported on this platform",
                ));
            }
        }
        if text.starts_with("unix-abstract:") {
            return Err(KjIoError::other(
                "parseAddress",
                "abstract Unix domain sockets are not implemented by kj-rs-io",
            ));
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
                },
            });
        }

        if let Ok(ip) = addr_part.parse::<IpAddr>() {
            return Ok(Self {
                spec: Spec::Ip {
                    addrs: vec![SocketAddr::new(ip, port)],
                    wildcard: false,
                },
            });
        }

        // Not a literal: resolve the hostname via getaddrinfo on tokio's blocking pool. The
        // completion is absorbed by a runtime task and forwarded to the loop thread so this
        // await resumes same-thread -- an optimization (a cross-thread wake would go through
        // the waker bridge's cross-thread fulfiller, which is legal but slower); see
        // `resolve_host` for the full rationale.
        let addrs: Vec<SocketAddr> = resolve_host(addr_part, port).await?;
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
            },
        })
    }

    async fn connect_index(&self, index: usize) -> Result<Box<TokioStream>> {
        match &self.spec {
            Spec::Ip { addrs, wildcard } => {
                if *wildcard {
                    return Err(KjIoError::other(
                        "connect()",
                        "cannot connect() to a wildcard address",
                    ));
                }
                let addr = addrs
                    .get(index)
                    .ok_or_else(|| KjIoError::other("connect()", "address index out of range"))?;
                let stream = TcpStream::connect(addr).await.map_err(op("connect()"))?;
                Ok(Box::new(TokioStream::from_tcp(stream)))
            }
            #[cfg(unix)]
            Spec::Unix { path } => {
                if index != 0 {
                    return Err(KjIoError::other("connect()", "address index out of range"));
                }
                let stream = UnixStream::connect(path).await.map_err(op("connect()"))?;
                Ok(Box::new(TokioStream::from_unix(stream)))
            }
        }
    }

    fn count(&self) -> usize {
        match &self.spec {
            Spec::Ip { addrs, .. } => addrs.len(),
            #[cfg(unix)]
            Spec::Unix { .. } => 1,
        }
    }

    fn raw_sockaddr(&self, index: usize) -> Result<Vec<u8>> {
        let sockaddr: socket2::SockAddr = match &self.spec {
            Spec::Ip { addrs, .. } => (*addrs
                .get(index)
                .ok_or_else(|| KjIoError::other("sockaddr", "address index out of range"))?)
            .into(),
            #[cfg(unix)]
            Spec::Unix { path } => {
                if index != 0 {
                    return Err(KjIoError::other("sockaddr", "address index out of range"));
                }
                socket2::SockAddr::unix(path).map_err(op("sockaddr"))?
            }
        };
        Ok(crate::ffi::sockaddr_to_bytes(&sockaddr))
    }

    fn listen(&self) -> Result<Box<TokioListener>> {
        let handle = runtime_handle()?;
        match &self.spec {
            Spec::Ip { addrs, wildcard } => {
                let addr = *addrs
                    .first()
                    .ok_or_else(|| KjIoError::other("listen()", "no addresses to bind"))?;
                let domain = socket2::Domain::for_address(addr);
                let socket = socket2::Socket::new(domain, socket2::Type::STREAM, None)
                    .map_err(op("socket()"))?;
                // KJ parity: SO_REUSEADDR on listeners; wildcard sockets accept both address
                // families (IPV6_V6ONLY off).
                socket.set_reuse_address(true).map_err(op("setsockopt()"))?;
                if *wildcard {
                    socket.set_only_v6(false).map_err(op("setsockopt()"))?;
                }
                socket.bind(&addr.into()).map_err(op("bind()"))?;
                socket.listen(LISTEN_BACKLOG).map_err(op("listen()"))?;
                socket.set_nonblocking(true).map_err(op("fcntl()"))?;
                // Registering with the I/O driver requires the runtime context.
                let _guard = handle.enter();
                let listener = TcpListener::from_std(socket.into()).map_err(op("wrap listener"))?;
                Ok(Box::new(TokioListener {
                    inner: ListenerInner::Tcp(listener),
                }))
            }
            #[cfg(unix)]
            Spec::Unix { path } => {
                // Like KJ, no unlink(): binding an existing path fails.
                let listener =
                    std::os::unix::net::UnixListener::bind(path).map_err(op("bind()"))?;
                listener.set_nonblocking(true).map_err(op("fcntl()"))?;
                let _guard = handle.enter();
                let listener = UnixListener::from_std(listener).map_err(op("wrap listener"))?;
                Ok(Box::new(TokioListener {
                    inner: ListenerInner::Unix(listener),
                }))
            }
        }
    }

    fn to_display_string(&self) -> String {
        match &self.spec {
            Spec::Ip { addrs, wildcard } => {
                if *wildcard {
                    format!("*:{}", addrs[0].port())
                } else {
                    let parts: Vec<String> = addrs.iter().map(ToString::to_string).collect();
                    parts.join(",")
                }
            }
            #[cfg(unix)]
            Spec::Unix { path } => format!("unix:{}", path.display()),
        }
    }
}

/// A listening socket (`kj::ConnectionReceiver` backend).
pub struct TokioListener {
    inner: ListenerInner,
}

enum ListenerInner {
    Tcp(TcpListener),
    #[cfg(unix)]
    Unix(UnixListener),
}

impl TokioListener {
    async fn accept(&self) -> Result<Box<TokioStream>> {
        match &self.inner {
            ListenerInner::Tcp(listener) => {
                let (stream, _peer) = listener.accept().await.map_err(op("accept()"))?;
                let _ = stream.set_nodelay(true);
                Ok(Box::new(TokioStream::from_tcp(stream)))
            }
            #[cfg(unix)]
            ListenerInner::Unix(listener) => {
                let (stream, _peer) = listener.accept().await.map_err(op("accept()"))?;
                Ok(Box::new(TokioStream::from_unix(stream)))
            }
        }
    }

    fn port(&self) -> Result<u16> {
        match &self.inner {
            ListenerInner::Tcp(listener) => {
                Ok(listener.local_addr().map_err(op("getsockname()"))?.port())
            }
            // KJ returns 0 for non-IP listeners.
            #[cfg(unix)]
            ListenerInner::Unix(_) => Ok(0),
        }
    }

    /// Borrows the live listener socket's fd (tokio listeners implement `AsFd`), for the
    /// sockopt/sockname passthrough behind `kj::ConnectionReceiver`.
    #[cfg(unix)]
    pub(crate) fn as_borrowed_fd(&self) -> std::os::fd::BorrowedFd<'_> {
        use std::os::fd::AsFd;
        match &self.inner {
            ListenerInner::Tcp(listener) => listener.as_fd(),
            ListenerInner::Unix(listener) => listener.as_fd(),
        }
    }

    /// Borrows the live listener socket's `SOCKET` (tokio's `TcpListener` implements
    /// `AsSocket`): the Windows counterpart of [`TokioListener::as_borrowed_fd`]. On Windows
    /// only the Tcp variant of `ListenerInner` exists.
    // Validated by Windows CI; mirrors the unix arm.
    #[cfg(windows)]
    pub(crate) fn as_borrowed_socket(&self) -> std::os::windows::io::BorrowedSocket<'_> {
        use std::os::windows::io::AsSocket;
        match &self.inner {
            ListenerInner::Tcp(listener) => listener.as_socket(),
        }
    }

    /// Raw `struct sockaddr` bytes of the listener's bound address (the `getsockname()`
    /// passthrough behind `kj::ConnectionReceiver::getsockname`).
    #[cfg(any(unix, windows))]
    fn local_addr_bytes(&self) -> Result<Vec<u8>> {
        #[cfg(unix)]
        let sock = self.as_borrowed_fd();
        // Validated by Windows CI; mirrors the unix arm.
        #[cfg(windows)]
        let sock = self.as_borrowed_socket();
        let addr = socket2::SockRef::from(&sock)
            .local_addr()
            .map_err(op("getsockname()"))?;
        Ok(crate::ffi::sockaddr_to_bytes(&addr))
    }

    #[cfg(not(any(unix, windows)))]
    fn local_addr_bytes(&self) -> Result<Vec<u8>> {
        Err(KjIoError::other(
            "getsockname",
            "not implemented by kj-rs-io on this platform",
        ))
    }
}

// ======================================================================================
// Bridge entry points (see lib.rs).

/// Resolves a hostname via blocking `getaddrinfo` on tokio's blocking pool. A task on the loop's
/// `LocalSet` owns the blocking `JoinHandle`, so the blocking-pool completion wakes tokio's own
/// scheduler waker and the result comes back over a oneshot, waking the awaiting future
/// same-thread. The kj-rs waker bridge is thread-safe, so `tokio::net::lookup_host` (whose
/// `JoinHandle` wake lands on the caller's waker cross-thread) would also be *correct*; this
/// shape is kept as an optimization — it keeps every bridged-waker wake on the loop thread's
/// fast path instead of a cross-thread fulfiller hop per lookup. Same blocking `getaddrinfo`
/// call and NSS/`/etc/hosts` parity as `lookup_host`.
async fn resolve_host(host: &str, port: u16) -> Result<Vec<SocketAddr>> {
    let host = host.to_owned();
    let (tx, rx) = tokio::sync::oneshot::channel::<std::io::Result<Vec<SocketAddr>>>();

    // The forwarding task's waker is tokio's own scheduler waker (Send + Sync), so the
    // cross-thread completion from the blocking pool terminates inside tokio's scheduler
    // (unparking this loop), never at a rust cross-thread waker. The task forwards the result on
    // the loop thread, waking the awaiting future same-thread. Spawned onto the loop's LocalSet
    // (kj_rs_tokio::spawn) so it is cancelled with the loop rather than with the runtime.
    let task = kj_rs_tokio::spawn(async move {
        let resolved = match tokio::task::spawn_blocking(move || {
            (host.as_str(), port)
                .to_socket_addrs()
                .map(std::iter::Iterator::collect::<Vec<SocketAddr>>)
        })
        .await
        {
            Ok(result) => result,
            Err(_) => Err(std::io::Error::other("getaddrinfo task failed")),
        };
        let _ = tx.send(resolved);
    });
    // If this future is dropped (KJ promise cancelled), abort the forwarding task rather than
    // leaving it to run to completion for nobody. The blocking getaddrinfo call itself cannot be
    // interrupted once started (an OS limitation shared with KJ's own resolver and tokio's
    // `lookup_host`), so its blocking-pool slot is reclaimed only when the syscall returns.
    let _abort_guard = crate::runtime::AbortOnDrop(task);

    match rx.await {
        Ok(result) => result.map_err(op("getaddrinfo()")),
        Err(_) => Err(KjIoError::other(
            "getaddrinfo()",
            "DNS resolver task dropped",
        )),
    }
}

pub async fn network_parse_address(addr: String, port_hint: u16) -> Result<Box<TokioAddress>> {
    with_runtime(async move { Ok(Box::new(TokioAddress::parse(&addr, port_hint).await?)) }).await
}

pub fn network_get_sockaddr(sockaddr: &[u8]) -> Result<Box<TokioAddress>> {
    #[cfg(any(unix, windows))]
    {
        let addr = sockaddr_from_bytes(sockaddr)?;
        if let Some(socket_addr) = addr.as_socket() {
            return Ok(Box::new(TokioAddress {
                spec: Spec::Ip {
                    addrs: vec![socket_addr],
                    wildcard: false,
                },
            }));
        }
        #[cfg(unix)]
        if let Some(path) = addr.as_pathname() {
            return Ok(Box::new(TokioAddress {
                spec: Spec::Unix { path: path.into() },
            }));
        }
        Err(KjIoError::other(
            "getSockaddr",
            "unsupported sockaddr family",
        ))
    }
    #[cfg(not(any(unix, windows)))]
    {
        let _ = sockaddr;
        Err(KjIoError::other(
            "getSockaddr",
            "not implemented on this platform",
        ))
    }
}

/// Connects to exactly the `index`th resolved address (no fallback). The C++ side drives the
/// try-each-address loop itself so it can apply `restrictPeers()` filtering per address before
/// initiating each connection attempt (KJ parity: a blocked address contributes a
/// "`connect()` blocked by `restrictPeers()`" failure; only the last address's error propagates).
pub async fn address_connect_index(addr: &TokioAddress, index: usize) -> Result<Box<TokioStream>> {
    with_runtime(addr.connect_index(index)).await
}

/// Number of resolved socket addresses behind this address (>= 1).
pub fn address_count(addr: &TokioAddress) -> usize {
    addr.count()
}

/// Raw `struct sockaddr` bytes of the `index`th resolved address, for the C++ side's
/// `kj::_::NetworkFilter` (restrictPeers) checks.
pub fn address_raw_sockaddr(addr: &TokioAddress, index: usize) -> Result<Vec<u8>> {
    addr.raw_sockaddr(index)
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

pub async fn listener_accept(listener: &TokioListener) -> Result<Box<TokioStream>> {
    with_runtime(listener.accept()).await
}

pub fn listener_port(listener: &TokioListener) -> Result<u16> {
    listener.port()
}

pub fn listener_local_addr(listener: &TokioListener) -> Result<Vec<u8>> {
    listener.local_addr_bytes()
}

// ======================================================================================
// Socket-handle wrapping. All handles arrive owned and non-blocking as an `i64` "raw socket
// handle" — a Unix fd or a win32 SOCKET (the C++ side normalizes KJ's TAKE_OWNERSHIP /
// ALREADY_CLOEXEC / ALREADY_NONBLOCK flags, dup'ing when not taking ownership). The platform
// split lives entirely in `ffi::own_socket_from_raw` (the one conversion point); everything
// here operates on the uniform `socket2::Socket` / std / tokio types.

#[cfg(any(unix, windows))]
fn socket_from_raw(handle: i64) -> socket2::Socket {
    crate::ffi::own_socket_from_raw(handle)
}

#[cfg(any(unix, windows))]
fn sockaddr_from_bytes(bytes: &[u8]) -> Result<socket2::SockAddr> {
    crate::ffi::sockaddr_from_bytes(bytes)
}

/// The handle tier of [`crate::take_kj_socket`] (unix only; see that function's docs): wraps
/// an *owned*, connected stream-socket fd (TCP or Unix domain, detected automatically) as a
/// [`crate::serve::ServeIo`]. Unlike [`wrap_socket_fd`] the fd is a fresh dup of a kj stream's
/// socket, so non-blocking mode is forced rather than assumed (the original may have come from
/// anywhere).
#[cfg(unix)]
pub fn serve_io_from_owned_fd(fd: std::os::fd::OwnedFd) -> Result<crate::serve::ServeIo> {
    let socket = socket2::Socket::from(fd);
    socket.set_nonblocking(true).map_err(op("fcntl()"))?;
    let local = socket.local_addr().map_err(op("getsockname()"))?;
    let _guard = runtime_handle()?.enter();
    match local.domain() {
        socket2::Domain::IPV4 | socket2::Domain::IPV6 => {
            let stream = TcpStream::from_std(socket.into()).map_err(op("takeKjSocket"))?;
            Ok(crate::serve::ServeIo::Tcp(stream))
        }
        socket2::Domain::UNIX => {
            let stream = UnixStream::from_std(socket.into()).map_err(op("takeKjSocket"))?;
            Ok(crate::serve::ServeIo::Unix(stream))
        }
        _ => Err(KjIoError::other(
            "takeKjSocket",
            "unsupported socket family",
        )),
    }
}

pub fn wrap_socket_fd(handle: i64) -> Result<Box<TokioStream>> {
    #[cfg(any(unix, windows))]
    {
        let socket = socket_from_raw(handle);
        let local = socket.local_addr().map_err(op("getsockname()"))?;
        let _guard = runtime_handle()?.enter();
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
    #[cfg(not(any(unix, windows)))]
    {
        let _ = handle;
        Err(KjIoError::other(
            "wrapSocketFd",
            "not implemented on this platform",
        ))
    }
}

pub fn wrap_listen_fd(handle: i64) -> Result<Box<TokioListener>> {
    #[cfg(any(unix, windows))]
    {
        let socket = socket_from_raw(handle);
        let local = socket.local_addr().map_err(op("getsockname()"))?;
        let _guard = runtime_handle()?.enter();
        match local.domain() {
            socket2::Domain::IPV4 | socket2::Domain::IPV6 => {
                let listener =
                    TcpListener::from_std(socket.into()).map_err(op("wrapListenSocketFd"))?;
                Ok(Box::new(TokioListener {
                    inner: ListenerInner::Tcp(listener),
                }))
            }
            #[cfg(unix)]
            socket2::Domain::UNIX => {
                let listener =
                    UnixListener::from_std(socket.into()).map_err(op("wrapListenSocketFd"))?;
                Ok(Box::new(TokioListener {
                    inner: ListenerInner::Unix(listener),
                }))
            }
            _ => Err(KjIoError::other(
                "wrapListenSocketFd",
                "unsupported socket family",
            )),
        }
    }
    #[cfg(not(any(unix, windows)))]
    {
        let _ = handle;
        Err(KjIoError::other(
            "wrapListenSocketFd",
            "not implemented on this platform",
        ))
    }
}

pub async fn wrap_connecting_socket_fd(handle: i64, sockaddr: Vec<u8>) -> Result<Box<TokioStream>> {
    #[cfg(any(unix, windows))]
    {
        with_runtime(async move {
            let addr = sockaddr_from_bytes(&sockaddr)?;
            let socket_addr = addr.as_socket().ok_or_else(|| {
                KjIoError::other(
                    "wrapConnectingSocketFd",
                    "only AF_INET/AF_INET6 sockaddrs are supported",
                )
            })?;
            let socket = socket_from_raw(handle);
            // TcpSocket::connect handles the nonblocking connect dance (EINPROGRESS, wait for
            // writability, check SO_ERROR) and registers with the I/O driver.
            let tcp_socket = tokio::net::TcpSocket::from_std_stream(socket.into());
            let stream = tcp_socket
                .connect(socket_addr)
                .await
                .map_err(op("connect()"))?;
            Ok(Box::new(TokioStream::from_tcp(stream)))
        })
        .await
    }
    #[cfg(not(any(unix, windows)))]
    {
        let _ = (handle, sockaddr);
        Err(KjIoError::other(
            "wrapConnectingSocketFd",
            "not implemented on this platform",
        ))
    }
}

#[cfg(test)]
mod tests {
    use std::net::IpAddr;
    use std::net::Ipv4Addr;
    use std::net::Ipv6Addr;
    use std::net::SocketAddr;

    use cxx::KjError;

    use super::*;

    /// `TokioAddress::parse` is `async` only because hostnames go through DNS; every literal
    /// form below resolves without ever awaiting, so a single poll with a no-op waker suffices.
    fn parse_literal(text: &str, port_hint: u16) -> Result<TokioAddress> {
        let mut fut = std::pin::pin!(TokioAddress::parse(text, port_hint));
        let mut cx = std::task::Context::from_waker(std::task::Waker::noop());
        match fut.as_mut().poll(&mut cx) {
            std::task::Poll::Ready(result) => result,
            std::task::Poll::Pending => panic!("literal address {text:?} should not await"),
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
            Spec::Ip { addrs, wildcard } => (addrs, *wildcard),
            #[cfg(unix)]
            Spec::Unix { .. } => panic!("expected an IP address"),
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
            Spec::Unix { path } => assert_eq!(path, std::path::Path::new("/tmp/sock")),
            Spec::Ip { .. } => panic!("expected a unix address"),
        }
        // Abstract sockets are a documented gap.
        assert!(
            parse_err("unix-abstract:foo", 0)
                .description()
                .contains("abstract")
        );
    }

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
}
