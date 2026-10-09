// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::sync::atomic::AtomicU64;

use super::*;
use crate::Event;
use crate::RecordingSink;

/// A clock that only moves when told to.
#[derive(Clone, Default)]
struct ManualClock(Arc<AtomicU64>);

impl ManualClock {
    fn set(&self, now: Nanos) {
        self.0.store(now, Ordering::Relaxed);
    }
}

impl Clock for ManualClock {
    fn now(&self) -> Nanos {
        self.0.load(Ordering::Relaxed)
    }
}

struct Fixture {
    tracker: Tracker,
    sink: RecordingSink,
    clock: ManualClock,
    isolate: Arc<IsolateState>,
}

impl Fixture {
    fn new() -> Self {
        let isolate = Arc::new(IsolateState::new());
        Self::with_isolate(isolate)
    }

    fn with_isolate(isolate: Arc<IsolateState>) -> Self {
        let clock = ManualClock::default();
        let mut tracker = Tracker::new(Arc::clone(&isolate), "main", None, Box::new(clock.clone()));
        let sink = RecordingSink::new();
        tracker.add_sink(Box::new(sink.clone()));
        // Discard ContextBegin; tests that care about it check it directly.
        let _ = sink.take();
        Self {
            tracker,
            sink,
            clock,
            isolate,
        }
    }

    /// Events since the last call, without flush markers.
    fn events(&self) -> Vec<Event> {
        self.sink
            .take()
            .into_iter()
            .filter(|event| *event != Event::Flush)
            .collect()
    }

    fn ctx(&self) -> ContextId {
        self.tracker.context_id()
    }

    fn create(&mut self, kind: Kind, name: &str) -> AsyncId {
        self.tracker.create(kind, name, 0, None)
    }
}

fn init_of(events: &[Event], id: AsyncId) -> (AsyncId, AsyncId, Kind, String) {
    events
        .iter()
        .find_map(|event| match event {
            Event::Init {
                id: event_id,
                trigger,
                execution,
                kind,
                name,
                ..
            } if *event_id == id => Some((*trigger, *execution, *kind, name.clone())),
            _ => None,
        })
        .unwrap()
}

#[test]
fn add_sink_reports_context() {
    let isolate = Arc::new(IsolateState::new());
    let mut tracker = Tracker::new(
        Arc::clone(&isolate),
        "svc",
        Some("actor-1"),
        Box::new(ManualClock::default()),
    );
    let sink = RecordingSink::new();
    tracker.add_sink(Box::new(sink.clone()));
    assert_eq!(
        sink.events(),
        vec![Event::ContextBegin {
            ctx: tracker.context_id(),
            isolate: isolate.id(),
            worker: "svc".to_owned(),
            actor: Some("actor-1".to_owned()),
        }]
    );
}

#[test]
fn contexts_have_distinct_ids() {
    assert_ne!(Fixture::new().ctx(), Fixture::new().ctx());
}

#[test]
fn create_outside_a_turn_has_no_trigger() {
    let mut f = Fixture::new();
    let id = f.create(Kind::Request, "fetch");
    assert_ne!(id, 0);
    let events = f.events();
    assert_eq!(
        init_of(&events, id),
        (0, 0, Kind::Request, "fetch".to_owned())
    );
}

#[test]
fn explicit_trigger_wins() {
    let mut f = Fixture::new();
    let request = f.create(Kind::Request, "fetch");
    let other = f.create(Kind::Other, "x");
    f.tracker.turn_begin(0);
    f.tracker.set_turn_cause(request);
    let id = f.tracker.create(Kind::Timer, "setTimeout", other, None);
    let events = f.events();
    assert_eq!(init_of(&events, id).0, other);
}

// The core property: without promise hooks, a resource created after `await` (that is, in a later
// turn, during the microtask drain) is triggered by whatever started that turn.
#[test]
fn turn_cause_is_the_default_trigger_across_awaits() {
    let mut f = Fixture::new();
    let request = f.create(Kind::Request, "fetch");

    // Turn 1: the handler runs and starts kv_get, whose bridge adopts it.
    f.tracker.turn_begin(0);
    f.tracker.set_turn_cause(request);
    let kv = f.create(Kind::Operation, "kv_get");
    assert_eq!(f.tracker.adopt_operation(), kv);
    f.tracker.turn_end();

    // kv_get completes on the KJ side.
    f.tracker.settle(kv, Outcome::Ok);

    // Turn 2: the bridge resumes JS. The code after `await` runs in the microtask drain, with no
    // scope of its own, and starts a second operation.
    f.tracker.turn_begin(0);
    f.tracker.set_turn_cause(kv);
    let cache = f.create(Kind::Operation, "cache_match");
    f.tracker.turn_end();

    let events = f.events();
    assert_eq!(init_of(&events, kv).0, request);
    assert_eq!(init_of(&events, kv).1, request);
    assert_eq!(init_of(&events, cache).0, kv);
    assert_eq!(init_of(&events, cache).1, kv);
}

#[test]
fn turn_reports_cause_scope_and_timing() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let timer = f.create(Kind::Timer, "setTimeout");
    let _ = f.events();

    f.clock.set(100);
    f.tracker.turn_begin(0);
    f.clock.set(150);
    f.tracker.turn_locked();
    f.clock.set(160);
    f.tracker.turn_locked(); // Only the first counts.
    f.tracker.set_turn_cause(timer);
    assert_eq!(f.tracker.current(), timer);
    f.clock.set(400);
    f.tracker.turn_end();
    assert_eq!(f.tracker.current(), 0);

    assert_eq!(
        f.events(),
        vec![
            Event::Before { ctx, id: timer },
            Event::After { ctx, id: timer },
            Event::Turn {
                ctx,
                turn: Turn {
                    cause: timer,
                    start: 100,
                    locked: Some(150),
                    end: 400,
                },
            },
        ]
    );
    assert_eq!(f.tracker.stats().unbalanced, 0);
}

#[test]
fn turn_without_cause() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    f.tracker.turn_begin(0);
    let id = f.create(Kind::Microtask, "queueMicrotask");
    f.tracker.turn_end();
    let events = f.events();
    assert_eq!(init_of(&events, id).0, 0);
    assert!(matches!(
        events.last(),
        Some(Event::Turn { ctx: c, turn }) if *c == ctx && turn.cause == 0 && turn.locked.is_none()
    ));
}

#[test]
fn flush_happens_after_outermost_turn_only() {
    let mut f = Fixture::new();
    f.tracker.turn_begin(0);
    f.tracker.turn_begin(0);
    f.tracker.turn_end();
    assert!(!f.sink.events().contains(&Event::Flush));
    f.tracker.turn_end();
    assert_eq!(f.sink.events().last(), Some(&Event::Flush));
}

#[test]
fn nested_turn_has_its_own_cause() {
    let mut f = Fixture::new();
    let outer = f.create(Kind::Timer, "outer");
    let inner = f.create(Kind::Timer, "inner");
    f.tracker.turn_begin(0);
    f.tracker.set_turn_cause(outer);
    f.tracker.turn_begin(0);
    f.tracker.set_turn_cause(inner);
    assert_eq!(f.tracker.turn_cause(), inner);
    f.tracker.turn_end();
    assert_eq!(f.tracker.turn_cause(), outer);
    assert_eq!(f.tracker.current(), outer);
    f.tracker.turn_end();
    assert_eq!(f.tracker.stats().unbalanced, 0);
}

#[test]
fn second_turn_cause_is_rejected() {
    let mut f = Fixture::new();
    let a = f.create(Kind::Timer, "a");
    let b = f.create(Kind::Timer, "b");
    f.tracker.turn_begin(0);
    f.tracker.set_turn_cause(a);
    f.tracker.set_turn_cause(b);
    assert_eq!(f.tracker.turn_cause(), a);
    assert_eq!(f.tracker.stats().unbalanced, 1);
    f.tracker.turn_end();
    assert_eq!(f.tracker.stats().unbalanced, 1);
}

#[test]
fn turn_misuse_outside_a_turn_is_counted() {
    let mut f = Fixture::new();
    let a = f.create(Kind::Timer, "a");
    f.tracker.set_turn_cause(a);
    f.tracker.turn_end();
    f.tracker.turn_locked(); // Harmless outside a turn.
    assert_eq!(f.tracker.stats().unbalanced, 2);
}

#[test]
fn settle_is_reported_once() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let id = f.create(Kind::KjToJs, "bridge");
    let _ = f.events();
    f.tracker.settle(id, Outcome::Error);
    f.tracker.settle(id, Outcome::Ok);
    assert_eq!(
        f.events(),
        vec![Event::Settle {
            ctx,
            id,
            outcome: Outcome::Error,
        }]
    );
}

#[test]
fn destroy_reports_only_unsettled_resources() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let canceled = f.create(Kind::Timer, "setTimeout");
    let finished = f.create(Kind::KjToJs, "bridge");
    f.tracker.settle(finished, Outcome::Ok);
    let _ = f.events();

    f.tracker.destroy(canceled);
    f.tracker.destroy(finished);
    // Both are released; only the unsettled one is destroyed.
    assert_eq!(
        f.events(),
        vec![
            Event::Destroy { ctx, id: canceled },
            Event::Release { ctx, id: canceled },
            Event::Release { ctx, id: finished },
        ]
    );

    // Both are forgotten.
    f.tracker.annotate(canceled, "k", "v");
    f.tracker.destroy(finished);
    assert_eq!(f.tracker.stats().unknown, 2);
    assert!(f.events().is_empty());
}

#[test]
fn annotate_reports_to_sinks() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let id = f.create(Kind::Operation, "fetch");
    let _ = f.events();
    f.tracker.annotate(id, "url", "https://example.com/");
    assert_eq!(
        f.events(),
        vec![Event::Annotate {
            ctx,
            id,
            key: "url".to_owned(),
            value: "https://example.com/".to_owned(),
        }]
    );
}

#[test]
fn unknown_ids_are_counted() {
    let mut f = Fixture::new();
    f.tracker.settle(12345, Outcome::Ok);
    f.tracker.annotate(12345, "k", "v");
    f.tracker.destroy(12345);
    assert_eq!(f.tracker.stats().unknown, 3);
    assert!(f.events().is_empty());
}

#[test]
fn id_zero_is_ignored_silently() {
    let mut f = Fixture::new();
    f.tracker.settle(0, Outcome::Ok);
    f.tracker.annotate(0, "k", "v");
    f.tracker.destroy(0);
    f.tracker.enter(0);
    f.tracker.exit(0);
    f.tracker.mark_bound(0);
    f.tracker.turn_begin(0);
    f.tracker.set_turn_cause(0);
    f.tracker.turn_end();
    assert_eq!(*f.tracker.stats(), ContextStats::default());
}

#[test]
fn nested_scopes() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let a = f.create(Kind::Timer, "a");
    let b = f.create(Kind::Microtask, "b");
    let _ = f.events();

    f.tracker.enter(a);
    let inner = f.create(Kind::Timer, "inner");
    assert_eq!(f.tracker.current(), a);
    f.tracker.enter(b);
    assert_eq!(f.tracker.current(), b);
    f.tracker.exit(b);
    f.tracker.exit(a);
    assert_eq!(f.tracker.current(), 0);

    let events = f.events();
    assert_eq!(init_of(&events, inner).1, a);
    let lifecycle: Vec<Event> = events
        .into_iter()
        .filter(|event| !matches!(event, Event::Init { .. }))
        .collect();
    assert_eq!(
        lifecycle,
        vec![
            Event::Before { ctx, id: a },
            Event::Before { ctx, id: b },
            Event::After { ctx, id: b },
            Event::After { ctx, id: a },
        ]
    );
    assert_eq!(f.tracker.stats().unbalanced, 0);
}

#[test]
fn mismatched_exit_discards_inner_scopes() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let a = f.create(Kind::Timer, "a");
    let b = f.create(Kind::Timer, "b");
    let _ = f.events();
    f.tracker.enter(a);
    f.tracker.enter(b);
    f.tracker.exit(a);
    assert_eq!(f.tracker.current(), 0);
    assert_eq!(f.tracker.stats().unbalanced, 1);
    // b's scope is gone, so its exit is unbalanced too.
    f.tracker.exit(b);
    assert_eq!(f.tracker.stats().unbalanced, 2);
    assert_eq!(
        f.events(),
        vec![
            Event::Before { ctx, id: a },
            Event::Before { ctx, id: b },
            Event::After { ctx, id: a },
        ]
    );
}

#[test]
fn unknown_scope_stays_balanced_but_is_not_reported() {
    let mut f = Fixture::new();
    f.tracker.enter(999);
    assert_eq!(f.tracker.current(), 999);
    f.tracker.exit(999);
    assert_eq!(f.tracker.current(), 0);
    assert_eq!(f.tracker.stats().unknown, 1);
    assert_eq!(f.tracker.stats().unbalanced, 0);
    assert!(f.events().is_empty());
}

#[test]
fn exit_after_destroy_still_reports_after() {
    // A callback's token can be dropped while the callback is still running.
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let id = f.create(Kind::Timer, "setTimeout");
    let _ = f.events();
    f.tracker.enter(id);
    f.tracker.destroy(id);
    f.tracker.exit(id);
    assert_eq!(
        f.events(),
        vec![
            Event::Before { ctx, id },
            Event::Destroy { ctx, id },
            Event::Release { ctx, id },
            Event::After { ctx, id },
        ]
    );
}

#[test]
fn exit_does_not_cross_into_an_enclosing_turn() {
    let mut f = Fixture::new();
    let outer = f.create(Kind::Timer, "outer");
    f.tracker.enter(outer);
    f.tracker.turn_begin(0);
    f.tracker.exit(outer);
    assert_eq!(f.tracker.current(), outer);
    assert_eq!(f.tracker.stats().unbalanced, 1);
    f.tracker.turn_end();
    f.tracker.exit(outer);
    assert_eq!(f.tracker.current(), 0);
    assert_eq!(f.tracker.stats().unbalanced, 1);
}

#[test]
fn turn_end_closes_scopes_left_open() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let cause = f.create(Kind::KjToJs, "bridge");
    let leaked = f.create(Kind::Microtask, "queueMicrotask");
    let _ = f.events();
    f.tracker.turn_begin(0);
    f.tracker.set_turn_cause(cause);
    f.tracker.enter(leaked);
    f.tracker.turn_end();
    assert_eq!(f.tracker.current(), 0);
    assert_eq!(f.tracker.stats().unbalanced, 1);
    let events = f.events();
    assert!(events.contains(&Event::After { ctx, id: cause }));
    assert!(!events.contains(&Event::After { ctx, id: leaked }));
}

#[test]
fn adopt_with_no_operation() {
    let mut f = Fixture::new();
    assert_eq!(f.tracker.adopt_operation(), 0);
    f.tracker.turn_begin(0);
    assert_eq!(f.tracker.adopt_operation(), 0);
    f.create(Kind::Timer, "not an operation");
    assert_eq!(f.tracker.adopt_operation(), 0);
    f.tracker.turn_end();
}

#[test]
fn adopt_takes_each_operation_once() {
    let mut f = Fixture::new();
    f.tracker.turn_begin(0);
    let op = f.create(Kind::Operation, "kv_get");
    assert_eq!(f.tracker.adopt_operation(), op);
    assert_eq!(f.tracker.adopt_operation(), 0);
    f.tracker.turn_end();
    assert_eq!(f.tracker.stats().ambiguous_bindings, 0);
}

#[test]
fn adopt_ignores_settled_operations() {
    // A synchronous phase, or a binding that answered from cache without going async.
    let mut f = Fixture::new();
    f.tracker.turn_begin(0);
    let phase = f.create(Kind::Operation, "fetch_prepare_request");
    f.tracker.settle(phase, Outcome::Ok);
    assert_eq!(f.tracker.adopt_operation(), 0);
    f.tracker.turn_end();
}

#[test]
fn adopt_ignores_destroyed_operations() {
    let mut f = Fixture::new();
    f.tracker.turn_begin(0);
    let op = f.create(Kind::Operation, "kv_get");
    f.tracker.destroy(op);
    assert_eq!(f.tracker.adopt_operation(), 0);
    f.tracker.turn_end();
}

#[test]
fn adopt_ignores_operations_from_earlier_turns() {
    let mut f = Fixture::new();
    f.tracker.turn_begin(0);
    f.create(Kind::Operation, "stashed");
    f.tracker.turn_end();
    f.tracker.turn_begin(0);
    assert_eq!(f.tracker.adopt_operation(), 0);
    f.tracker.turn_end();
}

#[test]
fn adopt_ignores_operations_created_outside_turns() {
    let mut f = Fixture::new();
    f.create(Kind::Operation, "outside");
    f.tracker.turn_begin(0);
    assert_eq!(f.tracker.adopt_operation(), 0);
    f.tracker.turn_end();
}

#[test]
fn adopt_only_sees_the_innermost_turn() {
    let mut f = Fixture::new();
    f.tracker.turn_begin(0);
    let outer = f.create(Kind::Operation, "outer");
    f.tracker.turn_begin(0);
    assert_eq!(f.tracker.adopt_operation(), 0);
    f.tracker.turn_end();
    assert_eq!(f.tracker.adopt_operation(), outer);
    f.tracker.turn_end();
}

#[test]
fn ambiguous_adoption_takes_the_latest_and_is_counted() {
    let mut f = Fixture::new();
    f.tracker.turn_begin(0);
    let first = f.create(Kind::Operation, "a");
    let second = f.create(Kind::Operation, "b");
    assert_eq!(f.tracker.adopt_operation(), second);
    assert_eq!(f.tracker.stats().ambiguous_bindings, 1);
    // One left, so no longer ambiguous.
    assert_eq!(f.tracker.adopt_operation(), first);
    assert_eq!(f.tracker.stats().ambiguous_bindings, 1);
    f.tracker.turn_end();
}

#[test]
fn mark_bound_removes_a_candidate() {
    let mut f = Fixture::new();
    f.tracker.turn_begin(0);
    let first = f.create(Kind::Operation, "a");
    let second = f.create(Kind::Operation, "b");
    f.tracker.mark_bound(second);
    assert_eq!(f.tracker.adopt_operation(), first);
    assert_eq!(f.tracker.stats().ambiguous_bindings, 0);
    f.tracker.turn_end();
}

#[test]
fn live_resource_cap() {
    let mut f = Fixture::new();
    f.tracker.set_max_live(1);
    let first = f.create(Kind::Timer, "a");
    assert_ne!(first, 0);
    assert_eq!(f.create(Kind::Timer, "b"), 0);
    assert_eq!(f.tracker.stats().dropped, 1);
    assert_eq!(f.tracker.stats().created, 1);
    f.tracker.destroy(first);
    assert_ne!(f.create(Kind::Timer, "c"), 0);
}

#[test]
fn ids_are_allocated_per_isolate() {
    let isolate = Arc::new(IsolateState::new());
    let mut first = Fixture::with_isolate(Arc::clone(&isolate));
    let mut second = Fixture::with_isolate(isolate);
    let one = first.create(Kind::Timer, "one");
    let two = second.create(Kind::Timer, "two");
    let three = first.create(Kind::Timer, "three");
    assert!(one < two && two < three);
}

#[test]
fn stacks_are_reported_once_per_tracker() {
    let mut f = Fixture::new();
    f.tracker.begin_stack();
    f.tracker.push_frame("handle", "worker.js", 3, 10, 5);
    f.tracker.push_frame("", "worker.js", 3, 20, 1);
    let stack = f.tracker.end_stack();
    assert!(stack.is_some());

    let first = f.tracker.create(Kind::Operation, "kv_get", 0, stack);
    let second = f.tracker.create(Kind::Operation, "kv_get", 0, stack);
    let events = f.events();
    assert!(matches!(
        &events[0],
        Event::Stack { isolate, id, frames }
            if *isolate == f.isolate.id() && Some(*id) == stack && frames.len() == 2
    ));
    assert!(matches!(&events[1], Event::Init { id, stack: s, .. } if *id == first && *s == stack));
    assert!(matches!(&events[2], Event::Init { id, stack: s, .. } if *id == second && *s == stack));
    assert_eq!(events.len(), 3);

    // The same stack, captured again, interns to the same ID; another tracker on the isolate
    // reports it to its own sinks.
    let mut other = Fixture::with_isolate(Arc::clone(&f.isolate));
    other.tracker.begin_stack();
    other.tracker.push_frame("handle", "worker.js", 3, 10, 5);
    other.tracker.push_frame("", "worker.js", 3, 20, 1);
    assert_eq!(other.tracker.end_stack(), stack);
    other.tracker.create(Kind::Operation, "kv_get", 0, stack);
    assert!(matches!(&other.events()[0], Event::Stack { .. }));
}

#[test]
fn empty_stack() {
    let mut f = Fixture::new();
    f.tracker.begin_stack();
    assert_eq!(f.tracker.end_stack(), None);
}

#[test]
fn close_reports_stats_then_ignores_everything() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let id = f.create(Kind::Timer, "a");
    f.tracker.settle(77, Outcome::Ok);
    f.tracker.turn_begin(0);
    let _ = f.sink.take();

    f.tracker.close();
    let stats = ContextStats {
        created: 1,
        unknown: 1,
        unbalanced: 1, // The turn left open.
        ..ContextStats::default()
    };
    assert_eq!(
        f.sink.take(),
        vec![Event::ContextEnd { ctx, stats }, Event::Flush]
    );

    assert_eq!(f.create(Kind::Timer, "b"), 0);
    f.tracker.settle(id, Outcome::Ok);
    f.tracker.enter(id);
    f.tracker.turn_end();
    f.tracker.close();
    f.tracker.add_sink(Box::new(RecordingSink::new()));
    assert!(f.sink.take().is_empty());
}

#[test]
fn drop_closes() {
    let f = Fixture::new();
    let ctx = f.ctx();
    let sink = f.sink.clone();
    drop(f);
    assert_eq!(
        sink.take(),
        vec![
            Event::ContextEnd {
                ctx,
                stats: ContextStats::default(),
            },
            Event::Flush,
        ]
    );
}

#[test]
fn every_sink_sees_every_event() {
    let mut f = Fixture::new();
    let second = RecordingSink::new();
    f.tracker.add_sink(Box::new(second.clone()));
    let _ = second.take();
    f.create(Kind::Timer, "a");
    assert_eq!(f.events(), second.take());
}

#[test]
fn default_cause_applies_when_nothing_better_is_known() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let request = f.create(Kind::Request, "fetch");
    let _ = f.events();
    f.tracker.turn_begin(request);
    assert_eq!(f.tracker.current(), request);
    assert_eq!(f.tracker.turn_cause(), request);
    let timer = f.create(Kind::Timer, "setTimeout");
    f.tracker.turn_end();

    let events = f.events();
    assert_eq!(init_of(&events, timer).0, request);
    assert_eq!(init_of(&events, timer).1, request);
    assert_eq!(events[0], Event::Before { ctx, id: request });
    assert_eq!(events[2], Event::After { ctx, id: request });
    assert!(matches!(&events[3], Event::Turn { turn, .. } if turn.cause == request));
    assert_eq!(f.tracker.stats().unbalanced, 0);
}

#[test]
fn explicit_cause_replaces_the_default() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let request = f.create(Kind::Request, "fetch");
    let bridge = f.create(Kind::KjToJs, "bridge");
    let _ = f.events();
    f.tracker.turn_begin(request);
    f.tracker.set_turn_cause(bridge);
    assert_eq!(f.tracker.current(), bridge);
    let timer = f.create(Kind::Timer, "setTimeout");
    f.tracker.turn_end();

    // The default was never entered, so it reports nothing.
    let events = f.events();
    assert_eq!(init_of(&events, timer).0, bridge);
    assert_eq!(events[0], Event::Before { ctx, id: bridge });
    assert!(matches!(&events[3], Event::Turn { turn, .. } if turn.cause == bridge));
    assert_eq!(f.tracker.stats().unbalanced, 0);

    // A second explicit cause is still rejected.
    f.tracker.turn_begin(request);
    f.tracker.set_turn_cause(bridge);
    f.tracker.set_turn_cause(timer);
    assert_eq!(f.tracker.turn_cause(), bridge);
    assert_eq!(f.tracker.stats().unbalanced, 1);
    f.tracker.turn_end();
}

#[test]
fn unknown_default_cause() {
    let mut f = Fixture::new();
    f.tracker.turn_begin(4242);
    assert_eq!(f.tracker.turn_cause(), 4242);
    f.create(Kind::Timer, "enters the default");
    f.tracker.turn_end();
    assert_eq!(f.tracker.stats().unknown, 1);
    assert_eq!(f.tracker.stats().unbalanced, 0);
}

#[test]
fn unused_operation_names_are_reported() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    f.tracker.count_unused_operation_name();
    f.tracker.count_unused_operation_name();
    let _ = f.sink.take();
    f.tracker.close();
    assert_eq!(
        f.events(),
        vec![Event::ContextEnd {
            ctx,
            stats: ContextStats {
                unused_operation_names: 2,
                ..ContextStats::default()
            },
        }]
    );
}

#[test]
fn foreign_thread_drops_are_reported() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    f.tracker.count_foreign_thread(2);
    f.tracker.count_foreign_thread(u64::MAX);
    let _ = f.sink.take();
    f.tracker.close();
    assert_eq!(
        f.events(),
        vec![Event::ContextEnd {
            ctx,
            stats: ContextStats {
                foreign_thread: u64::MAX,
                ..ContextStats::default()
            },
        }]
    );
}

#[test]
fn adopted_operation_outlives_its_creator() {
    // The binding's span ends (settling the operation) before the bridge's continuation runs.
    let mut f = Fixture::new();
    let ctx = f.ctx();
    f.tracker.turn_begin(0);
    let op = f.create(Kind::Operation, "kv_get");
    assert_eq!(f.tracker.adopt_operation(), op);
    f.tracker.turn_end();
    let _ = f.events();

    f.tracker.settle(op, Outcome::Ok);
    f.tracker.destroy(op); // The creator's handle.
    f.tracker.turn_begin(0);
    f.tracker.set_turn_cause(op); // The bridge's handle.
    f.tracker.turn_end();
    f.tracker.destroy(op);

    let events = f.events();
    assert_eq!(
        events[0],
        Event::Settle {
            ctx,
            id: op,
            outcome: Outcome::Ok
        }
    );
    assert_eq!(events[1], Event::Before { ctx, id: op });
    assert_eq!(events[2], Event::After { ctx, id: op });
    // The last holder's release comes after the turn.
    assert_eq!(events[4], Event::Release { ctx, id: op });
    assert_eq!(events.len(), 5, "{events:?}");
    assert_eq!(f.tracker.stats().unknown, 0);

    // Both handles are gone.
    f.tracker.destroy(op);
    assert_eq!(f.tracker.stats().unknown, 1);
}

#[test]
fn unsettled_adopted_operation_is_destroyed_once_by_its_last_holder() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    f.tracker.turn_begin(0);
    let op = f.create(Kind::Operation, "kv_get");
    assert_eq!(f.tracker.adopt_operation(), op);
    f.tracker.turn_end();
    let _ = f.events();

    f.tracker.destroy(op);
    assert!(f.events().is_empty());
    f.tracker.destroy(op);
    assert_eq!(
        f.events(),
        vec![
            Event::Destroy { ctx, id: op },
            Event::Release { ctx, id: op }
        ]
    );
    assert_eq!(f.tracker.stats().unknown, 0);
}

#[test]
fn unused_default_cause_reports_only_the_turn() {
    let mut f = Fixture::new();
    let request = f.create(Kind::Request, "fetch");
    let _ = f.events();
    f.tracker.turn_begin(request);
    assert_eq!(f.tracker.current(), request);
    f.tracker.turn_end();
    let events = f.events();
    assert_eq!(events.len(), 1, "{events:?}");
    assert!(matches!(&events[0], Event::Turn { turn, .. } if turn.cause == request));
}

#[test]
fn explicit_cause_after_the_default_was_entered_closes_it() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let request = f.create(Kind::Request, "fetch");
    let bridge = f.create(Kind::KjToJs, "bridge");
    let _ = f.events();
    f.tracker.turn_begin(request);
    let timer = f.create(Kind::Timer, "setTimeout");
    f.tracker.set_turn_cause(bridge);
    f.tracker.turn_end();
    let events = f.events();
    assert_eq!(events[0], Event::Before { ctx, id: request });
    assert_eq!(init_of(&events, timer).1, request);
    assert_eq!(events[2], Event::After { ctx, id: request });
    assert_eq!(events[3], Event::Before { ctx, id: bridge });
    assert_eq!(events[4], Event::After { ctx, id: bridge });
    assert_eq!(f.tracker.stats().unbalanced, 0);
}

#[test]
fn nested_turn_enters_the_outer_default_first() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let request = f.create(Kind::Request, "fetch");
    let _ = f.events();
    f.tracker.turn_begin(request);
    f.tracker.turn_begin(0);
    f.tracker.turn_end();
    f.tracker.turn_end();
    let events = f.events();
    assert_eq!(events[0], Event::Before { ctx, id: request });
    assert!(matches!(&events[1], Event::Turn { turn, .. } if turn.cause == 0));
    assert_eq!(events[2], Event::After { ctx, id: request });
    assert_eq!(f.tracker.stats().unbalanced, 0);
}

#[test]
fn accepts_resources_until_full_or_closed() {
    let mut f = Fixture::new();
    f.tracker.set_max_live(1);
    assert!(f.tracker.accepts_resources());
    f.create(Kind::Timer, "fills the tracker");
    assert!(!f.tracker.accepts_resources());
    let mut f = Fixture::new();
    f.tracker.close();
    assert!(!f.tracker.accepts_resources());
}

#[test]
fn unowned_resources_are_forgotten_when_settled() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    f.tracker.turn_begin(0);
    let promise = f.tracker.create_unowned(Kind::JsPromise, "Promise", 0);
    assert!(f.tracker.knows(promise));
    // A reaction promise settles during its own callback; the callback still reports `after`.
    f.tracker.enter(promise);
    f.tracker.settle(promise, Outcome::Ok);
    assert!(!f.tracker.knows(promise));
    f.tracker.exit(promise);
    f.tracker.turn_end();
    let events = f.events();
    assert!(events.contains(&Event::Before { ctx, id: promise }));
    assert!(events.contains(&Event::After { ctx, id: promise }));
    assert!(!events.iter().any(|e| matches!(e, Event::Destroy { .. })));
    assert_eq!(f.tracker.stats().unknown, 0);
    assert_eq!(f.tracker.stats().unbalanced, 0);
}

#[test]
fn link_source_is_the_turns_latest_operation_or_the_running_resource() {
    let mut f = Fixture::new();
    assert_eq!(f.tracker.link_source(), 0, "outside a turn");
    let timer = f.create(Kind::Timer, "setTimeout");
    f.tracker.turn_begin(0);
    let op = f.create(Kind::Operation, "fetch");
    // A bound (e.g. detached) operation still identifies the call.
    f.tracker.mark_bound(op);
    assert_eq!(f.tracker.link_source(), op);
    f.tracker.turn_end();
    // An operation from an earlier turn doesn't count; the running resource does.
    f.tracker.turn_begin(timer);
    assert_eq!(f.tracker.link_source(), timer);
    f.tracker.turn_end();
}

#[test]
fn link_reports_the_other_contexts_resource() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let request = f.create(Kind::Request, "fetch");
    let from = Link {
        isolate: 7,
        ctx: 3,
        id: 42,
    };
    f.tracker.link(request, from);
    f.tracker.link(request, Link { id: 0, ..from });
    f.tracker.link(request + 1000, from);
    let events = f.events();
    let links: Vec<_> = events
        .iter()
        .filter(|e| matches!(e, Event::Link { .. }))
        .collect();
    assert_eq!(
        links,
        vec![&Event::Link {
            ctx,
            id: request,
            from
        }]
    );
    assert_eq!(f.tracker.stats().unknown, 1);
}

#[test]
fn a_child_operation_names_its_parent() {
    let mut f = Fixture::new();
    let outer = f.create(Kind::Operation, "fetch");
    let inner = f
        .tracker
        .create_child(Kind::Operation, "fetch_attempt", 0, None, outer);
    let events = f.events();
    assert!(
        events
            .iter()
            .any(|e| matches!(e, Event::Init { id, parent, .. } if *id == outer && *parent == 0))
    );
    assert!(
        events.iter().any(
            |e| matches!(e, Event::Init { id, parent, .. } if *id == inner && *parent == outer)
        )
    );
}

#[test]
fn a_resource_settled_without_a_callback_is_still_released() {
    // For example, a detached span's operation: it settles, no bridge runs it, and its handle is
    // then dropped. Sinks that keep per-resource state rely on the release.
    let mut f = Fixture::new();
    let ctx = f.ctx();
    f.tracker.turn_begin(0);
    let op = f.create(Kind::Operation, "r2_put");
    f.tracker.mark_bound(op);
    f.tracker.turn_end();
    let _ = f.events();
    f.tracker.settle(op, Outcome::Ok);
    f.tracker.destroy(op);
    assert_eq!(
        f.events(),
        vec![
            Event::Settle {
                ctx,
                id: op,
                outcome: Outcome::Ok
            },
            Event::Release { ctx, id: op },
        ]
    );
}

#[test]
fn an_unowned_resource_is_released_when_it_settles() {
    let mut f = Fixture::new();
    let ctx = f.ctx();
    let promise = f.tracker.create_unowned(Kind::JsPromise, "Promise", 0);
    let _ = f.events();
    f.tracker.settle(promise, Outcome::Ok);
    assert_eq!(
        f.events(),
        vec![
            Event::Settle {
                ctx,
                id: promise,
                outcome: Outcome::Ok
            },
            Event::Release { ctx, id: promise },
        ]
    );
}
