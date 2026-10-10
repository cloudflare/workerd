// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![expect(
    clippy::float_cmp,
    reason = "the expected values are exactly what these operations compute"
)]

use super::*;

fn histogram_of(values: &[i64]) -> Histogram {
    let mut h = Histogram::new(&Options::default()).unwrap();
    for &value in values {
        assert!(h.record(value));
    }
    h
}

#[test]
fn default_options_match_node() {
    let h = Histogram::new(&Options::default()).unwrap();
    assert_eq!(h.lowest(), 1);
    assert_eq!(h.highest(), i64::MAX);
    assert_eq!(h.figures(), 3);
    assert_eq!(h.count(), 0);
    assert_eq!(h.min(), i64::MAX);
    assert_eq!(h.max(), 0);
    assert!(h.mean().is_nan());
    assert!(h.stddev().is_nan());
    assert_eq!(h.cdf(10), 0.0);
}

#[test]
fn out_of_range_values_count_as_exceeds() {
    let mut h = Histogram::new(&Options {
        highest: 1000,
        ..Options::default()
    })
    .unwrap();
    assert!(h.record(1000));
    assert!(!h.record(1001));
    assert!(!h.record_corrected(5000, 10));
    assert_eq!(h.count(), 1);
    assert_eq!(h.exceeds(), 2);
}

#[test]
fn percentile_rejects_out_of_range_arguments() {
    let h = histogram_of(&[1, 2, 3]);
    assert!(h.percentile(0.0).is_err());
    assert!(h.percentile(100.5).is_err());
    assert!(h.percentile(f64::NAN).is_err());
    assert_eq!(h.percentile(100.0), Ok(3));
}

#[test]
fn cdf_and_count_at() {
    let h = histogram_of(&[1, 2, 2, 3]);
    assert_eq!(h.cdf(0), 0.0);
    assert_eq!(h.cdf(1), 0.25);
    assert_eq!(h.cdf(2), 0.75);
    assert_eq!(h.cdf(1000), 1.0);
    assert_eq!(h.count_at(2), 2);
    assert_eq!(h.count_at(-5), 0);
}

#[test]
fn ewma_follows_half_life() {
    let mut h = Histogram::new(&Options {
        half_life: 1.0,
        threshold: 15,
        ..Options::default()
    })
    .unwrap();
    assert_eq!(h.ewma_mean(), 0.0);
    // With a half-life of 1 sample, alpha is 1/2.
    h.record(10);
    assert_eq!(h.ewma_mean(), 10.0);
    assert_eq!(h.ewma_error_rate(), 0.0);
    h.record(20);
    assert_eq!(h.ewma_mean(), 15.0);
    assert_eq!(h.ewma_stddev(), 25.0_f64.sqrt());
    assert_eq!(h.ewma_error_rate(), 0.5);

    h.reset();
    assert_eq!(h.ewma_mean(), 0.0);
    assert_eq!(h.reset_count(), 1);
}

#[test]
fn snapshot_is_independent() {
    let mut h = histogram_of(&[1, 2, 3]);
    let snapshot = h.snapshot().unwrap();
    h.record(4);
    h.reset();
    assert_eq!(snapshot.count(), 3);
    assert_eq!(snapshot.max(), 3);
    assert_eq!(snapshot.reset_count(), 0);
}

#[test]
fn diff_returns_values_recorded_after_snapshot() {
    let mut h = histogram_of(&[1, 2, 3]);
    let earlier = h.snapshot().unwrap();
    h.record(10);
    h.record(20);
    h.record(1_i64 << 62);
    let diff = h.diff(&earlier).unwrap();
    assert_eq!(diff.count(), 3);
    assert_eq!(diff.min(), 10);
    assert_eq!(diff.reset_count(), 0);
    assert_eq!(diff.exceeds(), 0);

    // Wrong order.
    assert_eq!(earlier.diff(&h).unwrap_err(), Error::NotEarlier);

    // Reset in between.
    let mut reset = h.snapshot().unwrap();
    reset.reset();
    assert_eq!(reset.diff(&earlier).unwrap_err(), Error::Reset);

    // Different layout.
    let other = Histogram::new(&Options {
        figures: 2,
        ..Options::default()
    })
    .unwrap();
    assert_eq!(h.diff(&other).unwrap_err(), Error::Incompatible);
}

#[test]
fn subtract_clamps_at_zero() {
    let mut h = histogram_of(&[1, 1, 5]);
    let other = histogram_of(&[1, 5, 5, 9]);
    assert_eq!(h.subtract(&other), 2);
    assert_eq!(h.count(), 1);
    assert_eq!(h.min(), 1);
    assert_eq!(h.max(), 1);
    assert_eq!(h.reset_count(), 1);
}

#[test]
fn add_merges_counts_and_exceeds() {
    let mut h = histogram_of(&[1]);
    let mut other = Histogram::new(&Options {
        highest: 1000,
        ..Options::default()
    })
    .unwrap();
    other.record(7);
    other.record(5000);
    assert_eq!(h.add(&other), 0);
    assert_eq!(h.count(), 2);
    assert_eq!(h.exceeds(), 1);
}

#[test]
fn record_delta_records_time_between_calls() {
    let mut h = histogram_of(&[]);
    assert_eq!(h.record_delta(1000), 0);
    assert_eq!(h.count(), 0);
    assert_eq!(h.record_delta(1250), 250);
    assert_eq!(h.count(), 1);
    assert_eq!(h.max(), 250);
}

#[test]
fn memory_size_includes_counts() {
    let h = histogram_of(&[]);
    assert!(h.memory_size() > 45056 * 8);
}
