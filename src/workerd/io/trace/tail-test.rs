// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use capnp::message::Builder;
use capnp::message::HeapAllocator;

use super::*;
use crate::JsRpcEventInfo;
use crate::SpanStatusCode;
use crate::TagValue;

fn round_trip(event: &TailEvent) -> TailEvent {
    let mut message = Builder::new(HeapAllocator::new());
    event.to_capnp(message.init_root()).unwrap();
    TailEvent::from_capnp(message.get_root_as_reader().unwrap()).unwrap()
}

fn event(kind: TailEventKind) -> TailEvent {
    TailEvent {
        span_context: SpanContext {
            trace_id: TraceId { high: 1, low: 2 },
            span_id: Some(SpanId(3)),
            trace_flags: None,
        },
        invocation_id: TraceId { high: 4, low: 5 },
        timestamp_ns: 6,
        sequence: 7,
        event: kind,
    }
}

fn script_version() -> ScriptVersion {
    let mut message = Builder::new(HeapAllocator::new());
    let mut version = message.init_root::<script_version_capnp::script_version::Builder<'_>>();
    version.reborrow().init_id().set_upper(8);
    version.set_tag("v1");
    ScriptVersion::from_capnp(version.into_reader()).unwrap()
}

#[test]
fn every_kind_round_trips() {
    let attribute = Attribute {
        name: b"key".to_vec(),
        values: vec![TagValue::Int64(1)],
    };
    let kinds = [
        TailEventKind::Onset(Box::new(Onset {
            span_id: SpanId(9),
            info: EventInfo::JsRpc(JsRpcEventInfo {
                method_name: b"run".to_vec(),
            }),
            worker: WorkerInfo {
                execution_model: ExecutionModel::DurableObject,
                script_name: Some(b"worker".to_vec()),
                script_version: Some(script_version()),
                preview: None,
                dispatch_namespace: None,
                script_id: Some(Vec::new()),
                script_tags: Some(vec![b"a".to_vec(), Vec::new()]),
                entrypoint: None,
                durable_object_id: Some(b"do".to_vec()),
            },
            attributes: vec![attribute.clone()],
        })),
        TailEventKind::Outcome(Outcome {
            outcome: EventOutcome::ExceededCpu,
            cpu_time_ms: 1,
            wall_time_ms: u64::MAX,
        }),
        TailEventKind::SpanOpen(SpanOpen {
            operation_name: b"fetch".to_vec(),
            info: Some(SpanOpenInfo::Custom(vec![attribute.clone()])),
            span_id: SpanId(10),
        }),
        TailEventKind::SpanClose(SpanClose {
            outcome: EventOutcome::Ok,
        }),
        TailEventKind::SpanUpdate(SpanUpdate::Status(SpanStatus::new(
            SpanStatusCode::Error,
            Some(b"failed".to_vec()),
        ))),
        TailEventKind::SpanLinks(SpanLinks(Vec::new())),
        TailEventKind::DiagnosticChannel(DiagnosticChannelEvent {
            timestamp_ns: -1,
            channel: b"channel".to_vec(),
            message: vec![0, 255],
        }),
        TailEventKind::Exception(Exception {
            timestamp_ns: 11,
            name: b"Error".to_vec(),
            message: b"boom".to_vec(),
            stack: None,
            code: Some(ExceptionCode::Number(2.5)),
        }),
        TailEventKind::Log(Log {
            timestamp_ns: 12,
            level: LogLevel::Warn,
            message: b"[\"hi\"]".to_vec(),
            truncated: true,
            error_info: Some(vec![
                None,
                Some(ErrorInfo {
                    name: b"TypeError".to_vec(),
                    message: Vec::new(),
                    stack: Some(b"at f".to_vec()),
                }),
            ]),
        }),
        TailEventKind::StreamDiagnostics(StreamDiagnosticsEvent {
            dropped_events_count: 13,
        }),
        TailEventKind::Return(Return {
            info: Some(FetchResponseInfo { status_code: 200 }),
        }),
        TailEventKind::Attributes(vec![attribute]),
    ];
    for kind in kinds {
        let event = event(kind);
        let read = round_trip(&event);
        assert_eq!(read, event);
        assert_eq!(
            read.span_context.trace_flags,
            event.span_context.trace_flags
        );
    }
}

#[test]
fn an_undefined_stream_diagnostic_is_rejected() {
    let mut message = Builder::new(HeapAllocator::new());
    message
        .init_root::<trace::stream_diagnostics_event::Builder<'_>>()
        .init_diagnostic()
        .set_undefined(());
    let reader = message
        .get_root_as_reader::<trace::stream_diagnostics_event::Reader<'_>>()
        .unwrap();
    assert!(StreamDiagnosticsEvent::from_capnp(reader).is_err());
}

#[test]
fn a_log_without_error_info_keeps_none_and_an_empty_list_stays_empty() {
    let mut log = Log {
        timestamp_ns: 0,
        level: LogLevel::Log,
        message: Vec::new(),
        truncated: false,
        error_info: None,
    };
    for error_info in [None, Some(Vec::new())] {
        log.error_info = error_info;
        let read = round_trip(&event(TailEventKind::Log(log.clone())));
        assert_eq!(read.event, TailEventKind::Log(log.clone()));
    }
}

#[test]
fn a_script_version_is_read_back() {
    let version = script_version();
    let tag = version
        .read(|version| version.get_tag().unwrap().to_str().unwrap().to_owned())
        .unwrap();
    assert_eq!(tag, "v1");
}
