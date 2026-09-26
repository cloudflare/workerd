//! The tokio-driven suite: what tokio-event-port-test.c++ proves for the KJ-driven port, proved
//! for `kj_rs_tokio::Runtime` driving the loop. Each test builds a Runtime, installs its context
//! for the C++ helpers (test-helpers.h), and blocks on a future that mixes bridged KJ promises,
//! spawned tokio tasks, and cross-thread callers.

use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::time::Duration;
use std::time::Instant;

use kj_rs_tokio::Runtime;

use crate::ffi;
use crate::test_helpers;

/// A Runtime with the helpers pointed at its context; cleared on drop, before the Runtime goes.
struct TestRuntime(Runtime);

impl TestRuntime {
    fn new() -> Self {
        let mut runtime = Runtime::new().expect("Runtime::new");
        ffi::install_test_context(runtime.context());
        Self(runtime)
    }
}

impl Drop for TestRuntime {
    fn drop(&mut self) {
        ffi::clear_test_context();
    }
}

fn block_on<F: std::future::Future>(runtime: &mut TestRuntime, future: F) -> F::Output {
    runtime.0.block_on(future).expect("block_on")
}

#[test]
fn kj_timer_fires_with_real_elapsed_time() {
    let mut rt = TestRuntime::new();
    let start = Instant::now();
    block_on(&mut rt, async {
        ffi::kj_timer_delay(30).await.expect("timer");
    });
    assert!(start.elapsed() >= Duration::from_millis(30));
    assert!(start.elapsed() < Duration::from_secs(5));
}

#[test]
fn absolute_timer_deadlines_stay_accurate_after_time_outside_the_loop() {
    // The KJ timer's now() is advanced only by the driver; time the thread spends elsewhere
    // (here, a blocking sleep between block_on calls) must not delay a deadline set afterwards.
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        ffi::kj_timer_delay(1).await.expect("timer");
    });
    std::thread::sleep(Duration::from_millis(40));
    let start = Instant::now();
    block_on(&mut rt, async {
        ffi::kj_timer_delay(20).await.expect("timer");
    });
    let elapsed = start.elapsed();
    assert!(elapsed >= Duration::from_millis(20), "{elapsed:?}");
    assert!(elapsed < Duration::from_millis(500), "{elapsed:?}");
}

#[test]
fn kj_timer_armed_by_a_task_while_the_driver_is_parked_is_honored() {
    // No KJ timer exists when the driver parks (it parks forever); the task arms one 10ms out.
    // The port's sleep hooks must re-plan the park, or the timer fires only when something else
    // wakes the driver -- here, never.
    let mut rt = TestRuntime::new();
    let start = Instant::now();
    block_on(&mut rt, async {
        test_helpers::task_awaits_kj_timer(30, 10)
            .await
            .expect("task");
    });
    let elapsed = start.elapsed();
    assert!(elapsed >= Duration::from_millis(40), "{elapsed:?}");
    assert!(elapsed < Duration::from_secs(5), "{elapsed:?}");
}

#[test]
fn kj_timer_armed_by_a_task_during_a_bounded_park_is_honored_at_its_own_deadline() {
    // The driver is parked against a 1s KJ timer; a task arms a 10ms one. The sooner deadline
    // must end the park early.
    let mut rt = TestRuntime::new();
    let start = Instant::now();
    block_on(&mut rt, async {
        let long = ffi::kj_timer_delay(1_000);
        let short = test_helpers::task_awaits_kj_timer(20, 10);
        futures::future::select(std::pin::pin!(long), std::pin::pin!(short)).await;
    });
    let elapsed = start.elapsed();
    assert!(elapsed >= Duration::from_millis(30), "{elapsed:?}");
    assert!(elapsed < Duration::from_millis(700), "{elapsed:?}");
}

#[test]
fn a_task_fulfilling_a_kj_fulfiller_while_parked_wakes_the_driver() {
    // The fulfiller arms a KJ event from a tokio task while the driver is parked with nothing
    // planned: setRunnable(true) is the only thing that can wake it.
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        let promise = ffi::test_fulfiller_promise();
        test_helpers::task_fulfills_kj_fulfiller(30, 7);
        assert_eq!(promise.await.expect("promise"), 7);
    });
}

#[test]
fn execute_sync_from_another_thread_runs_on_the_parked_loop() {
    // kj::Executor events are only drained when the port's poll() reports the wake latch; the
    // driver reaches poll() through pollTurns() after waking.
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        assert_eq!(ffi::execute_sync_from_thread(21).await.expect("exec"), 42);
    });
}

#[test]
fn cross_thread_fulfiller_wakes_the_parked_driver() {
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        assert_eq!(
            ffi::cross_thread_fulfill_from_thread(30, 5)
                .await
                .expect("fulfilled"),
            5
        );
    });
}

#[test]
fn a_bridged_rust_future_polled_by_a_kj_coroutine_completes() {
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        ffi::kj_awaits_rust_sleep(20).await.expect("coroutine");
    });
}

#[test]
fn spawned_tasks_and_kj_promises_interleave() {
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        let before = test_helpers::completed_task_count();
        let (a, b) = futures::future::join(
            test_helpers::spawn_task_on_runtime(10, 1),
            ffi::kj_timer_delay(5),
        )
        .await;
        assert_eq!(a.expect("task"), 1);
        b.expect("timer");
        assert!(test_helpers::completed_task_count() > before);
    });
}

#[test]
fn yield_until_would_sleep_completes() {
    // The would-sleep queue is only serviced by EventLoop::poll(), which the driver reaches
    // through pollTurns() once the ordinary queue is dry.
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        ffi::kj_yield_until_would_sleep().await.expect("yield");
        // And again, with a task busy meanwhile.
        let (yielded, ()) = futures::future::join(
            ffi::kj_yield_until_would_sleep(),
            tokio::time::sleep(Duration::from_millis(5)),
        )
        .await;
        yielded.expect("yield");
    });
}

#[test]
fn nested_wait_fails_with_a_clear_error_from_a_task_and_from_the_main_future() {
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        let from_main = ffi::nested_wait();
        let message = from_main.expect_err("wait() must fail").to_string();
        assert!(
            message.contains("not allowed on a tokio-driven"),
            "unexpected error: {message}"
        );
        test_helpers::nested_wait_from_task()
            .await
            .expect("task saw the error");
        // The loop is still healthy.
        ffi::kj_timer_delay(1).await.expect("timer");
    });
}

#[test]
fn a_panicking_task_surfaces_as_an_error_not_an_abort() {
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        assert!(test_helpers::spawn_panicking_task().await.is_err());
        ffi::kj_timer_delay(1).await.expect("timer");
    });
}

#[test]
fn a_bridged_future_woken_from_a_plain_thread_while_parked_completes() {
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        test_helpers::std_thread_wake_future().await.expect("woken");
    });
}

#[test]
fn teardown_with_tasks_holding_kj_timers_and_promises_is_clean() {
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        test_helpers::spawn_task_holding_kj_timer();
        test_helpers::spawn_task_awaiting_kj_never_promise();
        test_helpers::spawn_pending_task();
        // Let the tasks run to their first await so they actually hold the KJ objects.
        tokio::time::sleep(Duration::from_millis(5)).await;
    });
    drop(rt);
}

#[test]
fn teardown_with_a_kj_promise_pending_in_the_dropped_main_future_is_clean() {
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        let never = std::pin::pin!(ffi::kj_never_promise());
        let short = std::pin::pin!(tokio::time::sleep(Duration::from_millis(5)));
        futures::future::select(never, short).await;
    });
}

#[test]
fn a_task_that_yields_forever_does_not_starve_the_kj_loop() {
    let mut rt = TestRuntime::new();
    block_on(&mut rt, async {
        test_helpers::spawn_yield_loop_task();
        ffi::kj_timer_delay(20).await.expect("timer");
        assert_eq!(ffi::execute_sync_from_thread(4).await.expect("exec"), 8);
    });
}

#[test]
fn a_second_runtime_on_the_same_thread_is_refused() {
    let _rt = TestRuntime::new();
    let error = Runtime::new().err().expect("second runtime must fail");
    assert!(
        error.description().contains("already exists"),
        "unexpected error: {error:?}"
    );
}

#[test]
fn two_runtimes_on_two_threads_execute_async_into_each_other() {
    // Both directions at once, N round trips each way, so both drivers' wake paths (Executor ->
    // wake() -> Notify) race under load. A lost wake hangs the test. Neither loop goes away
    // before both have finished calling into the other (the barrier).
    const N: i32 = 200;
    let barrier = Arc::new(tokio::sync::Barrier::new(2));
    let run = |own: u8, peer: u8, barrier: Arc<tokio::sync::Barrier>| {
        move || {
            let mut rt = TestRuntime::new();
            ffi::publish_executor(own).expect("publish");
            block_on(&mut rt, async {
                for i in 0..N {
                    assert_eq!(ffi::execute_async_on(peer, i).await.expect("exec"), i);
                }
                barrier.wait().await;
            });
            ffi::clear_executor(own).expect("clear");
        }
    };
    let other = std::thread::spawn(run(1, 0, Arc::clone(&barrier)));
    run(0, 1, barrier)();
    other.join().expect("other thread");
}

#[test]
fn block_on_can_be_called_repeatedly() {
    let mut rt = TestRuntime::new();
    let flag = Arc::new(AtomicBool::new(false));
    for i in 0..3 {
        let flag = Arc::clone(&flag);
        block_on(&mut rt, async move {
            ffi::kj_timer_delay(1).await.expect("timer");
            tokio::time::sleep(Duration::from_millis(1)).await;
            flag.store(true, Ordering::SeqCst);
            i
        });
    }
    assert!(flag.load(Ordering::SeqCst));
}
