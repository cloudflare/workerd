use std::future::poll_fn;

use futures::executor::block_on;

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

fn spellings_of(head: Head) -> (http::HeaderMap, Option<HeaderCaseMap>) {
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
    let (headers, spellings) = spellings_of(head);
    assert_eq!(headers.len(), 3);
    assert!(spellings.is_none());
}

#[test]
fn an_empty_value_keeps_the_extension() {
    let mut head = Head::empty();
    head.append(b"X-Empty", b"");
    assert!(spellings_of(head).1.is_some());
}

#[test]
fn other_spellings_are_kept() {
    let mut head = Head::empty();
    head.append(b"Content-Type", b"text/plain");
    head.append(b"x-lower", b"1");
    head.append(b"X-UPPER", b"2");
    head.set(Builtin::Te, http::HeaderValue::from_static("trailers"));
    let spellings = spellings_of(head).1.unwrap();
    assert!(spelled(&spellings, "content-type").is_empty());
    assert_eq!(spelled(&spellings, "x-lower"), [Bytes::from("x-lower")]);
    assert_eq!(spelled(&spellings, "x-upper"), [Bytes::from("X-UPPER")]);
    assert_eq!(spelled(&spellings, "te"), [Bytes::from("TE")]);
}

#[test]
fn a_spelled_value_spells_its_names_earlier_values() {
    let mut head = Head::empty();
    head.append(b"Set-Cookie", b"a=1");
    head.append(b"Set-Cookie", b"b=2");
    head.append(b"set-cookie", b"c=3");
    head.append(b"Set-Cookie", b"d=4");
    let spellings = spellings_of(head).1.unwrap();
    assert_eq!(
        spelled(&spellings, "set-cookie"),
        ["Set-Cookie", "Set-Cookie", "set-cookie", "Set-Cookie"].map(Bytes::from)
    );
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
