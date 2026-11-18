// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(clippy::unwrap_used)]

use super::*;

fn refused<T>(answer: Answer<T>) -> (u16, String) {
    match answer {
        Err(Fail::Refuse { status, code, .. }) => (status, code),
        _ => panic!("not refused"),
    }
}

#[test]
fn request_targets() {
    for (url, path) in [
        ("http://b/k?list", "/k?list"),
        ("http://b", ""),
        ("/a/b://c", "/a/b://c"),
        ("/?list&prefix=http://x", "/?list&prefix=http://x"),
    ] {
        assert_eq!(target(url), path, "{url}");
    }
}

#[test]
fn ranges() {
    let from = |offset, length| Range::From { offset, length };
    for (header, parsed) in [
        ("bytes=2-4", from(2, Some(3))),
        ("bytes=2-2", from(2, Some(1))),
        ("bytes=2-", from(2, None)),
        ("bytes=-3", Range::Suffix(3)),
    ] {
        assert_eq!(range(header).ok(), Some(parsed), "{header}");
    }
    for header in [
        "bytes=4-2",
        "bytes=-",
        "items=0-1",
        "bytes=a-b",
        "bytes=0-1,3-4",
    ] {
        assert_eq!(refused(range(header)).0, 400, "{header}");
    }
}

#[test]
fn guards_and_metadata() {
    let map = |pairs: &[(&'static str, &[u8])]| {
        let mut map = http::HeaderMap::new();
        for (name, value) in pairs {
            let value = http::HeaderValue::from_bytes(value).unwrap();
            map.append(*name, value);
        }
        map
    };
    assert!(guard(&map(&[])).ok().unwrap().is_none());
    let absent = guard(&map(&[("if-none-match", b"*")])).ok().unwrap();
    assert!(matches!(absent, Some(Guard::Absent)));
    let matched = guard(&map(&[("if-match", b"\"v\"")])).ok().unwrap();
    assert!(matches!(matched, Some(Guard::Match(v)) if v.as_str() == "\"v\""));
    assert_eq!(refused(guard(&map(&[("if-none-match", b"\"v\"")]))).0, 400);

    let headers = map(&[
        ("content-type", b"text/plain"),
        ("x-meta-b", b"2"),
        ("x-meta-a", b"1"),
        ("x-other", b"x"),
    ]);
    let (content, metadata) = described(&headers).ok().unwrap();
    assert_eq!(content.content_type.as_deref(), Some("text/plain"));
    let names: Vec<_> = metadata.iter().map(|(n, v)| (&n[..], &v[..])).collect();
    assert_eq!(names, [("b", "2"), ("a", "1")]);
    let accented = map(&[("x-meta-a", "\u{e9}".as_bytes())]);
    assert_eq!(refused(described(&accented)).0, 400);
}
