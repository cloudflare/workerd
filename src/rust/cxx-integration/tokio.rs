use std::future::Future;
use std::sync::OnceLock;

use tokio::task::JoinHandle;
use tracing::{info, Instrument};

static TOKIO_RUNTIME: OnceLock<tokio::runtime::Runtime> = OnceLock::new();

/// Initialize tokio runtime.
/// Must be called after all forking and sandbox setup is finished.
pub(crate) fn init(worker_threads: Option<usize>) {
    assert!(TOKIO_RUNTIME.get().is_none());

    let mut builder = tokio::runtime::Builder::new_multi_thread();

    if let Some(worker_threads) = worker_threads {
        builder.worker_threads(worker_threads);
    }

    let runtime = builder.enable_time().enable_io().build().unwrap();

    TOKIO_RUNTIME.set(runtime).unwrap();
    spawn(async {
        info!(nosentry = true, "tokio runtime is online");
    });
}

/// Obtain a handle to the shared tokio runtime.
/// Requires calling [`init_tokio`] first.
/// # Panics
/// if tokio runtime is not available yet.
pub fn runtime_handle() -> tokio::runtime::Handle {
    TOKIO_RUNTIME.get().unwrap().handle().clone()
}

/// This is helper to set the spawn and stuff duplicating the signature of
/// `https://docs.rs/tokio/latest/tokio/task/fn.spawn.html`.
pub fn spawn<F>(future: F) -> JoinHandle<F::Output>
where
    F: Future + Send + 'static,
    F::Output: Send + 'static,
{
    let handle = runtime_handle();
    let _guard = handle.enter();

    tokio::spawn(future.in_current_span())
}

/// Exposes `Runtime::block_on`
/// `https://docs.rs/tokio/latest/tokio/runtime/struct.Runtime.html#method.block_on`.
pub fn block_on<F: Future>(f: F) -> F::Output {
    runtime_handle().block_on(f)
}

#[cfg(test)]
mod test {
    use std::time::Duration;

    use super::*;

    #[test]
    fn test_tokio_init() {
        init(None);
        let join = spawn(async move {
            tokio::time::sleep(Duration::from_millis(1)).await;
            42
        });
        let result = runtime_handle().block_on(join).unwrap();
        assert_eq!(42, result);
    }
}
