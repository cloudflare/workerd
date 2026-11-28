// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::collections::HashMap;

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
#[expect(
    clippy::struct_excessive_bools,
    reason = "independent per-invocation switches, not states of one thing"
)]
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
    /// Without it nothing is handed out: the buffer only works out the root's OTLP form, for an
    /// invocation whose logs are exported and whose trace is not.
    pub export_spans: bool,
    /// Keeps the root's OTLP form once it closes, for [`SpanBuffer::take_root`].
    pub keep_root: bool,
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
/// is 1.
pub struct SpanBuffer {
    options: Options,
    resource: proto::Resource,
    /// Applied to every span at close without overriding its own attributes.
    default_attributes: Attributes,
    root_closed: bool,
    last_sequence_number: i64,
    open: HashMap<u64, OpenSpan>,
    closed: Vec<proto::Span>,
    /// Encoded size of `closed`, kept so a close does not re-measure the batch.
    closed_bytes: usize,
    /// The closed root, with `keep_root`.
    root: Option<proto::Span>,
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
            open: HashMap::new(),
            closed: Vec::new(),
            closed_bytes: 0,
            root: None,
        }
    }

    /// The root span as it was handed out, once it has closed and with `keep_root`. Its
    /// attributes describe the whole invocation.
    pub fn take_root(&mut self) -> Option<proto::Span> {
        self.root.take()
    }

    /// Starts buffering a span. Returns false, leaving the buffer unchanged, when the id is already
    /// open or too many spans are.
    pub fn open(&mut self, span_id: u64, parent_span_id: u64, name: String, start: i64) -> bool {
        if self.open.len() >= MAX_OPEN_SPANS || self.open.contains_key(&span_id) {
            return false;
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

    /// Records an explicit status on an open span. UNSET is ignored and OK is final.
    pub fn set_status(&mut self, span_id: u64, code: proto::StatusCode, message: String) {
        if let Some(span) = self.open.get_mut(&span_id) {
            if code == proto::StatusCode::Unset
                || span
                    .status
                    .as_ref()
                    .is_some_and(|status| status.code == i32::from(proto::StatusCode::Ok))
            {
                return;
            }
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
            key_value("exception.message", message),
        ];
        if let Some(stack) = stack {
            attributes.push(key_value("exception.stacktrace", stack));
        }
        span.events.push(proto::Event {
            time_unix_nano,
            name: "exception".to_owned(),
            attributes,
        });
    }

    /// Closes a span with the attributes it ended up with. Returns a batch when one is due.
    pub fn close(&mut self, span_id: u64, end: i64, attributes: Attributes) -> Option<Batch> {
        let span = self.open.remove(&span_id)?;
        self.write(span_id, span, end, attributes);
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

    /// Adds a closed span to the batch in its OTLP form.
    fn write(&mut self, span_id: u64, span: OpenSpan, end: i64, mut attributes: Attributes) {
        let is_root = span_id == self.options.root_span_id;
        rules::add_url_attributes(&mut attributes, self.options.redact_query_string);
        attributes.set_defaults(&self.default_attributes);
        attributes.set(
            "cloudflare.invocation.sequence.number",
            span.sequence_number,
        );

        let kind = rules::span_kind(&span.name, &attributes, is_root);
        let (status, error_type) = resolve_status(span.status, &attributes, is_root);
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
        let keep = is_root && self.options.keep_root;
        if is_root {
            self.root_closed = true;
        }
        if !self.options.export_spans {
            if keep {
                self.root = Some(out);
            }
            return;
        }
        if keep {
            self.root = Some(out.clone());
        }
        self.closed_bytes += out.encoded_len();
        self.closed.push(out);
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
                scope_spans: vec![proto::ScopeSpans {
                    scope: Some(proto::InstrumentationScope {
                        name: "workers-runtime".to_owned(),
                    }),
                    spans,
                }],
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
/// A status the worker set to OK or ERROR stands. Otherwise the span's attributes may make it an
/// error (see [`rules::derived_error_type`]), which is the only case that adds `error.type`.
fn resolve_status(
    status: Option<proto::Status>,
    attributes: &Attributes,
    is_root: bool,
) -> (Option<proto::Status>, Option<String>) {
    if status
        .as_ref()
        .is_some_and(|status| status.code != i32::from(proto::StatusCode::Unset))
    {
        return (status, None);
    }
    let Some(error_type) = rules::derived_error_type(attributes, is_root) else {
        return (status, None);
    };
    let status = proto::Status {
        code: proto::StatusCode::Error.into(),
        ..status.unwrap_or_default()
    };
    (Some(status), Some(error_type))
}

#[cfg(test)]
#[path = "buffer-test.rs"]
mod tests;
