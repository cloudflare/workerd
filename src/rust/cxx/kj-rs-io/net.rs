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
