// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `node-internal:histogram`, the native half of `node:perf_hooks`' `createHistogram()`.
//!
//! A thin layer over the `histogram` crate that mirrors the `Histogram` handle of Node.js's
//! `internalBinding('performance')`. The TypeScript layer
//! (`src/node/internal/perf_hooks_histogram.ts`) validates the arguments.

#![expect(
    clippy::cast_precision_loss,
    reason = "values are converted to f64 exactly where Node.js converts them to double"
)]

use std::cell::RefCell;

use histogram::Dequantization;
use histogram::Options;
use jsg::ToJsTypedArray;
use jsg::v8;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

fn to_jsg_error(error: &histogram::Error) -> jsg::Error {
    jsg::Error::new_range_error(error.to_string())
}

/// The result of `percentilesBigInt()`: the percentiles and their values, in iteration order.
#[jsg_struct]
#[derive(Debug)]
pub struct PercentilesBigInt {
    pub percentiles: Vec<f64>,
    pub values: Vec<i64>,
}

/// The result of `qrde()`. The TypeScript layer adds the probabilities and the dequantization mode.
#[jsg_struct]
#[derive(Debug)]
pub struct QrdeResult {
    pub quantiles: Vec<f64>,
    pub densities: Vec<f64>,
    pub count: u64,
    /// The number of buckets with a nonzero count.
    pub buckets: f64,
    pub corrections: f64,
}

/// Why `diff()` failed, for the TypeScript layer to throw Node.js's error for it.
const DIFF_OK: u32 = 0;
const DIFF_INCOMPATIBLE: u32 = 1;
const DIFF_RESET: u32 = 2;
const DIFF_NOT_EARLIER: u32 = 3;

/// The result of `diff()`: the histogram, or `error` saying why there is none.
#[jsg_struct]
#[derive(Debug)]
pub struct DiffResult {
    pub histogram: Option<jsg::Rc<Histogram>>,
    pub error: u32,
}

/// Flattens `(value, count)` pairs into `[value, count, ...]`, as numbers.
fn flatten_buckets(buckets: Vec<(i64, i64)>) -> Vec<f64> {
    let mut flat = Vec::with_capacity(2 * buckets.len());
    for (value, count) in buckets {
        flat.extend([value as f64, count as f64]);
    }
    flat
}

/// A recordable HDR histogram.
#[jsg_resource(custom_trace)]
pub struct Histogram {
    inner: RefCell<histogram::Histogram>,
    // Reports `inner`, whose counts array does not change size, to V8 until the histogram is
    // dropped.
    #[cfg_attr(not(test), expect(dead_code, reason = "held, not read"))]
    external_memory: jsg::ExternalMemoryAdjustment,
}

// A histogram holds only counts and statistics.
impl jsg::Traced for Histogram {
    fn trace(&self, _visitor: &mut jsg::GcVisitor) {}
}

impl Histogram {
    fn from_inner(lock: &mut jsg::Lock<'_>, inner: histogram::Histogram) -> Self {
        let external_memory = lock.external_memory_adjustment(inner.memory_size());
        Self {
            inner: RefCell::new(inner),
            external_memory,
        }
    }

    fn wrap(lock: &mut jsg::Lock<'_>, inner: histogram::Histogram) -> jsg::Rc<Self> {
        let histogram = Self::from_inner(lock, inner);
        jsg::Rc::new(lock, histogram)
    }
}

#[jsg_resource]
impl Histogram {
    /// `threshold` is any number of at least 0. It is truncated to an integer, saturating, as
    /// Node.js casts it.
    ///
    /// # Errors
    /// A `RangeError` if the options do not describe a valid layout.
    #[jsg_constructor]
    #[expect(
        clippy::cast_possible_truncation,
        reason = "`threshold` truncates and saturates as in Node.js"
    )]
    fn constructor(
        lock: &mut jsg::Lock<'_>,
        lowest: i64,
        highest: i64,
        figures: i32,
        half_life: f64,
        threshold: f64,
    ) -> jsg::Result<Self> {
        let options = Options {
            lowest,
            highest,
            figures,
            half_life,
            threshold: threshold as i64,
        };
        let inner = histogram::Histogram::new(&options).map_err(|e| to_jsg_error(&e))?;
        Ok(Self::from_inner(lock, inner))
    }

    #[jsg_method]
    pub fn count(&self) -> f64 {
        self.inner.borrow().count() as f64
    }

    #[jsg_method]
    pub fn count_big_int(&self) -> u64 {
        self.inner.borrow().count()
    }

    #[jsg_method]
    pub fn exceeds(&self) -> f64 {
        self.inner.borrow().exceeds() as f64
    }

    #[jsg_method]
    pub fn exceeds_big_int(&self) -> u64 {
        self.inner.borrow().exceeds()
    }

    #[jsg_method]
    pub fn reset_count(&self) -> f64 {
        self.inner.borrow().reset_count() as f64
    }

    #[jsg_method]
    pub fn min(&self) -> f64 {
        self.inner.borrow().min() as f64
    }

    #[jsg_method]
    pub fn min_big_int(&self) -> i64 {
        self.inner.borrow().min()
    }

    #[jsg_method]
    pub fn max(&self) -> f64 {
        self.inner.borrow().max() as f64
    }

    #[jsg_method]
    pub fn max_big_int(&self) -> i64 {
        self.inner.borrow().max()
    }

    #[jsg_method]
    pub fn mean(&self) -> f64 {
        self.inner.borrow().mean()
    }

    #[jsg_method]
    pub fn stddev(&self) -> f64 {
        self.inner.borrow().stddev()
    }

    #[jsg_method]
    pub fn skewness(&self) -> f64 {
        self.inner.borrow().skewness()
    }

    #[jsg_method]
    pub fn kurtosis(&self) -> f64 {
        self.inner.borrow().kurtosis()
    }

    #[jsg_method]
    pub fn ewma_mean(&self) -> f64 {
        self.inner.borrow().ewma_mean()
    }

    #[jsg_method]
    pub fn ewma_stddev(&self) -> f64 {
        self.inner.borrow().ewma_stddev()
    }

    #[jsg_method]
    pub fn ewma_error_rate(&self) -> f64 {
        self.inner.borrow().ewma_error_rate()
    }

    /// # Errors
    /// A `RangeError` if `percentile` is not in (0, 100].
    #[jsg_method]
    pub fn percentile(&self, percentile: f64) -> jsg::Result<f64> {
        self.percentile_big_int(percentile)
            .map(|value| value as f64)
    }

    /// # Errors
    /// A `RangeError` if `percentile` is not in (0, 100].
    #[jsg_method]
    pub fn percentile_big_int(&self, percentile: f64) -> jsg::Result<i64> {
        self.inner
            .borrow()
            .percentile(percentile)
            .map_err(|e| to_jsg_error(&e))
    }

    /// The percentile distribution as a flat array of `[percentile, value, ...]` pairs, in
    /// iteration order. A percentile can repeat; a `Map` built from the pairs keeps the last value.
    #[jsg_method]
    pub fn percentiles(&self) -> Vec<f64> {
        let percentiles = self.inner.borrow().percentiles();
        let mut pairs = Vec::with_capacity(2 * percentiles.len());
        for (percentile, value) in percentiles {
            pairs.extend([percentile, value as f64]);
        }
        pairs
    }

    /// The percentile distribution of `percentiles()`, with the values without loss of precision.
    #[jsg_method]
    pub fn percentiles_big_int(&self) -> PercentilesBigInt {
        let (percentiles, values) = self.inner.borrow().percentiles().into_iter().unzip();
        PercentilesBigInt {
            percentiles,
            values,
        }
    }

    /// Records a number, which the TypeScript layer has checked is a safe integer of at least 0,
    /// so the conversion is exact. Taking an `f64` gives the method a Fast API callback.
    #[jsg_method]
    #[expect(clippy::cast_possible_truncation, reason = "`value` is a safe integer")]
    pub fn record(&self, value: f64) {
        self.inner.borrow_mut().record(value as i64);
    }

    #[jsg_method]
    pub fn record_big_int(&self, value: i64) {
        self.inner.borrow_mut().record(value);
    }

    /// Records the time since the previous call; `now` is the current time in milliseconds since
    /// the epoch, from `Date.now()`.
    #[jsg_method]
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "`now` is a positive integral number of milliseconds"
    )]
    pub fn record_delta(&self, now: f64) {
        self.inner
            .borrow_mut()
            .record_delta((now as u64).saturating_mul(1_000_000));
    }

    /// Runs `f` on this histogram, mutably, and `other`. When `other` is this histogram, `f` gets
    /// a copy of it, which gives the results Node.js gives for an operation on itself.
    fn update_with(
        &self,
        lock: &jsg::Lock<'_>,
        other: &jsg::Rc<Self>,
        f: impl FnOnce(&mut ::histogram::Histogram, &::histogram::Histogram),
    ) -> jsg::Result<()> {
        let other = other.get(lock);
        if std::ptr::eq(self, other) {
            let copy = self
                .inner
                .borrow()
                .snapshot()
                .map_err(|e| to_jsg_error(&e))?;
            f(&mut self.inner.borrow_mut(), &copy);
        } else {
            f(&mut self.inner.borrow_mut(), &other.inner.borrow());
        }
        Ok(())
    }

    /// # Errors
    /// Fails if `other` is this histogram and it cannot be copied.
    #[jsg_method]
    pub fn add(&self, lock: &mut jsg::Lock<'_>, other: jsg::Rc<Self>) -> jsg::Result<()> {
        self.update_with(lock, &other, |h, other| {
            h.add(other);
        })
    }

    /// # Errors
    /// Fails if `other` is this histogram and it cannot be copied.
    #[jsg_method]
    pub fn subtract(&self, lock: &mut jsg::Lock<'_>, other: jsg::Rc<Self>) -> jsg::Result<()> {
        self.update_with(lock, &other, |h, other| {
            h.subtract(other);
        })
    }

    /// Records a value with coordinated omission correction. Both are safe integers, checked by
    /// the TypeScript layer.
    #[jsg_method]
    #[expect(clippy::cast_possible_truncation, reason = "both are safe integers")]
    pub fn record_corrected(&self, value: f64, expected_interval: f64) {
        self.inner
            .borrow_mut()
            .record_corrected(value as i64, expected_interval as i64);
    }

    #[jsg_method]
    pub fn record_corrected_big_int(&self, value: i64, expected_interval: i64) {
        self.inner
            .borrow_mut()
            .record_corrected(value, expected_interval);
    }

    /// `value` is truncated to an integer, saturating, as Node.js casts it.
    #[jsg_method]
    #[expect(
        clippy::cast_possible_truncation,
        reason = "`value` truncates and saturates as in Node.js"
    )]
    pub fn count_at(&self, value: f64) -> f64 {
        self.inner.borrow().count_at(value as i64) as f64
    }

    /// `value` is truncated to an integer, saturating, as Node.js casts it.
    #[jsg_method]
    #[expect(
        clippy::cast_possible_truncation,
        reason = "`value` truncates and saturates as in Node.js"
    )]
    pub fn cdf(&self, value: f64) -> f64 {
        self.inner.borrow().cdf(value as i64)
    }

    /// The values at `percentiles`, which are ascending and in (0, 100].
    #[jsg_method]
    pub fn percentiles_at(&self, percentiles: Vec<f64>) -> Vec<f64> {
        self.inner
            .borrow()
            .percentiles_at(&percentiles)
            .into_iter()
            .map(|value| value as f64)
            .collect()
    }

    /// The counts in steps of `step_size`, as flat `[value, count, ...]` pairs.
    #[jsg_method]
    #[expect(
        clippy::cast_possible_truncation,
        reason = "`step_size` is a safe integer"
    )]
    pub fn linear_buckets(&self, step_size: f64) -> Vec<f64> {
        flatten_buckets(self.inner.borrow().linear_buckets(step_size as i64))
    }

    /// The counts in buckets that grow by `base`, as flat `[value, count, ...]` pairs.
    #[jsg_method]
    #[expect(
        clippy::cast_possible_truncation,
        reason = "`first_bucket` is a safe integer"
    )]
    pub fn log_buckets(&self, first_bucket: f64, base: f64) -> Vec<f64> {
        flatten_buckets(self.inner.borrow().log_buckets(first_bucket as i64, base))
    }

    /// `[mean, lower, upper]`.
    #[jsg_method(name = "meanCI")]
    pub fn mean_ci(&self, confidence: f64) -> Vec<f64> {
        let ci = self.inner.borrow().mean_ci(confidence);
        vec![ci.mean, ci.lower, ci.upper]
    }

    /// `[value, lower, upper]`.
    #[jsg_method(name = "percentileCI")]
    pub fn percentile_ci(&self, percentile: f64, confidence: f64) -> Vec<f64> {
        let ci = self.inner.borrow().percentile_ci(percentile, confidence);
        vec![ci.value as f64, ci.lower as f64, ci.upper as f64]
    }

    #[jsg_method]
    pub fn ks_test(&self, lock: &mut jsg::Lock<'_>, other: jsg::Rc<Self>) -> f64 {
        self.inner.borrow().ks_test(&other.get(lock).inner.borrow())
    }

    #[jsg_method]
    pub fn cohens_d(&self, lock: &mut jsg::Lock<'_>, other: jsg::Rc<Self>) -> f64 {
        self.inner
            .borrow()
            .cohens_d(&other.get(lock).inner.borrow())
    }

    #[jsg_method]
    pub fn cliffs_d(&self, lock: &mut jsg::Lock<'_>, other: jsg::Rc<Self>) -> f64 {
        self.inner
            .borrow()
            .cliffs_d(&other.get(lock).inner.borrow())
    }

    /// `[tStatistic, degreesOfFreedom, pValue, ciLower, ciUpper]`.
    #[jsg_method]
    pub fn welch_test(
        &self,
        lock: &mut jsg::Lock<'_>,
        other: jsg::Rc<Self>,
        confidence: f64,
    ) -> Vec<f64> {
        let test = self
            .inner
            .borrow()
            .welch_test(&other.get(lock).inner.borrow(), confidence);
        vec![
            test.t_statistic,
            test.degrees_of_freedom,
            test.p_value,
            test.ci_lower,
            test.ci_upper,
        ]
    }

    /// `[uStatistic, zScore, pValue]`.
    #[jsg_method]
    pub fn mann_whitney_test(&self, lock: &mut jsg::Lock<'_>, other: jsg::Rc<Self>) -> Vec<f64> {
        let test = self
            .inner
            .borrow()
            .mann_whitney_test(&other.get(lock).inner.borrow());
        vec![test.u_statistic, test.z_score, test.p_value]
    }

    /// A quantile-respectful density estimate. `mode` is 0 for no dequantization, 1 for `hdr` and
    /// 2 for `all`; the TypeScript layer validates `probabilities`.
    ///
    /// # Errors
    /// A `RangeError` if `probabilities` is not valid.
    #[jsg_method]
    #[expect(
        clippy::cast_precision_loss,
        reason = "Node.js converts the counts to double"
    )]
    pub fn qrde(&self, probabilities: Vec<f64>, mode: u32) -> jsg::Result<QrdeResult> {
        let dequantization = match mode {
            0 => Dequantization::None,
            1 => Dequantization::Hdr,
            _ => Dequantization::All,
        };
        let h = self.inner.borrow();
        let result = h
            .qrde(&probabilities, dequantization)
            .map_err(|e| to_jsg_error(&e))?;
        Ok(QrdeResult {
            quantiles: result.quantiles,
            densities: result.densities,
            count: result.count,
            buckets: result.bucket_count as f64,
            corrections: result.corrections as f64,
        })
    }

    /// An independent copy of this histogram.
    ///
    /// # Errors
    /// A `RangeError` if the copy cannot be allocated.
    #[jsg_method]
    pub fn snapshot(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<jsg::Rc<Self>> {
        let inner = self
            .inner
            .borrow()
            .snapshot()
            .map_err(|e| to_jsg_error(&e))?;
        Ok(Self::wrap(lock, inner))
    }

    /// The values recorded after `other`, an earlier snapshot of this histogram, was taken.
    ///
    /// # Errors
    /// A `RangeError` if the result cannot be allocated.
    #[jsg_method]
    pub fn diff(&self, lock: &mut jsg::Lock<'_>, other: jsg::Rc<Self>) -> jsg::Result<DiffResult> {
        let diff = self.inner.borrow().diff(&other.get(lock).inner.borrow());
        let error = match diff {
            Ok(inner) => {
                return Ok(DiffResult {
                    histogram: Some(Self::wrap(lock, inner)),
                    error: DIFF_OK,
                });
            }
            Err(::histogram::Error::Incompatible) => DIFF_INCOMPATIBLE,
            Err(::histogram::Error::Reset) => DIFF_RESET,
            Err(::histogram::Error::NotEarlier) => DIFF_NOT_EARLIER,
            Err(e) => return Err(to_jsg_error(&e)),
        };
        Ok(DiffResult {
            histogram: None,
            error,
        })
    }

    /// The histogram in Node.js's export format.
    #[jsg_method]
    pub fn export<'s>(&self, lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Uint8Array> {
        self.inner.borrow().export().to_js_typed_array(lock)
    }

    #[jsg_method]
    pub fn reset(&self) {
        self.inner.borrow_mut().reset();
    }
}

/// The `node-internal:histogram` module object.
#[jsg_resource]
pub struct HistogramModule;

#[jsg_resource(nested_types(Histogram))]
impl HistogramModule {
    pub fn new(lock: &mut jsg::Lock<'_>) -> jsg::Rc<Self> {
        jsg::Rc::new(lock, Self {})
    }

    /// A histogram from Node.js's export format, or `undefined` if `data` is not valid.
    #[jsg_method(name = "import")]
    pub fn import_histogram<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        data: v8::Local<'s, v8::Uint8Array>,
    ) -> Option<jsg::Rc<Histogram>> {
        let inner = ::histogram::Histogram::import(data.as_slice(lock)).ok()?;
        Some(Histogram::wrap(lock, inner))
    }
}

#[cfg(test)]
#[path = "histogram-test.rs"]
mod tests;
