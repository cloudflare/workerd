// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The part of the OTLP trace and logs schemas the exporter writes.
//!
//! Field numbers are those of `opentelemetry/proto/{common,resource,trace,logs}/v1`,
//! `collector/trace/v1/trace_service.proto` and `collector/logs/v1/logs_service.proto`, which are
//! stable. Fields the exporter never sets are
//! left out; a collector reads them as their defaults.

#[derive(Clone, PartialEq, prost::Message)]
pub struct ExportTraceServiceRequest {
    #[prost(message, repeated, tag = "1")]
    pub resource_spans: Vec<ResourceSpans>,
}

#[derive(Clone, PartialEq, prost::Message)]
pub struct ResourceSpans {
    #[prost(message, optional, tag = "1")]
    pub resource: Option<Resource>,
    #[prost(message, repeated, tag = "2")]
    pub scope_spans: Vec<ScopeSpans>,
}

#[derive(Clone, PartialEq, prost::Message)]
pub struct Resource {
    #[prost(message, repeated, tag = "1")]
    pub attributes: Vec<KeyValue>,
}

#[derive(Clone, PartialEq, prost::Message)]
pub struct ScopeSpans {
    #[prost(message, optional, tag = "1")]
    pub scope: Option<InstrumentationScope>,
    #[prost(message, repeated, tag = "2")]
    pub spans: Vec<Span>,
}

#[derive(Clone, PartialEq, Eq, prost::Message)]
pub struct InstrumentationScope {
    #[prost(string, tag = "1")]
    pub name: String,
}

#[derive(Clone, PartialEq, prost::Message)]
#[expect(
    clippy::struct_field_names,
    reason = "the fields are named as in the OTLP message"
)]
pub struct Span {
    #[prost(bytes = "vec", tag = "1")]
    pub trace_id: Vec<u8>,
    #[prost(bytes = "vec", tag = "2")]
    pub span_id: Vec<u8>,
    #[prost(bytes = "vec", tag = "4")]
    pub parent_span_id: Vec<u8>,
    #[prost(string, tag = "5")]
    pub name: String,
    #[prost(enumeration = "SpanKind", tag = "6")]
    pub kind: i32,
    #[prost(fixed64, tag = "7")]
    pub start_time_unix_nano: u64,
    #[prost(fixed64, tag = "8")]
    pub end_time_unix_nano: u64,
    #[prost(message, repeated, tag = "9")]
    pub attributes: Vec<KeyValue>,
    #[prost(message, repeated, tag = "11")]
    pub events: Vec<Event>,
    #[prost(message, optional, tag = "15")]
    pub status: Option<Status>,
    #[prost(fixed32, tag = "16")]
    pub flags: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, prost::Enumeration)]
#[repr(i32)]
pub enum SpanKind {
    Unspecified = 0,
    Internal = 1,
    Server = 2,
    Client = 3,
    Producer = 4,
    Consumer = 5,
}

#[derive(Clone, PartialEq, prost::Message)]
pub struct Event {
    #[prost(fixed64, tag = "1")]
    pub time_unix_nano: u64,
    #[prost(string, tag = "2")]
    pub name: String,
    #[prost(message, repeated, tag = "3")]
    pub attributes: Vec<KeyValue>,
}

#[derive(Clone, PartialEq, Eq, prost::Message)]
pub struct Status {
    #[prost(string, tag = "2")]
    pub message: String,
    #[prost(enumeration = "StatusCode", tag = "3")]
    pub code: i32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, prost::Enumeration)]
#[repr(i32)]
pub enum StatusCode {
    Unset = 0,
    Ok = 1,
    Error = 2,
}

#[derive(Clone, PartialEq, prost::Message)]
pub struct KeyValue {
    #[prost(string, tag = "1")]
    pub key: String,
    #[prost(message, optional, tag = "2")]
    pub value: Option<AnyValue>,
}

#[derive(Clone, PartialEq, prost::Message)]
pub struct AnyValue {
    #[prost(oneof = "any_value::Value", tags = "1, 2, 3, 4, 5, 6")]
    pub value: Option<any_value::Value>,
}

pub mod any_value {
    #[derive(Clone, PartialEq, prost::Oneof)]
    pub enum Value {
        #[prost(string, tag = "1")]
        String(String),
        #[prost(bool, tag = "2")]
        Bool(bool),
        #[prost(int64, tag = "3")]
        Int(i64),
        #[prost(double, tag = "4")]
        Double(f64),
        #[prost(message, tag = "5")]
        Array(super::ArrayValue),
        #[prost(message, tag = "6")]
        KvList(super::KeyValueList),
    }
}

#[derive(Clone, PartialEq, prost::Message)]
pub struct ArrayValue {
    #[prost(message, repeated, tag = "1")]
    pub values: Vec<AnyValue>,
}

#[derive(Clone, PartialEq, prost::Message)]
pub struct KeyValueList {
    #[prost(message, repeated, tag = "1")]
    pub values: Vec<KeyValue>,
}

#[derive(Clone, PartialEq, prost::Message)]
pub struct ExportLogsServiceRequest {
    #[prost(message, repeated, tag = "1")]
    pub resource_logs: Vec<ResourceLogs>,
}

#[derive(Clone, PartialEq, prost::Message)]
pub struct ResourceLogs {
    #[prost(message, optional, tag = "1")]
    pub resource: Option<Resource>,
    #[prost(message, repeated, tag = "2")]
    pub scope_logs: Vec<ScopeLogs>,
}

#[derive(Clone, PartialEq, prost::Message)]
pub struct ScopeLogs {
    #[prost(message, optional, tag = "1")]
    pub scope: Option<InstrumentationScope>,
    #[prost(message, repeated, tag = "2")]
    pub log_records: Vec<LogRecord>,
}

#[derive(Clone, PartialEq, prost::Message)]
pub struct LogRecord {
    #[prost(fixed64, tag = "1")]
    pub time_unix_nano: u64,
    #[prost(enumeration = "SeverityNumber", tag = "2")]
    pub severity_number: i32,
    #[prost(string, tag = "3")]
    pub severity_text: String,
    #[prost(message, optional, tag = "5")]
    pub body: Option<AnyValue>,
    #[prost(message, repeated, tag = "6")]
    pub attributes: Vec<KeyValue>,
    #[prost(fixed32, tag = "8")]
    pub flags: u32,
    #[prost(bytes = "vec", tag = "9")]
    pub trace_id: Vec<u8>,
    #[prost(bytes = "vec", tag = "10")]
    pub span_id: Vec<u8>,
    #[prost(fixed64, tag = "11")]
    pub observed_time_unix_nano: u64,
    #[prost(string, tag = "12")]
    pub event_name: String,
}

/// The first value of each OTLP severity range; the exporter uses no finer levels.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, prost::Enumeration)]
#[repr(i32)]
pub enum SeverityNumber {
    Unspecified = 0,
    Debug = 5,
    Info = 9,
    Warn = 13,
    Error = 17,
}
