// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `workerd bench`'s report: a `BenchReport` message (bench.capnp) with a group for each
//! `bench()` handler that `run::bench` runs.
//!
//! The worker factory runs a handler (`worker_bench`) and hands back the group the handler filled
//! in as an encoded message, which the report copies in under the group's name.

use std::time::Duration;
use std::time::Instant;

use bench_capnp::bench_report;
use capnp::message::ReaderOptions;
use capnp::message::TypedBuilder;
use kj_rs::KjMaybe;

use crate::Result;
use crate::bridge::ffi;
use crate::channels::Channel;
use crate::config::capnp_error;
use crate::config::text;
use crate::log;

/// A `BenchReport` being filled in, a group at a time.
pub struct Report {
    message: TypedBuilder<bench_report::Owned>,
    /// How many of the groups the report has room for are filled in.
    filled: u32,
    /// Whether a handler, or one of its cases, failed.
    failed: bool,
}

impl Report {
    /// A report with room for `groups` groups.
    #[must_use]
    pub fn new(groups: u32) -> Self {
        let mut message = TypedBuilder::<bench_report::Owned>::new_default();
        message.init_root().init_groups(groups);
        Self {
            message,
            filled: 0,
            failed: false,
        }
    }

    /// Whether at least one handler ran and nothing failed.
    #[must_use]
    pub fn passed(&self) -> bool {
        self.filled > 0 && !self.failed
    }

    /// The report, encoded (segment table, then segments) in words.
    #[must_use]
    pub fn to_words(&self) -> Vec<u64> {
        capnp::serialize::write_message_to_words(self.message.borrow_inner())
            .chunks_exact(8)
            .map(|chunk| u64::from_ne_bytes(chunk.try_into().unwrap_or_default()))
            .collect()
    }

    /// Runs the `bench()` handler behind `channel` with `params` (an encoded `BenchParams`
    /// message) as the report's next group, named `name`. A handler that cannot be started or
    /// run has the error as its group's.
    ///
    /// Progress is logged as `workerd test` logs it, at debug level: `[ BENCH ]` as the handler
    /// starts; `[ SKIP ]` and `[ FAIL ]` for its cases; then `[ DONE ]` with the time it took, or
    /// `[ FAIL ]`.
    pub async fn run_group(
        &mut self,
        channel: &dyn Channel,
        name: &str,
        params: &[u64],
    ) -> Result<()> {
        log::debug(&format!("[ BENCH ] {name}"));
        let start = Instant::now();
        let metadata = ffi::new_request_metadata(KjMaybe::None, KjMaybe::None);
        let result = match channel.start_request(metadata) {
            Ok(worker) => ffi::worker_bench(worker, params)
                .await
                .map_err(crate::Error::from),
            Err(error) => Err(error),
        };
        let duration = start.elapsed();

        let index = self.filled;
        self.filled += 1;
        let root = self.message.get_root().map_err(capnp_error)?;
        let mut groups = root.get_groups().map_err(capnp_error)?;
        match result {
            Ok(words) => {
                let message = read_words(&words).map_err(capnp_error)?;
                let group = message.get_root().map_err(capnp_error)?;
                groups.set_with_caveats(index, group).map_err(capnp_error)?;
            }
            Err(error) => groups.reborrow().get(index).set_error(error.description()),
        }
        let mut group = groups.get(index);
        group.set_name(name);
        if log_outcome(group.into_reader(), name, duration)? {
            self.failed = true;
        }
        Ok(())
    }
}

/// Logs how the group `name` went: `[ SKIP ]` and `[ FAIL ]` for its cases, then `[ DONE ]` with
/// `duration`, or `[ FAIL ]`. Whether it failed.
fn log_outcome(
    group: bench_report::group::Reader<'_>,
    name: &str,
    duration: Duration,
) -> Result<bool> {
    let mut failed = group.has_error();
    for case in group.get_cases().map_err(capnp_error)? {
        match case.get_status().map_err(|e| capnp_error(e.into()))? {
            bench_report::case::Status::Ok => {}
            bench_report::case::Status::Skipped => {
                log::debug(&format!("[ SKIP ] {name}/{}", text(case.get_name())?));
            }
            bench_report::case::Status::Failed => {
                let case_name = text(case.get_name())?;
                let error = text(case.get_error())?;
                log::debug(&format!("[ FAIL ] {name}/{case_name}: {error}"));
                failed = true;
            }
        }
    }
    if group.has_error() {
        log::debug(&format!("[ FAIL ] {name}: {}", text(group.get_error())?));
    } else {
        let verdict = if failed { "FAIL" } else { "DONE" };
        log::debug(&format!("[ {verdict} ] {name} ({duration:?})"));
    }
    Ok(failed)
}

/// An encoded message (segment table, then segments) in words, read. The messages are the
/// factory's, as large as a handler's results, so their size is not limited.
fn read_words(
    words: &[u64],
) -> capnp::Result<capnp::message::Reader<capnp::serialize::OwnedSegments>> {
    let bytes: Vec<u8> = words.iter().flat_map(|word| word.to_ne_bytes()).collect();
    let options = *ReaderOptions::new().traversal_limit_in_words(None);
    capnp::serialize::read_message(bytes.as_slice(), options)
}
