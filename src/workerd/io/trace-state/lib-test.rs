// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn normalizes_whitespace_and_empty_members() {
    assert_eq!(
        parse_tracestate(b" \ta=one , ,1tenant@vendor=two\t,"),
        "a=one,1tenant@vendor=two"
    );
    assert_eq!(parse_tracestate(b"a= leading space"), "a= leading space");
}

#[test]
fn rejects_invalid_members() {
    let long_key = format!("{}=1", "k".repeat(257));
    let long_value = format!("a={}", "x".repeat(257));
    for invalid in [
        b"A=1".as_slice(),
        b"a b=1",
        b"nokv",
        b"a=1,nokv",
        b"a=one=two",
        b"@b=1",
        b"a@@b=1",
        b"a@abcdefghijklmnop=1",
        long_key.as_bytes(),
        long_value.as_bytes(),
    ] {
        assert_eq!(
            parse_tracestate(invalid),
            "",
            "{}",
            String::from_utf8_lossy(invalid)
        );
    }
}

#[test]
fn values_without_members_are_empty() {
    for empty in [b"".as_slice(), b" , \t", b","] {
        assert_eq!(parse_tracestate(empty), "");
    }
}

#[test]
fn rejects_non_utf8() {
    assert_eq!(parse_tracestate(b"a=\xff"), "");
}

#[test]
fn keeps_the_first_32_members() {
    let members: Vec<String> = (0..33).map(|i| format!("k{i}=v")).collect();
    assert_eq!(
        parse_tracestate(members.join(",").as_bytes()),
        members[..32].join(",")
    );
}
