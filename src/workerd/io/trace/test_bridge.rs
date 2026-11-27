// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge through which `trace-types-differential-test.c++` checks the Rust trace types against
//! the C++ of `workerd/io/trace.c++`, which writes and reads them the same way.
//!
//! Ids cross as integers, and text is written into a buffer the caller owns.

#![allow(
    unsafe_code,
    reason = "holds the cxx bridge, which expands to unsafe FFI glue"
)]

use capnp::message::Builder;
use capnp::message::HeapAllocator;
use capnp::message::ReaderOptions;
use trace::AlarmEventInfo;
use trace::Attribute;
use trace::DiagnosticChannelEvent;
use trace::EmailEventInfo;
use trace::EventInfo;
use trace::Exception;
use trace::FetchEventInfo;
use trace::FetchResponseInfo;
use trace::HibernatableWebSocketEventInfo;
use trace::IdText;
use trace::InvocationSpanContext;
use trace::JsRpcEventInfo;
use trace::Log;
use trace::OnHibernateEventInfo;
use trace::Onset;
use trace::Outcome;
use trace::QueueEventInfo;
use trace::Return;
use trace::ScheduledEventInfo;
use trace::SpanClose;
use trace::SpanContext;
use trace::SpanId;
use trace::SpanLinks;
use trace::SpanOpen;
use trace::SpanStatus;
use trace::SpanUpdate;
use trace::StreamDiagnosticsEvent;
use trace::TagValue;
use trace::TailEvent;
use trace::TraceEventInfo;
use trace::TraceFlags;
use trace::TraceId;
use trace::TracePreview;
use trace::format_traceparent;
use trace_capnp::span_status;
use trace_capnp::tag_value;
use worker_interface_capnp::invocation_span_context;
use worker_interface_capnp::span_context;
use worker_interface_capnp::trace as rpc_trace;
use worker_interface_capnp::trace::attribute;
use worker_interface_capnp::trace::span_links;

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

        /// As `span_context_round_trip`, for a `TagValue`.
        fn tag_value_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.Attribute`.
        fn attribute_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.SpanLinks`.
        fn span_links_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.FetchEventInfo`.
        fn fetch_event_info_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.JsRpcEventInfo`.
        fn js_rpc_event_info_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.ScheduledEventInfo`.
        fn scheduled_event_info_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.AlarmEventInfo`.
        fn alarm_event_info_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.OnHibernateEventInfo`.
        fn on_hibernate_event_info_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.QueueEventInfo`.
        fn queue_event_info_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.EmailEventInfo`.
        fn email_event_info_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.TraceEventInfo`.
        fn trace_event_info_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.HibernatableWebSocketEventInfo`.
        fn hibernatable_web_socket_event_info_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.TracePreviewInfo`.
        fn trace_preview_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.FetchResponseInfo`.
        fn fetch_response_info_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for the info of a `Trace.Onset`, written back as an onset
        /// with only its info.
        fn event_info_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.DiagnosticChannelEvent`.
        fn diagnostic_channel_event_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.StreamDiagnosticsEvent`.
        fn stream_diagnostics_event_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.Log`.
        fn log_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.Exception`.
        fn exception_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.Return`.
        fn return_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.SpanOpen`.
        fn span_open_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.SpanClose`.
        fn span_close_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.SpanUpdate`.
        fn span_update_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.Onset`.
        fn onset_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.Outcome`.
        fn outcome_round_trip(message: &[u8]) -> RoundTrip;

        /// As `span_context_round_trip`, for a `Trace.TailEvent`.
        fn tail_event_round_trip(message: &[u8]) -> RoundTrip;

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

/// What a `to_capnp` returns: nothing, or whether it could write the value.
trait Written {
    fn succeeded(self) -> bool;
}

impl Written for () {
    fn succeeded(self) -> bool {
        true
    }
}

impl Written for capnp::Result<()> {
    fn succeeded(self) -> bool {
        self.is_ok()
    }
}

/// Reads a flat message whose root is an `O`, converts it to the Rust type with `read`, and
/// writes that back out with `write`. `read` returns `None` for a message the type rejects
/// without an error.
fn round_trip<O: capnp::traits::Owned, T, W: Written>(
    message: &[u8],
    read: impl FnOnce(O::Reader<'_>) -> capnp::Result<Option<T>>,
    write: impl FnOnce(&T, O::Builder<'_>) -> W,
) -> ffi::RoundTrip {
    let Some(reader) = read_flat(message) else {
        return rejected();
    };
    let Ok(Some(value)) = reader.get_root::<O::Reader<'_>>().and_then(read) else {
        return rejected();
    };
    let mut builder = Builder::new_default();
    if !write(&value, builder.init_root::<O::Builder<'_>>()).succeeded() {
        return rejected();
    }
    written(&builder)
}

fn span_context_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<span_context::Owned, _, _>(
        message,
        |reader| SpanContext::from_capnp(reader).map(Some),
        SpanContext::to_capnp,
    )
}

fn invocation_span_context_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<invocation_span_context::Owned, _, _>(
        message,
        InvocationSpanContext::from_capnp,
        InvocationSpanContext::to_capnp,
    )
}

fn span_status_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<span_status::Owned, _, _>(
        message,
        |reader| SpanStatus::from_capnp(reader).map(Some),
        SpanStatus::to_capnp,
    )
}

fn tag_value_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<tag_value::Owned, _, _>(
        message,
        |reader| TagValue::from_capnp(reader).map(Some),
        TagValue::to_capnp,
    )
}

fn attribute_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<attribute::Owned, _, _>(
        message,
        |reader| Attribute::from_capnp(reader).map(Some),
        Attribute::to_capnp,
    )
}

fn span_links_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<span_links::Owned, _, _>(
        message,
        |reader| SpanLinks::from_capnp(reader).map(Some),
        SpanLinks::to_capnp,
    )
}

fn fetch_event_info_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::fetch_event_info::Owned, _, _>(
        message,
        |reader| FetchEventInfo::from_capnp(reader).map(Some),
        FetchEventInfo::to_capnp,
    )
}

fn js_rpc_event_info_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::js_rpc_event_info::Owned, _, _>(
        message,
        |reader| JsRpcEventInfo::from_capnp(reader).map(Some),
        JsRpcEventInfo::to_capnp,
    )
}

fn scheduled_event_info_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::scheduled_event_info::Owned, _, _>(
        message,
        |reader| ScheduledEventInfo::from_capnp(reader).map(Some),
        ScheduledEventInfo::to_capnp,
    )
}

fn alarm_event_info_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::alarm_event_info::Owned, _, _>(
        message,
        |reader| Ok(Some(AlarmEventInfo::from_capnp(reader))),
        AlarmEventInfo::to_capnp,
    )
}

fn on_hibernate_event_info_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::on_hibernate_event_info::Owned, _, _>(
        message,
        |reader| OnHibernateEventInfo::from_capnp(reader).map(Some),
        OnHibernateEventInfo::to_capnp,
    )
}

fn queue_event_info_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::queue_event_info::Owned, _, _>(
        message,
        |reader| QueueEventInfo::from_capnp(reader).map(Some),
        QueueEventInfo::to_capnp,
    )
}

fn email_event_info_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::email_event_info::Owned, _, _>(
        message,
        |reader| EmailEventInfo::from_capnp(reader).map(Some),
        EmailEventInfo::to_capnp,
    )
}

fn trace_event_info_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::trace_event_info::Owned, _, _>(
        message,
        |reader| TraceEventInfo::from_capnp(reader).map(Some),
        TraceEventInfo::to_capnp,
    )
}

fn hibernatable_web_socket_event_info_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::hibernatable_web_socket_event_info::Owned, _, _>(
        message,
        |reader| HibernatableWebSocketEventInfo::from_capnp(reader).map(Some),
        |info, builder| info.to_capnp(builder),
    )
}

fn trace_preview_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::trace_preview_info::Owned, _, _>(
        message,
        |reader| TracePreview::from_capnp(reader).map(Some),
        TracePreview::to_capnp,
    )
}

fn fetch_response_info_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::fetch_response_info::Owned, _, _>(
        message,
        |reader| Ok(Some(FetchResponseInfo::from_capnp(reader))),
        |info, builder| info.to_capnp(builder),
    )
}

fn event_info_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::onset::Owned, _, _>(
        message,
        |reader| EventInfo::from_capnp(reader.get_info()?).map(Some),
        |info, builder| info.to_capnp(builder.init_info()),
    )
}

fn diagnostic_channel_event_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::diagnostic_channel_event::Owned, _, _>(
        message,
        |reader| DiagnosticChannelEvent::from_capnp(reader).map(Some),
        DiagnosticChannelEvent::to_capnp,
    )
}

fn stream_diagnostics_event_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::stream_diagnostics_event::Owned, _, _>(
        message,
        |reader| StreamDiagnosticsEvent::from_capnp(reader).map(Some),
        |event, builder| event.to_capnp(builder),
    )
}

fn log_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::log::Owned, _, _>(
        message,
        |reader| Log::from_capnp(reader).map(Some),
        Log::to_capnp,
    )
}

fn exception_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::exception::Owned, _, _>(
        message,
        |reader| Exception::from_capnp(reader).map(Some),
        Exception::to_capnp,
    )
}

fn return_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::return_::Owned, _, _>(
        message,
        |reader| Return::from_capnp(reader).map(Some),
        |ret, builder| ret.to_capnp(builder),
    )
}

fn span_open_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::span_open::Owned, _, _>(
        message,
        |reader| SpanOpen::from_capnp(reader).map(Some),
        SpanOpen::to_capnp,
    )
}

fn span_close_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::span_close::Owned, _, _>(
        message,
        |reader| SpanClose::from_capnp(reader).map(Some),
        |close, builder| close.to_capnp(builder),
    )
}

fn span_update_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::span_update::Owned, _, _>(
        message,
        |reader| SpanUpdate::from_capnp(reader).map(Some),
        SpanUpdate::to_capnp,
    )
}

fn onset_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::onset::Owned, _, _>(
        message,
        |reader| Onset::from_capnp(reader).map(Some),
        Onset::to_capnp,
    )
}

fn outcome_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::outcome::Owned, _, _>(
        message,
        |reader| Outcome::from_capnp(reader).map(Some),
        |outcome, builder| outcome.to_capnp(builder),
    )
}

fn tail_event_round_trip(message: &[u8]) -> ffi::RoundTrip {
    round_trip::<rpc_trace::tail_event::Owned, _, _>(
        message,
        |reader| TailEvent::from_capnp(reader).map(Some),
        TailEvent::to_capnp,
    )
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
