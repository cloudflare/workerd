// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn request(version: Version, specifier: &str) -> Request {
    Request {
        version,
        import_type: ImportType::Require,
        specifier: specifier.as_bytes().to_vec(),
        raw_specifier: Some(b"./module.js".to_vec()),
        referrer: b"/worker.js".to_vec(),
        attributes: vec![(b"type".to_vec(), b"json".to_vec())],
    }
}

/// Runs `f` on the module in `bytes`.
fn with_module<R>(bytes: &[u8], f: impl FnOnce(module::Reader<'_>) -> R) -> R {
    let mut slice = bytes;
    let message = capnp::serialize::read_message_from_flat_slice(
        &mut slice,
        capnp::message::ReaderOptions::new(),
    )
    .unwrap();
    f(message.get_root::<module::Reader<'_>>().unwrap())
}

fn which(body: &[u8]) -> String {
    let module = decode_module(body, b"a").unwrap();
    with_module(&module, |m| {
        match m.which().unwrap() {
            module::EsModule(_) => "esModule",
            module::CommonJsModule(_) => "commonJsModule",
            module::Text(_) => "text",
            module::Data(_) => "data",
            module::Wasm(_) => "wasm",
            module::Json(_) => "json",
            module::Obsolete(_) => "obsolete",
            module::PythonModule(_) => "pythonModule",
            module::ObsoletePythonRequirement(_) => "obsoletePythonRequirement",
            module::Bundled(_) => "bundled",
        }
        .to_owned()
    })
}

#[test]
fn v1_is_a_get_with_the_details_in_the_query() {
    let http = encode(&request(Version::V1, "/module.js"));
    assert_eq!(http.method, http::Method::GET);
    assert_eq!(
        http.target,
        "/?specifier=%2Fmodule.js&referrer=%2Fworker.js&rawSpecifier=.%2Fmodule.js"
    );
    assert_eq!(http.resolve_method, Some("require"));
    assert_eq!(http.body, None);
    assert_eq!(http.expected_name, b"module.js");
}

#[test]
fn v1_sends_an_absent_raw_specifier_as_empty() {
    let mut req = request(Version::V1, "/module.js");
    req.raw_specifier = None;
    let http = encode(&req);
    assert!(http.target.ends_with("&rawSpecifier="), "{}", http.target);
}

#[test]
fn v1_sends_only_the_last_segment_of_a_builtin() {
    for (specifier, sent) in [
        ("/bundle/node:fs", "node%3Afs"),
        ("/cloudflare:sockets", "cloudflare%3Asockets"),
        ("/a/workerd:x", "workerd%3Ax"),
    ] {
        let http = encode(&request(Version::V1, specifier));
        assert!(
            http.target.starts_with(&format!("/?specifier={sent}&")),
            "{}",
            http.target
        );
        assert_eq!(
            http.expected_name,
            specifier.rsplit('/').next().unwrap().as_bytes()
        );
    }
}

#[test]
fn www_form_matches_kj() {
    assert_eq!(www_form(b"aZ09-_.*"), "aZ09-_.*");
    assert_eq!(www_form(b"a b"), "a+b");
    assert_eq!(
        www_form(b"~!'()/?&=+%"),
        "%7E%21%27%28%29%2F%3F%26%3D%2B%25"
    );
    assert_eq!(www_form("é".as_bytes()), "%C3%A9");
    assert_eq!(www_form(b"\xff\x00"), "%FF%00");
}

#[test]
fn v2_is_a_post_with_a_json_body() {
    let mut req = request(Version::V2, "file:///bundle/text.txt");
    req.import_type = ImportType::Internal;
    let http = encode(&req);
    assert_eq!(http.method, http::Method::POST);
    assert_eq!(http.target, "/");
    assert_eq!(http.resolve_method, None);
    assert_eq!(
        http.body.as_deref(),
        Some(
            br#"{"type":"internal","specifier":"file:///bundle/text.txt","rawSpecifier":"./module.js","referrer":"/worker.js","attributes":[{"name":"type","value":"json"}]}"#
                .as_slice()
        )
    );
    assert_eq!(http.expected_name, b"file:///bundle/text.txt");
}

#[test]
fn v2_leaves_out_what_is_absent() {
    let mut req = request(Version::V2, "file:///a.js");
    req.import_type = ImportType::Import;
    req.raw_specifier = None;
    req.attributes.clear();
    let http = encode(&req);
    assert_eq!(
        http.body.as_deref(),
        Some(br#"{"type":"import","specifier":"file:///a.js","referrer":"/worker.js"}"#.as_slice())
    );
}

#[test]
fn v2_writes_every_attribute_in_order() {
    let mut req = request(Version::V2, "a");
    req.attributes.push((b"x".to_vec(), b"y".to_vec()));
    let http = encode(&req);
    let body = String::from_utf8(http.body.unwrap()).unwrap();
    assert!(
        body.ends_with(
            r#""attributes":[{"name":"type","value":"json"},{"name":"x","value":"y"}]}"#
        ),
        "{body}"
    );
}

#[test]
fn decodes_each_module_type() {
    let module = decode_module(br#"{"esModule":"export default 1;"}"#, b"a.js").unwrap();
    with_module(&module, |m| {
        assert_eq!(m.get_name().unwrap(), "a.js");
        assert!(matches!(m.which().unwrap(), module::EsModule(Ok(s)) if s == "export default 1;"));
    });
    for kind in [
        "commonJsModule",
        "text",
        "json",
        "obsolete",
        "pythonModule",
        "obsoletePythonRequirement",
        "bundled",
    ] {
        assert_eq!(which(format!(r#"{{"{kind}":"x"}}"#).as_bytes()), kind);
    }

    let module = decode_module(br#"{"data":[0,1,127,255]}"#, b"a").unwrap();
    with_module(&module, |m| {
        assert!(matches!(m.which().unwrap(), module::Data(Ok(d)) if d == [0, 1, 127, 255]));
    });
    let module = decode_module(br#"{"wasm":[0,97,115,109]}"#, b"a").unwrap();
    with_module(&module, |m| {
        assert!(matches!(m.which().unwrap(), module::Wasm(Ok(d)) if d == b"\0asm"));
    });

    let module =
        decode_module(br#"{"commonJsModule":"x","namedExports":["a","b"]}"#, b"a").unwrap();
    with_module(&module, |m| {
        let names: Vec<_> = m
            .get_named_exports()
            .unwrap()
            .iter()
            .map(|n| n.unwrap().to_string().unwrap())
            .collect();
        assert_eq!(names, ["a", "b"]);
    });
}

#[test]
fn text_is_bytes() {
    let module = decode_module(b"{\"text\":\"\xff\\ud800\"}", b"a").unwrap();
    with_module(&module, |m| {
        assert!(
            matches!(m.which().unwrap(), module::Text(Ok(s)) if s.as_bytes() == b"\xff\xed\xa0\x80")
        );
    });
}

#[test]
fn of_two_union_members_the_last_wins() {
    assert_eq!(which(br#"{"text":"t","esModule":"e"}"#), "esModule");
    assert_eq!(which(br#"{"esModule":"e","text":"t"}"#), "text");
}

#[test]
fn unknown_and_null_members_are_ignored() {
    let module = decode_module(
        br#"{"future":{"x":[1]},"name":null,"esModule":"e","text":null}"#,
        b"a",
    )
    .unwrap();
    with_module(&module, |m| {
        assert_eq!(m.get_name().unwrap(), "a");
        assert!(matches!(m.which().unwrap(), module::EsModule(_)));
    });
}

#[test]
fn a_matching_name_is_accepted_and_another_rejected() {
    assert!(decode_module(br#"{"name":"a","text":"t"}"#, b"a").is_ok());
    assert_eq!(
        decode_module(br#"{"name":"b","text":"t"}"#, b"a"),
        Err(DecodeError::NameMismatch {
            name: b"b".to_vec()
        })
    );
    assert!(decode_module(br#"{"name":"","text":"t"}"#, b"a").is_err());
    // The last of two names counts.
    assert!(decode_module(br#"{"name":"b","name":"a","text":"t"}"#, b"a").is_ok());
}

#[test]
fn malformed_modules_are_rejected() {
    for body in [
        &b"{"[..],
        b"[]",
        b"\"text\"",
        br#"{"text":1}"#,
        br#"{"data":"AAEC"}"#,
        br#"{"data":[256]}"#,
        br#"{"data":[1.5]}"#,
        br#"{"data":[-1]}"#,
        br#"{"data":["1"]}"#,
        br#"{"namedExports":"a"}"#,
        br#"{"namedExports":[1]}"#,
        br#"{"namedExports":[null]}"#,
    ] {
        assert!(
            decode_module(body, b"a").is_err(),
            "{}",
            String::from_utf8_lossy(body)
        );
    }
}

#[test]
fn whole_floats_are_bytes() {
    let module = decode_module(br#"{"data":[1.0,2e0,-0]}"#, b"a").unwrap();
    with_module(&module, |m| {
        assert!(matches!(m.which().unwrap(), module::Data(Ok(d)) if d == [1, 2, 0]));
    });
}
