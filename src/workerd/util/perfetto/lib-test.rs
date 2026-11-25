// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::future::Future;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use perfetto_test_session::Session;
use perfetto_test_session::compiled_in;

use super::*;

/// The events a session records while `f` runs, or `None` without perfetto.
#[expect(
    clippy::significant_drop_tightening,
    reason = "the session's lock must be held while `f` runs; `stop()` consumes it"
)]
fn record(f: impl FnOnce()) -> Option<Vec<String>> {
    let session = Session::start();
    f();
    let events = session.stop();
    if compiled_in() {
        Some(events)
    } else {
        assert_eq!(events, Vec::<String>::new());
        None
    }
}

/// Returns `Pending` once, so that other futures run in between.
struct YieldOnce(bool);

impl Future for YieldOnce {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if self.0 {
            Poll::Ready(())
        } else {
            self.0 = true;
            cx.waker().wake_by_ref();
            Poll::Pending
        }
    }
}

#[test]
fn slices_without_a_session() {
    let _no_session = perfetto_test_session::no_session();
    assert!(!category_enabled());
    {
        trace_event!("no session");
        trace_event_async!("no session async");
    }
    drop(ScopedSlice::begin(c"no session guard"));
    drop(AsyncSlice::begin(c"no session async guard"));
}

#[test]
fn category_is_enabled_during_a_session() {
    let mut enabled = false;
    record(|| enabled = category_enabled());
    assert_eq!(enabled, compiled_in());
    let _no_session = perfetto_test_session::no_session();
    assert!(!category_enabled());
}

#[test]
fn scoped_slices_nest_on_the_thread_track() {
    let Some(events) = record(|| {
        trace_event!("outer");
        {
            trace_event!("inner");
            trace_event!("innermost");
        }
    }) else {
        return;
    };
    assert_eq!(
        events,
        [
            "B thread outer",
            "B thread inner",
            "B thread innermost",
            "E thread",
            "E thread",
            "E thread",
        ]
    );
}

#[test]
fn slices_that_began_before_the_session_do_not_end_in_it() {
    // Begun under the lock, so that they don't write into another test's session.
    let (scoped, async_slice) = {
        let _no_session = perfetto_test_session::no_session();
        (
            ScopedSlice::begin(c"before"),
            AsyncSlice::begin(c"before async"),
        )
    };
    let Some(events) = record(|| {
        drop(scoped);
        drop(async_slice);
        trace_event!("during");
    }) else {
        return;
    };
    assert_eq!(events, ["B thread during", "E thread"]);
}

#[test]
fn interleaved_async_slices_keep_their_own_tracks() {
    let Some(events) = record(|| {
        let a = AsyncSlice::begin(c"a");
        let b = AsyncSlice::begin(c"b");
        drop(a);
        drop(b);
    }) else {
        return;
    };
    assert_eq!(events, ["B t1 a", "B t2 b", "E t1", "E t2"]);
}

#[test]
fn async_slices_reuse_the_tracks_of_ended_slices() {
    let Some(events) = record(|| {
        let a = AsyncSlice::begin(c"a");
        drop(a);
        let b = AsyncSlice::begin(c"b");
        let c = AsyncSlice::begin(c"c");
        drop(b);
        drop(c);
    }) else {
        return;
    };
    // `b` begins on the track `a` ended on; `c` begins while `b` is open, so it gets another.
    assert_eq!(
        events,
        ["B t1 a", "E t1", "B t1 b", "B t2 c", "E t1", "E t2"]
    );
}

#[test]
fn async_slices_span_awaits() {
    async fn task(yields: usize) {
        trace_event_async!("task");
        for _ in 0..yields {
            YieldOnce(false).await;
        }
    }
    let Some(events) = record(|| {
        futures::executor::block_on(futures::future::join(task(1), task(2)));
    }) else {
        return;
    };
    // The first task ends while the second is still waiting.
    assert_eq!(events, ["B t1 task", "B t2 task", "E t1", "E t2"]);
}

#[test]
fn async_slices_end_on_another_thread() {
    let Some(events) = record(|| {
        let slice = AsyncSlice::begin(c"moved");
        std::thread::spawn(move || {
            drop(slice);
            // The session reads only what this thread has committed.
            perfetto_test_session::flush_thread();
        })
        .join()
        .unwrap();
    }) else {
        return;
    };
    // The two threads write separate sequences, which the trace orders by when each was
    // committed.
    let mut events = events;
    events.sort();
    assert_eq!(events, ["B t1 moved", "E t1"]);
}

#[test]
fn track_ids_are_distinct() {
    let ids: std::collections::HashSet<u64> = (0..10_000).map(|_| next_track()).collect();
    assert_eq!(ids.len(), 10_000);
}
