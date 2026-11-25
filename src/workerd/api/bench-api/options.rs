// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The options JavaScript passes to `BenchController.run()`, the options it reads from
//! `BenchController.options`, and the parsing of the budgets they set.

#![expect(
    non_snake_case,
    reason = "the fields of the option structs are the property names JavaScript uses"
)]

use jsg::v8;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_struct;

/// A duration: a number of milliseconds, or a string with a unit, as in "500ms" or "2s".
#[jsg_oneof]
#[derive(Debug, Clone)]
pub enum BenchDuration {
    Number(f64),
    String(String),
}

/// Options for one case, the third argument of `BenchController.run()`.
#[jsg_struct]
pub struct BenchCaseOptions {
    /// Sampling budget: the batch size is calibrated so that all samples take about this long.
    pub minTime: Option<BenchDuration>,
    /// How long to call the function before calibrating and sampling, to let V8 optimize it.
    pub warmup: Option<BenchDuration>,
    /// The number of batches to time.
    pub samples: Option<f64>,
    /// Calls per batch, instead of calibrating.
    pub batch: Option<f64>,
    /// Called before each batch, untimed. Its result (awaited if it is a promise) is passed to
    /// every call of the function in that batch.
    pub setup: Option<v8::TracedGlobal<v8::Function>>,
    /// Called after each batch with the setup result (undefined without `setup`), untimed, and
    /// awaited if it returns a promise.
    pub teardown: Option<v8::TracedGlobal<v8::Function>>,
    /// Reports the case as skipped instead of running it.
    pub skip: Option<bool>,
}

/// `BenchController.options`.
#[jsg_struct(name = "Options")]
pub struct BenchOptions {
    /// The case filter given on the command line, if any.
    pub filter: Option<String>,
    /// Whether `--quick` was given: budgets are short, so results only show that cases run.
    pub quick: bool,
}

/// `duration`, which option `what` set, in nanoseconds.
pub fn parse_duration(duration: &BenchDuration, what: &str) -> jsg::Result<u64> {
    match duration {
        BenchDuration::Number(ms) => duration_to_ns(ms * 1e6, what),
        BenchDuration::String(text) => {
            let number_end = text
                .bytes()
                .position(|b| !(b.is_ascii_digit() || b == b'.'))
                .unwrap_or(text.len());
            let (number, unit) = text.split_at(number_end);
            let number: f64 = number.parse().map_err(|_| {
                jsg::Error::new_type_error(format!(
                    "{what} must be a number of milliseconds or a string like \"500ms\" or \
                     \"2s\"."
                ))
            })?;
            let scale = match unit {
                "ns" => 1.0,
                "us" | "µs" => 1e3,
                "ms" | "" => 1e6,
                "s" => 1e9,
                _ => {
                    return Err(jsg::Error::new_type_error(format!(
                        "{what} has an unknown unit \"{unit}\"; use \"ns\", \"us\", \"ms\", or \
                         \"s\"."
                    )));
                }
            };
            duration_to_ns(number * scale, what)
        }
    }
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "the duration is checked to be from 0 to 1e18 first"
)]
fn duration_to_ns(ns: f64, what: &str) -> jsg::Result<u64> {
    if !(0.0..1e18).contains(&ns) {
        return Err(jsg::Error::new_range_error(format!(
            "{what} must be a non-negative duration."
        )));
    }
    Ok(ns as u64)
}

/// `value`, which option `what` set, as an integer from 1 to `max`.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    clippy::cast_precision_loss,
    reason = "the value is checked to be an integer from 1 to `max`, compared as a double"
)]
pub fn parse_count(value: f64, max: u64, what: &str) -> jsg::Result<u64> {
    if !((1.0..=max as f64).contains(&value) && value.fract() == 0.0) {
        return Err(jsg::Error::new_range_error(format!(
            "{what} must be an integer from 1 to {max}."
        )));
    }
    Ok(value as u64)
}

#[cfg(test)]
#[path = "options-test.rs"]
mod tests;
