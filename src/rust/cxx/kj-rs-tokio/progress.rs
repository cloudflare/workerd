//! [`Progress`]: whether this thread's event loop moves on, as another thread sees it.

use std::fmt;
use std::sync::Arc;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;
use std::time::Duration;

use crate::ffi::start_progress_probe;
use crate::port::current_local_set;
use crate::port::current_progress;

/// A count that advances while this thread's event loop makes progress, readable from any
/// thread.
///
/// A watchdog on another thread that sees it stand still knows that the loop thread is held by
/// one KJ event, or one tokio task, that does not end: a deadlock, native code that does not
/// return, a turn that never ends.
///
/// It must not stand still while the loop is merely busy, and KJ's `EventLoop` has no hook that
/// runs per turn. Nor does anything else run between two turns of a busy loop: the loop calls
/// the port's `wait()` or `poll()` only once its run queue is empty, so until then no tokio task
/// runs, no `kj::Timer` fires (`kj::TimerImpl` fires timers only after the port's `wait()` or
/// `poll()`), and no event from another thread is taken in (`kj::Executor`'s queue is drained
/// only after the port wakes). A tokio interval, a KJ timer or a cross-thread ping with a timed
/// reply would all go unanswered by a loop that turns events without pause. So the count has
/// two sources, one for each way the loop spends its time:
///
/// * While KJ's run queue holds events, the port's progress probe (`TokioEventPort`,
///   tokio-event-port.c++) counts. It is a KJ event that, each time it fires, counts and arms
///   itself breadth-first again, behind every event armed before it, so that it fires once per
///   pass through the queue. Once it finds no depth-first or breadth-first event left but itself,
///   it leaves the queue, so that the loop still sleeps and still runs its `evalLast()` events.
///   The port arms it again whenever its `wait()` returns, and whenever its `poll()` leaves the
///   loop with events to turn.
/// * While the loop sleeps in the port, a task on the loop's `LocalSet` counts every `beat`.
///
/// So the count stands still while one event or one task keeps the thread. A chain of events
/// that each arm the next depth-first, as continuations of promises that are already resolved do,
/// runs ahead of the probe, as it runs ahead of every other event, until it ends: the count takes
/// it for one event.
///
/// It also stands still through a busy stretch that begins, with the probe out of the queue,
/// with one of three kinds of event, until the run queue next empties. Nothing of the port's
/// runs before them that could arm the probe:
///
/// * An `evalLast()` or `yieldUntilQueueEmpty()` event (`Event::armLast()`). The probe leaves
///   the queue so that these run, and cannot follow them: `armLast()` puts each in front of
///   those armed before it, and a probe armed in front of them fires again at once.
/// * A `yieldUntilWouldSleep()` waiter. With one waiting, KJ's `EventLoop::wait()` polls the port
///   instead of waiting on it, and if that leaves no event to turn, arms the waiter right after
///   the port's `poll()` returns. A probe armed in that `poll()` would leave one, and the waiter
///   would never run.
/// * An event from another thread (`kj::Executor`, `CrossThreadPromiseFulfiller`) that KJ takes
///   in after the port's `poll()` returns, rather than after its `wait()`. That `poll()` cannot
///   arm the probe for the wake that announces the event: KJ cancels a queued event without
///   consuming its wake, so a probe armed for every wake could keep `WaitScope::poll()` from
///   returning, and a would-sleep waiter from running, for as long as another thread submits and
///   cancels.
///
/// Only work that is ready without the port keeps the queue from emptying: promises that other
/// events resolve, and reads and writes that complete without waiting on the socket. Meanwhile
/// the loop takes in no new connection, timer or cross-thread event. A stretch like that which
/// lasts as long as a watchdog waits for the count (the cluster's `Timing::stall_ms`, three
/// leases) looks like a loop held by one event. KJ could close all three gaps by counting in
/// `EventLoop::turn()`, for which it has no hook.
///
/// The probe costs a turn per pass through the queue, and one per wake of the loop. Once
/// started, it runs for as long as the port does; the beat stops once the last clone of the
/// `Progress` that started it is dropped.
#[derive(Clone)]
pub struct Progress {
    count: Arc<AtomicU64>,
    /// The beat's task runs while this has a strong count.
    _beat: Arc<()>,
}

impl Progress {
    /// The count so far.
    #[must_use]
    pub fn count(&self) -> u64 {
        self.count.load(Ordering::Relaxed)
    }
}

/// Why [`watch_progress`] counts nothing.
#[derive(Debug)]
pub enum WatchError {
    /// No `TokioEventPort` drives this thread.
    NoPort,
    /// The port's progress probe did not start.
    Probe(String),
}

impl fmt::Display for WatchError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NoPort => f.write_str("no TokioEventPort drives this thread"),
            Self::Probe(e) => write!(f, "the progress probe did not start: {e}"),
        }
    }
}

impl std::error::Error for WatchError {}

/// Starts counting the progress of this thread's event loop, with a beat of `beat` while the
/// loop sleeps (`Progress`).
///
/// # Errors
///
/// [`WatchError`] if no `TokioEventPort` drives this thread, or its probe does not start.
pub fn watch_progress(beat: Duration) -> Result<Progress, WatchError> {
    let count = current_progress().ok_or(WatchError::NoPort)?;
    let local = current_local_set().ok_or(WatchError::NoPort)?;
    start_progress_probe().map_err(|e| WatchError::Probe(e.to_string()))?;
    let interest = Arc::new(());
    let watched = Arc::downgrade(&interest);
    let beats = Arc::clone(&count);
    local.spawn_local(async move {
        while watched.strong_count() > 0 {
            tokio::time::sleep(beat).await;
            beats.fetch_add(1, Ordering::Relaxed);
        }
    });
    Ok(Progress {
        count,
        _beat: interest,
    })
}

#[cfg(test)]
#[path = "progress-test.rs"]
mod tests;
