// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Turns OTLP export requests into Parquet files that `DuckDB` reads directly.
//!
//! One row per span or log record. The columns a query filters on are promoted; every other
//! attribute, the resource's included, goes into `attributes` as a JSON object, so nothing in the
//! request is lost. The schema is `v0-unstable`: names may change before a stable release.
//! Writing the file somewhere is left to the caller.

use std::fmt::Write;
use std::sync::Arc;

use otlp::proto::AnyValue;
use otlp::proto::ExportLogsServiceRequest;
use otlp::proto::ExportTraceServiceRequest;
use otlp::proto::KeyValue;
use otlp::proto::StatusCode;
use otlp::proto::any_value::Value;
use parquet::basic::Compression;
use parquet::basic::ZstdLevel;
use parquet::data_type::BoolType;
use parquet::data_type::ByteArray;
use parquet::data_type::ByteArrayType;
use parquet::data_type::Int32Type;
use parquet::data_type::Int64Type;
use parquet::errors::Result;
use parquet::file::properties::WriterProperties;
use parquet::file::writer::SerializedFileWriter;
use parquet::schema::parser::parse_message_type;
use parquet::schema::types::ColumnPath;
use serde_json::Map;

/// The version every file carries, for readers of mixed versions.
pub const SCHEMA: &str = "v0-unstable";

const SPANS: &str = "
message span {
  required binary node (STRING);
  required binary service (STRING);
  required binary trace_id (STRING);
  required binary span_id (STRING);
  optional binary parent_span_id (STRING);
  required binary name (STRING);
  required int32 kind (INTEGER(8,false));
  required int64 start_unix_us;
  required int64 duration_us;
  required boolean ok;
  optional binary error (STRING);
  optional binary cell (STRING);
  optional binary url (STRING);
  optional int32 http_status (INTEGER(16,false));
  required binary attributes (JSON);
}";

const LOGS: &str = "
message log {
  required binary node (STRING);
  required binary service (STRING);
  optional binary trace_id (STRING);
  optional binary span_id (STRING);
  required int64 time_unix_us;
  required int32 severity_number;
  required binary severity_text (STRING);
  required binary body (STRING);
  optional binary cell (STRING);
  required binary attributes (JSON);
}";

/// One span, flattened.
pub struct Span {
    pub service: String,
    pub trace_id: String,
    pub span_id: String,
    pub parent_span_id: Option<String>,
    pub name: String,
    pub kind: i32,
    pub start_unix_us: i64,
    pub duration_us: i64,
    pub ok: bool,
    pub error: Option<String>,
    pub cell: Option<String>,
    pub url: Option<String>,
    pub http_status: Option<i32>,
    pub attributes: String,
}

/// One log record, flattened.
pub struct Log {
    pub service: String,
    pub trace_id: Option<String>,
    pub span_id: Option<String>,
    pub time_unix_us: i64,
    pub severity_number: i32,
    pub severity_text: String,
    pub body: String,
    pub cell: Option<String>,
    pub attributes: String,
}

impl Span {
    /// What the buffer counts against its flush threshold.
    pub fn size(&self) -> usize {
        128 + self.name.len() + self.attributes.len()
    }
}

impl Log {
    pub fn size(&self) -> usize {
        96 + self.body.len() + self.attributes.len()
    }
}

/// The spans of one request, as rows.
pub fn spans(request: ExportTraceServiceRequest) -> Vec<Span> {
    let mut rows = Vec::new();
    for resource_spans in request.resource_spans {
        let resource = resource_spans
            .resource
            .map(|r| r.attributes)
            .unwrap_or_default();
        let service = string(&resource, "service.name").unwrap_or_default();
        for span in resource_spans.scope_spans.into_iter().flat_map(|s| s.spans) {
            let mut attributes = span.attributes;
            let cell = take(&mut attributes, "cloudflare.durable_object.id");
            let url = take(&mut attributes, "url.full");
            let http_status = take_int(&mut attributes, "http.response.status_code");
            let status = span.status.unwrap_or_default();
            let ok = status.code != StatusCode::Error as i32;
            rows.push(Span {
                service: service.clone(),
                trace_id: hex(&span.trace_id),
                span_id: hex(&span.span_id),
                parent_span_id: (!span.parent_span_id.is_empty())
                    .then(|| hex(&span.parent_span_id)),
                name: span.name,
                kind: span.kind,
                start_unix_us: us(span.start_time_unix_nano),
                duration_us: us(span.end_time_unix_nano) - us(span.start_time_unix_nano),
                ok,
                error: (!ok).then_some(status.message),
                cell,
                url,
                http_status,
                attributes: attributes_json(&resource, &attributes),
            });
        }
    }
    rows
}

/// The log records of one request, as rows.
pub fn logs(request: ExportLogsServiceRequest) -> Vec<Log> {
    let mut rows = Vec::new();
    for resource_logs in request.resource_logs {
        let resource = resource_logs
            .resource
            .map(|r| r.attributes)
            .unwrap_or_default();
        let service = string(&resource, "service.name").unwrap_or_default();
        for record in resource_logs
            .scope_logs
            .into_iter()
            .flat_map(|s| s.log_records)
        {
            let mut attributes = record.attributes;
            let cell = take(&mut attributes, "cloudflare.durable_object.id");
            let errors = take_any(&mut attributes, "cloudflare.log.error_info");
            let body = body(record.body.and_then(|b| b.value), errors);
            rows.push(Log {
                service: service.clone(),
                trace_id: (!record.trace_id.is_empty()).then(|| hex(&record.trace_id)),
                span_id: (!record.span_id.is_empty()).then(|| hex(&record.span_id)),
                time_unix_us: us(record.time_unix_nano),
                severity_number: record.severity_number,
                severity_text: severity(record.severity_number).to_owned(),
                body,
                cell,
                attributes: attributes_json(&resource, &attributes),
            });
        }
    }
    rows
}

fn properties() -> Arc<WriterProperties> {
    Arc::new(
        WriterProperties::builder()
            .set_compression(Compression::ZSTD(ZstdLevel::default()))
            // A point lookup by trace id is the query a time-partitioned scan is bad at.
            .set_column_bloom_filter_enabled(ColumnPath::from("trace_id"), true)
            .set_key_value_metadata(Some(vec![parquet::file::metadata::KeyValue::new(
                "workerd-schema".into(),
                SCHEMA.to_owned(),
            )]))
            .build(),
    )
}

// Columns are written in schema order, each arm consuming the next one. An optional column
// carries definition levels (1 present, 0 null) and packs only the present values.
macro_rules! column {
    ($group:ident, $type:ty, req $values:expr) => {{
        let mut column = $group.next_column()?.unwrap();
        column.typed::<$type>().write_batch(&$values, None, None)?;
        column.close()?;
    }};
    ($group:ident, $type:ty, opt $values:expr) => {{
        let mut column = $group.next_column()?.unwrap();
        let options: Vec<Option<_>> = $values;
        let levels: Vec<i16> = options.iter().map(|v| i16::from(v.is_some())).collect();
        let values: Vec<_> = options.into_iter().flatten().collect();
        column
            .typed::<$type>()
            .write_batch(&values, Some(&levels), None)?;
        column.close()?;
    }};
}

fn text(s: &str) -> ByteArray {
    ByteArray::from(s.as_bytes().to_vec())
}

/// A Parquet file of `rows`, written by `node`.
pub fn encode_spans(rows: &[Span], node: &str) -> Result<Vec<u8>> {
    let schema = Arc::new(parse_message_type(SPANS)?);
    let mut writer = SerializedFileWriter::new(Vec::new(), schema, properties())?;
    let mut g = writer.next_row_group()?;
    let r = rows.iter();
    column!(g, ByteArrayType, req r.clone().map(|_| text(node)).collect::<Vec<_>>());
    column!(g, ByteArrayType, req r.clone().map(|s| text(&s.service)).collect::<Vec<_>>());
    column!(g, ByteArrayType, req r.clone().map(|s| text(&s.trace_id)).collect::<Vec<_>>());
    column!(g, ByteArrayType, req r.clone().map(|s| text(&s.span_id)).collect::<Vec<_>>());
    column!(g, ByteArrayType, opt r.clone().map(|s| s.parent_span_id.as_deref().map(text)).collect());
    column!(g, ByteArrayType, req r.clone().map(|s| text(&s.name)).collect::<Vec<_>>());
    column!(g, Int32Type, req r.clone().map(|s| s.kind).collect::<Vec<_>>());
    column!(g, Int64Type, req r.clone().map(|s| s.start_unix_us).collect::<Vec<_>>());
    column!(g, Int64Type, req r.clone().map(|s| s.duration_us).collect::<Vec<_>>());
    column!(g, BoolType, req r.clone().map(|s| s.ok).collect::<Vec<_>>());
    column!(g, ByteArrayType, opt r.clone().map(|s| s.error.as_deref().map(text)).collect());
    column!(g, ByteArrayType, opt r.clone().map(|s| s.cell.as_deref().map(text)).collect());
    column!(g, ByteArrayType, opt r.clone().map(|s| s.url.as_deref().map(text)).collect());
    column!(g, Int32Type, opt r.clone().map(|s| s.http_status).collect());
    column!(g, ByteArrayType, req r.clone().map(|s| text(&s.attributes)).collect::<Vec<_>>());
    g.close()?;
    writer.into_inner()
}

/// A Parquet file of `rows`, written by `node`.
pub fn encode_logs(rows: &[Log], node: &str) -> Result<Vec<u8>> {
    let schema = Arc::new(parse_message_type(LOGS)?);
    let mut writer = SerializedFileWriter::new(Vec::new(), schema, properties())?;
    let mut g = writer.next_row_group()?;
    let r = rows.iter();
    column!(g, ByteArrayType, req r.clone().map(|_| text(node)).collect::<Vec<_>>());
    column!(g, ByteArrayType, req r.clone().map(|l| text(&l.service)).collect::<Vec<_>>());
    column!(g, ByteArrayType, opt r.clone().map(|l| l.trace_id.as_deref().map(text)).collect());
    column!(g, ByteArrayType, opt r.clone().map(|l| l.span_id.as_deref().map(text)).collect());
    column!(g, Int64Type, req r.clone().map(|l| l.time_unix_us).collect::<Vec<_>>());
    column!(g, Int32Type, req r.clone().map(|l| l.severity_number).collect::<Vec<_>>());
    column!(g, ByteArrayType, req r.clone().map(|l| text(&l.severity_text)).collect::<Vec<_>>());
    column!(g, ByteArrayType, req r.clone().map(|l| text(&l.body)).collect::<Vec<_>>());
    column!(g, ByteArrayType, opt r.clone().map(|l| l.cell.as_deref().map(text)).collect());
    column!(g, ByteArrayType, req r.clone().map(|l| text(&l.attributes)).collect::<Vec<_>>());
    g.close()?;
    writer.into_inner()
}

fn us(nanos: u64) -> i64 {
    i64::try_from(nanos / 1000).unwrap_or(i64::MAX)
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut s, b| {
        let _ = write!(s, "{b:02x}");
        s
    })
}

fn string(attributes: &[KeyValue], key: &str) -> Option<String> {
    attributes
        .iter()
        .find(|kv| kv.key == key)
        .and_then(|kv| match &kv.value {
            Some(AnyValue {
                value: Some(Value::String(s)),
            }) => Some(s.clone()),
            _ => None,
        })
}

fn take(attributes: &mut Vec<KeyValue>, key: &str) -> Option<String> {
    let value = string(attributes, key)?;
    attributes.retain(|kv| kv.key != key);
    Some(value)
}

fn take_any(attributes: &mut Vec<KeyValue>, key: &str) -> Option<Value> {
    let i = attributes.iter().position(|kv| kv.key == key)?;
    attributes.remove(i).value?.value
}

/// A console line as a terminal shows it: the arguments joined by spaces, a string as itself, an
/// `Error` as its stack (else `name: message`), anything else as JSON. `errors` is
/// `cloudflare.log.error_info`, one entry per argument. A body that is not an array, as a
/// truncated message's is not, is one argument.
fn body(body: Option<Value>, errors: Option<Value>) -> String {
    let errors = match errors {
        Some(Value::Array(a)) => a.values,
        _ => Vec::new(),
    };
    let args: Vec<Option<Value>> = match body {
        Some(Value::Array(a)) => a.values.into_iter().map(|v| v.value).collect(),
        Some(v) => vec![Some(v)],
        None => Vec::new(),
    };
    let arg = |(i, arg): (usize, &Option<Value>)| {
        if let Some(Value::KvList(e)) = errors.get(i).and_then(|e| e.value.as_ref()) {
            let field = |key: &str| match &e.values.iter().find(|kv| kv.key == key)?.value {
                Some(AnyValue {
                    value: Some(Value::String(s)),
                }) => Some(s.as_str()),
                _ => None,
            };
            return field("stack").map_or_else(
                || {
                    format!(
                        "{}: {}",
                        field("name").unwrap_or(""),
                        field("message").unwrap_or("")
                    )
                },
                str::to_owned,
            );
        }
        match arg {
            Some(Value::String(s)) => s.clone(),
            Some(v) => any(v).to_string(),
            None => "null".to_owned(),
        }
    };
    args.iter()
        .enumerate()
        .map(arg)
        .collect::<Vec<_>>()
        .join(" ")
}

/// The severity text for an OTLP severity number: DEBUG, INFO, WARN or ERROR.
fn severity(number: i32) -> &'static str {
    match number {
        1..=8 => "DEBUG",
        13..=16 => "WARN",
        17.. => "ERROR",
        _ => "INFO",
    }
}

fn take_int(attributes: &mut Vec<KeyValue>, key: &str) -> Option<i32> {
    let i = attributes.iter().position(|kv| kv.key == key)?;
    let Some(AnyValue {
        value: Some(Value::Int(v)),
    }) = attributes[i].value
    else {
        return None;
    };
    attributes.remove(i);
    i32::try_from(v).ok()
}

/// The resource's attributes other than `service.name`, then the record's, which win.
fn attributes_json(resource: &[KeyValue], attributes: &[KeyValue]) -> String {
    let resource = resource.iter().filter(|kv| kv.key != "service.name");
    serde_json::Value::Object(json(resource.chain(attributes))).to_string()
}

fn json<'a>(attributes: impl Iterator<Item = &'a KeyValue>) -> Map<String, serde_json::Value> {
    attributes
        .filter_map(|kv| Some((kv.key.clone(), any(kv.value.as_ref()?.value.as_ref()?))))
        .collect()
}

fn any(value: &Value) -> serde_json::Value {
    match value {
        Value::String(s) => s.clone().into(),
        Value::Bool(b) => (*b).into(),
        Value::Int(i) => (*i).into(),
        Value::Double(d) => (*d).into(),
        Value::Array(a) => a
            .values
            .iter()
            .filter_map(|v| v.value.as_ref())
            .map(any)
            .collect(),
        Value::KvList(l) => json(l.values.iter()).into(),
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
