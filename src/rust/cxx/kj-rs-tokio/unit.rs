//! [`LoopUnit`]: a KJ event loop driven by a task of a multi-thread tokio runtime.
//!
//! A unit is a KJ event loop with everything created on it, and it is a `Send` future: the
//! runtime polls it on whichever worker thread picks it up, and each poll enters the loop on
//! that thread, turns its events until none is left, and leaves the loop again. So a process can
//! have more loops than threads, and work stealing moves a loop that is ready to a thread that
//! is free. The C++ half and the KJ facts this rests on are in `loop-unit.h`.
//!
//! # What runs on the loop
//!
//! The unit's root future (what `make` returns) is polled by the KJ loop, as every Rust future
//! C++ awaits is, and so is everything it awaits. It need not be `Send`, and neither does
//! anything it creates: `Rc`s, KJ objects, kj-rs-io and kj-hyper objects. Only `make` itself and
//! the root future's output cross between threads as values, and they are `Send` by type.
//!
//! tokio resources (sockets, timers, channels) register with the shared runtime's drivers, which
//! some worker turns. Their wake-ups reach the loop as cross-thread wake-ups do
//! (`kj::EventPort::wake()`): the task is woken, and its next poll delivers them. There is no
//! `LocalSet`, so [`spawn`](crate::spawn) is not available in a unit; `tokio::spawn` is, for
//! futures that are `Send`.
//!
//! # The `Send` claim
//!
//! `LoopUnit: Send` is an `unsafe impl` (`ffi.rs`, `UnitContext`), which says where each
//! condition it depends on is enforced. In short: nothing of the unit is reachable from outside
//! it except through thread-safe handles, no thread-local holds a piece of it between polls, a
//! unit that has a suspended fiber is not moved (it is pinned, and blocks its thread instead),
//! and it is destroyed with its loop entered.

use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::sync::Condvar;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;
use std::time::Duration;

use cxx::KjError;
use cxx::KjExceptionType;
use tokio::runtime::Handle;
use tokio::runtime::RuntimeFlavor;
use tokio::time::Instant;
use tokio::time::Sleep;

use crate::ffi::LoopUnitContext;
use crate::ffi::UnitContext;
use crate::port::EnteredLoop;

/// How many events one poll turns before it yields to the runtime with more ready, so that a
/// worker returns to the scheduler (and, in its turn, to the I/O driver) even when a loop never
/// runs dry. A latency/throughput compromise; safe to retune.
const MAX_TURNS_PER_POLL: u32 = 1024;

/// A timer is not re-armed for a deadline less than this much sooner than the armed one: the
/// loop reports its next deadline as a timeout from now, which jitters from poll to poll.
const TIMER_SLACK: Duration = Duration::from_micros(250);

/// `LoopUnitContext::turn()`'s results (`UNIT_*` in `loop-unit.h`).
const UNIT_DONE: u8 = 0;
const UNIT_IDLE: u8 = 1;
const UNIT_YIELD: u8 = 2;
const UNIT_PINNED: u8 = 3;
/// `LoopUnitContext::nextTimerNs()` without a timer.
const UNIT_NO_TIMER: u64 = u64::MAX;

fn failed(message: &str) -> KjError {
    KjError::new(KjExceptionType::Failed, message.to_owned())
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    // Nothing here leaves the state half-updated, so a poisoned lock is as good as any.
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

/// What a unit's port shares with the threads that wake it.
struct WakeState {
    /// The `kj::EventPort::wake()` latch: set by `wake`, taken by `take`.
    woken: AtomicBool,
    /// The waker of the task that drives the unit, as of its last poll.
    waker: Mutex<Option<Waker>>,
    /// Ends `block`.
    unblocked: Condvar,
}

/// The Rust half of a `UnitEventPort` (C++): the wake latch, and how to wake the task.
pub struct UnitWake(Arc<WakeState>);

impl UnitWake {
    /// `kj::EventPort::wake()`, from any thread.
    pub(crate) fn wake(&self) {
        self.0.woken.store(true, Ordering::SeqCst);
        // Under the lock, so that a `block` that has seen the latch clear is waiting by the
        // time it is notified.
        if let Some(waker) = lock(&self.0.waker).as_ref() {
            waker.wake_by_ref();
        }
        self.0.unblocked.notify_all();
    }

    /// Takes the wake latch.
    pub(crate) fn take(&self) -> bool {
        self.0.woken.swap(false, Ordering::SeqCst)
    }

    /// Blocks the thread until the wake latch is set or `timeout_ns` passed (`UNIT_NO_TIMER`:
    /// until the latch is set).
    pub(crate) fn block(&self, timeout_ns: u64) {
        let deadline =
            (timeout_ns != UNIT_NO_TIMER).then(|| std::time::Instant::now() + nanos(timeout_ns));
        let mut guard = lock(&self.0.waker);
        while !self.0.woken.load(Ordering::SeqCst) {
            let unblocked = &self.0.unblocked;
            guard = match deadline {
                None => unblocked
                    .wait(guard)
                    .unwrap_or_else(PoisonError::into_inner),
                Some(deadline) => {
                    let left = deadline.saturating_duration_since(std::time::Instant::now());
                    if left.is_zero() {
                        break;
                    }
                    let waited = unblocked.wait_timeout(guard, left);
                    waited.unwrap_or_else(PoisonError::into_inner).0
                }
            };
        }
        drop(guard);
    }
}

fn nanos(ns: u64) -> Duration {
    Duration::from_nanos(ns)
}

/// Makes a unit's root future, on the loop, given the loop's context.
type MakeUnitFuture = dyn FnOnce(Pin<&mut LoopUnitContext>) -> UnitFuture + Send;

/// What makes a unit's root future, on the loop.
pub struct UnitTask(Box<MakeUnitFuture>);

/// A unit's root future.
pub struct UnitFuture(Pin<Box<dyn Future<Output = ()>>>);

/// Called by `LoopUnitContext::start()` with the loop entered.
#[expect(
    clippy::boxed_local,
    clippy::unnecessary_box_returns,
    reason = "the bridge hands an opaque Rust type to C++ and back as a Box"
)]
pub fn unit_task_start(task: Box<UnitTask>, context: Pin<&mut LoopUnitContext>) -> Box<UnitFuture> {
    Box::new((task.0)(context))
}

/// What the loop polls as the unit's root promise.
pub async fn unit_future_run(future: Box<UnitFuture>) {
    future.0.await;
}

/// What a unit has on the stack of every thread that enters it, around the entry: for a C++
/// frame that must be there while the loop's code runs, such as a `kj::ExceptionCallback`.
pub trait UnitScope: Send + Unpin + 'static {
    /// Calls `body` once.
    fn enter(&self, body: &mut dyn FnMut());
}

/// No scope.
impl UnitScope for () {
    fn enter(&self, body: &mut dyn FnMut()) {
        body();
    }
}

/// The loop and what drives it. Everything that belongs to the loop is behind `context`.
struct Core {
    /// Until the first poll.
    task: Option<Box<UnitTask>>,
    /// From the first poll until the unit is done.
    context: Option<UnitContext>,
    wake: Arc<WakeState>,
    /// The runtime of the first poll.
    handle: Option<Handle>,
    /// Wakes the task when the loop's next timer is due.
    sleep: Option<Pin<Box<Sleep>>>,
    /// What `sleep` is armed for, while it has not fired.
    armed: Option<Instant>,
    /// How many events one poll turns before it yields (`MAX_TURNS_PER_POLL`).
    max_turns: u32,
    done: bool,
}

impl Core {
    /// One poll: enters the loop and turns it until it is done or has nothing to do.
    fn drive(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), KjError>> {
        if self.done {
            return Poll::Ready(Err(failed("a loop unit was polled after it completed")));
        }
        let waker = cx.waker();
        {
            let mut slot = lock(&self.wake.waker);
            if !slot
                .as_ref()
                .is_some_and(|current| current.will_wake(waker))
            {
                *slot = Some(waker.clone());
            }
        }
        let handle = match &self.handle {
            Some(handle) => handle.clone(),
            None => match Handle::try_current() {
                // A pinned unit blocks its thread and leaves the rest of the runtime running
                // (`block_in_place`), which takes other threads.
                Ok(handle) if handle.runtime_flavor() == RuntimeFlavor::MultiThread => {
                    self.handle.insert(handle).clone()
                }
                _ => {
                    return self.end(Err(failed(
                        "a loop unit is driven by a task of a multi-thread tokio runtime",
                    )));
                }
            },
        };
        let Some(entered) = EnteredLoop::new(handle) else {
            // The loop cannot be entered here, so nothing of it can be destroyed here either:
            // that is left to the drop.
            self.done = true;
            return Poll::Ready(Err(failed(
                "a loop unit was polled on a thread that has a KJ event loop entered",
            )));
        };
        let result = self.turn(cx);
        let result = match result {
            Poll::Ready(result) => self.end(result),
            Poll::Pending => Poll::Pending,
        };
        drop(entered);
        result
    }

    /// The unit is over: what is left of the loop is destroyed, here, with it entered.
    fn end(&mut self, result: Result<(), KjError>) -> Poll<Result<(), KjError>> {
        self.done = true;
        self.task = None;
        self.context = None;
        self.sleep = None;
        Poll::Ready(result)
    }

    fn turn(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), KjError>> {
        if self.context.is_none() {
            let wake = Box::new(UnitWake(Arc::clone(&self.wake)));
            let context = self
                .context
                .insert(UnitContext(crate::ffi::new_loop_unit_context(wake)));
            if let Some(task) = self.task.take()
                && let Err(error) = context.0.as_mut().start(task)
            {
                return Poll::Ready(Err(error.into()));
            }
        }
        let Some(context) = &mut self.context else {
            unreachable!("the context was just made");
        };
        loop {
            let turned = match context.0.as_mut().turn(self.max_turns) {
                Ok(turned) => turned,
                Err(error) => return Poll::Ready(Err(error.into())),
            };
            match turned {
                UNIT_DONE => {
                    return Poll::Ready(context.0.as_mut().finish().map_err(Into::into));
                }
                UNIT_YIELD => {
                    cx.waker().wake_by_ref();
                    return Poll::Pending;
                }
                UNIT_PINNED => {
                    // The unit stays on this thread until it is no longer pinned. The worker's
                    // other tasks, and the runtime's drivers, move to another thread meanwhile.
                    let pinned = tokio::task::block_in_place(|| context.0.as_mut().turn_pinned());
                    if let Err(error) = pinned {
                        return Poll::Ready(Err(error.into()));
                    }
                }
                UNIT_IDLE => {
                    let timeout = context.0.as_mut().next_timer_ns();
                    if timeout == UNIT_NO_TIMER {
                        return Poll::Pending;
                    }
                    if timeout == 0 {
                        // Due already: the next turn's poll of the port fires it.
                        continue;
                    }
                    let deadline = Instant::now() + nanos(timeout);
                    let rearm = self
                        .armed
                        .is_none_or(|armed| deadline + TIMER_SLACK < armed);
                    let sleep = match &mut self.sleep {
                        Some(sleep) => {
                            if rearm {
                                sleep.as_mut().reset(deadline);
                            }
                            sleep
                        }
                        None => self
                            .sleep
                            .insert(Box::pin(tokio::time::sleep_until(deadline))),
                    };
                    if rearm {
                        self.armed = Some(deadline);
                    }
                    if sleep.as_mut().poll(cx).is_pending() {
                        return Poll::Pending;
                    }
                    // Fired; if that was early for the loop's timer, the next round re-arms.
                    self.armed = None;
                }
                other => unreachable!("LoopUnitContext::turn() returned {other}"),
            }
        }
    }
}

/// A KJ event loop with everything on it, as a `Send` future (see the module doc).
///
/// Spawn it on a multi-thread runtime with [`spawn_unit`], or poll it inside
/// `tokio::task::coop::unconstrained`: the loop polls tokio resources many times in one poll of
/// the unit, and under a task's cooperative budget they would soon all report that they are not
/// ready.
///
/// It resolves to the root future's output once that future is done, or to the error of a
/// `kj::Exception` that escaped the loop. However it ends, and when it is dropped before it
/// ends, everything created on the loop is destroyed first, with the loop entered: by the poll
/// that completes it, or by the drop, on the thread that drops it, which must not have a KJ
/// loop entered (the unit is leaked otherwise).
pub struct LoopUnit<T, S: UnitScope = ()> {
    core: Core,
    output: Arc<Mutex<Option<T>>>,
    scope: S,
}

impl<T: Send + 'static, S: UnitScope> LoopUnit<T, S> {
    /// A unit whose root future is `make`'s, called on the loop at the first poll with the
    /// loop's context, which outlives the future. Every thread enters the loop inside
    /// `scope.enter()`.
    pub fn new<F>(
        scope: S,
        make: impl FnOnce(Pin<&mut LoopUnitContext>) -> F + Send + 'static,
    ) -> Self
    where
        F: Future<Output = T> + 'static,
    {
        let output = Arc::new(Mutex::new(None));
        let task = {
            let output = Arc::clone(&output);
            UnitTask(Box::new(move |context| {
                let future = make(context);
                UnitFuture(Box::pin(async move {
                    let value = future.await;
                    *lock(&output) = Some(value);
                }))
            }))
        };
        Self {
            core: Core {
                task: Some(Box::new(task)),
                context: None,
                wake: Arc::new(WakeState {
                    woken: AtomicBool::new(false),
                    waker: Mutex::new(None),
                    unblocked: Condvar::new(),
                }),
                handle: None,
                sleep: None,
                armed: None,
                max_turns: MAX_TURNS_PER_POLL,
                done: false,
            },
            output,
            scope,
        }
    }
}

impl<T: Send + 'static, S: UnitScope> Future for LoopUnit<T, S> {
    type Output = Result<T, KjError>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        let core = &mut this.core;
        let mut result = Poll::Pending;
        this.scope.enter(&mut || result = core.drive(cx));
        match result {
            Poll::Pending => Poll::Pending,
            Poll::Ready(Err(error)) => Poll::Ready(Err(error)),
            Poll::Ready(Ok(())) => Poll::Ready(
                lock(&this.output)
                    .take()
                    .ok_or_else(|| failed("a loop unit's future ended without its output")),
            ),
        }
    }
}

impl<T, S: UnitScope> Drop for LoopUnit<T, S> {
    fn drop(&mut self) {
        let core = &mut self.core;
        // A task that never ran holds only what is `Send`.
        core.task = None;
        if core.context.is_none() {
            return;
        }
        self.scope.enter(&mut || {
            let entered = core.handle.clone().and_then(EnteredLoop::new);
            if entered.is_some() {
                core.context = None;
            } else if let Some(context) = core.context.take() {
                // This thread has a loop entered, so the unit's cannot be: its objects cannot
                // be destroyed here.
                std::mem::forget(context);
            }
        });
    }
}

/// Spawns `unit` on the current multi-thread runtime, outside the cooperative budget (see
/// [`LoopUnit`]).
///
/// # Panics
///
/// Outside a tokio runtime, as `tokio::spawn` does.
pub fn spawn_unit<T: Send + 'static, S: UnitScope>(
    unit: LoopUnit<T, S>,
) -> tokio::task::JoinHandle<Result<T, KjError>> {
    tokio::spawn(tokio::task::coop::unconstrained(unit))
}

#[cfg(test)]
#[path = "unit-test.rs"]
mod tests;
