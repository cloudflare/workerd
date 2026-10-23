// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use capnp::message::Builder;
use kj::http::Headers;

use super::*;

fn options(build: impl FnOnce(http_options::Builder<'_>)) -> HttpRewriter {
    let mut message = Builder::new_default();
    build(message.init_root::<http_options::Builder<'_>>());
    HttpRewriter::new(
        message
            .get_root_as_reader::<http_options::Reader<'_>>()
            .unwrap(),
    )
    .unwrap()
}

fn header(headers: &ffi::HttpHeaders, id: HeaderId) -> Option<Vec<u8>> {
    HeadersRef::from(headers).get(id).map(<[u8]>::to_vec)
}

#[test]
fn host_style_outgoing_moves_the_host_into_its_header() {
    let table = HeaderTable::builtin();
    let rewriter = options(|mut o| o.set_style(http_options::Style::Host));
    let mut headers = Headers::new(&table);
    headers.set(HeaderId::CONTENT_TYPE, "text/plain");
    let rewritten = rewriter
        .rewrite_outgoing_request(
            &table,
            "https://user:secret@example.com:8443/a/b?c=d",
            headers.as_ref(),
            None,
        )
        .unwrap();
    assert_eq!(rewritten.url, "/a/b?c=d");
    let edited = rewritten.headers.unwrap();
    assert_eq!(
        header(&edited, HeaderId::HOST).as_deref(),
        Some(&b"example.com:8443"[..])
    );
    assert_eq!(
        header(&edited, HeaderId::CONTENT_TYPE).as_deref(),
        Some(&b"text/plain"[..])
    );
}

#[test]
fn host_style_incoming_rebuilds_the_absolute_url() {
    let table = HeaderTable::builtin();
    let rewriter = options(|mut o| o.set_style(http_options::Style::Host));
    let mut headers = Headers::new(&table);
    headers.set(HeaderId::HOST, "foo.example");
    let (rewritten, cf_blob) = rewriter
        .rewrite_incoming_request(&table, "/x?y=1", "https", headers.as_ref())
        .unwrap()
        .unwrap();
    assert_eq!(rewritten.url, "https://foo.example/x?y=1");
    // Nothing to change in the headers, so they are the request's own.
    assert!(rewritten.headers.is_none());
    assert_eq!(cf_blob, None);
}

#[test]
fn host_style_incoming_without_a_host_is_a_bad_request() {
    let table = HeaderTable::builtin();
    let rewriter = options(|mut o| o.set_style(http_options::Style::Host));
    let headers = Headers::new(&table);
    assert!(
        rewriter
            .rewrite_incoming_request(&table, "/", "http", headers.as_ref())
            .unwrap()
            .is_none()
    );
}

#[test]
fn proxy_style_leaves_the_request_alone() {
    let table = HeaderTable::builtin();
    let rewriter = options(|mut o| o.set_style(http_options::Style::Proxy));
    let headers = Headers::new(&table);
    let rewritten = rewriter
        .rewrite_outgoing_request(&table, "http://h/p", headers.as_ref(), None)
        .unwrap();
    assert_eq!(rewritten.url, "http://h/p");
    assert!(rewritten.headers.is_none());
}

#[test]
fn a_header_the_table_lacks_is_refused() {
    // The factory's table has every header the config names; this one has only kj's own.
    let table = HeaderTable::builtin();
    let rewriter = options(|mut o| {
        o.set_style(http_options::Style::Proxy);
        o.init_inject_request_headers(1).get(0).set_name("X-Add");
    });
    let headers = Headers::new(&table);
    assert!(
        rewriter
            .rewrite_outgoing_request(&table, "http://h/", headers.as_ref(), None)
            .is_err()
    );
}
