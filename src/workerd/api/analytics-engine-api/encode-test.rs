// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use analytics_engine_capnp::analytics_engine_event;
use jsg::Nullable;

use super::*;

const PREFIX: &str = "prefix: ";

fn bytes(value: &str) -> Nullable<Bytes> {
    Nullable::Some(Bytes(value.as_bytes().to_vec()))
}

fn error_message(result: jsg::Result<()>) -> String {
    let err = result.expect_err("the data point should be over a limit");
    assert_eq!(err.name, jsg::ExceptionType::TypeError);
    err.message
}

#[test]
fn values_at_the_limits_pass() {
    let half = "a".repeat(MAX_CUMULATIVE_BYTES_IN_BLOBS / 2);
    let point = DataPoint {
        indexes: Some(vec![bytes(&"i".repeat(MAX_INDEX_SIZE_IN_BYTES))]),
        doubles: Some(vec![1.0; MAX_ARRAY_MEMBERS]),
        blobs: Some(vec![bytes(&half), bytes(&half)]),
    };
    validate(&point, PREFIX).unwrap();
    validate(&DataPoint::default(), PREFIX).unwrap();
}

#[test]
fn each_limit_has_its_error() {
    assert_eq!(
        error_message(validate_indexes(&[bytes("a"), bytes("b")], PREFIX)),
        "prefix: Maximum of 1 indexes supported."
    );
    assert_eq!(
        error_message(validate_indexes(&[bytes(&"a".repeat(97))], PREFIX)),
        "prefix: Size of indexes[0] exceeds 96 bytes)."
    );
    assert_eq!(
        error_message(validate_doubles(&[0.0; 21], PREFIX)),
        "prefix: Maximum of 20 doubles supported."
    );
    // Null blobs count towards the number of blobs.
    let nulls: Vec<_> = std::iter::repeat_with(|| Nullable::Null).take(21).collect();
    assert_eq!(
        error_message(validate_blobs(&nulls, PREFIX)),
        "prefix: Maximum of 20 blobs supported."
    );
    let half = "a".repeat(8000);
    assert_eq!(
        error_message(validate_blobs(
            &[bytes(&half), Nullable::Null, bytes(&(half.clone() + "a"))],
            PREFIX
        )),
        "prefix: Cumulative size of blobs exceeds 16000 bytes)."
    );
}

#[test]
fn limits_are_checked_for_indexes_then_blobs_then_doubles() {
    let point = |indexes: usize, blobs: usize| DataPoint {
        indexes: Some(vec![bytes("i"); indexes]),
        doubles: Some(vec![1.0; 21]),
        blobs: Some(vec![bytes("b"); blobs]),
    };
    assert!(error_message(validate(&point(2, 21), PREFIX)).contains("indexes"));
    assert!(error_message(validate(&point(1, 21), PREFIX)).contains("blobs"));
    assert!(error_message(validate(&point(1, 20), PREFIX)).contains("doubles"));
}

fn decode(message: &[u8], check: impl FnOnce(analytics_engine_event::Reader<'_>)) {
    let reader = capnp::serialize::read_message_from_flat_slice(
        &mut &message[..],
        capnp::message::ReaderOptions::new(),
    )
    .unwrap();
    check(
        reader
            .get_root::<analytics_engine_event::Reader<'_>>()
            .unwrap(),
    );
}

fn header() -> Header {
    Header {
        account_id: 1,
        dataset: "analytics".to_owned(),
        schema_version: 0,
    }
}

#[test]
fn an_empty_data_point_has_only_the_header() {
    decode(&encode(&header(), &DataPoint::default(), 42), |event| {
        assert_eq!(event.get_account_id(), 1);
        assert_eq!(event.get_dataset().unwrap(), b"analytics");
        assert_eq!(event.get_schema_version(), 0);
        assert_eq!(event.get_timestamp(), 42);
        // index1 is set, but empty.
        assert!(event.has_index1());
        assert_eq!(event.get_index1().unwrap(), b"");
        assert!(!event.has_blob1());
        assert!(event.get_double1().abs() < f64::EPSILON);
    });
}

#[test]
fn values_go_to_their_numbered_fields() {
    let point = DataPoint {
        indexes: Some(vec![bytes("idx")]),
        doubles: Some((1..=20).map(f64::from).collect()),
        blobs: Some(vec![
            Nullable::Null,
            bytes("two"),
            Nullable::Null,
            bytes("four"),
        ]),
    };
    decode(&encode(&header(), &point, 0), |event| {
        assert_eq!(event.get_index1().unwrap(), b"idx");
        assert!((event.get_double1() - 1.0).abs() < f64::EPSILON);
        assert!((event.get_double20() - 20.0).abs() < f64::EPSILON);
        // A null blob leaves its field unset, and the next one keeps its position.
        assert!(!event.has_blob1());
        assert_eq!(event.get_blob2().unwrap(), b"two");
        assert!(!event.has_blob3());
        assert_eq!(event.get_blob4().unwrap(), b"four");
    });
    // A null index leaves index1 empty.
    let point = DataPoint {
        indexes: Some(vec![Nullable::Null]),
        ..DataPoint::default()
    };
    decode(&encode(&header(), &point, 0), |event| {
        assert_eq!(event.get_index1().unwrap(), b"");
    });
}
