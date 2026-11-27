// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use capnp::message::Builder;
use capnp::message::HeapAllocator;

use super::*;

fn round_trip(attribute: &Attribute) -> Attribute {
    let mut message = Builder::new(HeapAllocator::new());
    attribute.to_capnp(message.init_root());
    let reader = message
        .get_root_as_reader::<attribute::Reader<'_>>()
        .unwrap();
    Attribute::from_capnp(reader).unwrap()
}

#[test]
fn every_kind_of_value_round_trips() {
    let attribute = Attribute {
        name: b"key".to_vec(),
        values: vec![
            TagValue::String(b"text".to_vec()),
            TagValue::String(vec![0xff, 0xfe]),
            TagValue::Bool(true),
            TagValue::Int64(-7),
            TagValue::Float64(1.5),
            TagValue::StringArray(vec![Some(b"a".to_vec()), None]),
            TagValue::BoolArray(vec![None, Some(false)]),
            TagValue::Float64Array(vec![Some(2.5), None]),
            TagValue::StringArray(Vec::new()),
        ],
    };
    assert_eq!(round_trip(&attribute), attribute);
}

#[test]
fn a_one_element_array_stays_an_array() {
    let attribute = Attribute {
        name: b"key".to_vec(),
        values: vec![TagValue::BoolArray(vec![Some(true)])],
    };
    assert_eq!(round_trip(&attribute).values, attribute.values);
    assert_ne!(attribute.values[0], TagValue::Bool(true));
}

#[test]
fn an_attribute_needs_a_value_list() {
    let mut message = Builder::new(HeapAllocator::new());
    message
        .init_root::<attribute::Builder<'_>>()
        .set_name("key");
    let reader = message
        .get_root_as_reader::<attribute::Reader<'_>>()
        .unwrap();
    assert!(Attribute::from_capnp(reader).is_err());
}
