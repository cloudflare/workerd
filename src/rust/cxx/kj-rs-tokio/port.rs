//! Per-thread tokio `current_thread` runtime management and the Rust half of `TokioEventPort`.

use std::cell::RefCell;
use std::future::Future;
use std::rc::Rc;
use std::sync::Arc;
#[cfg(unix)]
use std::sync::Condvar;
#[cfg(unix)]
use std::sync::Mutex;
#[cfg(unix)]
use std::sync::PoisonError;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::time::Duration;
#[cfg(unix)]
use std::time::Instant;

use tokio::runtime::Builder;
use tokio::runtime::Handle;
use tokio::runtime::Runtime;
use tokio::sync::Notify;
use tokio::task::JoinHandle;
use tokio::task::LocalSet;

#[cfg(unix)]
use crate::ffi;

/// How many scheduler turns `poll()` grants the runtime. Each `yield_now` re-queues the main
/// future at the back of the run queue, so every already-ready spawned task gets a chance to run
/// (repeatedly, up to the budget) without ever parking the thread.
///
/// The value is a latency/throughput compromise, not derived from any tokio internal: large
/// enough to drain a typical burst of already-ready tasks in one `poll()` call, small enough to
/// bound how long `poll()` withholds control from the KJ loop when spawned tasks keep re-readying
/// each other. Safe to retune if profiling shows either starvation or excessive poll latency.
const POLL_YIELD_BUDGET: u32 = 16;

/// Timeouts strictly below this go through the [`HiResTimer`] short-sleep path: tokio's timer
/// wheel has ~1 ms granularity, which would quantize sub-millisecond KJ timers (e.g. a 100 µs
/// `timer.afterDelay`) to a ~1 ms sleep. At and above a couple of milliseconds the wheel's
/// error is proportionally small, so long sleeps stay on the plain tokio path.
#[cfg(unix)]
const HIRES_TIMEOUT_THRESHOLD: Duration = Duration::from_millis(2);
