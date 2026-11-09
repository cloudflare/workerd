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
    pub fn add_log(
        &mut self,
        span_id: u64,
        time: i64,
        level: Level,
        message: &str,
        truncated: bool,
    ) -> Option<Batch> {
        let body = parse_json(message).unwrap_or_else(|| Value::String(message.to_owned()));
        let mut attributes = Vec::new();
        if truncated {
            attributes.push(key_value("cloudflare.log.truncated", true));
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
mod tests {
    use super::*;

    const SPAN: u64 = 0x1000;
    const T0: i64 = 1_000_000_000;

    fn buffer() -> LogBuffer {
        let mut resource = Attributes::default();
        resource.push("service.name", "my-worker");
        let mut defaults = Attributes::default();
        defaults.push("cloudflare.entrypoint", "MyEntrypoint");
        LogBuffer::new(
            Options {
                trace_id: (1..=16).collect(),
                trace_flags: 1,
                omit_timestamps: false,
            },
            resource,
            defaults,
        )
    }

    fn decode(batch: &Batch) -> Vec<proto::LogRecord> {
        let request = proto::ExportLogsServiceRequest::decode(batch.request.as_slice()).unwrap();
        assert_eq!(request.resource_logs.len(), 1);
        let resource_logs = &request.resource_logs[0];
        assert_eq!(
            resource_logs.resource.as_ref().unwrap().attributes,
            vec![key_value("service.name", "my-worker")]
        );
        assert_eq!(resource_logs.scope_logs.len(), 1);
        let scope_logs = &resource_logs.scope_logs[0];
        assert_eq!(scope_logs.scope.as_ref().unwrap().name, "workers-runtime");
        assert_eq!(scope_logs.log_records.len(), batch.record_count);
        scope_logs.log_records.clone()
    }

    fn body(record: &proto::LogRecord) -> &Value {
        record.body.as_ref().unwrap().value.as_ref().unwrap()
    }

    fn string(value: &str) -> proto::AnyValue {
        any_value(Some(Value::String(value.to_owned())))
    }

    #[test]
    fn logs_are_batched_until_flushed() {
        let mut buffer = buffer();
        assert!(
            buffer
                .add_log(SPAN, T0, Level::Log, r#"["hello", 1]"#, false)
                .is_none()
        );
        assert!(
            buffer
                .add_log(0x2000, T0 + 1, Level::Warn, r#"["careful"]"#, false)
                .is_none()
        );
        let records = decode(&buffer.flush().unwrap());
        assert_eq!(records.len(), 2);

        let log = &records[0];
        assert_eq!(log.time_unix_nano, T0.cast_unsigned());
        assert_eq!(log.observed_time_unix_nano, T0.cast_unsigned());
        assert_eq!(log.severity_number(), proto::SeverityNumber::Info);
        assert_eq!(log.severity_text, "log");
        assert_eq!(log.trace_id, (1..=16).collect::<Vec<u8>>());
        assert_eq!(log.span_id, SPAN.to_be_bytes().to_vec());
        assert_eq!(log.flags, 1);
        assert_eq!(log.event_name, "");
        assert_eq!(
            log.attributes,
            vec![key_value("cloudflare.entrypoint", "MyEntrypoint")]
        );
        assert_eq!(
            *body(log),
            Value::Array(proto::ArrayValue {
                values: vec![string("hello"), any_value(Some(Value::Int(1)))],
            })
        );

        let warn = &records[1];
        assert_eq!(warn.severity_number(), proto::SeverityNumber::Warn);
        assert_eq!(warn.severity_text, "warn");
        assert_eq!(warn.span_id, 0x2000_u64.to_be_bytes().to_vec());

        // Once flushed, each record goes out on its own.
        let late = buffer.add_log(SPAN, T0, Level::Debug, r#"["late"]"#, false);
        let records = decode(&late.unwrap());
        assert_eq!(records[0].severity_number(), proto::SeverityNumber::Debug);
        assert!(buffer.flush().is_none());
    }

    #[test]
    fn levels_map_to_severities() {
        let cases = [
            (Level::Debug, proto::SeverityNumber::Debug, "debug"),
            (Level::Info, proto::SeverityNumber::Info, "info"),
            (Level::Log, proto::SeverityNumber::Info, "log"),
            (Level::Warn, proto::SeverityNumber::Warn, "warn"),
            (Level::Error, proto::SeverityNumber::Error, "error"),
        ];
        for (level, number, text) in cases {
            assert_eq!(level.severity(), (number, text));
        }
    }

    #[test]
    fn json_bodies_keep_their_shape() {
        let mut buffer = buffer();
        buffer.flush();
        let message = r#"[{"z": 1, "a": [true, null, 1.5], "big": 18446744073709551615}, null]"#;
        let records = decode(
            &buffer
                .add_log(SPAN, T0, Level::Log, message, false)
                .unwrap(),
        );
        let object = proto::KeyValueList {
            values: vec![
                proto::KeyValue {
                    key: "z".to_owned(),
                    value: Some(any_value(Some(Value::Int(1)))),
                },
                proto::KeyValue {
                    key: "a".to_owned(),
                    value: Some(any_value(Some(Value::Array(proto::ArrayValue {
                        values: vec![
                            any_value(Some(Value::Bool(true))),
                            any_value(None),
                            any_value(Some(Value::Double(1.5))),
                        ],
                    })))),
                },
                proto::KeyValue {
                    key: "big".to_owned(),
                    value: Some(any_value(Some(Value::Double(18_446_744_073_709_551_615.0)))),
                },
            ],
        };
        assert_eq!(
            *body(&records[0]),
            Value::Array(proto::ArrayValue {
                values: vec![any_value(Some(Value::KvList(object))), any_value(None)],
            })
        );
    }

    #[test]
    fn invalid_json_is_kept_as_a_string() {
        let mut buffer = buffer();
        buffer.flush();
        for message in [
            r#"["cut sho"#,
            "null",
            &format!("{}{}", "[".repeat(200), "]".repeat(200)),
        ] {
            let records = decode(&buffer.add_log(SPAN, T0, Level::Log, message, true).unwrap());
            assert_eq!(*body(&records[0]), Value::String(message.to_owned()));
            assert_eq!(
                records[0].attributes,
                vec![
                    key_value("cloudflare.log.truncated", true),
                    key_value("cloudflare.entrypoint", "MyEntrypoint"),
                ]
            );
        }
    }

    #[test]
    fn exceptions_are_exception_events() {
        let mut buffer = buffer();
        buffer.add_exception(
            SPAN,
            T0,
            "TypeError".to_owned(),
            "boom".to_owned(),
            Some("at x".to_owned()),
        );
        let records = decode(&buffer.flush().unwrap());
        let exception = &records[0];
        assert_eq!(exception.event_name, "exception");
        assert_eq!(exception.severity_number(), proto::SeverityNumber::Error);
        assert_eq!(exception.severity_text, "error");
        assert_eq!(*body(exception), Value::String("boom".to_owned()));
        assert_eq!(
            exception.attributes,
            vec![
                key_value("exception.type", "TypeError"),
                key_value("exception.message", "boom"),
                key_value("exception.stacktrace", "at x"),
                key_value("cloudflare.entrypoint", "MyEntrypoint"),
            ]
        );
    }

    #[test]
    fn a_full_batch_is_handed_out() {
        let mut buffer = buffer();
        for _ in 1..MAX_BATCH_RECORDS {
            assert!(
                buffer
                    .add_log(SPAN, T0, Level::Log, r#"["x"]"#, false)
                    .is_none()
            );
        }
        let batch = buffer.add_log(SPAN, T0, Level::Log, r#"["x"]"#, false);
        assert_eq!(batch.unwrap().record_count, MAX_BATCH_RECORDS);

        let big = format!(r#"["{}"]"#, "x".repeat(MAX_BATCH_BYTES / 2));
        assert!(buffer.add_log(SPAN, T0, Level::Log, &big, false).is_none());
        let batch = buffer.add_log(SPAN, T0, Level::Log, &big, false);
        assert_eq!(batch.unwrap().record_count, 2);
        assert!(buffer.flush().is_none());
    }

    #[test]
    fn timestamps_can_be_omitted() {
        let mut buffer = LogBuffer::new(
            Options {
                trace_id: vec![0; 16],
                trace_flags: 0,
                omit_timestamps: true,
            },
            Attributes::default(),
            Attributes::default(),
        );
        buffer.flush();
        let batch = buffer.add_log(0, T0, Level::Log, "[]", false).unwrap();
        let request = proto::ExportLogsServiceRequest::decode(batch.request.as_slice()).unwrap();
        let record = &request.resource_logs[0].scope_logs[0].log_records[0];
        assert_eq!(record.time_unix_nano, 0);
        assert_eq!(record.observed_time_unix_nano, 0);
        assert!(record.span_id.is_empty());
    }
}
