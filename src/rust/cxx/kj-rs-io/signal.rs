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
//!   the process-global signal registry at `signal()` time). The C++ adapter starts the promise
//!   eagerly, so from a caller's point of view the handler is installed by the time `onSignal()`
//!   returns; KJ does not block the signal beforehand the way `UnixEventPort::captureSignal()`
//!   does, so a signal delivered before the *call* takes its default disposition.
//! - tokio's signal registration is process-wide and persists for the life of the process
//!   (dropping the future stops *watching*, but does not restore `SIG_DFL`).
//!
//! Tokio's signal registry is process-global. Its broadcast may run on another runtime's
//! thread on Unix (any other tokio runtime in the process with the signal driver enabled), or
//! on the OS-created console-control thread on Windows. The bridged future's cloned waker owns
//! a kj-rs `ArcWaker`, whose cross-thread fulfiller schedules the next poll on the owning KJ
//! loop. Signal streams can therefore be awaited directly here. The test "onSignal is delivered
//! even when another runtime's thread consumes the signal" in tests/async-io-test.c++ exercises
//! delivery with another runtime parked on a different thread.
//!
//! On Windows the signums workerd actually passes are mapped to console control events:
//! SIGINT -> `ctrl_c` (the interactive interrupt, exactly SIGINT's role) and SIGTERM ->
//! `ctrl_break`. The latter is a conscious choice among the candidates: `CTRL_BREAK_EVENT` is
//! the event a parent process can send programmatically (`GenerateConsoleCtrlEvent`) to ask a
//! console child to wind down, which is what `kill -TERM` is on Unix; `CTRL_CLOSE_EVENT` gives
//! the handler ~5 s before the process is killed regardless, too short for a drain; and
//! `CTRL_SHUTDOWN_EVENT` is delivered to services and at system shutdown, not to a console
//! process someone wants to stop. Anything else errors.

use crate::ensure_loop_thread;
use crate::error::KjIoError;
use crate::error::Result;
use crate::error::op;

/// Resolves when the process receives signal `signum` (on Windows: the console control event
/// conventionally mapped to it). Errors immediately for unmapped signums.
pub async fn wait_for_signal(signum: i32) -> Result<()> {
    // Creating the signal stream registers with the entered runtime's signal driver (lib.rs,
    // "The tokio runtime").
    ensure_loop_thread()?;
    #[cfg(unix)]
    {
        let kind = tokio::signal::unix::SignalKind::from_raw(signum);
        let mut sig = tokio::signal::unix::signal(kind).map_err(op("signal"))?;
        sig.recv()
            .await
            .ok_or_else(|| KjIoError::other("signal", "signal stream closed unexpectedly"))?;
        Ok(())
    }
    #[cfg(windows)]
    {
        // `<csignal>` values as the C++ callers pass them (MSVC defines SIGINT=2, SIGTERM=15).
        // workerd's only caller passes SIGTERM (graceful drain; server/cli-main.c++);
        // SIGINT is mapped for completeness. See the module docs for why SIGTERM is
        // CTRL_BREAK_EVENT.
        const SIGINT: i32 = 2;
        const SIGTERM: i32 = 15;
        // tokio's `SetConsoleCtrlHandler` handler broadcasts from an OS-spawned console-ctrl
        // thread; the thread-safe waker bridge absorbs that (see the module doc).
        let received = match signum {
            SIGTERM => {
                let mut sig = tokio::signal::windows::ctrl_break().map_err(op("signal"))?;
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
        received.ok_or_else(|| KjIoError::other("signal", "signal stream closed unexpectedly"))?;
        Ok(())
    }
}
