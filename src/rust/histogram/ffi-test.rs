// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn rejects_invalid_options() {
    assert!(new_histogram(0, 100, 3).is_err());
    assert!(new_histogram(1, 1, 3).is_err());
    assert!(new_histogram(1, 100, 6).is_err());
}

#[test]
fn summarizes_like_the_histogram() {
    let mut h = new_histogram(1, 1_000_000, 3).unwrap();
    for v in 1..=100 {
        assert!(h.record(v * 10));
    }
    assert!(!h.record(2_000_000));
    assert_eq!(h.exceeds(), 1);

    let summary = summarize(&h, 0.95);
    let median_ci = h.percentile_ci(50.0, 0.95);
    let mean_ci = h.mean_ci(0.95);
    assert_eq!(
        summary,
        Summary {
            count: 100,
            min: 10,
            max: 1000,
            mean: h.mean(),
            stddev: h.stddev(),
            median: 500,
            p75: 750,
            p99: 990,
            median_lower: median_ci.lower,
            median_upper: median_ci.upper,
            mean_lower: mean_ci.lower,
            mean_upper: mean_ci.upper,
            skewness: h.skewness(),
            kurtosis: h.kurtosis(),
        }
    );
    assert!(summary.median_lower < 500 && summary.median_upper > 500);
    assert!(summary.mean_lower < summary.mean && summary.mean_upper > summary.mean);
}

#[test]
fn summarizes_an_empty_histogram() {
    let h = new_histogram(1, 1_000_000, 3).unwrap();
    let summary = summarize(&h, 0.95);
    assert_eq!(summary.count, 0);
    assert_eq!(summary.min, i64::MAX);
    assert!(summary.mean.is_nan());
}
