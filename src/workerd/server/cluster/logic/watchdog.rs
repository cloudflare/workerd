// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Whether the server's event loop still makes progress, as the lease's
//! keeper sees it.
//!
//! The lease is kept on a thread of its own, so it holds whatever the loop
//! does. A node whose loop is stuck (a deadlock, native code that does not
//! return, a turn that never ends) must give its objects up all the same:
//! the keeper reads a count that the loop advances while it makes progress,
//! once a round, and fences once the count has stood still for
//! `Timing::stall_ms`. What advances the count is the executor's to say.

use crate::lease::Timing;

pub struct Watchdog {
    stall_ms: u64,
    /// The count last observed, and when it was first observed at that
    /// value.
    last: Option<(u64, u64)>,
}

impl Watchdog {
    #[must_use]
    pub const fn new(timing: &Timing) -> Self {
        Self {
            stall_ms: timing.stall_ms,
            last: None,
        }
    }

    /// The loop's count, observed at `now_ms` on a clock that does not go
    /// back: why the node must fence, once the count has stood still for
    /// `stall_ms` since it was first observed at its value.
    pub fn observe(&mut self, now_ms: u64, count: u64) -> Option<String> {
        match self.last {
            Some((last, since_ms)) if last == count => {
                let still_ms = now_ms.saturating_sub(since_ms);
                (still_ms >= self.stall_ms)
                    .then(|| format!("its event loop made no progress for {still_ms} ms"))
            }
            _ => {
                self.last = Some((count, now_ms));
                None
            }
        }
    }
}

#[cfg(test)]
#[path = "watchdog-test.rs"]
mod tests;
