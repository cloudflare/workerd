// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The equivalent of Node.js's native `node::Histogram` class (`src/histogram.{h,cc}`): an HDR
//! histogram plus the state Node.js keeps next to it (the `exceeds` count, the reset count, the
//! `recordDelta()` timestamp, and the EWMA).

#![expect(
    clippy::cast_precision_loss,
    reason = "values are converted to f64 exactly where Node.js converts them to double"
)]
#![expect(
    clippy::suboptimal_flops,
    reason = "fused operations would round differently from Node.js"
)]

use crate::Error;
use crate::hdr::Hdr;
use crate::hdr::RecordedBucket;

/// Options for a new histogram: `node::Histogram::Options`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Options {
    /// The lowest discernible value, at least 1.
    pub lowest: i64,
    /// The highest trackable value, at least twice `lowest`.
    pub highest: i64,
    /// Significant figures of precision, 1 to 5.
    pub figures: i32,
    /// The EWMA half-life in samples. 0 disables the EWMA.
    pub half_life: f64,
    /// Values above this count as errors in the EWMA error rate. 0 disables the error rate.
    pub threshold: i64,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            lowest: 1,
            highest: i64::MAX,
            figures: 3,
            half_life: 0.0,
            threshold: 0,
        }
    }
}

/// A recordable HDR histogram with the semantics of Node.js's `Histogram`.
///
/// Methods that compare two histograms walk the counts arrays index by index, as Node.js does, so
/// their results are only meaningful for histograms with the same layout (`is_compatible()`).
#[derive(Debug)]
pub struct Histogram {
    pub(crate) hdr: Hdr,
    pub(crate) prev: u64,
    pub(crate) exceeds: u64,
    pub(crate) reset_count: u64,
    pub(crate) ewma_alpha: f64,
    pub(crate) ewma_mean: f64,
    pub(crate) ewma_variance: f64,
    pub(crate) ewma_error_rate: f64,
    pub(crate) ewma_initialized: bool,
    pub(crate) threshold: i64,
}

impl Histogram {
    /// `Histogram::Create()`.
    pub fn new(options: &Options) -> Result<Self, Error> {
        let hdr = Hdr::new(options.lowest, options.highest, options.figures)?;
        Ok(Self::from_hdr(hdr, options))
    }

    fn from_hdr(hdr: Hdr, options: &Options) -> Self {
        // alpha = 1 - 2^(-1/halfLife). With halfLife <= 0, the EWMA is disabled.
        let ewma_alpha = if options.half_life > 0.0 {
            1.0 - (-(2.0_f64.ln()) / options.half_life).exp()
        } else {
            0.0
        };
        Self {
            hdr,
            prev: 0,
            exceeds: 0,
            reset_count: 0,
            ewma_alpha,
            ewma_mean: 0.0,
            ewma_variance: 0.0,
            ewma_error_rate: 0.0,
            ewma_initialized: false,
            threshold: options.threshold,
        }
    }

    /// `Histogram::CreateWithSameLayout()`: an empty histogram with the same layout and default
    /// values for everything else.
    fn with_same_layout(&self) -> Result<Self, Error> {
        Ok(Self::from_hdr(
            self.hdr.with_same_layout()?,
            &Options::default(),
        ))
    }

    /// `Histogram::Clone()`, which backs `histogram.snapshot()`: an independent copy of the
    /// configuration, recorded values, and all statistical state.
    pub fn snapshot(&self) -> Result<Self, Error> {
        Ok(Self {
            hdr: self.hdr.try_clone()?,
            ..*self
        })
    }

    /// `Histogram::IsCompatible()`: whether both histograms map values to the same indexes.
    pub fn is_compatible(&self, other: &Self) -> bool {
        self.hdr.counts_len() == other.hdr.counts_len()
            && self.hdr.lowest_discernible_value() == other.hdr.lowest_discernible_value()
            && self.hdr.highest_trackable_value() == other.hdr.highest_trackable_value()
            && self.hdr.significant_figures() == other.hdr.significant_figures()
    }

    /// `Histogram::Diff()`: the values recorded in this histogram after `other`, an earlier
    /// snapshot of it, was taken. The result has no EWMA state and a reset count of 0.
    pub fn diff(&self, other: &Self) -> Result<Self, Error> {
        if !self.is_compatible(other) {
            return Err(Error::Incompatible);
        }
        let mut diff = self.with_same_layout()?;
        diff.hdr.counts_mut().copy_from_slice(self.hdr.counts());
        diff.exceeds = self.exceeds;

        if self.reset_count != other.reset_count {
            return Err(Error::Reset);
        }
        if diff.exceeds < other.exceeds {
            return Err(Error::NotEarlier);
        }
        for (target, &source) in diff.hdr.counts_mut().iter_mut().zip(other.hdr.counts()) {
            if *target < source {
                return Err(Error::NotEarlier);
            }
            *target -= source;
        }
        diff.exceeds -= other.exceeds;
        diff.hdr.reset_internal_counters();
        Ok(diff)
    }

    fn update_ewma(&mut self, value: f64) {
        if self.ewma_alpha <= 0.0 {
            return;
        }
        if !self.ewma_initialized {
            self.ewma_mean = value;
            self.ewma_variance = 0.0;
            self.ewma_initialized = true;
            if self.threshold > 0 {
                self.ewma_error_rate = if value > self.threshold as f64 {
                    1.0
                } else {
                    0.0
                };
            }
            return;
        }
        let diff = value - self.ewma_mean;
        self.ewma_mean += self.ewma_alpha * diff;
        self.ewma_variance =
            (1.0 - self.ewma_alpha) * (self.ewma_variance + self.ewma_alpha * diff * diff);

        // Binary EWMA for the SLO error rate: 1 if over the threshold, 0 otherwise.
        if self.threshold > 0 {
            let exceeded = if value > self.threshold as f64 {
                1.0
            } else {
                0.0
            };
            self.ewma_error_rate += self.ewma_alpha * (exceeded - self.ewma_error_rate);
        }
    }

    /// Records a value. Returns false, and counts it in `exceeds()`, if it is out of range.
    pub fn record(&mut self, value: i64) -> bool {
        let recorded = self.hdr.record_value(value);
        if recorded {
            self.update_ewma(value as f64);
        } else {
            self.exceeds += 1;
        }
        recorded
    }

    /// Records a value with coordinated omission correction: when `value` exceeds
    /// `expected_interval`, the values that a stall prevented from being recorded are backfilled
    /// at `expected_interval` steps. The EWMA sees only `value`.
    pub fn record_corrected(&mut self, value: i64, expected_interval: i64) -> bool {
        let recorded = self.hdr.record_corrected_value(value, expected_interval);
        if recorded {
            self.update_ewma(value as f64);
        } else {
            self.exceeds += 1;
        }
        recorded
    }

    /// `recordDelta()`, with the current time passed in: records the time since the previous call
    /// and returns it. The first call records nothing and returns 0.
    pub fn record_delta(&mut self, now: u64) -> u64 {
        let mut delta = 0;
        if self.prev > 0 {
            delta = now.saturating_sub(self.prev);
            match i64::try_from(delta) {
                Ok(value) if self.hdr.record_value(value) => self.update_ewma(value as f64),
                _ => self.exceeds += 1,
            }
        }
        self.prev = now;
        delta
    }

    /// Adds the values of `other` to this histogram. Returns the number of values that did not
    /// fit; they are not counted in `exceeds()`.
    pub fn add(&mut self, other: &Self) -> u64 {
        self.exceeds += other.exceeds;
        if other.prev > self.prev {
            self.prev = other.prev;
        }
        self.hdr.add(&other.hdr).cast_unsigned()
    }

    /// Subtracts the counts of `other` index by index, clamping at zero. Returns the number of
    /// values that would have gone below zero, and increments the reset count.
    pub fn subtract(&mut self, other: &Self) -> u64 {
        let mut dropped = 0_i64;
        for (target, &source) in self.hdr.counts_mut().iter_mut().zip(other.hdr.counts()) {
            let mut count = *target - source;
            if count < 0 {
                dropped += -count;
                count = 0;
            }
            *target = count;
        }
        self.hdr.reset_internal_counters();
        self.reset_count += 1;
        self.exceeds = self.exceeds.saturating_sub(other.exceeds);
        dropped.cast_unsigned()
    }

    /// Clears the recorded values and statistical state, and increments the reset count.
    pub fn reset(&mut self) {
        self.hdr.reset();
        self.reset_count += 1;
        self.exceeds = 0;
        self.prev = 0;
        self.ewma_mean = 0.0;
        self.ewma_variance = 0.0;
        self.ewma_error_rate = 0.0;
        self.ewma_initialized = false;
    }

    pub fn lowest(&self) -> i64 {
        self.hdr.lowest_discernible_value()
    }

    pub fn highest(&self) -> i64 {
        self.hdr.highest_trackable_value()
    }

    pub fn figures(&self) -> i32 {
        self.hdr.significant_figures()
    }

    pub fn count(&self) -> u64 {
        self.hdr.total_count().cast_unsigned()
    }

    pub fn exceeds(&self) -> u64 {
        self.exceeds
    }

    /// The number of calls to `reset()` and `subtract()`. A snapshot carries the reset count of
    /// its source, so comparing two snapshots shows whether the source was reset in between.
    pub fn reset_count(&self) -> u64 {
        self.reset_count
    }

    /// The smallest value recorded, rounded down to its bucket. `i64::MAX` when empty.
    pub fn min(&self) -> i64 {
        self.hdr.min()
    }

    /// The largest value recorded, rounded up to its bucket. 0 when empty.
    pub fn max(&self) -> i64 {
        self.hdr.max()
    }

    /// The mean of the bucket midpoints. NaN when empty.
    pub fn mean(&self) -> f64 {
        self.hdr.mean()
    }

    /// The population standard deviation of the bucket midpoints. NaN when empty.
    pub fn stddev(&self) -> f64 {
        self.hdr.stddev()
    }

    /// The value at a percentile in (0, 100].
    pub fn percentile(&self, percentile: f64) -> Result<i64, Error> {
        if !(percentile > 0.0 && percentile <= 100.0) {
            return Err(Error::InvalidArgument("percentile must be in (0, 100]"));
        }
        Ok(self.hdr.value_at_percentile(percentile))
    }

    /// The values at several ascending percentiles, in one pass.
    pub fn percentiles_at(&self, percentiles: &[f64]) -> Vec<i64> {
        self.hdr.value_at_percentiles(percentiles)
    }

    /// The percentile distribution as Node.js's `histogram.percentiles` reports it, as
    /// `(percentile, value)` pairs in iteration order. A key can repeat; a `Map` built from the
    /// pairs keeps the last value for each key.
    pub fn percentiles(&self) -> Vec<(f64, i64)> {
        self.hdr.percentiles(1)
    }

    /// The number of values recorded in the bucket that `value` falls into.
    pub fn count_at(&self, value: i64) -> i64 {
        self.hdr.count_at_value(value)
    }

    /// The fraction of values up to and including the first bucket whose highest equivalent value
    /// is at least `value`. 0 when empty.
    pub fn cdf(&self, value: i64) -> f64 {
        let total = self.hdr.total_count();
        if total == 0 {
            return 0.0;
        }
        let mut iter = self.hdr.iter_all();
        while self.hdr.move_next(&mut iter) {
            if iter.highest_equivalent_value >= value {
                return iter.cumulative_count as f64 / total as f64;
            }
            // All recorded data is accounted for; the remaining buckets are empty.
            if iter.cumulative_count >= total {
                break;
            }
        }
        1.0
    }

    /// The counts rebucketed into steps of `step_size`, as `(value, count)` pairs.
    pub fn linear_buckets(&self, step_size: i64) -> Vec<(i64, i64)> {
        self.hdr.linear_buckets(step_size)
    }

    /// The counts rebucketed into buckets that grow by `base`, as `(value, count)` pairs.
    pub fn log_buckets(&self, first_bucket: i64, base: f64) -> Vec<(i64, i64)> {
        self.hdr.log_buckets(first_bucket, base)
    }

    /// The buckets with a nonzero count, in ascending order.
    pub fn recorded(&self) -> impl Iterator<Item = RecordedBucket> + '_ {
        self.hdr.recorded()
    }

    pub fn ewma_mean(&self) -> f64 {
        if self.ewma_initialized {
            self.ewma_mean
        } else {
            0.0
        }
    }

    pub fn ewma_stddev(&self) -> f64 {
        if self.ewma_initialized {
            self.ewma_variance.sqrt()
        } else {
            0.0
        }
    }

    pub fn ewma_error_rate(&self) -> f64 {
        if self.ewma_initialized {
            self.ewma_error_rate
        } else {
            0.0
        }
    }

    /// The memory used by the counts array and this struct.
    pub fn memory_size(&self) -> usize {
        size_of::<Self>() - size_of::<Hdr>() + self.hdr.memory_size()
    }
}

#[cfg(test)]
#[path = "histogram-test.rs"]
mod tests;
