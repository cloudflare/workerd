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
//! Cross-thread delivery note: tokio's signal registry is process-global, and its broadcast can
//! run on a different thread — another runtime's loop thread on unix (whichever runtime's driver
//! consumes the signal's wake byte, e.g. workerd's inspector thread), or the OS-spawned
//! console-ctrl thread on Windows. That is fine: the kj-rs waker bridge is thread-safe (a
//! cross-thread wake is delivered through the `FutureWakerCell`'s cross-thread fulfiller; see
//! kj-rs/waker.h), so the streams are awaited directly from the bridged future here. The
//! multi-runtime case is covered by the "onSignal is delivered even when another runtime's
//! thread consumes the signal" test in tests/async-io-test.c++ — the scenario that, before the
//! bridge was thread-safe, made workerd ignore SIGTERM whenever the inspector thread's runtime
//! won the race.
//!
//! On Windows the signums workerd actually passes are mapped to their conventional console
//! control events: SIGTERM -> `ctrl_shutdown`, SIGINT -> `ctrl_c`. Anything else errors.

use crate::error::KjIoError;
use crate::error::Result;
use crate::runtime::on_loop_runtime;

/// Resolves when the process receives signal `signum` (on Windows: the console control event
/// conventionally mapped to it). Errors immediately for unmapped signums / other platforms.
pub async fn wait_for_signal(signum: i32) -> Result<()> {
    #[cfg(unix)]
    {
        use crate::error::op;
        on_loop_runtime(async move {
            let kind = tokio::signal::unix::SignalKind::from_raw(signum);
            let mut sig = tokio::signal::unix::signal(kind).map_err(op("signal"))?;
            sig.recv()
                .await
                .ok_or_else(|| KjIoError::other("signal", "signal stream closed unexpectedly"))?;
            Ok(())
        })
        .await
    }
    // Validated by Windows CI. tokio's `SetConsoleCtrlHandler` handler broadcasts from an
    // OS-spawned console-ctrl thread; the thread-safe waker bridge absorbs that (see the module
    // doc), so this arm awaits directly too.
    #[cfg(windows)]
    {
        use crate::error::op;
        // `<csignal>` values as the C++ callers pass them (MSVC defines SIGINT=2, SIGTERM=15).
        // workerd's only caller passes SIGTERM (graceful drain; server/cli-io-backend.c++);
        // SIGINT is mapped for completeness.
        const SIGINT: i32 = 2;
        const SIGTERM: i32 = 15;
        on_loop_runtime(async move {
            // SIGTERM -> ctrl_shutdown, SIGINT -> ctrl_c (the conventional mappings).
            let received = match signum {
                SIGTERM => {
                    let mut sig = tokio::signal::windows::ctrl_shutdown().map_err(op("signal"))?;
                    sig.recv().await
                }
                SIGINT => {
                    let mut sig = tokio::signal::windows::ctrl_c().map_err(op("signal"))?;
                    sig.recv().await
                }
                _ => {
                    return Err(KjIoError::other(
                        "signal",
                        "kj-rs-io only watches SIGTERM/SIGINT on Windows",
                    ));
                }
            };
            received
                .ok_or_else(|| KjIoError::other("signal", "signal stream closed unexpectedly"))?;
            Ok(())
        })
        .await
    }
    #[cfg(not(any(unix, windows)))]
    {
        let _ = signum;
        Err(KjIoError::other(
            "signal",
            "kj-rs-io signal watching is not implemented on this platform",
        ))
    }
}
