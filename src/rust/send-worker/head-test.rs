// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use hyper::ext::HeaderCaseMap;
use kj::http::HeaderId;
use kj::http::Headers;

use super::*;

/// Every enumerator of `kj::http::Method`, and its name as KJ reads and writes it.
const METHODS: [(Method, &str); 29] = [
    (Method::GET, "GET"),
    (Method::HEAD, "HEAD"),
    (Method::POST, "POST"),
    (Method::PUT, "PUT"),
    (Method::DELETE, "DELETE"),
    (Method::PATCH, "PATCH"),
    (Method::PURGE, "PURGE"),
    (Method::OPTIONS, "OPTIONS"),
    (Method::TRACE, "TRACE"),
    (Method::COPY, "COPY"),
    (Method::LOCK, "LOCK"),
    (Method::MKCOL, "MKCOL"),
    (Method::MOVE, "MOVE"),
    (Method::PROPFIND, "PROPFIND"),
    (Method::PROPPATCH, "PROPPATCH"),
    (Method::SEARCH, "SEARCH"),
    (Method::UNLOCK, "UNLOCK"),
    (Method::ACL, "ACL"),
    (Method::REPORT, "REPORT"),
    (Method::MKACTIVITY, "MKACTIVITY"),
    (Method::CHECKOUT, "CHECKOUT"),
    (Method::MERGE, "MERGE"),
    (Method::MSEARCH, "MSEARCH"),
    (Method::NOTIFY, "NOTIFY"),
    (Method::SUBSCRIBE, "SUBSCRIBE"),
    (Method::UNSUBSCRIBE, "UNSUBSCRIBE"),
    (Method::QUERY, "QUERY"),
    (Method::BAN, "BAN"),
    (Method::CONNECT, "CONNECT"),
];

/// `kj::HttpHeaders` holding `headers`, each name spelled as given.
fn kj_headers<'t>(table: &'t HeaderTable, headers: &[(&str, &str)]) -> Borrowing<'t, HttpHeaders> {
    let mut head = Head::empty();
    for (name, value) in headers {
        head.append(name.as_bytes(), value.as_bytes());
    }
    let mut map = http::HeaderMap::new();
    let mut extensions = http::Extensions::new();
    head.apply(&mut map, &mut extensions);
    headers_to_kj(table, &map, &extensions).unwrap()
}

/// The spellings `extensions` records for `name`.
fn spellings(extensions: &http::Extensions, name: &str) -> Vec<String> {
    let name = http::HeaderName::from_bytes(name.as_bytes()).unwrap();
    extensions
        .get::<HeaderCaseMap>()
        .expect("no spellings")
        .get_all_internal(&name)
        .map(|spelling| String::from_utf8(spelling.to_vec()).unwrap())
        .collect()
}

#[test]
fn request_parts_maps_the_method_the_url_and_the_headers() {
    let table = HeaderTable::builtin();
    let mut headers = Headers::new(&table);
    headers.set(HeaderId::HOST, "example.com");
    headers.set(HeaderId::CONTENT_TYPE, "text/plain");

    let parts = request_parts(Method::POST, b"/a?b=c", HeadersRef::from(&headers)).unwrap();
    assert_eq!(parts.method, http::Method::POST);
    assert_eq!(parts.uri, "/a?b=c");
    assert_eq!(parts.uri.path(), "/a");
    assert_eq!(parts.uri.query(), Some("b=c"));
    assert_eq!(parts.uri.scheme(), None);
    assert_eq!(parts.headers["host"], "example.com");
    assert_eq!(parts.headers["content-type"], "text/plain");
    assert_eq!(parts.headers.len(), 2);

    let parts = request_parts(
        Method::GET,
        b"http://example.com/a?b=c",
        HeadersRef::from(&headers),
    )
    .unwrap();
    assert_eq!(parts.method, http::Method::GET);
    assert_eq!(parts.uri.scheme_str(), Some("http"));
    assert_eq!(parts.uri.host(), Some("example.com"));
    assert_eq!(parts.uri.path(), "/a");
    assert_eq!(parts.uri.query(), Some("b=c"));
}

#[test]
fn request_parts_refuses_a_url_http_uri_refuses() {
    let table = HeaderTable::builtin();
    let headers = Headers::new(&table);
    let error = request_parts(Method::GET, b"/a b", HeadersRef::from(&headers)).unwrap_err();
    assert_eq!(error.exception_type(), kj::KjExceptionType::Failed);
    assert!(error.description().contains("URL"), "{error:?}");
}

/// `method` to `http::Method` through `request_parts`, and back through `method_to_kj`.
fn round_trip(method: Method, name: &str) {
    let table = HeaderTable::builtin();
    let headers = Headers::new(&table);
    let parts = request_parts(method, b"/", HeadersRef::from(&headers)).unwrap();
    assert_eq!(parts.method.as_str(), name);
    assert_eq!(method_to_kj(&parts.method).unwrap(), method, "{name}");
}

#[test]
fn every_kj_method_round_trips_through_http_method() {
    for (method, name) in METHODS {
        round_trip(method, name);
    }
}

#[test]
fn every_kj_method_maps_to_an_http_method_of_its_name() {
    let table = HeaderTable::builtin();
    let headers = Headers::new(&table);
    for (method, name) in METHODS {
        let parts = request_parts(method, b"/", HeadersRef::from(&headers)).unwrap();
        assert_eq!(parts.method.as_str(), name);
    }
}

#[test]
fn a_method_kj_does_not_know_is_refused() {
    let brew = http::Method::from_bytes(b"BREW").unwrap();
    let error = method_to_kj(&brew).unwrap_err();
    assert!(error.description().contains("BREW"), "{error:?}");
}

#[test]
fn header_names_keep_their_spelling_and_repeated_headers_keep_every_value() {
    let table = HeaderTable::builtin();
    let original = kj_headers(
        &table,
        &[
            ("X-MiXeD-Case", "1"),
            ("X-MiXeD-Case", "2"),
            ("Content-Type", "text/plain"),
        ],
    );

    let mut extensions = http::Extensions::new();
    let map = headers_from_kj(HeadersRef::from(&*original), &mut extensions);
    let values: Vec<_> = map.get_all("x-mixed-case").iter().collect();
    assert_eq!(values, ["1", "2"]);
    assert_eq!(map["content-type"], "text/plain");
    assert_eq!(spellings(&extensions, "x-mixed-case"), ["X-MiXeD-Case"; 2]);

    let back = headers_to_kj(&table, &map, &extensions).unwrap();
    let back = HeadersRef::from(&*back);
    assert_eq!(back.get_by_name("x-mixed-case"), Some(&b"1"[..]));
    assert_eq!(back.get(HeaderId::CONTENT_TYPE), Some(&b"text/plain"[..]));

    // A second trip finds the spelling in KJ's headers, not in the extensions it started from.
    let mut again = http::Extensions::new();
    let map = headers_from_kj(back, &mut again);
    let values: Vec<_> = map.get_all("x-mixed-case").iter().collect();
    assert_eq!(values, ["1", "2"]);
    assert_eq!(spellings(&again, "x-mixed-case"), ["X-MiXeD-Case"; 2]);
}

#[test]
fn a_canonical_reason_phrase_adds_no_extension_and_a_custom_one_is_carried() {
    let table = HeaderTable::builtin();
    let headers = Headers::new(&table);

    let parts = response_parts(200, "OK", HeadersRef::from(&headers)).unwrap();
    assert_eq!(parts.status, http::StatusCode::OK);
    assert!(parts.extensions.get::<ReasonPhrase>().is_none());
    assert_eq!(reason(&parts), "OK");

    let parts = response_parts(200, "Fine", HeadersRef::from(&headers)).unwrap();
    assert_eq!(parts.status, http::StatusCode::OK);
    assert_eq!(
        parts.extensions.get::<ReasonPhrase>().unwrap().as_bytes(),
        b"Fine"
    );
    assert_eq!(reason(&parts), "Fine");

    // A status without a canonical reason keeps the one it came with.
    let parts = response_parts(599, "Odd", HeadersRef::from(&headers)).unwrap();
    assert_eq!(reason(&parts), "Odd");
}

#[test]
fn an_invalid_status_is_refused() {
    let table = HeaderTable::builtin();
    let headers = Headers::new(&table);
    for status in [0, 99, 1000, u32::from(u16::MAX) + 200] {
        let error = response_parts(status, "X", HeadersRef::from(&headers)).unwrap_err();
        assert!(
            error.description().contains("status"),
            "{status}: {error:?}"
        );
    }
}
