//! Loop-runtime precondition: every kj-rs-io operation runs on a thread owning a
//! `kj_rs_tokio::TokioEventPort`, whose tokio runtime context that thread is permanently inside
//! (see `kj_rs_tokio`'s `EnteredRuntime`). tokio resources therefore register with the loop's
//! I/O driver and timers without any per-call `Handle::enter()`; the only thing left to do here
//! is turn "no port on this thread" into a `kj::Exception` with a useful message instead of the
//! panic tokio would raise.

use std::future::Future;

use crate::error::KjIoError;
use crate::error::Result;

/// Errors (`kj::Exception`-convertible) unless this thread owns a `TokioEventPort`.
pub fn require_loop_runtime() -> Result<()> {
    if kj_rs_tokio::current_handle().is_some() {
        Ok(())
    } else {
        Err(KjIoError::other(
            "kj_rs_io",
            "no kj-rs-tokio runtime on this thread; kj-rs-io requires a TokioEventPort \
             (see kj_rs_io::setupTokioAsyncIo())",
        ))
    }
}

/// Aborts the wrapped tokio task when dropped. Used by the "task forwards a result over a
/// oneshot" pattern (see `net.rs::resolve_host`): if the awaiting bridged future is dropped (KJ
/// promise cancelled), the forwarding task is aborted instead of lingering until its underlying
/// operation completes on its own.
pub struct AbortOnDrop(pub tokio::task::JoinHandle<()>);

impl Drop for AbortOnDrop {
    fn drop(&mut self) {
        self.0.abort();
    }
}

/// Runs `fut` after checking the loop-runtime precondition (the check happens once, when the
/// future is first polled -- not per poll).
pub async fn on_loop_runtime<T>(fut: impl Future<Output = Result<T>>) -> Result<T> {
    require_loop_runtime()?;
    fut.await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn require_loop_runtime_errors_without_a_port() {
        // A thread with no TokioEventPort has no loop runtime; the precondition must be a
        // kj::Exception-convertible error naming it, not a tokio panic.
        let err = cxx::KjError::from(require_loop_runtime().unwrap_err());
        assert!(
            err.description()
                .contains("no kj-rs-tokio runtime on this thread"),
            "{}",
            err.description()
        );
    }
}
