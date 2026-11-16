// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::fmt;

use prost::Message;
use serde::Deserialize;
use serde::Deserializer;
use serde::de::MapAccess;
use serde::de::SeqAccess;
use serde::de::Visitor;

use crate::attributes::Attributes;
use crate::attributes::key_value;
use crate::proto;
use crate::proto::any_value::Value;

/// A batch is handed out once it reaches either bound, the same as a batch of spans.
const MAX_BATCH_RECORDS: usize = 512;
const MAX_BATCH_BYTES: usize = 1 << 20;

/// The console method a log came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Level {
    Debug,
    Info,
    Log,
    Warn,
    Error,
}

impl Level {
    /// `log` and `info` share a severity; the text tells them apart, as the streaming tail's
    /// `level` does.
    fn severity(self) -> (proto::SeverityNumber, &'static str) {
        match self {
            Self::Debug => (proto::SeverityNumber::Debug, "debug"),
            Self::Info => (proto::SeverityNumber::Info, "info"),
            Self::Log => (proto::SeverityNumber::Info, "log"),
            Self::Warn => (proto::SeverityNumber::Warn, "warn"),
            Self::Error => (proto::SeverityNumber::Error, "error"),
        }
    }
}

/// What is fixed for one invocation's logs.
pub struct Options {
    /// The 16-byte trace id every record shares with the invocation's spans.
    pub trace_id: Vec<u8>,
    /// The W3C trace flags, zero when the trace carries none.
    pub trace_flags: u32,
    /// Leaves every timestamp at zero, for tests that compare whole records.
    pub omit_timestamps: bool,
}

/// The structured fields of a `console` argument that was a native `Error`, which its place in
/// the JSON message does not keep.
pub struct ErrorInfo {
    pub name: String,
    pub message: String,
    pub stack: Option<String>,
}

/// An encoded `ExportLogsServiceRequest` and the number of records in it.
pub struct Batch {
    pub request: Vec<u8>,
    pub record_count: usize,
}

/// One invocation's logs and uncaught exceptions on their way to an OTLP collector, as the
/// `LogRecord`s that the streaming tail's `log` and `exception` events would be.
///
/// Each record carries the trace id and the id of the span that was active when it was made, so a
/// collector can join it to the invocation's spans, and the same resource and default attributes.
/// Records collect into a batch that is handed out when it grows large or when [`Self::flush`]
/// is called, which the invocation's span closing does. From then on every record is handed out
/// on its own: it belongs to work that outlives the invocation, and nothing bounds how long the
/// next one would take to arrive.
pub struct LogBuffer {
    options: Options,
    resource: proto::Resource,
    default_attributes: Vec<proto::KeyValue>,
    flushed: bool,
    records: Vec<proto::LogRecord>,
    /// Encoded size of `records`, kept so an add does not re-measure the batch.
    records_bytes: usize,
}

impl LogBuffer {
    pub fn new(options: Options, resource: Attributes, default_attributes: Attributes) -> Self {
        Self {
            options,
            resource: proto::Resource {
                attributes: resource.into_proto(),
            },
            default_attributes: default_attributes.into_proto(),
            flushed: false,
            records: Vec::new(),
            records_bytes: 0,
        }
    }

    /// Adds a `console` log. `message` is the JSON array of the call's arguments, which becomes
    /// the body as an array, keeping the order of object keys. A message that is not valid JSON,
    /// as a truncated one is not, becomes a string body instead. `truncated` adds
    /// `cloudflare.log.truncated`.
    ///
    /// `error_info` has one entry per argument, `Some` for each argument that was an `Error`, or
    /// none at all when no argument was. It becomes `cloudflare.log.error_info`, an array in the
    /// arguments' order, the same shape as the streaming tail's `errorInfo`: `name`, `message`
    /// and `stack` for an `Error`, an empty value for any other argument.
    pub fn add_log(
        &mut self,
        span_id: u64,
        time: i64,
        level: Level,
        message: &str,
        truncated: bool,
        error_info: Vec<Option<ErrorInfo>>,
    ) -> Option<Batch> {
        let body = parse_json(message).unwrap_or_else(|| Value::String(message.to_owned()));
        let mut attributes = Vec::new();
        if truncated {
            attributes.push(key_value("cloudflare.log.truncated", true));
        }
        if !error_info.is_empty() {
            attributes.push(key_value(
                "cloudflare.log.error_info",
                error_info_value(error_info),
            ));
        }
        self.add(span_id, time, level, body, attributes, String::new())
    }

    /// Adds an uncaught exception as an `exception` event with the `exception.*` attributes and
    /// the message as its body.
    pub fn add_exception(
        &mut self,
        span_id: u64,
        time: i64,
        name: String,
        message: String,
        stack: Option<String>,
    ) -> Option<Batch> {
        let mut attributes = vec![
            key_value("exception.type", name),
            key_value("exception.message", message.clone()),
        ];
        if let Some(stack) = stack {
            attributes.push(key_value("exception.stacktrace", stack));
        }
        self.add(
            span_id,
            time,
            Level::Error,
            Value::String(message),
            attributes,
            "exception".to_owned(),
        )
    }

    /// Adds the record of the invocation as a whole, made from its root span once that closed: a
    /// `cloudflare.invocation` event at the time the invocation ended, with the span's name as its
    /// body and the span's attributes, which describe the trigger, the response and the outcome
    /// with its CPU and wall time. It stands for the invocation log the streaming tail assembles
    /// from its `onset`, `return` and `outcome` events, so it has no severity.
    pub fn add_invocation(&mut self, root: proto::Span) -> Option<Batch> {
        let record = proto::LogRecord {
            time_unix_nano: root.end_time_unix_nano,
            severity_number: proto::SeverityNumber::Unspecified.into(),
            severity_text: String::new(),
            body: Some(proto::AnyValue {
                value: Some(Value::String(root.name)),
            }),
            attributes: root.attributes,
            flags: root.flags,
            trace_id: root.trace_id,
            span_id: root.span_id,
            observed_time_unix_nano: root.end_time_unix_nano,
            event_name: "cloudflare.invocation".to_owned(),
        };
        self.push(record)
    }

    /// Hands out every buffered record, and from then on each record as it is added.
    pub fn flush(&mut self) -> Option<Batch> {
        self.flushed = true;
        self.take_batch()
    }

    fn add(
        &mut self,
        span_id: u64,
        time: i64,
        level: Level,
        body: Value,
        mut attributes: Vec<proto::KeyValue>,
        event_name: String,
    ) -> Option<Batch> {
        for default in &self.default_attributes {
            if !attributes.iter().any(|kv| kv.key == default.key) {
                attributes.push(default.clone());
            }
        }
        let (severity, severity_text) = level.severity();
        let time = self.timestamp(time);
        let span_id = match span_id {
            0 => Vec::new(),
            id => id.to_be_bytes().to_vec(),
        };
        let record = proto::LogRecord {
            time_unix_nano: time,
            severity_number: severity.into(),
            severity_text: severity_text.to_owned(),
            body: Some(proto::AnyValue { value: Some(body) }),
            attributes,
            flags: self.options.trace_flags,
            trace_id: self.options.trace_id.clone(),
            span_id,
            observed_time_unix_nano: time,
            event_name,
        };
        self.push(record)
    }

    fn push(&mut self, record: proto::LogRecord) -> Option<Batch> {
        self.records_bytes += record.encoded_len();
        self.records.push(record);
        let due = self.flushed
            || self.records.len() >= MAX_BATCH_RECORDS
            || self.records_bytes >= MAX_BATCH_BYTES;
        if due { self.take_batch() } else { None }
    }

    fn take_batch(&mut self) -> Option<Batch> {
        if self.records.is_empty() {
            return None;
        }
        let log_records = std::mem::take(&mut self.records);
        self.records_bytes = 0;
        let record_count = log_records.len();
        let request = proto::ExportLogsServiceRequest {
            resource_logs: vec![proto::ResourceLogs {
                resource: Some(self.resource.clone()),
                scope_logs: vec![proto::ScopeLogs {
                    scope: Some(proto::InstrumentationScope {
                        name: "workers-runtime".to_owned(),
                    }),
                    log_records,
                }],
            }],
        };
        Some(Batch {
            request: request.encode_to_vec(),
            record_count,
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

fn error_info_value(error_info: Vec<Option<ErrorInfo>>) -> Value {
    let values = error_info
        .into_iter()
        .map(|info| {
            any_value(info.map(|info| {
                let mut fields = vec![
                    key_value("name", info.name),
                    key_value("message", info.message),
                ];
                if let Some(stack) = info.stack {
                    fields.push(key_value("stack", stack));
                }
                Value::KvList(proto::KeyValueList { values: fields })
            }))
        })
        .collect();
    Value::Array(proto::ArrayValue { values })
}

/// `text` as an OTLP value, or `None` when it is not JSON. serde_json's default recursion limit
/// bounds how deeply nested it may be.
fn parse_json(text: &str) -> Option<Value> {
    serde_json::from_str::<JsonValue>(text).ok().map(|v| v.0)
}

/// A JSON value read straight into its OTLP form, so object keys keep their order. `null` is a
/// value with nothing set.
struct JsonValue(Value);

/// The OTLP form of a nested JSON value, which unlike the top level may be `null`.
fn any_value(value: Option<Value>) -> proto::AnyValue {
    proto::AnyValue { value }
}

impl<'de> Deserialize<'de> for JsonValue {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        deserializer
            .deserialize_any(JsonVisitor)?
            .map(JsonValue)
            .ok_or_else(|| serde::de::Error::custom("null"))
    }
}

/// Builds the OTLP form of one JSON value: `None` for `null`.
struct JsonVisitor;

/// A nested JSON value, `null` included.
struct NestedJson(Option<Value>);

impl<'de> Deserialize<'de> for NestedJson {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        deserializer.deserialize_any(JsonVisitor).map(NestedJson)
    }
}

impl<'de> Visitor<'de> for JsonVisitor {
    type Value = Option<Value>;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("a JSON value")
    }

    fn visit_bool<E>(self, v: bool) -> Result<Self::Value, E> {
        Ok(Some(Value::Bool(v)))
    }

    fn visit_i64<E>(self, v: i64) -> Result<Self::Value, E> {
        Ok(Some(Value::Int(v)))
    }

    fn visit_u64<E>(self, v: u64) -> Result<Self::Value, E> {
        // Above i64::MAX only a double holds it, as it does in JavaScript.
        #[expect(clippy::cast_precision_loss, reason = "JavaScript numbers are doubles")]
        let value = i64::try_from(v).map_or(Value::Double(v as f64), Value::Int);
        Ok(Some(value))
    }

    fn visit_f64<E>(self, v: f64) -> Result<Self::Value, E> {
        Ok(Some(Value::Double(v)))
    }

    fn visit_str<E>(self, v: &str) -> Result<Self::Value, E> {
        Ok(Some(Value::String(v.to_owned())))
    }

    fn visit_string<E>(self, v: String) -> Result<Self::Value, E> {
        Ok(Some(Value::String(v)))
    }

    fn visit_unit<E>(self) -> Result<Self::Value, E> {
        Ok(None)
    }

    fn visit_seq<A: SeqAccess<'de>>(self, mut seq: A) -> Result<Self::Value, A::Error> {
        let mut values = Vec::new();
        while let Some(NestedJson(value)) = seq.next_element()? {
            values.push(any_value(value));
        }
        Ok(Some(Value::Array(proto::ArrayValue { values })))
    }

    fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Self::Value, A::Error> {
        let mut values = Vec::new();
        while let Some((key, NestedJson(value))) = map.next_entry::<String, NestedJson>()? {
            values.push(proto::KeyValue {
                key,
                value: Some(any_value(value)),
            });
        }
        Ok(Some(Value::KvList(proto::KeyValueList { values })))
    }
}

#[cfg(test)]
#[path = "logs-test.rs"]
mod tests;
