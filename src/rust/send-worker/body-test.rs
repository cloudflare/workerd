// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::future::Future;
use std::future::poll_fn;

use futures::FutureExt;
use http_body::Body as _;
use kj::KjExceptionType;

use super::*;

/// Runs `future` to completion on a runtime of its own; a body needs no event loop.
fn block_on<F: Future>(future: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
        .build()
        .unwrap()
        .block_on(future)
}

/// The next chunk, which must be data.
async fn data(body: &mut Body) -> Bytes {
    body.chunk().await.expect("the body ended").unwrap()
}

/// The next chunk, which must be an error.
async fn error(body: &mut Body) -> Error {
    match body.chunk().await {
        Some(Err(error)) => error,
        other => panic!("expected an error, got {other:?}"),
    }
}

#[test]
fn an_empty_body_has_no_bytes_and_has_ended() {
    let mut body = Body::empty();
    assert_eq!(body.length(), Some(0));
    assert!(body.is_end_stream());
    assert!(block_on(body.chunk()).is_none());
}

#[test]
fn a_full_body_yields_its_bytes_once_and_then_ends() {
    block_on(async {
        let mut body = Body::full("hello");
        assert_eq!(body.length(), Some(5));
        assert!(!body.is_end_stream());
        assert_eq!(data(&mut body).await, "hello");
        assert_eq!(body.length(), Some(0));
        assert!(body.is_end_stream());
        assert!(body.chunk().await.is_none());
        assert!(body.chunk().await.is_none());
    });
}

#[test]
fn a_full_body_of_no_bytes_is_an_empty_body() {
    let body = Body::full(Bytes::new());
    assert!(matches!(body.0, Kind::Empty));
    assert_eq!(body.length(), Some(0));
    assert!(body.is_end_stream());
}

#[test]
fn a_channel_body_yields_its_chunks_in_order_and_ends_cleanly_after_finish() {
    block_on(async {
        let (mut sender, mut body) = Body::channel(None);
        let sending = tokio::spawn(async move {
            for i in 0..100 {
                sender.send(Bytes::from(format!("{i}"))).await.unwrap();
            }
            sender.finish();
        });
        for i in 0..100 {
            assert_eq!(data(&mut body).await, format!("{i}"));
        }
        assert!(body.chunk().await.is_none());
        assert!(body.is_end_stream());
        sending.await.unwrap();
    });
}

#[test]
fn a_sender_dropped_without_finishing_ends_the_body_with_disconnected_after_the_queued_chunks() {
    block_on(async {
        let (mut sender, mut body) = Body::channel(None);
        sender.send(Bytes::from("a")).await.unwrap();
        sender.send(Bytes::from("b")).await.unwrap();
        drop(sender);
        assert_eq!(data(&mut body).await, "a");
        assert_eq!(data(&mut body).await, "b");
        let error = error(&mut body).await;
        assert_eq!(error.exception_type(), KjExceptionType::Disconnected);
        // An error ends the body.
        assert!(body.chunk().await.is_none());
    });
}

#[test]
fn fail_delivers_its_error_after_the_queued_chunks() {
    block_on(async {
        let (mut sender, mut body) = Body::channel(None);
        sender.send(Bytes::from("a")).await.unwrap();
        sender.fail(kj::overloaded!("the worker gave up")).await;
        assert_eq!(data(&mut body).await, "a");
        let error = error(&mut body).await;
        assert_eq!(error.exception_type(), KjExceptionType::Overloaded);
        assert_eq!(error.description(), "the worker gave up");
        assert!(body.chunk().await.is_none());
    });
}

#[test]
fn send_waits_once_the_channel_holds_its_chunks_and_proceeds_when_the_reader_takes_one() {
    block_on(async {
        let (mut sender, mut body) = Body::channel(None);
        for i in 0..CHANNEL_CHUNKS {
            let sent = sender.send(Bytes::from(format!("{i}"))).now_or_never();
            assert_eq!(sent, Some(Ok(())), "chunk {i}");
        }
        {
            let mut blocked = std::pin::pin!(sender.send(Bytes::from("next")));
            assert!((&mut blocked).now_or_never().is_none());
            assert_eq!(data(&mut body).await, "0");
            assert_eq!(blocked.now_or_never(), Some(Ok(())));
        }
        // Full again.
        assert!(sender.send(Bytes::from("more")).now_or_never().is_none());
        sender.finish();
        for expected in ["1", "2", "3", "next"] {
            assert_eq!(data(&mut body).await, expected);
        }
        assert!(body.chunk().await.is_none());
    });
}

#[test]
fn send_fails_with_closed_and_closed_resolves_once_the_body_is_dropped() {
    block_on(async {
        let (mut sender, body) = Body::channel(None);
        assert!(sender.closed().now_or_never().is_none());
        drop(body);
        assert_eq!(sender.closed().now_or_never(), Some(()));
        assert_eq!(sender.send(Bytes::from("a")).await, Err(Closed));
    });
}

#[test]
fn send_waiting_for_room_fails_with_closed_when_the_body_is_dropped() {
    block_on(async {
        let (mut sender, body) = Body::channel(None);
        for _ in 0..CHANNEL_CHUNKS {
            sender.send(Bytes::from("a")).await.unwrap();
        }
        let mut blocked = std::pin::pin!(sender.send(Bytes::from("b")));
        assert!((&mut blocked).now_or_never().is_none());
        drop(body);
        assert_eq!(blocked.await, Err(Closed));
    });
}

#[test]
fn a_declared_length_counts_down_as_chunks_are_read_and_an_exact_body_ends_cleanly() {
    block_on(async {
        let (mut sender, mut body) = Body::channel(Some(5));
        assert_eq!(body.length(), Some(5));
        sender.send(Bytes::from("ab")).await.unwrap();
        sender.send(Bytes::from("cde")).await.unwrap();
        sender.finish();
        // Queued chunks are not counted until they are read.
        assert_eq!(body.length(), Some(5));
        assert_eq!(data(&mut body).await, "ab");
        assert_eq!(body.length(), Some(3));
        assert_eq!(data(&mut body).await, "cde");
        assert_eq!(body.length(), Some(0));
        assert!(body.chunk().await.is_none());
    });
}

#[test]
fn a_body_that_delivers_more_than_its_declared_length_fails() {
    block_on(async {
        let (mut sender, mut body) = Body::channel(Some(3));
        sender.send(Bytes::from("ab")).await.unwrap();
        sender.send(Bytes::from("cd")).await.unwrap();
        sender.finish();
        assert_eq!(data(&mut body).await, "ab");
        let error = error(&mut body).await;
        assert_eq!(error.exception_type(), KjExceptionType::Failed);
        assert!(error.description().contains("longer"), "{error:?}");
        assert!(body.chunk().await.is_none());
    });
}

#[test]
fn a_body_finished_short_of_its_declared_length_fails() {
    block_on(async {
        let (mut sender, mut body) = Body::channel(Some(5));
        sender.send(Bytes::from("ab")).await.unwrap();
        sender.finish();
        assert_eq!(data(&mut body).await, "ab");
        let error = error(&mut body).await;
        assert_eq!(error.exception_type(), KjExceptionType::Failed);
        assert!(error.description().contains("shorter"), "{error:?}");
        assert!(body.chunk().await.is_none());
    });
}

#[test]
fn the_size_hint_is_exact_for_a_known_length_and_open_ended_otherwise() {
    let exact = |body: &Body| {
        let hint = body.size_hint();
        (hint.lower(), hint.upper())
    };
    assert_eq!(exact(&Body::empty()), (0, Some(0)));
    assert_eq!(exact(&Body::full("abc")), (3, Some(3)));
    let (_sender, declared) = Body::channel(Some(7));
    assert_eq!(exact(&declared), (7, Some(7)));
    let (_sender, open) = Body::channel(None);
    assert_eq!(exact(&open), (0, None));
}

#[test]
fn poll_frame_yields_data_frames() {
    block_on(async {
        let (mut sender, mut body) = Body::channel(None);
        sender.send(Bytes::from("a")).await.unwrap();
        sender.send(Bytes::from("b")).await.unwrap();
        sender.finish();
        let mut frames = Vec::new();
        while let Some(frame) = poll_fn(|cx| Pin::new(&mut body).poll_frame(cx)).await {
            frames.push(frame.unwrap().into_data().unwrap());
        }
        assert_eq!(frames, ["a", "b"]);
        assert!(body.is_end_stream());
    });
}

#[test]
fn an_empty_chunk_is_not_delivered_and_takes_no_room() {
    block_on(async {
        let (mut sender, mut body) = Body::channel(None);
        for _ in 0..CHANNEL_CHUNKS * 4 {
            assert_eq!(sender.send(Bytes::new()).now_or_never(), Some(Ok(())));
        }
        sender.send(Bytes::from("x")).await.unwrap();
        sender.finish();
        assert_eq!(data(&mut body).await, "x");
        assert!(body.chunk().await.is_none());
    });
}
