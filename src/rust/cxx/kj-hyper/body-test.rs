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
