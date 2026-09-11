//! tokio-backed fd readiness watching.
//!
//! This backs `kj_rs_io::FileWatcher` (file-watcher.h), the tokio-loop replacement for
//! workerd's `--watch` file watcher. The C++ side owns the platform notification fd (inotify on
//! Linux, kqueue on macOS/BSD) and does all the event parsing; the Rust side only supplies
//! "resolve when this fd becomes readable", replacing
//! `kj::UnixEventPort::FdObserver::whenBecomesReadable()`.
//!
//! Ownership: a [`TokioFdWatcher`] owns its own `dup(2)` of the notification fd and ONE
//! registration of it with the tokio I/O driver, created when the C++ `FileWatcher::Impl` is
//! constructed and released when it is destroyed. Nothing about the original fd is borrowed past
//! the constructor call, and C++ never has to keep an fd open for a pending promise or avoid
//! registering it twice -- the two contracts the previous borrowed-raw-fd design left to
//! comments.
//!
//! Semantics of [`TokioFdWatcher::readable`]:
//!
//! - Readiness that already exists when it is called is reported immediately (both epoll and
//!   kqueue report existing readiness at registration; tokio remembers it thereafter).
//! - The wake consumes tokio's cached readiness (`clear_ready`) BEFORE resolving, so after C++
//!   drains the fd (via the original) the next call sleeps until a genuinely new event. An
//!   event that lands between the clear and the drain is drained, and costs at most one
//!   spurious wake later (C++'s read then sees EAGAIN and calls again) -- never a lost wake
//!   and never a busy loop.
//! - Dropping a pending future just removes its waker; the registration stays.

use crate::error::Result;

/// See the module docs. Unix only; on other platforms construction fails.
pub struct TokioFdWatcher {
    #[cfg(unix)]
    afd: tokio::io::unix::AsyncFd<std::os::fd::OwnedFd>,
}

/// Creates a watcher for `fd`, which is borrowed only for the duration of this call.
///
/// # Errors
///
/// Fails if the fd cannot be duplicated or registered with the loop runtime's I/O driver, or on
/// non-Unix platforms.
pub fn new_fd_watcher(fd: i32) -> Result<Box<TokioFdWatcher>> {
    #[cfg(unix)]
    {
        use tokio::io::Interest;
        use tokio::io::unix::AsyncFd;

        use crate::error::op;
        // The caller (C++ FileWatcher::Impl) owns `fd` and it is open for this call; we keep
        // only the dup (see `dup_raw_fd`).
        let owned = crate::ffi::dup_raw_fd(fd)?;
        crate::runtime::require_loop_runtime()?;
        let afd = AsyncFd::with_interest(owned, Interest::READABLE).map_err(op("AsyncFd"))?;
        Ok(Box::new(TokioFdWatcher { afd }))
    }
    #[cfg(not(unix))]
    {
        use crate::error::KjIoError;
        let _ = fd;
        Err(KjIoError::other(
            "new_fd_watcher",
            "kj-rs-io fd readiness watching is only implemented on Unix",
        ))
    }
}

impl TokioFdWatcher {
    /// Resolves when the watched fd becomes readable. See the module docs for the exact
    /// semantics.
    ///
    /// # Errors
    ///
    /// Fails if the I/O driver reports an error for the fd, or on non-Unix platforms.
    pub async fn readable(&self) -> Result<()> {
        #[cfg(unix)]
        {
            use crate::error::op;
            use crate::runtime::on_loop_runtime;
            on_loop_runtime(async {
                let mut guard = self.afd.readable().await.map_err(op("readable"))?;
                guard.clear_ready();
                Ok(())
            })
            .await
        }
        #[cfg(not(unix))]
        {
            use crate::error::KjIoError;
            Err(KjIoError::other(
                "readable",
                "kj-rs-io fd readiness watching is only implemented on Unix",
            ))
        }
    }
}
