//! The tokio-driven mode: a tokio `current_thread` runtime that owns the loop thread, with the KJ
//! event loop as one participant.
//!
//! [`Runtime::block_on`] runs the caller's future on the runtime's `LocalSet` alongside a driver
//! task that turns the KJ loop through the C++ context's primitives (`run_turns`, `poll_turns`,
//! `prepare_to_yield`; see tokio-event-port.h). The schedule is the KJ-driven port's, with the
//! roles swapped: tokio runs only while the KJ loop is idle, exactly as a KJ-driven loop pumps
//! tokio only from `wait()`/`poll()`, so the order in which KJ events and tokio-delivered I/O run
//! is the same in both modes (workerd's `--predictable` tests depend on it). The driver's cycle:
//!
//! 1. Run the KJ loop to idle.
//! 2. Pump tokio the way the KJ-driven port's `poll()` does: a bounded number of yields, each
//!    letting ready tasks and I/O run, with the KJ loop run back to idle after each (an event
//!    armed by a task runs before the next yield).
//! 3. `poll_turns`: the only path that drains cross-thread `kj::Executor` events and promotes
//!    `kj::yieldUntilWouldSleep()` waiters (KJ's `EventLoop::run()` never polls the port). A
//!    would-sleep waiter is promoted only once neither KJ nor the pumped tokio produced work,
//!    which is what "the thread would sleep" means with two schedulers on it.
//! 4. Nothing anywhere: park on the port's `Notify`, bounded by the next KJ timer deadline.
//!    `wake()` from another thread, `setRunnable(true)` from a task arming a KJ event, and a
//!    sooner timer armed by a task all end the park (port.rs, `notify_kj_service`); tokio's own
//!    tasks and I/O run meanwhile because the parked driver is just another pending task.
//!
//! Every time the driver gives up the thread it first calls `prepare_to_yield`, which installs
//! the timer's sleep hooks: KJ timers armed by tokio tasks see the live clock, and a deadline
//! sooner than the planned park re-plans it. The next cycle's `advanceTo` clears the hooks, so
//! `kj::Timer::now()` is frozen during KJ turns exactly as under KJ's own loop.
//!
//! The driver runs `unconstrained`: KJ turns happen inside its poll, and tokio resources polled
//! from within them (bridged futures) must not see an exhausted coop budget, which would make
//! them return `Pending` and wake immediately.
//!
//! Teardown is the reverse of construction and is fixed by field order: the C++ context first
//! (which cancels the `LocalSet`'s tasks while the loop and timer are alive, then destroys the
//! `WaitScope` and the loop, which asserts an empty queue), then the tokio runtime.

use std::convert::Infallible;
use std::future::Future;
use std::pin::Pin;
use std::pin::pin;
use std::sync::Arc;
use std::time::Duration;

use cxx::KjError;
use cxx::KjExceptionType;
use futures::future::Either;
use futures::future::select;
use kj_rs::KjOwn;
use tokio::runtime::Builder;
use tokio::runtime::Handle;
use tokio::task::unconstrained;
use tokio::task::yield_now;

use crate::ffi::EnteredRuntime;
use crate::ffi::TokioAsyncIoContext;
use crate::port::POLL_YIELD_BUDGET;
use crate::port::SharedState;
use crate::port::TokioPort;
use crate::port::current_local_set;

/// `run_turns` / `poll_turns` bound: run the KJ loop until its queue is empty (see the module
/// doc: tokio runs only while KJ is idle, as under the KJ-driven port). A KJ loop that keeps
/// re-arming itself starves tokio in both modes alike.
const RUN_TO_IDLE: u32 = u32::MAX;

pub type Result<T> = std::result::Result<T, KjError>;

fn failed(message: impl Into<String>) -> KjError {
    KjError::new(KjExceptionType::Failed, message.into())
}

/// A tokio `current_thread` runtime that owns this thread and drives a KJ event loop as a
/// participant (see the module doc).
///
/// One per thread, like the KJ-driven port; the thread stays inside the runtime's context for
/// the Runtime's whole life, so tokio resources created anywhere on it -- including by C++
/// inside KJ turns -- register with its drivers.
///
/// The C++ side reaches the loop's `kj::Timer`, `kj::EventLoop` and `kj::WaitScope` through
/// [`Runtime::context`]. `promise.wait()` on that `WaitScope` throws: nothing may block the
/// loop thread.
pub struct Runtime {
    // Field order is drop order (see the module doc): the context before the tokio runtime.
    context: KjOwn<TokioAsyncIoContext>,
    state: Arc<SharedState>,
    tokio: EnteredRuntime,
}

impl Runtime {
    /// Builds the runtime, enters it on this thread, and builds the KJ loop on it.
    ///
    /// Fails if this thread already has a kj-rs-tokio runtime or a current KJ event loop, or if
    /// tokio cannot build the runtime.
    pub fn new() -> Result<Self> {
        if crate::port::current_handle().is_some() {
            return Err(failed(
                "a kj-rs-tokio runtime already exists on this thread (one KJ event loop per \
                 thread)",
            ));
        }
        let tokio = Builder::new_current_thread()
            .enable_time()
            .enable_io()
            .build()
            .map_err(|error| failed(format!("failed to build the tokio runtime: {error}")))?;
        let tokio = EnteredRuntime::new(tokio);
        let state = SharedState::new();
        let port = TokioPort::new_tokio_driven(tokio.handle().clone(), Arc::clone(&state));
        let context = crate::ffi::new_tokio_driven_context(Box::new(port))?;
        Ok(Self {
            context,
            state,
            tokio,
        })
    }

    /// The C++ context: the loop's `kj::Timer`, `kj::EventLoop` and `kj::WaitScope`, for the C++
    /// that builds on them.
    pub fn context(&mut self) -> Pin<&mut TokioAsyncIoContext> {
        self.context.as_mut()
    }

    /// A handle to the tokio runtime, usable from any thread.
    #[must_use]
    pub fn handle(&self) -> Handle {
        self.tokio.handle().clone()
    }

    /// Runs `future` to completion on this thread, driving the tokio scheduler, the tasks on the
    /// loop's `LocalSet` ([`crate::spawn`]) and the KJ event loop meanwhile. Tasks still pending
    /// when `future` completes stay pending until the next `block_on` or the Runtime's drop.
    ///
    /// The error is a `kj::Exception` thrown by a KJ event and not caught before it reached the
    /// loop (KJ's `EventLoop::run()` propagates it), which ends the run.
    pub fn block_on<F: Future>(&mut self, future: F) -> Result<F::Output> {
        let local = current_local_set()
            .ok_or_else(|| failed("the runtime's LocalSet is gone; the thread is tearing down"))?;
        let state = &self.state;
        let context = self.context.as_mut();
        local.block_on(&self.tokio, async move {
            let future = pin!(future);
            let driver = pin!(unconstrained(drive(context, state)));
            match select(future, driver).await {
                Either::Left((output, _)) => Ok(output),
                Either::Right((Err(error), _)) => Err(error),
                Either::Right((Ok(never), _)) => match never {},
            }
        })
    }
}

/// The KJ side of the schedule (see the module doc). Never returns: the run ends when the main
/// future completes (the caller drops this) or a KJ event throws.
async fn drive(
    mut context: Pin<&mut TokioAsyncIoContext>,
    state: &SharedState,
) -> Result<Infallible> {
    loop {
        context.as_mut().run_turns(RUN_TO_IDLE)?;
        for _ in 0..POLL_YIELD_BUDGET {
            context.as_mut().prepare_to_yield()?;
            yield_now().await;
            context.as_mut().run_turns(RUN_TO_IDLE)?;
        }
        if context.as_mut().poll_turns(RUN_TO_IDLE)? {
            continue;
        }
        let next_timer: Option<u64> = context.as_mut().prepare_to_yield()?.into();
        match next_timer {
            Some(nanoseconds) => {
                // Timing out is one of the ways the park ends, not an error: a due timer fires in
                // the next cycle's advanceTo().
                let _ = tokio::time::timeout(
                    Duration::from_nanos(nanoseconds),
                    state.notify.notified(),
                )
                .await;
            }
            None => state.notify.notified().await,
        }
    }
}
