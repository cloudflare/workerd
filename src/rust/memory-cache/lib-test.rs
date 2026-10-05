#![expect(
    clippy::significant_drop_tightening,
    reason = "tests keep decisions and permits alive to exercise ownership transitions"
)]

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::Barrier;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering as WakeOrdering;
use std::task::Context;
use std::task::Wake;
use std::task::Waker;
use std::thread;

use super::*;

struct CountingWake(AtomicUsize);

impl Wake for CountingWake {
    fn wake(self: Arc<Self>) {
        self.0.fetch_add(1, WakeOrdering::Relaxed);
    }
}

fn test_namespace(max_total_value_size: Option<u64>) -> Namespace {
    Namespace::new(max_total_value_size)
}

fn read(
    binding: &Binding,
    key: &str,
    now_ms: f64,
    with_fallback: bool,
) -> Result<ReadDecision, CacheError> {
    binding.read(
        key.as_bytes(),
        now_ms,
        if with_fallback {
            ReadMode::WithFallback
        } else {
            ReadMode::CacheOnly
        },
    )
}

fn release(binding: &mut Binding, now_ms: f64) {
    binding.release(now_ms);
}

fn delete(binding: &Binding, key: &str) {
    binding.delete(key.as_bytes());
}

fn stats(binding: &Binding) -> Stats {
    binding.stats()
}

fn fallback_succeed(
    permit: FallbackPermit,
    bytes: Vec<u8>,
    has_expiration: bool,
    expiration: f64,
    now_ms: f64,
) -> Result<WriteTrace, CacheError> {
    permit.succeed(bytes, has_expiration.then_some(expiration), now_ms)
}

fn limits(keys: u32, value: u32, total: u64) -> Limits {
    Limits {
        max_keys: keys,
        max_value_size: value,
        max_total_value_size: total,
    }
}

fn binding(namespace: &Namespace, id: &str, limits: Limits) -> Binding {
    namespace.bind(Some(id), limits).unwrap()
}

fn poll(waiter: &mut Waiter) -> Poll<WaitOutcome> {
    let mut context = Context::from_waker(Waker::noop());
    Pin::new(waiter).poll(&mut context)
}

fn poll_with_waker(waiter: &mut Waiter, wake: &Arc<CountingWake>) -> Poll<WaitOutcome> {
    let waker = Arc::clone(wake).into();
    let mut context = Context::from_waker(&waker);
    Pin::new(waiter).poll(&mut context)
}

fn leader(binding: &Binding, key: &str) -> FallbackPermit {
    let mut decision = read(binding, key, 1.0, true).unwrap();
    assert_eq!(decision.kind(), ReadKind::Leader);
    decision.take_permit().unwrap()
}

fn put(binding: &Binding, key: &str, value: Vec<u8>, expiration: Option<f64>, now: f64) {
    fallback_succeed(
        leader(binding, key),
        value,
        expiration.is_some(),
        expiration.unwrap_or_default(),
        now,
    )
    .unwrap();
}

fn assert_consistent(binding: &Binding) {
    let state = lock(&binding.cache.state);
    assert_eq!(
        state.expirations.len(),
        state
            .entries
            .values()
            .filter(|entry| entry.expiration.is_some())
            .count()
    );
    assert_eq!(
        state.total_value_size,
        state
            .entries
            .values()
            .map(|entry| entry.value.len())
            .sum::<usize>()
    );

    for (key, entry) in &state.entries {
        if let Some(expiration) = entry.expiration {
            assert!(state.expirations.contains(&ExpirationRecord {
                expiration,
                key: Arc::clone(key),
            }));
        }
    }
    for record in &state.expirations {
        let entry = &state.entries[&record.key];
        assert_eq!(Some(record.expiration), entry.expiration);
    }

    let mut waiter_count = 0;
    for (key, in_flight) in &state.in_flight_fallbacks {
        let in_flight = in_flight
            .upgrade()
            .unwrap_or_else(|| unreachable!("dead fallback remained discoverable"));
        assert_eq!(&**key, &*in_flight.key);
        waiter_count += in_flight.waiters.load(AtomicOrdering::Relaxed);
    }
    assert_eq!(
        binding.cache.live_waiters.load(AtomicOrdering::Relaxed),
        waiter_count
    );

    let mut expected_limits = Limits::default();
    for limits in state.bindings.values() {
        expected_limits.max_keys = expected_limits.max_keys.max(limits.max_keys);
        expected_limits.max_value_size = expected_limits.max_value_size.max(limits.max_value_size);
        expected_limits.max_total_value_size = expected_limits
            .max_total_value_size
            .max(limits.max_total_value_size);
    }
    if let Some(cap) = binding.namespace.max_total_value_size {
        expected_limits.max_total_value_size = expected_limits.max_total_value_size.min(cap);
    }
    if expected_limits.max_keys == 0
        || expected_limits.max_value_size == 0
        || expected_limits.max_total_value_size == 0
    {
        expected_limits = Limits::default();
    } else {
        expected_limits.max_value_size = expected_limits.max_value_size.min(
            expected_limits
                .max_total_value_size
                .try_into()
                .unwrap_or(u32::MAX),
        );
    }
    assert_eq!(state.effective_limits, expected_limits);
    assert!(state.entries.len() <= state.effective_limits.max_keys as usize);
    assert!(state.total_value_size <= state.effective_limits.max_total_value_size as usize);
    assert!(
        state
            .entries
            .values()
            .all(|entry| entry.value.len() <= state.effective_limits.max_value_size as usize)
    );
}

fn insert_entry_for_test(binding: &Binding, key: &str, value: Vec<u8>, expiration: Option<f64>) {
    let mut state = lock(&binding.cache.state);
    state
        .insert_entry(
            Arc::from(key.as_bytes()),
            Entry {
                value: Bytes::from(value),
                expiration,
            },
        )
        .unwrap();
}

#[test]
fn named_sharing_private_isolation_and_explicit_teardown() {
    let namespace = test_namespace(None);
    let named_a = binding(&namespace, "shared", limits(2, 8, 16));
    let named_b = binding(&namespace, "shared", limits(4, 4, 32));
    let private = namespace.bind(None, limits(2, 8, 16)).unwrap();
    assert_eq!(stats(&named_a).bindings, 2);
    assert_eq!(stats(&private).bindings, 1);
    assert_eq!(stats(&named_a).limits, limits(4, 8, 32));
    drop(named_b);
    assert_eq!(stats(&named_a).bindings, 1);
    drop(namespace);
    assert_eq!(stats(&named_a).bindings, 1);
}

#[test]
fn named_cache_drop_removes_weak_namespace_entry() {
    let namespace = test_namespace(None);
    let named = binding(&namespace, "shared", limits(2, 8, 16));
    let cache = Arc::downgrade(&named.cache);
    assert_eq!(lock(&namespace.inner.state).named.len(), 1);

    drop(named);

    assert!(cache.upgrade().is_none());
    assert!(lock(&namespace.inner.state).named.is_empty());
}

#[test]
fn deterministic_operations_preserve_model_and_internal_indexes() {
    let namespace = test_namespace(None);
    let binding = binding(&namespace, "shared", limits(16, 8, 128));
    let mut model: HashMap<String, (Vec<u8>, Option<f64>)> = HashMap::new();
    let mut seed = 0x6a09_e667_f3bc_c909_u64;
    let mut now = 1.0;
    let mut operation_counts = [0; 4];
    let mut model_hits = 0;
    let mut expiration_reads = 0;

    for step in 0..10_000 {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        let key = format!("key-{}", (seed >> 32) % 8);
        let operation = (seed % 4) as usize;
        operation_counts[operation] += 1;
        match operation {
            0 | 1 => {
                delete(&binding, &key);
                model.remove(&key);
                let value = vec![(seed >> 8) as u8; (seed as usize % 8) + 1];
                let expiration = match (seed >> 16) % 4 {
                    0 => None,
                    1 => Some(now - 1.0),
                    2 => Some(now),
                    _ => Some(now + 5.0),
                };
                let trace = fallback_succeed(
                    leader(&binding, &key),
                    value.clone(),
                    expiration.is_some(),
                    expiration.unwrap_or_default(),
                    now,
                )
                .unwrap();
                if expiration.is_some_and(|expiration| expiration < now) {
                    assert_eq!(trace.outcome, WriteOutcome::AlreadyExpired);
                } else {
                    assert!(matches!(trace.outcome, WriteOutcome::Success { .. }));
                    model.insert(key, (value, expiration));
                }
            }
            2 => {
                if model
                    .get(&key)
                    .is_some_and(|(_, expiration)| expiration.is_some_and(|value| value < now))
                {
                    model.remove(&key);
                    expiration_reads += 1;
                }
                let mut decision = read(&binding, &key, now, false).unwrap();
                if let Some((expected, _)) = model.get(&key) {
                    model_hits += 1;
                    assert_eq!(decision.kind(), ReadKind::Value);
                    assert_eq!(decision.take_value().unwrap().bytes(), expected);
                } else {
                    assert_eq!(decision.kind(), ReadKind::Miss);
                }
            }
            _ => {
                delete(&binding, &key);
                model.remove(&key);
            }
        }
        if step % 17 == 0 {
            now += 1.0;
        }
        if step % 101 == 0 {
            let temporary = self::binding(&namespace, "shared", limits(4, 4, 16));
            assert_eq!(stats(&temporary).bindings, 2);
            drop(temporary);
        }
        assert_consistent(&binding);
    }
    assert!(operation_counts.into_iter().all(|count| count > 2_000));
    assert!(model_hits > 100);
    assert!(expiration_reads > 10);
}

#[test]
fn limits_expiration_rejections_and_decision_errors_cover_boundaries() {
    let namespace = test_namespace(None);
    let binding = binding(&namespace, "shared", limits(2, 4, 6));
    put(&binding, "a", vec![1; 4], None, 1.0);
    put(&binding, "b", vec![2; 2], Some(5.0), 1.0);
    assert_consistent(&binding);

    let mut equal_expiration = read(&binding, "b", 5.0, false).unwrap();
    assert_eq!(equal_expiration.kind(), ReadKind::Value);
    assert_eq!(equal_expiration.take_value().unwrap().bytes(), [2; 2]);
    assert!(matches!(
        equal_expiration.take_value(),
        Err(CacheError::InvalidDecision(_))
    ));
    assert_eq!(
        read(&binding, "b", 5.1, false).unwrap().kind(),
        ReadKind::Miss
    );

    put(&binding, "b", vec![2; 2], None, 6.0);
    put(&binding, "c", vec![3], None, 6.0);
    assert_eq!(
        read(&binding, "a", 6.0, false).unwrap().kind(),
        ReadKind::Miss
    );
    assert_eq!(
        read(&binding, "b", 6.0, false).unwrap().kind(),
        ReadKind::Value
    );
    assert_eq!(
        read(&binding, "c", 6.0, false).unwrap().kind(),
        ReadKind::Value
    );
    assert_consistent(&binding);

    let oversized = leader(&binding, "oversized");
    insert_entry_for_test(&binding, "oversized", vec![9], None);
    let trace = fallback_succeed(oversized, vec![9; 5], false, 0.0, 6.0).unwrap();
    assert!(matches!(trace.outcome, WriteOutcome::ValueTooLarge { .. }));
    assert_eq!(
        read(&binding, "oversized", 6.0, false).unwrap().kind(),
        ReadKind::Miss
    );
    assert_consistent(&binding);

    let expired = leader(&binding, "expired");
    insert_entry_for_test(&binding, "expired", vec![8], None);
    let trace = fallback_succeed(expired, vec![8], true, 5.0, 6.0).unwrap();
    assert_eq!(trace.outcome, WriteOutcome::AlreadyExpired);
    assert_eq!(
        read(&binding, "expired", 6.0, false).unwrap().kind(),
        ReadKind::Miss
    );
    assert_consistent(&binding);

    let capped_namespace = test_namespace(Some(6));
    let capped = self::binding(&capped_namespace, "capped", limits(4, 10, 10));
    assert_eq!(stats(&capped).limits, limits(4, 6, 6));
    put(&capped, "exact", vec![1; 6], None, 1.0);
    let rejected =
        fallback_succeed(leader(&capped, "too-large"), vec![1; 7], false, 0.0, 1.0).unwrap();
    assert!(matches!(
        rejected.outcome,
        WriteOutcome::ValueTooLarge { .. }
    ));
    assert_consistent(&capped);

    let disabled_namespace = test_namespace(None);
    let disabled = self::binding(&disabled_namespace, "disabled", Limits::default());
    let rejected = fallback_succeed(leader(&disabled, "key"), vec![1], false, 0.0, 1.0).unwrap();
    assert!(matches!(
        rejected.outcome,
        WriteOutcome::ValueTooLarge { .. }
    ));
    assert_consistent(&disabled);
}

#[test]
fn fallback_transition_matrix_cleans_up() {
    let namespace = test_namespace(None);
    let binding = binding(&namespace, "shared", limits(8, 32, 128));

    let permit = leader(&binding, "success");
    let mut first = read(&binding, "success", 1.0, true)
        .unwrap()
        .take_waiter()
        .unwrap();
    let mut second = read(&binding, "success", 1.0, true)
        .unwrap()
        .take_waiter()
        .unwrap();
    assert!(poll(&mut first).is_pending());
    assert!(poll(&mut second).is_pending());
    fallback_succeed(permit, vec![1, 2, 3], false, 0.0, 1.0).unwrap();
    for waiter in [&mut first, &mut second] {
        let Poll::Ready(mut outcome) = poll(waiter) else {
            panic!("successful fallback did not notify waiter");
        };
        assert_eq!(outcome.kind(), WaitKind::Value);
        assert_eq!(outcome.take_value().unwrap().bytes(), [1, 2, 3]);
    }
    assert_consistent(&binding);

    let permit = leader(&binding, "failure");
    let mut first = read(&binding, "failure", 1.0, true)
        .unwrap()
        .take_waiter()
        .unwrap();
    let mut second = read(&binding, "failure", 1.0, true)
        .unwrap()
        .take_waiter()
        .unwrap();
    assert!(poll(&mut first).is_pending());
    assert!(poll(&mut second).is_pending());
    drop(permit);
    let Poll::Ready(mut promoted) = poll(&mut first) else {
        panic!("first waiter was not promoted");
    };
    assert_eq!(promoted.kind(), WaitKind::Leader);
    drop(promoted.take_permit().unwrap());
    let Poll::Ready(mut promoted) = poll(&mut second) else {
        panic!("second waiter was not promoted");
    };
    assert_eq!(promoted.kind(), WaitKind::Leader);
    drop(promoted.take_permit().unwrap());
    assert_eq!(stats(&binding).in_flight_fallbacks, 0);
    assert_consistent(&binding);
}

#[test]
fn successful_completion_broadcasts_to_all_waiters() {
    const COUNT: usize = 64;
    let namespace = test_namespace(None);
    let binding = binding(&namespace, "shared", limits(4, 32, 64));
    let permit = leader(&binding, "key");
    let mut waiters = Vec::with_capacity(COUNT);
    let mut wakes = Vec::with_capacity(COUNT);
    for _ in 0..COUNT {
        let mut waiter = read(&binding, "key", 1.0, true)
            .unwrap()
            .take_waiter()
            .unwrap();
        let wake = Arc::new(CountingWake(AtomicUsize::new(0)));
        assert!(poll_with_waker(&mut waiter, &wake).is_pending());
        waiters.push(waiter);
        wakes.push(wake);
    }
    let mut late = read(&binding, "key", 1.0, true)
        .unwrap()
        .take_waiter()
        .unwrap();
    let value = vec![1, 2, 3];
    let allocation = value.as_ptr();

    fallback_succeed(permit, value, false, 0.0, 1.0).unwrap();

    assert!(
        wakes
            .iter()
            .all(|wake| wake.0.load(WakeOrdering::Relaxed) > 0)
    );
    for mut waiter in waiters {
        let Poll::Ready(mut outcome) = poll(&mut waiter) else {
            panic!("broadcast waiter remained pending");
        };
        assert_eq!(outcome.kind(), WaitKind::Value);
        assert_eq!(outcome.take_value().unwrap().bytes().as_ptr(), allocation);
    }
    let Poll::Ready(mut outcome) = poll(&mut late) else {
        panic!("waiter first polled after completion remained pending");
    };
    assert_eq!(outcome.take_value().unwrap().bytes().as_ptr(), allocation);
}

#[test]
fn fallback_success_retains_the_input_allocation() {
    let namespace = test_namespace(None);
    let binding = binding(&namespace, "shared", limits(4, 32, 64));
    let value = vec![1, 2, 3, 4];
    let allocation = value.as_ptr();

    fallback_succeed(leader(&binding, "key"), value, false, 0.0, 1.0).unwrap();

    let mut hit = read(&binding, "key", 1.0, false).unwrap();
    assert_eq!(hit.take_value().unwrap().bytes().as_ptr(), allocation);
}

#[test]
fn repeated_completion_cancellation_races_preserve_invariants() {
    let namespace = test_namespace(None);
    let binding = Arc::new(binding(&namespace, "shared", limits(128, 32, 4096)));
    for round in 0..100 {
        let key = format!("race-{round}");
        let permit = leader(&binding, &key);
        let mut waiters = Vec::new();
        for _ in 0..64 {
            waiters.push(
                read(&binding, &key, 1.0, true)
                    .unwrap()
                    .take_waiter()
                    .unwrap(),
            );
        }
        let canceled_before = stats(&binding).canceled_waiters;
        let barrier = Arc::new(Barrier::new(3));
        let complete_barrier = Arc::clone(&barrier);
        let complete = thread::spawn(move || {
            complete_barrier.wait();
            fallback_succeed(permit, vec![1, 2, 3], false, 0.0, 1.0).unwrap()
        });
        let cancel_barrier = Arc::clone(&barrier);
        let cancel = thread::spawn(move || {
            cancel_barrier.wait();
            drop(waiters);
        });
        barrier.wait();
        let trace = complete.join().unwrap();
        cancel.join().unwrap();
        let canceled_after = stats(&binding).canceled_waiters;
        assert!(trace.waiters_notified <= 64);
        assert!(canceled_after - canceled_before <= 64);
        let stats = stats(&binding);
        assert_eq!(stats.in_flight_fallbacks, 0);
        assert_eq!(stats.waiters, 0);
        assert_consistent(&binding);
    }
}

#[test]
fn fifty_thousand_reverse_and_random_cancellations_clean_up() {
    const COUNT: usize = 50_000;
    let namespace = test_namespace(None);
    let binding = binding(&namespace, "shared", limits(4, 32, 64));
    let permit = leader(&binding, "key");
    let mut waiters = Vec::with_capacity(COUNT);
    for _ in 0..COUNT {
        waiters.push(
            read(&binding, "key", 1.0, true)
                .unwrap()
                .take_waiter()
                .unwrap(),
        );
    }
    for index in (COUNT / 2..COUNT).rev() {
        drop(waiters.swap_remove(index));
    }
    let mut seed = 0x9e37_79b9_u64;
    while !waiters.is_empty() {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        let index = seed as usize % waiters.len();
        drop(waiters.swap_remove(index));
    }
    let stats = stats(&binding);
    assert_eq!(stats.waiters, 0);
    assert_eq!(stats.canceled_waiters, COUNT);
    drop(permit);
}

#[test]
fn abandonment_promotes_a_live_waiter_and_skips_canceled_waiters() {
    let namespace = test_namespace(None);
    let binding = binding(&namespace, "shared", limits(4, 32, 64));
    let first = leader(&binding, "key");
    let mut canceled = read(&binding, "key", 1.0, true)
        .unwrap()
        .take_waiter()
        .unwrap();
    let mut next = read(&binding, "key", 1.0, true)
        .unwrap()
        .take_waiter()
        .unwrap();
    assert!(poll(&mut canceled).is_pending());
    assert!(poll(&mut next).is_pending());
    drop(canceled);
    drop(first);
    let Poll::Ready(promoted) = poll(&mut next) else {
        panic!("live waiter was not promoted");
    };
    assert_eq!(promoted.kind(), WaitKind::Leader);
}

#[test]
fn poll_order_controls_promotion_and_post_failure_cancellation_cleans_up() {
    let namespace = test_namespace(None);
    let binding = binding(&namespace, "shared", limits(4, 32, 64));
    let permit = leader(&binding, "fifo");
    let mut first = read(&binding, "fifo", 1.0, true)
        .unwrap()
        .take_waiter()
        .unwrap();
    let mut second = read(&binding, "fifo", 1.0, true)
        .unwrap()
        .take_waiter()
        .unwrap();

    assert!(poll(&mut second).is_pending());
    assert!(poll(&mut first).is_pending());
    drop(permit);
    assert!(poll(&mut first).is_pending());
    let Poll::Ready(mut promoted) = poll(&mut second) else {
        panic!("first polled waiter was not promoted");
    };
    drop(promoted.take_permit().unwrap());
    drop(first);
    assert_eq!(stats(&binding).in_flight_fallbacks, 0);

    let permit = leader(&binding, "cleanup");
    let waiter = read(&binding, "cleanup", 1.0, true)
        .unwrap()
        .take_waiter()
        .unwrap();
    drop(permit);
    drop(waiter);
    assert_eq!(stats(&binding).in_flight_fallbacks, 0);
}

#[test]
fn read_during_abandonment_becomes_leader() {
    let namespace = test_namespace(None);
    let binding = binding(&namespace, "shared", limits(4, 32, 64));
    let permit = leader(&binding, "key");
    let in_flight = Arc::clone(&permit.in_flight);

    drop(permit);
    let mut waiter = read(&binding, "key", 1.0, true)
        .unwrap()
        .take_waiter()
        .unwrap();
    let Poll::Ready(mut outcome) = poll(&mut waiter) else {
        panic!("waiter did not acquire the abandoned fallback");
    };
    let replacement = outcome.take_permit().unwrap();

    assert!(Arc::ptr_eq(&in_flight, &replacement.in_flight));
    drop(in_flight);
    drop(replacement);
    assert_eq!(stats(&binding).in_flight_fallbacks, 0);
}

#[test]
fn successful_fanout_shares_arc_and_races_with_cancellation() {
    let namespace = test_namespace(None);
    let binding = Arc::new(binding(&namespace, "shared", limits(4, 32, 64)));
    let permit = leader(&binding, "key");
    let mut waiters = Vec::new();
    for _ in 0..1000 {
        waiters.push(
            read(&binding, "key", 1.0, true)
                .unwrap()
                .take_waiter()
                .unwrap(),
        );
    }
    let cancel = thread::spawn(move || drop(waiters));
    let trace = fallback_succeed(permit, vec![1, 2, 3], false, 0.0, 1.0).unwrap();
    cancel.join().unwrap();
    assert!(trace.waiters_notified <= 1000);
    assert_eq!(stats(&binding).waiters, 0);
    let mut hit = read(&binding, "key", 2.0, false).unwrap();
    assert_eq!(hit.take_value().unwrap().bytes(), [1, 2, 3]);
}

#[test]
fn indexed_eviction_and_large_limit_reduction_preserve_order() {
    let namespace = test_namespace(None);
    let mut large_binding = binding(&namespace, "shared", limits(10_000, 64, 640_000));
    for index in 0..5000 {
        let expiration = (index % 10 == 0).then_some(10.0 + f64::from(index));
        put(
            &large_binding,
            &format!("key-{index:05}"),
            vec![0; 32],
            expiration,
            1.0,
        );
    }
    let small = binding(&namespace, "shared", limits(10, 8, 80));
    release(&mut large_binding, 100_000.0);
    let stats = stats(&small);
    assert!(stats.entries <= 10);
    assert!(stats.limits.max_value_size >= 8);
    drop(small);
}

#[test]
fn expired_entries_win_before_lru_and_ties_use_key() {
    let namespace = test_namespace(None);
    let binding = binding(&namespace, "shared", limits(2, 8, 16));
    put(&binding, "permanent", vec![1], None, 1.0);
    put(&binding, "expired-b", vec![2], Some(2.0), 1.0);
    let trace = fallback_succeed(leader(&binding, "incoming"), vec![3], false, 0.0, 3.0).unwrap();
    assert_eq!(trace.evictions[0].reason, EvictionReason::Expiration);
    assert_eq!(&*trace.evictions[0].key, b"expired-b");
}

#[test]
fn reads_refresh_lru_order() {
    let namespace = test_namespace(None);
    let binding = binding(&namespace, "shared", limits(2, 8, 16));
    put(&binding, "a", vec![1], None, 1.0);
    put(&binding, "b", vec![2], None, 1.0);
    read(&binding, "a", 2.0, false).unwrap();

    let trace = fallback_succeed(leader(&binding, "c"), vec![3], false, 0.0, 2.0).unwrap();

    assert_eq!(trace.evictions[0].reason, EvictionReason::Lru);
    assert_eq!(&*trace.evictions[0].key, b"b");
}

#[test]
fn stale_fallback_cannot_mutate_replacement() {
    let namespace = test_namespace(None);
    let binding = binding(&namespace, "shared", limits(4, 8, 32));
    let stale = leader(&binding, "key");
    {
        let mut cache_state = lock(&binding.cache.state);
        cache_state.remove_in_flight_fallback(b"key");
    }
    let replacement = leader(&binding, "key");
    fallback_succeed(stale, vec![1], false, 0.0, 1.0).unwrap();
    assert_eq!(stats(&binding).in_flight_fallbacks, 1);
    assert_eq!(stats(&binding).entries, 0);
    drop(replacement);
}

#[test]
fn binding_teardown_during_fallback_keeps_fallback_and_clears_entries() {
    let namespace = test_namespace(None);
    let mut original = binding(&namespace, "shared", limits(2, 8, 16));
    put(&original, "stored", vec![1], None, 1.0);
    let permit = leader(&original, "in_flight");
    release(&mut original, 2.0);
    let replacement = binding(&namespace, "shared", limits(2, 8, 16));
    assert_eq!(stats(&replacement).in_flight_fallbacks, 1);
    assert_eq!(stats(&replacement).entries, 0);
    drop(permit);
}
