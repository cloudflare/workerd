// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use capnp::message::Builder;
use capnp::message::HeapAllocator;

use super::*;

fn traceparent(s: &str) -> Option<SpanContext> {
    SpanContext::from_traceparent(s.as_bytes())
}

const VALID: &str = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

#[test]
fn traceparent_round_trips() {
    let context = traceparent(VALID).unwrap();
    assert_eq!(
        context.trace_id,
        TraceId {
            high: 0x4bf9_2f35_77b3_4da6,
            low: 0xa3ce_929d_0e0e_4736
        }
    );
    assert_eq!(context.span_id, Some(SpanId(0x00f0_67aa_0ba9_02b7)));
    assert_eq!(context.trace_flags, Some(TraceFlags(1)));
    assert_eq!(context.to_traceparent().unwrap(), VALID);
}

#[test]
fn traceparent_accepts_upper_case_and_writes_lower_case() {
    let context = traceparent(&VALID.to_ascii_uppercase()).unwrap();
    assert_eq!(context.to_traceparent().unwrap(), VALID);
}

#[test]
fn traceparent_rejects_what_cpp_rejects() {
    for bad in [
        "",
        &VALID[..54],
        &format!("{VALID}0"),
        "01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
        "ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
        "00-00000000000000000000000000000000-00f067aa0ba902b7-01",
        "00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01",
        "00_4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
        "00-4bf92f3577b34da6a3ce929d0e0e4736_00f067aa0ba902b7-01",
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7_01",
        "00-4bf92f3577b34da6a3ce929d0e0e473g-00f067aa0ba902b7-01",
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0g",
    ] {
        assert_eq!(traceparent(bad), None, "{bad}");
    }
}

#[test]
fn traceparent_needs_a_span_id_flags_and_nonzero_ids() {
    let mut context = traceparent(VALID).unwrap();
    context.trace_flags = None;
    assert!(context.to_traceparent().is_none());
    let mut context = traceparent(VALID).unwrap();
    context.span_id = None;
    assert!(context.to_traceparent().is_none());
    let mut context = traceparent(VALID).unwrap();
    context.span_id = Some(SpanId::NULL);
    assert!(context.to_traceparent().is_none());
    let mut context = traceparent(VALID).unwrap();
    context.trace_id = TraceId::NULL;
    assert!(context.to_traceparent().is_none());
}

#[test]
fn flags_are_two_digits() {
    let context = SpanContext {
        trace_id: TraceId { high: 0, low: 1 },
        span_id: Some(SpanId(1)),
        trace_flags: Some(TraceFlags(0x0a)),
    };
    assert_eq!(
        context.to_traceparent().unwrap(),
        "00-00000000000000000000000000000001-0000000000000001-0a"
    );
}

fn span_context_round_trip(context: SpanContext) -> SpanContext {
    let mut message = Builder::new(HeapAllocator::new());
    context.to_capnp(message.init_root());
    let reader = message
        .get_root_as_reader::<span_context::Reader<'_>>()
        .unwrap();
    SpanContext::from_capnp(reader).unwrap()
}

#[test]
fn span_context_capnp_keeps_flags_only_when_set() {
    let with = SpanContext {
        trace_id: TraceId { high: 1, low: 2 },
        span_id: Some(SpanId(3)),
        trace_flags: Some(TraceFlags(1)),
    };
    let back = span_context_round_trip(with);
    assert_eq!(back, with);
    assert_eq!(back.trace_flags, Some(TraceFlags(1)));

    let without = SpanContext {
        trace_flags: None,
        span_id: None,
        ..with
    };
    let back = span_context_round_trip(without);
    assert_eq!(back, without);
    assert_eq!(back.trace_flags, None);
}

#[test]
fn invocation_contexts_round_trip_through_capnp_as_triggers() {
    let parent = InvocationSpanContext {
        trace_id: TraceId { high: 2, low: 1 },
        invocation_id: TraceId { high: 4, low: 3 },
        span_id: SpanId(5),
        trace_flags: None,
        parent: None,
        is_trigger: false,
    };
    let mut context = InvocationSpanContext {
        span_id: SpanId(6),
        parent: Some(Box::new(parent.clone())),
        ..parent
    };
    context.set_default_trace_flags(TraceFlags(1));
    context.set_default_trace_flags(TraceFlags(0));
    assert_eq!(context.trace_flags, Some(TraceFlags(1)));
    // Equality looks at the ids only.
    assert_ne!(context, parent);
    assert_eq!(
        context,
        InvocationSpanContext {
            trace_flags: None,
            parent: None,
            ..context.clone()
        }
    );

    let mut message = Builder::new(HeapAllocator::new());
    context.to_capnp(message.init_root());
    let reader = message
        .get_root_as_reader::<invocation_span_context::Reader<'_>>()
        .unwrap();
    let trigger = InvocationSpanContext::from_capnp(reader).unwrap().unwrap();
    assert_eq!(trigger, context);
    assert!(trigger.is_trigger);
    assert!(trigger.parent.is_none());
    assert_eq!(trigger.trace_flags, Some(TraceFlags(1)));
}

#[test]
fn invocation_context_capnp_needs_both_ids() {
    let mut message = Builder::new(HeapAllocator::new());
    {
        let mut root = message.init_root::<invocation_span_context::Builder<'_>>();
        TraceId { high: 0, low: 1 }.to_capnp(root.reborrow().init_trace_id());
    }
    let reader = message
        .get_root_as_reader::<invocation_span_context::Reader<'_>>()
        .unwrap();
    assert!(InvocationSpanContext::from_capnp(reader).unwrap().is_none());

    let mut message = Builder::new(HeapAllocator::new());
    {
        let mut root = message.init_root::<invocation_span_context::Builder<'_>>();
        TraceId { high: 0, low: 1 }.to_capnp(root.reborrow().init_trace_id());
        TraceId::NULL.to_capnp(root.reborrow().init_invocation_id());
    }
    let reader = message
        .get_root_as_reader::<invocation_span_context::Reader<'_>>()
        .unwrap();
    assert!(InvocationSpanContext::from_capnp(reader).unwrap().is_none());
}
