// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::mpsc::Receiver;

use futures::executor::block_on;

use super::*;
use crate::bucket::Entry;
use crate::bucket::Guard;
use crate::bucket::Payload;
use crate::bucket::Store;
use crate::bucket::Version;
use crate::bucket::memory::Memory;

/// A lease of `lease_ms`, short enough for a test to wait out, renewed
/// every 10 ms, a failed renewal retried every 5 ms. Half a lease bounds a
/// round, so that a lease thread that cannot renew fences with most of a
/// round to spare: a loaded machine may run it late by that much. A loop
/// that makes no progress for two leases fences the node.
fn timing(lease_ms: u64) -> Timing {
    Timing {
        lease_ms,
        interval_ms: 10,
        renew_timeout_ms: lease_ms / 2,
        list_timeout_ms: lease_ms / 2,
        retry_ms: 5,
        renew_attempt_ms: lease_ms / 4,
        list_attempt_ms: lease_ms / 4,
        contended_pause_ms: 5,
        max_skew_ms: lease_ms / 20,
        stall_ms: 2 * lease_ms,
    }
}

/// A bucket that refuses every write once `down` is set.
struct Flaky {
    store: Memory,
    down: Arc<AtomicBool>,
}

#[async_trait::async_trait(?Send)]
impl Store for Flaky {
    async fn get(&self, key: &str, to: Option<&Path>) -> Result<Option<(Vec<u8>, Version)>> {
        self.store.get(key, to).await
    }

    async fn put(
        &self,
        key: &str,
        body: Payload,
        guard: Option<Guard>,
        metadata: &[(&str, String)],
    ) -> Result<Option<Version>> {
        if self.down.load(Ordering::Relaxed) {
            return Err(Error::Http("the bucket is down".into()));
        }
        self.store.put(key, body, guard, metadata).await
    }

    async fn list(&self, prefix: &str) -> Result<Vec<Entry>> {
        self.store.list(prefix).await
    }

    async fn delete(&self, key: &str) -> Result<()> {
        self.store.delete(key).await
    }
}

/// A lease thread for node `a` with `timing`, over a bucket that works
/// until the flag is set, and what it fenced with and when.
fn start(timing: Timing) -> (Thread, Arc<AtomicBool>, Receiver<(String, u64, Duration)>) {
    start_within(timing, here())
}

/// As `start`, its body run `within`.
fn start_within(
    timing: Timing,
    within: Within,
) -> (Thread, Arc<AtomicBool>, Receiver<(String, u64, Duration)>) {
    let down = Arc::new(AtomicBool::new(false));
    let (fenced, fence) = std::sync::mpsc::channel();
    let open = {
        let down = Arc::clone(&down);
        move || {
            let store = Memory::default();
            Ok(Bucket::new(Flaky { store, down }, ""))
        }
    };
    let fenced = Box::new(move |reason, remaining| {
        let _ = fenced.send((reason, now_ms(), remaining));
    });
    let thread = Thread::spawn(open, "a".into(), "a:1".into(), timing, fenced, within).unwrap();
    (thread, down, fence)
}

/// Runs the lease thread's body as it is.
fn here() -> Within {
    Box::new(|body| body())
}

fn until_ms(thread: &Thread) -> u64 {
    thread.held().until_ms.load(Ordering::Acquire)
}

/// Nothing on the caller's thread drives the renewals, which it would if
/// they ran on its executor: it only reads the expiry they publish.
#[test]
fn the_lease_thread_renews_the_lease_by_itself() {
    let timing = Timing {
        interval_ms: 10,
        ..Timing::DEFAULT
    };
    let (thread, _, fence) = start(timing);
    assert!(!thread.held().holds(now_ms()));
    block_on(thread.take_lease()).unwrap();
    assert!(thread.held().holds(now_ms()));
    let taken = until_ms(&thread);
    thread.maintain();
    let deadline = now_ms() + 10_000;
    while until_ms(&thread) == taken {
        assert!(now_ms() < deadline, "the lease was not renewed");
        std::thread::sleep(ms(1));
    }
    assert!(thread.held().holds(now_ms()));
    assert!(fence.try_recv().is_err());
}

/// Without its rounds, a lease taken once runs out.
#[test]
fn a_lease_nobody_renews_runs_out() {
    let timing = timing(100);
    let (thread, _, _fence) = start(timing);
    block_on(thread.take_lease()).unwrap();
    std::thread::sleep(ms(timing.lease_ms));
    assert!(!thread.held().holds(now_ms()));
}

#[test]
fn a_lease_that_cannot_be_renewed_fences_before_it_expires() {
    let timing = timing(1000);
    let (thread, down, fence) = start(timing);
    block_on(thread.take_lease()).unwrap();
    thread.maintain();
    down.store(true, Ordering::Relaxed);
    // Nobody polls anything here either: the lease thread fences by itself.
    let (_, fenced_ms, remaining) = fence.recv_timeout(ms(10 * timing.lease_ms)).unwrap();
    // Before a peer whose clock is off from this one's by up to twice the
    // skew allowed may judge the lease expired.
    let until = until_ms(&thread);
    assert!(
        timing.holds(until, fenced_ms),
        "fenced at {fenced_ms}, the lease expiring at {until}"
    );
    // The fence is told it has time left to end the process in, as of
    // when it was called.
    assert!(remaining > Duration::ZERO);
    assert!(remaining >= ms(timing.remaining_ms(until, fenced_ms)));
}

#[test]
fn a_node_that_left_renews_no_more() {
    // Rounds that fence only once a lease thread got no time for most of
    // the lease, which this test waits out.
    let timing = Timing {
        renew_timeout_ms: 20,
        renew_attempt_ms: 10,
        max_skew_ms: 5,
        ..timing(300)
    };
    let (thread, _, fence) = start(timing);
    block_on(thread.take_lease()).unwrap();
    thread.maintain();
    thread.start_draining();
    block_on(thread.announce_draining());
    block_on(thread.deregister()).unwrap();
    assert!(matches!(
        block_on(thread.take_lease()),
        Err(Error::NotHere(_))
    ));
    // Leaving is no reason to fence, however long ago it was.
    std::thread::sleep(ms(timing.lease_ms + timing.interval_ms));
    assert!(fence.try_recv().is_err());
    assert!(!thread.held().holds(now_ms()));
}

#[test]
fn a_bucket_that_does_not_open_fails_the_start() {
    let open = || Err(Error::Config("no such bucket".into()));
    let fence = Box::new(|_, _| {});
    let started = Thread::spawn(open, "a".into(), "a:1".into(), timing(100), fence, here());
    assert!(matches!(started, Err(Error::Config(_))));
}

/// Dropping the thread ends it, and its rounds and their requests with
/// it, before the drop returns.
#[test]
fn dropping_the_lease_thread_waits_for_it_to_end() {
    let ended = Arc::new(AtomicBool::new(false));
    let within: Within = {
        let ended = Arc::clone(&ended);
        Box::new(move |body| {
            body();
            ended.store(true, Ordering::Release);
        })
    };
    let (thread, _, fence) = start_within(timing(1000), within);
    block_on(thread.take_lease()).unwrap();
    thread.maintain();
    drop(thread);
    assert!(ended.load(Ordering::Acquire));
    // A thread told to stop is no reason to fence.
    assert!(fence.try_recv().is_err());
}

/// Closes the lease thread's commands' channel without a `Command::Stop`,
/// which ends its body as no `Thread` dropped would.
fn close_commands(thread: &mut Thread) {
    thread.commands = mpsc::unbounded_channel().0;
}

/// Waits for the lease thread to end, by itself.
fn wait_for_end(thread: &Thread) {
    let handle = thread.handle.as_ref().unwrap();
    let deadline = now_ms() + 10_000;
    while !handle.is_finished() {
        assert!(now_ms() < deadline, "the lease thread did not end");
        std::thread::sleep(ms(1));
    }
}

/// A lease thread whose body ends without being told to stop renews no
/// more, while the node goes on serving under the lease it last renewed.
#[test]
fn a_lease_thread_that_ends_unasked_fences() {
    let timing = timing(1000);
    let (mut thread, _, fence) = start(timing);
    block_on(thread.take_lease()).unwrap();
    thread.maintain();
    close_commands(&mut thread);
    let (reason, _, remaining) = fence.recv_timeout(ms(timing.lease_ms)).unwrap();
    assert_eq!(reason, "its lease cannot be kept: the lease thread stopped");
    assert!(remaining > Duration::ZERO);
    wait_for_end(&thread);
    assert!(fence.try_recv().is_err());
}

/// A panic outside the thread's tasks fences as its body unwinds.
#[test]
fn a_lease_thread_that_panics_fences() {
    let timing = timing(1000);
    let (thread, _, fence) = start(timing);
    block_on(thread.take_lease()).unwrap();
    thread.maintain();
    thread.send(Command::Panic);
    let (reason, _, remaining) = fence.recv_timeout(ms(timing.lease_ms)).unwrap();
    assert_eq!(reason, "its lease cannot be kept: the lease thread stopped");
    assert!(remaining > Duration::ZERO);
}

/// A node whose rounds have not started keeps no lease the thread's end
/// would leave unrenewed: its start fails instead.
#[test]
fn a_lease_thread_that_ends_before_its_rounds_does_not_fence() {
    let (mut thread, _, fence) = start(timing(1000));
    block_on(thread.take_lease()).unwrap();
    close_commands(&mut thread);
    wait_for_end(&thread);
    assert!(fence.try_recv().is_err());
}

#[test]
fn a_lease_thread_that_ends_after_the_node_left_does_not_fence() {
    let (mut thread, _, fence) = start(timing(1000));
    block_on(thread.take_lease()).unwrap();
    thread.maintain();
    block_on(thread.deregister()).unwrap();
    close_commands(&mut thread);
    wait_for_end(&thread);
    assert!(fence.try_recv().is_err());
}

/// Rounds that fenced leave nothing for the thread's end to fence with.
#[test]
fn a_lease_thread_fences_once() {
    let timing = timing(1000);
    let (mut thread, down, fence) = start(timing);
    block_on(thread.take_lease()).unwrap();
    thread.maintain();
    down.store(true, Ordering::Relaxed);
    let (reason, _, _) = fence.recv_timeout(ms(10 * timing.lease_ms)).unwrap();
    assert!(reason.starts_with("its lease cannot be kept: "), "{reason}");
    assert_ne!(reason, "its lease cannot be kept: the lease thread stopped");
    close_commands(&mut thread);
    wait_for_end(&thread);
    assert!(fence.try_recv().is_err());
}

/// For the watchdog's tests: a lease that a loaded machine keeps, and a
/// stall shorter than it, so that they wait little.
fn watching() -> Timing {
    Timing {
        stall_ms: 200,
        ..timing(400)
    }
}

/// A loop that makes progress while `moving` is set.
#[derive(Default)]
struct Loop {
    moving: AtomicBool,
    /// When the loop last made progress.
    last_ms: std::sync::atomic::AtomicU64,
}

/// The progress of `the_loop`, whose count advances at each reading while
/// it moves.
fn progress(the_loop: &Arc<Loop>) -> Progress {
    let the_loop = Arc::clone(the_loop);
    let count = std::sync::atomic::AtomicU64::new(0);
    Box::new(move || {
        if the_loop.moving.load(Ordering::Relaxed) {
            the_loop.last_ms.store(now_ms(), Ordering::Relaxed);
            count.fetch_add(1, Ordering::Relaxed);
        }
        count.load(Ordering::Relaxed)
    })
}

fn moving() -> Arc<Loop> {
    let the_loop = Arc::new(Loop::default());
    the_loop.moving.store(true, Ordering::Relaxed);
    the_loop
}

#[test]
fn a_loop_that_makes_progress_keeps_the_lease() {
    let timing = watching();
    let the_loop = moving();
    let (thread, _, fence) = start(timing);
    block_on(thread.take_lease()).unwrap();
    thread.maintain();
    thread.watch(progress(&the_loop));
    std::thread::sleep(ms(2 * timing.stall_ms));
    assert!(fence.try_recv().is_err());
    assert!(thread.held().holds(now_ms()));
}

/// The lease thread renews the lease all the while, and fences with it
/// still held.
#[test]
fn a_loop_that_makes_no_progress_fences_the_node_while_its_lease_holds() {
    // A stall longer than the lease, which only renewals keep.
    let timing = Timing {
        stall_ms: 600,
        ..watching()
    };
    let the_loop = moving();
    let (thread, _, fence) = start(timing);
    block_on(thread.take_lease()).unwrap();
    thread.maintain();
    thread.watch(progress(&the_loop));
    std::thread::sleep(ms(timing.lease_ms / 2));
    the_loop.moving.store(false, Ordering::Relaxed);
    let (reason, fenced_ms, remaining) = fence.recv_timeout(ms(10 * timing.stall_ms)).unwrap();
    assert!(
        reason.starts_with("its event loop made no progress for "),
        "{reason}"
    );
    // The loop's last progress is stamped as the lease thread reads it,
    // just after the watchdog's clock: a lease thread descheduled between
    // the two opens a gap that `slack` allows for.
    let stuck_ms = the_loop.last_ms.load(Ordering::Relaxed);
    assert!(
        fenced_ms + slack(&timing) >= stuck_ms + timing.stall_ms,
        "fenced at {fenced_ms}, stuck since {stuck_ms}"
    );
    let until = until_ms(&thread);
    assert!(
        timing.holds(until, fenced_ms),
        "fenced at {fenced_ms}, the lease expiring at {until}"
    );
    assert!(remaining > Duration::ZERO);
}

/// Linking, between `join` and serving, runs in turns as long as itself:
/// the rounds watch no loop before `watch`, and time a loop that stands
/// still from then.
#[test]
fn the_rounds_watch_the_loop_only_from_watch_on() {
    let timing = watching();
    let (thread, _, fence) = start(timing);
    block_on(thread.take_lease()).unwrap();
    thread.maintain();
    // Startup, in one turn of a loop no one watches yet.
    std::thread::sleep(ms(2 * timing.stall_ms));
    assert!(fence.try_recv().is_err());
    assert!(thread.held().holds(now_ms()));
    let watched_ms = now_ms();
    thread.watch(progress(&Arc::new(Loop::default())));
    let (reason, fenced_ms, _) = fence.recv_timeout(ms(10 * timing.stall_ms)).unwrap();
    assert!(
        reason.starts_with("its event loop made no progress for "),
        "{reason}"
    );
    assert!(
        fenced_ms + slack(&timing) >= watched_ms + timing.stall_ms,
        "fenced at {fenced_ms}, watched since {watched_ms}"
    );
}

/// How much earlier than `stall_ms` after a reading a fence may look, as
/// the tests' wall clock sees it: the watchdog times the stall on a clock
/// of its own, read apart from the count, by a thread a loaded machine may
/// delay in between. A quarter of the stall still tells a fence that waits
/// for the stall from one that does not.
const fn slack(timing: &Timing) -> u64 {
    timing.stall_ms / 4
}
