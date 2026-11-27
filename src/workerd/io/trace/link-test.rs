// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use capnp::message::Builder;
use capnp::message::HeapAllocator;

use super::*;
use crate::SpanId;
use crate::TagValue;
use crate::TraceFlags;
use crate::TraceId;

#[test]
fn links_round_trip_with_zero_ids_and_attributes() {
    let links = SpanLinks(vec![
        SpanLink {
            context: SpanContext {
                trace_id: TraceId::NULL,
                span_id: Some(SpanId::NULL),
                trace_flags: Some(TraceFlags(1)),
            },
            attributes: vec![Attribute {
                name: b"link.kind".to_vec(),
                values: vec![TagValue::String(b"follows".to_vec())],
            }],
            dropped_attributes_count: 3,
        },
        SpanLink {
            context: SpanContext {
                trace_id: TraceId { high: 1, low: 2 },
                span_id: Some(SpanId(3)),
                trace_flags: None,
            },
            attributes: Vec::new(),
            dropped_attributes_count: 0,
        },
    ]);
    let mut message = Builder::new(HeapAllocator::new());
    links.to_capnp(message.init_root());
    let reader = message
        .get_root_as_reader::<span_links::Reader<'_>>()
        .unwrap();
    let back = SpanLinks::from_capnp(reader).unwrap();
    assert_eq!(back, links);
    assert_eq!(back.0[0].context.trace_flags, Some(TraceFlags(1)));
}
