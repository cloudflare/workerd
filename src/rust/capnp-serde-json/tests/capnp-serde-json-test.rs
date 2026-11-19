// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests of the types capnpc-rust-serde-json generates.
//!
//! Most cases decode JSON as the C++ `capnp::JsonCodec` (with `handleByAnnotation()`) writes it
//! with its default `HasMode::NON_NULL`, and check the JSON the Rust types write, which leaves out
//! fields that hold their defaults as the C++ codec does with `HasMode::NON_DEFAULT`.
//! `capnp-serde-json-ffi-test` checks the same documents against the C++ codec.

#![expect(
    clippy::unwrap_used,
    reason = "the test helpers fail the test when a document does not decode"
)]

use capnp_serde_json_test_schema as fixture;
use serde::Serialize;
use serde::de::DeserializeOwned;
use serde_json::Value;
use serde_json::json;

/// Decodes `json` as `T`, and returns the value and the JSON `T` writes, which `T` must decode to
/// the same value.
fn round_trip<T: Serialize + DeserializeOwned + PartialEq + std::fmt::Debug>(
    json: &str,
) -> (T, Value) {
    let value: T =
        serde_json::from_str(json).unwrap_or_else(|error| panic!("decoding {json}: {error}"));
    let rust_json = serde_json::to_string(&value).unwrap();
    let again: T = serde_json::from_str(&rust_json).unwrap();
    assert_eq!(again, value, "{rust_json}");
    (value, serde_json::from_str(&rust_json).unwrap())
}

fn rejects<T: DeserializeOwned>(json: &str) -> bool {
    serde_json::from_str::<T>(json).is_err()
}

#[test]
fn constants() {
    assert_eq!(fixture::ANSWER, -42);
    assert_eq!(fixture::BIG, u64::MAX);
    const { assert!(fixture::YES) };
    assert_eq!(fixture::OUTER_LIMIT, 7);
}

#[test]
fn primitives() {
    let (value, json) = round_trip::<fixture::Primitives>(
        r#"{"bool": true, "int8": -8, "int16": -16, "int32": -32, "int64": "-64", "uint8": 8,
            "uint16": 16, "uint32": 32, "uint64": "18446744073709551615"}"#,
    );
    assert_eq!(value.int64, -64);
    assert_eq!(value.uint64, u64::MAX);
    assert_eq!(
        json,
        json!({"bool": true, "int8": -8, "int16": -16, "int32": -32, "int64": "-64", "uint8": 8,
               "uint16": 16, "uint32": 32, "uint64": "18446744073709551615"})
    );

    let (value, json) = round_trip::<fixture::Primitives>(
        r#"{"bool": false, "int8": 0, "int16": 0, "int32": 0, "int64": "0", "uint8": 0,
            "uint16": 0, "uint32": 0, "uint64": "0"}"#,
    );
    assert_eq!(value, fixture::Primitives::default());
    assert_eq!(json, json!({}));

    // 64-bit integers may be JSON numbers, as other writers write them.
    let value: fixture::Primitives =
        serde_json::from_str(r#"{"int64": -5, "uint64": 18446744073709551615}"#).unwrap();
    assert_eq!(value.int64, -5);
    assert_eq!(value.uint64, u64::MAX);
}

#[test]
fn defaults() {
    let value = fixture::Defaults::default();
    assert!(value.bool);
    assert_eq!(value.int8, -8);
    assert_eq!(value.int64, -64);
    assert_eq!(value.uint32, u32::MAX);
    assert_eq!(value.uint64, u64::MAX);

    // A missing field takes its schema default.
    let (value, json) = round_trip::<fixture::Defaults>("{}");
    assert_eq!(value, fixture::Defaults::default());
    assert_eq!(json, json!({}));

    let (_, json) = round_trip::<fixture::Defaults>(
        r#"{"bool": true, "int8": -8, "int64": "-64", "uint32": 4294967295,
            "uint64": "18446744073709551615"}"#,
    );
    assert_eq!(json, json!({}));

    let (_, json) = round_trip::<fixture::Defaults>(
        r#"{"bool": false, "int8": 0, "int64": "0", "uint32": 0, "uint64": "0"}"#,
    );
    assert_eq!(
        json,
        json!({"bool": false, "int8": 0, "int64": "0", "uint32": 0, "uint64": "0"})
    );

    let value: fixture::Defaults = serde_json::from_str(r#"{"int8": 3}"#).unwrap();
    assert_eq!(value.int8, 3);
    assert_eq!(value.uint64, u64::MAX);
}

#[test]
fn pointers() {
    let (value, json) = round_trip::<fixture::Pointers>(
        r#"{"text": "text", "base64": "YWJj", "hex": "616263", "ints": ["-1", "2"],
            "shorts": [0, 65535], "texts": ["x", "y"],
            "structs": [
              {"bool": false, "int8": 1, "int16": 0, "int32": 0, "int64": "0", "uint8": 0,
               "uint16": 0, "uint32": 0, "uint64": "0"},
              {"bool": false, "int8": 0, "int16": 0, "int32": 0, "int64": "0", "uint8": 0,
               "uint16": 0, "uint32": 0, "uint64": "0"}],
            "primitives": {"bool": false, "int8": 0, "int16": 0, "int32": 0, "int64": "0",
                           "uint8": 0, "uint16": 0, "uint32": 5, "uint64": "0"},
            "not-a-rust-identifier": "renamed", "self": "self", "match": "match"}"#,
    );
    assert_eq!(value.base64.as_deref(), Some(&b"abc"[..]));
    assert_eq!(value.hex.as_deref(), Some(&b"abc"[..]));
    assert_eq!(value.self_.as_deref(), Some("self"));
    assert_eq!(value.r#match.as_deref(), Some("match"));
    assert_eq!(
        json,
        json!({"text": "text", "base64": "YWJj", "hex": "616263", "ints": ["-1", "2"],
               "shorts": [0, 65535], "texts": ["x", "y"], "structs": [{"int8": 1}, {}],
               "primitives": {"uint32": 5}, "not-a-rust-identifier": "renamed", "self": "self",
               "match": "match"})
    );

    let (value, json) = round_trip::<fixture::Pointers>("{}");
    assert_eq!(value, fixture::Pointers::default());
    assert_eq!(json, json!({}));

    // null is a null pointer, as in C++.
    let value: fixture::Pointers =
        serde_json::from_str(r#"{"text": null, "hex": null, "ints": null, "primitives": null}"#)
            .unwrap();
    assert_eq!(value, fixture::Pointers::default());
}

#[test]
fn unions() {
    let (value, json) = round_trip::<fixture::Unions>(r#"{"common": "c", "a": 3, "number": "5"}"#);
    assert_eq!(value.common.as_deref(), Some("c"));
    assert_eq!(value.named, fixture::UnionsNamed::A(3));
    assert_eq!(value.which, fixture::UnionsWhich::Number(5));
    assert_eq!(json, json!({"common": "c", "a": 3, "number": "5"}));

    let (value, json) = round_trip::<fixture::Unions>(r#"{"b": ["x"], "string": "t"}"#);
    assert_eq!(value.named, fixture::UnionsNamed::B(vec!["x".to_owned()]));
    assert_eq!(value.which, fixture::UnionsWhich::Text("t".to_owned()));
    assert_eq!(json, json!({"b": ["x"], "string": "t"}));

    let (value, json) = round_trip::<fixture::Unions>(r#"{"a": 0, "data": "6162"}"#);
    assert_eq!(value.which, fixture::UnionsWhich::Data(b"ab".to_vec()));
    assert_eq!(json, json!({"a": 0, "data": "6162"}));

    let (value, json) = round_trip::<fixture::Unions>(
        r#"{"a": 0, "primitives": {"bool": false, "int8": 0, "int16": 7, "int32": 0,
            "int64": "0", "uint8": 0, "uint16": 0, "uint32": 0, "uint64": "0"}}"#,
    );
    assert_eq!(
        value.which,
        fixture::UnionsWhich::Primitives(fixture::Primitives {
            int16: 7,
            ..Default::default()
        })
    );
    assert_eq!(json, json!({"a": 0, "primitives": {"int16": 7}}));

    // A union member's value is written even when it is the union's default, as C++ writes it.
    let (value, json) = round_trip::<fixture::Unions>(r#"{"a": 0, "number": "0"}"#);
    assert_eq!(value, fixture::Unions::default());
    assert_eq!(json, json!({"a": 0, "number": "0"}));
}

#[test]
fn tagged_unions() {
    let (value, json) = round_trip::<fixture::Tagged>(r#"{"id": 1, "type": "foo"}"#);
    assert_eq!(value.id, 1);
    assert_eq!(value.payload, fixture::TaggedPayload::Foo);
    assert_eq!(json, json!({"id": 1, "type": "foo"}));

    let (value, json) = round_trip::<fixture::Tagged>(
        r#"{"id": 0, "type": "bar", "text": "t",
            "primitives": {"bool": true, "int8": 0, "int16": 0, "int32": 0, "int64": "0",
                           "uint8": 0, "uint16": 0, "uint32": 0, "uint64": "0"}}"#,
    );
    let fixture::TaggedPayload::Bar(bar) = &value.payload else {
        panic!("{value:?}");
    };
    assert_eq!(bar.text.as_deref(), Some("t"));
    assert!(bar.primitives.as_ref().unwrap().bool);
    assert_eq!(
        json,
        json!({"type": "bar", "text": "t", "primitives": {"bool": true}})
    );

    // A flattened member that holds a tagged union of its own.
    let (value, json) = round_trip::<fixture::Tagged>(
        r#"{"id": 0, "type": "etag", "value": "v", "kind": "is-weak"}"#,
    );
    assert_eq!(
        value.payload,
        fixture::TaggedPayload::Etag(fixture::Etag {
            value: Some("v".to_owned()),
            kind: fixture::EtagKind::Weak,
        })
    );
    assert_eq!(
        json,
        json!({"type": "etag", "value": "v", "kind": "is-weak"})
    );

    // A member without fields of its own has its default.
    let value: fixture::Tagged = serde_json::from_str(r#"{"type": "bar"}"#).unwrap();
    assert_eq!(
        value.payload,
        fixture::TaggedPayload::Bar(fixture::Pointers::default())
    );

    // The tag may come anywhere.
    let value: fixture::Tagged =
        serde_json::from_str(r#"{"text": "t", "id": 2, "type": "bar"}"#).unwrap();
    assert_eq!(value.id, 2);
    assert!(matches!(value.payload, fixture::TaggedPayload::Bar(_)));
}

#[test]
fn lists_of_tagged_structs() {
    let (value, json) = round_trip::<fixture::Etags>(
        r#"{"etags": [{"value": "a", "kind": "strong"}, {"kind": "is-weak"}]}"#,
    );
    let etags = value.etags.unwrap();
    assert_eq!(etags[0].value.as_deref(), Some("a"));
    assert_eq!(etags[1].kind, fixture::EtagKind::Weak);
    assert_eq!(
        json,
        json!({"etags": [{"value": "a", "kind": "strong"}, {"kind": "is-weak"}]})
    );
}

#[test]
fn recursive() {
    let (value, json) = round_trip::<fixture::Recursive>(
        r#"{"name": "root", "children": [{"name": "a", "children": [{"name": "b"}]}]}"#,
    );
    let children = value.children.unwrap();
    assert_eq!(
        children[0].children.as_ref().unwrap()[0].name.as_deref(),
        Some("b")
    );
    assert_eq!(
        json,
        json!({"name": "root", "children": [{"name": "a", "children": [{"name": "b"}]}]})
    );
}

#[test]
fn nested_declarations() {
    let value = fixture::Outer::default();
    assert_eq!(value.count, 3);

    let (value, json) = round_trip::<fixture::Outer>(r#"{"inner": {"x": 1}, "count": 0}"#);
    assert_eq!(value.inner, Some(fixture::OuterInner { x: 1 }));
    assert_eq!(json, json!({"inner": {"x": 1}, "count": 0}));

    let (_, json) = round_trip::<fixture::Outer>(r#"{"count": 3}"#);
    assert_eq!(json, json!({}));
}

#[test]
fn unknown_fields_are_ignored() {
    let value: fixture::Primitives =
        serde_json::from_str(r#"{"unknown": 1, "int8": 1, "other": {"a": [1]}}"#).unwrap();
    assert_eq!(value.int8, 1);
    let value: fixture::Tagged =
        serde_json::from_str(r#"{"type": "foo", "unknown": 1, "id": 4}"#).unwrap();
    assert_eq!(value.id, 4);
}

#[test]
fn json_that_only_other_writers_produce_is_rejected() {
    // Decoding follows serde rather than the more lenient C++ codec.
    for json in [
        r#"{"int8": "1"}"#,
        r#"{"int8": 1.0}"#,
        r#"{"int64": " 1"}"#,
        r#"{"int64": "0x10"}"#,
        r#"{"uint64": 1.0}"#,
        r#"{"int8": 128}"#,
        r#"{"int8": 1, "int8": 2}"#,
        r#"{"bool": "true"}"#,
    ] {
        assert!(rejects::<fixture::Primitives>(json), "{json}");
    }
    for json in [
        r#"{"base64": "YW Jj"}"#,
        r#"{"base64": "YWJ"}"#,
        r#"{"hex": "6x"}"#,
    ] {
        assert!(rejects::<fixture::Pointers>(json), "{json}");
    }
    // An externally tagged union needs a member, which is not null.
    for json in ["{}", r#"{"a": 1}"#, r#"{"a": 1, "string": null}"#] {
        assert!(rejects::<fixture::Unions>(json), "{json}");
    }
    // Of more than one member, the first decodes, where C++ takes the last, and the others are
    // ignored, null or not.
    for json in [
        r#"{"a": 1, "number": "1", "string": "s"}"#,
        r#"{"a": 1, "number": "1", "string": null}"#,
    ] {
        let value: fixture::Unions = serde_json::from_str(json).unwrap();
        assert_eq!(value.which, fixture::UnionsWhich::Number(1), "{json}");
    }
    // A repeated key is an error in a struct with flattened unions too.
    assert!(rejects::<fixture::Unions>(
        r#"{"common": "a", "common": "b", "a": 1, "number": "1"}"#
    ));
    // An internally tagged union needs a tag that names a member.
    for json in [
        "{}",
        r#"{"id": 1}"#,
        r#"{"type": "nope"}"#,
        r#"{"type": true}"#,
        r#"{"type": "foo", "type": "foo"}"#,
    ] {
        assert!(rejects::<fixture::Tagged>(json), "{json}");
    }
    // serde takes a member's index for its name, which C++ rejects.
    let value: fixture::Tagged = serde_json::from_str(r#"{"type": 0}"#).unwrap();
    assert_eq!(value.payload, fixture::TaggedPayload::Foo);
    assert!(rejects::<fixture::Primitives>("1"));

    // serde reads a struct without a union from an array of its fields in order, which C++
    // rejects. A struct with a union, which is flattened, needs an object.
    let value: fixture::Primitives = serde_json::from_str("[true, 1]").unwrap();
    assert!(value.bool);
    assert_eq!(value.int8, 1);
    assert!(rejects::<fixture::Unions>(r#"["c", {"a": 1}]"#));
}
