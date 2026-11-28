// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

/// 1 for production's times; the others as tests divide them.
const DIVISORS: [u64; 4] = [1, 2, 4, 10];

fn timings() -> [Timing; 4] {
    DIVISORS.map(|d| Timing::divided(d).unwrap())
}

fn seen(now_ms: u64) -> Observed {
    Observed {
        now_ms,
        skew_ms: Some(0),
    }
}

#[test]
fn divided_times_keep_their_proportions() {
    let t = Timing::DEFAULT;
    for (d, divided) in DIVISORS.into_iter().zip(timings()) {
        assert_eq!(divided.times().map(|ms| ms * d), t.times(), "{divided:?}");
        assert_eq!(divided.max_skew_ms, t.max_skew_ms, "{divided:?}");
        // A round renews before the lease needs to fence.
        let round = divided.interval_ms + divided.renew_timeout_ms;
        assert!(round + divided.safety_ms < divided.lease_ms, "{divided:?}");
        // A turn of the loop that outlasts the lease keeps it, and the
        // node: the watchdog waits out a longer stall than that.
        assert!(divided.stall_ms >= 3 * divided.lease_ms, "{divided:?}");
        // An idle loop beats once a round, which the keeper reads once a
        // round: either late by a round leaves it far from a stall.
        assert!(4 * divided.interval_ms < divided.stall_ms, "{divided:?}");
    }
}

#[test]
fn every_divided_timing_sends_a_stalled_request_again_within_its_round() {
    // Every divisor up to the shortest time, which rounds it to 1 ms; none
    // past it (`a_divisor_that_rounds_a_time_to_zero_is_refused`).
    let shortest = Timing::DEFAULT.times().into_iter().min().unwrap();
    for t in (1..=shortest).map(|d| Timing::divided(d).unwrap()) {
        assert!(t.renew_attempt_ms < t.renew_timeout_ms, "{t:?}");
        assert!(t.list_attempt_ms < t.list_timeout_ms, "{t:?}");
    }
}

#[test]
fn a_divisor_that_rounds_a_time_to_zero_is_refused() {
    let shortest = Timing::DEFAULT.times().into_iter().min().unwrap();
    assert_eq!(Timing::divided(0), None);
    assert!(Timing::divided(shortest).is_some());
    assert_eq!(Timing::divided(shortest + 1), None);
}

#[test]
fn renews_and_holds_until_the_safety_margin() {
    for t in timings() {
        let mut lease = Lease::new(t);
        assert_eq!(lease.before_renew(&seen(0)), Step::Renew);
        assert_eq!(lease.renewed(0, Ok(t.lease_ms)), Step::Renew);
        let last = t.lease_ms - t.safety_ms;
        assert!(lease.holds(last - 1) && !lease.holds(last), "{t:?}");
    }
}

#[test]
fn a_failed_renewal_retries_until_the_lease_nearly_lapses() {
    for t in timings() {
        let mut lease = Lease::new(t);
        lease.renewed(0, Ok(t.lease_ms));
        assert!(matches!(
            lease.renewed(t.lease_ms / 4, Err("x".into())),
            Step::Retry(_)
        ));
        let last = t.lease_ms - t.safety_ms - t.renew_timeout_ms;
        assert!(matches!(
            lease.renewed(last - 1, Err("x".into())),
            Step::Retry(_)
        ));
        assert!(matches!(
            lease.renewed(last, Err("x".into())),
            Step::Fence(_)
        ));
    }
}

#[test]
fn a_node_stalled_past_its_lease_fences_instead_of_renewing() {
    for t in timings() {
        let mut lease = Lease::new(t);
        lease.renewed(0, Ok(t.lease_ms));
        assert_eq!(lease.before_renew(&seen(t.interval_ms)), Step::Renew);
        let step = lease.before_renew(&seen(t.lease_ms + 1));
        assert!(matches!(step, Step::Fence(_)));
    }
}

#[test]
fn the_margin_covers_two_clocks_each_off_by_the_max_skew() {
    let t = Timing::DEFAULT;
    assert_eq!(t.safety_ms, 2 * t.max_skew_ms);
}

#[test]
fn a_clock_not_known_within_the_max_skew_fences_at_once() {
    for t in timings() {
        let lease = Lease::new(t);
        let skewed = |skew_ms| lease.before_renew(&Observed { skew_ms, ..seen(0) });
        // Less what it may drift before the next check.
        let most = t.max_skew_ms - t.round_ms() / 100;
        assert_eq!(skewed(Some(most)), Step::Renew);
        assert!(matches!(skewed(Some(most + 1)), Step::Fence(_)));
        assert!(matches!(skewed(None), Step::Fence(_)));
    }
}

#[test]
fn a_clock_that_fails_is_read_again_while_the_lease_holds_past_it() {
    let t = Timing::DEFAULT;
    let mut lease = Lease::new(t);
    lease.renewed(0, Ok(t.lease_ms));
    let skewed = |now_ms, ms| {
        lease.before_renew(&Observed {
            now_ms,
            skew_ms: Some(ms),
        })
    };
    // A clock behind by the far end still fences a round before peers may
    // take over.
    let last = t.lease_ms - t.safety_ms - t.round_ms() - t.max_skew_ms;
    assert_eq!(skewed(last - 1, t.max_skew_ms), Step::Measure);
    assert!(matches!(skewed(last, t.max_skew_ms), Step::Fence(_)));
    assert!(matches!(skewed(0, t.lease_ms), Step::Fence(_)));
}

#[test]
fn a_lease_holds_for_as_long_as_it_has_time_remaining() {
    for t in timings() {
        let until = 1_000_000;
        for now in [
            0,
            until - t.safety_ms - 1,
            until - t.safety_ms,
            until,
            2 * until,
        ] {
            let remaining = t.remaining_ms(until, now);
            assert_eq!(t.holds(until, now), remaining > 0, "{t:?} at {now}");
            if remaining > 0 {
                assert!(t.holds(until, now + remaining - 1), "{t:?} at {now}");
                assert!(!t.holds(until, now + remaining), "{t:?} at {now}");
            }
        }
    }
}

/// An answer stamped at the start of the bucket's second 1,000,000, at
/// once, to a clock that reads `local` then, as its monotonic clock does.
fn stamped(known: Option<Skew>, local: i64) -> Skew {
    Skew::answered(known, local, local, 1_000_000)
}

#[test]
fn a_skew_is_the_far_end_of_what_the_dates_allow() {
    // 1.9 to 2.9 s ahead: the end nearest 0 is within `max_skew_ms`, but the
    // clock may be at the other.
    let ahead = stamped(None, 1_002_900);
    assert_eq!(ahead.most_ms(1_002_900, 1_002_900, 1_002_900), 2_900);
    assert!(Lease::new(Timing::DEFAULT).clock(Some(2_900)).is_err());
    // Kept 10 s, the window widens by the drift.
    let synced = stamped(None, 1_000_100);
    assert_eq!(synced.most_ms(1_000_100, 1_000_100, 1_000_100), 900);
    assert_eq!(synced.most_ms(1_010_100, 1_010_100, 1_010_100), 1_000);
}

/// `n` answers 370 ms apart by this clock, each 30 ms, to a clock `skew(i)`
/// off at the `i`th.
fn answers(n: i64, skew: impl Fn(i64) -> i64) -> Skew {
    let mut known = None;
    for i in 0..n {
        // This clock reads `l` as the bucket stamps the date, from the
        // bucket's 1,000,000.
        let l = 1_000_000 + skew(0) + i * 370;
        let date = (l - skew(i)) / 1000 * 1000;
        let s = Skew::answered(known, l - 10, l + 20, date);
        assert!(s.lo <= skew(i) && skew(i) <= s.hi, "{}: {s:?}", skew(i));
        known = Some(s);
    }
    known.unwrap()
}

#[test]
fn answers_across_the_bucket_second_narrow_the_skew() {
    for skew in [-1_900, -300, 0, 450, 1_900] {
        let s = answers(20, |_| skew);
        // From 1,030 ms; the drift each answer allows keeps it at 146.
        assert!(s.hi - s.lo <= 150, "{skew}: {s:?}");
        let most = s.most_ms(s.at, s.at, s.at);
        assert!(most <= skew.unsigned_abs() + 150, "{skew}: {s:?}");
    }
}

#[test]
fn a_clock_drifting_1_percent_stays_in_its_window() {
    for rate in [-10, 10] {
        answers(100, |i| 1_500 + rate * i * 370 / 1_000);
    }
}

#[test]
fn a_stepped_clock_moves_its_window_at_once() {
    // 1.8 s ahead, known to 100 ms, then stepped 600 ms further.
    let known = Skew {
        lo: 1_750,
        hi: 1_850,
        at: 1_000_000,
    };
    assert_eq!(known.most_ms(1_000_000, 1_000_000, 1_000_600), 2_450);
    // An answer after the step, 200 ms into the bucket's second.
    let after = Skew::answered(Some(known), 1_001_000, 1_001_000, 999_000);
    assert_eq!(after.most_ms(1_001_000, 1_001_000, 1_001_600), 2_460);
    // An answer that fits none of the window starts over.
    let over = Skew::answered(Some(known), 1_000_000, 1_000_000, 995_000);
    assert_eq!(over.most_ms(1_000_000, 1_000_000, 1_000_000), 5_000);
}

#[test]
fn a_pause_after_the_wall_clock_is_read_hides_no_skew() {
    // 1.75 to 1.85 s ahead; the wall clock read, then a 50 ms pause before
    // the monotonic clock is, which alone would take 50 ms off.
    let known = Skew {
        lo: 1_750,
        hi: 1_850,
        at: 1_000_000,
    };
    assert_eq!(known.most_ms(1_000_050, 1_000_050, 1_000_000), 1_801);
    assert_eq!(known.most_ms(1_000_000, 1_000_050, 1_000_000), 1_851);
}
