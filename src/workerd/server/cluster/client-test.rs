// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(clippy::unwrap_used)]

use super::*;

#[test]
fn http_dates() {
    assert_eq!(
        http_date_ms("Mon, 12 Oct 2026 23:10:55 GMT"),
        Some(1_791_846_655_000)
    );
    assert_eq!(
        http_date_ms("Tue, 29 Feb 2000 00:00:00 GMT"),
        Some(951_782_400_000)
    );
    assert_eq!(http_date_ms("nonsense"), None);
}

#[test]
fn listing_xml() {
    let xml = "<ListBucketResult><IsTruncated>true</IsTruncated>\
        <Contents><Key>a&amp;b</Key><Size>3</Size></Contents>\
        <CommonPrefixes><Prefix>p/</Prefix></CommonPrefixes>\
        <NextContinuationToken>t</NextContinuationToken></ListBucketResult>";
    let keys: Vec<_> = elements(xml, "Contents")
        .map(|c| unescape(elements(c, "Key").next().unwrap()))
        .collect();
    assert_eq!(keys, ["a&b"]);
    let prefixes: Vec<_> = elements(xml, "CommonPrefixes")
        .map(|c| elements(c, "Prefix").next().unwrap())
        .collect();
    assert_eq!(prefixes, ["p/"]);
    assert_eq!(elements(xml, "NextContinuationToken").next(), Some("t"));
}

#[test]
fn sigv4_encoding() {
    assert_eq!(encode("ns/a b+c", false), "ns/a%20b%2Bc");
    assert_eq!(encode("ns/a", true), "ns%2Fa");
}
