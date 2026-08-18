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
