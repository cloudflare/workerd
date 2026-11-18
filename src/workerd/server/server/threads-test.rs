// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::future::Future;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use super::*;

#[test]
fn only_the_inspector_is_an_obstacle() {
    assert_eq!(obstacles(false), Vec::<String>::new());
    let found = obstacles(true);
    assert_eq!(found.len(), 1, "{found:?}");
    assert!(found[0].contains("--inspector-addr"), "{found:?}");
    assert!(
        found[0].ends_with(
            "not supported on more than one thread; serve this config without --threads."
        ),
        "{found:?}"
    );
}

#[test]
fn a_session_pipe_carries_bytes_between_threads() {
    use tokio::io::AsyncReadExt;
    use tokio::io::AsyncWriteExt;

    let (SessionEnd(mut ours), theirs) = session_pipe();
    let echo = std::thread::spawn(move || {
        let SessionEnd(mut theirs) = theirs;
        let runtime = tokio::runtime::Builder::new_current_thread()
            .build()
            .unwrap();
        runtime.block_on(async {
            let mut received = [0; 5];
            theirs.read_exact(&mut received).await.unwrap();
            theirs.write_all(&received).await.unwrap();
        });
        // Dropping the end is the end of the stream for the other.
    });
    let runtime = tokio::runtime::Builder::new_current_thread()
        .build()
        .unwrap();
    runtime.block_on(async {
        ours.write_all(b"hello").await.unwrap();
        let mut received = Vec::new();
        ours.read_to_end(&mut received).await.unwrap();
        assert_eq!(received, b"hello");
    });
    echo.join().unwrap();
}

fn poll<T>(future: &mut std::pin::Pin<Box<impl Future<Output = T>>>) -> Poll<T> {
    future
        .as_mut()
        .poll(&mut Context::from_waker(Waker::noop()))
}

/// The watcher of `count` threads, with the events they send and the phase they follow.
fn watched(
    count: usize,
) -> (
    std::pin::Pin<Box<impl Future<Output = Result<()>>>>,
    mpsc::UnboundedSender<Event>,
    watch::Sender<Phase>,
) {
    let (events, received) = mpsc::unbounded_channel();
    let phase = watch::Sender::new(Phase::Serving);
    let watching = Box::pin(watch_threads(received, phase.subscribe(), count));
    (watching, events, phase)
}

#[test]
fn the_watch_ends_once_every_thread_ended_after_the_drain() {
    let (mut watching, events, phase) = watched(2);
    assert!(poll(&mut watching).is_pending());
    phase.send_replace(Phase::Draining);
    events.send(Event::Exited(2, Ok(()))).unwrap();
    assert!(poll(&mut watching).is_pending());
    events.send(Event::Exited(1, Ok(()))).unwrap();
    assert!(matches!(poll(&mut watching), Poll::Ready(Ok(()))));
}

#[test]
fn a_thread_that_fails_fails_the_watch_and_is_named() {
    let (mut watching, events, phase) = watched(2);
    phase.send_replace(Phase::Draining);
    let failure = kj::disconnected!("accept(): too many open files");
    events.send(Event::Exited(2, Err(failure))).unwrap();
    let Poll::Ready(Err(error)) = poll(&mut watching) else {
        panic!("expected the thread's failure");
    };
    assert_eq!(
        error.description(),
        "loop thread 2: accept(): too many open files"
    );
    assert_eq!(error.exception_type(), cxx::KjExceptionType::Disconnected);
}

#[test]
fn a_thread_that_stops_serving_before_the_drain_fails_the_watch() {
    let (mut watching, events, _phase) = watched(2);
    events.send(Event::Exited(1, Ok(()))).unwrap();
    let Poll::Ready(Err(error)) = poll(&mut watching) else {
        panic!("expected a failure");
    };
    assert_eq!(error.description(), "loop thread 1 stopped serving");
}

#[test]
fn a_thread_that_ends_without_a_report_fails_the_watch() {
    let (mut watching, events, _phase) = watched(1);
    drop(events);
    assert!(matches!(poll(&mut watching), Poll::Ready(Err(_))));

    // A thread that unwinds past its report says so.
    let (mut watching, events, _phase) = watched(1);
    drop(ExitReport {
        index: 1,
        events,
        result: None,
    });
    let Poll::Ready(Err(error)) = poll(&mut watching) else {
        panic!("expected a failure");
    };
    assert_eq!(error.description(), "loop thread 1: the thread panicked");
}

#[test]
fn signals_follow_the_phase_and_take_a_lost_first_thread_for_a_stop() {
    let phase = watch::Sender::new(Phase::Starting);
    let (events, _received) = mpsc::unbounded_channel();
    let signals = Signals {
        phase: phase.subscribe(),
        events,
    };
    let mut serving = Box::pin(signals.reached(Phase::Serving));
    assert!(poll(&mut serving).is_pending());
    // A phase that was skipped is reached too.
    phase.send_replace(Phase::Draining);
    assert_eq!(poll(&mut serving), Poll::Ready(Phase::Draining));

    let mut stopping = Box::pin(signals.reached(Phase::Stopping));
    assert!(poll(&mut stopping).is_pending());
    drop(phase);
    assert_eq!(poll(&mut stopping), Poll::Ready(Phase::Stopping));
}

#[test]
fn a_thread_that_ended_behind_the_last_ready_fails_the_start() {
    let (events, received) = mpsc::unbounded_channel();
    let phase = watch::Sender::new(Phase::Starting);
    events.send(Event::Ready).unwrap();
    events.send(Event::Ready).unwrap();
    let failure = kj::failed!("out of memory");
    events.send(Event::Exited(1, Err(failure))).unwrap();
    let mut starting = Box::pin(await_ready(received, phase.subscribe(), 2));
    let Poll::Ready(Err(error)) = poll(&mut starting) else {
        panic!("expected the thread's failure");
    };
    assert_eq!(error.description(), "loop thread 1: out of memory");
}

#[test]
fn a_thread_that_ends_after_the_start_fails_the_check() {
    let (events, received) = mpsc::unbounded_channel();
    let phase = watch::Sender::new(Phase::Starting);
    events.send(Event::Ready).unwrap();
    let mut starting = Box::pin(await_ready(received, phase.subscribe(), 1));
    let Poll::Ready(Ok(mut started)) = poll(&mut starting) else {
        panic!("expected the start to succeed");
    };
    assert!(started.check().is_ok());
    events.send(Event::Exited(1, Ok(()))).unwrap();
    let Err(error) = started.check() else {
        panic!("expected a failure");
    };
    assert_eq!(error.description(), "loop thread 1 ended while starting");
}
