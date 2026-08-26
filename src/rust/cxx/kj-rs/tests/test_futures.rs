#![allow(clippy::unused_async)]
#![allow(clippy::semicolon_if_nothing_returned)]

use std::future;
use std::future::Future;
use std::future::IntoFuture;
use std::pin::Pin;
use std::pin::pin;
use std::sync::Arc;
use std::task::Context;
use std::task::Poll;
use std::task::Wake;
use std::task::Waker;

use crate::Error;
use crate::Result;
use crate::ffi::CloningAction;
use crate::ffi::WakingAction;

pub async fn new_pending_future_void() {
    std::future::pending().await
}

pub async fn new_ready_future_void() {
    std::future::ready(()).await
}

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

pub async fn new_waking_future_void(cloning_action: CloningAction, waking_action: WakingAction) {
    WakingFuture::new(cloning_action, waking_action).await
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
    std::thread::scope(|scope| match scope.spawn(f).join() {
        Ok(value) => value,
        Err(payload) => std::panic::resume_unwind(payload),
    })
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

pub async fn new_threaded_delay_future_void() {
    ThreadedDelayFuture::new().await
}

pub async fn new_layered_ready_future_void() -> Result<()> {
    crate::ffi::new_ready_promise_void()
        .await
        .map_err(Error::other)?;
    crate::ffi::new_coroutine_promise_void()
        .await
        .map_err(Error::other)?;
    Ok(())
}

// From example at https://doc.rust-lang.org/std/future/fn.poll_fn.html#capturing-a-pinned-state
async fn naive_select<T>(a: impl Future<Output = T>, b: impl Future<Output = T>) -> T {
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

// A Future which polls multiple OwnPromiseNodes at once.
pub async fn new_naive_select_future_void() -> Result<()> {
    naive_select(
        crate::ffi::new_pending_promise_void().into_future(),
        naive_select(
            crate::ffi::new_coroutine_promise_void().into_future(),
            crate::ffi::new_coroutine_promise_void().into_future(),
        ),
    )
    .await
    .map_err(Error::other)
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
pub async fn new_wrapped_waker_future_void() -> Result<()> {
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
    .await
    .map_err(Error::other)
}

pub async fn new_errored_future_void() -> Result<()> {
    Err(std::io::Error::other("test error"))
}

pub async fn new_kj_errored_future_void() -> std::result::Result<(), cxx::KjError> {
    Err(cxx::KjError::new(
        cxx::KjExceptionType::Overloaded,
        String::from("test error"),
    ))
}

pub async fn new_error_handling_future_void_infallible() {
    let err = crate::ffi::new_errored_promise_void()
        .await
        .expect_err("should throw");
    assert!(err.what().contains("test error"));
}

pub async fn new_promise_i32_awaiting_future_void() -> Result<()> {
    let value = crate::ffi::new_ready_promise_i32(123)
        .await
        .expect("should not throw");
    assert_eq!(value, 123);
    Ok(())
}

pub async fn new_ready_future_i32(value: i32) -> Result<i32> {
    Ok(value)
}

// =======================================================================================
// Cancellation test helpers
//
// These functions help verify that cancellation propagates correctly across the Rust/C++ async FFI
// boundary. The C++ side provides a "cancellation-detecting promise" which never resolves but
// increments a counter when it is destroyed (i.e., cancelled). These Rust async functions consume
// that promise in various ways so that the C++ test driver can verify cancellation occurred.

/// Awaits a cancellation-detecting KJ promise. When this future is cancelled by dropping the
/// enclosing `kj::Promise<T>` on the C++ side, the inner KJ promise is also cancelled, which
/// increments the cancellation counter.
pub async fn new_future_awaiting_cancellable_promise() -> Result<()> {
    crate::ffi::new_cancellation_detecting_promise_void()
        .await
        .map_err(Error::other)?;
    Ok(())
}

/// Two-step future: the first step completes normally, and the second step awaits a
/// cancellation-detecting promise that never resolves. After one poll, the future will have
/// advanced past step 1 and be suspended at step 2.
pub async fn new_two_step_cancellable_future() -> Result<()> {
    crate::ffi::new_coroutine_promise_void()
        .await
        .map_err(Error::other)?;
    crate::ffi::new_cancellation_detecting_promise_void()
        .await
        .map_err(Error::other)?;
    Ok(())
}

/// Races a coroutine promise (which resolves) against a cancellation-detecting promise (which never
/// resolves) using `naive_select`. When the coroutine wins, the cancellation-detecting promise is
/// dropped, verifying that Rust-internal cancellation propagates to sub-KJ promises.
pub async fn new_select_with_cancellation() -> Result<()> {
    naive_select(
        crate::ffi::new_coroutine_promise_void().into_future(),
        crate::ffi::new_cancellation_detecting_promise_void().into_future(),
    )
    .await
    .map_err(Error::other)
}

// =======================================================================================
// NaughtyFuture test helpers
//
// These helpers test that a RustPromiseAwaiter can survive the death of the FuturePollEvent that
// first polled it, and be correctly re-linked to a new FuturePollEvent by a subsequent poll.
//
// The pattern: phase 1 (poll_and_stash_promise_future) creates a PromiseFuture for a manually
// fulfillable KJ promise, polls it once under a KJ coroutine (linking the RustPromiseAwaiter to
// that coroutine's FuturePollEvent), then stashes the future in a thread_local. Phase 2
// (unstash_and_await_promise_future) retrieves it and awaits it under a different coroutine.
//
// We use a thread_local because we can't easily return the PromiseFuture to C++ through the FFI --
// it's a Rust trait object (dyn Future) with no CXX-compatible representation. The C++ side uses a
// file-scope variable for the fulfiller for similar reasons (kj::PromiseFulfiller has no CXX bridge
// representation).

use std::cell::RefCell;

type StashedFuture = Pin<Box<dyn Future<Output = std::result::Result<(), cxx::KjException>>>>;

thread_local! {
    static STASHED_FUTURE: RefCell<Option<StashedFuture>> = const { RefCell::new(None) };
}

/// Phase 1: Create a `PromiseFuture` for a fulfillable KJ promise, poll it once (creating the
/// `RustPromiseAwaiter` and linking it to the current `FuturePollEvent`), then stash it.
pub async fn poll_and_stash_promise_future() -> Result<()> {
    let mut future: StashedFuture =
        Box::pin(crate::ffi::new_fulfillable_promise_void().into_future());

    // Poll once to initialize the RustPromiseAwaiter and link it to our FuturePollEvent.
    let is_ready = std::future::poll_fn(|cx| match future.as_mut().poll(cx) {
        Poll::Pending => Poll::Ready(false),
        Poll::Ready(_) => Poll::Ready(true),
    })
    .await;

    assert!(!is_ready, "expected the fulfillable promise to be pending");

    STASHED_FUTURE.with(|f| {
        *f.borrow_mut() = Some(future);
    });

    Ok(())
}

/// Phase 2: Retrieve the stashed future and await it to completion under a new `FuturePollEvent`.
pub async fn unstash_and_await_promise_future() -> Result<()> {
    let future = STASHED_FUTURE.with(|f| f.borrow_mut().take().expect("no stashed future"));
    future.await.map_err(Error::other)?;
    Ok(())
}

/// Creates a cancellation-detecting promise future and immediately drops it without ever polling it.
/// This verifies that Rust's `OwnPromiseNode::drop()` correctly cancels the underlying KJ promise
/// even when no `RustPromiseAwaiter` was constructed.
pub async fn new_drop_cancellable_promise_without_polling() -> Result<()> {
    let _future = crate::ffi::new_cancellation_detecting_promise_void();
    // _future is dropped here without being .awaited
    Ok(())
}
