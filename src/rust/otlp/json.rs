// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! OTLP/JSON: an encoded export request re-encoded as the JSON a collector takes as
//! `application/json`. That is protobuf's JSON mapping with OTLP's exceptions: ids are hex, not
//! base64, and enums are numbers. 64-bit integers are strings, as in the mapping. Fields at their
//! default are left out.
//!
//! The JSON is written as the decoded request is walked, with no tree in between: this runs on
//! the event loop for every batch.

use std::fmt::Display;
use std::io::Write;

use prost::Message;

use crate::proto;
use crate::proto::any_value::Value;

/// `request`, an encoded `ExportTraceServiceRequest`, or with `logs` an
/// `ExportLogsServiceRequest`, as OTLP/JSON.
pub fn encode(logs: bool, request: &[u8]) -> Result<Vec<u8>, prost::DecodeError> {
    let mut j = Json(Vec::with_capacity(request.len() * 3));
    let mut o = Object::new(&mut j);
    if logs {
        let r = proto::ExportLogsServiceRequest::decode(request)?;
        array(o.key("resourceLogs"), &r.resource_logs, resource_logs);
    } else {
        let r = proto::ExportTraceServiceRequest::decode(request)?;
        array(o.key("resourceSpans"), &r.resource_spans, resource_spans);
    }
    o.end();
    Ok(j.0)
}

/// The output. Writes to a `Vec` cannot fail, so their results are dropped.
struct Json(Vec<u8>);

impl Json {
    fn raw(&mut self, s: &str) {
        self.0.extend_from_slice(s.as_bytes());
    }

    fn str(&mut self, s: &str) {
        let _ = serde_json::to_writer(&mut self.0, s);
    }

    /// A 64-bit integer, as a string.
    fn quoted(&mut self, n: impl Display) {
        let _ = write!(self.0, "\"{n}\"");
    }
}

/// One object being written: `key` starts each member.
struct Object<'a> {
    j: &'a mut Json,
    first: bool,
}

impl<'a> Object<'a> {
    fn new(j: &'a mut Json) -> Self {
        j.raw("{");
        Self { j, first: true }
    }

    fn key(&mut self, key: &str) -> &mut Json {
        if !self.first {
            self.j.raw(",");
        }
        self.first = false;
        self.j.raw("\"");
        self.j.raw(key);
        self.j.raw("\":");
        self.j
    }

    fn int(&mut self, key: &str, n: i64) {
        if n != 0 {
            let _ = write!(self.key(key).0, "{n}");
        }
    }

    fn text(&mut self, key: &str, s: &str) {
        if !s.is_empty() {
            self.key(key).str(s);
        }
    }

    fn hex(&mut self, key: &str, bytes: &[u8]) {
        if bytes.is_empty() {
            return;
        }
        let j = self.key(key);
        j.raw("\"");
        for b in bytes {
            let _ = write!(j.0, "{b:02x}");
        }
        j.raw("\"");
    }

    fn attributes(&mut self, key: &str, kvs: &[proto::KeyValue]) {
        if !kvs.is_empty() {
            array(self.key(key), kvs, key_value);
        }
    }

    fn end(self) {
        self.j.raw("}");
    }
}

fn array<T>(j: &mut Json, items: &[T], item: fn(&mut Json, &T)) {
    j.raw("[");
    for (i, t) in items.iter().enumerate() {
        if i > 0 {
            j.raw(",");
        }
        item(j, t);
    }
    j.raw("]");
}

fn resource_spans(j: &mut Json, r: &proto::ResourceSpans) {
    let mut o = Object::new(j);
    if let Some(resource) = &r.resource {
        resource_(o.key("resource"), resource);
    }
    array(o.key("scopeSpans"), &r.scope_spans, |j, s| {
        let mut o = Object::new(j);
        if let Some(scope) = &s.scope {
            scope_(o.key("scope"), scope);
        }
        array(o.key("spans"), &s.spans, span);
        o.end();
    });
    o.end();
}

fn resource_logs(j: &mut Json, r: &proto::ResourceLogs) {
    let mut o = Object::new(j);
    if let Some(resource) = &r.resource {
        resource_(o.key("resource"), resource);
    }
    array(o.key("scopeLogs"), &r.scope_logs, |j, s| {
        let mut o = Object::new(j);
        if let Some(scope) = &s.scope {
            scope_(o.key("scope"), scope);
        }
        array(o.key("logRecords"), &s.log_records, log_record);
        o.end();
    });
    o.end();
}

fn resource_(j: &mut Json, r: &proto::Resource) {
    let mut o = Object::new(j);
    array(o.key("attributes"), &r.attributes, key_value);
    o.end();
}

fn scope_(j: &mut Json, s: &proto::InstrumentationScope) {
    let mut o = Object::new(j);
    o.key("name").str(&s.name);
    o.end();
}

fn span(j: &mut Json, s: &proto::Span) {
    let mut o = Object::new(j);
    o.hex("traceId", &s.trace_id);
    o.hex("spanId", &s.span_id);
    o.hex("parentSpanId", &s.parent_span_id);
    o.key("name").str(&s.name);
    o.int("kind", s.kind.into());
    o.key("startTimeUnixNano").quoted(s.start_time_unix_nano);
    o.key("endTimeUnixNano").quoted(s.end_time_unix_nano);
    o.attributes("attributes", &s.attributes);
    if !s.events.is_empty() {
        array(o.key("events"), &s.events, event);
    }
    if let Some(status) = &s.status {
        let mut o = Object::new(o.key("status"));
        o.text("message", &status.message);
        o.int("code", status.code.into());
        o.end();
    }
    o.int("flags", s.flags.into());
    o.end();
}

fn event(j: &mut Json, e: &proto::Event) {
    let mut o = Object::new(j);
    o.key("timeUnixNano").quoted(e.time_unix_nano);
    o.key("name").str(&e.name);
    o.attributes("attributes", &e.attributes);
    o.end();
}

fn log_record(j: &mut Json, r: &proto::LogRecord) {
    let mut o = Object::new(j);
    o.key("timeUnixNano").quoted(r.time_unix_nano);
    o.key("observedTimeUnixNano")
        .quoted(r.observed_time_unix_nano);
    o.int("severityNumber", r.severity_number.into());
    o.text("severityText", &r.severity_text);
    if let Some(body) = &r.body {
        any(o.key("body"), body);
    }
    o.attributes("attributes", &r.attributes);
    o.int("flags", r.flags.into());
    o.hex("traceId", &r.trace_id);
    o.hex("spanId", &r.span_id);
    o.text("eventName", &r.event_name);
    o.end();
}

fn key_value(j: &mut Json, kv: &proto::KeyValue) {
    let mut o = Object::new(j);
    o.key("key").str(&kv.key);
    if let Some(value) = &kv.value {
        any(o.key("value"), value);
    }
    o.end();
}

fn any(j: &mut Json, v: &proto::AnyValue) {
    let mut o = Object::new(j);
    match &v.value {
        None => {}
        Some(Value::String(s)) => o.key("stringValue").str(s),
        Some(Value::Bool(b)) => o.key("boolValue").raw(if *b { "true" } else { "false" }),
        Some(Value::Int(i)) => o.key("intValue").quoted(i),
        // JSON has no NaN or infinities; serde_json writes them as null.
        Some(Value::Double(d)) => {
            let _ = serde_json::to_writer(&mut o.key("doubleValue").0, d);
        }
        Some(Value::Array(a)) => {
            let mut o = Object::new(o.key("arrayValue"));
            array(o.key("values"), &a.values, any);
            o.end();
        }
        Some(Value::KvList(l)) => {
            let mut o = Object::new(o.key("kvlistValue"));
            array(o.key("values"), &l.values, key_value);
            o.end();
        }
    }
    o.end();
}

#[cfg(test)]
#[path = "json-test.rs"]
mod tests;
