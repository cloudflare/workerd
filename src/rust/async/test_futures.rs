use std::pin::Pin;
use std::future::Future;
use std::task::Poll;

use crate::BoxFuture;

pub fn new_pending_future_void() -> BoxFuture<()> {
    Box::pin(std::future::pending()).into()
}
pub fn new_ready_future_void() -> BoxFuture<()> {
    Box::pin(std::future::ready(())).into()
}

// TODO(now): Make configurable:
// - Synchronous wake_by_ref()
// - Synchronous clone().wake()
// - wake_by_ref() on different thread
// - clone().wake() on different thread
// - List of different wake styles
struct WakingFuture {
    done: bool,
}

impl WakingFuture {
    fn new() -> Self {
        Self { done: false }
    }
}

impl Future for WakingFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut std::task::Context) -> Poll<()> {
        let waker = cx.waker();
        // waker.clone().wake();
        waker.wake_by_ref();
        if self.done {
            Poll::Ready(())
        } else {
            self.done = true;
            Poll::Pending
        }
    }
}

pub fn new_waking_future_void() -> BoxFuture<()> {
    Box::pin(WakingFuture::new()).into()
}

struct ThreadedDelayFuture {
    handle: Option<std::thread::JoinHandle<()>>,
}

impl ThreadedDelayFuture {
    fn new() -> Self {
        Self { handle: None }
    }
}

impl Future for ThreadedDelayFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut std::task::Context) -> Poll<()> {
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
            return Poll::Ready(());
        }
        let waker = cx.waker();
        let waker = std::thread::scope(|scope| scope.spawn(|| waker.clone()).join().unwrap());
        self.handle = Some(std::thread::spawn(|| {
            std::thread::sleep(std::time::Duration::from_millis(100));
            waker.wake();
        }));
        Poll::Pending
    }
}

pub fn new_threaded_delay_future_void() -> BoxFuture<()> {
    Box::pin(ThreadedDelayFuture::new()).into()
}

pub fn new_layered_ready_future_void() -> BoxFuture<()> {
    Box::pin(async {
        crate::ffi::new_ready_promise_node().await;
        crate::ffi::new_coroutine_promise_node().await;
    }).into()
}
