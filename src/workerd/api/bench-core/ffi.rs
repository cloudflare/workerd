// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge through which C++ hands a group's results to Rust to encode as a
//! `BenchReport.Group`.

use bench_capnp::bench_report::case::Status;

use crate::CaseResult;
use crate::GroupResult;
use crate::Overhead;

#[cxx::bridge(namespace = "workerd::api")]
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
#[expect(clippy::unnecessary_box_returns, reason = "cxx requires a Box")]
mod bridge {
    extern "Rust" {
        /// A group's results, collected from C++.
        type BenchGroupReport;

        fn new_bench_group_report() -> Box<BenchGroupReport>;

        /// Sets the group's error: `bench()` itself failed. Strings are bytes, which needn't be
        /// UTF-8, and are written to the report unchanged.
        fn set_error(self: &mut BenchGroupReport, error: &[u8]);

        /// Sets the overhead that was subtracted; see `BenchReport.Overhead`.
        fn set_overhead(
            self: &mut BenchGroupReport,
            sync_ns: f64,
            sync_cpu_ns: f64,
            async_ns: f64,
            async_cpu_ns: f64,
            black_box_ns: f64,
        );

        /// Adds a case. `status` is a `BenchReport.Case.Status`; `returns_promise` selects the
        /// overhead that is subtracted.
        fn add_case(
            self: &mut BenchGroupReport,
            name: &[u8],
            status: u16,
            iterations_per_sample: u64,
            wall_ns: &[f64],
            cpu_ns: &[f64],
            returns_promise: bool,
        ) -> Result<()>;

        /// Sets the error of the case added last, even if `error` is empty: a case has an error
        /// exactly when this was called for it, as a C++ `kj::Maybe<kj::String>` that is set.
        fn set_case_error(self: &mut BenchGroupReport, error: &[u8]);

        /// The group, as an encoded `BenchReport.Group` message (segment table, then segments)
        /// in words.
        fn encode(self: &BenchGroupReport) -> Result<Vec<u64>>;
    }
}

/// A group's results, collected from C++.
pub struct BenchGroupReport(GroupResult);

#[expect(clippy::unnecessary_box_returns, reason = "cxx requires a Box")]
fn new_bench_group_report() -> Box<BenchGroupReport> {
    Box::new(BenchGroupReport(GroupResult::default()))
}

impl BenchGroupReport {
    fn set_error(&mut self, error: &[u8]) {
        self.0.error = Some(error.to_vec());
    }

    fn set_overhead(
        &mut self,
        sync_ns: f64,
        sync_cpu_ns: f64,
        async_ns: f64,
        async_cpu_ns: f64,
        black_box_ns: f64,
    ) {
        self.0.overhead = Some(Overhead {
            sync_ns,
            sync_cpu_ns,
            async_ns,
            async_cpu_ns,
            black_box_ns,
        });
    }

    fn add_case(
        &mut self,
        name: &[u8],
        status: u16,
        iterations_per_sample: u64,
        wall_ns: &[f64],
        cpu_ns: &[f64],
        returns_promise: bool,
    ) -> Result<(), capnp::NotInSchema> {
        self.0.cases.push(CaseResult {
            name: name.to_vec(),
            status: Status::try_from(status)?,
            error: None,
            iterations_per_sample,
            wall_ns: wall_ns.to_vec(),
            cpu_ns: cpu_ns.to_vec(),
            returns_promise,
        });
        Ok(())
    }

    fn set_case_error(&mut self, error: &[u8]) {
        if let Some(case) = self.0.cases.last_mut() {
            case.error = Some(error.to_vec());
        }
    }

    fn encode(&self) -> Result<Vec<u64>, crate::Error> {
        crate::encode_group(&self.0)
    }
}

#[cfg(test)]
#[path = "ffi-test.rs"]
mod tests;
