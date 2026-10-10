// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! C++ interface, used by the benchmark harness.

use crate::Histogram;
use crate::Options;

#[cxx::bridge(namespace = "workerd::rust::histogram")]
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
mod bridge {
    /// Summary statistics of a histogram, in its units.
    #[derive(Debug, Clone, Copy, PartialEq)]
    struct Summary {
        count: u64,
        min: i64,
        max: i64,
        mean: f64,
        stddev: f64,
        median: i64,
        p75: i64,
        p99: i64,
        /// Confidence interval on the median (exact binomial).
        median_lower: i64,
        median_upper: i64,
        /// Confidence interval on the mean (Student's t).
        mean_lower: f64,
        mean_upper: f64,
        skewness: f64,
        kurtosis: f64,
    }

    extern "Rust" {
        type Histogram;

        /// A histogram tracking values in `[lowest, highest]` to `figures` significant figures.
        fn new_histogram(lowest: i64, highest: i64, figures: i32) -> Result<Box<Histogram>>;

        /// Records `value`; returns false, counting it as exceeding, if it is out of range.
        fn record(self: &mut Histogram, value: i64) -> bool;

        /// The number of values that were out of range.
        fn exceeds(self: &Histogram) -> u64;

        /// Summary statistics, with confidence intervals at `confidence` (in (0, 1)).
        fn summarize(histogram: &Histogram, confidence: f64) -> Summary;
    }
}

pub use bridge::Summary;

/// Creates a histogram for C++.
///
/// # Errors
///
/// Fails if the options are invalid.
pub fn new_histogram(
    lowest: i64,
    highest: i64,
    figures: i32,
) -> Result<Box<Histogram>, cxx::KjError> {
    let options = Options {
        lowest,
        highest,
        figures,
        ..Options::default()
    };
    Histogram::new(&options)
        .map(Box::new)
        .map_err(|e| cxx::KjError::new(cxx::KjExceptionType::Failed, e.to_string()))
}

/// Summarizes a histogram for C++.
#[must_use]
pub fn summarize(histogram: &Histogram, confidence: f64) -> Summary {
    // percentile() only fails for percentiles outside (0, 100], which these aren't.
    let percentile = |p| histogram.percentile(p).unwrap_or(0);
    let median_ci = histogram.percentile_ci(50.0, confidence);
    let mean_ci = histogram.mean_ci(confidence);
    Summary {
        count: histogram.count(),
        min: histogram.min(),
        max: histogram.max(),
        mean: histogram.mean(),
        stddev: histogram.stddev(),
        median: percentile(50.0),
        p75: percentile(75.0),
        p99: percentile(99.0),
        median_lower: median_ci.lower,
        median_upper: median_ci.upper,
        mean_lower: mean_ci.lower,
        mean_upper: mean_ci.upper,
        skewness: histogram.skewness(),
        kurtosis: histogram.kurtosis(),
    }
}

#[cfg(test)]
#[path = "ffi-test.rs"]
mod tests;
