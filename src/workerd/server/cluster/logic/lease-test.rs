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
    Observed { now_ms, skew_ms: 0 }
}

#[test]
fn divided_times_keep_their_proportions() {
    let t = Timing::DEFAULT;
    for (d, divided) in DIVISORS.into_iter().zip(timings()) {
        assert_eq!(divided.times().map(|ms| ms * d), t.times(), "{divided:?}");
        // A round renews before the lease needs to fence.
        let round = divided.interval_ms + divided.renew_timeout_ms;
        assert!(
            round + divided.safety_ms() < divided.lease_ms,
            "{divided:?}"
        );
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
        let last = t.lease_ms - t.safety_ms();
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
        let last = t.lease_ms - t.safety_ms() - t.renew_timeout_ms;
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
fn skew_fences_at_once() {
    for t in timings() {
        let lease = Lease::new(t);
        let max = i64::try_from(t.max_skew_ms).unwrap();
        let skewed = |skew_ms| lease.before_renew(&Observed { skew_ms, ..seen(0) });
        assert_eq!(skewed(-max), Step::Renew);
        assert!(matches!(skewed(max + 1), Step::Fence(_)));
        assert!(matches!(skewed(-max - 1), Step::Fence(_)));
    }
}
