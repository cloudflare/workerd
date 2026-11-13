// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests of the types capnpc-rust-serde generates, against the C++ `capnp::JsonCodec`.
//!
//! Each case is a message in Cap'n Proto text format. `capnp convert` encodes it as JSON with the
//! C++ codec (with `handleByAnnotation()`), the generated type decodes that JSON and encodes it
//! again, and `capnp convert` decodes both JSONs, which must decode to the same message. Some cases also check the JSON the Rust types produce, which leaves out fields that
//! hold their defaults as the C++ codec does with `HasMode::NON_DEFAULT`.

#![expect(
    clippy::unwrap_used,
    reason = "the test helpers fail the test when the environment is not as expected"
)]

use std::io::Write as _;
use std::process::Command;
use std::process::Stdio;

use capnp_serde_test_schema as fixture;
use r2_api_serde as r2;
use serde::Serialize;
use serde::de::DeserializeOwned;
use serde_json::Value;
use serde_json::json;

/// The `capnp` tool and a schema file.
struct Schema {
    tool: String,
    include: String,
    path: String,
}

impl Schema {
    fn new(path_env: &str) -> Self {
        let system_include = std::env::var("CAPNP_SYSTEM").unwrap();
        let include = system_include
            .split(' ')
            .find_map(|path| path.strip_suffix("/capnp/c++.capnp"))
            .unwrap()
            .to_owned();
        Self {
            tool: std::env::var("CAPNP_TOOL").unwrap(),
            include,
            path: std::env::var(path_env).unwrap(),
        }
    }

    fn test() -> Self {
        Self::new("TEST_SCHEMA")
    }

    fn r2() -> Self {
        Self::new("R2_SCHEMA")
    }

    /// Runs `capnp convert <conversion>` on `input`, with `ty` the root type.
    fn convert(&self, conversion: &str, ty: &str, input: &str) -> String {
        let mut child = Command::new(&self.tool)
            .args([
                "convert",
                conversion,
                "--no-standard-import",
                &format!("-I{}", self.include),
                &self.path,
                ty,
            ])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .unwrap();
        child
            .stdin
            .take()
            .unwrap()
            .write_all(input.as_bytes())
            .unwrap();
        let output = child.wait_with_output().unwrap();
        assert!(
            output.status.success(),
            "capnp convert {conversion} {ty} failed on {input}:\n{}",
            String::from_utf8_lossy(&output.stderr)
        );
        String::from_utf8(output.stdout).unwrap()
    }

    /// Round-trips the message `text` of type `ty` through the C++ codec's JSON and `T`, and
    /// returns the value and the JSON `T` produces.
    fn round_trip<T: Serialize + DeserializeOwned>(&self, ty: &str, text: &str) -> (T, Value) {
        let cpp_json = self.convert("text:json", ty, text);
        let value: T = serde_json::from_str(&cpp_json)
            .unwrap_or_else(|error| panic!("decoding {cpp_json} as {ty}: {error}"));
        let rust_json = serde_json::to_string(&value).unwrap();

        // Compared with what C++ decodes from its own JSON, rather than with `text`: the codec's
        // JSON does not tell a null union member from an empty one.
        let expected = self.convert("json:text", ty, &cpp_json);
        let actual = self.convert("json:text", ty, &rust_json);
        assert_eq!(
            actual, expected,
            "{ty} {text} changed going through Rust:\nC++ JSON:  {cpp_json}\nRust JSON: {rust_json}"
        );

        // And Rust decodes its own JSON to the same value.
        let again: T = serde_json::from_str(&rust_json).unwrap();
        assert_eq!(serde_json::to_string(&again).unwrap(), rust_json);

        (value, serde_json::from_str(&rust_json).unwrap())
    }
}

/// Runs the plugin on `schema` and returns its error message.
fn plugin_error(schema: &str) -> String {
    let capnp = Schema::test();
    let plugin = std::fs::canonicalize(std::env::var("PLUGIN").unwrap()).unwrap();
    let dir = std::path::PathBuf::from(std::env::var("TEST_TMPDIR").unwrap()).join("rejected");
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("rejected.capnp");
    std::fs::write(
        &path,
        format!(
            "@0xd3c1e5a7b9f20481;\nusing Json = import \"/capnp/compat/json.capnp\";\n{schema}"
        ),
    )
    .unwrap();
    let output = Command::new(&capnp.tool)
        .arg("compile")
        .arg("--no-standard-import")
        .arg(format!("-I{}", capnp.include))
        .arg(format!("--src-prefix={}", dir.display()))
        .arg(format!("-o{}:{}", plugin.display(), dir.display()))
        .arg(&path)
        .output()
        .unwrap();
    assert!(!output.status.success(), "the plugin accepted {schema}");
    String::from_utf8(output.stderr).unwrap()
}

#[test]
fn rejected_schemas() {
    for (schema, error) in [
        (
            "struct S { g :group $Json.flatten(prefix = \"p.\") { a @0 :Text; } }",
            "$Json.flatten(prefix = \"p.\") has no serde equivalent",
        ),
        (
            "struct S { union { a @0 :T $Json.flatten(); b @1 :Text; } } struct T { x @0 :Text; }",
            "union member `a` is flattened, which needs $Json.discriminator",
        ),
        (
            "struct S { a @0 :Text $Json.flatten(); }",
            "only struct fields can be flattened",
        ),
        (
            "struct S { a @0 :AnyPointer; }",
            "AnyPointer has no JSON representation",
        ),
        (
            "struct S { a @0 :I; } interface I {}",
            "interfaces have no JSON representation",
        ),
        (
            "struct OtherA {} struct Other { struct A {} }",
            "more than one item would be named `OtherA`",
        ),
        (
            "struct S { s @0 :S $Json.flatten(); }",
            "rejected.capnp:S is flattened into itself",
        ),
        (
            "struct S { t @0 :T $Json.flatten(); } struct T { g :group $Json.flatten() { s @0 :S \
             $Json.flatten(); } }",
            "is flattened into itself",
        ),
        (
            "struct S $Json.discriminator(name = \"t\") { union { s @0 :S $Json.flatten(); \
             n @1 :Void; } }",
            "rejected.capnp:S is flattened into itself",
        ),
    ] {
        let message = plugin_error(schema);
        assert!(
            message.contains(error),
            "{schema}: expected {error:?} in:\n{message}"
        );
    }
}

#[test]
fn constants() {
    assert_eq!(fixture::ANSWER, -42);
    assert_eq!(fixture::GREETING, "hello \"world\"");
    assert!((fixture::PI - 3.25).abs() < f64::EPSILON);
    assert_eq!(fixture::BLOB, &[1, 2]);
    assert_eq!(fixture::FAVORITE, fixture::Color::DarkBlue);
    assert_eq!(fixture::OUTER_LIMIT, 7);
}

#[test]
fn primitives() {
    let schema = Schema::test();
    let (value, json) = schema.round_trip::<fixture::Primitives>(
        "Primitives",
        "(void = void, bool = true, int8 = -8, int16 = -16, int32 = -32, int64 = -64, uint8 = 8, \
         uint16 = 16, uint32 = 32, uint64 = 18446744073709551615, float32 = 1.5, float64 = -2.25, \
         color = darkBlue)",
    );
    assert_eq!(value.uint64, u64::MAX);
    assert_eq!(value.color, fixture::Color::DarkBlue);
    assert_eq!(
        json,
        json!({"bool": true, "int8": -8, "int16": -16, "int32": -32, "int64": "-64", "uint8": 8,
               "uint16": 16, "uint32": 32, "uint64": "18446744073709551615", "float32": 1.5,
               "float64": -2.25, "color": "blue"})
    );

    let (_, json) = schema.round_trip::<fixture::Primitives>("Primitives", "()");
    assert_eq!(json, json!({}));

    let (value, json) = schema.round_trip::<fixture::Primitives>(
        "Primitives",
        "(float32 = inf, float64 = nan, color = lightGreen)",
    );
    assert!(value.float64.is_nan());
    assert_eq!(
        json,
        json!({"float32": "Infinity", "float64": "NaN", "color": "lightGreen"})
    );
}

#[test]
fn defaults() {
    let schema = Schema::test();
    let value = fixture::Defaults::default();
    assert!(value.bool);
    assert_eq!(value.int8, -8);
    assert_eq!(value.int64, -64);
    assert_eq!(value.uint64, u64::MAX);
    assert!((value.float32 - 1.5).abs() < f32::EPSILON);
    assert!(value.float64.is_infinite() && value.float64.is_sign_positive());
    assert!(value.nan.is_nan());
    assert_eq!(value.color, fixture::Color::LightGreen);

    let (_, json) = schema.round_trip::<fixture::Defaults>("Defaults", "()");
    assert_eq!(json, json!({}));

    let (_, json) = schema.round_trip::<fixture::Defaults>(
        "Defaults",
        "(bool = false, int8 = 0, int64 = 0, uint64 = 0, float32 = 0, float64 = 0, nan = 0, \
         color = red)",
    );
    assert_eq!(
        json,
        json!({"bool": false, "int8": 0, "int64": "0", "uint64": "0", "float32": 0.0,
               "float64": 0.0, "nan": 0.0, "color": "red"})
    );

    // A missing field takes the schema default.
    let value: fixture::Defaults = serde_json::from_value(json!({"int8": 3})).unwrap();
    assert_eq!(value.int8, 3);
    assert_eq!(value.uint64, u64::MAX);
}

#[test]
fn pointers() {
    let schema = Schema::test();
    let (value, json) = schema.round_trip::<fixture::Pointers>(
        "Pointers",
        r#"(text = "text", data = "abc", base64 = "abc", hex = "abc", ints = [-1, 2],
            bytes = [0, 255], datas = ["a", ""], texts = ["x", "y"], colors = [red, darkBlue],
            structs = [(int8 = 1), ()], nested = [[1.5], []], primitives = (uint32 = 5),
            json = (object = [(name = "k", value = (array = [(number = 1), (null = void)]))]),
            renamed = "renamed", self = "self", match = "match")"#,
    );
    assert_eq!(value.self_.as_deref(), Some("self"));
    assert_eq!(value.r#match.as_deref(), Some("match"));
    assert_eq!(
        json,
        json!({"text": "text", "data": [97, 98, 99], "base64": "YWJj", "hex": "616263",
               "ints": ["-1", "2"], "bytes": [0, 255], "datas": [[97], []], "texts": ["x", "y"],
               "colors": ["red", "blue"], "structs": [{"int8": 1}, {}], "nested": [[1.5], []],
               "primitives": {"uint32": 5}, "json": {"k": [1, null]},
               "not-a-rust-identifier": "renamed", "self": "self", "match": "match"})
    );

    let (value, json) = schema.round_trip::<fixture::Pointers>("Pointers", "()");
    assert_eq!(value, fixture::Pointers::default());
    assert_eq!(json, json!({}));

    // null is a null pointer, as in C++.
    let value: fixture::Pointers =
        serde_json::from_value(json!({"text": null, "hex": null, "ints": null})).unwrap();
    assert_eq!(value, fixture::Pointers::default());
}

#[test]
fn unions() {
    let schema = Schema::test();
    let (value, json) = schema.round_trip::<fixture::Unions>(
        "Unions",
        r#"(common = "c", number = 5, plain = (b = void), tagged = (x = 3), flat = (p = "pee"),
            grouped = (inGroup = "in", g2 = void), flatGroup = (flatMember = 9))"#,
    );
    assert_eq!(value.which, fixture::UnionsWhich::Number(5));
    assert_eq!(value.plain, fixture::UnionsPlain::B(()));
    assert_eq!(value.tagged, fixture::UnionsTagged::X { value: 3 });
    assert_eq!(
        value.flat,
        fixture::UnionsFlat::P {
            value: "pee".to_owned()
        }
    );
    assert_eq!(value.grouped.which, fixture::UnionsGroupedWhich::G2(()));
    assert_eq!(value.flat_group.flat_member, 9);
    assert_eq!(
        json,
        json!({"common": "c", "plain": {"b": null}, "tagged": {"kind": "x", "x": 3},
               "flat": "p", "value": "pee", "grouped": {"inGroup": "in", "g2": null},
               "flatMember": 9, "number": "5"})
    );

    let (value, json) = schema.round_trip::<fixture::Unions>(
        "Unions",
        r#"(text = "t", plain = (a = -1), tagged = (y = void), flat = (q = -inf),
            grouped = (g1 = "one"))"#,
    );
    assert_eq!(value.which, fixture::UnionsWhich::Text("t".to_owned()));
    assert_eq!(
        json,
        json!({"plain": {"a": -1}, "tagged": {"kind": "y"}, "flat": "renamed-q",
               "value": "-Infinity", "grouped": {"g1": "one"}, "string": "t"})
    );

    let (value, json) = schema
        .round_trip::<fixture::Unions>("Unions", r#"(data = "ab", tagged = (z = (int16 = 7)))"#);
    assert_eq!(value.which, fixture::UnionsWhich::Data(b"ab".to_vec()));
    assert_eq!(
        value.tagged,
        fixture::UnionsTagged::Z(fixture::Primitives {
            int16: 7,
            ..Default::default()
        })
    );
    assert_eq!(json["tagged"], json!({"kind": "z", "int16": 7}));
    assert_eq!(json["data"], json!("6162"));

    let (value, json) = schema.round_trip::<fixture::Unions>("Unions", "()");
    assert_eq!(value, fixture::Unions::default());
    assert_eq!(json["none"], Value::Null);
}

#[test]
fn discriminated_unions() {
    let schema = Schema::test();
    let (value, json) = schema.round_trip::<fixture::Discriminated>("Discriminated", "(foo = 4)");
    assert_eq!(value.which, fixture::DiscriminatedWhich::Foo { value: 4 });
    assert_eq!(json, json!({"type": "foo", "foo": 4}));

    let (_, json) = schema.round_trip::<fixture::Discriminated>(
        "Discriminated",
        r#"(bar = (text = "t", primitives = (bool = true)))"#,
    );
    assert_eq!(
        json,
        json!({"type": "bar", "text": "t", "primitives": {"bool": true}})
    );

    let (value, json) = schema
        .round_trip::<fixture::Discriminated>("Discriminated", r#"(baz = (bazMember = "b"))"#);
    assert_eq!(
        value.which,
        fixture::DiscriminatedWhich::Baz(fixture::DiscriminatedBaz {
            baz_member: Some("b".to_owned())
        })
    );
    assert_eq!(json, json!({"type": "baz", "bazMember": "b"}));

    let (_, json) = schema.round_trip::<fixture::Discriminated>("Discriminated", "(qux = void)");
    assert_eq!(json, json!({"type": "qux"}));
}

#[test]
fn tag_after_members() {
    // C++ writes the tag first, but it may come anywhere: the keys before it wait for it.
    let value: fixture::Discriminated =
        serde_json::from_str(r#"{"text": "t", "x": 1, "primitives": {"int8": 2}, "type": "bar"}"#)
            .unwrap();
    let fixture::DiscriminatedWhich::Bar(bar) = &value.which else {
        panic!("{value:?}");
    };
    assert_eq!(bar.text.as_deref(), Some("t"));
    assert_eq!(bar.primitives.as_ref().unwrap().int8, 2);

    let value: fixture::Discriminated =
        serde_json::from_str(r#"{"foo": 4, "bazMember": "b", "type": "foo"}"#).unwrap();
    assert_eq!(value.which, fixture::DiscriminatedWhich::Foo { value: 4 });

    let value: fixture::Discriminated =
        serde_json::from_str(r#"{"bazMember": "b", "type": "baz", "foo": 4}"#).unwrap();
    assert_eq!(
        value.which,
        fixture::DiscriminatedWhich::Baz(fixture::DiscriminatedBaz {
            baz_member: Some("b".to_owned())
        })
    );

    let value: fixture::Discriminated =
        serde_json::from_str(r#"{"foo": 4, "type": "qux"}"#).unwrap();
    assert_eq!(value.which, fixture::DiscriminatedWhich::Qux);

    // A union flattened into a struct, with keys of the struct's own between.
    let value: fixture::Unions = serde_json::from_str(
        r#"{"value": "pee", "common": "c", "tagged": {"int16": 7, "kind": "z"}, "flat": "p",
            "none": null}"#,
    )
    .unwrap();
    assert_eq!(value.common.as_deref(), Some("c"));
    assert_eq!(
        value.flat,
        fixture::UnionsFlat::P {
            value: "pee".to_owned()
        }
    );
    assert_eq!(
        value.tagged,
        fixture::UnionsTagged::Z(fixture::Primitives {
            int16: 7,
            ..Default::default()
        })
    );
}

#[test]
fn tagged_unions() {
    // A member without its value has its default, as in C++.
    let value: fixture::Discriminated = serde_json::from_str(r#"{"type": "foo"}"#).unwrap();
    assert_eq!(value.which, fixture::DiscriminatedWhich::Foo { value: 0 });

    // The tag must be a member's name, once.
    for json in [
        r#"{"type": 0}"#,
        r#"{"type": "nope"}"#,
        r#"{"type": "foo", "type": "foo"}"#,
        r#"{"foo": 4}"#,
        "{}",
        "[]",
    ] {
        assert!(
            serde_json::from_str::<fixture::Discriminated>(json).is_err(),
            "{json}"
        );
    }
}

#[test]
fn duplicate_keys() {
    // The last value wins, as in C++.
    let value: fixture::Unions = serde_json::from_str(
        r#"{"common": "a", "common": "b", "number": "1", "string": "s",
            "plain": {"a": 1, "zz": 2, "b": null}, "flat": "p"}"#,
    )
    .unwrap();
    assert_eq!(value.common.as_deref(), Some("b"));
    assert_eq!(value.which, fixture::UnionsWhich::Text("s".to_owned()));
    assert_eq!(value.plain, fixture::UnionsPlain::B(()));

    // A union without a member is an error.
    assert!(
        serde_json::from_str::<fixture::Unions>(r#"{"plain": {}, "none": null, "flat": "p"}"#)
            .is_err()
    );
    assert!(serde_json::from_str::<fixture::Unions>(r#"{"flat": "p"}"#).is_err());
}

#[test]
fn boxed() {
    let schema = Schema::test();
    let (value, json) = schema.round_trip::<fixture::Boxed>(
        "Boxed",
        r#"(inner = (name = "n", outer = (none = void)), member = (label = "l"))"#,
    );
    assert_eq!(value.inner.unwrap().name.as_deref(), Some("n"));
    assert_eq!(
        json,
        json!({"name": "n", "outer": {"kind": "none"}, "kind": "member", "label": "l"})
    );
}

#[test]
fn recursive() {
    let schema = Schema::test();
    let (value, json) = schema.round_trip::<fixture::Recursive>(
        "Recursive",
        r#"(name = "root", child = (name = "child", cycle = (other = (name = "other"))),
            children = [(name = "a"), (name = "b")])"#,
    );
    let child = value.child.unwrap();
    assert_eq!(child.name.as_deref(), Some("child"));
    let fixture::RecursiveCycle::Other(other) = &child.cycle else {
        panic!("expected other");
    };
    assert_eq!(other.name.as_deref(), Some("other"));
    assert_eq!(
        json,
        json!({"name": "root",
               "child": {"name": "child",
                         "cycle": {"other": {"name": "other", "cycle": {"none": null}}}},
               "children": [{"name": "a", "cycle": {"none": null}},
                            {"name": "b", "cycle": {"none": null}}],
               "cycle": {"none": null}})
    );
}

#[test]
fn flattened() {
    let schema = Schema::test();
    let (value, json) = schema.round_trip::<fixture::Flattened>(
        "Flattened",
        r#"(before = "b", inner = (int32 = 5, color = darkBlue), after = "a")"#,
    );
    assert_eq!(value.inner.unwrap().int32, 5);
    assert_eq!(
        json,
        json!({"before": "b", "int32": 5, "color": "blue", "after": "a"})
    );
}

#[test]
fn nested_declarations() {
    let schema = Schema::test();
    let value = fixture::Outer::default();
    assert_eq!(value.mode, fixture::OuterMode::Off);

    let (value, json) =
        schema.round_trip::<fixture::Outer>("Outer", "(inner = (x = 1), mode = on)");
    assert_eq!(value.inner, Some(fixture::OuterInner { x: 1 }));
    assert_eq!(json, json!({"inner": {"x": 1}, "mode": "on"}));

    let (_, json) = schema.round_trip::<fixture::Outer>("Outer", "(mode = off)");
    assert_eq!(json, json!({}));
}

#[test]
fn lenient_decoding() {
    // Integers as strings, 64-bit integers as numbers, and enums as ordinals, as the C++ codec
    // accepts.
    let value: fixture::Primitives = serde_json::from_value(
        json!({"int8": "-3", "int64": 5, "uint64": 6.0, "color": 2, "float64": null}),
    )
    .unwrap();
    assert_eq!(value.int8, -3);
    assert_eq!(value.int64, 5);
    assert_eq!(value.uint64, 6);
    assert_eq!(value.color, fixture::Color::LightGreen);
    assert!(value.float64.is_nan());

    // Unknown fields are ignored.
    let value: fixture::Primitives = serde_json::from_value(json!({"unknown": 1})).unwrap();
    assert_eq!(value.int8, 0);
}

#[test]
fn r2_requests() {
    let schema = Schema::r2();
    let (value, json) = schema.round_trip::<r2::R2BindingRequest>(
        "R2BindingRequest",
        r#"(version = 1, payload = (get = (object = "foo", range = (offset = 5),
            onlyIf = (etagMatches = [(value = "x", type = (weak = void))], uploadedBefore = 10),
            ssec = (key = "k"))))"#,
    );
    let r2::R2BindingRequestPayload::Get(get) = &value.payload else {
        panic!("expected get");
    };
    assert_eq!(get.object.as_deref(), Some("foo"));
    assert_eq!(
        json,
        json!({"version": 1, "method": "get", "object": "foo", "range": {"offset": "5"},
               "onlyIf": {"etagMatches": [{"value": "x", "type": "weak"}],
                          "uploadedBefore": "10"},
               "ssec": {"key": "k"}})
    );

    let (_, json) = schema.round_trip::<r2::R2BindingRequest>(
        "R2BindingRequest",
        r#"(version = 1, payload = (put = (object = "o", md5 = "abc", sha256 = "abc",
            customFields = [(k = "a", v = "b")],
            httpFields = (contentType = "text/plain", cacheExpiry = 1000),
            storageClass = "Standard")))"#,
    );
    assert_eq!(
        json,
        json!({"version": 1, "method": "put", "object": "o",
               "customFields": [{"k": "a", "v": "b"}],
               "httpFields": {"contentType": "text/plain", "cacheExpiry": "1000"},
               "md5": "YWJj", "sha256": "616263", "storageClass": "Standard"})
    );

    let (_, json) = schema.round_trip::<r2::R2BindingRequest>(
        "R2BindingRequest",
        r#"(version = 1, payload = (list = (limit = 10, prefix = "p/", delimiter = "/",
            include = [0, 1], newRuntime = true)))"#,
    );
    assert_eq!(
        json,
        json!({"version": 1, "method": "list", "limit": 10, "prefix": "p/", "delimiter": "/",
               "include": [0, 1], "newRuntime": true})
    );

    let (value, json) = schema.round_trip::<r2::R2BindingRequest>(
        "R2BindingRequest",
        r#"(version = 1, payload = (delete = (objects = ["a", "b"])))"#,
    );
    let r2::R2BindingRequestPayload::Delete(delete) = &value.payload else {
        panic!("expected delete");
    };
    assert_eq!(
        delete.which,
        r2::R2DeleteRequestWhich::Objects(vec!["a".to_owned(), "b".to_owned()])
    );
    assert_eq!(
        json,
        json!({"version": 1, "method": "delete", "objects": ["a", "b"]})
    );

    let (_, json) = schema.round_trip::<r2::R2BindingRequest>(
        "R2BindingRequest",
        r#"(version = 1, payload = (completeMultipartUpload = (object = "o", uploadId = "u",
            parts = [(etag = "e1", part = 1), (etag = "e2", part = 2)])))"#,
    );
    assert_eq!(
        json,
        json!({"version": 1, "method": "completeMultipartUpload", "object": "o",
               "uploadId": "u", "parts": [{"etag": "e1", "part": 1}, {"etag": "e2", "part": 2}]})
    );

    for payload in [
        r#"head = (object = "o")"#,
        r#"createBucket = (bucket = "b")"#,
        r#"listBucket = (limit = 5, prefix = "p", cursor = "c")"#,
        r#"deleteBucket = (bucket = "b")"#,
        r#"createMultipartUpload = (object = "o", storageClass = "InfrequentAccess")"#,
        r#"uploadPart = (object = "o", uploadId = "u", partNumber = 3)"#,
        r#"abortMultipartUpload = (object = "o", uploadId = "u")"#,
    ] {
        schema.round_trip::<r2::R2BindingRequest>(
            "R2BindingRequest",
            &format!("(version = 1, payload = ({payload}))"),
        );
    }
}

#[test]
fn r2_request_as_rust_builds_it() {
    let request = r2::R2BindingRequest {
        version: r2::VERSION_PUBLIC_BETA,
        payload: r2::R2BindingRequestPayload::Head(r2::R2HeadRequest {
            object: Some("key".to_owned()),
        }),
    };
    assert_eq!(
        serde_json::to_value(&request).unwrap(),
        json!({"version": 1, "method": "head", "object": "key"})
    );
}

#[test]
fn r2_responses() {
    let schema = Schema::r2();
    let (value, json) = schema.round_trip::<r2::R2HeadResponse>(
        "R2HeadResponse",
        r#"(name = "n", version = "v", size = 12345678901, etag = "e",
            uploadedMillisecondsSinceEpoch = 1700000000000,
            httpFields = (contentType = "a/b"), customFields = [(k = "k", v = "v")],
            range = (offset = 1, length = 2), checksums = (md5 = "ab", sha1 = "cd"),
            storageClass = "Standard", ssec = (algorithm = "AES256", keyMd5 = "m"))"#,
    );
    assert_eq!(value.size, 12_345_678_901);
    assert_eq!(value.uploaded_milliseconds_since_epoch, 1_700_000_000_000);
    assert_eq!(value.checksums.unwrap().md5, Some(b"ab".to_vec()));
    assert_eq!(
        json,
        json!({"name": "n", "version": "v", "size": "12345678901", "etag": "e",
               "uploaded": "1700000000000", "httpFields": {"contentType": "a/b"},
               "customFields": [{"k": "k", "v": "v"}], "range": {"offset": "1", "length": "2"},
               "checksums": {"0": "6162", "1": "6364"}, "storageClass": "Standard",
               "ssec": {"algorithm": "AES256", "keyMd5": "m"}})
    );

    // The R2 service writes 64-bit integers as numbers.
    let value: r2::R2HeadResponse =
        serde_json::from_value(json!({"name": "n", "size": 5, "uploaded": 1_700_000_000_000_u64}))
            .unwrap();
    assert_eq!(value.size, 5);
    assert_eq!(value.uploaded_milliseconds_since_epoch, 1_700_000_000_000);

    schema.round_trip::<r2::R2ListResponse>(
        "R2ListResponse",
        r#"(objects = [(name = "a", size = 1), (name = "b")], truncated = true, cursor = "c",
            delimitedPrefixes = ["p/"])"#,
    );
    schema.round_trip::<r2::R2ListBucketResponse>(
        "R2ListBucketResponse",
        r#"(buckets = [(name = "b", createdMillisecondsSinceEpoch = 5)], truncated = false)"#,
    );
    schema.round_trip::<r2::R2ErrorResponse>(
        "R2ErrorResponse",
        r#"(version = 1, v4code = 10007, message = "The specified key does not exist.")"#,
    );
    schema.round_trip::<r2::R2CreateMultipartUploadResponse>(
        "R2CreateMultipartUploadResponse",
        r#"(uploadId = "u", ssec = (algorithm = "AES256"))"#,
    );
    schema.round_trip::<r2::R2UploadPartResponse>("R2UploadPartResponse", r#"(etag = "e")"#);
    schema.round_trip::<r2::R2DeleteResponse>("R2DeleteResponse", "()");
}
