// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::time::Instant;

use super::*;

/// How long a test waits for something that must happen.
const SOON: Duration = Duration::from_secs(10);

/// Sets its flag when dropped.
struct DropGuard(Arc<AtomicBool>);

impl Drop for DropGuard {
    fn drop(&mut self) {
        self.0.store(true, std::sync::atomic::Ordering::SeqCst);
    }
}

#[test]
fn a_runtime_of_no_threads_still_runs_tasks() {
    let runtime = Runtime::new(0).unwrap();
    let answer = futures::executor::block_on(runtime.handle().spawn(async { 42 })).unwrap();
    assert_eq!(answer, 42);
    runtime.shutdown(SOON);
}

#[test]
fn a_task_runs_on_a_thread_of_the_runtime_named_send_worker() {
    let runtime = Runtime::new(2).unwrap();
    let caller = std::thread::current().id();
    let (id, name) = futures::executor::block_on(runtime.handle().spawn(async {
        let thread = std::thread::current();
        (thread.id(), thread.name().map(str::to_owned))
    }))
    .unwrap();
    assert_ne!(id, caller);
    assert_eq!(name.as_deref(), Some("send-worker"));
    runtime.shutdown(SOON);
}

#[test]
fn shutdown_drops_pending_tasks_and_cancels_tasks_spawned_afterwards() {
    let runtime = Runtime::new(2).unwrap();
    let handle = runtime.handle();
    let dropped = Arc::new(AtomicBool::new(false));
    let (started_tx, started_rx) = std::sync::mpsc::channel();
    let guard = DropGuard(Arc::clone(&dropped));
    let pending = handle.spawn(async move {
        let _guard = guard;
        started_tx.send(()).unwrap();
        std::future::pending::<()>().await;
    });
    started_rx.recv_timeout(SOON).unwrap();
    assert!(!dropped.load(std::sync::atomic::Ordering::SeqCst));

    runtime.shutdown(SOON);
    let deadline = Instant::now() + SOON;
    while !dropped.load(std::sync::atomic::Ordering::SeqCst) {
        assert!(
            Instant::now() < deadline,
            "the pending task was not dropped"
        );
        std::thread::yield_now();
    }
    assert!(
        futures::executor::block_on(pending)
            .unwrap_err()
            .is_cancelled()
    );

    let late = handle.spawn(async { 1 });
    assert!(
        futures::executor::block_on(late)
            .unwrap_err()
            .is_cancelled()
    );
}

// The only test that touches the process's runtime: once shut down, it cannot be started again.
#[test]
fn the_shared_runtime_is_built_once_and_ends_for_good_at_shutdown_shared() {
    let first = shared(1).unwrap();
    let second = shared(3).unwrap();
    assert_eq!(first.id(), second.id());
    assert_eq!(
        futures::executor::block_on(second.spawn(async { 7 })).unwrap(),
        7
    );

    shutdown_shared(SOON);
    let error = shared(1).unwrap_err();
    assert!(error.description().contains("shut down"), "{error:?}");
    assert!(
        futures::executor::block_on(first.spawn(async { 1 }))
            .unwrap_err()
            .is_cancelled()
    );

    shutdown_shared(SOON);
    assert!(shared(1).is_err());
}
