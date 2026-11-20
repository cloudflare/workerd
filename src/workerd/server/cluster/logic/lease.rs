// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An instance's lease in the bucket: when to renew and when to stop
//! serving (fence).
//!
//! The executor runs one round every `interval_ms` (`retry_ms` after a
//! failed renewal): it asks `before_renew`, renews if told to and reports
//! the result to `renewed`. It runs on a thread of its own, so a round
//! waits on the bucket alone, never on the server's event loop.

/// A lease's times, in ms. Each is sized against the others, so a test
/// that waits out leases divides them, all but `max_skew_ms`, by one number
/// (`divided`).
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
    /// A round's bucket request that has not answered within its round's
    /// bound is sent once more, the first copy kept in flight
    /// (`http::prompt`). About one request in 8,000 to a bucket stalls for
    /// seconds, and the same request sent again answers in its usual time;
    /// a bucket slow to answer every request answers the first copy
    /// soonest. A listing's LIST answers in 32 ms at the median and 241 ms
    /// at the 99.9th percentile, and R2's usual answers take up to a
    /// second, which no bound cuts short. Each bound is below its round's
    /// deadline.
    ///
    /// The bounds are a request's wall time, so a slowdown of the whole
    /// bucket doubles a round's requests. A listing runs on the server's
    /// event loop, so a stall of the loop past its bound looks like a
    /// stalled bucket and costs a resend.
    ///
    /// A renewal is a write, and after a refusal a reread and a write
    /// again. A stalled write, its copy's answer, the reread and the
    /// write take 1.5 s and three usual answers, within 4 s while those
    /// take under 830 ms. A copy refused while the first is out waits up to
    /// another 1.5 s for the first's answer, the round's success if the
    /// first landed; if it never answers, the reread and the write have
    /// what is left of the 4 s, about 1 s.
    pub renew_attempt_ms: u64,
    /// A listing is a LIST, a request for each page past the first, and
    /// the records' reads side by side. One stalled request among them
    /// takes 1 s and its copy's answer, within the listing's 2 s. Two in a
    /// row, a stalled LIST and then a stalled read or page, take 2 s and
    /// more: the listing times out, which only delays routing to a new
    /// peer until the next.
    pub list_attempt_ms: u64,
    /// How long a renewal waits to write again when its guard failed on a
    /// version that is still current: a write of ours to it is in
    /// progress. R2 refuses more than about one write a second to one key.
    pub contended_pause_ms: u64,
    /// The owner stops serving this long before its lease expires: peers
    /// judge it by their own clocks, each within `max_skew_ms` of the
    /// bucket's, so twice that. A test's nodes share one clock.
    pub safety_ms: u64,
    /// A node whose clock may be off from the bucket's by more than this
    /// (`Skew`) does not join, or exits. Clocks run at their own pace in
    /// tests too: `divided` keeps it.
    pub max_skew_ms: u64,
    /// A node whose event loop makes no progress for this long fences
    /// (`watchdog`). The lease is kept on a thread of its own, so a turn of
    /// the loop longer than a lease does not lose it; a loop stuck for good
    /// would keep it, and the node's objects unreachable, for good. Three
    /// leases leaves room for turns longer than a lease, which the lease
    /// thread rides out, and is short enough that peers take a stuck node's
    /// objects over within a few leases more.
    pub stall_ms: u64,
}

impl Timing {
    pub const DEFAULT: Self = Self {
        lease_ms: 20_000,
        interval_ms: 3_000,
        renew_timeout_ms: 4_000,
        list_timeout_ms: 2_000,
        retry_ms: 1_000,
        renew_attempt_ms: 1_500,
        list_attempt_ms: 1_000,
        contended_pause_ms: 1_000,
        safety_ms: 4_000,
        max_skew_ms: 2_000,
        stall_ms: 60_000,
    };

    /// `DEFAULT` with every time but `max_skew_ms` divided by `divisor`: for
    /// tests, which would otherwise wait out a full lease. `None` if
    /// `divisor` is 0 or rounds a time down to 0. A bucket that stalls for
    /// longer than the shortened bounds fences every node.
    #[must_use]
    pub fn divided(divisor: u64) -> Option<Self> {
        let t = Self::DEFAULT;
        let divided = Self {
            lease_ms: t.lease_ms.checked_div(divisor)?,
            interval_ms: t.interval_ms / divisor,
            renew_timeout_ms: t.renew_timeout_ms / divisor,
            list_timeout_ms: t.list_timeout_ms / divisor,
            retry_ms: t.retry_ms / divisor,
            renew_attempt_ms: t.renew_attempt_ms / divisor,
            list_attempt_ms: t.list_attempt_ms / divisor,
            contended_pause_ms: t.contended_pause_ms / divisor,
            safety_ms: t.safety_ms / divisor,
            max_skew_ms: t.max_skew_ms,
            stall_ms: t.stall_ms / divisor,
        };
        (!divided.times().contains(&0)).then_some(divided)
    }

    /// The longest from one round's start to the next's.
    #[must_use]
    pub const fn round_ms(self) -> u64 {
        self.renew_timeout_ms + self.interval_ms
    }

    const fn times(self) -> [u64; 10] {
        [
            self.lease_ms,
            self.interval_ms,
            self.renew_timeout_ms,
            self.list_timeout_ms,
            self.retry_ms,
            self.renew_attempt_ms,
            self.list_attempt_ms,
            self.contended_pause_ms,
            self.safety_ms,
            self.stall_ms,
        ]
    }

    /// Whether a lease that expires at `until_ms` holds at `now_ms`, with
    /// peers' clocks allowed for.
    #[must_use]
    pub const fn holds(&self, until_ms: u64, now_ms: u64) -> bool {
        now_ms + self.safety_ms < until_ms
    }

    /// How much longer than `now_ms` a lease that expires at `until_ms`
    /// holds (`holds`), in ms: 0 once it does not.
    #[must_use]
    pub const fn remaining_ms(&self, until_ms: u64, now_ms: u64) -> u64 {
        until_ms.saturating_sub(now_ms + self.safety_ms)
    }

    /// When peers, by clocks up to `safety_ms` ahead, may judge a lease that
    /// expires at `until_ms` expired: it `holds` before.
    #[must_use]
    pub const fn holds_until(&self, until_ms: u64) -> u64 {
        until_ms.saturating_sub(self.safety_ms)
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
    /// How far this node's clock may be off from the bucket's
    /// (`Skew::most_ms`); `None` before an answer with a date.
    pub skew_ms: Option<u64>,
}

/// What this node's clock less the bucket's may be, from the `Date` of the
/// bucket's answers.
///
/// Each answer bounds it to a window over a second wide. The windows are
/// kept on the monotonic clock, which is never stepped, so a step of this
/// clock moves them at once; each is widened by how far the clocks may have
/// drifted apart since, so answers at different points of the bucket's
/// second narrow it. An answer that fits none of them, as from a bucket
/// whose clock was stepped, starts over.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Skew {
    /// The monotonic clock less the bucket's.
    lo: i64,
    hi: i64,
    /// When, by the monotonic clock.
    at: i64,
}

impl Skew {
    /// `known`, with an answer dated `date`, the start of one of the
    /// bucket's whole seconds, to a request sent at `sent` and answered at
    /// `received` by the monotonic clock.
    #[must_use]
    pub fn answered(known: Option<Self>, sent: i64, received: i64, date: i64) -> Self {
        // The bucket stamped the header between the send and the answer.
        let new = Self {
            lo: sent - date - 1000,
            hi: received - date,
            at: sent,
        }
        .widened(received);
        match known.map(|k| k.widened(received)) {
            Some(k) if k.lo.max(new.lo) <= k.hi.min(new.hi) => Self {
                lo: k.lo.max(new.lo),
                hi: k.hi.min(new.hi),
                at: received,
            },
            _ => new,
        }
    }

    /// How far the clock may be off when it reads `wall`, at `at` by the
    /// monotonic clock.
    #[must_use]
    pub fn most_ms(self, at: i64, wall: i64) -> u64 {
        let k = self.widened(at);
        let step = wall - at;
        (k.lo + step)
            .unsigned_abs()
            .max((k.hi + step).unsigned_abs())
    }

    /// A clock gains or loses at most 1% of its own time on the bucket's,
    /// 10 ms a second; NTP slews it by at most 0.05%. chrony slews faster,
    /// but toward the bucket's time: that narrows the skew, which the far
    /// end still bounds.
    fn widened(self, at: i64) -> Self {
        // In whole ms, rounded up.
        let drift = (at - self.at + 99).max(0) / 100;
        Self {
            lo: self.lo - drift,
            hi: self.hi + drift,
            at,
        }
    }
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

    /// Whether this node knows its clock to within `max_skew_ms` of the
    /// bucket's, as it must to hold a lease: `skew_ms` as `Observed` has it.
    /// It checks again within a round, its clock drifting 1% of that
    /// (`Skew`).
    pub fn clock(&self, skew_ms: Option<u64>) -> Result<(), String> {
        match skew_ms {
            Some(ms) if ms + self.timing.round_ms() / 100 <= self.timing.max_skew_ms => Ok(()),
            Some(ms) => Err(format!("clock may be off from the bucket's by {ms} ms")),
            None => Err("the bucket has answered with no date".into()),
        }
    }

    pub fn before_renew(&self, o: &Observed) -> Step {
        if let Err(e) = self.clock(o.skew_ms) {
            return Step::Fence(e);
        }
        // A node stalled past its lease (a paused process, a lease thread
        // that got no time) may have been taken over: it must not renew
        // and serve on.
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
        self.timing.holds(self.until_ms, now_ms)
    }

    /// When peers, by clocks up to `safety_ms` ahead, may judge this lease expired: it
    /// [`Lease::holds`] before. 0 once it left.
    #[must_use]
    pub fn holds_until(&self) -> u64 {
        if self.left {
            return 0;
        }
        self.timing.holds_until(self.until_ms)
    }
}

#[cfg(test)]
#[path = "lease-test.rs"]
mod tests;
