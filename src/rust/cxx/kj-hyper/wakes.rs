//! Wakers that record which of a task's futures woke it, so the task polls only those.
//!
//! A task that drives several futures by hand ([`crate::server::serve_connection`] drives hyper's
//! connection, the transport's hang-up, the server's drain and the handler calls) gives each one
//! a [`Wakes::waker`] of its own. Waking one sets the future's bit and wakes the task, except
//! while the task is polling: it then picks the bit up itself ([`Wakes::take`]) instead of being
//! polled again.

use std::sync::Arc;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;
use std::task::Context;
use std::task::Wake;
use std::task::Waker;

use futures::task::AtomicWaker;

/// Set while the task polls: a wake then only sets its future's bit.
const POLLING: u64 = 1 << 63;

/// The futures of one task, each named by a bit, or several by one; at most 63 bits.
#[derive(Clone)]
pub struct Wakes(Arc<State>);

struct State {
    /// The bits of the futures woken since the task took them, and [`POLLING`].
    woken: AtomicU64,
    task: AtomicWaker,
}

impl State {
    fn wake(&self, bits: u64) {
        if self.woken.fetch_or(bits, Ordering::AcqRel) & POLLING == 0 {
            self.task.wake();
        }
    }
}

struct Flag {
    state: Arc<State>,
    bit: u64,
}

impl Wake for Flag {
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        self.state.wake(self.bit);
    }
}

impl Wakes {
    /// Every future counts as woken until the task first polls it.
    pub fn new() -> Self {
        Self(Arc::new(State {
            woken: AtomicU64::new(!POLLING),
            task: AtomicWaker::new(),
        }))
    }

    /// The waker of the future named by `bit`.
    pub fn waker(&self, bit: u64) -> Waker {
        debug_assert!(bit != 0 && bit & POLLING == 0);
        Waker::from(Arc::new(Flag {
            state: Arc::clone(&self.0),
            bit,
        }))
    }

    /// Starts a poll of the task: the futures woken since its last poll.
    pub fn begin(&self, cx: &Context<'_>) -> u64 {
        self.0.task.register(cx.waker());
        self.0.woken.swap(POLLING, Ordering::AcqRel) & !POLLING
    }

    /// Whether the future named by `bit` was woken during this poll; clears it.
    pub fn take(&self, bit: u64) -> bool {
        self.take_any(bit) != 0
    }

    /// Those of `bits` woken during this poll; clears them.
    pub fn take_any(&self, bits: u64) -> u64 {
        self.0.woken.fetch_and(!bits, Ordering::AcqRel) & bits
    }

    /// Wakes the futures named by `bits`, as their wakers would.
    pub fn wake(&self, bits: u64) {
        self.0.wake(bits);
    }

    /// Ends the poll: a future woken since it was last taken gets the task polled again.
    pub fn end(&self, cx: &Context<'_>) {
        if self.0.woken.fetch_and(!POLLING, Ordering::AcqRel) & !POLLING != 0 {
            cx.waker().wake_by_ref();
        }
    }
}

#[cfg(test)]
#[path = "wakes-test.rs"]
mod tests;
