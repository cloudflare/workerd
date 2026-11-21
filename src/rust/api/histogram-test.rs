// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![expect(
    clippy::float_cmp,
    reason = "the numbers compared are integers, exact in doubles"
)]

use jsg_test::Harness;

use super::*;

fn default_histogram(lock: &mut jsg::Lock<'_>) -> Histogram {
    Histogram::constructor(lock, 1, 9_007_199_254_740_991, 3, 0.0, 0.0).unwrap()
}

fn with_lock(f: impl FnOnce(&mut jsg::Lock<'_>)) {
    Harness::new().run_in_context(|lock, _ctx| {
        f(lock);
        Ok(())
    });
}

#[test]
fn record_converts_safe_integers_exactly() {
    with_lock(|lock| {
        let h = default_histogram(lock);
        h.record(9_007_199_254_740_991.0);
        h.record_big_int(1);
        assert_eq!(h.count_big_int(), 2);
        assert_eq!(h.min_big_int(), 1);
        // The largest value rounds up to its bucket, as in Node.js.
        assert!(h.max_big_int() >= 9_007_199_254_740_991);
        assert_eq!(h.exceeds_big_int(), 0);
    });
}

#[test]
fn record_big_int_out_of_range_counts_as_exceeds() {
    with_lock(|lock| {
        let h = default_histogram(lock);
        h.record_big_int(i64::MAX);
        assert_eq!(h.count_big_int(), 0);
        assert_eq!(h.exceeds_big_int(), 1);
    });
}

#[test]
fn record_delta_converts_milliseconds_to_nanoseconds() {
    with_lock(|lock| {
        let h = default_histogram(lock);
        h.record_delta(1_000.0);
        assert_eq!(h.count_big_int(), 0);
        h.record_delta(1_003.0);
        assert_eq!(h.count_big_int(), 1);
        // 3 ms, within the bucket precision of 3 significant figures.
        let delta = h.min_big_int();
        assert!((2_997_000..=3_000_000).contains(&delta), "{delta}");
    });
}

#[test]
fn record_delta_saturates_far_future_times() {
    with_lock(|lock| {
        let h = default_histogram(lock);
        h.record_delta(1.0);
        // Converts to more nanoseconds than a u64 holds; saturates instead of wrapping.
        h.record_delta(f64::MAX);
        assert_eq!(h.count_big_int(), 0);
        assert_eq!(h.exceeds_big_int(), 1);
    });
}

#[test]
fn threshold_saturates() {
    with_lock(|lock| {
        for threshold in [f64::INFINITY, 1e300, f64::NAN, 2.5] {
            assert!(Histogram::constructor(lock, 1, 1000, 3, 10.0, threshold).is_ok());
        }
    });
}

#[test]
fn invalid_options_are_a_range_error() {
    with_lock(|lock| {
        assert!(Histogram::constructor(lock, 10, 15, 3, 0.0, 0.0).is_err());
    });
}

#[test]
fn add_to_itself_doubles() {
    let harness = Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let h = default_histogram(lock);
        let h = jsg::Rc::new(lock, h);
        for value in [1, 5, 1000] {
            h.get(lock).record_big_int(value);
        }
        let before = h.get(lock).percentiles_big_int().values;
        let other = h.clone(lock);
        h.get(lock).add(lock, other)?;
        assert_eq!(h.get(lock).count_big_int(), 6);
        assert_eq!(
            h.get(lock).percentiles_big_int().values.last(),
            before.last()
        );
        Ok(())
    });
}

#[test]
fn subtract_from_itself_empties() {
    let harness = Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let h = default_histogram(lock);
        let h = jsg::Rc::new(lock, h);
        for value in [1, 5, 1000] {
            h.get(lock).record_big_int(value);
        }
        let other = h.clone(lock);
        h.get(lock).subtract(lock, other)?;
        assert_eq!(h.get(lock).count_big_int(), 0);
        assert_eq!(h.get(lock).reset_count(), 1.0);
        Ok(())
    });
}

#[test]
fn diff_maps_errors_to_codes() {
    let harness = Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let h = default_histogram(lock);
        let h = jsg::Rc::new(lock, h);
        h.get(lock).record_big_int(5);
        let before = h.get(lock).snapshot(lock)?;
        h.get(lock).record_big_int(7);
        let later = h.get(lock).snapshot(lock)?;

        let other = before.clone(lock);
        let diff = h.get(lock).diff(lock, other)?;
        assert_eq!(diff.error, DIFF_OK);
        let Some(result) = diff.histogram else {
            unreachable!("a successful diff has a histogram")
        };
        assert_eq!(result.get(lock).count_big_int(), 1);

        let diff = before.get(lock).diff(lock, later)?;
        assert_eq!(diff.error, DIFF_NOT_EARLIER);
        assert!(diff.histogram.is_none());

        let other = Histogram::constructor(lock, 1, 1_000_000, 2, 0.0, 0.0).unwrap();
        let other = jsg::Rc::new(lock, other);
        assert_eq!(h.get(lock).diff(lock, other)?.error, DIFF_INCOMPATIBLE);

        h.get(lock).reset();
        assert_eq!(h.get(lock).diff(lock, before)?.error, DIFF_RESET);
        Ok(())
    });
}

#[test]
fn qrde_maps_modes() {
    with_lock(|lock| {
        let h = default_histogram(lock);
        for value in [3, 9, 9, 1000, 123_456_789] {
            h.record_big_int(value);
        }
        let probabilities = vec![0.0, 0.5, 0.9, 1.0];
        for (mode, dequantization) in [
            (0, Dequantization::None),
            (1, Dequantization::Hdr),
            (2, Dequantization::All),
        ] {
            let result = h.qrde(probabilities.clone(), mode).unwrap();
            let expected = h
                .inner
                .borrow()
                .qrde(&probabilities, dequantization)
                .unwrap();
            assert_eq!(result.quantiles, expected.quantiles);
            assert_eq!(result.densities, expected.densities);
            assert_eq!(result.count, 5);
            assert_eq!(result.buckets, expected.bucket_count as f64);
            assert_eq!(result.corrections, expected.corrections as f64);
        }
    });
}

#[test]
fn qrde_rejects_invalid_probabilities() {
    with_lock(|lock| {
        let h = default_histogram(lock);
        h.record_big_int(1);
        assert!(h.qrde(vec![0.5], 1).is_err());
    });
}

#[test]
fn reports_counts_to_v8() {
    with_lock(|lock| {
        let h = default_histogram(lock);
        let size = h.inner.borrow().memory_size();
        assert!(size > 300 * 1024, "{size}");
        assert_eq!(h.external_memory.amount(), size);
    });
}
