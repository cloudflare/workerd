//! tokio-backed fd readiness watching.
//!
//! This backs `kj_rs_io::FileWatcher` (file-watcher.h), the tokio-loop replacement for
//! workerd's `--watch` file watcher. The C++ side owns the platform notification fd (inotify on
//! Linux, kqueue on macOS/BSD) and does all the event parsing; the Rust side only supplies
//! "resolve when this fd becomes readable", replacing
//! `kj::UnixEventPort::FdObserver::whenBecomesReadable()`.
//!
//! Semantics:
//!
//! - The fd is registered with the tokio I/O driver per call (edge-triggered underneath, but
//!   both epoll and kqueue report readiness that already exists at registration time, so events
//!   queued on the fd before the call are not missed).
//! - Dropping the future deregisters the fd without consuming anything.
//! - The caller must keep the fd open until the future resolves or is dropped, and should not
//!   have the same fd registered through this function twice concurrently (tokio's I/O driver
//!   does not support duplicate registrations of one fd).

use crate::error::Result;
use crate::runtime::with_runtime;

/// Resolves when `fd` becomes readable. Unix only; errors immediately on other platforms.
pub async fn wait_fd_readable(fd: i32) -> Result<()> {
    #[cfg(unix)]
    {
        use tokio::io::Interest;
        use tokio::io::unix::AsyncFd;

        use crate::error::op;
        with_runtime(async move {
            let afd = AsyncFd::with_interest(fd, Interest::READABLE).map_err(op("AsyncFd"))?;
            // The guard's readiness state is intentionally not cleared: the AsyncFd is
            // deregistered immediately below (drop), and the next call re-registers, at which
            // point still-pending readiness is reported again.
            let _guard = afd.readable().await.map_err(op("readable"))?;
            Ok(())
        })
        .await
    }
    #[cfg(not(unix))]
    {
        use crate::error::KjIoError;
        let _ = fd;
        Err(KjIoError::other(
            "wait_fd_readable",
            "kj-rs-io fd readiness watching is only implemented on Unix",
        ))
    }
}
