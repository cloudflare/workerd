// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge through which `trace-ids-differential-test.c++` checks the Rust ids against the C++
//! of `workerd/io/trace.c++`, which writes and reads them the same way.
//!
//! Ids cross as integers, and text is written into a buffer the caller owns.

#![allow(
    unsafe_code,
    reason = "holds the cxx bridge, which expands to unsafe FFI glue"
)]

use capnp::message::Builder;
use capnp::message::HeapAllocator;
use capnp::message::ReaderOptions;
use trace::IdText;
use trace::InvocationSpanContext;
use trace::SpanContext;
use trace::SpanId;
use trace::SpanStatus;
use trace::TraceFlags;
use trace::TraceId;
use trace::format_traceparent;
use trace_capnp::span_status;
use worker_interface_capnp::invocation_span_context;
use worker_interface_capnp::span_context;

#[cxx::bridge(namespace = "workerd::rust::trace_test")]
mod ffi {
    struct TraceIdParts {
        high: u64,
        low: u64,
    }

    /// A parsed trace id; the id is zero if `ok` is false.
    struct ParsedTraceId {
        ok: bool,
        id: TraceIdParts,
    }

    /// A parsed span id; the id is zero if `ok` is false.
    struct ParsedSpanId {
        ok: bool,
        id: u64,
    }

    /// A parsed `traceparent`; the rest is zero if `ok` is false.
    struct ParsedTraceparent {
        ok: bool,
        trace_id: TraceIdParts,
        span_id: u64,
        flags: u8,
    }

    /// A message read into the Rust type and written back out; `ok` is false, and `message`
    /// empty, if Rust rejected it.
    struct RoundTrip {
        ok: bool,
        message: Vec<u8>,
    }

    extern "Rust" {
        /// Reads a flat single-segment message holding a `SpanContext` and writes the Rust value
        /// back out.
        fn span_context_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for an `InvocationSpanContext`.
        fn invocation_span_context_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `SpanStatus`.
        fn span_status_round_trip(message: &[u8]) -> RoundTrip;

        /// Writes the `traceparent` of a span context to `out`, which has room for 55 bytes, and
        /// returns its length, or 0 if it has none.
        fn span_context_to_traceparent(
            high: u64,
            low: u64,
            span_id: u64,
            has_span_id: bool,
            flags: u8,
            has_flags: bool,
            out: &mut [u8],
        ) -> usize;

        /// Writes the Go text of a trace id to `out`, which has room for 32 bytes, and returns
        /// its length.
        fn trace_id_to_go(high: u64, low: u64, out: &mut [u8]) -> usize;

        /// Writes the 32-byte W3C text of a trace id to `out`.
        fn trace_id_to_w3c(high: u64, low: u64, out: &mut [u8]);

        fn trace_id_from_go(text: &[u8]) -> ParsedTraceId;

        /// Parses exactly 32 hex digits.
        fn trace_id_from_w3c(text: &[u8]) -> ParsedTraceId;

        /// Parses exactly 16 hex digits.
        fn span_id_from_w3c(text: &[u8]) -> ParsedSpanId;

        /// Writes the 16 protobuf bytes of a trace id to `out`.
        fn trace_id_to_protobuf(high: u64, low: u64, out: &mut [u8]);

        fn trace_id_from_protobuf(bytes: &[u8]) -> ParsedTraceId;

        /// Writes the 16-byte Go text of a span id to `out`.
        fn span_id_to_go(id: u64, out: &mut [u8]);

        fn parse_traceparent(text: &[u8]) -> ParsedTraceparent;

        /// Writes the 55-byte `traceparent` header to `out`.
        fn format_traceparent_to(high: u64, low: u64, span_id: u64, flags: u8, out: &mut [u8]);
    }
}

fn write(text: IdText, out: &mut [u8]) -> usize {
    let bytes = text.as_bytes();
    out[..bytes.len()].copy_from_slice(bytes);
    bytes.len()
}

fn parts(id: TraceId) -> ffi::TraceIdParts {
    ffi::TraceIdParts {
        high: id.high,
        low: id.low,
    }
}

fn parsed(id: Option<TraceId>) -> ffi::ParsedTraceId {
    ffi::ParsedTraceId {
        ok: id.is_some(),
        id: parts(id.unwrap_or_default()),
    }
}

fn trace_id_to_go(high: u64, low: u64, out: &mut [u8]) -> usize {
    write(TraceId { high, low }.to_go_text(), out)
}

fn trace_id_to_w3c(high: u64, low: u64, out: &mut [u8]) {
    write(TraceId { high, low }.to_w3c_text(), out);
}

fn trace_id_from_go(text: &[u8]) -> ffi::ParsedTraceId {
    parsed(TraceId::from_go_text(text))
}

fn trace_id_from_w3c(text: &[u8]) -> ffi::ParsedTraceId {
    parsed(TraceId::from_w3c_text(text))
}

fn span_id_from_w3c(text: &[u8]) -> ffi::ParsedSpanId {
    let id = SpanId::from_w3c_text(text);
    ffi::ParsedSpanId {
        ok: id.is_some(),
        id: id.unwrap_or_default().0,
    }
}

fn trace_id_to_protobuf(high: u64, low: u64, out: &mut [u8]) {
    out[..16].copy_from_slice(&TraceId { high, low }.to_protobuf());
}

fn trace_id_from_protobuf(bytes: &[u8]) -> ffi::ParsedTraceId {
    parsed(TraceId::from_protobuf(bytes))
}

fn span_id_to_go(id: u64, out: &mut [u8]) {
    write(SpanId(id).to_go_text(), out);
}

fn parse_traceparent(text: &[u8]) -> ffi::ParsedTraceparent {
    match SpanContext::from_traceparent(text) {
        Some(context) => ffi::ParsedTraceparent {
            ok: true,
            trace_id: parts(context.trace_id),
            span_id: context.span_id.unwrap_or_default().0,
            flags: context.trace_flags.map_or(0, |flags| flags.0),
        },
        None => ffi::ParsedTraceparent {
            ok: false,
            trace_id: parts(TraceId::NULL),
            span_id: 0,
            flags: 0,
        },
    }
}

fn format_traceparent_to(high: u64, low: u64, span_id: u64, flags: u8, out: &mut [u8]) {
    write(
        format_traceparent(TraceId { high, low }, SpanId(span_id), TraceFlags(flags)),
        out,
    );
}

type FlatReader<'a> = capnp::message::Reader<capnp::serialize::BufferSegments<&'a [u8]>>;

fn read_flat(message: &[u8]) -> Option<FlatReader<'_>> {
    let mut slice = message;
    capnp::serialize::read_message_from_flat_slice(&mut slice, ReaderOptions::new()).ok()
}

fn rejected() -> ffi::RoundTrip {
    ffi::RoundTrip {
        ok: false,
        message: Vec::new(),
    }
}

fn written(builder: &Builder<HeapAllocator>) -> ffi::RoundTrip {
    ffi::RoundTrip {
        ok: true,
        message: capnp::serialize::write_message_to_words(builder),
    }
}

fn span_context_round_trip(message: &[u8]) -> ffi::RoundTrip {
    let Some(reader) = read_flat(message) else {
        return rejected();
    };
    let Some(context) = reader
        .get_root::<span_context::Reader<'_>>()
        .and_then(SpanContext::from_capnp)
        .ok()
    else {
        return rejected();
    };
    let mut builder = Builder::new_default();
    context.to_capnp(builder.init_root());
    written(&builder)
}

fn invocation_span_context_round_trip(message: &[u8]) -> ffi::RoundTrip {
    let Some(reader) = read_flat(message) else {
        return rejected();
    };
    let Some(context) = reader
        .get_root::<invocation_span_context::Reader<'_>>()
        .and_then(InvocationSpanContext::from_capnp)
        .ok()
        .flatten()
    else {
        return rejected();
    };
    let mut builder = Builder::new_default();
    context.to_capnp(builder.init_root());
    written(&builder)
}

fn span_status_round_trip(message: &[u8]) -> ffi::RoundTrip {
    let Some(reader) = read_flat(message) else {
        return rejected();
    };
    let Some(status) = reader
        .get_root::<span_status::Reader<'_>>()
        .and_then(SpanStatus::from_capnp)
        .ok()
    else {
        return rejected();
    };
    let mut builder = Builder::new_default();
    status.to_capnp(builder.init_root());
    written(&builder)
}

fn span_context_to_traceparent(
    high: u64,
    low: u64,
    span_id: u64,
    has_span_id: bool,
    flags: u8,
    has_flags: bool,
    out: &mut [u8],
) -> usize {
    let context = SpanContext {
        trace_id: TraceId { high, low },
        span_id: has_span_id.then_some(SpanId(span_id)),
        trace_flags: has_flags.then_some(TraceFlags(flags)),
    };
    context.to_traceparent().map_or(0, |text| write(text, out))
}
