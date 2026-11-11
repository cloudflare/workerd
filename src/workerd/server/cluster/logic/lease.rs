// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An instance's lease in the bucket: when to renew and when to stop
//! serving (fence).
//!
//! The executor runs one round every `interval_ms` (`retry_ms` after a
//! failed renewal): it asks `before_renew`, renews if told to and reports
//! the result to `renewed`.

/// A lease's times, in ms. Each is sized against the others, so a test
/// that waits out leases divides them all by one number (`divided`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Timing {
    /// A node's lease. A peer whose record has not been renewed for a full
    /// lease is dead, so the lease spans several rounds.
    pub lease_ms: u64,
    /// Renewals every 3 s, each bounded at 4 s, a failed one retried every
    /// second.
    ///
    /// A node rides out about 8 s of bucket trouble and fences while its
    /// lease holds. R2 stalls writes ~5.5 s at every 5-minute mark; a 2 s
    /// bound cancelled them and fenced every node.
    pub interval_ms: u64,
    pub renew_timeout_ms: u64,
    pub list_timeout_ms: u64,
    pub retry_ms: u64,
    /// Peers judge this node's lease by their own clocks; one off from the
    /// bucket's clock by more than this exits.
    pub max_skew_ms: u64,
}

impl Timing {
    pub const DEFAULT: Self = Self {
        lease_ms: 20_000,
        interval_ms: 3_000,
        renew_timeout_ms: 4_000,
        list_timeout_ms: 2_000,
        retry_ms: 1_000,
        max_skew_ms: 2_000,
    };

    /// `DEFAULT` with every time divided by `divisor`: for tests, which
    /// would otherwise wait out a full lease. `None` if `divisor` is 0 or
    /// rounds a time down to 0. A bucket that stalls for longer than the
    /// shortened bounds fences every node.
    #[must_use]
    pub fn divided(divisor: u64) -> Option<Self> {
        let t = Self::DEFAULT;
        let divided = Self {
            lease_ms: t.lease_ms.checked_div(divisor)?,
            interval_ms: t.interval_ms / divisor,
            renew_timeout_ms: t.renew_timeout_ms / divisor,
            list_timeout_ms: t.list_timeout_ms / divisor,
            retry_ms: t.retry_ms / divisor,
            max_skew_ms: t.max_skew_ms / divisor,
        };
        (!divided.times().contains(&0)).then_some(divided)
    }

    const fn times(self) -> [u64; 6] {
        [
            self.lease_ms,
            self.interval_ms,
            self.renew_timeout_ms,
            self.list_timeout_ms,
            self.retry_ms,
            self.max_skew_ms,
        ]
    }

    /// Peers judge a lease by their own clocks, each within `max_skew_ms`
    /// of the bucket's, so the owner stops serving this long before it
    /// expires.
    const fn safety_ms(&self) -> u64 {
        2 * self.max_skew_ms
    }
}

impl Default for Timing {
    fn default() -> Self {
        Self::DEFAULT
    }
}

/// What the executor observed before a round's renewal.
pub struct Observed {
    pub now_ms: u64,
    /// This node's clock less the bucket's.
    pub skew_ms: i64,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Step {
    /// Renew now.
    Renew,
    /// Stop serving: the process exits before peers may take over.
    Fence(String),
    /// Try again after `retry_ms`; the lease still holds.
    Retry(String),
}

#[derive(Default)]
pub struct Lease {
    pub timing: Timing,
    /// The lease expires at this time; 0 before the first renewal.
    until_ms: u64,
    /// The instance left the cluster: it renews no more.
    pub left: bool,
}

impl Lease {
    #[must_use]
    pub fn new(timing: Timing) -> Self {
        Self {
            timing,
            ..Self::default()
        }
    }

    pub fn before_renew(&self, o: &Observed) -> Step {
        if o.skew_ms.unsigned_abs() > self.timing.max_skew_ms {
            return Step::Fence(format!("clock off from the bucket's by {} ms", o.skew_ms));
        }
        // A node stalled past its lease (a paused process, a blocked loop)
        // may have been taken over: it must not renew and serve on.
        if self.until_ms != 0 && !self.holds(o.now_ms + self.timing.renew_timeout_ms) {
            return Step::Fence("the lease lapsed before it could be renewed".into());
        }
        Step::Renew
    }

    /// The renewal's outcome: the new expiry, or why it failed. `Renew`
    /// here means the round succeeded.
    pub fn renewed(&mut self, now_ms: u64, result: Result<u64, String>) -> Step {
        match result {
            Ok(until_ms) => {
                self.until_ms = until_ms;
                Step::Renew
            }
            // Fence while the lease still holds, so the node is gone before
            // a peer may take over.
            Err(e) if !self.holds(now_ms + self.timing.renew_timeout_ms) => Step::Fence(e),
            Err(e) => Step::Retry(e),
        }
    }

    /// Whether the lease holds at `now_ms`, with peers' clocks allowed
    /// for: an actor serves only while it does.
    pub fn holds(&self, now_ms: u64) -> bool {
        now_ms + self.timing.safety_ms() < self.until_ms
    }
}

#[cfg(test)]
#[path = "lease-test.rs"]
mod tests;
