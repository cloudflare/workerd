// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use otlp::proto::AnyValue;
use otlp::proto::ExportLogsServiceRequest;
use otlp::proto::ExportTraceServiceRequest;
use otlp::proto::KeyValue;
use otlp::proto::LogRecord;
use otlp::proto::Resource;
use otlp::proto::ResourceLogs;
use otlp::proto::ResourceSpans;
use otlp::proto::ScopeLogs;
use otlp::proto::ScopeSpans;
use otlp::proto::Span;
use otlp::proto::Status;
use otlp::proto::StatusCode;
use otlp::proto::any_value::Value;
use parquet::file::reader::FileReader;
use parquet::file::reader::SerializedFileReader;
use parquet::record::Field;
use parquet::record::Row;

use super::encode_logs;
use super::encode_spans;
use super::logs;
use super::spans;

fn kv(key: &str, value: Value) -> KeyValue {
    KeyValue {
        key: key.into(),
        value: Some(AnyValue { value: Some(value) }),
    }
}

fn resource() -> Resource {
    Resource {
        attributes: vec![
            kv("service.name", Value::String("hello".into())),
            kv("cloudflare.script_name", Value::String("hello".into())),
        ],
    }
}

fn read(file: Vec<u8>) -> Vec<Row> {
    let reader = SerializedFileReader::new(bytes::Bytes::from(file)).unwrap();
    let meta = reader
        .metadata()
        .file_metadata()
        .key_value_metadata()
        .unwrap();
    assert_eq!(meta[0].value.as_deref(), Some(super::SCHEMA));
    reader
        .get_row_iter(None)
        .unwrap()
        .map(Result::unwrap)
        .collect()
}

fn get<'a>(row: &'a Row, name: &str) -> &'a Field {
    row.get_column_iter().find(|(n, _)| *n == name).unwrap().1
}

#[test]
fn spans_round_trip() {
    let span = |id: u8, parent: u8, status| Span {
        trace_id: vec![0xab; 16],
        span_id: vec![id; 8],
        parent_span_id: if parent == 0 { vec![] } else { vec![parent; 8] },
        name: format!("span{id}"),
        kind: 2,
        start_time_unix_nano: 5_000_000,
        end_time_unix_nano: 7_500_000,
        attributes: vec![
            kv("url.full", Value::String("https://x/".into())),
            kv("http.response.status_code", Value::Int(503)),
            kv("cloudflare.durable_object.id", Value::String("abc".into())),
            kv("cloudflare.outcome", Value::String("ok".into())),
        ],
        events: vec![],
        status: Some(status),
        flags: 0,
    };
    let request = ExportTraceServiceRequest {
        resource_spans: vec![ResourceSpans {
            resource: Some(resource()),
            scope_spans: vec![ScopeSpans {
                scope: None,
                spans: vec![
                    span(1, 0, Status::default()),
                    span(
                        2,
                        1,
                        Status {
                            code: StatusCode::Error as i32,
                            message: "boom".into(),
                        },
                    ),
                ],
            }],
        }],
    };
    let rows = read(encode_spans(&spans(request), "n1").unwrap());
    assert_eq!(rows.len(), 2);
    let (a, b) = (&rows[0], &rows[1]);
    assert_eq!(get(a, "node"), &Field::Str("n1".into()));
    assert_eq!(get(a, "service"), &Field::Str("hello".into()));
    assert_eq!(get(a, "trace_id"), &Field::Str("ab".repeat(16)));
    assert_eq!(get(a, "span_id"), &Field::Str("01".repeat(8)));
    assert_eq!(get(a, "parent_span_id"), &Field::Null);
    assert_eq!(get(b, "parent_span_id"), &Field::Str("01".repeat(8)));
    assert_eq!(get(a, "start_unix_us"), &Field::Long(5000));
    assert_eq!(get(a, "duration_us"), &Field::Long(2500));
    assert_eq!(get(a, "ok"), &Field::Bool(true));
    assert_eq!(get(a, "error"), &Field::Null);
    assert_eq!(get(b, "ok"), &Field::Bool(false));
    assert_eq!(get(b, "error"), &Field::Str("boom".into()));
    assert_eq!(get(a, "cell"), &Field::Str("abc".into()));
    assert_eq!(get(a, "url"), &Field::Str("https://x/".into()));
    assert_eq!(get(a, "http_status"), &Field::UShort(503));
    let Field::Str(attributes) = get(a, "attributes") else {
        panic!()
    };
    let attributes: serde_json::Value = serde_json::from_str(attributes).unwrap();
    assert_eq!(
        attributes,
        serde_json::json!({"cloudflare.script_name": "hello", "cloudflare.outcome": "ok"})
    );
}

#[test]
fn logs_round_trip() {
    let record = |body: Value, span: Vec<u8>| LogRecord {
        time_unix_nano: 9_000_000,
        severity_number: 17,
        severity_text: "ERROR".into(),
        body: Some(AnyValue { value: Some(body) }),
        trace_id: if span.is_empty() {
            vec![]
        } else {
            vec![0xab; 16]
        },
        span_id: span,
        ..Default::default()
    };
    let request = ExportLogsServiceRequest {
        resource_logs: vec![ResourceLogs {
            resource: Some(resource()),
            scope_logs: vec![ScopeLogs {
                scope: None,
                log_records: vec![
                    record(Value::String("hi".into()), vec![1; 8]),
                    record(Value::Int(3), vec![]),
                ],
            }],
        }],
    };
    let rows = read(encode_logs(&logs(request), "n1").unwrap());
    assert_eq!(rows.len(), 2);
    assert_eq!(get(&rows[0], "body"), &Field::Str("hi".into()));
    assert_eq!(get(&rows[1], "body"), &Field::Str("3".into()));
    assert_eq!(get(&rows[0], "span_id"), &Field::Str("01".repeat(8)));
    assert_eq!(get(&rows[1], "trace_id"), &Field::Null);
    assert_eq!(get(&rows[0], "time_unix_us"), &Field::Long(9000));
    assert_eq!(get(&rows[0], "severity_number"), &Field::Int(17));
    assert_eq!(get(&rows[0], "severity_text"), &Field::Str("ERROR".into()));
}

#[test]
fn log_lines_read_as_console_lines() {
    use otlp::proto::ArrayValue;
    use otlp::proto::KeyValueList;
    let any = |value| AnyValue { value: Some(value) };
    let array = |values| Value::Array(ArrayValue { values });
    let error = |fields: Vec<(&str, &str)>| {
        let values = fields
            .into_iter()
            .map(|(k, v)| kv(k, Value::String(v.into())))
            .collect();
        any(Value::KvList(KeyValueList { values }))
    };
    let object = Value::KvList(KeyValueList {
        values: vec![kv("x", Value::Int(1))],
    });
    let record = LogRecord {
        severity_number: 13,
        severity_text: "warn".into(),
        body: Some(any(array(vec![
            any(Value::String("a b".into())),
            any(object),
            any(Value::String("{}".into())),
            any(Value::String("{}".into())),
        ]))),
        attributes: vec![kv(
            "cloudflare.log.error_info",
            array(vec![
                AnyValue { value: None },
                AnyValue { value: None },
                error(vec![
                    ("name", "E"),
                    ("message", "m"),
                    ("stack", "E: m\n  at f"),
                ]),
                error(vec![("name", "TypeError"), ("message", "no")]),
            ]),
        )],
        ..Default::default()
    };
    let rows = logs(ExportLogsServiceRequest {
        resource_logs: vec![ResourceLogs {
            resource: None,
            scope_logs: vec![ScopeLogs {
                scope: None,
                log_records: vec![record],
            }],
        }],
    });
    assert_eq!(rows[0].body, "a b {\"x\":1} E: m\n  at f TypeError: no");
    assert_eq!(rows[0].severity_text, "WARN");
    assert_eq!(rows[0].attributes, "{}");
}
