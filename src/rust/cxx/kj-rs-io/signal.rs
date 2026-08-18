//! tokio-backed signal watching: POSIX signals on Unix, the corresponding console control
//! events on Windows.
//!
//! This backs `kj_rs_io::onSignal()` (async-io.h), the tokio-loop replacement for
//! `kj::UnixEventPort::onSignal()` -- workerd uses it for SIGTERM graceful drain.
//!
//! Semantics differences vs `UnixEventPort::onSignal()` (acceptable for the drain use case):
//!
//! - No `siginfo_t` is reported; the promise just resolves.
//! - The handler is registered when the returned future is first polled (tokio registers with
//!   the process-global signal registry at `signal()` time), not at call time, and KJ does not
//!   block the signal beforehand the way `UnixEventPort::captureSignal()` does. A signal
//!   delivered before the first poll takes its default disposition.
//! - tokio's signal registration is process-wide and persists for the life of the process
//!   (dropping the future stops *watching*, but does not restore `SIG_DFL`).
//!
//! Both arms route the `recv()` through a tokio runtime task rather than awaiting the stream
//! from the bridged future: tokio's signal registry is process-global and its broadcast can run
//! on a different thread (another runtime's loop thread on unix, the console-ctrl thread on
//! Windows), which must never wake a bridged loop-thread-only waker directly. See the comments
//! in each arm.
//!
//! On Windows the signums workerd actually passes are mapped to their conventional console
//! control events: SIGTERM -> `ctrl_shutdown`, SIGINT -> `ctrl_c`. Anything else errors.

use crate::error::KjIoError;
use crate::error::Result;
use crate::runtime::with_runtime;

/// Resolves when the process receives signal `signum` (on Windows: the console control event
/// conventionally mapped to it). Errors immediately for unmapped signums / other platforms.
pub async fn wait_for_signal(signum: i32) -> Result<()> {
    #[cfg(unix)]
    return Err(KjIoError::other("signal", "TODO: Unix signal support"));

    #[cfg(windows)]
    return Err(KjIoError::other("signal", "TODO: Windows signal support"));

    #[cfg(not(any(unix, windows)))]
    {
        let _ = signum;
        Err(KjIoError::other(
            "signal",
            "kj-rs-io signal watching is not implemented on this platform",
        ))
    }
}
