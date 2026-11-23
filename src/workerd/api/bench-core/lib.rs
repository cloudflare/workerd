// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The parts of `workerd bench` that need no JSG, so that both the worker side and the server can
//! use them.
//!
//! This crate root turns the samples that the runner took for a group's cases into a
//! `BenchReport.Group` (bench.capnp), with summary statistics computed after subtracting the
//! runner's own overhead and flags for results that can't be trusted. The [`startup`] module
//! times a Worker's startup and reports it the same way, [`clock`] reads the clocks both time
//! with, and [`GlobFilter`] is the case filter.
//!
//! The group's name is left unset, since the caller knows it.
//!
//! Names and errors are bytes, written to the report's `Text` fields unchanged: they come from
//! JavaScript strings, which needn't be valid UTF-16, as WTF-8, as C++ JSG converts them to a
//! `kj::String`, so a Rust `String` couldn't hold all of them.
//! The [`startup`] module makes its own phase names, and gets a failed start's error as a
//! `String`.

pub mod clock;
mod glob_filter;
pub mod startup;

use bench_capnp::bench_report;
use bench_capnp::bench_report::case::Flag;
use bench_capnp::bench_report::case::Status;
use histogram::Histogram;
use histogram::Options;

pub use crate::glob_filter::GlobFilter;

/// The sampling budget, when neither the case nor the command line sets one.
pub const DEFAULT_MIN_TIME_NS: u64 = 1_000_000_000;
/// The number of samples, when neither the case nor the command line sets one.
pub const DEFAULT_SAMPLES: u32 = 50;

/// Under `--quick`, the sampling budget and the number of samples are capped at these.
pub const QUICK_MIN_TIME_NS: u64 = 10_000_000;
pub const QUICK_SAMPLES: u32 = 5;

/// The most samples a case can ask for.
pub const MAX_SAMPLES: u32 = 1_000_000;

/// A reading of the monotonic and thread CPU clocks ([`clock::now`]), or the difference of two,
/// which is how long the code between them took, in nanoseconds.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Timing {
    pub wall_ns: i64,
    pub cpu_ns: i64,
}

impl std::ops::Sub for Timing {
    type Output = Self;

    fn sub(self, start: Self) -> Self {
        Self {
            wall_ns: self.wall_ns - start.wall_ns,
            cpu_ns: self.cpu_ns - start.cpu_ns,
        }
    }
}

/// Samples are recorded into histograms in picoseconds, up to an hour per call.
const HISTOGRAM_HIGHEST_PS: i64 = 3_600_000_000_000_000;
const HISTOGRAM_FIGURES: i32 = 3;
const CONFIDENCE: f64 = 0.95;

/// A case is flagged as high-variance when the coefficient of variation of its wall time per call
/// exceeds this.
const HIGH_VARIANCE_CV: f64 = 0.1;

/// A case is flagged as at the measurement floor when its median wall time before subtracting the
/// overhead is less than this multiple of the overhead.
const AT_FLOOR_FACTOR: f64 = 2.0;

/// The error of a case with a sample longer than the histograms hold.
const TOO_LONG: &str =
    "A call took longer than an hour, the longest that the statistics can represent.";

/// The runner's own cost per call; see `BenchReport.Overhead`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Overhead {
    pub sync_ns: f64,
    pub sync_cpu_ns: f64,
    pub async_ns: f64,
    pub async_cpu_ns: f64,
    pub black_box_ns: f64,
}

/// The results of one case, before they are written to a `BenchReport.Case`.
#[derive(Debug, Clone)]
pub struct CaseResult {
    pub name: Vec<u8>,
    pub status: Status,
    pub error: Option<Vec<u8>>,
    pub iterations_per_sample: u64,
    /// Wall and thread CPU time per call, one value per sampled batch, including the runner's
    /// overhead.
    pub wall_ns: Vec<f64>,
    pub cpu_ns: Vec<f64>,
    /// Whether the function returned a promise, which selects the overhead to subtract.
    pub returns_promise: bool,
}

/// The results of one group.
#[derive(Debug, Clone, Default)]
pub struct GroupResult {
    /// Set if the group itself failed, such as a `bench()` handler that threw.
    pub error: Option<Vec<u8>>,
    pub cases: Vec<CaseResult>,
    /// Set once overhead has been measured.
    pub overhead: Option<Overhead>,
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error(transparent)]
    Histogram(#[from] histogram::Error),
    #[error(transparent)]
    Capnp(#[from] capnp::Error),
    /// A sample that the histogram didn't record, though `fill_group` checked that it is in
    /// range.
    #[error("A sample of {0} ps is out of the histogram's range.")]
    NotRecorded(i64),
}

/// The median of `samples`, or NaN if `samples` is empty.
#[must_use]
pub fn median(samples: &[f64]) -> f64 {
    let mut sorted = samples.to_vec();
    sorted.sort_by(f64::total_cmp);
    let middle = sorted.len() / 2;
    match sorted.len() {
        0 => f64::NAN,
        len if len % 2 == 1 => sorted[middle],
        _ => f64::midpoint(sorted[middle - 1], sorted[middle]),
    }
}

/// Writes `result` to `group`.
pub fn fill_group(
    result: &GroupResult,
    mut group: bench_report::group::Builder<'_>,
) -> Result<(), Error> {
    if let Some(error) = &result.error {
        group.set_error(capnp::text::Reader::from(error.as_slice()));
    }
    if let Some(o) = result.overhead {
        let mut overhead = group.reborrow().init_overhead();
        overhead.set_sync_ns(o.sync_ns);
        overhead.set_sync_cpu_ns(o.sync_cpu_ns);
        overhead.set_async_ns(o.async_ns);
        overhead.set_async_cpu_ns(o.async_cpu_ns);
        overhead.set_black_box_ns(o.black_box_ns);
    }
    let mut cases = group.init_cases(list_len(&result.cases));
    for (index, from) in (0..).zip(&result.cases) {
        fill_case(from, result.overhead, cases.reborrow().get(index))?;
    }
    Ok(())
}

/// `result` as an encoded `BenchReport.Group` message (segment table, then segments) in words.
pub fn encode_group(result: &GroupResult) -> Result<Vec<u64>, Error> {
    let mut message = capnp::message::TypedBuilder::<bench_report::group::Owned>::new_default();
    fill_group(result, message.init_root())?;
    to_words(&message)
}

/// `message`, encoded (segment table, then segments) in words.
///
/// The message is written straight into a buffer of its final size, with no intermediate copy as
/// bytes, which matters for large groups.
fn to_words(
    message: &capnp::message::TypedBuilder<bench_report::group::Owned>,
) -> Result<Vec<u64>, Error> {
    let message = message.borrow_inner();
    let mut words = Vec::with_capacity(capnp::serialize::compute_serialized_size_in_words(message));
    capnp::serialize::write_message(Words(&mut words), message)?;
    Ok(words)
}

/// A `capnp::io::Write` that appends to a vector of words. The serializer writes the segment table
/// and each segment in whole words.
struct Words<'a>(&'a mut Vec<u64>);

impl capnp::io::Write for Words<'_> {
    fn write_all(&mut self, buf: &[u8]) -> capnp::Result<()> {
        let chunks = buf.chunks_exact(8);
        if !chunks.remainder().is_empty() {
            return Err(capnp::Error::failed(format!(
                "A write of {} bytes isn't a whole number of words.",
                buf.len()
            )));
        }
        self.0.extend(chunks.map(|chunk| {
            let mut word = [0; 8];
            word.copy_from_slice(chunk);
            u64::from_ne_bytes(word)
        }));
        Ok(())
    }
}

fn fill_case(
    from: &CaseResult,
    overhead: Option<Overhead>,
    mut to: bench_report::case::Builder<'_>,
) -> Result<(), Error> {
    to.set_name(capnp::text::Reader::from(from.name.as_slice()));
    to.set_status(from.status);
    if let Some(error) = &from.error {
        to.set_error(capnp::text::Reader::from(error.as_slice()));
    }
    if from.status != Status::Ok {
        return Ok(());
    }
    if exceeds_histogram(&from.wall_ns) || exceeds_histogram(&from.cpu_ns) {
        // The histograms can't record such a sample; fail the case rather than leave it out.
        to.set_status(Status::Failed);
        to.set_error(TOO_LONG);
        return Ok(());
    }
    to.set_iterations_per_sample(from.iterations_per_sample);

    let (wall_overhead, cpu_overhead) = overhead.map_or((0.0, 0.0), |o| {
        if from.returns_promise {
            (o.async_ns, o.async_cpu_ns)
        } else {
            (o.sync_ns, o.sync_cpu_ns)
        }
    });
    let subtract = |samples: &[f64], overhead: f64| -> Vec<f64> {
        samples
            .iter()
            .map(|sample| (sample - overhead).max(0.0))
            .collect()
    };
    let wall = fill_metric(
        &subtract(&from.wall_ns, wall_overhead),
        to.reborrow().init_wall_ns(),
    )?;
    fill_metric(
        &subtract(&from.cpu_ns, cpu_overhead),
        to.reborrow().init_cpu_ns(),
    )?;

    let flag = if wall_overhead > 0.0 && median(&from.wall_ns) < AT_FLOOR_FACTOR * wall_overhead {
        Some(Flag::AtFloor)
    } else if wall.mean > 0.0 && wall.stddev / wall.mean > HIGH_VARIANCE_CV {
        // At the floor, what's left after subtracting the overhead is mostly noise, so its variance
        // means nothing.
        Some(Flag::HighVariance)
    } else {
        None
    };
    if let Some(flag) = flag {
        to.init_flags(1).set(0, flag);
    }
    Ok(())
}

/// Whether any of `samples` is longer than the histograms hold.
fn exceeds_histogram(samples: &[f64]) -> bool {
    #[expect(
        clippy::cast_precision_loss,
        reason = "the bound is a round number of picoseconds, exact in an f64"
    )]
    let highest = HISTOGRAM_HIGHEST_PS as f64;
    samples.iter().any(|ns| ns * 1000.0 > highest)
}

/// Writes `samples` and their statistics to `metric`, and returns the statistics.
fn fill_metric(
    samples: &[f64],
    mut metric: bench_report::metric::Builder<'_>,
) -> Result<histogram::ffi::Summary, Error> {
    let mut list = metric.reborrow().init_samples(list_len(samples));
    for (index, &sample) in (0..).zip(samples) {
        list.set(index, sample);
    }

    let mut histogram = Histogram::new(&Options {
        lowest: 1,
        highest: HISTOGRAM_HIGHEST_PS,
        figures: HISTOGRAM_FIGURES,
        ..Options::default()
    })?;
    for &sample in samples {
        // fill_case() fails cases with samples out of range, so every sample is recorded.
        let ps = (sample.max(0.0) * 1000.0).round() as i64;
        if !histogram.record(ps) {
            return Err(Error::NotRecorded(ps));
        }
    }
    let summary = histogram::ffi::summarize(&histogram, CONFIDENCE);

    #[expect(
        clippy::cast_precision_loss,
        reason = "values up to an hour in picoseconds, 3.6e18, keep their three significant \
                  figures in an f64"
    )]
    let ns = |ps: i64| ps as f64 / 1000.0;
    metric.set_mean(summary.mean / 1000.0);
    metric.set_mean_low(summary.mean_lower / 1000.0);
    metric.set_mean_high(summary.mean_upper / 1000.0);
    metric.set_median(ns(summary.median));
    metric.set_median_low(ns(summary.median_lower));
    metric.set_median_high(ns(summary.median_upper));
    metric.set_p75(ns(summary.p75));
    metric.set_p99(ns(summary.p99));
    metric.set_min(ns(summary.min));
    metric.set_max(ns(summary.max));
    metric.set_stddev(summary.stddev / 1000.0);
    metric.set_skewness(summary.skewness);
    metric.set_kurtosis(summary.kurtosis);
    Ok(summary)
}

/// The length of a capnp list of `items`: a group has at most `MAX_SAMPLES` (1,000,000) samples
/// per case and as many cases as a handler registers, far below 2^29, capnp's limit.
fn list_len<T>(items: &[T]) -> u32 {
    items.len() as u32
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
