use std::future::poll_fn;

use futures::executor::block_on;
use kj::http::HeaderId;
use kj::http::HeadersRef;

use super::*;

#[test]
fn a_write_beyond_the_declared_length_fails_before_queueing() {
    block_on(async {
        let (sink, _abort, mut body) = channel(Some(5));
        assert!(sink.write(b"0123456789").await.is_err());
        sink.write(b"01234").await.unwrap();
        drop(sink);
        let frame = poll_fn(|cx| Pin::new(&mut body).poll_frame(cx))
            .await
            .unwrap()
            .unwrap();
        assert_eq!(frame.into_data().unwrap(), &b"01234"[..]);
    });
}

#[test]
fn an_aborted_body_fails_rather_than_ending() {
    block_on(async {
        let (sink, abort, mut body) = channel(None);
        drop(sink);
        abort.abort();
        let frame = poll_fn(|cx| Pin::new(&mut body).poll_frame(cx)).await;
        assert!(matches!(frame, Some(Err(Aborted))));
    });
}

#[test]
fn a_dropped_abort_handle_lets_the_body_end() {
    block_on(async {
        let (sink, abort, mut body) = channel(None);
        drop(abort);
        drop(sink);
        let frame = poll_fn(|cx| Pin::new(&mut body).poll_frame(cx)).await;
        assert!(frame.is_none());
    });
}

#[test]
fn a_sink_aborted_before_its_drop_fails_the_body() {
    block_on(async {
        let (sink, _abort, mut body) = channel(None);
        sink.write(b"hello").await.unwrap();
        sink.abort();
        drop(sink);
        let frame = poll_fn(|cx| Pin::new(&mut body).poll_frame(cx)).await;
        assert!(matches!(frame, Some(Err(Aborted))));
    });
}

fn applied(head: Head) -> (http::HeaderMap, Option<HeaderCaseMap>) {
    let mut parts = http::Response::new(()).into_parts().0;
    head.apply(&mut parts.headers, &mut parts.extensions);
    (parts.headers, parts.extensions.remove::<HeaderCaseMap>())
}

fn spelled(spellings: &HeaderCaseMap, name: &str) -> Vec<Bytes> {
    spellings
        .get_all_internal(&http::HeaderName::from_bytes(name.as_bytes()).unwrap())
        .cloned()
        .collect()
}

#[test]
fn names_spelled_as_hyper_title_cases_them_carry_no_spellings() {
    let mut head = Head::empty();
    head.append(b"Content-Type", b"text/plain");
    head.append(b"X-Request-Id", b"1");
    head.append(b"Content-Length", b"5");
    head.frame(Some(5));
    let (headers, spellings) = applied(head);
    assert_eq!(headers.len(), 3);
    assert!(spellings.is_none());
}

#[test]
fn an_empty_value_keeps_the_extension() {
    let mut head = Head::empty();
    head.append(b"X-Empty", b"");
    let (headers, spellings) = applied(head);
    assert_eq!(headers.get("x-empty").unwrap(), "");
    assert!(spellings.is_some());
}

#[test]
fn other_spellings_are_kept() {
    let mut head = Head::empty();
    head.append(b"Content-Type", b"text/plain");
    head.append(b"x-lower", b"1");
    head.append(b"X-UPPER", b"2");
    head.set(Builtin::Te, http::HeaderValue::from_static("trailers"));
    let spellings = applied(head).1.unwrap();
    assert!(spelled(&spellings, "content-type").is_empty());
    assert_eq!(spelled(&spellings, "x-lower"), [Bytes::from("x-lower")]);
    assert_eq!(spelled(&spellings, "x-upper"), [Bytes::from("X-UPPER")]);
    assert_eq!(spelled(&spellings, "te"), [Bytes::from("TE")]);
}

#[test]
fn a_spelled_value_spells_its_names_other_values() {
    let mut head = Head::empty();
    head.append(b"Set-Cookie", b"a=1");
    head.append(b"Set-Cookie", b"b=2");
    head.append(b"set-cookie", b"c=3");
    head.append(b"Set-Cookie", b"d=4");
    head.append(b"X-Other", b"e");
    let (headers, spellings) = applied(head);
    let spellings = spellings.unwrap();
    assert_eq!(
        spelled(&spellings, "set-cookie"),
        ["Set-Cookie", "Set-Cookie", "set-cookie", "Set-Cookie"].map(Bytes::from)
    );
    assert!(spelled(&spellings, "x-other").is_empty());
    let cookies: Vec<_> = headers.get_all("set-cookie").iter().collect();
    assert_eq!(cookies, ["a=1", "b=2", "c=3", "d=4"]);
}

#[test]
fn builtins_go_first_and_others_keep_kjs_order() {
    let mut head = Head::empty();
    head.append(b"X-B", b"1");
    head.append(b"Host", b"example.com");
    head.append(b"X-A", b"2");
    head.frame(None);
    let headers = applied(head).0;
    let names: Vec<_> = headers.keys().map(http::HeaderName::as_str).collect();
    assert_eq!(names, ["transfer-encoding", "host", "x-b", "x-a"]);
}

#[test]
fn values_and_spellings_share_one_buffer() {
    let mut head = Head::empty();
    head.reserve(3, 64);
    head.append(b"X-A", b"one");
    head.append(b"x-b", b"two");
    head.append(b"X-C", b"three");
    let (headers, spellings) = applied(head);
    let one = headers.get("x-a").unwrap().as_bytes();
    let two = headers.get("x-b").unwrap().as_bytes();
    let three = headers.get("x-c").unwrap().as_bytes();
    let spelling = spelled(&spellings.unwrap(), "x-b").remove(0);
    // Copied in the order appended: a value, then the name if it is spelled.
    assert_eq!(two.as_ptr(), one.as_ptr().wrapping_add(one.len()));
    assert_eq!(spelling.as_ptr(), two.as_ptr().wrapping_add(two.len()));
    assert_eq!(
        three.as_ptr(),
        spelling.as_ptr().wrapping_add(spelling.len())
    );
    assert_eq!((one, two, three), (&b"one"[..], &b"two"[..], &b"three"[..]));
}

#[test]
fn a_value_http_refuses_is_dropped() {
    let mut head = Head::empty();
    head.append(b"X-Split", b"a\r\nb");
    head.append(b"X-Control", b"a\x01b");
    let (headers, _) = applied(head);
    assert!(headers.get("x-split").is_none());
    // As kj: only NUL, CR and LF are refused.
    assert_eq!(headers.get("x-control").unwrap(), "a\x01b");
}

#[test]
fn many_lowercase_headers_are_each_spelled() {
    const COUNT: usize = 20_000;
    let mut head = Head::empty();
    for i in 0..COUNT {
        head.append(format!("x-header-{i}").as_bytes(), b"v");
    }
    let (headers, spellings) = applied(head);
    let spellings = spellings.unwrap();
    assert_eq!(headers.len(), COUNT);
    for i in [0, COUNT / 2, COUNT - 1] {
        let name = format!("x-header-{i}");
        assert_eq!(spelled(&spellings, &name), [Bytes::from(name.clone())]);
    }
}

#[test]
fn title_case_is_hypers() {
    assert!(is_title_case(b"Content-Type"));
    assert!(is_title_case(b"X-1-Foo"));
    assert!(is_title_case(b"-X"));
    assert!(!is_title_case(b"content-type"));
    assert!(!is_title_case(b"Content-type"));
    assert!(!is_title_case(b"ETag"));
    assert!(!is_title_case(b"Sec-WebSocket-Key"));
    let name = http::HeaderName::from_static("sec-websocket-key");
    assert_eq!(title_case(&name), "Sec-Websocket-Key");
}

#[test]
fn builtin_names_are_the_builtin_spellings_lowercased() {
    for (spelling, name) in BUILTIN.iter().zip(BUILTIN_NAMES) {
        assert_eq!(name.as_str(), spelling.to_ascii_lowercase());
    }
}

/// `map` through a [`HeaderBlock`] into kj, and back out through a [`Head`].
fn through_kj(map: &http::HeaderMap, extensions: &http::Extensions) -> http::HeaderMap {
    let table = HttpHeaderTable::builtin();
    let headers = HeaderBlock::new(map, extensions).into_kj(&table).unwrap();
    applied(Head::new(&headers)).0
}

#[test]
fn a_header_block_keeps_kjs_joining_of_repeated_headers() {
    let mut map = http::HeaderMap::new();
    map.append(header::DATE, http::HeaderValue::from_static("a"));
    map.append("x-dup", http::HeaderValue::from_static("1"));
    map.append(header::SET_COOKIE, http::HeaderValue::from_static("a=1"));
    map.append(header::DATE, http::HeaderValue::from_static("b"));
    map.append("x-dup", http::HeaderValue::from_static("2"));
    map.append(header::SET_COOKIE, http::HeaderValue::from_static("b=2"));
    map.append("x-empty", http::HeaderValue::from_static(""));
    let table = HttpHeaderTable::builtin();
    let headers = HeaderBlock::new(&map, &http::Extensions::new())
        .into_kj(&table)
        .unwrap();
    let kj = HeadersRef::from(&*headers);
    // An indexed header's values are joined, as kj joins them.
    assert_eq!(kj.get(HeaderId::DATE), Some(&b"a, b"[..]));
    assert_eq!(kj.get_by_name("x-empty"), Some(&b""[..]));
    let back = applied(Head::new(&headers)).0;
    let values = |name: &str| -> Vec<_> { back.get_all(name).iter().collect() };
    assert_eq!(values("date"), ["a, b"]);
    assert_eq!(values("x-dup"), ["1", "2"]);
    assert_eq!(values("set-cookie"), ["a=1", "b=2"]);
    assert_eq!(values("x-empty"), [""]);
}

#[test]
fn a_kj_value_that_is_not_utf8_keeps_its_bytes() {
    let mut map = http::HeaderMap::new();
    let value = http::HeaderValue::from_bytes(b"a\xffb\x80").unwrap();
    map.append("x-opaque", value.clone());
    let back = through_kj(&map, &http::Extensions::new());
    assert_eq!(back["x-opaque"], value);
}

#[test]
fn a_header_block_keeps_the_peers_spellings() {
    let mut map = http::HeaderMap::new();
    let mut spellings = HeaderCaseMap::default();
    for (spelling, value) in [("x-lower", "1"), ("X-MiXeD", "2"), ("x-mixed", "3")] {
        let name = http::HeaderName::from_bytes(spelling.as_bytes()).unwrap();
        spellings.append(&name, Bytes::from_static(spelling.as_bytes()));
        map.append(name, http::HeaderValue::from_static(value));
    }
    let mut extensions = http::Extensions::new();
    extensions.insert(spellings);
    let table = HttpHeaderTable::builtin();
    let headers = HeaderBlock::new(&map, &extensions).into_kj(&table).unwrap();
    let (_, spellings) = applied(Head::new(&headers));
    let spellings = spellings.unwrap();
    assert_eq!(spelled(&spellings, "x-lower"), [Bytes::from("x-lower")]);
    assert_eq!(
        spelled(&spellings, "x-mixed"),
        ["X-MiXeD", "x-mixed"].map(Bytes::from)
    );
}

#[test]
fn a_header_block_outlives_the_map_it_was_built_from() {
    let table = HttpHeaderTable::builtin();
    let headers = {
        let mut map = http::HeaderMap::new();
        map.append("x-a", http::HeaderValue::from_static("kept"));
        HeaderBlock::new(&map, &http::Extensions::new())
            .into_kj(&table)
            .unwrap()
    };
    let kj = HeadersRef::from(&*headers);
    assert_eq!(kj.get_by_name("x-a"), Some(&b"kept"[..]));
}

#[test]
fn an_empty_header_block_builds_empty_headers() {
    assert!(through_kj(&http::HeaderMap::new(), &http::Extensions::new()).is_empty());
}

#[test]
fn many_headers_pass_through_a_header_block() {
    const COUNT: usize = 5000;
    let mut map = http::HeaderMap::new();
    for i in 0..COUNT {
        let name = http::HeaderName::from_bytes(format!("x-h-{i}").as_bytes()).unwrap();
        map.append(name, http::HeaderValue::from_str(&i.to_string()).unwrap());
    }
    let back = through_kj(&map, &http::Extensions::new());
    assert_eq!(back, map);
}
