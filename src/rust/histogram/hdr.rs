// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A port of the core of `HdrHistogram_c` (`hdr_histogram.c`, version 0.12.0 as vendored by
//! Node.js in `deps/histogram`), which is released to the public domain under CC0.
//!
//! Only what Node.js's `Histogram` uses is ported: bucket layout, recording, merging, the
//! internal-counter reset, the value-equivalence helpers, percentiles, mean, standard deviation,
//! and the all-values, recorded, percentile, linear, and logarithmic iterators. The behavior,
//! including edge cases and the order of floating-point operations, follows the C code so that
//! results match Node.js exactly.
//!
//! Two parts of the C structure are left out. `normalizing_index_offset` is only changed by the
//! shift operations, which Node.js never calls, so it is always zero here. `conversion_ratio` is
//! only read by the double-histogram API; it is kept as an opaque value so that an import followed
//! by an export round-trips it.

#![expect(
    clippy::cast_precision_loss,
    reason = "counts and values are converted to f64 exactly where HdrHistogram_c converts them to double"
)]
#![expect(
    clippy::suboptimal_flops,
    reason = "fused or specialized operations would round differently from the C code"
)]

use crate::Error;

/// The bucket layout and recorded counts of a histogram: `struct hdr_histogram`.
#[derive(Debug)]
pub struct Hdr {
    lowest_discernible_value: i64,
    highest_trackable_value: i64,
    unit_magnitude: i32,
    significant_figures: i32,
    sub_bucket_half_count_magnitude: i32,
    sub_bucket_half_count: i32,
    sub_bucket_mask: i64,
    sub_bucket_count: i32,
    min_value: i64,
    max_value: i64,
    counts_len: i32,
    total_count: i64,
    conversion_ratio: f64,
    counts: Vec<i64>,
}

/// The position and derived values of an iterator over the counts array: the common fields of
/// `struct hdr_iter`.
#[derive(Debug, Clone, Copy)]
pub struct IterState {
    counts_index: i32,
    total_count: i64,
    /// The count at the current index.
    pub count: i64,
    /// The sum of the counts up to and including the current index.
    pub cumulative_count: i64,
    /// The lowest value of the current index.
    pub value: i64,
    pub highest_equivalent_value: i64,
    pub lowest_equivalent_value: i64,
    pub median_equivalent_value: i64,
    pub value_iterated_from: i64,
    pub value_iterated_to: i64,
}

/// One step of a recorded-values iteration.
#[derive(Debug, Clone, Copy)]
pub struct RecordedBucket {
    pub value: i64,
    pub count: i64,
    pub cumulative_count: i64,
    pub median_equivalent_value: i64,
}

fn power(base: i64, mut exp: i64) -> i64 {
    let mut result = 1_i64;
    while exp != 0 {
        result *= base;
        exp -= 1;
    }
    result
}

fn buckets_needed_to_cover_value(value: i64, sub_bucket_count: i32, unit_magnitude: i32) -> i32 {
    let mut smallest_untrackable_value = i64::from(sub_bucket_count) << unit_magnitude;
    let mut buckets_needed = 1;
    while smallest_untrackable_value <= value {
        if smallest_untrackable_value > i64::MAX / 2 {
            return buckets_needed + 1;
        }
        smallest_untrackable_value <<= 1;
        buckets_needed += 1;
    }
    buckets_needed
}

fn value_from_index(bucket_index: i32, sub_bucket_index: i32, unit_magnitude: i32) -> i64 {
    i64::from(sub_bucket_index) << (bucket_index + unit_magnitude)
}

impl Hdr {
    /// `hdr_init()`: validates the layout and allocates the counts array.
    pub fn new(
        lowest_discernible_value: i64,
        highest_trackable_value: i64,
        significant_figures: i32,
    ) -> Result<Self, Error> {
        if lowest_discernible_value < 1
            || !(1..=5).contains(&significant_figures)
            || lowest_discernible_value > highest_trackable_value / 2
        {
            return Err(Error::InvalidOptions);
        }

        let largest_value_with_single_unit_resolution =
            2 * power(10, i64::from(significant_figures));
        // C computes both magnitudes as log(x) / log(2), then rounds up and truncates
        // respectively. log2() would round differently for exact powers of two.
        let sub_bucket_count_magnitude =
            ((largest_value_with_single_unit_resolution as f64).ln() / 2_f64.ln()).ceil() as i32;
        let sub_bucket_half_count_magnitude = sub_bucket_count_magnitude.max(1) - 1;
        let unit_magnitude = (lowest_discernible_value as f64).ln() / 2_f64.ln();
        // A positive i64 has a magnitude below 63, so the truncation cannot overflow.
        let unit_magnitude = unit_magnitude as i32;

        let sub_bucket_count = 1_i32 << (sub_bucket_half_count_magnitude + 1);
        let sub_bucket_half_count = sub_bucket_count / 2;

        if unit_magnitude + sub_bucket_half_count_magnitude > 61 {
            return Err(Error::InvalidOptions);
        }

        let sub_bucket_mask = (i64::from(sub_bucket_count) - 1) << unit_magnitude;
        let bucket_count = buckets_needed_to_cover_value(
            highest_trackable_value,
            sub_bucket_count,
            unit_magnitude,
        );
        let counts_len = (bucket_count + 1) * (sub_bucket_count / 2);

        let len = usize::try_from(counts_len).map_err(|_| Error::InvalidOptions)?;
        let mut counts = Vec::new();
        counts
            .try_reserve_exact(len)
            .map_err(|_| Error::OutOfMemory)?;
        counts.resize(len, 0);

        Ok(Self {
            lowest_discernible_value,
            highest_trackable_value,
            unit_magnitude,
            significant_figures,
            sub_bucket_half_count_magnitude,
            sub_bucket_half_count,
            sub_bucket_mask,
            sub_bucket_count,
            min_value: i64::MAX,
            max_value: 0,
            counts_len,
            total_count: 0,
            conversion_ratio: 1.0,
            counts,
        })
    }

    /// A copy of the layout and recorded data. Unlike `Clone`, reports allocation failure.
    pub fn try_clone(&self) -> Result<Self, Error> {
        let mut counts = Vec::new();
        counts
            .try_reserve_exact(self.counts.len())
            .map_err(|_| Error::OutOfMemory)?;
        counts.extend_from_slice(&self.counts);
        Ok(Self { counts, ..*self })
    }

    /// An empty histogram with the same layout.
    pub fn with_same_layout(&self) -> Result<Self, Error> {
        Self::new(
            self.lowest_discernible_value,
            self.highest_trackable_value,
            self.significant_figures,
        )
    }

    pub fn lowest_discernible_value(&self) -> i64 {
        self.lowest_discernible_value
    }

    pub fn highest_trackable_value(&self) -> i64 {
        self.highest_trackable_value
    }

    pub fn significant_figures(&self) -> i32 {
        self.significant_figures
    }

    pub fn counts_len(&self) -> i32 {
        self.counts_len
    }

    pub fn total_count(&self) -> i64 {
        self.total_count
    }

    pub fn counts(&self) -> &[i64] {
        &self.counts
    }

    /// The raw `min_value` field: the smallest nonzero value recorded, or `i64::MAX`.
    pub fn min_value(&self) -> i64 {
        self.min_value
    }

    /// The raw `max_value` field: the largest value recorded, or 0.
    pub fn max_value(&self) -> i64 {
        self.max_value
    }

    pub fn conversion_ratio(&self) -> f64 {
        self.conversion_ratio
    }

    pub fn set_conversion_ratio(&mut self, ratio: f64) {
        self.conversion_ratio = ratio;
    }

    /// Overwrites the raw `min_value` and `max_value` fields, as an import does.
    pub fn set_min_max(&mut self, min_value: i64, max_value: i64) {
        self.min_value = min_value;
        self.max_value = max_value;
    }

    /// The count at an index; out-of-range indexes read as 0, as in `hdr_count_at_index()`.
    pub fn count_at_index(&self, index: i32) -> i64 {
        usize::try_from(index)
            .ok()
            .and_then(|i| self.counts.get(i))
            .copied()
            .unwrap_or(0)
    }

    /// Mutable access to the counts. A caller that changes them must call
    /// `reset_internal_counters()` afterwards.
    pub fn counts_mut(&mut self) -> &mut [i64] {
        &mut self.counts
    }

    /// `hdr_get_memory_size()`, with this struct in place of the C one.
    pub fn memory_size(&self) -> usize {
        size_of::<Self>() + self.counts.len() * size_of::<i64>()
    }

    // Index arithmetic.

    fn bucket_index(&self, value: i64) -> i32 {
        // The smallest power of 2 containing the value. Callers pass non-negative values.
        let leading_zeros = (value | self.sub_bucket_mask)
            .cast_unsigned()
            .leading_zeros()
            .cast_signed();
        let pow2ceiling = 64 - leading_zeros;
        pow2ceiling - self.unit_magnitude - (self.sub_bucket_half_count_magnitude + 1)
    }

    fn sub_bucket_index(&self, value: i64, bucket_index: i32) -> i32 {
        // Bounded by sub_bucket_count, so the truncation is lossless.
        (value >> (bucket_index + self.unit_magnitude)) as i32
    }

    fn counts_index(&self, bucket_index: i32, sub_bucket_index: i32) -> i32 {
        let bucket_base_index = (bucket_index + 1) << self.sub_bucket_half_count_magnitude;
        let offset_in_bucket = sub_bucket_index - self.sub_bucket_half_count;
        bucket_base_index + offset_in_bucket
    }

    fn counts_index_for(&self, value: i64) -> i32 {
        let bucket_index = self.bucket_index(value);
        let sub_bucket_index = self.sub_bucket_index(value, bucket_index);
        self.counts_index(bucket_index, sub_bucket_index)
    }

    /// `hdr_value_at_index()`.
    pub fn value_at_index(&self, index: i32) -> i64 {
        let mut bucket_index = (index >> self.sub_bucket_half_count_magnitude) - 1;
        let mut sub_bucket_index =
            (index & (self.sub_bucket_half_count - 1)) + self.sub_bucket_half_count;
        if bucket_index < 0 {
            sub_bucket_index -= self.sub_bucket_half_count;
            bucket_index = 0;
        }
        value_from_index(bucket_index, sub_bucket_index, self.unit_magnitude)
    }

    fn size_of_range_given_indices(&self, bucket_index: i32, sub_bucket_index: i32) -> i64 {
        let adjusted_bucket = if sub_bucket_index >= self.sub_bucket_count {
            bucket_index + 1
        } else {
            bucket_index
        };
        1_i64 << (self.unit_magnitude + adjusted_bucket)
    }

    /// `hdr_size_of_equivalent_value_range()`.
    pub fn size_of_equivalent_value_range(&self, value: i64) -> i64 {
        let bucket_index = self.bucket_index(value);
        let sub_bucket_index = self.sub_bucket_index(value, bucket_index);
        self.size_of_range_given_indices(bucket_index, sub_bucket_index)
    }

    /// `hdr_lowest_equivalent_value()`.
    pub fn lowest_equivalent_value(&self, value: i64) -> i64 {
        let bucket_index = self.bucket_index(value);
        let sub_bucket_index = self.sub_bucket_index(value, bucket_index);
        value_from_index(bucket_index, sub_bucket_index, self.unit_magnitude)
    }

    /// `highest_equivalent_value()`, saturating at the top of the range.
    pub fn highest_equivalent_value(&self, value: i64) -> i64 {
        let low = self.lowest_equivalent_value(value);
        let size = self.size_of_equivalent_value_range(value);
        if low > i64::MAX - size {
            return i64::MAX;
        }
        low + size - 1
    }

    /// `hdr_median_equivalent_value()`.
    pub fn median_equivalent_value(&self, value: i64) -> i64 {
        self.lowest_equivalent_value(value) + (self.size_of_equivalent_value_range(value) >> 1)
    }

    // Recording.

    fn update_min_max(&mut self, value: i64) {
        if value > self.max_value {
            self.max_value = value;
        }
        if value != 0 && value < self.min_value {
            self.min_value = value;
        }
    }

    fn record_value_counted(&mut self, value: i64, count: i64) -> bool {
        if value < 0 || self.highest_trackable_value < value {
            return false;
        }
        let Ok(index) = usize::try_from(self.counts_index_for(value)) else {
            return false;
        };
        let Some(slot) = self.counts.get_mut(index) else {
            return false;
        };
        // The C code overflows here only with more than 2^63 values recorded; wrap as it does.
        *slot = slot.wrapping_add(count);
        self.total_count = self.total_count.wrapping_add(count);
        self.update_min_max(value);
        true
    }

    /// `hdr_record_value()`.
    pub fn record_value(&mut self, value: i64) -> bool {
        self.record_value_counted(value, 1)
    }

    /// `hdr_record_values()`.
    pub fn record_values(&mut self, value: i64, count: i64) -> bool {
        if count < 0 {
            return false;
        }
        self.record_value_counted(value, count)
    }

    /// `hdr_record_corrected_value()`.
    pub fn record_corrected_value(&mut self, value: i64, expected_interval: i64) -> bool {
        if !self.record_values(value, 1) {
            return false;
        }
        if expected_interval <= 0 || value <= expected_interval {
            return true;
        }
        let mut missing_value = value - expected_interval;
        while missing_value >= expected_interval {
            if !self.record_values(missing_value, 1) {
                return false;
            }
            missing_value -= expected_interval;
        }
        true
    }

    /// `hdr_add()`: records every value of `from`. Returns the number of values that did not fit.
    pub fn add(&mut self, from: &Self) -> i64 {
        let mut dropped = 0_i64;
        for bucket in from.recorded() {
            if !self.record_values(bucket.value, bucket.count) {
                dropped = dropped.wrapping_add(bucket.count);
            }
        }
        dropped
    }

    /// `hdr_reset()`.
    pub fn reset(&mut self) {
        self.total_count = 0;
        self.min_value = i64::MAX;
        self.max_value = 0;
        self.counts.fill(0);
    }

    /// `hdr_reset_internal_counters()`: recomputes the total count, `min_value`, and `max_value`
    /// from the counts.
    pub fn reset_internal_counters(&mut self) {
        let mut min_non_zero_index = -1_i32;
        let mut max_index = -1_i32;
        let mut observed_total_count = 0_i64;

        for (i, &count) in (0_i32..).zip(self.counts.iter()) {
            if count > 0 {
                observed_total_count = if count > i64::MAX - observed_total_count {
                    i64::MAX
                } else {
                    observed_total_count + count
                };
                max_index = i;
                if min_non_zero_index == -1 && i != 0 {
                    min_non_zero_index = i;
                }
            }
        }

        self.max_value = if max_index == -1 {
            0
        } else {
            self.highest_equivalent_value(self.value_at_index(max_index))
        };
        self.min_value = if min_non_zero_index == -1 {
            i64::MAX
        } else {
            self.value_at_index(min_non_zero_index)
        };
        self.total_count = observed_total_count;
    }

    // Queries.

    /// `hdr_min()`.
    pub fn min(&self) -> i64 {
        if self.count_at_index(0) > 0 {
            return 0;
        }
        if self.min_value == i64::MAX {
            return i64::MAX;
        }
        self.lowest_equivalent_value(self.min_value)
    }

    /// `hdr_max()`.
    pub fn max(&self) -> i64 {
        if self.max_value == 0 {
            return 0;
        }
        self.highest_equivalent_value(self.max_value)
    }

    /// `hdr_mean()`. NaN for an empty histogram.
    pub fn mean(&self) -> f64 {
        let mut total = 0.0_f64;
        let mut count = 0_i64;
        let mut iter = self.iter_all();
        while self.move_next(&mut iter) && count < self.total_count {
            if iter.count != 0 {
                count += iter.count;
                total += iter.count as f64 * self.median_equivalent_value(iter.value) as f64;
            }
        }
        total / self.total_count as f64
    }

    /// `hdr_stddev()`: the population standard deviation. NaN for an empty histogram.
    pub fn stddev(&self) -> f64 {
        let mean = self.mean();
        let mut geometric_dev_total = 0.0_f64;
        let mut iter = self.iter_all();
        while self.move_next(&mut iter) {
            if iter.count != 0 {
                let dev = self.median_equivalent_value(iter.value) as f64 - mean;
                geometric_dev_total += (dev * dev) * iter.count as f64;
            }
        }
        (geometric_dev_total / self.total_count as f64).sqrt()
    }

    /// `hdr_count_at_value()`.
    pub fn count_at_value(&self, value: i64) -> i64 {
        if value < 0 {
            return 0;
        }
        self.count_at_index(self.counts_index_for(value))
    }

    fn value_from_index_up_to_count(&self, count_at_percentile: i64) -> i64 {
        let count_at_percentile = count_at_percentile.max(1);
        let mut running = 0_i64;
        for (index, &count) in (0_i32..).zip(self.counts.iter()) {
            running += count;
            if running >= count_at_percentile {
                return self.value_at_index(index);
            }
        }
        0
    }

    /// `hdr_value_at_percentile()`. The target count is rounded half up, not up.
    pub fn value_at_percentile(&self, percentile: f64) -> i64 {
        let requested_percentile = percentile.min(100.0);
        let count_at_percentile =
            ((requested_percentile / 100.0) * self.total_count as f64 + 0.5) as i64;
        let value = self.value_from_index_up_to_count(count_at_percentile);
        if percentile == 0.0 {
            return self.lowest_equivalent_value(value);
        }
        self.highest_equivalent_value(value)
    }

    /// `hdr_value_at_percentiles()`: the values at each percentile in one pass. The percentiles
    /// must be ascending. A position the scan does not reach, which only happens for an empty
    /// histogram or descending input, keeps its target count, as in C.
    pub fn value_at_percentiles(&self, percentiles: &[f64]) -> Vec<i64> {
        let mut values: Vec<i64> = percentiles
            .iter()
            .map(|&p| {
                let requested_percentile = if p < 100.0 { p } else { 100.0 };
                let count_at_percentile =
                    ((requested_percentile / 100.0) * self.total_count as f64 + 0.5) as i64;
                count_at_percentile.max(1)
            })
            .collect();

        let mut total = 0_u64;
        let mut at_pos = 0;
        for (index, &count) in (0_i32..).zip(self.counts.iter()) {
            if at_pos >= values.len() {
                break;
            }
            total = total.wrapping_add(count.cast_unsigned());
            while at_pos < values.len() && total.cast_signed() >= values[at_pos] {
                values[at_pos] = self.highest_equivalent_value(self.value_at_index(index));
                at_pos += 1;
            }
        }
        values
    }

    // Iteration. `IterState` mirrors the fields of `struct hdr_iter` and the functions below
    // mirror the C iterator functions one to one, so that the iterators built on them visit the
    // same values in the same order.

    /// `hdr_iter_init()`.
    pub fn iter_all(&self) -> IterState {
        IterState {
            counts_index: -1,
            total_count: self.total_count,
            count: 0,
            cumulative_count: 0,
            value: 0,
            highest_equivalent_value: 0,
            lowest_equivalent_value: 0,
            median_equivalent_value: 0,
            value_iterated_from: 0,
            value_iterated_to: 0,
        }
    }

    fn has_buckets(&self, iter: &IterState) -> bool {
        iter.counts_index < self.counts_len
    }

    fn has_next(iter: &IterState) -> bool {
        iter.cumulative_count < iter.total_count
    }

    /// `move_next()`: advances to the next index. Returns false, leaving the derived values of
    /// the previous index in place, when there is none.
    pub fn move_next(&self, iter: &mut IterState) -> bool {
        iter.counts_index += 1;
        if !self.has_buckets(iter) {
            return false;
        }
        iter.count = self.count_at_index(iter.counts_index);
        iter.cumulative_count += iter.count;
        let value = self.value_at_index(iter.counts_index);
        let bucket_index = self.bucket_index(value);
        let sub_bucket_index = self.sub_bucket_index(value, bucket_index);
        let leq = value_from_index(bucket_index, sub_bucket_index, self.unit_magnitude);
        let size = self.size_of_range_given_indices(bucket_index, sub_bucket_index);
        iter.lowest_equivalent_value = leq;
        iter.value = value;
        iter.highest_equivalent_value = if leq > i64::MAX - size {
            i64::MAX
        } else {
            leq + size - 1
        };
        iter.median_equivalent_value = leq + (size >> 1);
        true
    }

    fn peek_next_value_from_index(&self, iter: &IterState) -> i64 {
        let index = iter.counts_index + 1;
        let mut bucket_index =
            (index.cast_unsigned() >> self.sub_bucket_half_count_magnitude).cast_signed() - 1;
        let mut sub_bucket_index =
            (index & (self.sub_bucket_half_count - 1)) + self.sub_bucket_half_count;
        if bucket_index < 0 {
            sub_bucket_index -= self.sub_bucket_half_count;
            bucket_index = 0;
        }
        let shift = bucket_index + self.unit_magnitude;
        if shift >= 63
            || u64::from(sub_bucket_index.cast_unsigned()) > (i64::MAX.cast_unsigned() >> shift)
        {
            return i64::MAX;
        }
        value_from_index(bucket_index, sub_bucket_index, self.unit_magnitude)
    }

    fn next_value_greater_than_reporting_level_upper_bound(
        &self,
        iter: &IterState,
        reporting_level_upper_bound: i64,
    ) -> bool {
        if iter.counts_index >= self.counts_len {
            return false;
        }
        self.peek_next_value_from_index(iter) > reporting_level_upper_bound
    }

    fn basic_iter_next(&self, iter: &mut IterState) -> bool {
        if !Self::has_next(iter) || iter.counts_index >= self.counts_len {
            return false;
        }
        self.move_next(iter);
        true
    }

    fn update_iterated_values(iter: &mut IterState, new_value_iterated_to: i64) {
        iter.value_iterated_from = iter.value_iterated_to;
        iter.value_iterated_to = new_value_iterated_to;
    }

    /// The recorded-values iterator (`hdr_iter_recorded_init()`): every index with a nonzero
    /// count, in ascending order.
    pub fn recorded(&self) -> impl Iterator<Item = RecordedBucket> + '_ {
        let mut iter = self.iter_all();
        std::iter::from_fn(move || {
            while self.basic_iter_next(&mut iter) {
                if iter.count != 0 {
                    let value = iter.value;
                    Self::update_iterated_values(&mut iter, value);
                    return Some(RecordedBucket {
                        value: iter.value,
                        count: iter.count,
                        cumulative_count: iter.cumulative_count,
                        median_equivalent_value: iter.median_equivalent_value,
                    });
                }
            }
            None
        })
    }

    /// The percentile iterator (`hdr_iter_percentile_init()`), as `(percentile, value)` pairs
    /// where `value` is the lowest value of the index the iterator is on, which is what Node.js
    /// reports. Like the C iterator, it can report the last index more than once.
    pub fn percentiles(&self, ticks_per_half_distance: i32) -> Vec<(f64, i64)> {
        let mut out = Vec::new();
        let mut iter = self.iter_all();
        let mut seen_last_value = false;
        let mut percentile_to_iterate_to = 0.0_f64;

        loop {
            // percentile_iter_next()
            if !Self::has_next(&iter) {
                if seen_last_value {
                    break;
                }
                seen_last_value = true;
                out.push((100.0, iter.value));
                continue;
            }

            if iter.counts_index == -1 && !self.basic_iter_next(&mut iter) {
                break;
            }

            let mut percentile = None;
            loop {
                let current_percentile =
                    (100.0 * iter.cumulative_count as f64) / self.total_count as f64;
                if iter.count != 0 && percentile_to_iterate_to <= current_percentile {
                    let highest = self.highest_equivalent_value(iter.value);
                    Self::update_iterated_values(&mut iter, highest);
                    percentile = Some(percentile_to_iterate_to);
                    let temp =
                        ((100.0 / (100.0 - percentile_to_iterate_to)).ln() / 2_f64.ln()) as i64;
                    // `as` saturates where the C conversions are undefined; that only happens
                    // when percentile_to_iterate_to rounds to 100.
                    let temp = temp.saturating_add(1);
                    let half_distance = 2_f64.powf(temp as f64) as i64;
                    let percentile_reporting_ticks =
                        i64::from(ticks_per_half_distance).saturating_mul(half_distance);
                    percentile_to_iterate_to += 100.0 / percentile_reporting_ticks as f64;
                    break;
                }
                if !self.basic_iter_next(&mut iter) {
                    break;
                }
            }
            // When the inner loop runs out, C still reports a step, with the percentile left at
            // its previous value.
            let reported = percentile.unwrap_or_else(|| out.last().map_or(0.0, |&(p, _)| p));
            out.push((reported, iter.value));
        }
        out
    }

    /// The linear iterator (`hdr_iter_linear_init()`), as `(value, count)` pairs where `value` is
    /// the lowest value of the index the iterator stopped on and `count` is the number of values
    /// added in the step, as Node.js reports them.
    pub fn linear_buckets(&self, value_units_per_bucket: i64) -> Vec<(i64, i64)> {
        let (mut next_level, mut next_level_lowest_equivalent) = if value_units_per_bucket <= 0 {
            (i64::MAX, i64::MAX)
        } else {
            (
                value_units_per_bucket,
                self.lowest_equivalent_value(value_units_per_bucket),
            )
        };

        let mut out = Vec::new();
        let mut iter = self.iter_all();
        loop {
            // iter_linear_next()
            let mut count_added = 0_i64;
            if !(Self::has_next(&iter)
                || self.next_value_greater_than_reporting_level_upper_bound(
                    &iter,
                    next_level_lowest_equivalent,
                ))
            {
                break;
            }
            loop {
                if iter.value >= next_level_lowest_equivalent {
                    Self::update_iterated_values(&mut iter, next_level);
                    if next_level == i64::MAX {
                        next_level_lowest_equivalent = i64::MAX;
                    } else if value_units_per_bucket <= 0
                        || next_level > i64::MAX - value_units_per_bucket
                    {
                        next_level = i64::MAX;
                        next_level_lowest_equivalent = self.lowest_equivalent_value(i64::MAX);
                    } else {
                        next_level += value_units_per_bucket;
                        next_level_lowest_equivalent = self.lowest_equivalent_value(next_level);
                    }
                    break;
                }
                if !self.move_next(&mut iter) {
                    break;
                }
                count_added += iter.count;
            }
            out.push((iter.value, count_added));
        }
        out
    }

    /// The logarithmic iterator (`hdr_iter_log_init()`), reported as `linear_buckets()` is.
    pub fn log_buckets(&self, value_units_first_bucket: i64, log_base: f64) -> Vec<(i64, i64)> {
        let (mut next_level, mut next_level_lowest_equivalent) = if value_units_first_bucket <= 0
            || !log_base.is_finite()
            || log_base <= 1.0
            || log_base >= i64::MAX as f64
        {
            (i64::MAX, i64::MAX)
        } else {
            (
                value_units_first_bucket,
                self.lowest_equivalent_value(value_units_first_bucket),
            )
        };

        let mut out = Vec::new();
        let mut iter = self.iter_all();
        loop {
            // log_iter_next()
            let mut count_added = 0_i64;
            if !(Self::has_next(&iter)
                || self.next_value_greater_than_reporting_level_upper_bound(
                    &iter,
                    next_level_lowest_equivalent,
                ))
            {
                break;
            }
            loop {
                if iter.value >= next_level_lowest_equivalent {
                    Self::update_iterated_values(&mut iter, next_level);
                    // A finite base below i64::MAX, checked above, so the truncation is defined.
                    let base = log_base as i64;
                    if next_level == i64::MAX {
                        next_level_lowest_equivalent = i64::MAX;
                    } else if base <= 1 || next_level <= 0 || next_level > i64::MAX / base {
                        next_level = i64::MAX;
                        next_level_lowest_equivalent = self.lowest_equivalent_value(i64::MAX);
                    } else {
                        next_level *= base;
                        next_level_lowest_equivalent = self.lowest_equivalent_value(next_level);
                    }
                    break;
                }
                if !self.move_next(&mut iter) {
                    break;
                }
                count_added += iter.count;
            }
            out.push((iter.value, count_added));
        }
        out
    }
}

#[cfg(test)]
#[path = "hdr-test.rs"]
mod tests;
