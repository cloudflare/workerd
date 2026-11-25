use prost::Message;
use serde_json::json;

use super::encode;
use crate::proto;
use crate::proto::AnyValue;
use crate::proto::KeyValue;
use crate::proto::any_value;

fn kv(key: &str, value: any_value::Value) -> KeyValue {
    KeyValue {
        key: key.to_owned(),
        value: Some(AnyValue { value: Some(value) }),
    }
}

fn json(logs: bool, request: &impl Message) -> serde_json::Value {
    serde_json::from_slice(&encode(logs, &request.encode_to_vec()).unwrap()).unwrap()
}

#[test]
fn spans_take_otlp_json_form() {
    let request = proto::ExportTraceServiceRequest {
        resource_spans: vec![proto::ResourceSpans {
            resource: Some(proto::Resource {
                attributes: vec![kv("service.name", any_value::Value::String("w".into()))],
            }),
            scope_spans: vec![proto::ScopeSpans {
                scope: Some(proto::InstrumentationScope {
                    name: "workerd".into(),
                }),
                spans: vec![proto::Span {
                    trace_id: (1..=16).collect(),
                    span_id: vec![0xab; 8],
                    name: "fetch".into(),
                    kind: 2,
                    start_time_unix_nano: 1,
                    end_time_unix_nano: u64::MAX,
                    attributes: vec![kv("n", any_value::Value::Int(-3))],
                    status: Some(proto::Status {
                        message: String::new(),
                        code: 2,
                    }),
                    ..Default::default()
                }],
            }],
        }],
    };
    assert_eq!(
        json(false, &request),
        json!({ "resourceSpans": [{
            "resource": { "attributes": [{ "key": "service.name", "value": { "stringValue": "w" } }] },
            "scopeSpans": [{
                "scope": { "name": "workerd" },
                "spans": [{
                    "traceId": "0102030405060708090a0b0c0d0e0f10",
                    "spanId": "abababababababab",
                    "name": "fetch",
                    "kind": 2,
                    "startTimeUnixNano": "1",
                    "endTimeUnixNano": "18446744073709551615",
                    "attributes": [{ "key": "n", "value": { "intValue": "-3" } }],
                    "status": { "code": 2 },
                }],
            }],
        }] })
    );
}

#[test]
fn logs_take_otlp_json_form() {
    let request = proto::ExportLogsServiceRequest {
        resource_logs: vec![proto::ResourceLogs {
            resource: None,
            scope_logs: vec![proto::ScopeLogs {
                scope: None,
                log_records: vec![proto::LogRecord {
                    time_unix_nano: 5,
                    severity_number: 9,
                    severity_text: "log".into(),
                    body: Some(AnyValue {
                        value: Some(any_value::Value::Array(proto::ArrayValue {
                            values: vec![AnyValue {
                                value: Some(any_value::Value::Bool(true)),
                            }],
                        })),
                    }),
                    span_id: vec![1; 8],
                    ..Default::default()
                }],
            }],
        }],
    };
    assert_eq!(
        json(true, &request),
        json!({ "resourceLogs": [{ "scopeLogs": [{ "logRecords": [{
            "timeUnixNano": "5",
            "observedTimeUnixNano": "0",
            "severityNumber": 9,
            "severityText": "log",
            "body": { "arrayValue": { "values": [{ "boolValue": true }] } },
            "spanId": "0101010101010101",
        }] }] }] })
    );
}

#[test]
fn concatenated_requests_decode_as_one() {
    let one = proto::ExportTraceServiceRequest {
        resource_spans: vec![proto::ResourceSpans::default()],
    }
    .encode_to_vec();
    let both = [one.as_slice(), one.as_slice()].concat();
    let value: serde_json::Value = serde_json::from_slice(&encode(false, &both).unwrap()).unwrap();
    assert_eq!(value["resourceSpans"].as_array().unwrap().len(), 2);
}

#[test]
fn garbage_is_an_error() {
    assert!(encode(false, &[0xff, 0xff]).is_err());
}

#[test]
fn strings_are_escaped_and_non_finite_doubles_are_null() {
    let request = proto::ExportTraceServiceRequest {
        resource_spans: vec![proto::ResourceSpans {
            resource: Some(proto::Resource {
                attributes: vec![
                    kv("q\"\n", any_value::Value::String("\u{1}é".into())),
                    kv("d", any_value::Value::Double(f64::NAN)),
                ],
            }),
            scope_spans: vec![],
        }],
    };
    assert_eq!(
        json(false, &request)["resourceSpans"][0]["resource"]["attributes"],
        json!([
            { "key": "q\"\n", "value": { "stringValue": "\u{1}é" } },
            { "key": "d", "value": { "doubleValue": null } },
        ])
    );
}
