// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use serde_json::Value;
use serde_json::json;

use super::*;

fn node(kind: &str, name: &str, inner: Vec<String>) -> String {
    // clang-ast expects kind before inner, as in Clang's JSON output.
    format!(
        r#"{{"id":"0x0","kind":{},"name":{},"inner":[{}]}}"#,
        serde_json::to_string(kind).unwrap(),
        serde_json::to_string(name).unwrap(),
        inner.join(","),
    )
}

fn parameters(names: &[&str]) -> Vec<String> {
    names
        .iter()
        .map(|name| node("ParmVarDecl", name, vec![]))
        .collect()
}

fn extract(members: Vec<String>) -> Vec<Value> {
    let ast = node(
        "TranslationUnitDecl",
        "",
        vec![node(
            "NamespaceDecl",
            "workerd",
            vec![node(
                "NamespaceDecl",
                "api",
                vec![node("CXXRecordDecl", "TextDecoder", members)],
            )],
        )],
    );
    let ast: ClangNode = serde_json::from_str(&ast).unwrap();
    let result = serde_json::to_value(get_parameter_names(ast)).unwrap();
    result.as_array().unwrap().clone()
}

#[test]
fn native_constructors_do_not_overwrite_javascript_constructor_names() {
    // TextDecoder's JS constructor includes a leading jsg::Lock argument. Its
    // two native constructors have overlapping parameter indexes, but these
    // must not replace the static constructor's js/label/options names.
    let factory = node(
        "CXXMethodDecl",
        "constructor",
        parameters(&["js", "label", "options"]),
    );
    let native = vec![
        node(
            "CXXConstructorDecl",
            "TextDecoder",
            parameters(&["decoder"]),
        ),
        node(
            "CXXConstructorDecl",
            "TextDecoder",
            parameters(&["decoder", "options"]),
        ),
    ];
    let expected: Vec<Value> = ["js", "label", "options"]
        .iter()
        .enumerate()
        .map(|(index, name)| {
            json!({
                "fully_qualified_parent_name": ["workerd", "api", "TextDecoder"],
                "function_like_name": "constructor",
                "index": index,
                "name": name,
            })
        })
        .collect();
    for factory_first in [true, false] {
        let mut members = native.clone();
        members.insert(
            if factory_first { 0 } else { members.len() },
            factory.clone(),
        );
        assert_eq!(extract(members), expected);
    }
}

#[test]
fn native_constructors_without_a_javascript_factory_are_ignored() {
    assert!(
        extract(vec![node(
            "CXXConstructorDecl",
            "TextDecoder",
            parameters(&["decoder", "options"]),
        )])
        .is_empty()
    );
}

#[test]
fn method_and_function_parameter_names_are_preserved() {
    let result = extract(vec![
        node(
            "CXXMethodDecl",
            "decode",
            parameters(&["js", "input", "options"]),
        ),
        node("FunctionDecl", "helper", parameters(&["value"])),
    ]);
    let names: Vec<(&str, u64, &str)> = result
        .iter()
        .map(|record| {
            (
                record["function_like_name"].as_str().unwrap(),
                record["index"].as_u64().unwrap(),
                record["name"].as_str().unwrap(),
            )
        })
        .collect();
    assert_eq!(
        names,
        vec![
            ("decode", 0, "js"),
            ("decode", 1, "input"),
            ("decode", 2, "options"),
            ("helper", 0, "value")
        ]
    );
}
