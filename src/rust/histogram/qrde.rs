// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Quantile-respectful density estimation (QRDE), ported from Node.js's `histogram.qrde()`.
//!
//! The estimate places Harrell-Davis quantiles at the requested probability boundaries; the density
//! of each bin is its probability mass over its width. Values can be spread deterministically
//! within their buckets ("dequantized") to reduce quantization artifacts.
//!
//! Node.js runs the computation on its thread pool and returns a promise. Here it is synchronous:
//! it is linear in the number of occupied buckets per boundary, which is fast enough to run inline.
//! For the same reason there is no equivalent of Node.js's `cache` option, which reuses the
//! snapshot of occupied buckets between calls.

#![expect(
    clippy::cast_precision_loss,
    reason = "counts and values are converted to f64 exactly where Node.js converts them to double"
)]
#![expect(
    clippy::suboptimal_flops,
    reason = "fused operations would round differently from Node.js"
)]

use crate::Error;
use crate::Histogram;
use crate::stats::BetaParameters;
use crate::stats::std_clamp;
use crate::stats::std_max;
use crate::stats::std_min;

/// Which values QRDE spreads across their bucket's range rather than treating as the bucket
/// midpoint. Only buckets holding more than one value are spread.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Dequantization {
    /// No values: every value is its bucket midpoint.
    None,
    /// Values in buckets wider than one unit, whose midpoint is not exact. The default.
    Hdr,
    /// Values in every bucket, including unit-wide ones, as for integer data with ties.
    All,
}

/// The result of [`Histogram::qrde()`].
#[derive(Debug, Clone, PartialEq)]
pub struct Qrde {
    /// The quantile at each requested probability. Empty for an empty histogram.
    pub quantiles: Vec<f64>,
    /// The density of each bin between consecutive quantiles; infinite for a bin of zero width.
    pub densities: Vec<f64>,
    /// The number of values.
    pub count: u64,
    /// The number of occupied buckets.
    pub bucket_count: usize,
    /// How many quantiles were raised to the previous one to keep the sequence non-decreasing.
    pub corrections: usize,
}

/// One occupied bucket: `Histogram::RecordedBucket` in Node.js.
struct Bucket {
    /// The bucket midpoint.
    value: f64,
    /// The bucket width.
    resolution: f64,
    count: i64,
    cumulative_count: i64,
}

struct Snapshot {
    buckets: Vec<Bucket>,
    total_count: i64,
}

impl Snapshot {
    fn new(histogram: &Histogram) -> Self {
        let hdr = &histogram.hdr;
        Self {
            buckets: hdr
                .recorded()
                .map(|bucket| Bucket {
                    value: bucket.median_equivalent_value as f64,
                    resolution: hdr.size_of_equivalent_value_range(bucket.value) as f64,
                    count: bucket.count,
                    cumulative_count: bucket.cumulative_count,
                })
                .collect(),
            total_count: hdr.total_count(),
        }
    }

    fn should_dequantize(&self, index: usize, dequantization: Dequantization) -> bool {
        let bucket = &self.buckets[index];
        bucket.count > 1
            && (dequantization == Dequantization::All
                || (dequantization == Dequantization::Hdr && bucket.resolution > 1.0))
    }

    /// The range a bucket's values are spread over. The first and last buckets only spread
    /// inward, so the estimate stays within the recorded range.
    fn bucket_range(&self, index: usize, dequantization: Dequantization) -> (f64, f64) {
        let bucket = &self.buckets[index];
        if !self.should_dequantize(index, dequantization) {
            return (bucket.value, bucket.value);
        }
        let half_resolution = bucket.resolution / 2.0;
        if self.buckets.len() == 1 {
            return (
                bucket.value - half_resolution,
                bucket.value + half_resolution,
            );
        }
        if index == 0 {
            return (bucket.value, bucket.value + half_resolution);
        }
        if index + 1 == self.buckets.len() {
            return (bucket.value - half_resolution, bucket.value);
        }
        (
            bucket.value - half_resolution,
            bucket.value + half_resolution,
        )
    }

    /// The range of buckets outside of which the Harrell-Davis weights are negligible, or `None`
    /// if the weights cannot be evaluated.
    fn exact_support(
        &self,
        params: &BetaParameters,
        reflected: &BetaParameters,
        count: f64,
    ) -> Option<(usize, usize)> {
        // Collapsing both tails changes a quantile by at most 2^-79 times the histogram range,
        // which is less than 2^-16 over the i64 value domain.
        const TAIL_MASS: f64 = 1.0 / (1_u128 << 80) as f64;
        let len = self.buckets.len();

        let mut lo = 0;
        let mut hi = len;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let rank = self.buckets[mid].cumulative_count as f64 / count;
            let cdf = params.cdf(rank);
            if !cdf.is_finite() {
                return None;
            }
            if cdf < TAIL_MASS {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo == len {
            return None;
        }
        let first = lo;

        hi = len;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let remaining_rank =
                (self.total_count - self.buckets[mid].cumulative_count) as f64 / count;
            let survival = reflected.cdf(remaining_rank);
            if !survival.is_finite() {
                return None;
            }
            if survival > TAIL_MASS {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo == len || lo < first {
            return None;
        }
        Some((first, lo + 1))
    }

    /// The Harrell-Davis estimate of the quantile at `p`: the expected value of the order
    /// statistic at rank (n + 1) p, weighting each bucket by the Beta((n + 1) p, (n + 1)(1 - p))
    /// mass of its rank interval.
    #[expect(
        clippy::too_many_lines,
        reason = "a single pass over the buckets, as in Node.js"
    )]
    fn quantile(&self, p: f64, dequantization: Dequantization) -> f64 {
        let count = self.total_count as f64;
        let a = (count + 1.0) * p;
        let b = (count + 1.0) * (1.0 - p);
        let mass_params = BetaParameters::new(a, b, true);
        let reflected_params = mass_params.reflect();
        let len = self.buckets.len();

        let mut begin_index = 0;
        let mut end_index = len;
        if mass_params.use_asymptotic_cdf {
            // Outside this interval the normal-tail mass is below 2e-33.
            const SUPPORT_DEVIATIONS: f64 = 12.0;
            let lower_rank = std_max(
                0.0,
                mass_params.mean - SUPPORT_DEVIATIONS * mass_params.standard_deviation,
            );
            let upper_rank = std_min(
                1.0,
                mass_params.mean + SUPPORT_DEVIATIONS * mass_params.standard_deviation,
            );
            let lower = lower_rank * count;
            let upper = upper_rank * count;
            let first = self
                .buckets
                .partition_point(|bucket| (bucket.cumulative_count as f64) < lower);
            let last = first
                + self.buckets[first..]
                    .partition_point(|bucket| (bucket.cumulative_count as f64) < upper);
            begin_index = first;
            if last != len {
                end_index = last + 1;
            }
        } else if len >= 512
            && let Some((begin, end)) = self.exact_support(&mass_params, &reflected_params, count)
        {
            begin_index = begin;
            end_index = end;
        }

        let dequantize = dequantization != Dequantization::None;
        let lower_endpoint = self.bucket_range(0, dequantization).0;
        let upper_endpoint = self.bucket_range(len - 1, dequantization).1;

        let mut previous_cdf = 0.0;
        let mut previous_survival = 0.0;
        let mut previous_front = 0.0;
        let mut previous_count = if begin_index == 0 {
            0
        } else {
            self.buckets[begin_index - 1].cumulative_count
        };
        if previous_count != 0 {
            let previous_rank = previous_count as f64 / count;
            let (cdf, front) = mass_params.cdf_and_front(previous_rank);
            let front = if dequantize { front } else { 0.0 };
            if !cdf.is_finite() || (dequantize && !front.is_finite()) {
                begin_index = 0;
                end_index = len;
                previous_count = 0;
                previous_cdf = 0.0;
                previous_front = 0.0;
            } else {
                previous_cdf = std_clamp(cdf, 0.0, 1.0);
                previous_front = front;
            }
        }

        let mut quantile = lower_endpoint * previous_cdf;
        let mut using_survival = false;

        for i in begin_index..end_index {
            let bucket = &self.buckets[i];
            let u0 = previous_count as f64 / count;
            let u1 = bucket.cumulative_count as f64 / count;
            let mut front = 0.0;
            let mass;
            // Use the reflected CDF above the symmetry point so that small upper-tail interval
            // weights are not lost to subtraction from one.
            if u1 <= mass_params.symmetry_point {
                let (cdf, cdf_front) = mass_params.cdf_and_front(u1);
                if dequantize {
                    front = cdf_front;
                }
                let cdf = std_clamp(cdf, previous_cdf, 1.0);
                mass = cdf - previous_cdf;
                previous_cdf = cdf;
            } else {
                let remaining_rank = (self.total_count - bucket.cumulative_count) as f64 / count;
                let (mut survival, survival_front) = reflected_params.cdf_and_front(remaining_rank);
                if dequantize {
                    front = survival_front;
                }
                if using_survival {
                    survival = std_clamp(survival, 0.0, previous_survival);
                    mass = previous_survival - survival;
                } else {
                    survival = std_clamp(survival, 0.0, 1.0 - previous_cdf);
                    mass = 1.0 - previous_cdf - survival;
                    using_survival = true;
                }
                previous_survival = survival;
            }

            if self.should_dequantize(i, dequantization) {
                // I_x(a + 1, b) = I_x(a, b) - front / a, so the first moment within this rank
                // interval does not need a second beta CDF.
                let sum = a + b;
                let rank_width = bucket.count as f64 / count;
                let local_moment = std_clamp(
                    (a / sum - u0) * mass - (front - previous_front) / sum,
                    0.0,
                    rank_width * mass,
                );
                let (lower, upper) = self.bucket_range(i, dequantization);
                quantile += lower * mass + (upper - lower) * local_moment / rank_width;
            } else {
                quantile += bucket.value * mass;
            }

            previous_front = front;
            previous_count = bucket.cumulative_count;
        }

        let remaining_mass = if using_survival {
            previous_survival
        } else {
            1.0 - previous_cdf
        };
        quantile += upper_endpoint * std_clamp(remaining_mass, 0.0, 1.0);
        quantile
    }
}

impl Qrde {
    /// `bins + 1` evenly spaced probability boundaries from 0 to 1, as Node.js's `bins` option
    /// produces. `bins` must be 1 to 1000.
    pub fn uniform_probabilities(bins: u32) -> Result<Vec<f64>, Error> {
        if !(1..=1000).contains(&bins) {
            return Err(Error::InvalidArgument("bins must be 1 to 1000"));
        }
        Ok((0..=bins).map(|i| f64::from(i) / f64::from(bins)).collect())
    }
}

impl Histogram {
    /// A quantile-respectful density estimate at the given probability boundaries, which must be
    /// 2 to 1001 strictly increasing values starting with 0 and ending with 1.
    pub fn qrde(
        &self,
        probabilities: &[f64],
        dequantization: Dequantization,
    ) -> Result<Qrde, Error> {
        if !(2..=1001).contains(&probabilities.len()) {
            return Err(Error::InvalidArgument(
                "probabilities must have 2 to 1001 elements",
            ));
        }
        let mut previous = -1.0;
        for &probability in probabilities {
            if !(0.0..=1.0).contains(&probability) {
                return Err(Error::InvalidArgument("probabilities must be in [0, 1]"));
            }
            if probability <= previous {
                return Err(Error::InvalidArgument(
                    "probabilities must be strictly increasing",
                ));
            }
            previous = probability;
        }
        if probabilities.first() != Some(&0.0) || probabilities.last() != Some(&1.0) {
            return Err(Error::InvalidArgument(
                "probabilities must start with 0 and end with 1",
            ));
        }

        let snapshot = Snapshot::new(self);
        let mut result = Qrde {
            quantiles: Vec::new(),
            densities: Vec::new(),
            count: self.count(),
            bucket_count: snapshot.buckets.len(),
            corrections: 0,
        };
        if snapshot.buckets.is_empty() {
            return Ok(result);
        }

        let n = probabilities.len();
        let lowest = snapshot.bucket_range(0, dequantization).0;
        let highest = snapshot
            .bucket_range(snapshot.buckets.len() - 1, dequantization)
            .1;
        let mut quantiles = vec![0.0; n];
        quantiles[0] = lowest;
        quantiles[n - 1] = highest;
        for i in 1..n - 1 {
            let mut value = std_clamp(
                snapshot.quantile(probabilities[i], dequantization),
                lowest,
                highest,
            );
            if value < quantiles[i - 1] {
                value = quantiles[i - 1];
                result.corrections += 1;
            }
            quantiles[i] = value;
        }

        result.densities = (0..n - 1)
            .map(|i| {
                let width = quantiles[i + 1] - quantiles[i];
                let probability_mass = probabilities[i + 1] - probabilities[i];
                if width == 0.0 {
                    f64::INFINITY
                } else {
                    probability_mass / width
                }
            })
            .collect();
        result.quantiles = quantiles;
        Ok(result)
    }
}

#[cfg(test)]
#[path = "qrde-test.rs"]
mod tests;
