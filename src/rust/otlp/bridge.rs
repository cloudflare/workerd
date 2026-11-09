// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The interface C++ drives the crate through. Text crosses as bytes because KJ strings need not
//! be UTF-8; it is converted lossily.

#![allow(unsafe_code, reason = "the CXX bridge generates unsafe FFI code")]
#![expect(
    clippy::boxed_local,
    clippy::needless_pass_by_value,
    clippy::unnecessary_box_returns,
    reason = "the CXX boundary requires these ownership and transport representations"
)]

use kj_rs::KjMaybe;

use crate::attributes::Attributes;
use crate::buffer;
use crate::buffer::SpanBuffer;
use crate::logs;
use crate::logs::LogBuffer;
use crate::proto;

#[cxx::bridge(namespace = "workerd::rust::otlp")]
mod ffi {
    enum StatusCode {
        Unset,
        Ok,
        Error,
    }

    /// What is fixed for one invocation's spans.
    struct Options {
        /// The W3C trace flags, zero when the trace carries none.
        trace_flags: u32,
        /// The invocation's own span: the one whose kind and status follow the root rules, and
        /// whose close hands out the invocation's spans.
        root_span_id: u64,
        /// Cut the query from `url.full` and leave out `url.query`.
        redact_query_string: bool,
        /// Leave every timestamp at zero, for tests that compare whole spans.
        omit_timestamps: bool,
    }

    /// An encoded `ExportTraceServiceRequest` and the number of spans in it. When there is nothing
    /// to send yet, `request` is empty and `span_count` is zero.
    struct Batch {
        request: Vec<u8>,
        span_count: usize,
    }

    /// The console method a log came from.
    enum LogLevel {
        Debug,
        Info,
        Log,
        Warn,
        Error,
    }

    /// The structured fields of one argument of a `console` call. For an argument that was not a
    /// native `Error`, `is_error` is false and the rest is empty.
    struct LogArgumentError<'a> {
        is_error: bool,
        name: &'a [u8],
        message: &'a [u8],
        has_stack: bool,
        stack: &'a [u8],
    }

    /// An encoded `ExportLogsServiceRequest` and the number of records in it. When there is
    /// nothing to send yet, `request` is empty and `record_count` is zero.
    struct LogBatch {
        request: Vec<u8>,
        record_count: usize,
    }

    extern "Rust" {
        /// Span or resource attributes, in the order they were added.
        type Attributes;

        fn new_attributes() -> Box<Attributes>;
        fn add_string(attributes: &mut Attributes, key: &[u8], value: &[u8]);
        fn add_bool(attributes: &mut Attributes, key: &[u8], value: bool);
        fn add_int(attributes: &mut Attributes, key: &[u8], value: i64);
        fn add_double(attributes: &mut Attributes, key: &[u8], value: f64);

        /// One invocation's spans on their way to a collector. Times are unix nanoseconds.
        type SpanBuffer;

        /// `trace_id` is the 16-byte id every span shares. `resource` describes the worker and
        /// goes on every request. `default_attributes` are applied to every span at its close
        /// without overriding the span's own.
        fn new_span_buffer(
            trace_id: &[u8],
            options: Options,
            resource: Box<Attributes>,
            default_attributes: Box<Attributes>,
        ) -> Box<SpanBuffer>;

        /// Returns false when the span is not buffered: its id is already open, or too many spans
        /// are. Updates and the close of such a span are ignored.
        fn open_span(
            buffer: &mut SpanBuffer,
            span_id: u64,
            parent_span_id: u64,
            name: &[u8],
            start: i64,
        ) -> bool;
        fn set_span_name(buffer: &mut SpanBuffer, span_id: u64, name: &[u8]);
        fn set_span_status(buffer: &mut SpanBuffer, span_id: u64, code: StatusCode, message: &[u8]);
        fn add_span_exception(
            buffer: &mut SpanBuffer,
            span_id: u64,
            time: i64,
            name: &[u8],
            message: &[u8],
            stack: KjMaybe<&[u8]>,
        );
        /// `attributes` are the ones the span ended up with.
        fn close_span(
            buffer: &mut SpanBuffer,
            span_id: u64,
            end: i64,
            attributes: Box<Attributes>,
        ) -> Batch;
        /// Ends the invocation: spans still open are closed at `now` and flagged as not ended.
        fn finish(buffer: &mut SpanBuffer, now: i64) -> Batch;

        /// One invocation's logs on their way to a collector. Times are unix nanoseconds.
        type LogBuffer;

        /// `trace_id`, `trace_flags`, `omit_timestamps`, `resource` and `default_attributes` are
        /// as for the invocation's `SpanBuffer`.
        fn new_log_buffer(
            trace_id: &[u8],
            trace_flags: u32,
            omit_timestamps: bool,
            resource: Box<Attributes>,
            default_attributes: Box<Attributes>,
        ) -> Box<LogBuffer>;
        /// `span_id` is the span that was active, or zero. `message` is the JSON array of the
        /// call's arguments, unless `truncated`. `error_info` has an entry per argument, or none
        /// when no argument was an `Error`.
        fn add_log(
            buffer: &mut LogBuffer,
            span_id: u64,
            time: i64,
            level: LogLevel,
            message: &[u8],
            truncated: bool,
            error_info: &[LogArgumentError],
        ) -> LogBatch;
        fn add_exception_log(
            buffer: &mut LogBuffer,
            span_id: u64,
            time: i64,
            name: &[u8],
            message: &[u8],
            stack: KjMaybe<&[u8]>,
        ) -> LogBatch;
        /// Hands out every buffered record, and from then on each record as it is added. Called
        /// when the invocation's span closes.
        fn flush_logs(buffer: &mut LogBuffer) -> LogBatch;
    }
}

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

fn batch(batch: Option<buffer::Batch>) -> ffi::Batch {
    batch.map_or_else(
        || ffi::Batch {
            request: Vec::new(),
            span_count: 0,
        },
        |batch| ffi::Batch {
            request: batch.request,
            span_count: batch.span_count,
        },
    )
}

fn log_batch(batch: Option<logs::Batch>) -> ffi::LogBatch {
    batch.map_or_else(
        || ffi::LogBatch {
            request: Vec::new(),
            record_count: 0,
        },
        |batch| ffi::LogBatch {
            request: batch.request,
            record_count: batch.record_count,
        },
    )
}

fn new_attributes() -> Box<Attributes> {
    Box::default()
}

fn add_string(attributes: &mut Attributes, key: &[u8], value: &[u8]) {
    attributes.push(text(key), text(value));
}

fn add_bool(attributes: &mut Attributes, key: &[u8], value: bool) {
    attributes.push(text(key), value);
}

fn add_int(attributes: &mut Attributes, key: &[u8], value: i64) {
    attributes.push(text(key), value);
}

fn add_double(attributes: &mut Attributes, key: &[u8], value: f64) {
    attributes.push(text(key), value);
}

fn new_span_buffer(
    trace_id: &[u8],
    options: ffi::Options,
    resource: Box<Attributes>,
    default_attributes: Box<Attributes>,
) -> Box<SpanBuffer> {
    let options = buffer::Options {
        trace_id: trace_id.to_vec(),
        trace_flags: options.trace_flags,
        root_span_id: options.root_span_id,
        redact_query_string: options.redact_query_string,
        omit_timestamps: options.omit_timestamps,
    };
    Box::new(SpanBuffer::new(options, *resource, *default_attributes))
}

fn open_span(
    buffer: &mut SpanBuffer,
    span_id: u64,
    parent_span_id: u64,
    name: &[u8],
    start: i64,
) -> bool {
    buffer.open(span_id, parent_span_id, text(name), start)
}

fn set_span_name(buffer: &mut SpanBuffer, span_id: u64, name: &[u8]) {
    buffer.set_name(span_id, text(name));
}

fn set_span_status(buffer: &mut SpanBuffer, span_id: u64, code: ffi::StatusCode, message: &[u8]) {
    let code = match code {
        ffi::StatusCode::Ok => proto::StatusCode::Ok,
        ffi::StatusCode::Error => proto::StatusCode::Error,
        _ => proto::StatusCode::Unset,
    };
    buffer.set_status(span_id, code, text(message));
}

fn add_span_exception(
    buffer: &mut SpanBuffer,
    span_id: u64,
    time: i64,
    name: &[u8],
    message: &[u8],
    stack: KjMaybe<&[u8]>,
) {
    let stack = Option::from(stack).map(text);
    buffer.add_exception(span_id, time, text(name), text(message), stack);
}

fn close_span(
    buffer: &mut SpanBuffer,
    span_id: u64,
    end: i64,
    attributes: Box<Attributes>,
) -> ffi::Batch {
    batch(buffer.close(span_id, end, *attributes))
}

fn finish(buffer: &mut SpanBuffer, now: i64) -> ffi::Batch {
    batch(buffer.finish(now))
}

fn new_log_buffer(
    trace_id: &[u8],
    trace_flags: u32,
    omit_timestamps: bool,
    resource: Box<Attributes>,
    default_attributes: Box<Attributes>,
) -> Box<LogBuffer> {
    let options = logs::Options {
        trace_id: trace_id.to_vec(),
        trace_flags,
        omit_timestamps,
    };
    Box::new(LogBuffer::new(options, *resource, *default_attributes))
}

fn add_log(
    buffer: &mut LogBuffer,
    span_id: u64,
    time: i64,
    level: ffi::LogLevel,
    message: &[u8],
    truncated: bool,
    error_info: &[ffi::LogArgumentError],
) -> ffi::LogBatch {
    let level = match level {
        ffi::LogLevel::Debug => logs::Level::Debug,
        ffi::LogLevel::Info => logs::Level::Info,
        ffi::LogLevel::Warn => logs::Level::Warn,
        ffi::LogLevel::Error => logs::Level::Error,
        _ => logs::Level::Log,
    };
    let message = String::from_utf8_lossy(message);
    let error_info = error_info
        .iter()
        .map(|argument| {
            argument.is_error.then(|| logs::ErrorInfo {
                name: text(argument.name),
                message: text(argument.message),
                stack: argument.has_stack.then(|| text(argument.stack)),
            })
        })
        .collect();
    log_batch(buffer.add_log(span_id, time, level, &message, truncated, error_info))
}

fn add_exception_log(
    buffer: &mut LogBuffer,
    span_id: u64,
    time: i64,
    name: &[u8],
    message: &[u8],
    stack: KjMaybe<&[u8]>,
) -> ffi::LogBatch {
    let stack = Option::from(stack).map(text);
    log_batch(buffer.add_exception(span_id, time, text(name), text(message), stack))
}

fn flush_logs(buffer: &mut LogBuffer) -> ffi::LogBatch {
    log_batch(buffer.flush())
}
