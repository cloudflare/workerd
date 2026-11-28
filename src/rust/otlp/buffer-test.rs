use super::*;
use crate::proto::SpanKind;
use crate::proto::StatusCode;
use crate::proto::any_value::Value;

/// Above every id the tests hand out in a loop.
const ROOT: u64 = 0x1000;
const T0: i64 = 1_000_000_000;

fn buffer_with(redact_query_string: bool, default_attributes: Attributes) -> SpanBuffer {
    let mut resource = Attributes::default();
    resource.push("service.name", "my-worker");
    SpanBuffer::new(
        Options {
            trace_id: (1..=16).collect(),
            trace_flags: 1,
            root_span_id: ROOT,
            redact_query_string,
            omit_timestamps: false,
            export_spans: true,
            keep_root: false,
        },
        resource,
        default_attributes,
    )
}

fn buffer() -> SpanBuffer {
    buffer_with(false, Attributes::default())
}

fn attributes(pairs: &[(&str, Value)]) -> Attributes {
    let mut attributes = Attributes::default();
    for (key, value) in pairs {
        attributes.push(*key, value.clone());
    }
    attributes
}

fn string(value: &str) -> Value {
    Value::String(value.to_owned())
}

fn decode(batch: &Batch) -> proto::ExportTraceServiceRequest {
    let request = proto::ExportTraceServiceRequest::decode(batch.request.as_slice()).unwrap();
    assert_eq!(request.resource_spans.len(), 1);
    assert_eq!(request.resource_spans[0].scope_spans.len(), 1);
    assert_eq!(spans(&request).len(), batch.span_count);
    request
}

fn spans(request: &proto::ExportTraceServiceRequest) -> &[proto::Span] {
    &request.resource_spans[0].scope_spans[0].spans
}

fn named<'a>(request: &'a proto::ExportTraceServiceRequest, name: &str) -> &'a proto::Span {
    spans(request)
        .iter()
        .find(|span| span.name == name)
        .unwrap()
}

fn attribute<'a>(attributes: &'a [proto::KeyValue], key: &str) -> Option<&'a Value> {
    attributes
        .iter()
        .find(|kv| kv.key == key)
        .and_then(|kv| kv.value.as_ref()?.value.as_ref())
}

fn status_code(span: &proto::Span) -> StatusCode {
    span.status
        .as_ref()
        .map_or(StatusCode::Unset, proto::Status::code)
}

#[test]
fn spans_are_batched_until_the_root_closes() {
    let mut defaults = Attributes::default();
    defaults.push("cloudflare.entrypoint", "MyEntrypoint");
    defaults.push("faas.trigger", "not-applied");
    let mut buffer = buffer_with(false, defaults);

    assert!(buffer.open(ROOT, 0x20, "GET".to_owned(), T0));
    assert!(buffer.open(1, ROOT, "fetch".to_owned(), T0));
    // An id that is already open is refused rather than clobbering the open span.
    assert!(!buffer.open(1, ROOT, "other".to_owned(), T0));
    assert!(buffer.open(3, ROOT, "my-span".to_owned(), T0));
    buffer.set_name(3, "renamed".to_owned());
    buffer.set_status(3, StatusCode::Error, "boom".to_owned());
    buffer.add_exception(3, T0 + 5, "TypeError".to_owned(), "boom".to_owned(), None);
    // Updates for a span that is not open are ignored.
    buffer.set_name(2, "ignored".to_owned());

    // Nothing is handed out until the root closes.
    assert!(
        buffer
            .close(1, T0 + 10, attributes(&[("cached", Value::Bool(true))]))
            .is_none()
    );
    assert!(buffer.close(3, T0 + 10, Attributes::default()).is_none());
    let batch = buffer
        .close(
            ROOT,
            T0 + 20,
            attributes(&[
                ("faas.trigger", string("http")),
                ("cloudflare.outcome", string("ok")),
            ]),
        )
        .unwrap();

    let request = decode(&batch);
    let resource = request.resource_spans[0].resource.as_ref().unwrap();
    assert_eq!(
        attribute(&resource.attributes, "service.name"),
        Some(&string("my-worker"))
    );
    let [fetch, renamed, root] = spans(&request) else {
        panic!("expected three spans");
    };
    assert_eq!(fetch.name, "fetch");
    assert_eq!(fetch.trace_id, (1..=16).collect::<Vec<u8>>());
    assert_eq!(fetch.span_id, [0, 0, 0, 0, 0, 0, 0, 1]);
    assert_eq!(fetch.parent_span_id, [0, 0, 0, 0, 0, 0, 0x10, 0]);
    assert_eq!(fetch.flags, 1);
    assert_eq!(
        attribute(&fetch.attributes, "cached"),
        Some(&Value::Bool(true))
    );
    assert_eq!(renamed.name, "renamed");
    assert_eq!(status_code(renamed), StatusCode::Error);
    assert_eq!(renamed.status.as_ref().unwrap().message, "boom");
    let [exception] = renamed.events.as_slice() else {
        panic!("expected one event");
    };
    assert_eq!(exception.name, "exception");
    assert_eq!(exception.time_unix_nano, (T0 + 5).cast_unsigned());
    assert_eq!(
        attribute(&exception.attributes, "exception.type"),
        Some(&string("TypeError"))
    );
    assert_eq!(
        attribute(&exception.attributes, "exception.stacktrace"),
        None
    );
    assert_eq!(root.name, "GET");
    assert_eq!(root.kind(), SpanKind::Server);
    assert_eq!(root.status, None);
    assert_eq!(root.parent_span_id, [0, 0, 0, 0, 0, 0, 0, 0x20]);

    // Spans are numbered in open order, the root first, and every span carries the default
    // attributes, which do not override its own.
    for (span, sequence_number) in [(fetch, 2), (renamed, 3), (root, 1)] {
        assert_eq!(
            attribute(&span.attributes, "cloudflare.invocation.sequence.number"),
            Some(&Value::Int(sequence_number))
        );
        assert_eq!(
            attribute(&span.attributes, "cloudflare.entrypoint"),
            Some(&string("MyEntrypoint"))
        );
    }
    assert_eq!(
        attribute(&fetch.attributes, "faas.trigger"),
        Some(&string("not-applied"))
    );
    assert_eq!(
        attribute(&root.attributes, "faas.trigger"),
        Some(&string("http"))
    );

    // A span closing after the root is handed out as soon as it closes.
    assert!(buffer.open(4, ROOT, "late".to_owned(), T0));
    let batch = buffer.close(4, T0, Attributes::default()).unwrap();
    assert_eq!(spans(&decode(&batch))[0].name, "late");
    assert!(buffer.finish(T0).is_none());
}

#[test]
fn a_full_batch_is_handed_out_and_open_spans_are_capped() {
    let mut buffer = buffer();
    for id in 1..MAX_BATCH_SPANS as u64 {
        assert!(buffer.open(id, ROOT, "s".to_owned(), T0));
        assert!(buffer.close(id, T0, Attributes::default()).is_none());
    }
    assert!(buffer.open(1, ROOT, "s".to_owned(), T0));
    let batch = buffer.close(1, T0, Attributes::default()).unwrap();
    assert_eq!(batch.span_count, MAX_BATCH_SPANS);

    for id in 1..=MAX_OPEN_SPANS as u64 {
        assert!(buffer.open(id, ROOT, "s".to_owned(), T0));
    }
    assert!(!buffer.open(MAX_OPEN_SPANS as u64 + 1, ROOT, "s".to_owned(), T0));

    // Spans still open at the end are closed then, flagged, in the order they were opened.
    let batch = buffer.finish(T0 - 1).unwrap();
    assert_eq!(batch.span_count, MAX_OPEN_SPANS);
    let request = decode(&batch);
    let unclosed = spans(&request);
    assert_eq!(unclosed[0].span_id, 1u64.to_be_bytes());
    assert_eq!(unclosed[1].span_id, 2u64.to_be_bytes());
    assert_eq!(
        attribute(&unclosed[0].attributes, "cloudflare.warning.type"),
        Some(&string("span_not_ended"))
    );
    assert!(attribute(&unclosed[0].attributes, "cloudflare.warning.message").is_some());
    assert!(unclosed[0].end_time_unix_nano >= unclosed[0].start_time_unix_nano);
    assert!(buffer.finish(T0).is_none());
}

#[test]
fn a_batch_is_handed_out_once_it_is_large() {
    let mut buffer = buffer();
    let big = "x".repeat(MAX_BATCH_BYTES / 2);
    assert!(buffer.open(1, ROOT, "s".to_owned(), T0));
    assert!(
        buffer
            .close(1, T0, attributes(&[("big", string(&big))]))
            .is_none()
    );
    assert!(buffer.open(2, ROOT, "s".to_owned(), T0));
    let batch = buffer
        .close(2, T0, attributes(&[("big", string(&big))]))
        .unwrap();
    assert_eq!(batch.span_count, 2);
}

#[test]
fn kind_status_and_url_rules_apply_at_close() {
    let mut buffer = buffer();
    assert!(buffer.open(ROOT, 0, "scheduled".to_owned(), T0));
    for (id, name) in [(1, "fetch"), (3, "queue_send"), (6, "fetch")] {
        assert!(buffer.open(id, ROOT, name.to_owned(), T0));
    }

    // A 4xx child becomes an error with error.type; its URL is split into parts.
    buffer.close(
        1,
        T0,
        attributes(&[
            (
                "url.full",
                string("https://example.com:8443/a/b?x=1&y#frag"),
            ),
            ("http.response.status_code", Value::Int(404)),
        ]),
    );
    buffer.close(3, T0, Attributes::default());
    // A 2xx is left unset, and an end before the start is moved to the start.
    buffer.close(
        6,
        T0 - 1,
        attributes(&[
            ("url.full", string("http://[::1]")),
            ("http.response.status_code", Value::Int(200)),
        ]),
    );
    // A root with a non-ok outcome is an error, and a consumer for a timer trigger.
    let batch = buffer
        .close(
            ROOT,
            T0,
            attributes(&[
                ("faas.trigger", string("timer")),
                ("cloudflare.outcome", string("exceededCpu")),
            ]),
        )
        .unwrap();
    let request = decode(&batch);
    assert_eq!(batch.span_count, 4);

    let fetch = named(&request, "fetch");
    assert_eq!(fetch.kind(), SpanKind::Client);
    assert_eq!(status_code(fetch), StatusCode::Error);
    assert_eq!(
        attribute(&fetch.attributes, "error.type"),
        Some(&string("404"))
    );
    assert_eq!(
        attribute(&fetch.attributes, "server.port"),
        Some(&Value::Int(8443))
    );
    assert_eq!(
        attribute(&fetch.attributes, "url.query"),
        Some(&string("x=1&y"))
    );

    let queue_send = named(&request, "queue_send");
    assert_eq!(queue_send.kind(), SpanKind::Producer);
    assert_eq!(
        attribute(
            &queue_send.attributes,
            "cloudflare.invocation.sequence.number"
        ),
        Some(&Value::Int(3))
    );

    let ipv6 = &spans(&request)[2];
    assert_eq!(ipv6.status, None);
    assert_eq!(attribute(&ipv6.attributes, "error.type"), None);
    assert_eq!(ipv6.end_time_unix_nano, ipv6.start_time_unix_nano);
    assert_eq!(fetch.start_time_unix_nano, T0.cast_unsigned());
    assert_eq!(ipv6.start_time_unix_nano, T0.cast_unsigned());

    let root = named(&request, "scheduled");
    assert_eq!(root.kind(), SpanKind::Consumer);
    assert_eq!(status_code(root), StatusCode::Error);
    assert_eq!(
        attribute(&root.attributes, "error.type"),
        Some(&string("exceededCpu"))
    );
    assert_eq!(root.parent_span_id, [] as [u8; 0]);
}

#[test]
fn unset_does_not_clear_an_explicit_error() {
    let mut buffer = buffer();
    assert!(buffer.open(ROOT, 0, "mine".to_owned(), T0));
    buffer.set_status(ROOT, StatusCode::Error, "first error".to_owned());
    buffer.set_status(ROOT, StatusCode::Error, "second error".to_owned());
    buffer.set_status(ROOT, StatusCode::Unset, String::new());

    let batch = buffer.close(ROOT, T0, Attributes::default()).unwrap();
    let request = decode(&batch);
    let span = named(&request, "mine");
    assert_eq!(status_code(span), StatusCode::Error);
    assert_eq!(span.status.as_ref().unwrap().message, "second error");
}

#[test]
fn ok_is_final_for_explicit_status_updates() {
    let mut buffer = buffer();
    assert!(buffer.open(ROOT, 0, "root".to_owned(), T0));
    for (id, code) in [(1, StatusCode::Error), (2, StatusCode::Unset)] {
        assert!(buffer.open(id, ROOT, format!("after-ok-{id}"), T0));
        buffer.set_status(id, StatusCode::Error, "temporary error".to_owned());
        buffer.set_status(id, StatusCode::Ok, String::new());
        buffer.set_status(id, code, "ignored".to_owned());
        assert!(buffer.close(id, T0, Attributes::default()).is_none());
    }

    let batch = buffer.close(ROOT, T0, Attributes::default()).unwrap();
    let request = decode(&batch);
    for name in ["after-ok-1", "after-ok-2"] {
        let span = named(&request, name);
        assert_eq!(status_code(span), StatusCode::Ok);
        assert_eq!(span.status.as_ref().unwrap().message, "");
    }
}

#[test]
fn explicit_error_messages_take_precedence_over_recorded_exceptions() {
    let mut buffer = buffer();
    assert!(buffer.open(ROOT, 0, "root".to_owned(), T0));
    for (id, exception_first) in [(1, false), (2, true)] {
        assert!(buffer.open(id, ROOT, format!("explicit-error-{id}"), T0));
        if !exception_first {
            buffer.set_status(id, StatusCode::Error, "user description".to_owned());
        }
        buffer.add_exception(
            id,
            T0,
            "Error".to_owned(),
            "exception message".to_owned(),
            None,
        );
        if exception_first {
            buffer.set_status(id, StatusCode::Error, "user description".to_owned());
        }
        assert!(buffer.close(id, T0, Attributes::default()).is_none());
    }

    let batch = buffer.close(ROOT, T0, Attributes::default()).unwrap();
    let request = decode(&batch);
    for name in ["explicit-error-1", "explicit-error-2"] {
        let span = named(&request, name);
        assert_eq!(status_code(span), StatusCode::Error);
        assert_eq!(span.status.as_ref().unwrap().message, "user description");
        let [exception] = span.events.as_slice() else {
            panic!("expected one exception event");
        };
        assert_eq!(exception.name, "exception");
        assert_eq!(
            attribute(&exception.attributes, "exception.message"),
            Some(&string("exception message"))
        );
    }
}

#[test]
fn a_status_the_worker_set_and_exceptions_come_before_derived_errors() {
    let mut buffer = buffer_with(true, Attributes::default());
    assert!(buffer.open(ROOT, 0, "GET".to_owned(), T0));
    assert!(buffer.open(1, ROOT, "mine".to_owned(), T0));
    assert!(buffer.open(2, ROOT, "unset".to_owned(), T0));
    buffer.set_status(1, StatusCode::Ok, String::new());
    buffer.set_status(2, StatusCode::Unset, String::new());
    let failed = attributes(&[("http.response.status_code", Value::Int(503))]);
    buffer.close(1, T0, failed.clone());
    buffer.close(2, T0, failed);
    buffer.add_exception(
        ROOT,
        T0,
        "Error".to_owned(),
        "boom".to_owned(),
        Some("at fetch".to_owned()),
    );
    let batch = buffer
        .close(
            ROOT,
            T0,
            attributes(&[
                ("url.full", string("https://example.com/p?secret=1")),
                ("http.response.status_code", Value::Int(500)),
            ]),
        )
        .unwrap();
    let request = decode(&batch);

    let mine = named(&request, "mine");
    assert_eq!(status_code(mine), StatusCode::Ok);
    assert_eq!(attribute(&mine.attributes, "error.type"), None);
    // An explicit UNSET does not stand in the way of a derived error.
    let unset = named(&request, "unset");
    assert_eq!(status_code(unset), StatusCode::Error);
    assert_eq!(
        attribute(&unset.attributes, "error.type"),
        Some(&string("503"))
    );

    let root = named(&request, "GET");
    assert_eq!(status_code(root), StatusCode::Error);
    assert_eq!(root.status.as_ref().unwrap().message, "boom");
    assert_eq!(
        attribute(&root.attributes, "error.type"),
        Some(&string("500"))
    );
    assert_eq!(
        attribute(&root.events[0].attributes, "exception.stacktrace"),
        Some(&string("at fetch"))
    );
    assert_eq!(
        attribute(&root.attributes, "url.full"),
        Some(&string("https://example.com/p"))
    );
    assert_eq!(attribute(&root.attributes, "url.query"), None);
}

#[test]
fn a_buffer_that_exports_no_spans_still_keeps_its_root() {
    let mut resource = Attributes::default();
    resource.push("service.name", "my-worker");
    let mut buffer = SpanBuffer::new(
        Options {
            trace_id: (1..=16).collect(),
            trace_flags: 0,
            root_span_id: ROOT,
            redact_query_string: true,
            omit_timestamps: false,
            export_spans: false,
            keep_root: true,
        },
        resource,
        Attributes::default(),
    );
    assert!(buffer.open(ROOT, 0, "GET".to_owned(), T0));
    let closed = attributes(&[
        ("url.full", string("https://example.com/a?b=1")),
        ("cloudflare.outcome", string("exception")),
    ]);
    assert!(buffer.close(ROOT, T0 + 1, closed).is_none());
    assert!(buffer.finish(T0 + 2).is_none());

    // The root's OTLP form: the URL rules, the derived error and the sequence number apply.
    let root = buffer.take_root().unwrap();
    assert_eq!(root.name, "GET");
    assert_eq!(root.end_time_unix_nano, (T0 + 1).cast_unsigned());
    assert_eq!(
        attribute(&root.attributes, "url.full"),
        Some(&string("https://example.com/a"))
    );
    assert_eq!(
        attribute(&root.attributes, "error.type"),
        Some(&string("exception"))
    );
    assert!(buffer.take_root().is_none());
}

#[test]
fn timestamps_can_be_omitted() {
    let mut buffer = SpanBuffer::new(
        Options {
            trace_id: vec![0; 16],
            trace_flags: 0,
            root_span_id: ROOT,
            redact_query_string: false,
            omit_timestamps: true,
            export_spans: true,
            keep_root: false,
        },
        Attributes::default(),
        Attributes::default(),
    );
    assert!(buffer.open(ROOT, 0, "GET".to_owned(), T0));
    buffer.add_exception(ROOT, T0, "Error".to_owned(), "boom".to_owned(), None);
    let batch = buffer.close(ROOT, T0 + 1, Attributes::default()).unwrap();
    let request = decode(&batch);
    let root = named(&request, "GET");
    assert_eq!(root.start_time_unix_nano, 0);
    assert_eq!(root.end_time_unix_nano, 0);
    assert_eq!(root.events[0].time_unix_nano, 0);
    assert_eq!(root.flags, 0);
}
