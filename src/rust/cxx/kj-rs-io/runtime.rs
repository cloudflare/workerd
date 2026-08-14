//! Runtime-context plumbing: tokio I/O objects must be created (registered with the I/O driver)
//! from within a tokio runtime context, but kj-rs bridge futures are polled by the KJ event
//! loop, outside any `block_on`. These helpers enter this thread's `kj_rs_tokio` runtime context
//! around each poll / each synchronous operation.

use std::future::Future;

use tokio::runtime::Handle;

use crate::error::KjIoError;
use crate::error::Result;

/// Returns a handle to this thread's KJ-loop tokio runtime, or a `kj::Exception`-convertible
/// error if there is no `TokioEventPort` on this thread.
pub fn runtime_handle() -> Result<Handle> {
    kj_rs_tokio::current_handle().ok_or_else(|| {
        KjIoError::other(
            "kj_rs_io",
            "no kj-rs-tokio runtime on this thread; kj-rs-io requires a TokioEventPort \
             (see kj_rs_io::setupTokioAsyncIo())",
        )
    })
}

/// Aborts the wrapped tokio task when dropped. Used by the "runtime task forwards a result over
/// a oneshot" pattern (see `net.rs::resolve_host`): if the awaiting bridged future is dropped
/// (KJ promise cancelled), the forwarding task is aborted instead of lingering until its
/// underlying operation completes on its own.
pub struct AbortOnDrop(pub tokio::task::JoinHandle<()>);

impl Drop for AbortOnDrop {
    fn drop(&mut self) {
        self.0.abort();
    }
}

/// Runs `fut` with the current thread's KJ-loop runtime context entered around every poll, so
/// tokio resources created inside it can register with the runtime's I/O driver and timers.
pub async fn with_runtime<T>(fut: impl Future<Output = Result<T>>) -> Result<T> {
    let handle = runtime_handle()?;
    // Pin the future on the stack, then poll it through `poll_fn` with the runtime guard held
    // across each poll. This needs no manual pin-projection (`std::pin::pin!` gives a safe
    // `Pin<&mut _>`), so the whole helper is safe.
    let mut fut = std::pin::pin!(fut);
    std::future::poll_fn(|cx| {
        let _guard = handle.enter();
        fut.as_mut().poll(cx)
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn runtime_handle_errors_without_a_port() {
        // A thread with no TokioEventPort has no loop runtime; runtime_handle() must return a
        // kj::Exception-convertible error naming the precondition, not panic.
        let err = cxx::KjError::from(runtime_handle().unwrap_err());
        assert!(
            err.description()
                .contains("no kj-rs-tokio runtime on this thread"),
            "{}",
            err.description()
        );
    }
}
