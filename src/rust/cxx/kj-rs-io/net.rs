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

        Self::parse_inet(text, port_hint).await
    }
}

impl TokioAddress {
    async fn parse_inet(text: &str, port_hint: u16) -> Result<Self> {
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

        // Not a literal: resolve the hostname via getaddrinfo. To honor kj-rs's single-thread
        // axiom, the blocking getaddrinfo runs on tokio's blocking pool but its completion is
        // absorbed by tokio's own scheduler (via a runtime task) and forwarded on the loop thread,
        // so this await resumes same-thread (never cross-thread). See `resolve_host`.
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
}

impl TokioAddress {
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
}

impl TokioAddress {
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
