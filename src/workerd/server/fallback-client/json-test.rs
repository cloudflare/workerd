// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn string(input: &[u8]) -> Vec<u8> {
    match parse(input).unwrap() {
        Json::String(s) => s,
        other => panic!("expected a string, got {other:?}"),
    }
}

fn number(input: &str) -> Result<f64, String> {
    match parse(input.as_bytes())? {
        Json::Number(x) => Ok(x),
        other => panic!("expected a number, got {other:?}"),
    }
}

#[test]
fn parses_values() {
    assert_eq!(
        parse(br#" { "a" : [ null , true , false , -1.5e2 , "s" ] , "b" : { } } "#).unwrap(),
        Json::Object(vec![
            (
                b"a".to_vec(),
                Json::Array(vec![
                    Json::Null,
                    Json::Bool(true),
                    Json::Bool(false),
                    Json::Number(-150.0),
                    Json::String(b"s".to_vec()),
                ])
            ),
            (b"b".to_vec(), Json::Object(vec![])),
        ])
    );
}

#[test]
fn keeps_members_in_order_with_duplicates() {
    assert_eq!(
        parse(br#"{"b":1,"a":2,"b":3}"#).unwrap(),
        Json::Object(vec![
            (b"b".to_vec(), Json::Number(1.0)),
            (b"a".to_vec(), Json::Number(2.0)),
            (b"b".to_vec(), Json::Number(3.0)),
        ])
    );
}

#[test]
fn strings_are_bytes() {
    assert_eq!(string(b"\"\xff\xfe\""), b"\xff\xfe");
    // Control characters need no escape.
    assert_eq!(string(b"\"a\nb\tc\x01\""), b"a\nb\tc\x01");
    assert_eq!(string(br#""\"\\\/\b\f\n\r\t""#), b"\"\\/\x08\x0c\n\r\t");
}

#[test]
fn decodes_each_unicode_escape_on_its_own() {
    assert_eq!(string(br#""\u0041\u0000""#), b"A\0");
    assert_eq!(string(br#""\u00e9""#), "é".as_bytes());
    assert_eq!(string(br#""\u20AC""#), "€".as_bytes());
    // A lone surrogate is three bytes; so is each half of a pair.
    assert_eq!(string(br#""\ud800""#), b"\xed\xa0\x80");
    assert_eq!(string(br#""\ud83d\ude00""#), b"\xed\xa0\xbd\xed\xb8\x80");
    assert!(parse(br#""\u12g4""#).is_err());
    assert!(parse(br#""\u12""#).is_err());
    assert!(parse(br#""\x""#).is_err());
}

#[test]
fn a_nul_byte_ends_the_input() {
    assert_eq!(parse(b"{}\0 trailing").unwrap(), Json::Object(vec![]));
    assert!(parse(b"\0{}").is_err());
    assert!(parse(b"\"a\0b\"").is_err());
}

#[test]
fn numbers_follow_strtod() {
    assert_eq!(number("0"), Ok(0.0));
    assert_eq!(number("-0"), Ok(-0.0));
    assert_eq!(number("1."), Ok(1.0));
    assert_eq!(number("1.e2"), Ok(100.0));
    assert_eq!(number("2E+1"), Ok(20.0));
    assert_eq!(number("1e400"), Ok(f64::INFINITY));
    assert!(number("1e").is_err());
    assert!(number("1e+").is_err());
    assert!(parse(b"-").is_err());
    assert!(parse(b"01").is_err());
    assert!(parse(b".5").is_err());
    assert!(parse(b"+1").is_err());
}

#[test]
fn whitespace_is_space_tab_cr_and_lf() {
    assert!(parse(b" \t\r\n{} \t\r\n").is_ok());
    assert!(parse(b"\x0c{}").is_err());
}

#[test]
fn rejects_what_the_codec_rejects() {
    for input in [
        &b""[..],
        b"   ",
        b"nul",
        b"{",
        b"[1,]",
        b"[1 2]",
        b"{\"a\" 1}",
        b"{a:1}",
        b"{} {}",
        b"\"unterminated",
    ] {
        assert!(parse(input).is_err(), "{}", String::from_utf8_lossy(input));
    }
}

#[test]
fn nests_at_most_64_deep() {
    let deep = |n: usize| format!("{}{}", "[".repeat(n), "]".repeat(n));
    assert!(parse(deep(64).as_bytes()).is_ok());
    assert!(parse(deep(65).as_bytes()).is_err());
}

#[test]
fn encodes_strings_as_the_codec_does() {
    let mut out = Vec::new();
    encode_string(b"a\"\\\x08\x0c\n\r\t\x01\x1f\x7f\xff/", &mut out);
    assert_eq!(out, b"\"a\\\"\\\\\\b\\f\\n\\r\\t\\u0001\\u001f\x7f\xff/\"");
}
