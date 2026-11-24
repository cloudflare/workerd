// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Startup groups: timing how long a Worker takes to start in a new isolate, as a cold start
//! does. See `BenchReport.Group.startup` for what each phase covers and when sampling stops.
//!
//! The caller supplies the function that starts a new copy of the Worker and destroys it, and
//! reads the clocks between the phases; this module decides how many starts to time and turns
//! their timings into a group.

use std::future::Future;

use bench_capnp::bench_params;
use bench_capnp::bench_report;
use bench_capnp::bench_report::case::Status;

use crate::CaseResult;
use crate::DEFAULT_MIN_TIME_NS;
use crate::DEFAULT_SAMPLES;
use crate::Error;
use crate::GlobFilter;
use crate::GroupResult;
use crate::MAX_SAMPLES;
use crate::QUICK_MIN_TIME_NS;
use crate::QUICK_SAMPLES;
use crate::Timing;

/// Untimed starts before a startup group's samples, except under `--quick`.
const STARTUP_WARMUPS: u32 = 1;

/// A startup group stops at the sampling budget only once it has at least this many samples, so
/// that a slow Worker still gets usable statistics.
const STARTUP_MIN_SAMPLES: usize = 5;

/// How long the phases of starting one Worker took; see `BenchReport.Group.startup` for what each
/// covers.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct StartupTiming {
    pub isolate: Timing,
    pub script: Timing,
    pub worker: Timing,
    pub total: Timing,
}

/// A phase of starting a Worker: its case name, and how to get its timing from a start's.
type Phase = (&'static str, fn(&StartupTiming) -> Timing);

/// The phases, as cases. The prefix keeps a pattern meant for a handler's cases, such as "w*",
/// from selecting a phase. Its separator isn't "/", after which the case filter starts matching
/// the pattern again.
const PHASES: [Phase; 4] = [
    ("startup:isolate", |timing| timing.isolate),
    ("startup:script", |timing| timing.script),
    ("startup:worker", |timing| timing.worker),
    ("startup:total", |timing| timing.total),
];

/// Times starts of a Worker for a startup group.
///
/// `params` is an encoded `BenchParams` message, in words. `start` starts a new copy of the
/// Worker in a new isolate and destroys it, returning how long its phases took, or the
/// description of why it failed to start.
///
/// Starts continue until the samples or the sampling budget run out, and only the phases that the
/// case filter selects are reported; with none selected, nothing starts. Returns the group,
/// without its name, as an encoded `BenchReport.Group` message in words. A start that fails fails
/// the group, with its description as the group's error.
pub async fn time_startup<F, Fut>(params: &[u64], mut start: F) -> Result<Vec<u64>, Error>
where
    F: FnMut() -> Fut,
    Fut: Future<Output = Result<StartupTiming, String>>,
{
    let plan = Plan::read(params)?;

    let mut message = capnp::message::TypedBuilder::<bench_report::group::Owned>::new_default();
    message.init_root().set_startup(true);
    if plan.phases.is_empty() {
        return crate::to_words(&message);
    }

    // Each start is a sample, so there is no batch to calibrate. Starts stop at `samples`, or at
    // the sampling budget once there are enough of them to summarize. Except under --quick,
    // starts that aren't timed go first, like a case's warmup.
    let mut timings = Vec::with_capacity(plan.samples);
    let started: Result<(), String> = async {
        for _ in 0..plan.warmups {
            start().await?;
        }
        let mut sampled_ns: u64 = 0;
        while timings.len() < plan.samples
            && (timings.len() < STARTUP_MIN_SAMPLES || sampled_ns < plan.budget_ns)
        {
            let timing = start().await?;
            sampled_ns = sampled_ns.saturating_add(timing.total.wall_ns.max(0).unsigned_abs());
            timings.push(timing);
        }
        Ok(())
    }
    .await;
    if let Err(error) = started {
        message.get_root()?.set_error(error.as_str());
        return crate::to_words(&message);
    }

    let result = GroupResult {
        error: None,
        cases: plan
            .phases
            .iter()
            .map(|(name, phase)| CaseResult {
                name: name.as_bytes().to_vec(),
                status: Status::Ok,
                error: None,
                iterations_per_sample: 1,
                wall_ns: timings.iter().map(|t| to_f64(phase(t).wall_ns)).collect(),
                cpu_ns: timings.iter().map(|t| to_f64(phase(t).cpu_ns)).collect(),
                returns_promise: false,
            })
            .collect(),
        overhead: None,
    };
    crate::fill_group(&result, message.get_root()?)?;
    crate::to_words(&message)
}

/// What a startup group's `BenchParams` ask for.
struct Plan {
    phases: Vec<Phase>,
    warmups: u32,
    samples: usize,
    budget_ns: u64,
}

impl Plan {
    fn read(words: &[u64]) -> Result<Self, Error> {
        let bytes: Vec<u8> = words.iter().flat_map(|word| word.to_ne_bytes()).collect();
        let message =
            capnp::serialize::read_message(bytes.as_slice(), capnp::message::ReaderOptions::new())?;
        let params: bench_params::Reader<'_> = message.get_root()?;

        let case_filter = params.get_case_filter()?.as_bytes();
        let filter = GlobFilter::new(if case_filter.is_empty() {
            b"*"
        } else {
            case_filter
        });
        let phases = PHASES
            .into_iter()
            .filter(|(name, _)| filter.matches(name.as_bytes()))
            .collect();

        let quick = params.get_quick();
        let defaults = params.get_defaults()?;
        let mut samples = match defaults.get_samples() {
            0 => DEFAULT_SAMPLES,
            samples => samples,
        };
        samples = samples.min(if quick { QUICK_SAMPLES } else { MAX_SAMPLES });
        let mut budget_ns = match defaults.get_min_time_ns() {
            0 => DEFAULT_MIN_TIME_NS,
            budget_ns => budget_ns,
        };
        if quick {
            budget_ns = budget_ns.min(QUICK_MIN_TIME_NS);
        }
        Ok(Self {
            phases,
            warmups: if quick { 0 } else { STARTUP_WARMUPS },
            samples: samples as usize,
            budget_ns,
        })
    }
}

#[expect(
    clippy::cast_precision_loss,
    reason = "a phase takes far less than 2^53 ns (about 104 days)"
)]
fn to_f64(ns: i64) -> f64 {
    ns as f64
}

#[cfg(test)]
#[path = "startup-test.rs"]
mod tests;
