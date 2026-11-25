// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! OTLP/JSON: an encoded export request re-encoded as the JSON a collector takes as
//! `application/json`. That is protobuf's JSON mapping with OTLP's exceptions: ids are hex, not
//! base64, and enums are numbers. 64-bit integers are strings, as in the mapping. Fields at their
//! default are left out.

use std::fmt::Write;

use prost::Message;
use serde_json::Map;
use serde_json::Value;
use serde_json::json;

use crate::proto;
use crate::proto::any_value;

/// `request`, an encoded `ExportTraceServiceRequest`, or with `logs` an
/// `ExportLogsServiceRequest`, as OTLP/JSON.
pub fn encode(logs: bool, request: &[u8]) -> Result<Vec<u8>, prost::DecodeError> {
    let value = if logs {
        let request = proto::ExportLogsServiceRequest::decode(request)?;
        json!({ "resourceLogs": request.resource_logs.iter().map(resource_logs).collect::<Vec<_>>() })
    } else {
        let request = proto::ExportTraceServiceRequest::decode(request)?;
        json!({ "resourceSpans": request.resource_spans.iter().map(resource_spans).collect::<Vec<_>>() })
    };
    Ok(value.to_string().into_bytes())
}

fn resource_spans(r: &proto::ResourceSpans) -> Value {
    let scopes = r.scope_spans.iter().map(|s| {
        let spans: Vec<_> = s.spans.iter().map(span).collect();
        object([
            ("scope", s.scope.as_ref().map(scope)),
            ("spans", Some(spans.into())),
        ])
    });
    object([
        ("resource", r.resource.as_ref().map(resource)),
        ("scopeSpans", Some(scopes.collect::<Vec<_>>().into())),
    ])
}

fn resource_logs(r: &proto::ResourceLogs) -> Value {
    let scopes = r.scope_logs.iter().map(|s| {
        let records: Vec<_> = s.log_records.iter().map(log_record).collect();
        object([
            ("scope", s.scope.as_ref().map(scope)),
            ("logRecords", Some(records.into())),
        ])
    });
    object([
        ("resource", r.resource.as_ref().map(resource)),
        ("scopeLogs", Some(scopes.collect::<Vec<_>>().into())),
    ])
}

fn resource(r: &proto::Resource) -> Value {
    json!({ "attributes": attributes(&r.attributes) })
}

fn scope(s: &proto::InstrumentationScope) -> Value {
    json!({ "name": s.name })
}

fn span(s: &proto::Span) -> Value {
    object([
        ("traceId", hex(&s.trace_id)),
        ("spanId", hex(&s.span_id)),
        ("parentSpanId", hex(&s.parent_span_id)),
        ("name", Some(s.name.as_str().into())),
        ("kind", nonzero(s.kind)),
        (
            "startTimeUnixNano",
            Some(s.start_time_unix_nano.to_string().into()),
        ),
        (
            "endTimeUnixNano",
            Some(s.end_time_unix_nano.to_string().into()),
        ),
        ("attributes", list(&s.attributes)),
        (
            "events",
            (!s.events.is_empty()).then(|| s.events.iter().map(event).collect()),
        ),
        ("status", s.status.as_ref().map(status)),
        ("flags", nonzero(s.flags)),
    ])
}

fn event(e: &proto::Event) -> Value {
    object([
        ("timeUnixNano", Some(e.time_unix_nano.to_string().into())),
        ("name", Some(e.name.as_str().into())),
        ("attributes", list(&e.attributes)),
    ])
}

fn status(s: &proto::Status) -> Value {
    object([
        (
            "message",
            (!s.message.is_empty()).then(|| s.message.as_str().into()),
        ),
        ("code", nonzero(s.code)),
    ])
}

fn log_record(r: &proto::LogRecord) -> Value {
    object([
        ("timeUnixNano", Some(r.time_unix_nano.to_string().into())),
        (
            "observedTimeUnixNano",
            Some(r.observed_time_unix_nano.to_string().into()),
        ),
        ("severityNumber", nonzero(r.severity_number)),
        (
            "severityText",
            (!r.severity_text.is_empty()).then(|| r.severity_text.as_str().into()),
        ),
        ("body", r.body.as_ref().map(any)),
        ("attributes", list(&r.attributes)),
        ("flags", nonzero(r.flags)),
        ("traceId", hex(&r.trace_id)),
        ("spanId", hex(&r.span_id)),
        (
            "eventName",
            (!r.event_name.is_empty()).then(|| r.event_name.as_str().into()),
        ),
    ])
}

fn attributes(kvs: &[proto::KeyValue]) -> Vec<Value> {
    kvs.iter()
        .map(|kv| {
            object([
                ("key", Some(kv.key.as_str().into())),
                ("value", kv.value.as_ref().map(any)),
            ])
        })
        .collect()
}

fn list(kvs: &[proto::KeyValue]) -> Option<Value> {
    (!kvs.is_empty()).then(|| attributes(kvs).into())
}

fn any(v: &proto::AnyValue) -> Value {
    match &v.value {
        None => json!({}),
        Some(any_value::Value::String(s)) => json!({ "stringValue": s }),
        Some(any_value::Value::Bool(b)) => json!({ "boolValue": b }),
        Some(any_value::Value::Int(i)) => json!({ "intValue": i.to_string() }),
        // JSON has no NaN or infinities; serde_json writes them as null.
        Some(any_value::Value::Double(d)) => json!({ "doubleValue": d }),
        Some(any_value::Value::Array(a)) => {
            json!({ "arrayValue": { "values": a.values.iter().map(any).collect::<Vec<_>>() } })
        }
        Some(any_value::Value::KvList(l)) => {
            json!({ "kvlistValue": { "values": attributes(&l.values) } })
        }
    }
}

fn hex(bytes: &[u8]) -> Option<Value> {
    (!bytes.is_empty()).then(|| {
        bytes
            .iter()
            .fold(String::with_capacity(bytes.len() * 2), |mut s, b| {
                // Writing to a String cannot fail.
                let _ = write!(s, "{b:02x}");
                s
            })
            .into()
    })
}

fn nonzero<T: Into<Value> + Default + PartialEq>(n: T) -> Option<Value> {
    (n != T::default()).then(|| n.into())
}

/// An object of the fields that are `Some`.
fn object<const N: usize>(fields: [(&str, Option<Value>); N]) -> Value {
    let map: Map<String, Value> = fields
        .into_iter()
        .filter_map(|(k, v)| Some((k.to_owned(), v?)))
        .collect();
    map.into()
}

#[cfg(test)]
#[path = "json-test.rs"]
mod tests;
