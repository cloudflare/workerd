use std::future;
use std::future::Future;
use std::future::IntoFuture;

use std::pin::pin;
use std::pin::Pin;

use std::sync::Arc;

use std::task::Context;
use std::task::Poll;
use std::task::Wake;
use std::task::Waker;

use crate::BoxFuture;
use crate::Result;
use crate::Error;

pub fn new_pending_future_void() -> BoxFuture<()> {
    Box::pin(std::future::pending()).into()
}
pub fn new_ready_future_void() -> BoxFuture<()> {
    Box::pin(std::future::ready(())).into()
}

use crate::ffi::CloningAction;
use crate::ffi::WakingAction;

struct WakingFuture {
    done: bool,
    cloning_action: CloningAction,
    waking_action: WakingAction,
}

impl WakingFuture {
    fn new(cloning_action: CloningAction, waking_action: WakingAction) -> Self {
        Self {
            done: false,
            cloning_action,
            waking_action,
        }
    }
}

fn do_no_clone_wake(waker: &Waker, waking_action: WakingAction) {
    match waking_action {
        WakingAction::None => {}
        WakingAction::WakeByRefSameThread => waker.wake_by_ref(),
        WakingAction::WakeByRefBackgroundThread => on_background_thread(|| waker.wake_by_ref()),
        WakingAction::WakeSameThread | WakingAction::WakeBackgroundThread => {
            panic!("cannot wake() without cloning");
        }
        _ => panic!("invalid WakingAction"),
    }
}

fn do_cloned_wake(waker: Waker, waking_action: WakingAction) {
    match waking_action {
        WakingAction::None => {}
        WakingAction::WakeByRefSameThread => waker.wake_by_ref(),
        WakingAction::WakeByRefBackgroundThread => on_background_thread(|| waker.wake_by_ref()),
        WakingAction::WakeSameThread => waker.wake(),
        WakingAction::WakeBackgroundThread => on_background_thread(move || waker.wake()),
        _ => panic!("invalid WakingAction"),
    }
}

impl Future for WakingFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut std::task::Context) -> Poll<()> {
        if self.done {
            return Poll::Ready(());
        }

        let waker = cx.waker();

        match self.cloning_action {
            CloningAction::None => {
                do_no_clone_wake(waker, self.waking_action);
            }
            CloningAction::CloneSameThread => {
                let waker = waker.clone();
                do_cloned_wake(waker, self.waking_action);
            }
            CloningAction::CloneBackgroundThread => {
                let waker = on_background_thread(|| waker.clone());
                do_cloned_wake(waker, self.waking_action);
            }
            CloningAction::WakeByRefThenCloneSameThread => {
                waker.wake_by_ref();
                let waker = waker.clone();
                do_cloned_wake(waker, self.waking_action);
            }
            _ => panic!("invalid CloningAction"),
        }

        self.done = true;
        Poll::Pending
    }
}

pub fn new_waking_future_void(
    cloning_action: CloningAction,
    waking_action: WakingAction,
) -> BoxFuture<()> {
    Box::pin(WakingFuture::new(cloning_action, waking_action)).into()
}

struct ThreadedDelayFuture {
    handle: Option<std::thread::JoinHandle<()>>,
}

impl ThreadedDelayFuture {
    fn new() -> Self {
        Self { handle: None }
    }
}

/// Run a function, `f`, on a thread in the background and return its result.
fn on_background_thread<T: Send>(f: impl FnOnce() -> T + Send) -> T {
    std::thread::scope(|scope| scope.spawn(|| f()).join().unwrap())
}

impl Future for ThreadedDelayFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut std::task::Context) -> Poll<()> {
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
            return Poll::Ready(());
        }

        let waker = cx.waker();
        let waker = on_background_thread(|| waker.clone());
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

pub fn new_layered_ready_future_void() -> BoxFuture<Result<()>> {
    Box::pin(async {
        crate::ffi::new_ready_promise_void().await.map_err(Error::other)?;
        crate::ffi::new_coroutine_promise_void().await.map_err(Error::other)
    })
    .into()
}

// From example at https://doc.rust-lang.org/std/future/fn.poll_fn.html#capturing-a-pinned-state
fn naive_select<T>(
    a: impl Future<Output = T> + Send,
    b: impl Future<Output = T> + Send,
) -> impl Future<Output = T> + Send {
    async {
        let (mut a, mut b) = (pin!(a), pin!(b));
        future::poll_fn(move |cx| {
            if let Poll::Ready(r) = a.as_mut().poll(cx) {
                Poll::Ready(r)
            } else if let Poll::Ready(r) = b.as_mut().poll(cx) {
                Poll::Ready(r)
            } else {
                Poll::Pending
            }
        })
        .await
    }
}

// A Future which polls multiple OwnPromiseNodes at once.
pub fn new_naive_select_future_void() -> BoxFuture<Result<()>> {
    Box::pin(async {
        naive_select(
            crate::ffi::new_pending_promise_void().into_future(),
            naive_select(
                crate::ffi::new_coroutine_promise_void().into_future(),
                crate::ffi::new_coroutine_promise_void().into_future(),
            ),
        ).await.map_err(Error::other)
    })
    .into()
}

struct WrappedWaker(Waker);

impl Wake for WrappedWaker {
    fn wake(self: Arc<Self>) {
        // We cannot consume our internal Waker without interior mutability, so we don't call
        // wake().
        self.0.wake_by_ref()
    }
    fn wake_by_ref(self: &Arc<Self>) {
        self.0.wake_by_ref()
    }
}

// Return a Future which awaits a KJ promise using a custom Waker implementation, opaque to KJ.
pub fn new_wrapped_waker_future_void() -> BoxFuture<Result<()>> {
    Box::pin(async {
        let mut promise = pin!(crate::ffi::new_coroutine_promise_void().into_future());
        future::poll_fn(move |cx| {
            let waker = cx.waker().clone();
            let waker = Arc::new(WrappedWaker(waker)).into();
            let mut cx = Context::from_waker(&waker);
            if let Poll::Ready(r) = promise.as_mut().poll(&mut cx) {
                Poll::Ready(r)
            } else {
                Poll::Pending
            }
        })
        .await.map_err(Error::other)
    })
    .into()
}

pub fn new_errored_future_fallible_void() -> BoxFuture<Result<()>> {
    Box::pin(std::future::ready(Err(Error::other("test error")))).into()
}

pub fn new_error_handling_future_void() -> BoxFuture<()> {
    Box::pin(async {
        let err = crate::ffi::new_errored_promise_void().await.expect_err("should throw");
        assert!(err.what().contains("test error"));
    }).into()
}

// TODO(now): Rename to new_promise_i32_awaiting_future_void
pub fn new_awaiting_future_i32() -> BoxFuture<()> {
    Box::pin(async {
        let value = crate::ffi::new_ready_promise_i32(123).await.expect("should not throw");
        assert_eq!(value, 123);
    }).into()
}

pub fn new_ready_future_fallible_i32(value: i32) -> BoxFuture<Result<i32>> {
    Box::pin(std::future::ready(Ok(value))).into()
}
