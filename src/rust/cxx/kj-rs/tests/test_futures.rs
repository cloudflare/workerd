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

// Eager-by-default vs `RustFuture::lazily()` test helpers: they record when their body ran
// so the C++ driver can observe eager promises running at creation and `.lazily()` ones only
// when awaited. The bridge's generated shims always apply the eager conversion, so the
// `.lazily()` tests receive the raw `RustFuture` through the plain `extern "C"` helpers
// below instead of bridged `async fn` declarations.

static SIDE_EFFECT_COUNTER: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

pub fn reset_side_effect_counter() {
    SIDE_EFFECT_COUNTER.store(0, std::sync::atomic::Ordering::SeqCst);
}

pub fn get_side_effect_counter() -> u64 {
    SIDE_EFFECT_COUNTER.load(std::sync::atomic::Ordering::SeqCst)
}

/// Increments the side-effect counter when its body runs (first poll), then completes.
pub async fn new_side_effect_future_void() {
    SIDE_EFFECT_COUNTER.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
}

/// Same body as [`new_side_effect_future_void`], but handed to C++ as a raw `RustFuture`
/// (via [`kj_rs_demo_lazy_side_effect_future`]) and converted with `RustFuture::lazily()`:
/// the C++ promise is cold, so the counter only moves once the promise is first awaited.
pub async fn new_lazy_side_effect_future_void() {
    SIDE_EFFECT_COUNTER.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
}

/// Hands C++ the raw, not-yet-converted [`new_lazy_side_effect_future_void`] future.
///
/// `awaitables-cc-test.c++` converts it with `RustFuture::lazily()` (future.h). Plain
/// `extern "C"` because the bridge's generated `async fn` shims always apply the
/// eager-by-default `kj::Promise` conversion before C++ ever sees the future.
///
/// # Safety
///
/// `out` must point to uninitialized storage for one `::kj_rs::repr::RustFuture`
/// (future.h), which the caller takes ownership of.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kj_rs_demo_lazy_side_effect_future(
    out: *mut kj_rs::repr::RustInfallibleFuture<'static, ()>,
) {
    let fut = kj_rs::repr::infallible_future(Box::pin(new_lazy_side_effect_future_void()));
    // SAFETY: `out` points to uninitialized storage for one RustFuture, per this fn's
    // `# Safety` contract.
    unsafe { out.write(fut) };
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

/// Runs `f` on a freshly spawned thread (one with no KJ event loop) and returns its result,
/// propagating panics. Used to exercise the cross-thread arms of the `Waker` contract.
fn on_background_thread<T: Send>(f: impl FnOnce() -> T + Send) -> T {
    std::thread::scope(|scope| match scope.spawn(f).join() {
        Ok(value) => value,
        Err(payload) => std::panic::resume_unwind(payload),
    })
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

// A future that, on its first poll, stashes a clone of its waker and returns Pending WITHOUT
// waking; a later call to `wake_delayed_future()` — made by the C++ driver on the event loop's own
// thread, after poll() has already returned — wakes the stashed clone, arming the FuturePollEvent
// so the next poll completes. This exercises an *asynchronous* wake (one that arrives after poll()
// returned, via a cloned waker) on the loop thread; `wake_stashed_waker_from_background_thread()`
// below reuses the stash to exercise the same shape from a foreign thread, including after the
// future has been destroyed.

thread_local! {
    static DELAYED_WAKER: std::cell::RefCell<Option<Waker>> = const { std::cell::RefCell::new(None) };
}

struct DelayedWakeFuture {
    done: bool,
}

impl DelayedWakeFuture {
    fn new() -> Self {
        Self { done: false }
    }
}

impl Future for DelayedWakeFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut std::task::Context) -> Poll<()> {
        if self.done {
            return Poll::Ready(());
        }
        // Stash a clone of the waker for the C++ driver to wake later, on this same thread.
        DELAYED_WAKER.with(|w| *w.borrow_mut() = Some(cx.waker().clone()));
        self.done = true;
        Poll::Pending
    }
}

pub async fn new_threaded_delay_future_void() {
    DelayedWakeFuture::new().await
}

/// Wake the waker stashed by [`DelayedWakeFuture`]. Called by the C++ test driver on the event
/// loop thread, after the future's first poll has returned Pending.
pub fn wake_delayed_future() {
    DELAYED_WAKER.with(|w| {
        if let Some(waker) = w.borrow_mut().take() {
            waker.wake();
        }
    });
}

/// Take the waker stashed by [`DelayedWakeFuture`] and wake it from a spawned foreign thread
/// (joined before returning, so the wake has fully happened when this returns). The C++ driver
/// calls this either while the future is alive (the wake must arm its event, cross-thread) or
/// after the future has been destroyed (the wake must be a safe neutralized no-op — the
/// asan-visible regression for a freed `FuturePollEvent`).
pub fn wake_stashed_waker_from_background_thread() {
    let waker = DELAYED_WAKER.with(|w| w.borrow_mut().take());
    if let Some(waker) = waker {
        let handle = std::thread::spawn(move || waker.wake());
        let _ = handle.join();
    }
}

/// A future woken from a foreign thread across MULTIPLE polls: each round hands a fresh waker
/// clone to a spawned thread, which wakes it from there; the round only completes once that wake
/// has actually been delivered (spurious polls stay Pending). Exercises the cross-thread
/// fulfiller's per-poll renewal (waker.h): every round's wake must be delivered, not just the
/// first.
struct MultiRoundCrossThreadWakeFuture {
    rounds_left: u32,
    /// `Some(flag)` while a round's foreign wake is in flight; the thread sets the flag (then
    /// wakes), and the next poll that observes it completes the round.
    pending_wake: Option<Arc<std::sync::atomic::AtomicBool>>,
}

impl Future for MultiRoundCrossThreadWakeFuture {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if let Some(flag) = &self.pending_wake {
            if !flag.load(std::sync::atomic::Ordering::SeqCst) {
                // Spurious poll: this round's foreign wake hasn't been delivered yet.
                return Poll::Pending;
            }
            self.pending_wake = None;
            self.rounds_left -= 1;
        }
        if self.rounds_left == 0 {
            return Poll::Ready(());
        }
        // Start the next round: this poll's waker goes to a fresh foreign thread.
        let flag = Arc::new(std::sync::atomic::AtomicBool::new(false));
        self.pending_wake = Some(Arc::clone(&flag));
        let waker = cx.waker().clone();
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_millis(5));
            flag.store(true, std::sync::atomic::Ordering::SeqCst);
            waker.wake();
        });
        Poll::Pending
    }
}

pub async fn new_multi_round_cross_thread_wake_future_void() {
    MultiRoundCrossThreadWakeFuture {
        rounds_left: 4,
        pending_wake: None,
    }
    .await
}

/// A future woken FROM ANOTHER THREAD: its first poll clones the waker and hands it to a spawned
/// `std::thread`, which stores a flag and calls `wake()` from that thread — one with no KJ event
/// loop at all. Exercises the full cross-thread `Waker` contract (`Waker: Send + Sync`): the
/// clone crosses the thread boundary, the wake runs there, and so does the drop of that handle.
struct CrossThreadWakeFuture {
    woken: Arc<std::sync::atomic::AtomicBool>,
    spawned: bool,
}

impl Future for CrossThreadWakeFuture {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if self.woken.load(std::sync::atomic::Ordering::SeqCst) {
            return Poll::Ready(());
        }
        if !self.spawned {
            self.spawned = true;
            let waker = cx.waker().clone();
            let woken = Arc::clone(&self.woken);
            std::thread::spawn(move || {
                std::thread::sleep(std::time::Duration::from_millis(10));
                woken.store(true, std::sync::atomic::Ordering::SeqCst);
                // The wake may even race poll() returning Pending; both orders must work (the
                // cross-thread delivery coalesces into the next poll).
                waker.wake();
            });
        }
        Poll::Pending
    }
}

pub async fn new_cross_thread_wake_future_void() {
    CrossThreadWakeFuture {
        woken: Arc::new(std::sync::atomic::AtomicBool::new(false)),
        spawned: false,
    }
    .await
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

// Unwind-protection helpers (kj-rs/future.rs vtable): a panic escaping a bridged future's
// poll() must surface to C++ as a rejected kj::Promise carrying a kj::Exception, not a
// process abort. These panic at different points to cover both the fallible and infallible
// vtables and both the first-poll and event-loop-driven poll paths.

pub async fn new_panicking_future_void() -> Result<()> {
    panic!("bridged future panicked on purpose");
}

pub async fn new_panicking_infallible_future_void() {
    panic!("bridged infallible future panicked on purpose");
}

pub async fn new_panicking_after_await_future_void() -> Result<()> {
    crate::ffi::new_ready_promise_void()
        .await
        .expect("should not throw");
    panic!("bridged future panicked after a suspension point");
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

/// Like [`new_future_awaiting_cancellable_promise`], but handed to C++ as a raw
/// `RustFuture` (via [`kj_rs_demo_lazy_future_awaiting_cancellable_promise`]) and converted
/// with `RustFuture::lazily()`, so the C++ promise really is never polled unless awaited —
/// the only way to exercise the "drop a never-polled future" path now that bridged promises
/// are eager by default.
pub async fn new_lazy_future_awaiting_cancellable_promise() -> Result<()> {
    crate::ffi::new_cancellation_detecting_promise_void()
        .await
        .map_err(Error::other)?;
    Ok(())
}

/// Hands C++ the raw, not-yet-converted [`new_lazy_future_awaiting_cancellable_promise`]
/// future.
///
/// `awaitables-cc-test.c++` converts it with `RustFuture::lazily()` (future.h). Plain
/// `extern "C"` because the bridge's generated `async fn` shims always apply the
/// eager-by-default `kj::Promise` conversion before C++ ever sees the future.
///
/// # Safety
///
/// `out` must point to uninitialized storage for one `::kj_rs::repr::RustFuture`
/// (future.h), which the caller takes ownership of.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kj_rs_demo_lazy_future_awaiting_cancellable_promise(
    out: *mut kj_rs::repr::RustFuture<'static, ()>,
) {
    let fut = kj_rs::repr::future(Box::pin(kj_rs::map_err(
        new_lazy_future_awaiting_cancellable_promise(),
        file!(),
        line!(),
    )));
    // SAFETY: `out` points to uninitialized storage for one RustFuture, per this fn's
    // `# Safety` contract.
    unsafe { out.write(fut) };
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
