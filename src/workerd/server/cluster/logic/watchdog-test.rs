// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

/// 1 for production's times; the others as tests divide them.
const DIVISORS: [u64; 4] = [1, 2, 4, 10];

fn timings() -> [Timing; 4] {
    DIVISORS.map(|d| Timing::divided(d).unwrap())
}

#[test]
fn a_count_that_advances_never_fences() {
    for t in timings() {
        let mut watchdog = Watchdog::new(&t);
        for round in 0..1_000 {
            assert_eq!(watchdog.observe(round * t.stall_ms, round), None, "{t:?}");
        }
    }
}

#[test]
fn a_count_that_stands_still_fences_once_it_has_for_the_stall() {
    for t in timings() {
        let mut watchdog = Watchdog::new(&t);
        let start = 1_000_000;
        assert_eq!(watchdog.observe(start, 7), None);
        assert_eq!(watchdog.observe(start + t.stall_ms - 1, 7), None, "{t:?}");
        let reason = watchdog.observe(start + t.stall_ms, 7).unwrap();
        assert_eq!(
            reason,
            format!("its event loop made no progress for {} ms", t.stall_ms)
        );
    }
}

#[test]
fn the_first_observation_starts_the_clock() {
    for t in timings() {
        let mut watchdog = Watchdog::new(&t);
        // However late the keeper first looks.
        assert_eq!(watchdog.observe(10 * t.stall_ms, 0), None, "{t:?}");
        assert_eq!(watchdog.observe(11 * t.stall_ms - 1, 0), None, "{t:?}");
        assert!(watchdog.observe(11 * t.stall_ms, 0).is_some(), "{t:?}");
    }
}

#[test]
fn a_count_that_moves_again_starts_the_clock_again() {
    for t in timings() {
        let mut watchdog = Watchdog::new(&t);
        assert_eq!(watchdog.observe(0, 1), None);
        assert_eq!(watchdog.observe(t.stall_ms - 1, 1), None);
        assert_eq!(watchdog.observe(t.stall_ms, 2), None, "{t:?}");
        assert_eq!(watchdog.observe(2 * t.stall_ms - 1, 2), None, "{t:?}");
        assert!(watchdog.observe(2 * t.stall_ms, 2).is_some(), "{t:?}");
    }
}

/// An idle loop advances its count once a beat (`interval_ms`), late by up
/// to a beat; the keeper reads it once a round, as often as every
/// `retry_ms`, and may skip rounds. It never looks stuck.
#[test]
fn an_idle_loop_beating_once_a_round_never_fences() {
    for t in timings() {
        let mut watchdog = Watchdog::new(&t);
        for step in 0..10_000 {
            let now = step * t.retry_ms;
            let count = now.saturating_sub(t.interval_ms) / t.interval_ms;
            if step % 3 != 0 {
                assert_eq!(watchdog.observe(now, count), None, "{t:?}");
            }
        }
    }
}
