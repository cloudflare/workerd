//! Per-connection I/O-stall watchdog, shared by the inbound server (server.rs) and the
//! outbound client (client.rs).
//!
//! A peer that stops accepting and/or sending bytes and then vanishes without the kernel ever
//! erroring the socket (observed on macOS loopback: the peer's PCB is silently reaped while our
//! side sits zero-window with a full send queue, or mid-body — no RST, no EPIPE, no EOF, ever)
//! would otherwise wedge a connection forever: hyper waits for socket readability/writability,
//! the service or caller blocks on the body channels, and everything downstream waits on that.
//! kj-http rarely hits this because it never buffers more than one write; the kj-hyper paths'
//! pipelined channels make "input consumed, output undeliverable" (and the mirror-image
//! "remaining body bytes never arrive") the common case when peers abort mid-stream.
//!
//! Each connection therefore tracks I/O *progress*: its transport is wrapped in a [`StallIo`]
//! adapter feeding a [`WriteStallTracker`], and a per-connection watchdog future — raced
//! against the connection future — resolves when outstanding I/O has made zero progress for
//! the applicable grace period, upon which the connection is aborted. Any transferred byte
//! resets the clocks, so live-but-slow peers are unaffected. The direction-specific policies
//! (which sides count, drain behavior, upgrade exemptions) are documented in the module docs
//! of server.rs and client.rs and on the watchdog functions below.

use std::pin::Pin;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::PoisonError;
use std::time::Duration;
use std::time::Instant;

use tokio::sync::watch;

/// How long a connection may sit with a socket write outstanding and zero bytes accepted by
/// the kernel before it is considered dead and aborted. Generous: a live peer that is merely
/// slow keeps completing writes as its receive window reopens; only a peer that accepts
/// *nothing* for a full minute trips this. On the client this grace also bounds zero-progress
/// reads (see [`client_stall_watchdog`]).
pub(crate) const WRITE_STALL_GRACE: Duration = Duration::from_secs(60);

/// The tighter bound the inbound server applies once a graceful shutdown (`drain()` /
/// `shutdown()`) was requested, so a draining server is never held up more than ~10 s by
/// connections that can make no progress (the SIGTERM-wedge case). During drain this bound
/// also applies to the *read* side: a connection whose in-flight request is waiting on request
/// bytes that never arrive (client vanished mid-upload without the socket ever erroring) would
/// otherwise hold the drain forever. Outside of drain, read-idleness is normal on the server
/// (keep-alive gaps, slow uploaders) and is never treated as a stall. The outbound client has
/// no drain concept and never uses this bound.
pub(crate) const WRITE_STALL_DRAIN_GRACE: Duration = Duration::from_secs(10);

/// How often the watchdogs sample the tracker.
pub(crate) const WRITE_STALL_CHECK_INTERVAL: Duration = Duration::from_secs(1);

/// The steady-state grace period: [`WRITE_STALL_GRACE`], overridable through the
/// `WORKERD_HYPER_IO_STALL_GRACE_MS` environment variable (read once per process). The
/// override exists as a test hook — the stall tests would otherwise have to wait out the full
/// minute — but also serves as an operator escape hatch.
pub(crate) fn steady_stall_grace() -> Duration {
    static GRACE: std::sync::OnceLock<Duration> = std::sync::OnceLock::new();
    *GRACE.get_or_init(|| {
        std::env::var("WORKERD_HYPER_IO_STALL_GRACE_MS")
            .ok()
            .and_then(|value| value.parse::<u64>().ok())
            .map_or(WRITE_STALL_GRACE, Duration::from_millis)
    })
}

/// Shared between the [`StallIo`] adapter (updated from the connection task's I/O polls) and
/// the per-connection watchdog. `None` = no I/O outstanding on that side; `Some(t)` = a poll
/// first returned `Pending` at `t` and nothing has completed since.
pub(crate) struct WriteStallTracker {
    pending_since: Mutex<Option<Instant>>,
    read_state: Mutex<ReadState>,
    /// Client-only read policy (see [`client_stall_watchdog`]): while the connection is
    /// awaiting the first byte of a response — request bytes went out and nothing has been
    /// read since — pending reads do NOT arm the read-stall clock. kj's client waits for a
    /// response head indefinitely (a server may legitimately take arbitrarily long to produce
    /// it), so reaping that wait would fail requests kj serves fine; only *mid-response*
    /// silence (bytes seen, then nothing) marks a dead origin. The inbound server keeps the
    /// unconditional arming: its drain-mode read bound exists precisely to reap connections
    /// idle at a message boundary or mid-request.
    reads_exempt_awaiting_response: bool,
}

struct ReadState {
    pending_since: Option<Instant>,
    /// True from a completed socket write (request bytes accepted by the kernel) until the
    /// next completed socket read (first response bytes arrived). Only consulted when
    /// `reads_exempt_awaiting_response` is set.
    awaiting_response: bool,
}

impl WriteStallTracker {
    /// Tracker for the inbound server: pending reads always arm the read-stall clock (the
    /// drain watchdog is the only consumer of it there).
    pub(crate) fn new() -> Arc<Self> {
        Self::with_read_exemption(false)
    }

    /// Tracker for the outbound client: reads awaiting a response head are exempt (see the
    /// field docs on `reads_exempt_awaiting_response`).
    pub(crate) fn new_client() -> Arc<Self> {
        Self::with_read_exemption(true)
    }

    fn with_read_exemption(reads_exempt_awaiting_response: bool) -> Arc<Self> {
        Arc::new(Self {
            pending_since: Mutex::new(None),
            read_state: Mutex::new(ReadState {
                pending_since: None,
                awaiting_response: false,
            }),
            reads_exempt_awaiting_response,
        })
    }

    /// Record the outcome of a data-write poll: `Pending` starts the stall clock (if not
    /// already running), a completion resets it (bytes were accepted). On a client tracker a
    /// completion also moves the read side into the awaiting-response state: the bytes just
    /// written are (part of) a request, and until the response's first byte arrives a pending
    /// read means "waiting for the origin to respond", not a stalled transfer.
    fn note<T>(&self, poll: &std::task::Poll<T>) {
        let mut pending = self
            .pending_since
            .lock()
            .unwrap_or_else(PoisonError::into_inner);
        match poll {
            std::task::Poll::Pending => {
                if pending.is_none() {
                    *pending = Some(Instant::now());
                }
            }
            std::task::Poll::Ready(_) => {
                *pending = None;
                if self.reads_exempt_awaiting_response {
                    let mut read = self
                        .read_state
                        .lock()
                        .unwrap_or_else(PoisonError::into_inner);
                    read.awaiting_response = true;
                    read.pending_since = None;
                }
            }
        }
    }

    /// Record the outcome of a flush/shutdown poll. `Pending` arms the stall clock like a
    /// write (a flush that cannot complete is equally stuck, e.g. TLS records the socket will
    /// not take), but `Ready` does NOT clear it: hyper interleaves `poll_flush` after pending
    /// writes, and a plain TCP flush is always `Ready` without implying any bytes were
    /// accepted — clearing on it would blind the watchdog to a write-stalled connection.
    fn note_flush<T>(&self, poll: &std::task::Poll<T>) {
        if poll.is_pending() {
            let mut pending = self
                .pending_since
                .lock()
                .unwrap_or_else(PoisonError::into_inner);
            if pending.is_none() {
                *pending = Some(Instant::now());
            }
        }
    }

    /// Record the outcome of a read poll (consulted by the server only while draining; always
    /// consulted by the client). A completion clears the awaiting-response exemption: the
    /// response has started, so from here on read silence counts.
    fn note_read<T>(&self, poll: &std::task::Poll<T>) {
        let mut read = self
            .read_state
            .lock()
            .unwrap_or_else(PoisonError::into_inner);
        match poll {
            std::task::Poll::Pending => {
                if read.pending_since.is_none()
                    && !(self.reads_exempt_awaiting_response && read.awaiting_response)
                {
                    read.pending_since = Some(Instant::now());
                }
            }
            std::task::Poll::Ready(_) => {
                read.pending_since = None;
                read.awaiting_response = false;
            }
        }
    }

    fn stalled_for(&self) -> Option<Duration> {
        self.pending_since
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .map(|since| since.elapsed())
    }

    fn read_stalled_for(&self) -> Option<Duration> {
        self.read_state
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .pending_since
            .map(|since| since.elapsed())
    }
}

/// I/O adapter feeding the [`WriteStallTracker`]: records every read- and write-side poll
/// outcome, otherwise transparent. Sits under hyper's `TokioIo` (and, for TLS connections,
/// under hyper but *over* the TLS stream, so "write pending" includes TLS records the socket
/// will not take).
pub(crate) struct StallIo<S> {
    inner: S,
    tracker: Arc<WriteStallTracker>,
}

impl<S> StallIo<S> {
    pub(crate) fn new(inner: S, tracker: Arc<WriteStallTracker>) -> Self {
        Self { inner, tracker }
    }
}

impl<S: tokio::io::AsyncRead + Unpin> tokio::io::AsyncRead for StallIo<S> {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
        buf: &mut tokio::io::ReadBuf<'_>,
    ) -> std::task::Poll<std::io::Result<()>> {
        let this = &mut *self;
        let poll = Pin::new(&mut this.inner).poll_read(cx, buf);
        this.tracker.note_read(&poll);
        poll
    }
}

impl<S: tokio::io::AsyncWrite + Unpin> tokio::io::AsyncWrite for StallIo<S> {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
        buf: &[u8],
    ) -> std::task::Poll<std::io::Result<usize>> {
        let this = &mut *self;
        let poll = Pin::new(&mut this.inner).poll_write(cx, buf);
        this.tracker.note(&poll);
        poll
    }

    fn poll_write_vectored(
        mut self: Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
        bufs: &[std::io::IoSlice<'_>],
    ) -> std::task::Poll<std::io::Result<usize>> {
        let this = &mut *self;
        let poll = Pin::new(&mut this.inner).poll_write_vectored(cx, bufs);
        this.tracker.note(&poll);
        poll
    }

    fn is_write_vectored(&self) -> bool {
        self.inner.is_write_vectored()
    }

    fn poll_flush(
        mut self: Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
    ) -> std::task::Poll<std::io::Result<()>> {
        let this = &mut *self;
        let poll = Pin::new(&mut this.inner).poll_flush(cx);
        this.tracker.note_flush(&poll);
        poll
    }

    fn poll_shutdown(
        mut self: Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
    ) -> std::task::Poll<std::io::Result<()>> {
        let this = &mut *self;
        let poll = Pin::new(&mut this.inner).poll_shutdown(cx);
        this.tracker.note_flush(&poll);
        poll
    }
}

/// The inbound server's watchdog: resolves when the connection's write side has made no
/// progress for the applicable grace period — [`steady_stall_grace`] normally,
/// [`WRITE_STALL_DRAIN_GRACE`] once draining (when zero-progress *reads* count too; see the
/// constant docs and the server.rs module docs). The connection is then aborted.
pub(crate) async fn write_stall_watchdog(
    tracker: Arc<WriteStallTracker>,
    drain_rx: watch::Receiver<bool>,
) {
    loop {
        tokio::time::sleep(WRITE_STALL_CHECK_INTERVAL).await;
        let draining = *drain_rx.borrow();
        let grace = if draining {
            WRITE_STALL_DRAIN_GRACE
        } else {
            steady_stall_grace()
        };
        if tracker.stalled_for().is_some_and(|stall| stall >= grace) {
            return;
        }
        // Read stalls only count while draining: a request whose remaining body bytes never
        // arrive must not hold the drain forever, but outside of drain read-idleness is normal.
        if draining
            && tracker
                .read_stalled_for()
                .is_some_and(|stall| stall >= WRITE_STALL_DRAIN_GRACE)
        {
            return;
        }
    }
}

/// The outbound client's watchdog: resolves when *any* outstanding socket I/O — write, flush,
/// shutdown, or read — has made zero progress for [`steady_stall_grace`]. The connection
/// future is then dropped, closing the socket: in-flight requests observe a canceled/closed
/// hyper error and the `SendRequest` handle reads as closed, poisoning the connection out of
/// the keep-alive pool.
///
/// Unlike the server there is no drain mode (the client has no drain concept). Read stalls
/// count, with one carve-out (the awaiting-response exemption on the client's tracker, see
/// [`WriteStallTracker::new_client`]): a read outstanding while the origin has not yet sent
/// the first byte of a response never trips the watchdog. kj's client waits for a response
/// head indefinitely — a server may take arbitrarily long to produce one (e.g. kj's own
/// `HttpServer` error path building a 501 for an unimplemented CONNECT) — and reaping that wait
/// would fail requests kj serves fine. So a read stall here means either a silently-dead
/// origin *mid-response* (bytes seen, then nothing — the case this watchdog exists for) or an
/// idle pooled connection worth retiring anyway (kj's own `HttpClientSettings::idleTimeout`
/// retires those after 5 s). The one behavioral cost — a response *body* that sends nothing
/// at all for over a minute gets aborted where kj would wait forever — is documented as a
/// divergence in hyper-http.h. Upgraded (WebSocket/CONNECT) streams are exempt: the
/// connection future resolves at the upgrade handoff and takes this watchdog down with it
/// (see `finish_http1_handshake` in client.rs).
pub(crate) async fn client_stall_watchdog(tracker: Arc<WriteStallTracker>) {
    let grace = steady_stall_grace();
    loop {
        tokio::time::sleep(WRITE_STALL_CHECK_INTERVAL).await;
        if tracker.stalled_for().is_some_and(|stall| stall >= grace)
            || tracker
                .read_stalled_for()
                .is_some_and(|stall| stall >= grace)
        {
            return;
        }
    }
}
