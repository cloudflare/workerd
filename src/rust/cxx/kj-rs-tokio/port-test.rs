use super::*;

thread_local! {
    static LATE_DROP_PORT: RefCell<Option<TokioPort>> = const { RefCell::new(None) };
}

#[test]
fn port_can_outlive_tokio_thread_locals() {
    std::thread::spawn(|| {
        // Initialize the holder before Tokio's context TLS. Thread-local values are dropped
        // in reverse initialization order, so the stored port is destroyed after Tokio's
        // context has already been torn down.
        LATE_DROP_PORT.with(|holder| {
            assert!(holder.borrow().is_none());
        });
        let port = TokioPort::new();
        LATE_DROP_PORT.with(|holder| {
            *holder.borrow_mut() = Some(port);
        });
    })
    .join()
    .expect("late TokioPort destruction must not panic");
}

#[test]
fn port_drop_does_not_wait_for_blocking_tasks() {
    let port = TokioPort::new();
    let release = Arc::new(AtomicBool::new(false));
    let task_release = Arc::clone(&release);
    let (started_tx, started_rx) = std::sync::mpsc::channel();
    let (done_tx, done_rx) = std::sync::mpsc::channel();
    port.handle().spawn_blocking(move || {
        started_tx.send(()).unwrap();
        while !task_release.load(Ordering::SeqCst) {
            std::thread::sleep(Duration::from_millis(1));
        }
        done_tx.send(()).unwrap();
    });
    started_rx.recv().unwrap();

    let watchdog_release = Arc::clone(&release);
    let watchdog = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(250));
        watchdog_release.store(true, Ordering::SeqCst);
    });
    let start = std::time::Instant::now();
    drop(port);
    let elapsed = start.elapsed();
    release.store(true, Ordering::SeqCst);
    done_rx.recv_timeout(Duration::from_secs(1)).unwrap();
    watchdog.join().unwrap();

    assert!(
        elapsed < Duration::from_millis(100),
        "port teardown waited for a blocking task: {elapsed:?}"
    );
}

#[test]
fn foreign_cancellation_does_not_cancel_the_callers_loop() {
    let owner_port = TokioPort::new();
    std::thread::scope(|scope| {
        scope
            .spawn(|| {
                let caller_port = TokioPort::new();
                let ran = Arc::new(AtomicBool::new(false));
                let task_ran = Arc::clone(&ran);
                let _task = spawn(async move {
                    task_ran.store(true, Ordering::SeqCst);
                });

                owner_port.cancel_spawned_tasks();
                assert!(!caller_port.poll());
                assert!(ran.load(Ordering::SeqCst));
            })
            .join()
            .unwrap();
    });
}

#[test]
fn bare_port_drop_cancels_spawned_tasks() {
    // A TokioPort NOT owned by a TokioEventPort: dropping it must still cancel tasks
    // spawned onto its LocalSet (the Drop fallback), and leave the thread clean so a fresh
    // port can be created afterward.
    {
        let port = TokioPort::new();
        drop(spawn(std::future::pending::<()>()));
        assert!(current_handle().is_some());
        drop(port);
    }
    // TLS is cleared: a new port on this thread succeeds (would panic-on-double-register
    // otherwise).
    assert!(current_handle().is_none());
    let port2 = TokioPort::new();
    assert!(current_handle().is_some());
    drop(port2);
}

#[test]
fn concurrent_wake_storm_from_many_threads_terminates() {
    // TSAN stressor for SharedState (notify + woken latch): four threads hammer wake()
    // concurrently while the loop thread services wait_timeout_ns. The assertion is
    // deliberately interleaving-agnostic -- it only requires termination and that at least
    // one wake was observed -- because exact latch counts are inherently racy. (Scoped
    // threads borrowing `&TokioPort`: the port is `Sync` but, by design, not `Send`.)
    use std::sync::atomic::AtomicUsize;
    let port = TokioPort::new();
    let done = AtomicUsize::new(0);
    std::thread::scope(|scope| {
        for _ in 0..4 {
            scope.spawn(|| {
                for _ in 0..250 {
                    port.wake();
                }
                done.fetch_add(1, Ordering::SeqCst);
            });
        }
        let mut latched = 0u64;
        loop {
            if port.wait_timeout_ns(1_000_000) {
                latched += 1;
            }
            // Once every waker has finished, one more wait must eventually drain to `false`.
            if done.load(Ordering::SeqCst) == 4 && !port.wait_timeout_ns(1_000_000) {
                break;
            }
        }
        assert!(latched >= 1);
    });
}

#[test]
fn handle_spawns_a_runtime_task_from_another_thread() {
    // `handle()` + `tokio::spawn` (the runtime-driven path, distinct from spawn()/LocalSet)
    // from a foreign thread: the Send task runs when the loop next drives block_on.
    let port = TokioPort::new();
    let flag = Arc::new(AtomicBool::new(false));
    let handle = port.handle();
    let f2 = Arc::clone(&flag);
    std::thread::spawn(move || {
        handle.spawn(async move {
            f2.store(true, Ordering::SeqCst);
        });
    })
    .join()
    .unwrap();
    let mut ran = false;
    for _ in 0..1000 {
        let _ = port.wait_timeout_ns(1_000_000);
        if flag.load(Ordering::SeqCst) {
            ran = true;
            break;
        }
    }
    assert!(ran);
}

#[test]
fn poll_advances_a_ready_local_task() {
    // Complements poll_never_sleeps (pending task): a ready LocalSet task (no await) is
    // actually driven to completion by poll() within its budget, and poll() never latches.
    let port = TokioPort::new();
    let flag = Arc::new(AtomicBool::new(false));
    let f2 = Arc::clone(&flag);
    let _jh = spawn(async move {
        f2.store(true, Ordering::SeqCst);
    });
    let mut ran = false;
    for _ in 0..10 {
        assert!(!port.poll(), "poll() must not report a wake latch here");
        if flag.load(Ordering::SeqCst) {
            ran = true;
            break;
        }
    }
    assert!(ran);
}

#[test]
fn wake_latch_semantics() {
    let port = TokioPort::new();
    // No wake: a timed-out wait reports false.
    assert!(!port.wait_timeout_ns(1_000_000));
    // Wake before wait: latch is reported exactly once.
    port.wake();
    assert!(port.wait_timeout_ns(1_000_000));
    assert!(!port.wait_timeout_ns(1_000_000));
    // Wake is also consumed by poll().
    port.wake();
    assert!(port.poll());
    assert!(!port.poll());
}

#[test]
fn wake_from_other_thread_unblocks_wait_forever() {
    let port = TokioPort::new();
    std::thread::scope(|scope| {
        scope.spawn(|| {
            std::thread::sleep(Duration::from_millis(10));
            port.wake();
        });
        assert!(port.wait_forever());
    });
}

/// The loop thread is inside the runtime's context for the port's whole life, so tokio
/// resources can be created outside `block_on` without any explicit `enter()`.
#[test]
fn loop_thread_is_permanently_in_the_runtime_context() {
    assert!(tokio::runtime::Handle::try_current().is_err());
    {
        let port = TokioPort::new();
        let current = tokio::runtime::Handle::try_current().expect("entered");
        assert_eq!(current.id(), port.handle().id());
        // A resource needing the I/O driver, created from plain sync code on this thread.
        let std_listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        std_listener.set_nonblocking(true).unwrap();
        let _listener = tokio::net::TcpListener::from_std(std_listener).unwrap();
    }
    // Dropping the port leaves the context.
    assert!(tokio::runtime::Handle::try_current().is_err());
}

#[test]
fn spawned_tasks_run_during_wait() {
    let port = TokioPort::new();
    let (tx, mut rx) = tokio::sync::oneshot::channel::<u32>();
    let mut jh = spawn(async move {
        tokio::time::sleep(Duration::from_millis(5)).await;
        tx.send(42).unwrap();
    });
    // The task only runs inside wait_impl's block_on.
    let mut done = false;
    for _ in 0..100 {
        let _ = port.wait_timeout_ns(20_000_000);
        if let Ok(v) = rx.try_recv() {
            assert_eq!(v, 42);
            done = true;
            break;
        }
    }
    assert!(done);
    // The JoinHandle should complete promptly now.
    port.runtime.as_ref().unwrap().block_on(&mut jh).unwrap();
}

#[test]
fn poll_never_sleeps() {
    let port = TokioPort::new();
    // A pending spawned task must not make poll() block.
    let _jh = spawn(std::future::pending::<()>());
    let start = std::time::Instant::now();
    assert!(!port.poll());
    assert!(start.elapsed() < Duration::from_millis(100));
}

/// `spawn` accepts `!Send` futures because it is backed by `LocalSet::spawn_local`. This
/// future holds an `Rc` — which is `!Send` — across an await point, exercising that path,
/// and proves such a task actually runs to completion on the loop thread.
#[test]
fn spawn_accepts_non_send_futures() {
    use std::cell::Cell;
    let port = TokioPort::new();
    let counter = Rc::new(Cell::new(0u32));
    let task_counter = Rc::clone(&counter);
    // Detached on purpose; the `Rc` capture makes the future `!Send`.
    let _jh = spawn(async move {
        for _ in 0..3 {
            tokio::task::yield_now().await;
        }
        task_counter.set(task_counter.get() + 1);
    });
    let mut done = false;
    for _ in 0..100 {
        let _ = port.wait_timeout_ns(1_000_000);
        if counter.get() == 1 {
            done = true;
            break;
        }
    }
    assert!(done, "non-Send spawned task did not run to completion");
}

/// Timed waits stay on tokio's timer wheel and remain accurate to its ~1 ms granularity --
/// the same granularity KJ's own epoll-based port has (`epoll_pwait` takes a millisecond
/// timeout).
#[test]
fn timeouts_are_accurate_to_the_wheel() {
    let port = TokioPort::new();
    let start = std::time::Instant::now();
    let _ = port.wait_timeout_ns(20_000_000);
    let elapsed = start.elapsed();
    assert!(
        elapsed >= Duration::from_millis(19),
        "woke early: {elapsed:?}"
    );
    assert!(
        elapsed < Duration::from_millis(500),
        "woke far too late: {elapsed:?}"
    );
}
