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
    {
        use crate::error::op;
        with_runtime(async move {
            let kind = tokio::signal::unix::SignalKind::from_raw(signum);
            // Create the stream here (inside the runtime context, at first poll of the bridged
            // future) so the process-global handler registration happens as early as possible.
            let mut sig = tokio::signal::unix::signal(kind).map_err(op("signal"))?;

            // Do NOT `sig.recv().await` directly from this (bridged) future. tokio's signal
            // registry is process-global: when several tokio runtimes exist in the process (one
            // per KJ event loop thread — e.g. workerd's main loop plus the inspector thread's
            // loop), the runtime whose driver consumes the signal's wake byte performs the
            // broadcast, so the stored waker can be woken FROM THAT OTHER THREAD. A directly
            // parked waker here would be the bridged future's loop-thread-only, non-atomic
            // `kj_rs` `FutureWakerCell`: waking it cross-thread is UB under the bridge's
            // single-thread waker axiom, and in practice loses the wakeup (observed as workerd
            // ignoring SIGTERM whenever the inspector thread's runtime won the race). So, like
            // `net.rs::resolve_host` (the model citizen for this pattern), a tokio *runtime*
            // task owns the `recv()`: the cross-thread broadcast terminates at tokio's own
            // `Send + Sync` scheduler waker (which unparks this loop), the task then runs on the
            // loop thread and hands the result back over a oneshot, waking the bridged future
            // same-thread.
            let (tx, rx) = tokio::sync::oneshot::channel::<Result<()>>();
            let task = tokio::spawn(async move {
                let result = sig
                    .recv()
                    .await
                    .ok_or_else(|| KjIoError::other("signal", "signal stream closed unexpectedly"));
                let _ = tx.send(result);
            });
            // If this future is dropped (KJ promise cancelled), abort the watcher task so its
            // signal-stream registration is torn down instead of lingering for the process
            // lifetime.
            let _abort_guard = crate::runtime::AbortOnDrop(task);
            match rx.await {
                Ok(result) => result,
                Err(_) => Err(KjIoError::other("signal", "signal watcher task dropped")),
            }
        })
        .await
    }

    // Validated by Windows CI.
    //
    // Same forwarding-task pattern as the unix arm, for the Windows flavor of the same hazard:
    // tokio's `SetConsoleCtrlHandler` handler runs on an OS-spawned console-ctrl thread and
    // broadcasts to every registered watcher's stored waker FROM THAT THREAD. Awaiting the
    // stream directly here would park a clone of the bridged future's waker -- a
    // loop-thread-only, non-atomic `kj_rs` `FutureWakerCell` -- in tokio's signal registry, and
    // one Ctrl-C/shutdown event would wake it cross-thread: UB under the bridge's single-thread
    // waker axiom. The tokio *runtime* task owning the `recv()` terminates the cross-thread
    // broadcast at tokio's own `Send + Sync` scheduler waker (which unparks this loop); the task
    // then runs on the loop thread and hands the result back over a oneshot, waking the bridged
    // future same-thread.
    #[cfg(windows)]
    {
        use crate::error::op;
        // `<csignal>` values as the C++ callers pass them (MSVC defines SIGINT=2, SIGTERM=15).
        // workerd's only caller passes SIGTERM (graceful drain; server/cli-io-backend.c++);
        // SIGINT is mapped for completeness.
        const SIGINT: i32 = 2;
        const SIGTERM: i32 = 15;
        with_runtime(async move {
            if signum != SIGTERM && signum != SIGINT {
                return Err(KjIoError::other(
                    "signal",
                    "kj-rs-io only watches SIGTERM/SIGINT on Windows",
                ));
            }
            let (tx, rx) = tokio::sync::oneshot::channel::<Result<()>>();
            let task = tokio::spawn(async move {
                let result = async {
                    // SIGTERM -> ctrl_shutdown, SIGINT -> ctrl_c (the conventional mappings).
                    let received = match signum {
                        SIGTERM => {
                            let mut sig =
                                tokio::signal::windows::ctrl_shutdown().map_err(op("signal"))?;
                            sig.recv().await
                        }
                        // SIGINT; anything else already errored before the spawn.
                        _ => {
                            let mut sig = tokio::signal::windows::ctrl_c().map_err(op("signal"))?;
                            sig.recv().await
                        }
                    };
                    received.ok_or_else(|| {
                        KjIoError::other("signal", "signal stream closed unexpectedly")
                    })
                }
                .await;
                let _ = tx.send(result);
            });
            // If this future is dropped (KJ promise cancelled), abort the watcher task so its
            // signal-stream registration is torn down instead of lingering for the process
            // lifetime.
            let _abort_guard = crate::runtime::AbortOnDrop(task);
            match rx.await {
                Ok(result) => result,
                Err(_) => Err(KjIoError::other("signal", "signal watcher task dropped")),
            }
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
