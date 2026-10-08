// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `workerd bench`'s options and report: a `BenchReport` message (bench.capnp) with a group for
//! each `bench()` handler that `run::bench` runs, written as a table or as JSON.
//!
//! The worker factory runs a handler (`worker_bench`) and hands back the group the handler filled
//! in as an encoded message, which the report copies in under the group's name.

use std::fmt::Write as _;
use std::io::Write as _;
use std::time::Duration;
use std::time::Instant;

use bench_capnp::bench_params;
use bench_capnp::bench_report;
use capnp::dynamic_struct;
use capnp::dynamic_value;
use capnp::message::ReaderOptions;
use capnp::message::TypedBuilder;
use kj_rs::KjMaybe;
use serde::Serialize;
use serde::Serializer;
use serde::ser::Error as _;
use serde::ser::SerializeMap;
use serde::ser::SerializeSeq;

use crate::Result;
use crate::bindings::len_u32;
use crate::bridge::ffi;
use crate::channels::Channel;
use crate::config::capnp_error;
use crate::config::text;
use crate::log;

/// `workerd bench`'s options, besides the service and entrypoint patterns.
#[expect(
    clippy::struct_excessive_bools,
    reason = "one bool per command-line flag"
)]
pub struct Options {
    /// `--no-verbose`: no info logging, which otherwise shows uncaught exceptions.
    pub no_verbose: bool,
    /// `--all-autogates`.
    pub all_autogates: bool,
    /// `--compat-date`: the compatibility date of every worker.
    pub compat_date: Option<String>,
    /// `--quick`: short budgets, to check that the benchmarks run rather than to measure them.
    pub quick: bool,
    /// The filter's `/<case-pattern>`: a glob on case names.
    pub case_pattern: Option<String>,
    /// `--format=json`: the report as JSON rather than as a table.
    pub json: bool,
    /// `--output`: where to write the report; stdout if none.
    pub output: Option<String>,
}

impl Options {
    /// The options the C++ bootstrap applies.
    pub(crate) fn process(&self) -> ffi::BenchOptions {
        ffi::BenchOptions {
            no_verbose: self.no_verbose,
            all_autogates: self.all_autogates,
            compat_date: self.compat_date.clone().into(),
        }
    }

    /// The handlers' parameters, as an encoded `BenchParams` message.
    pub(crate) fn params(&self) -> Vec<u64> {
        let mut message = TypedBuilder::<bench_params::Owned>::new_default();
        let mut params = message.init_root();
        if let Some(pattern) = &self.case_pattern {
            params.set_case_filter(pattern);
        }
        params.set_quick(self.quick);
        to_words(message.borrow_inner())
    }
}

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
        to_words(self.message.borrow_inner())
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

    /// Records where the report was measured: the build and the machine, with `options`'
    /// compatibility date and autogates, and warnings for what makes the results unreliable.
    pub(crate) fn set_environment(&mut self, options: &Options) -> Result<()> {
        let build = ffi::build_info()?;
        let cpu_model = cpu_model();
        let governor = governor();
        let mut warnings = Vec::new();
        if build.build_mode != "release" {
            warnings.push(format!(
                "This is a {} build; its performance differs from a release build's, so use \
                 results only to compare runs of this build.",
                build.build_mode
            ));
        }
        if let Some(governor) = governor.as_deref().filter(|g| *g != "performance") {
            warnings.push(format!(
                "The CPU frequency governor is \"{governor}\"; set it to \"performance\" for \
                 more stable results."
            ));
        }

        let root = self.message.get_root().map_err(capnp_error)?;
        let mut environment = root.init_environment();
        environment.set_workerd_version(&build.workerd_version);
        environment.set_build_mode(&build.build_mode);
        environment.set_v8_version(&build.v8_version);
        if let Some(model) = &cpu_model {
            environment.set_cpu_model(model);
        }
        if let Some(governor) = &governor {
            environment.set_governor(governor);
        }
        if let Some(date) = &options.compat_date {
            environment.set_compat_date(date);
        }
        environment.set_all_autogates(options.all_autogates);
        let mut list = environment.init_warnings(len_u32(&warnings));
        for (index, warning) in (0..).zip(&warnings) {
            list.set(index, warning);
        }
        Ok(())
    }

    /// Writes the report as `options` ask: as text or JSON, to stdout or to `--output`.
    pub(crate) fn write(&self, options: &Options) -> Result<()> {
        let report = self.message.get_root_as_reader().map_err(capnp_error)?;
        let text = if options.json {
            format_json(report)?
        } else {
            format_text(report)?
        };
        let written = if let Some(path) = &options.output {
            write_file(path, &text)
        } else {
            let mut stdout = std::io::stdout().lock();
            stdout
                .write_all(text.as_bytes())
                .and_then(|()| stdout.flush())
        };
        let destination = options.output.as_deref().unwrap_or("stdout");
        written.map_err(|e| kj::failed!("writing the report to {destination}: {e}"))
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

/// `message`, encoded (segment table, then segments) in words.
fn to_words<A: capnp::message::Allocator>(message: &capnp::message::Builder<A>) -> Vec<u64> {
    capnp::serialize::write_message_to_words(message)
        .chunks_exact(8)
        .map(|chunk| u64::from_ne_bytes(chunk.try_into().unwrap_or_default()))
        .collect()
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

/// Writes `text` to the file at `path`, creating its directory if need be.
fn write_file(path: &str, text: &str) -> std::io::Result<()> {
    let path = std::path::Path::new(path);
    if let Some(dir) = path.parent().filter(|dir| !dir.as_os_str().is_empty()) {
        std::fs::create_dir_all(dir)?;
    }
    std::fs::write(path, text)
}

/// The model name of the machine's CPU, as Linux reports it.
fn cpu_model() -> Option<String> {
    let cpuinfo = std::fs::read_to_string("/proc/cpuinfo").ok()?;
    cpuinfo.lines().find_map(|line| {
        let (key, value) = line.split_once(':')?;
        (key.trim() == "model name").then(|| value.trim().to_owned())
    })
}

/// The CPU frequency governor, as Linux reports it for the first CPU.
fn governor() -> Option<String> {
    let path = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor";
    Some(std::fs::read_to_string(path).ok()?.trim().to_owned())
}

/// `report` as text: a header describing the environment, then a table for each group.
fn format_text(report: bench_report::Reader<'_>) -> Result<String> {
    let mut out = String::from(
        "Results compare runs on this machine and build; they don't predict production latency.\n",
    );
    let environment = report.get_environment().map_err(capnp_error)?;
    let mut parts = Vec::new();
    if environment.has_workerd_version() {
        parts.push(format!(
            "workerd {}",
            text(environment.get_workerd_version())?
        ));
    }
    parts.push(format!("{} build", text(environment.get_build_mode())?));
    parts.push(format!("V8 {}", text(environment.get_v8_version())?));
    if environment.has_cpu_model() {
        parts.push(text(environment.get_cpu_model())?);
    }
    if environment.has_governor() {
        parts.push(format!("governor {}", text(environment.get_governor())?));
    }
    if environment.has_compat_date() {
        parts.push(format!(
            "compat date {}",
            text(environment.get_compat_date())?
        ));
    }
    if environment.get_all_autogates() {
        parts.push("all autogates".to_owned());
    }
    let _ = writeln!(out, "{}", parts.join(", "));
    for warning in environment.get_warnings().map_err(capnp_error)? {
        let _ = writeln!(out, "WARNING: {}", text(warning)?);
    }
    for group in report.get_groups().map_err(capnp_error)? {
        format_group(&mut out, group)?;
    }
    Ok(out)
}

/// The columns of a group's table.
const COLUMNS: [&str; 6] = ["case", "median", "95% CI", "mean", "cpu median", "samples"];

/// A row of a group's table: a cell for each column, with any notes on the case as the message;
/// or the case's name, with a message saying why it has no statistics.
struct Row {
    cells: Vec<String>,
    message: Option<String>,
}

/// Appends `group` to `out`: its name, error and the overhead subtracted from its cases, then a
/// table of its cases, the columns padded to line up.
fn format_group(out: &mut String, group: bench_report::group::Reader<'_>) -> Result<()> {
    let _ = writeln!(out, "\n{}", text(group.get_name())?);
    if group.has_error() {
        let _ = writeln!(out, "  FAILED: {}", first_line(&text(group.get_error())?));
    }
    if group.has_overhead() {
        let overhead = group.get_overhead().map_err(capnp_error)?;
        let _ = writeln!(
            out,
            "  overhead per call, subtracted: {}, {} async; blackBox() {}",
            format_duration(overhead.get_sync_ns()),
            format_duration(overhead.get_async_ns()),
            format_duration(overhead.get_black_box_ns())
        );
    }
    let mut rows = Vec::new();
    for case in group.get_cases().map_err(capnp_error)? {
        rows.push(case_row(case)?);
    }
    if rows.is_empty() {
        return Ok(());
    }

    let width = |cell: &str| cell.chars().count();
    let mut widths = COLUMNS.map(width);
    for row in &rows {
        for (column, cell) in widths.iter_mut().zip(&row.cells) {
            *column = (*column).max(width(cell));
        }
    }
    let header = Row {
        cells: COLUMNS.map(str::to_owned).to_vec(),
        message: None,
    };
    for row in std::iter::once(&header).chain(&rows) {
        out.push_str("  ");
        for (index, (cell, column)) in row.cells.iter().zip(widths).enumerate() {
            // The last cell of a row has no padding after it.
            if index + 1 == row.cells.len() && row.message.is_none() {
                out.push_str(cell);
            } else {
                let _ = write!(out, "{cell:<width$}", width = column + 2);
            }
        }
        if let Some(message) = &row.message {
            out.push_str(message);
        }
        out.push('\n');
    }
    Ok(())
}

/// The row of `case`: its statistics, or why it has none.
fn case_row(case: bench_report::case::Reader<'_>) -> Result<Row> {
    use bench_report::case::Status;

    let name = text(case.get_name())?;
    let message = match case.get_status().map_err(|e| capnp_error(e.into()))? {
        Status::Ok => return statistics_row(name, case),
        Status::Skipped => "skipped".to_owned(),
        Status::Failed => format!("FAILED: {}", first_line(&text(case.get_error())?)),
    };
    Ok(Row {
        cells: vec![name],
        message: Some(message),
    })
}

/// The row of `case`, which ran, named `name`: its statistics, with notes on its flags.
fn statistics_row(name: String, case: bench_report::case::Reader<'_>) -> Result<Row> {
    use bench_report::case::Flag;

    let wall = case.get_wall_ns().map_err(capnp_error)?;
    let median_interval = format!(
        "{} - {}",
        format_duration(wall.get_median_low()),
        format_duration(wall.get_median_high())
    );
    let samples = wall.get_samples().map_err(capnp_error)?.len();
    let cells = vec![
        name,
        format_duration(wall.get_median()),
        median_interval,
        format_duration(wall.get_mean()),
        format_duration(case.get_cpu_ns().map_err(capnp_error)?.get_median()),
        format!("{samples} x {}", case.get_iterations_per_sample()),
    ];
    let mut notes = Vec::new();
    for flag in case.get_flags().map_err(capnp_error)? {
        notes.push(match flag.map_err(|e| capnp_error(e.into()))? {
            Flag::HighVariance => "high variance: try more samples or a longer minTime",
            Flag::AtFloor => "at measurement floor: pass results through b.blackBox()",
        });
    }
    Ok(Row {
        cells,
        message: (!notes.is_empty()).then(|| format!("({})", notes.join("; "))),
    })
}

/// The first line of `text`.
fn first_line(text: &str) -> &str {
    text.split_once('\n').map_or(text, |(first, _)| first)
}

/// `ns` nanoseconds with three significant figures and a unit, as in "12.3 ns" or "1.50 ms".
fn format_duration(ns: f64) -> String {
    const UNITS: [(f64, &str); 4] = [(1.0, "ns"), (1e3, "us"), (1e6, "ms"), (1e9, "s")];
    if !ns.is_finite() {
        return ns.to_string();
    }
    let (scale, unit) = UNITS
        .iter()
        .rev()
        .find(|(scale, _)| ns.abs() >= *scale)
        .unwrap_or(&UNITS[0]);
    let value = ns / scale;
    // Three significant figures: 1.23, 12.3, 123.
    let decimals = match value.abs() {
        magnitude if magnitude < 10.0 => 2,
        magnitude if magnitude < 100.0 => 1,
        _ => 0,
    };
    format!("{value:.decimals$} {unit}")
}

/// `report` as pretty-printed JSON, as capnp's C++ `JsonCodec` encodes it (see `Json`).
fn format_json(report: bench_report::Reader<'_>) -> Result<String> {
    let mut json = serde_json::to_string_pretty(&Json(report.into()))
        .map_err(|e| kj::failed!("encoding the report as JSON: {e}"))?;
    json.push('\n');
    Ok(json)
}

/// A capnp value, serialized as capnp's C++ `JsonCodec` encodes it: struct fields in the
/// schema's order, without the pointers that are null; 64-bit integers as strings, which JSON numbers
/// cannot hold exactly; `NaN` and the infinities as strings, which JSON numbers cannot be;
/// enumerants by name.
struct Json<'a>(dynamic_value::Reader<'a>);

impl Serialize for Json<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error> {
        match self.0 {
            dynamic_value::Reader::Void => serializer.serialize_unit(),
            dynamic_value::Reader::Bool(value) => serializer.serialize_bool(value),
            dynamic_value::Reader::Int8(value) => serializer.serialize_i8(value),
            dynamic_value::Reader::Int16(value) => serializer.serialize_i16(value),
            dynamic_value::Reader::Int32(value) => serializer.serialize_i32(value),
            dynamic_value::Reader::UInt8(value) => serializer.serialize_u8(value),
            dynamic_value::Reader::UInt16(value) => serializer.serialize_u16(value),
            dynamic_value::Reader::UInt32(value) => serializer.serialize_u32(value),
            dynamic_value::Reader::Int64(value) => serializer.collect_str(&value),
            dynamic_value::Reader::UInt64(value) => serializer.collect_str(&value),
            dynamic_value::Reader::Float32(value) => serialize_float(serializer, value.into()),
            dynamic_value::Reader::Float64(value) => serialize_float(serializer, value),
            dynamic_value::Reader::Enum(value) => {
                match value.get_enumerant().map_err(S::Error::custom)? {
                    Some(enumerant) => {
                        let name = enumerant.get_proto().get_name();
                        serializer.serialize_str(name.and_then(to_str).map_err(S::Error::custom)?)
                    }
                    None => serializer.serialize_u16(value.get_value()),
                }
            }
            dynamic_value::Reader::Text(value) => {
                serializer.serialize_str(value.to_str().map_err(S::Error::custom)?)
            }
            dynamic_value::Reader::Data(value) => serializer.collect_seq(value),
            dynamic_value::Reader::List(list) => {
                let mut seq = serializer.serialize_seq(Some(list.len() as usize))?;
                for element in list {
                    seq.serialize_element(&Json(element.map_err(S::Error::custom)?))?;
                }
                seq.end()
            }
            dynamic_value::Reader::Struct(value) => serialize_struct(serializer, value),
            dynamic_value::Reader::AnyPointer(_) | dynamic_value::Reader::Capability(_) => Err(
                S::Error::custom("AnyPointer and capability fields have no JSON encoding"),
            ),
        }
    }
}

/// `text` as a `&str`; an error if it is not UTF-8.
fn to_str(text: capnp::text::Reader<'_>) -> capnp::Result<&str> {
    Ok(text.to_str()?)
}

/// A float as a JSON number, except `NaN` and the infinities, which are strings.
fn serialize_float<S: Serializer>(
    serializer: S,
    value: f64,
) -> std::result::Result<S::Ok, S::Error> {
    if value.is_nan() {
        serializer.serialize_str("NaN")
    } else if value.is_infinite() {
        serializer.serialize_str(if value > 0.0 { "Infinity" } else { "-Infinity" })
    } else {
        serializer.serialize_f64(value)
    }
}

/// A struct as a JSON object. A union's member is written when it is set, even to a null
/// pointer (as `null`), unless it is the union's first member, which is set by default.
fn serialize_struct<S: Serializer>(
    serializer: S,
    value: dynamic_struct::Reader<'_>,
) -> std::result::Result<S::Ok, S::Error> {
    let active = value.which().map_err(S::Error::custom)?;
    let mut map = serializer.serialize_map(None)?;
    for field in value.get_schema().get_fields().map_err(S::Error::custom)? {
        let proto = field.get_proto();
        let name = proto
            .get_name()
            .and_then(to_str)
            .map_err(S::Error::custom)?;
        if value.has(field).map_err(S::Error::custom)? {
            map.serialize_entry(name, &Json(value.get(field).map_err(S::Error::custom)?))?;
        } else if active.is_some_and(|active| active.get_index() == field.get_index())
            && proto.get_discriminant_value() != 0
        {
            map.serialize_entry(name, &())?;
        }
    }
    map.end()
}

#[cfg(test)]
#[path = "bench-test.rs"]
mod tests;
