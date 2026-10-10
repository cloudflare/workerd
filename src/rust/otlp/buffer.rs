// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::collections::HashMap;
use std::collections::HashSet;

use prost::Message;

use crate::attributes::Attributes;
use crate::attributes::key_value;
use crate::proto;
use crate::rules;

/// A batch is handed out once it reaches either bound.
const MAX_BATCH_SPANS: usize = 512;
const MAX_BATCH_BYTES: usize = 1 << 20;
/// Spans awaiting their close; further opens are refused once this many are buffered.
const MAX_OPEN_SPANS: usize = 1024;

/// What is fixed for one invocation's spans.
pub struct Options {
    /// The 16-byte trace id every span shares.
    pub trace_id: Vec<u8>,
    /// The W3C trace flags, zero when the trace carries none.
    pub trace_flags: u32,
    /// The invocation's own span. It is the one whose kind and status follow the root rules, and
    /// its close is when the invocation's spans are handed out.
    pub root_span_id: u64,
    pub redact_query_string: bool,
    /// Leaves every timestamp at zero, for tests that compare whole spans.
    pub omit_timestamps: bool,
}

/// An encoded `ExportTraceServiceRequest` and the number of spans in it.
pub struct Batch {
    pub request: Vec<u8>,
    pub span_count: usize,
}

struct OpenSpan {
    parent_span_id: u64,
    name: String,
    start: i64,
    sequence_number: i64,
    status: Option<proto::Status>,
    events: Vec<proto::Event>,
    last_exception_message: Option<String>,
}

/// One invocation's spans on their way to an OTLP collector.
///
/// A span is buffered from its open until its close, which is when its attributes arrive and its
/// OTLP form is decided (see [`rules`]). Closed spans collect into a batch that is handed out as an
/// encoded request when the root span closes or the batch grows large. Once the root has closed,
/// every later close is handed out on its own: such spans belong to work that outlives the
/// invocation, and nothing bounds how long the next one would take to arrive.
///
/// Each span also gets `cloudflare.invocation.sequence.number`, counted in open order so the root
/// is 1, and a start time no other span of the invocation shares.
pub struct SpanBuffer {
    options: Options,
    resource: proto::Resource,
    /// Applied to every span at close without overriding its own attributes.
    default_attributes: Attributes,
    root_closed: bool,
    last_sequence_number: i64,
    /// Start times handed out so far, as unix nanoseconds.
    start_times: HashSet<i64>,
    open: HashMap<u64, OpenSpan>,
    closed: Vec<proto::Span>,
    /// Encoded size of `closed`, kept so a close does not re-measure the batch.
    closed_bytes: usize,
}

impl SpanBuffer {
    pub fn new(options: Options, resource: Attributes, default_attributes: Attributes) -> Self {
        Self {
            options,
            resource: proto::Resource {
                attributes: resource.into_proto(),
            },
            default_attributes,
            root_closed: false,
            last_sequence_number: 0,
            start_times: HashSet::new(),
            open: HashMap::new(),
            closed: Vec::new(),
            closed_bytes: 0,
        }
    }

    /// Starts buffering a span. Returns false, leaving the buffer unchanged, when the id is already
    /// open or too many spans are.
    pub fn open(&mut self, span_id: u64, parent_span_id: u64, name: String, start: i64) -> bool {
        if self.open.len() >= MAX_OPEN_SPANS || self.open.contains_key(&span_id) {
            return false;
        }
        let mut start = start;
        while !self.start_times.insert(start) {
            start += 1;
        }
        self.last_sequence_number += 1;
        self.open.insert(
            span_id,
            OpenSpan {
                parent_span_id,
                name,
                start,
                sequence_number: self.last_sequence_number,
                status: None,
                events: Vec::new(),
                last_exception_message: None,
            },
        );
        true
    }

    /// Renames an open span. Like every update, it is ignored for a span that is not open.
    pub fn set_name(&mut self, span_id: u64, name: String) {
        if let Some(span) = self.open.get_mut(&span_id) {
            span.name = name;
        }
    }

    /// Records the status the worker set on an open span.
    pub fn set_status(&mut self, span_id: u64, code: proto::StatusCode, message: String) {
        if let Some(span) = self.open.get_mut(&span_id) {
            span.status = Some(proto::Status {
                message,
                code: code.into(),
            });
        }
    }

    /// Records an exception on an open span as an `exception` event.
    pub fn add_exception(
        &mut self,
        span_id: u64,
        time: i64,
        name: String,
        message: String,
        stack: Option<String>,
    ) {
        let time_unix_nano = self.timestamp(time);
        let Some(span) = self.open.get_mut(&span_id) else {
            return;
        };
        let mut attributes = vec![
            key_value("exception.type", name),
            key_value("exception.message", message.clone()),
        ];
        if let Some(stack) = stack {
            attributes.push(key_value("exception.stacktrace", stack));
        }
        span.events.push(proto::Event {
            time_unix_nano,
            name: "exception".to_owned(),
            attributes,
        });
        span.last_exception_message = Some(message);
    }

    /// Closes a span with the attributes it ended up with. Returns a batch when one is due.
    pub fn close(&mut self, span_id: u64, end: i64, attributes: Attributes) -> Option<Batch> {
        let span = self.open.remove(&span_id)?;
        if !self.write(span_id, span, end, attributes) {
            return None;
        }
        let due = self.root_closed
            || self.closed.len() >= MAX_BATCH_SPANS
            || self.closed_bytes >= MAX_BATCH_BYTES;
        if due { self.take_batch() } else { None }
    }

    /// Ends the invocation: spans still open are closed at `now`, flagged as not ended, and
    /// everything left is handed out.
    pub fn finish(&mut self, now: i64) -> Option<Batch> {
        let mut open: Vec<_> = self.open.drain().collect();
        open.sort_by_key(|(_, span)| span.sequence_number);
        for (span_id, span) in open {
            let mut attributes = Attributes::default();
            attributes.push("cloudflare.warning.type", "span_not_ended");
            attributes.push(
                "cloudflare.warning.message",
                "The span was still open when the invocation ended.",
            );
            self.write(span_id, span, now, attributes);
        }
        self.take_batch()
    }

    /// Adds a closed span to the batch in its OTLP form; false when the span is not reported.
    fn write(
        &mut self,
        span_id: u64,
        span: OpenSpan,
        end: i64,
        mut attributes: Attributes,
    ) -> bool {
        let is_root = span_id == self.options.root_span_id;
        rules::add_url_attributes(&mut attributes, self.options.redact_query_string);
        if rules::is_internal_binding_fetch(&span.name, &attributes) {
            return false;
        }
        attributes.set_defaults(&self.default_attributes);
        attributes.set(
            "cloudflare.invocation.sequence.number",
            span.sequence_number,
        );

        let kind = rules::span_kind(&span.name, &attributes, is_root);
        let (status, error_type) = resolve_status(
            span.status,
            span.last_exception_message,
            &attributes,
            is_root,
        );
        let mut attributes = attributes.into_proto();
        if let Some(error_type) = error_type {
            attributes.push(key_value("error.type", error_type));
        }

        let parent_span_id = match span.parent_span_id {
            0 => Vec::new(),
            id => id.to_be_bytes().to_vec(),
        };
        let out = proto::Span {
            trace_id: self.options.trace_id.clone(),
            span_id: span_id.to_be_bytes().to_vec(),
            parent_span_id,
            name: span.name,
            kind: kind.into(),
            start_time_unix_nano: self.timestamp(span.start),
            // A span never ends before it starts, whatever the clocks said.
            end_time_unix_nano: self.timestamp(end.max(span.start)),
            attributes,
            events: span.events,
            status,
            flags: self.options.trace_flags,
        };
        self.closed_bytes += out.encoded_len();
        self.closed.push(out);
        if is_root {
            self.root_closed = true;
        }
        true
    }

    fn take_batch(&mut self) -> Option<Batch> {
        if self.closed.is_empty() {
            return None;
        }
        let spans = std::mem::take(&mut self.closed);
        self.closed_bytes = 0;
        let span_count = spans.len();
        let request = proto::ExportTraceServiceRequest {
            resource_spans: vec![proto::ResourceSpans {
                resource: Some(self.resource.clone()),
                scope_spans: vec![proto::ScopeSpans { spans }],
            }],
        };
        Some(Batch {
            request: request.encode_to_vec(),
            span_count,
        })
    }

    fn timestamp(&self, unix_nanos: i64) -> u64 {
        if self.options.omit_timestamps {
            0
        } else {
            unix_nanos.cast_unsigned()
        }
    }
}

/// The status a span is exported with, and the `error.type` attribute that goes with it.
///
/// A status the worker set to OK or ERROR stands. Otherwise an exception makes the span an error
/// with the last exception's message, and failing that the span's attributes may (see
/// [`rules::derived_error_type`]), which is the only case that adds `error.type`.
fn resolve_status(
    status: Option<proto::Status>,
    last_exception_message: Option<String>,
    attributes: &Attributes,
    is_root: bool,
) -> (Option<proto::Status>, Option<String>) {
    let error = i32::from(proto::StatusCode::Error);
    if status
        .as_ref()
        .is_some_and(|status| status.code != i32::from(proto::StatusCode::Unset))
    {
        return (status, None);
    }
    if let Some(message) = last_exception_message {
        let status = proto::Status {
            message,
            code: error,
        };
        return (Some(status), None);
    }
    let Some(error_type) = rules::derived_error_type(attributes, is_root) else {
        return (status, None);
    };
    let status = proto::Status {
        code: error,
        ..status.unwrap_or_default()
    };
    (Some(status), Some(error_type))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proto::SpanKind;
    use crate::proto::StatusCode;
    use crate::proto::any_value::Value;

    /// Above every id the tests hand out in a loop.
    const ROOT: u64 = 0x1000;
    const T0: i64 = 1_000_000_000;

    fn buffer_with(redact_query_string: bool, default_attributes: Attributes) -> SpanBuffer {
        let mut resource = Attributes::default();
        resource.push("service.name", "my-worker");
        SpanBuffer::new(
            Options {
                trace_id: (1..=16).collect(),
                trace_flags: 1,
                root_span_id: ROOT,
                redact_query_string,
                omit_timestamps: false,
            },
            resource,
            default_attributes,
        )
    }

    fn buffer() -> SpanBuffer {
        buffer_with(false, Attributes::default())
    }

    fn attributes(pairs: &[(&str, Value)]) -> Attributes {
        let mut attributes = Attributes::default();
        for (key, value) in pairs {
            attributes.push(*key, value.clone());
        }
        attributes
    }

    fn string(value: &str) -> Value {
        Value::String(value.to_owned())
    }

    fn decode(batch: &Batch) -> proto::ExportTraceServiceRequest {
        let request = proto::ExportTraceServiceRequest::decode(batch.request.as_slice()).unwrap();
        assert_eq!(request.resource_spans.len(), 1);
        assert_eq!(request.resource_spans[0].scope_spans.len(), 1);
        assert_eq!(spans(&request).len(), batch.span_count);
        request
    }

    fn spans(request: &proto::ExportTraceServiceRequest) -> &[proto::Span] {
        &request.resource_spans[0].scope_spans[0].spans
    }

    fn named<'a>(request: &'a proto::ExportTraceServiceRequest, name: &str) -> &'a proto::Span {
        spans(request)
            .iter()
            .find(|span| span.name == name)
            .unwrap()
    }

    fn attribute<'a>(attributes: &'a [proto::KeyValue], key: &str) -> Option<&'a Value> {
        attributes
            .iter()
            .find(|kv| kv.key == key)
            .and_then(|kv| kv.value.as_ref()?.value.as_ref())
    }

    fn status_code(span: &proto::Span) -> StatusCode {
        span.status
            .as_ref()
            .map_or(StatusCode::Unset, proto::Status::code)
    }

    #[test]
    fn spans_are_batched_until_the_root_closes() {
        let mut defaults = Attributes::default();
        defaults.push("cloudflare.entrypoint", "MyEntrypoint");
        defaults.push("faas.trigger", "not-applied");
        let mut buffer = buffer_with(false, defaults);

        assert!(buffer.open(ROOT, 0x20, "GET".to_owned(), T0));
        assert!(buffer.open(1, ROOT, "fetch".to_owned(), T0));
        // An id that is already open is refused rather than clobbering the open span.
        assert!(!buffer.open(1, ROOT, "other".to_owned(), T0));
        assert!(buffer.open(3, ROOT, "my-span".to_owned(), T0));
        buffer.set_name(3, "renamed".to_owned());
        buffer.set_status(3, StatusCode::Error, "boom".to_owned());
        buffer.add_exception(3, T0 + 5, "TypeError".to_owned(), "boom".to_owned(), None);
        // Updates for a span that is not open are ignored.
        buffer.set_name(2, "ignored".to_owned());

        // Nothing is handed out until the root closes.
        assert!(
            buffer
                .close(1, T0 + 10, attributes(&[("cached", Value::Bool(true))]))
                .is_none()
        );
        assert!(buffer.close(3, T0 + 10, Attributes::default()).is_none());
        let batch = buffer
            .close(
                ROOT,
                T0 + 20,
                attributes(&[
                    ("faas.trigger", string("http")),
                    ("cloudflare.outcome", string("ok")),
                ]),
            )
            .unwrap();

        let request = decode(&batch);
        let resource = request.resource_spans[0].resource.as_ref().unwrap();
        assert_eq!(
            attribute(&resource.attributes, "service.name"),
            Some(&string("my-worker"))
        );
        let [fetch, renamed, root] = spans(&request) else {
            panic!("expected three spans");
        };
        assert_eq!(fetch.name, "fetch");
        assert_eq!(fetch.trace_id, (1..=16).collect::<Vec<u8>>());
        assert_eq!(fetch.span_id, [0, 0, 0, 0, 0, 0, 0, 1]);
        assert_eq!(fetch.parent_span_id, [0, 0, 0, 0, 0, 0, 0x10, 0]);
        assert_eq!(fetch.flags, 1);
        assert_eq!(
            attribute(&fetch.attributes, "cached"),
            Some(&Value::Bool(true))
        );
        assert_eq!(renamed.name, "renamed");
        assert_eq!(status_code(renamed), StatusCode::Error);
        assert_eq!(renamed.status.as_ref().unwrap().message, "boom");
        let [exception] = renamed.events.as_slice() else {
            panic!("expected one event");
        };
        assert_eq!(exception.name, "exception");
        assert_eq!(exception.time_unix_nano, (T0 + 5).cast_unsigned());
        assert_eq!(
            attribute(&exception.attributes, "exception.type"),
            Some(&string("TypeError"))
        );
        assert_eq!(
            attribute(&exception.attributes, "exception.stacktrace"),
            None
        );
        assert_eq!(root.name, "GET");
        assert_eq!(root.kind(), SpanKind::Server);
        assert_eq!(root.status, None);
        assert_eq!(root.parent_span_id, [0, 0, 0, 0, 0, 0, 0, 0x20]);

        // Spans are numbered in open order, the root first, and every span carries the default
        // attributes, which do not override its own.
        for (span, sequence_number) in [(fetch, 2), (renamed, 3), (root, 1)] {
            assert_eq!(
                attribute(&span.attributes, "cloudflare.invocation.sequence.number"),
                Some(&Value::Int(sequence_number))
            );
            assert_eq!(
                attribute(&span.attributes, "cloudflare.entrypoint"),
                Some(&string("MyEntrypoint"))
            );
        }
        assert_eq!(
            attribute(&fetch.attributes, "faas.trigger"),
            Some(&string("not-applied"))
        );
        assert_eq!(
            attribute(&root.attributes, "faas.trigger"),
            Some(&string("http"))
        );

        // A span closing after the root is handed out as soon as it closes.
        assert!(buffer.open(4, ROOT, "late".to_owned(), T0));
        let batch = buffer.close(4, T0, Attributes::default()).unwrap();
        assert_eq!(spans(&decode(&batch))[0].name, "late");
        assert!(buffer.finish(T0).is_none());
    }

    #[test]
    fn a_full_batch_is_handed_out_and_open_spans_are_capped() {
        let mut buffer = buffer();
        for id in 1..MAX_BATCH_SPANS as u64 {
            assert!(buffer.open(id, ROOT, "s".to_owned(), T0));
            assert!(buffer.close(id, T0, Attributes::default()).is_none());
        }
        assert!(buffer.open(1, ROOT, "s".to_owned(), T0));
        let batch = buffer.close(1, T0, Attributes::default()).unwrap();
        assert_eq!(batch.span_count, MAX_BATCH_SPANS);

        for id in 1..=MAX_OPEN_SPANS as u64 {
            assert!(buffer.open(id, ROOT, "s".to_owned(), T0));
        }
        assert!(!buffer.open(MAX_OPEN_SPANS as u64 + 1, ROOT, "s".to_owned(), T0));

        // Spans still open at the end are closed then, flagged, in the order they were opened.
        let batch = buffer.finish(T0 - 1).unwrap();
        assert_eq!(batch.span_count, MAX_OPEN_SPANS);
        let request = decode(&batch);
        let unclosed = spans(&request);
        assert_eq!(unclosed[0].span_id, 1u64.to_be_bytes());
        assert_eq!(unclosed[1].span_id, 2u64.to_be_bytes());
        assert_eq!(
            attribute(&unclosed[0].attributes, "cloudflare.warning.type"),
            Some(&string("span_not_ended"))
        );
        assert!(attribute(&unclosed[0].attributes, "cloudflare.warning.message").is_some());
        assert!(unclosed[0].end_time_unix_nano >= unclosed[0].start_time_unix_nano);
        assert!(buffer.finish(T0).is_none());
    }

    #[test]
    fn a_batch_is_handed_out_once_it_is_large() {
        let mut buffer = buffer();
        let big = "x".repeat(MAX_BATCH_BYTES / 2);
        assert!(buffer.open(1, ROOT, "s".to_owned(), T0));
        assert!(
            buffer
                .close(1, T0, attributes(&[("big", string(&big))]))
                .is_none()
        );
        assert!(buffer.open(2, ROOT, "s".to_owned(), T0));
        let batch = buffer
            .close(2, T0, attributes(&[("big", string(&big))]))
            .unwrap();
        assert_eq!(batch.span_count, 2);
    }

    #[test]
    fn kind_status_and_url_rules_apply_at_close() {
        let mut buffer = buffer();
        assert!(buffer.open(ROOT, 0, "scheduled".to_owned(), T0));
        for (id, name) in [(1, "fetch"), (2, "fetch"), (3, "queue_send"), (6, "fetch")] {
            assert!(buffer.open(id, ROOT, name.to_owned(), T0));
        }

        // A 4xx child becomes an error with error.type; its URL is split into parts.
        buffer.close(
            1,
            T0,
            attributes(&[
                (
                    "url.full",
                    string("https://example.com:8443/a/b?x=1&y#frag"),
                ),
                ("http.response.status_code", Value::Int(404)),
            ]),
        );
        // A fetch to a binding's internal host is not reported.
        buffer.close(
            2,
            T0,
            attributes(&[("url.full", string("https://d1/query"))]),
        );
        buffer.close(3, T0, Attributes::default());
        // A 2xx is left unset, and an end before the start is moved to the start.
        buffer.close(
            6,
            T0 - 1,
            attributes(&[
                ("url.full", string("http://[::1]")),
                ("http.response.status_code", Value::Int(200)),
            ]),
        );
        // A root with a non-ok outcome is an error, and a consumer for a timer trigger.
        let batch = buffer
            .close(
                ROOT,
                T0,
                attributes(&[
                    ("faas.trigger", string("timer")),
                    ("cloudflare.outcome", string("exceededCpu")),
                ]),
            )
            .unwrap();
        let request = decode(&batch);
        assert_eq!(batch.span_count, 4);

        let fetch = named(&request, "fetch");
        assert_eq!(fetch.kind(), SpanKind::Client);
        assert_eq!(status_code(fetch), StatusCode::Error);
        assert_eq!(
            attribute(&fetch.attributes, "error.type"),
            Some(&string("404"))
        );
        assert_eq!(
            attribute(&fetch.attributes, "server.port"),
            Some(&Value::Int(8443))
        );
        assert_eq!(
            attribute(&fetch.attributes, "url.query"),
            Some(&string("x=1&y"))
        );

        let queue_send = named(&request, "queue_send");
        assert_eq!(queue_send.kind(), SpanKind::Producer);
        // The span that was not reported took a sequence number at open.
        assert_eq!(
            attribute(
                &queue_send.attributes,
                "cloudflare.invocation.sequence.number"
            ),
            Some(&Value::Int(4))
        );

        let ipv6 = &spans(&request)[2];
        assert_eq!(ipv6.status, None);
        assert_eq!(attribute(&ipv6.attributes, "error.type"), None);
        assert_eq!(ipv6.end_time_unix_nano, ipv6.start_time_unix_nano);
        // Spans opened at the same instant get distinct start times.
        assert!(ipv6.start_time_unix_nano > fetch.start_time_unix_nano);

        let root = named(&request, "scheduled");
        assert_eq!(root.kind(), SpanKind::Consumer);
        assert_eq!(status_code(root), StatusCode::Error);
        assert_eq!(
            attribute(&root.attributes, "error.type"),
            Some(&string("exceededCpu"))
        );
        assert!(root.parent_span_id.is_empty());
    }

    #[test]
    fn a_status_the_worker_set_and_exceptions_come_before_derived_errors() {
        let mut buffer = buffer_with(true, Attributes::default());
        assert!(buffer.open(ROOT, 0, "GET".to_owned(), T0));
        assert!(buffer.open(1, ROOT, "mine".to_owned(), T0));
        assert!(buffer.open(2, ROOT, "unset".to_owned(), T0));
        buffer.set_status(1, StatusCode::Ok, String::new());
        buffer.set_status(2, StatusCode::Unset, String::new());
        let failed = attributes(&[("http.response.status_code", Value::Int(503))]);
        buffer.close(1, T0, failed.clone());
        buffer.close(2, T0, failed);
        buffer.add_exception(
            ROOT,
            T0,
            "Error".to_owned(),
            "boom".to_owned(),
            Some("at fetch".to_owned()),
        );
        let batch = buffer
            .close(
                ROOT,
                T0,
                attributes(&[
                    ("url.full", string("https://example.com/p?secret=1")),
                    ("http.response.status_code", Value::Int(500)),
                ]),
            )
            .unwrap();
        let request = decode(&batch);

        let mine = named(&request, "mine");
        assert_eq!(status_code(mine), StatusCode::Ok);
        assert_eq!(attribute(&mine.attributes, "error.type"), None);
        // An explicit UNSET does not stand in the way of a derived error.
        let unset = named(&request, "unset");
        assert_eq!(status_code(unset), StatusCode::Error);
        assert_eq!(
            attribute(&unset.attributes, "error.type"),
            Some(&string("503"))
        );

        let root = named(&request, "GET");
        assert_eq!(status_code(root), StatusCode::Error);
        assert_eq!(root.status.as_ref().unwrap().message, "boom");
        assert_eq!(attribute(&root.attributes, "error.type"), None);
        assert_eq!(
            attribute(&root.events[0].attributes, "exception.stacktrace"),
            Some(&string("at fetch"))
        );
        assert_eq!(
            attribute(&root.attributes, "url.full"),
            Some(&string("https://example.com/p"))
        );
        assert_eq!(attribute(&root.attributes, "url.query"), None);
    }

    #[test]
    fn timestamps_can_be_omitted() {
        let mut buffer = SpanBuffer::new(
            Options {
                trace_id: vec![0; 16],
                trace_flags: 0,
                root_span_id: ROOT,
                redact_query_string: false,
                omit_timestamps: true,
            },
            Attributes::default(),
            Attributes::default(),
        );
        assert!(buffer.open(ROOT, 0, "GET".to_owned(), T0));
        buffer.add_exception(ROOT, T0, "Error".to_owned(), "boom".to_owned(), None);
        let batch = buffer.close(ROOT, T0 + 1, Attributes::default()).unwrap();
        let request = decode(&batch);
        let root = named(&request, "GET");
        assert_eq!(root.start_time_unix_nano, 0);
        assert_eq!(root.end_time_unix_nano, 0);
        assert_eq!(root.events[0].time_unix_nano, 0);
        assert_eq!(root.flags, 0);
    }
}
