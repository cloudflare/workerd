//! Wakers that record which of a task's futures woke it, so the task polls only those.
//!
//! A task that drives several futures by hand ([`crate::server::serve_connection`] drives hyper's
//! connection, the transport's hang-up and the server's drain) gives each one a [`Wakes::waker`]
//! of its own. Waking one sets the future's bit and wakes the task, except while the task is
//! polling: it then picks the bit up itself ([`Wakes::take`]) instead of being polled again.

use std::sync::Arc;
use std::sync::atomic::AtomicU8;
use std::sync::atomic::Ordering;
use std::task::Context;
use std::task::Wake;
use std::task::Waker;

use futures::task::AtomicWaker;

/// Set while the task polls: a wake then only sets its future's bit.
const POLLING: u8 = 0x80;

/// The futures of one task, each named by a bit, at most seven.
pub struct Wakes(Arc<State>);

struct State {
    /// The bits of the futures woken since the task took them, and [`POLLING`].
    woken: AtomicU8,
    task: AtomicWaker,
}

struct Flag {
    state: Arc<State>,
    bit: u8,
}

impl Wake for Flag {
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        if self.state.woken.fetch_or(self.bit, Ordering::AcqRel) & POLLING == 0 {
            self.state.task.wake();
        }
    }
}

impl Wakes {
    /// Every future counts as woken until the task first polls it.
    pub fn new() -> Self {
        Self(Arc::new(State {
            woken: AtomicU8::new(!POLLING),
            task: AtomicWaker::new(),
        }))
    }

    /// The waker of the future named by `bit`.
    pub fn waker(&self, bit: u8) -> Waker {
        debug_assert!(bit != 0 && bit & POLLING == 0);
        Waker::from(Arc::new(Flag {
            state: Arc::clone(&self.0),
            bit,
        }))
    }

    /// Starts a poll of the task: the futures woken since its last poll.
    pub fn begin(&self, cx: &Context<'_>) -> u8 {
        self.0.task.register(cx.waker());
        self.0.woken.swap(POLLING, Ordering::AcqRel) & !POLLING
    }

    /// Whether the future named by `bit` was woken during this poll; clears it.
    pub fn take(&self, bit: u8) -> bool {
        self.0.woken.fetch_and(!bit, Ordering::AcqRel) & bit != 0
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
