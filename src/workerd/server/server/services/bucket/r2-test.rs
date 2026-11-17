// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(clippy::unwrap_used)]

use super::*;

#[test]
fn range_headers() {
    let from = |offset, length| Some(Range::From { offset, length });
    for (header, parsed) in [
        ("bytes=2-4", from(2, Some(3))),
        (" Bytes = 2 - ", from(2, None)),
        ("bytes=-3", Some(Range::Suffix(3))),
        ("bytes=0-18446744073709551615", from(0, Some(u64::MAX))),
        // Anything else reads all of it, as Miniflare does.
        ("bytes=4-2", None),
        ("bytes=-0", None),
        ("bytes=-", None),
        ("items=0-1", None),
        ("bytes=0-1,3-4", None),
    ] {
        assert_eq!(header_range(header), parsed, "{header}");
    }
}

#[test]
fn records_are_ascii() {
    let req = json!({
        "httpFields": { "contentType": "text/plain", "contentDisposition": "é" },
        "customFields": [{ "k": "ключ", "v": "😀" }],
    });
    let (record, content) = record(&req).ok().unwrap();
    assert_eq!(content.content_type.as_deref(), Some("text/plain"));
    assert_eq!(content.content_disposition, None);
    let [(name, json)] = &envelope(&record)[..] else {
        panic!("one envelope");
    };
    assert_eq!(name, ENVELOPE);
    assert!(json.is_ascii(), "{json}");
    let back: Record = serde_json::from_str(json).unwrap();
    assert_eq!(back.custom["ключ"], "😀");
    assert_eq!(back.http["contentDisposition"], "é");
}
