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

// =======================================================================================
// Multithreaded stress helpers (TSAN targets)
//
// These exist to be run under ThreadSanitizer (`bazel test --config=tsan`): they put the
// bridge's cross-thread machinery — the atomic cell refcount, the mutex-guarded cross-thread
// fulfiller, and the owner-side neutralize path — under real concurrency, so TSAN can observe
// every pair of racing accesses. workerd's tsan config instruments BOTH sides of the bridge:
// the C++ (kj mutexes and atomics) and, via `-Zsanitizer=thread` plus a TSan-built standard
// library, this Rust code and the `std::task::Waker` machinery it drives.

/// A future whose waker is woken CONCURRENTLY by `threads` foreign threads, `wakes` times each,
/// while the owning loop keeps re-polling. Each thread holds its own waker clone and calls
/// `wake_by_ref()` in a tight loop (every wake racing the owner's per-poll fulfiller renewal and
/// every other thread's wakes), then delivers one final `wake()` (consuming its clone on the
/// foreign thread). Completes once every thread's wakes have all been observed and joined.
struct WakeStormFuture {
    threads: u32,
    wakes_per_thread: u32,
    counter: Arc<std::sync::atomic::AtomicU64>,
    handles: Vec<std::thread::JoinHandle<()>>,
    spawned: bool,
}

impl Future for WakeStormFuture {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if !self.spawned {
            self.spawned = true;
            for _ in 0..self.threads {
                let waker = cx.waker().clone();
                let counter = Arc::clone(&self.counter);
                let wake_count = self.wakes_per_thread;
                self.handles.push(std::thread::spawn(move || {
                    for _ in 0..wake_count {
                        // Increment BEFORE waking: the wake that follows the final increment
                        // guarantees a subsequent poll observes the full count.
                        counter.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                        waker.wake_by_ref();
                    }
                    // Consume the clone with a final wake, on this foreign thread.
                    waker.wake();
                }));
            }
            return Poll::Pending;
        }

        let target = u64::from(self.threads) * u64::from(self.wakes_per_thread);
        if self.counter.load(std::sync::atomic::Ordering::SeqCst) < target {
            return Poll::Pending;
        }
        // All wakes delivered; the threads have nothing left but their final wake, so joining
        // here is near-instant and never blocks the loop meaningfully.
        for handle in self.handles.drain(..) {
            let _ = handle.join();
        }
        Poll::Ready(())
    }
}

pub async fn new_wake_storm_future_void(threads: u32, wakes_per_thread: u32) {
    WakeStormFuture {
        threads,
        wakes_per_thread,
        counter: Arc::new(std::sync::atomic::AtomicU64::new(0)),
        handles: Vec::new(),
        spawned: false,
    }
    .await
}

/// Pure refcount churn: `threads` foreign threads each clone-and-drop the waker
/// `clones_per_thread` times concurrently (hammering the cell's atomic refcount from every
/// direction, including dropping clones on foreign threads), then wake once to report done.
struct CloneStormFuture {
    threads: u32,
    clones_per_thread: u32,
    finished: Arc<std::sync::atomic::AtomicU64>,
    handles: Vec<std::thread::JoinHandle<()>>,
    spawned: bool,
}

impl Future for CloneStormFuture {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if !self.spawned {
            self.spawned = true;
            for _ in 0..self.threads {
                let waker = cx.waker().clone();
                let finished = Arc::clone(&self.finished);
                let clones = self.clones_per_thread;
                self.handles.push(std::thread::spawn(move || {
                    for _ in 0..clones {
                        // Clone-of-clone and drop, all on this foreign thread.
                        drop(waker.clone());
                    }
                    finished.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                    waker.wake();
                }));
            }
            return Poll::Pending;
        }

        if self.finished.load(std::sync::atomic::Ordering::SeqCst) < u64::from(self.threads) {
            return Poll::Pending;
        }
        for handle in self.handles.drain(..) {
            let _ = handle.join();
        }
        Poll::Ready(())
    }
}

pub async fn new_clone_storm_future_void(threads: u32, clones_per_thread: u32) {
    CloneStormFuture {
        threads,
        clones_per_thread,
        finished: Arc::new(std::sync::atomic::AtomicU64::new(0)),
        handles: Vec::new(),
        spawned: false,
    }
    .await
}

// Stashed-waker storm: the pieces for racing foreign-thread wakes against the owning thread
// DESTROYING the future (the neutralize path). The future stashes waker clones into a global,
// then stays Pending forever; the C++ driver starts a storm of threads waking those clones in a
// loop, destroys the future mid-storm (running ~FuturePollEvent / neutralize() concurrently with
// in-flight wakeByRef calls), and only then joins the storm and drops the stashed clones (some
// on a foreign thread, after the event is long dead).

static STASHED_WAKERS: std::sync::Mutex<Vec<Waker>> = std::sync::Mutex::new(Vec::new());
static STORM_HANDLES: std::sync::Mutex<Vec<std::thread::JoinHandle<()>>> =
    std::sync::Mutex::new(Vec::new());

struct StashWakersFuture {
    clones: u32,
    stashed: bool,
}

impl Future for StashWakersFuture {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if !self.stashed {
            self.stashed = true;
            let mut stash = STASHED_WAKERS.lock().expect("stash poisoned");
            for _ in 0..self.clones {
                stash.push(cx.waker().clone());
            }
        }
        // Never completes; the C++ driver cancels (drops) it mid-storm.
        Poll::Pending
    }
}

/// Stash `clones` waker clones into the global stash on first poll, then stay Pending forever.
pub async fn new_stash_wakers_future_void(clones: u32) {
    StashWakersFuture {
        clones,
        stashed: false,
    }
    .await
}

/// Spawn `threads` foreign threads, each waking every stashed waker `iterations` times. The
/// threads run concurrently with whatever the C++ driver does next — including destroying the
/// future the wakers belong to.
pub fn start_wake_storm(threads: u32, iterations: u32) {
    let mut handles = STORM_HANDLES.lock().expect("handles poisoned");
    for _ in 0..threads {
        handles.push(std::thread::spawn(move || {
            for _ in 0..iterations {
                let stash = STASHED_WAKERS.lock().expect("stash poisoned");
                for waker in stash.iter() {
                    waker.wake_by_ref();
                }
            }
        }));
    }
}

/// Join every thread started by [`start_wake_storm`].
pub fn join_wake_storm() {
    let handles = std::mem::take(&mut *STORM_HANDLES.lock().expect("handles poisoned"));
    for handle in handles {
        let _ = handle.join();
    }
}

/// Drop all stashed waker clones on a spawned foreign thread (joined before returning). When the
/// C++ driver calls this after destroying the future, the drops release the last references to
/// the (neutralized) waker cell from a thread that never had a KJ event loop.
pub fn clear_stashed_wakers_on_background_thread() {
    let stash = std::mem::take(&mut *STASHED_WAKERS.lock().expect("stash poisoned"));
    let handle = std::thread::spawn(move || drop(stash));
    let _ = handle.join();
}

// Shared (non-thread-local) single-waker stash: lets the C++ driver wake a future's waker from
// contexts the thread-local stash can't reach — another thread running a DIFFERENT KJ event
// loop (the waker must recognize that thread's executor is not its own), or the original thread
// after its event loop has been destroyed.

static SHARED_STASHED_WAKER: std::sync::Mutex<Option<Waker>> = std::sync::Mutex::new(None);
static SHARED_STASH_WOKEN: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);

struct SharedStashWakerFuture;

impl Future for SharedStashWakerFuture {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if SHARED_STASH_WOKEN.swap(false, std::sync::atomic::Ordering::SeqCst) {
            return Poll::Ready(());
        }
        *SHARED_STASHED_WAKER.lock().expect("stash poisoned") = Some(cx.waker().clone());
        Poll::Pending
    }
}

/// Stash a waker clone into the shared (cross-thread-visible) slot; completes after
/// [`wake_shared_stashed_waker`] runs.
pub async fn new_shared_stash_waker_future_void() {
    SharedStashWakerFuture.await
}

/// Take the shared stashed waker and wake it from WHATEVER thread this is called on — the C++
/// driver calls it from a thread running a different KJ event loop (the wake must take the
/// cross-thread path even though a loop IS current there), or after the owning loop has been
/// destroyed entirely (the wake must be a quiet no-op). Consumes the stashed clone; a no-op if
/// nothing is stashed.
pub fn wake_shared_stashed_waker() {
    let waker = SHARED_STASHED_WAKER.lock().expect("stash poisoned").take();
    if let Some(waker) = waker {
        SHARED_STASH_WOKEN.store(true, std::sync::atomic::Ordering::SeqCst);
        waker.wake();
    }
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

/// Drop the stashed future without awaiting it. The C++ driver calls this AFTER destroying the
/// KJ event loop the future belongs to: the drop glue (`~PromiseAwaiter` →
/// `~GuardedRustPromiseAwaiter` → `~RustPromiseAwaiter`, cancelling the inner promise) must
/// tolerate running on a thread with no event loop at all — the same teardown shape as tokio
/// drop glue running after `~EventLoop`, which `ExecutorGuarded`'s destructor explicitly permits.
pub fn drop_stashed_future() {
    STASHED_FUTURE.with(|f| {
        *f.borrow_mut() = None;
    });
}

/// Polls a pending KJ promise with custom (non-KJ) Wakers and asserts the awaiter's exact
/// stored-clone behavior via `Arc` strong counts:
///
/// - first poll stores a clone of the waker (count +1),
/// - re-polling with the SAME waker keeps the stored clone (`Waker::will_wake` — no re-clone),
/// - polling with a DIFFERENT waker drops the old clone and stores the new one,
/// - dropping the future (cancelling the promise) drops the stored clone.
pub async fn new_waker_reuse_and_replace_future_void() -> Result<()> {
    struct NoopWake;
    #[expect(
        clippy::manual_noop_waker,
        reason = "Waker::noop() is a shared static; this test observes the Arc's strong count \
                  to verify the awaiter's clone/reuse/drop behavior, which needs a real Arc-backed \
                  waker per identity"
    )]
    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    let arc1 = Arc::new(NoopWake);
    let arc2 = Arc::new(NoopWake);
    // The wakers outlive the inner block, so the only thing dropped at its end is the future
    // (and with it the awaiter's stored clone).
    let waker1: Waker = Arc::clone(&arc1).into();
    let waker2: Waker = Arc::clone(&arc2).into();
    {
        let mut promise = pin!(crate::ffi::new_pending_promise_void().into_future());

        let mut cx1 = Context::from_waker(&waker1);
        assert!(promise.as_mut().poll(&mut cx1).is_pending());
        assert_eq!(
            Arc::strong_count(&arc1),
            3,
            "first poll must store one clone"
        );

        assert!(promise.as_mut().poll(&mut cx1).is_pending());
        assert_eq!(
            Arc::strong_count(&arc1),
            3,
            "re-poll with the same waker must keep the stored clone (will_wake), not re-clone"
        );

        let mut cx2 = Context::from_waker(&waker2);
        assert!(promise.as_mut().poll(&mut cx2).is_pending());
        assert_eq!(
            Arc::strong_count(&arc1),
            2,
            "polling with a different waker must drop the old stored clone"
        );
        assert_eq!(Arc::strong_count(&arc2), 3, "…and store the new one");

        // `promise` (and with it the awaiter and its stored clone) drops here, cancelling the
        // pending KJ promise.
    }
    assert_eq!(
        Arc::strong_count(&arc2),
        2,
        "dropping the future must drop the stored clone"
    );
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

// =======================================================================================
// Coverage-gap helpers: uncovered branches of the bridge, each driven by a C++ test.

/// Same-thread wake storm inside a single poll: `wakes` `wake_by_ref()` calls on the borrowed
/// waker, all during `poll()`. `Event::armDepthFirst()` must coalesce them into one re-poll.
struct SyncWakeStormFuture {
    wakes: u32,
    done: bool,
}

impl Future for SyncWakeStormFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if self.done {
            return Poll::Ready(());
        }
        for _ in 0..self.wakes {
            cx.waker().wake_by_ref();
        }
        self.done = true;
        Poll::Pending
    }
}

pub async fn new_sync_wake_storm_future_void(wakes: u32) {
    SyncWakeStormFuture { wakes, done: false }.await
}

/// Stashes a waker clone (into the same slot `new_threaded_delay_future_void` uses) and then
/// completes IMMEDIATELY. Waking the stash afterwards arms a `FuturePollEvent` whose future is
/// already done: `FutureAwaiter::poll()` must take its `isDone()` early return rather than poll
/// a completed future again (which is a contract violation that panics for `async fn` futures).
struct StashThenReadyFuture;

impl Future for StashThenReadyFuture {
    type Output = ();
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        DELAYED_WAKER.with(|w| *w.borrow_mut() = Some(cx.waker().clone()));
        Poll::Ready(())
    }
}

pub async fn new_stash_then_ready_future_void() {
    StashThenReadyFuture.await
}

/// A Rust future parked on a KJ promise that never resolves, for driving KJ's async tracing
/// (`kj::Promise::trace()`) through the bridge: `FuturePollEvent::tracePromise()` follows its
/// first `leaves` entry into `RustPromiseAwaiter::tracePromise()`.
pub async fn new_await_pending_promise_future_void() -> Result<()> {
    crate::ffi::new_pending_promise_void()
        .await
        .map_err(Error::other)
}

/// Optimized -> generic path transition on one `RustPromiseAwaiter`. Polled under a C++ `co_await`:
///
/// 1. the fulfillable promise is first polled with the real (`PollWaker`) context, taking the
///    optimized path and linking to the enclosing `FuturePollEvent`;
/// 2. it is then polled with a custom, non-KJ waker (which records being woken and forwards to
///    the real waker), switching the awaiter to the generic path: the link is cleared and a
///    clone of the custom waker is stored;
/// 3. we return Pending. The C++ driver fulfills the promise; `fire()` must now wake the STORED
///    custom waker (not arm the event directly). Its forward re-polls us, and we assert the
///    custom waker really was the one woken before completing.
struct OptimizedThenGenericFuture {
    promise: Pin<Box<dyn Future<Output = std::result::Result<(), cxx::KjException>>>>,
    custom_woken: Arc<std::sync::atomic::AtomicBool>,
    switched: bool,
}

struct RecordingWaker {
    woken: Arc<std::sync::atomic::AtomicBool>,
    forward: Waker,
}

impl Wake for RecordingWaker {
    fn wake(self: Arc<Self>) {
        self.woken.store(true, std::sync::atomic::Ordering::SeqCst);
        self.forward.wake_by_ref();
    }
}

impl Future for OptimizedThenGenericFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if !self.switched {
            // Step 1: optimized path (real PollWaker context).
            assert!(
                self.promise.as_mut().poll(cx).is_pending(),
                "fulfillable promise must start pending"
            );
            // Step 2: generic path (custom waker context).
            let custom: Waker = Arc::new(RecordingWaker {
                woken: Arc::clone(&self.custom_woken),
                forward: cx.waker().clone(),
            })
            .into();
            let mut custom_cx = Context::from_waker(&custom);
            assert!(self.promise.as_mut().poll(&mut custom_cx).is_pending());
            self.switched = true;
            return Poll::Pending;
        }
        // Step 3: woken. Prove it was via the stored custom waker.
        assert!(
            self.custom_woken.load(std::sync::atomic::Ordering::SeqCst),
            "fire() must have woken the stored custom waker (generic path), not the event"
        );
        match self.promise.as_mut().poll(cx) {
            Poll::Ready(result) => {
                result.expect("promise should resolve successfully");
                Poll::Ready(())
            }
            Poll::Pending => Poll::Pending,
        }
    }
}

pub async fn new_optimized_then_generic_future_void() {
    OptimizedThenGenericFuture {
        promise: Box::pin(crate::ffi::new_fulfillable_promise_void().into_future()),
        custom_woken: Arc::new(std::sync::atomic::AtomicBool::new(false)),
        switched: false,
    }
    .await
}

/// Generic -> optimized transition: after the generic path stored a clone of a custom waker,
/// re-polling under the real `PollWaker` context must DROP that clone (the optimized link
/// replaces it). Asserted via the custom waker's Arc strong count, with the promise still alive.
pub async fn new_generic_then_optimized_drops_clone_future_void() -> Result<()> {
    struct NoopWake;
    #[expect(
        clippy::manual_noop_waker,
        reason = "the Arc's strong count is the instrument; Waker::noop() is a shared static"
    )]
    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    let arc = Arc::new(NoopWake);
    let custom: Waker = Arc::clone(&arc).into();
    let mut promise = Box::pin(crate::ffi::new_pending_promise_void().into_future());

    std::future::poll_fn(|cx| {
        let mut custom_cx = Context::from_waker(&custom);
        assert!(promise.as_mut().poll(&mut custom_cx).is_pending());
        assert_eq!(
            Arc::strong_count(&arc),
            3,
            "generic path must store a clone"
        );

        // Now the real PollWaker context: optimized path, link the event, drop the clone.
        assert!(promise.as_mut().poll(cx).is_pending());
        assert_eq!(
            Arc::strong_count(&arc),
            2,
            "switching to the optimized path must drop the stored custom-waker clone"
        );
        Poll::Ready(())
    })
    .await;
    Ok(())
}

/// Polls a fulfillable KJ promise with a Context built from a CLONE of the KJ waker (a
/// `FutureWakerCell`-backed `Waker`, not the borrowed `PollWaker`). `try_poll_waker` cannot see a
/// `PollWaker` behind it, so the awaiter takes the generic path and stores a clone of a KJ cell
/// waker; when the promise fires, waking that clone arms our own `FuturePollEvent` through the
/// cell (same-thread), which re-polls us to completion.
struct ClonedKjWakerContextFuture {
    promise: Pin<Box<dyn Future<Output = std::result::Result<(), cxx::KjException>>>>,
    started: bool,
}

impl Future for ClonedKjWakerContextFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if !self.started {
            self.started = true;
            let cloned = cx.waker().clone();
            let mut cloned_cx = Context::from_waker(&cloned);
            assert!(self.promise.as_mut().poll(&mut cloned_cx).is_pending());
            return Poll::Pending;
        }
        match self.promise.as_mut().poll(cx) {
            Poll::Ready(result) => {
                result.expect("promise should resolve successfully");
                Poll::Ready(())
            }
            Poll::Pending => Poll::Pending,
        }
    }
}

pub async fn new_cloned_kj_waker_context_future_void() {
    ClonedKjWakerContextFuture {
        promise: Box::pin(crate::ffi::new_fulfillable_promise_void().into_future()),
        started: false,
    }
    .await
}

// Cross-loop ping-pong: two futures on two KJ event loops (two threads) wake EACH OTHER for a
// number of rounds. Every wake is a cross-loop wake taken while both loops are live and
// racing, so the owning-executor check must route each one to the right loop every time.
// A side only counts a round when its peer actually woke it (WOKEN flag), so spurious polls
// don't skew the count; a side that finishes wakes its peer one last time and marks itself
// DONE so the peer terminates too.

static PING_PONG_SLOTS: [std::sync::Mutex<Option<Waker>>; 2] =
    [std::sync::Mutex::new(None), std::sync::Mutex::new(None)];
static PING_PONG_WOKEN: [std::sync::atomic::AtomicBool; 2] = [
    std::sync::atomic::AtomicBool::new(false),
    std::sync::atomic::AtomicBool::new(false),
];
static PING_PONG_DONE: [std::sync::atomic::AtomicBool; 2] = [
    std::sync::atomic::AtomicBool::new(false),
    std::sync::atomic::AtomicBool::new(false),
];

struct PingPongFuture {
    id: usize,
    rounds_left: u32,
}

/// Take the peer's stashed waker (releasing the slot lock first) and wake it, flagging the wake
/// so the peer counts it as a round.
fn wake_peer(peer: usize) {
    let taken = PING_PONG_SLOTS[peer].lock().expect("slot poisoned").take();
    if let Some(w) = taken {
        PING_PONG_WOKEN[peer].store(true, std::sync::atomic::Ordering::SeqCst);
        w.wake();
    }
}

impl Future for PingPongFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        use std::sync::atomic::Ordering::SeqCst;
        let me = self.id;
        let peer = 1 - me;
        if PING_PONG_WOKEN[me].swap(false, SeqCst) {
            self.rounds_left -= 1;
        }
        if self.rounds_left == 0 || PING_PONG_DONE[peer].load(SeqCst) {
            PING_PONG_DONE[me].store(true, SeqCst);
            wake_peer(peer);
            return Poll::Ready(());
        }
        *PING_PONG_SLOTS[me].lock().expect("slot poisoned") = Some(cx.waker().clone());
        wake_peer(peer);
        Poll::Pending
    }
}

/// Reset the ping-pong state. Call before starting either side.
pub fn reset_ping_pong() {
    use std::sync::atomic::Ordering::SeqCst;
    for i in 0..2 {
        *PING_PONG_SLOTS[i].lock().expect("slot poisoned") = None;
        PING_PONG_WOKEN[i].store(false, SeqCst);
        PING_PONG_DONE[i].store(false, SeqCst);
    }
}

/// One side of the ping-pong (`side` is 0 or 1), completing after `rounds` peer wakes or as
/// soon as the peer reports done.
pub async fn new_ping_pong_future_void(side: u32, rounds: u32) {
    PingPongFuture {
        id: side as usize,
        rounds_left: rounds,
    }
    .await
}

/// A future that is never ready and whose `Drop` panics. Dropping it from C++ (cancelling the
/// bridged promise) hits `RustFuture::drop_in_place`, which has no error channel: the bridge
/// documents a deterministic labeled abort there rather than an unwind into C++. The C++ death
/// test asserts exactly that.
struct PanicOnDropFuture;

impl Future for PanicOnDropFuture {
    type Output = ();
    fn poll(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<()> {
        Poll::Pending
    }
}

impl Drop for PanicOnDropFuture {
    fn drop(&mut self) {
        panic!("deliberate panic in a bridged future's Drop");
    }
}

pub async fn new_panic_on_drop_future_void() {
    PanicOnDropFuture.await;
}

/// A Rust future that simply awaits the C++ test's manually fulfillable promise
/// (`new_fulfillable_promise_void` / `fulfill_stored_promise`): a bridged future that is
/// re-polled -- same-thread, through its `RustPromiseAwaiter` leaf -- exactly when the C++ test
/// fulfills the promise.
pub async fn new_await_fulfillable_promise_future_void() -> Result<()> {
    crate::ffi::new_fulfillable_promise_void()
        .await
        .map_err(Error::other)
}
