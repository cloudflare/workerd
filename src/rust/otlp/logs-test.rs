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
            .add_log(SPAN, T0, Level::Log, r#"["hello", 1]"#, false, Vec::new())
            .is_none()
    );
    assert!(
        buffer
            .add_log(
                0x2000,
                T0 + 1,
                Level::Warn,
                r#"["careful"]"#,
                false,
                Vec::new()
            )
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
    let late = buffer.add_log(SPAN, T0, Level::Debug, r#"["late"]"#, false, Vec::new());
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
            .add_log(SPAN, T0, Level::Log, message, false, Vec::new())
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
        let records = decode(
            &buffer
                .add_log(SPAN, T0, Level::Log, message, true, Vec::new())
                .unwrap(),
        );
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
fn error_arguments_keep_their_fields() {
    let mut buffer = buffer();
    buffer.flush();
    let error_info = vec![
        None,
        Some(ErrorInfo {
            name: "TypeError".to_owned(),
            message: "boom".to_owned(),
            stack: Some("at x".to_owned()),
        }),
        Some(ErrorInfo {
            name: "Error".to_owned(),
            message: "no stack".to_owned(),
            stack: None,
        }),
    ];
    let batch = buffer.add_log(
        SPAN,
        T0,
        Level::Error,
        r#"["failed", {}, {}]"#,
        false,
        error_info,
    );
    let records = decode(&batch.unwrap());
    let error = |fields: Vec<proto::KeyValue>| {
        any_value(Some(Value::KvList(proto::KeyValueList { values: fields })))
    };
    assert_eq!(
        records[0].attributes,
        vec![
            key_value(
                "cloudflare.log.error_info",
                Value::Array(proto::ArrayValue {
                    values: vec![
                        any_value(None),
                        error(vec![
                            key_value("name", "TypeError"),
                            key_value("message", "boom"),
                            key_value("stack", "at x"),
                        ]),
                        error(vec![
                            key_value("name", "Error"),
                            key_value("message", "no stack"),
                        ]),
                    ],
                }),
            ),
            key_value("cloudflare.entrypoint", "MyEntrypoint"),
        ]
    );
}

#[test]
fn an_invocation_is_a_record_of_its_root_span() {
    let mut buffer = buffer();
    let root = proto::Span {
        trace_id: (1..=16).collect(),
        span_id: SPAN.to_be_bytes().to_vec(),
        name: "GET".to_owned(),
        end_time_unix_nano: 7,
        flags: 0,
        attributes: vec![
            key_value("cloudflare.outcome", "ok"),
            key_value("cloudflare.entrypoint", "MyEntrypoint"),
        ],
        ..Default::default()
    };
    buffer.add_invocation(root);
    let records = decode(&buffer.flush().unwrap());
    let invocation = &records[0];
    assert_eq!(invocation.event_name, "cloudflare.invocation");
    assert_eq!(
        invocation.severity_number(),
        proto::SeverityNumber::Unspecified
    );
    assert_eq!(invocation.time_unix_nano, 7);
    assert_eq!(invocation.observed_time_unix_nano, 7);
    assert_eq!(invocation.span_id, SPAN.to_be_bytes().to_vec());
    assert_eq!(invocation.flags, 0);
    assert_eq!(*body(invocation), Value::String("GET".to_owned()));
    // The span's own attributes, which already carry the default ones.
    assert_eq!(
        invocation.attributes,
        vec![
            key_value("cloudflare.outcome", "ok"),
            key_value("cloudflare.entrypoint", "MyEntrypoint"),
        ]
    );
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
                .add_log(SPAN, T0, Level::Log, r#"["x"]"#, false, Vec::new())
                .is_none()
        );
    }
    let batch = buffer.add_log(SPAN, T0, Level::Log, r#"["x"]"#, false, Vec::new());
    assert_eq!(batch.unwrap().record_count, MAX_BATCH_RECORDS);

    let big = format!(r#"["{}"]"#, "x".repeat(MAX_BATCH_BYTES / 2));
    assert!(
        buffer
            .add_log(SPAN, T0, Level::Log, &big, false, Vec::new())
            .is_none()
    );
    let batch = buffer.add_log(SPAN, T0, Level::Log, &big, false, Vec::new());
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
    let batch = buffer
        .add_log(0, T0, Level::Log, "[]", false, Vec::new())
        .unwrap();
    let request = proto::ExportLogsServiceRequest::decode(batch.request.as_slice()).unwrap();
    let record = &request.resource_logs[0].scope_logs[0].log_records[0];
    assert_eq!(record.time_unix_nano, 0);
    assert_eq!(record.observed_time_unix_nano, 0);
    assert!(record.span_id.is_empty());
}
