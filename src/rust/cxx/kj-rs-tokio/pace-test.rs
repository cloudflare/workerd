use std::future::poll_fn;
use std::pin::pin;
use std::task::Waker;

use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;

use super::*;
use crate::Runtime;

/// What `future` resolves to if it does when polled once.
fn now<F: Future>(future: F) -> Option<F::Output> {
    match pin!(future).poll(&mut Context::from_waker(Waker::noop())) {
        Poll::Ready(output) => Some(output),
        Poll::Pending => None,
    }
}

#[test]
fn an_operation_waits_for_the_port_to_run_since_the_one_before() {
    let mut runtime = Runtime::new().unwrap();
    runtime
        .block_on(async {
            let pace = PortPace::default();
            let ready = || now(poll_fn(|cx| pace.poll_ready(cx))).is_some();
            // Nothing completed yet.
            assert!(ready());
            assert!(ready());
            pace.completed();
            // This task has run since, but the port has not run a task again.
            assert!(!ready());
            assert!(!ready());
            poll_fn(|cx| pace.poll_ready(cx)).await;
            // Waited out once, the next operation need not wait.
            assert!(ready());
        })
        .unwrap();
}

#[test]
fn an_operation_does_not_wait_when_the_port_ran_meanwhile() {
    let mut runtime = Runtime::new().unwrap();
    let pace = std::rc::Rc::new(PortPace::default());
    let completed = std::rc::Rc::clone(&pace);
    runtime
        .block_on(async move { completed.completed() })
        .unwrap();
    // The port ran to end the task above, and runs for this one.
    runtime
        .block_on(async move {
            assert!(now(poll_fn(|cx| pace.poll_ready(cx))).is_some());
        })
        .unwrap();
}

#[test]
fn reads_of_a_paced_stream_get_every_byte_one_port_run_apart() {
    let mut runtime = Runtime::new().unwrap();
    runtime
        .block_on(async {
            let (mut near, far) = tokio::io::duplex(64);
            let mut far = PortPaced::new(far);
            near.write_all(b"abcdef").await.unwrap();
            let mut first = [0; 3];
            far.read_exact(&mut first).await.unwrap();
            assert_eq!(&first, b"abc");
            // The rest is there to read, but the port has not run since the first read.
            let mut rest = [0; 3];
            assert!(now(far.read_exact(&mut rest)).is_none());
            far.read_exact(&mut rest).await.unwrap();
            assert_eq!(&rest, b"def");
            // Writes are not held back.
            assert!(now(far.write_all(b"x")).is_some());
        })
        .unwrap();
}

#[test]
fn a_thread_without_a_port_is_not_paced() {
    let pace = PortPace::default();
    pace.completed();
    assert!(now(poll_fn(|cx| pace.poll_ready(cx))).is_some());
}
