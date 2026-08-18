//! The native-serve entry points: give a Rust server (any tokio consumer) the
//! best-available tokio-side byte stream for an owned `kj::AsyncIoStream`.
//!
//! Three tiers, two entry points:
//! (1) kj-rs-io-originated streams give up their native tokio socket (the hollow
//! wrapper is destroyed); (2) [`take_kj_socket`] only — a foreign fd-backed stream's fd is
//! duplicated into a fresh tokio socket; (3) [`serve_kj_stream`] only — any other foreign
//! stream is bridged through an in-memory duplex plus a pump future that owns it. Ownership
//! arrives as a [`KjOwn`], so both entry points are safe functions: there is no
//! keep-it-alive caller contract, and the stream is destroyed by the tier that consumed it.
//! See the two entry points' docs for the remaining semantics, in particular why the fd tier
//! is caller-asserted and never automatic.
//!
//! # Pump semantics (matching the hand-built pumps this subsumes)
//!
//! - Bidirectional; each direction ends independently.
//! - Half-close propagates both ways: kj-side EOF shuts down the duplex write half (the tokio
//!   consumer reads EOF); the consumer shutting down (or dropping) its duplex end results in
//!   `shutdownWrite()` on the kj stream.
//! - Peer-teardown-shaped kj failures (DISCONNECTED reads/writes) are treated as normal EOF,
//!   not errors — abrupt client disconnects are normal server load.
//! - Dropping the pump future cancels the in-flight bridged kj promises synchronously, drops
//!   the kj-side duplex end (the tokio consumer observes EOF), and destroys the owned kj
//!   stream — the peer observes teardown, not a zombie half-open connection (abort-on-drop).

use std::future::Future;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use cxx::KjError;
use cxx::KjException;
use cxx::KjExceptionType;
use kj_rs::KjOwn;
use tokio::io::AsyncRead;
use tokio::io::AsyncReadExt;
use tokio::io::AsyncWrite;
use tokio::io::AsyncWriteExt;
use tokio::io::DuplexStream;
use tokio::io::ReadBuf;
use tokio::net::TcpStream;
#[cfg(unix)]
use tokio::net::UnixStream;

use crate::ffi::KjAsyncIoStream;
#[cfg(unix)]
use crate::ffi::dup_raw_fd;
#[cfg(unix)]
use crate::ffi::kj_stream_get_handle;
use crate::ffi::split_kj_stream;
use crate::ffi::unwrap_tokio_stream;

/// Read chunk size for the pump fallback.
const PUMP_BUF: usize = 8192;

/// In-memory buffer per direction of the pump's duplex (how far the two sides may run ahead
/// of each other before backpressure).
pub(crate) const DUPLEX_CAPACITY: usize = 4 * PUMP_BUF;

/// Which transport path [`serve_kj_stream`] produced (perf observability: `Pumped` costs FFI
/// promise round-trips per buffer, `Native` costs none).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServePath {
    /// A native tokio socket (unwrap fast path).
    Native,
    /// An in-memory duplex fed by the FFI stream pump.
    Pumped,
}

impl std::fmt::Display for ServePath {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Self::Native => "native",
            Self::Pumped => "pumped",
        })
    }
}

/// The tokio-side byte stream for a served kj stream: a native socket or the consumer end of
/// the pump's duplex.
///
/// Implements `AsyncRead + AsyncWrite`, so it drops into any tokio
/// consumer -- but see [`ServedKjStream::io`] for the thread-affinity contract: only the
/// native variants may be driven off the KJ event-loop thread; `Duplex` is loop-thread-only.
pub enum ServeIo {
    Tcp(TcpStream),
    #[cfg(unix)]
    Unix(UnixStream),
    Duplex(DuplexStream),
}

impl ServeIo {
    /// Which transport path this stream is on (see [`ServePath`]).
    #[must_use]
    pub fn path(&self) -> ServePath {
        match self {
            Self::Tcp(_) => ServePath::Native,
            #[cfg(unix)]
            Self::Unix(_) => ServePath::Native,
            Self::Duplex(_) => ServePath::Pumped,
        }
    }

    /// Sets `TCP_NODELAY` on the underlying socket where applicable (a no-op for Unix-domain
    /// and duplex transports, which have no Nagle to disable).
    ///
    /// # Errors
    ///
    /// Returns the underlying `std::io::Error` if setting the socket option fails.
    pub fn set_nodelay(&self, nodelay: bool) -> std::io::Result<()> {
        match self {
            Self::Tcp(s) => s.set_nodelay(nodelay),
            #[cfg(unix)]
            Self::Unix(_) => Ok(()),
            Self::Duplex(_) => Ok(()),
        }
    }
}

impl AsyncRead for ServeIo {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        match self.get_mut() {
            Self::Tcp(s) => Pin::new(s).poll_read(cx, buf),
            #[cfg(unix)]
            Self::Unix(s) => Pin::new(s).poll_read(cx, buf),
            Self::Duplex(s) => Pin::new(s).poll_read(cx, buf),
        }
    }
}
