// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use capnp::message::Builder;
use capnp::message::HeapAllocator;

use super::*;

fn round_trip(status: &SpanStatus) -> SpanStatus {
    let mut message = Builder::new(HeapAllocator::new());
    status.to_capnp(message.init_root());
    let reader = message
        .get_root_as_reader::<span_status::Reader<'_>>()
        .unwrap();
    SpanStatus::from_capnp(reader).unwrap()
}

#[test]
fn only_an_error_keeps_its_message() {
    let error = SpanStatus::new(SpanStatusCode::Error, Some(b"boom".to_vec()));
    assert_eq!(error.message(), Some(&b"boom"[..]));
    assert_eq!(error.size(), 4);
    assert_eq!(round_trip(&error), error);

    let ok = SpanStatus::new(SpanStatusCode::Ok, Some(b"boom".to_vec()));
    assert_eq!(ok.message(), None);
    assert_eq!(ok.size(), 0);
    assert_eq!(round_trip(&ok), ok);
}

#[test]
fn a_message_on_a_non_error_code_is_dropped_when_read() {
    let mut message = Builder::new(HeapAllocator::new());
    {
        let mut root = message.init_root::<span_status::Builder<'_>>();
        root.set_code(SpanStatusCode::Unset);
        root.init_message().set_text("ignored");
    }
    let reader = message
        .get_root_as_reader::<span_status::Reader<'_>>()
        .unwrap();
    let status = SpanStatus::from_capnp(reader).unwrap();
    assert_eq!(status.code(), SpanStatusCode::Unset);
    assert_eq!(status.message(), None);
}
