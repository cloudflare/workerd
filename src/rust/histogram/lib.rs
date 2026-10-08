// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! HDR histograms with the behavior of Node.js's `perf_hooks` histograms.
//!
//! [`Histogram`] matches Node.js's native histogram (`src/histogram.{h,cc}` in Node.js): the same
//! bucket layout, the same results for every query including edge cases, and the same rounding.
//! It backs the statistics of the `workerd bench` harness and `node:perf_hooks`'
//! `createHistogram()`.
//!
//! [`hdr`] is a port of the core of `HdrHistogram_c`, the C library Node.js builds on.

mod cbor;
pub mod hdr;
mod histogram;
mod stats;

pub use histogram::Histogram;
pub use histogram::Options;
pub use stats::MannWhitneyTest;
pub use stats::MeanCi;
pub use stats::PercentileCi;
pub use stats::WelchTest;

/// Errors from creating, combining, or querying histograms.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum Error {
    /// The `lowest`, `highest`, and `figures` options do not describe a valid layout.
    #[error("Invalid histogram options")]
    InvalidOptions,
    /// The counts array could not be allocated.
    #[error("Out of memory allocating histogram")]
    OutOfMemory,
    /// An argument is out of range.
    #[error("{0}")]
    InvalidArgument(&'static str),
    /// Two histograms that must have the same layout do not.
    #[error("other must have the same configuration as the histogram")]
    Incompatible,
    /// Values were removed from a histogram after an earlier snapshot of it was taken.
    #[error("Values were removed from the histogram after other was taken")]
    Reset,
    /// A supposedly earlier snapshot holds values that the histogram does not.
    #[error("other contains values that are not in the histogram")]
    NotEarlier,
    /// The input to `Histogram::import()` is not valid export data.
    #[error("Invalid histogram export data")]
    InvalidExportData,
}
