// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Sparklines of a case's samples, for `workerd bench`'s table.
//!
//! A sparkline has [`WIDTH`] equal-width columns spanning the samples' minimum to their maximum,
//! each as tall as the share of the samples that falls in it, relative to the fullest column.
//!
//! The share of a column is either counted ([`Binning::Linear`]) or taken from a
//! quantile-respectful density estimate of the samples ([`Binning::Qrde`]), which smooths the
//! few dozen samples a case has into the shape of their distribution.

#![expect(
    clippy::cast_precision_loss,
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "column indexes and picosecond counts are far below 2^52, and are clamped to range"
)]

use histogram::Dequantization;
use histogram::Histogram;
use histogram::Qrde;

use crate::Result;

/// The number of characters in a sparkline.
pub const WIDTH: usize = 16;

/// The number of bins of the density estimate, which are spread over the columns.
const QRDE_BINS: u32 = 16;

/// The highest sample the density estimate can hold, in picoseconds: one hour, as for the
/// statistics the C++ side computes (`HISTOGRAM_HIGHEST_PS` in `api/bench.c++`).
const HIGHEST_PS: i64 = 3_600_000_000_000_000;

/// Which samples of each case get a sparkline.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Metrics {
    Wall,
    Cpu,
    Both,
}

impl Metrics {
    #[must_use]
    pub const fn wall(self) -> bool {
        matches!(self, Self::Wall | Self::Both)
    }

    #[must_use]
    pub const fn cpu(self) -> bool {
        matches!(self, Self::Cpu | Self::Both)
    }
}

/// How a sparkline divides the samples among its columns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Binning {
    /// The number of samples in each column.
    Linear,
    /// The probability mass in each column of a quantile-respectful density estimate (Node.js's
    /// `histogram.qrde()`) of the samples.
    Qrde,
}

/// The characters a sparkline is drawn with.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Charset {
    /// Block elements, from `▁` to `█`.
    Unicode,
    /// ASCII characters, from `.` to `@`.
    Ascii,
}

impl Charset {
    /// The characters for the process's locale (see [`Self::for_locale`]).
    #[must_use]
    pub fn from_env() -> Self {
        let locale = ["LC_ALL", "LC_CTYPE", "LANG"]
            .into_iter()
            .filter_map(std::env::var_os)
            .find(|value| !value.is_empty());
        Self::for_locale(locale.as_deref().and_then(std::ffi::OsStr::to_str))
    }

    /// The characters for `locale`, the first of `LC_ALL`, `LC_CTYPE` and `LANG` that is set:
    /// ASCII if it names an encoding other than UTF-8; otherwise, as for no locale at all (as on
    /// Windows), Unicode.
    #[must_use]
    pub fn for_locale(locale: Option<&str>) -> Self {
        let Some(locale) = locale else {
            return Self::Unicode;
        };
        let lower = locale.to_ascii_lowercase();
        if lower.contains("utf-8") || lower.contains("utf8") {
            Self::Unicode
        } else {
            Self::Ascii
        }
    }

    /// The characters for columns from the shortest to the tallest.
    const fn levels(self) -> &'static [char; 8] {
        match self {
            Self::Unicode => &['▁', '▂', '▃', '▄', '▅', '▆', '▇', '█'],
            Self::Ascii => &['.', ':', '-', '=', '+', '*', '#', '@'],
        }
    }
}

/// Which sparklines the table has, and how they are drawn.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Options {
    pub metrics: Metrics,
    pub binning: Binning,
    pub charset: Charset,
}

/// The sparkline of `samples`, durations in nanoseconds.
///
/// Only the finite samples count. The sparkline is [`WIDTH`] characters from the smallest to the
/// largest, or a single full-height character if they are all the same; empty if there are none.
pub fn render(samples: &[f64], binning: Binning, charset: Charset) -> Result<String> {
    let samples: Vec<f64> = samples.iter().copied().filter(|s| s.is_finite()).collect();
    let levels = charset.levels();
    let (Some(min), Some(max)) = (
        samples.iter().copied().reduce(f64::min),
        samples.iter().copied().reduce(f64::max),
    ) else {
        return Ok(String::new());
    };
    if max <= min {
        return Ok(levels[levels.len() - 1].to_string());
    }
    let axis = Axis { min, max };
    let masses = match binning {
        Binning::Linear => axis.counts(&samples),
        Binning::Qrde => axis.qrde_masses(&samples)?,
    };
    Ok(draw(&masses, levels))
}

/// The columns' characters: each column as tall as its mass relative to the largest, and at
/// least the shortest if it has any.
fn draw(masses: &[f64; WIDTH], levels: &[char; 8]) -> String {
    let largest = masses.iter().copied().fold(0.0, f64::max);
    masses
        .iter()
        .map(|&mass| {
            if mass <= 0.0 || largest <= 0.0 {
                return ' ';
            }
            let level = (mass / largest * levels.len() as f64).ceil() as usize;
            levels[level.clamp(1, levels.len()) - 1]
        })
        .collect()
}

/// The range a sparkline spans, divided into [`WIDTH`] columns. `min` is less than `max`.
struct Axis {
    min: f64,
    max: f64,
}

impl Axis {
    fn column_width(&self) -> f64 {
        (self.max - self.min) / WIDTH as f64
    }

    /// The column holding `value`; the last one holds `max`.
    fn column(&self, value: f64) -> usize {
        let column = ((value - self.min) / self.column_width()).floor();
        (column.max(0.0) as usize).min(WIDTH - 1)
    }

    /// The number of `samples` in each column.
    fn counts(&self, samples: &[f64]) -> [f64; WIDTH] {
        let mut counts = [0.0; WIDTH];
        for &sample in samples {
            counts[self.column(sample)] += 1.0;
        }
        counts
    }

    /// The probability mass in each column of the density estimate of `samples`. The estimate is
    /// of an HDR histogram of them in picoseconds, laid out as the C++ side lays out the one its
    /// statistics come from.
    fn qrde_masses(&self, samples: &[f64]) -> Result<[f64; WIDTH]> {
        let error = |e: histogram::Error| kj::failed!("estimating the density of samples: {e}");
        let mut histogram = Histogram::new(&histogram::Options {
            lowest: 1,
            highest: HIGHEST_PS,
            figures: 3,
            ..histogram::Options::default()
        })
        .map_err(error)?;
        for &sample in samples {
            let ps = (sample.max(0.0) * 1000.0).round() as i64;
            if !histogram.record(ps) {
                return Err(kj::failed!(
                    "a sample of {sample} ns is too large to estimate"
                ));
            }
        }
        let probabilities = Qrde::uniform_probabilities(QRDE_BINS).map_err(error)?;
        let qrde = histogram
            .qrde(&probabilities, Dequantization::Hdr)
            .map_err(error)?;

        let mut masses = [0.0; WIDTH];
        for (bounds, mass) in qrde
            .quantiles
            .windows(2)
            .zip(probabilities.windows(2).map(|p| p[1] - p[0]))
        {
            let low = (bounds[0] / 1000.0).clamp(self.min, self.max);
            let high = (bounds[1] / 1000.0).clamp(self.min, self.max);
            self.spread(&mut masses, low, high, mass);
        }
        Ok(masses)
    }

    /// Adds `mass`, spread evenly from `low` to `high`, to the columns it covers.
    fn spread(&self, masses: &mut [f64; WIDTH], low: f64, high: f64, mass: f64) {
        if high <= low {
            masses[self.column(low)] += mass;
            return;
        }
        let width = self.column_width();
        let first = self.column(low);
        let columns = masses
            .iter_mut()
            .enumerate()
            .take(self.column(high) + 1)
            .skip(first);
        for (column, column_mass) in columns {
            let start = (column as f64).mul_add(width, self.min);
            let overlap = high.min(start + width) - low.max(start);
            if overlap > 0.0 {
                *column_mass += mass * overlap / (high - low);
            }
        }
    }
}

#[cfg(test)]
#[path = "sparkline-test.rs"]
mod tests;
