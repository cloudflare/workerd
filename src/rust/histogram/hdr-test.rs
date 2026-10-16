// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

const MAX_SAFE_INTEGER: i64 = (1 << 53) - 1;

#[test]
fn layout_matches_hdr_histogram_c() {
    // Node.js documents 352 KiB for the default createHistogram() options; that is the counts
    // array of (1 + 43 buckets) * 1024 half sub-buckets.
    let h = Hdr::new(1, MAX_SAFE_INTEGER, 3).unwrap();
    assert_eq!(h.counts_len(), 45056);
    assert_eq!(h.counts().len(), 45056);

    let h = Hdr::new(1, i64::MAX, 3).unwrap();
    assert_eq!(h.counts_len(), 54 * 1024);

    let h = Hdr::new(1, 1_000_000, 1).unwrap();
    // 32 sub-buckets; 16 buckets cover 32 * 2^15 = 1048576.
    assert_eq!(h.sub_bucket_count, 32);
    assert_eq!(h.counts_len(), 17 * 16);
}

#[test]
fn invalid_layouts_are_rejected() {
    assert_eq!(Hdr::new(0, 100, 3).unwrap_err(), Error::InvalidOptions);
    assert_eq!(Hdr::new(1, 100, 0).unwrap_err(), Error::InvalidOptions);
    assert_eq!(Hdr::new(1, 100, 6).unwrap_err(), Error::InvalidOptions);
    assert_eq!(Hdr::new(51, 100, 3).unwrap_err(), Error::InvalidOptions);
    assert!(Hdr::new(50, 100, 3).is_ok());
    // unit_magnitude + sub_bucket_half_count_magnitude must not exceed 61.
    assert_eq!(
        Hdr::new(1 << 50, i64::MAX, 5).unwrap_err(),
        Error::InvalidOptions
    );
}

#[test]
fn equivalent_value_ranges() {
    let h = Hdr::new(1, MAX_SAFE_INTEGER, 3).unwrap();
    // Unit resolution up to 2048, then 2 up to 4096, then 4.
    assert_eq!(h.size_of_equivalent_value_range(2047), 1);
    assert_eq!(h.size_of_equivalent_value_range(2048), 2);
    assert_eq!(h.lowest_equivalent_value(2049), 2048);
    assert_eq!(h.highest_equivalent_value(2048), 2049);
    assert_eq!(h.median_equivalent_value(2048), 2049);
    assert_eq!(h.size_of_equivalent_value_range(4096), 4);
    assert_eq!(h.lowest_equivalent_value(4099), 4096);
    assert_eq!(h.highest_equivalent_value(4096), 4099);
    assert_eq!(h.highest_equivalent_value(i64::MAX), i64::MAX);
}

#[test]
fn index_round_trip() {
    let h = Hdr::new(1, MAX_SAFE_INTEGER, 2).unwrap();
    for value in [
        0,
        1,
        2,
        199,
        200,
        255,
        256,
        1000,
        123_456_789,
        MAX_SAFE_INTEGER,
    ] {
        let index = h.counts_index_for(value);
        assert_eq!(
            h.value_at_index(index),
            h.lowest_equivalent_value(value),
            "{value}"
        );
    }
}

#[test]
fn lowest_power_of_two_sets_unit_magnitude() {
    let h = Hdr::new(1024, 1 << 40, 3).unwrap();
    assert_eq!(h.unit_magnitude, 10);
    assert_eq!(h.size_of_equivalent_value_range(0), 1024);
}

#[test]
fn record_tracks_raw_min_and_max() {
    let mut h = Hdr::new(1, 1_000_000, 3).unwrap();
    assert_eq!(h.min(), i64::MAX);
    assert_eq!(h.max(), 0);
    assert!(h.mean().is_nan());
    assert!(h.stddev().is_nan());

    assert!(h.record_value(5000));
    assert!(h.record_value(7));
    assert_eq!(h.min_value(), 7);
    assert_eq!(h.max_value(), 5000);
    assert_eq!(h.min(), 7);
    // 5000 is in a bucket of width 4.
    assert_eq!(h.max(), 5003);

    // Recording 0 makes the minimum 0 without changing the raw field.
    assert!(h.record_value(0));
    assert_eq!(h.min(), 0);
    assert_eq!(h.min_value(), 7);

    assert!(!h.record_value(-1));
    assert!(!h.record_value(1_000_001));
    assert!(!h.record_values(1, -1));
    assert_eq!(h.total_count(), 3);
}

#[test]
fn reset_internal_counters_derives_min_and_max_from_counts() {
    let mut h = Hdr::new(1, 1_000_000, 3).unwrap();
    h.record_value(5001);
    h.record_value(9);
    h.reset_internal_counters();
    assert_eq!(h.total_count(), 2);
    assert_eq!(h.min_value(), 9);
    assert_eq!(h.max_value(), 5003);

    h.reset();
    h.reset_internal_counters();
    assert_eq!(h.min_value(), i64::MAX);
    assert_eq!(h.max_value(), 0);
}

#[test]
fn percentile_rounds_target_count_half_up() {
    let mut h = Hdr::new(1, 1_000, 3).unwrap();
    for value in 1..=10 {
        h.record_value(value);
    }
    // 12% of 10 values is 1.2, which rounds to the first value; rounding up would give the second.
    assert_eq!(h.value_at_percentile(12.0), 1);
    assert_eq!(h.value_at_percentile(15.0), 2);
    assert_eq!(h.value_at_percentile(50.0), 5);
    assert_eq!(h.value_at_percentile(100.0), 10);
    assert_eq!(h.value_at_percentile(150.0), 10);
    assert_eq!(
        h.value_at_percentiles(&[12.0, 15.0, 50.0, 100.0]),
        [1, 2, 5, 10]
    );
}

#[test]
fn value_at_percentiles_of_empty_histogram_keeps_target_counts() {
    let h = Hdr::new(1, 1_000, 3).unwrap();
    assert_eq!(h.value_at_percentiles(&[50.0, 100.0]), [1, 1]);
    assert_eq!(h.value_at_percentile(50.0), 0);
}

#[test]
fn record_corrected_value_backfills() {
    let mut h = Hdr::new(1, 1_000, 3).unwrap();
    assert!(h.record_corrected_value(100, 30));
    let values: Vec<_> = h.recorded().map(|b| b.value).collect();
    assert_eq!(values, [40, 70, 100]);
}

#[test]
fn add_reports_dropped_values() {
    let mut small = Hdr::new(1, 1_000, 3).unwrap();
    let mut big = Hdr::new(1, 1_000_000, 3).unwrap();
    big.record_values(10, 3);
    big.record_values(500_000, 2);
    assert_eq!(small.add(&big), 2);
    assert_eq!(small.total_count(), 3);
}

#[test]
fn linear_and_log_buckets() {
    let mut h = Hdr::new(1, 1_000, 3).unwrap();
    for value in [1, 2, 3, 15, 25] {
        h.record_value(value);
    }
    assert_eq!(h.linear_buckets(10), [(10, 3), (20, 1), (30, 1)]);
    assert_eq!(
        h.log_buckets(1, 2.0),
        [(1, 1), (2, 1), (4, 1), (8, 0), (16, 1), (32, 1)]
    );
    // A non-positive step never advances; everything lands in one step.
    assert_eq!(
        h.linear_buckets(0),
        [(h.value_at_index(h.counts_len() - 1), 5)]
    );
}

#[test]
fn percentile_iteration_of_empty_histogram() {
    let h = Hdr::new(1, 1_000, 3).unwrap();
    assert_eq!(h.percentiles(1), [(100.0, 0)]);
}
