// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::time::Duration;

use super::*;

/// A time in whole seconds, as HTTP dates are.
const NOW: u64 = 1_700_000_000_000;

fn date(ms: u64) -> String {
    httpdate::fmt_http_date(UNIX_EPOCH + Duration::from_millis(ms))
}

fn headers(pairs: &[(&str, &str)]) -> HeaderMap {
    pairs
        .iter()
        .map(|&(name, value)| {
            let name = HeaderName::from_bytes(name.as_bytes()).unwrap();
            (name, HeaderValue::from_str(value).unwrap())
        })
        .collect()
}

/// The lifetime of a 200 with `pairs`, put at `NOW` without `Authorization`.
fn ttl(pairs: &[(&str, &str)]) -> Option<u64> {
    lifetime(200, &mut headers(pairs), false, NOW)
}

#[test]
fn a_lifetime_is_s_maxage_or_max_age_less_age() {
    assert_eq!(ttl(&[("cache-control", "max-age=3600")]), Some(3_600_000));
    assert_eq!(
        ttl(&[("cache-control", "s-maxage=1, max-age=10")]),
        Some(1000)
    );
    assert_eq!(
        ttl(&[("cache-control", "max-age=10"), ("age", "4")]),
        Some(6000)
    );
    assert_eq!(
        ttl(&[("cache-control", "max-age=10"), ("age", "40")]),
        Some(0)
    );
    assert_eq!(ttl(&[("cache-control", "max-age=\"10s\"")]), Some(10_000));
    assert_eq!(ttl(&[("cache-control", "max-age")]), Some(0));
    assert_eq!(ttl(&[("cache-control", "max-age=60, no-cache")]), Some(0));
}

#[test]
fn a_lifetime_is_expires_less_date_else_a_tenth_since_last_modified() {
    let at = |ms| date(NOW - ms);
    assert_eq!(
        ttl(&[("date", &at(5000)), ("expires", &at(3000))]),
        Some(2000)
    );
    // Without a Date, from when it was put.
    assert_eq!(ttl(&[("expires", &date(NOW + 2000))]), Some(2000));
    assert_eq!(ttl(&[("expires", &at(2000))]), Some(0));
    assert_eq!(ttl(&[("expires", "0")]), Some(0));
    let (now, modified) = (date(NOW), at(20_000));
    assert_eq!(
        ttl(&[("date", &now), ("last-modified", &modified)]),
        Some(2000)
    );
    assert_eq!(ttl(&[("last-modified", &modified)]), Some(2000));
    let immutable = ("cache-control", "immutable");
    assert_eq!(
        ttl(&[("last-modified", &modified), immutable]),
        Some(DAY_MS)
    );
    // No freshness information: a 200 is stored for no time.
    assert_eq!(ttl(&[]), Some(0));
    let expires = date(NOW + 2000);
    assert_eq!(
        ttl(&[("pragma", "no-cache"), ("expires", &expires)]),
        Some(0)
    );
}

#[test]
fn some_responses_are_not_stored() {
    let hour = ("cache-control", "max-age=3600");
    assert_eq!(ttl(&[("cache-control", "private")]), None);
    assert_eq!(ttl(&[("cache-control", "no-store, max-age=60")]), None);
    assert_eq!(ttl(&[hour, ("set-cookie", "key=value")]), None);
    // A status http-cache-semantics does not know, or does not store without freshness
    // information.
    let status = |status, pairs: &[(&str, &str)]| lifetime(status, &mut headers(pairs), false, NOW);
    assert_eq!(status(500, &[hour]), None);
    assert_eq!(status(206, &[]), None);
    assert_eq!(status(302, &[hour]), Some(3_600_000));
    assert_eq!(status(404, &[]), Some(0));
    // A put with Authorization, unless the response is public or similar.
    assert_eq!(lifetime(200, &mut headers(&[hour]), true, NOW), None);
    let public = ("cache-control", "public, max-age=60");
    assert_eq!(
        lifetime(200, &mut headers(&[public]), true, NOW),
        Some(60_000)
    );
}

#[test]
fn stored_headers_drop_hop_by_hop_ones_and_private_cookies() {
    let mut stored = headers(&[
        ("cache-control", "max-age=60, private=set-cookie"),
        ("set-cookie", "key=value"),
        ("transfer-encoding", "chunked"),
        ("connection", "X-Hop"),
        ("x-hop", "1"),
        ("x-kept", "2"),
    ]);
    assert_eq!(lifetime(200, &mut stored, false, NOW), Some(60_000));
    let kept = [("cache-control", "max-age=60"), ("x-kept", "2")];
    assert_eq!(stored, headers(&kept));
    let mut stored = headers(&[
        ("cache-control", "private=set-cookie"),
        ("expires", &date(NOW)),
    ]);
    assert_eq!(lifetime(200, &mut stored, false, NOW), Some(0));
    assert!(!stored.contains_key(header::CACHE_CONTROL));
}

#[test]
fn ranges_parse_as_miniflare_parses_them() {
    let ranges = |header| parse_ranges(header, 10);
    assert_eq!(ranges("bytes=2-4"), Some(vec![(2, 4)]));
    assert_eq!(ranges("bytes=-3"), Some(vec![(7, 9)]));
    assert_eq!(ranges("bytes=5-"), Some(vec![(5, 9)]));
    assert_eq!(ranges("bytes=0-100"), Some(vec![(0, 9)]));
    assert_eq!(ranges("bytes=1-3,5-6"), Some(vec![(1, 3), (5, 6)]));
    assert_eq!(ranges(" BYTES = 1 - 3 , 5-"), Some(vec![(1, 3), (5, 9)]));
    // The whole body.
    for header in ["bytes=", "bytes=-10", "bytes=-0"] {
        assert_eq!(ranges(header), Some(vec![]), "{header}");
    }
    let unsatisfiable = [
        "bytes=15-",
        "bytes=10-",
        "bytes=4-2",
        "bytes=-",
        "bytes=a-b",
        "bytes=+1-2",
        "items=0-1",
        "0-1",
    ];
    for header in unsatisfiable {
        assert_eq!(ranges(header), None, "{header}");
    }
    assert_eq!(parse_ranges("bytes=0-", 0), None);
}

#[test]
fn conditionals_match_weak_etags_and_imf_dates() {
    let stored = headers(&[
        ("etag", "\"thing\""),
        ("last-modified", "Tue, 13 Sep 2022 12:00:00 GMT"),
    ]);
    let if_none_match = |value| not_modified(Some(value), None, &stored);
    assert!(if_none_match("\"thing\""));
    assert!(if_none_match("   W/\"thing\"      "));
    assert!(!if_none_match("\"not the thing\""));
    assert!(if_none_match(
        "\"not the thing\",    \"thing\"    , W/\"still not\""
    ));
    assert!(if_none_match("*"));
    assert!(if_none_match("    *   "));
    assert!(!not_modified(Some("*"), None, &HeaderMap::new()));
    let if_modified_since = |value| not_modified(None, Some(value), &stored);
    assert!(!if_modified_since("Tue, 13 Sep 2022 11:00:00 GMT"));
    assert!(if_modified_since("Tue, 13 Sep 2022 12:00:00 GMT"));
    assert!(if_modified_since("Tue, 13 Sep 2022 13:00:00 GMT"));
    assert!(!if_modified_since("13 Sep 2022 13:00:00 GMT"));
}

#[test]
fn heads_parse_strictly() {
    let ok = (200, "OK".to_owned(), headers(&[("a", "b"), ("c", "d")]));
    assert_eq!(parse_head(b"HTTP/1.1 200 OK\r\nA: b \r\nc:d"), Some(ok));
    let no_text = (204, String::new(), HeaderMap::new());
    assert_eq!(parse_head(b"HTTP/1.1 204"), Some(no_text));
    let bad = [
        &b"HTTP/1.1 2000 OK"[..],
        b"HTTP/1.1 OK",
        b"HTP/1.1 200 OK",
        b"HTTP/1.1 200 OK\r\nno colon",
        b"HTTP/1.1 200 OK\r\nbad name: x",
    ];
    for head in bad {
        assert_eq!(parse_head(head), None, "{}", String::from_utf8_lossy(head));
    }
}

#[test]
fn a_record_is_its_expiry_then_the_response_head() {
    let stored = headers(&[("content-type", "text/plain"), ("x", "1"), ("x", "2")]);
    let head = record_head(NOW, 404, "Not Found", &stored);
    let expected = "1700000000000\r\nHTTP/1.1 404 Not Found\r\ncontent-type: text/plain\r\n\
                    x: 1\r\nx: 2\r\n\r\n";
    assert_eq!(String::from_utf8(head.clone()).unwrap(), expected);
    let end = blank_line(&head, 0).unwrap();
    assert_eq!(end + 4, head.len());
    let fresh = (404, "Not Found".to_owned(), stored);
    assert_eq!(parse_record(&head[..end], NOW - 1), Some(fresh));
    assert_eq!(parse_record(&head[..end], NOW), None);
    assert_eq!(parse_record(b"soon\r\nHTTP/1.1 200 OK", 0), None);
}

#[test]
fn a_blank_line_is_found_across_reads() {
    let buf = b"HTTP/1.1 200 OK\r\n\r\nbody";
    assert_eq!(blank_line(buf, 0), Some(15));
    assert_eq!(blank_line(buf, 17), Some(15));
    assert_eq!(blank_line(buf, 19), None);
    assert_eq!(blank_line(b"\r\n", 0), None);
}

#[test]
fn keys_are_the_namespace_and_the_cf_cache_key_or_url() {
    assert_eq!(
        entry_name(b"", b"http://a/"),
        "f890e4f0687138b4616f6b95be7ad9942250b94ab3dc99b041fee0e4ca4771d0"
    );
    assert_ne!(
        entry_name(b"", b"http://a/"),
        entry_name(b"n", b"http://a/")
    );
    assert_eq!(cache_key(r#"{"cacheKey":"k"}"#).as_deref(), Some("k"));
    assert_eq!(cache_key(r#"{"cacheKey":""}"#), None);
    assert_eq!(cache_key(r#"{"cacheTtl":5}"#), None);
}

#[test]
fn a_node_removes_only_directories_no_node_holds() {
    let tmp = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let root = tmp.join("claim");
    std::fs::create_dir_all(&root).unwrap();
    let (a, a_lock) = clustered::claim(&root, "a").unwrap();
    let (b, _b_lock) = clustered::claim(&root, "b").unwrap();
    assert!(a.exists());
    drop(a_lock);
    let (c, _c_lock) = clustered::claim(&root, "c").unwrap();
    assert!(!a.exists() && b.exists() && c.exists());
}
