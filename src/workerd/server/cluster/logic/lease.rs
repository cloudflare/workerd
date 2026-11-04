// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An instance's lease in the bucket: when to renew and when to stop
//! serving (fence).
//!
//! The executor runs one round every `INTERVAL_MS` (`RETRY_MS` after a
//! failed renewal): it asks `before_renew`, renews if told to and reports
//! the result to `renewed`.

/// A node's lease. A peer whose record has not been renewed for a full
/// lease is dead, so the lease spans several rounds.
pub const LEASE_MS: u64 = 20_000;
/// Renewals every 3 s, each bounded at 4 s, a failed one retried every
/// second.
///
/// A node rides out about 8 s of bucket trouble and fences while its
/// lease holds. R2 stalls writes ~5.5 s at every 5-minute mark; a 2 s
/// bound cancelled them and fenced every node.
pub const INTERVAL_MS: u64 = 3_000;
pub const RENEW_TIMEOUT_MS: u64 = 4_000;
pub const LIST_TIMEOUT_MS: u64 = 2_000;
pub const RETRY_MS: u64 = 1_000;
/// Peers judge this node's lease by their own clocks; one off from the
/// bucket's clock by more than this exits.
pub const MAX_SKEW_MS: i64 = 2_000;
/// Peers judge a lease by their own clocks, each within `MAX_SKEW_MS` of
/// the bucket's, so the owner stops serving this long before it expires.
const SAFETY_MS: u64 = 2 * MAX_SKEW_MS as u64;

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
    /// Try again after `RETRY_MS`; the lease still holds.
    Retry(String),
}

#[derive(Default)]
pub struct Lease {
    /// The lease expires at this time; 0 before the first renewal.
    until_ms: u64,
    pub draining: bool,
}

impl Lease {
    pub fn before_renew(&self, o: &Observed) -> Step {
        if o.skew_ms.abs() > MAX_SKEW_MS {
            return Step::Fence(format!("clock off from the bucket's by {} ms", o.skew_ms));
        }
        // A node stalled past its lease (a paused process, a blocked loop)
        // may have been taken over: it must not renew and serve on.
        if self.until_ms != 0 && !self.holds(o.now_ms + RENEW_TIMEOUT_MS) {
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
            Err(e) if !self.holds(now_ms + RENEW_TIMEOUT_MS) => Step::Fence(e),
            Err(e) => Step::Retry(e),
        }
    }

    /// Whether the lease holds at `now_ms`, with peers' clocks allowed
    /// for: an actor serves only while it does.
    pub fn holds(&self, now_ms: u64) -> bool {
        now_ms + SAFETY_MS < self.until_ms
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn seen(now_ms: u64) -> Observed {
        Observed { now_ms, skew_ms: 0 }
    }

    #[test]
    fn renews_and_holds_until_the_safety_margin() {
        let mut lease = Lease::default();
        assert_eq!(lease.before_renew(&seen(0)), Step::Renew);
        assert_eq!(lease.renewed(0, Ok(LEASE_MS)), Step::Renew);
        let last = LEASE_MS - SAFETY_MS;
        assert!(lease.holds(last - 1) && !lease.holds(last));
    }

    #[test]
    fn a_failed_renewal_retries_until_the_lease_nearly_lapses() {
        let mut lease = Lease::default();
        lease.renewed(0, Ok(LEASE_MS));
        assert!(matches!(
            lease.renewed(5_000, Err("x".into())),
            Step::Retry(_)
        ));
        let last = LEASE_MS - SAFETY_MS - RENEW_TIMEOUT_MS;
        assert!(matches!(
            lease.renewed(last - 1, Err("x".into())),
            Step::Retry(_)
        ));
        assert!(matches!(
            lease.renewed(last, Err("x".into())),
            Step::Fence(_)
        ));
    }

    #[test]
    fn a_node_stalled_past_its_lease_fences_instead_of_renewing() {
        let mut lease = Lease::default();
        lease.renewed(0, Ok(LEASE_MS));
        assert_eq!(lease.before_renew(&seen(INTERVAL_MS)), Step::Renew);
        let step = lease.before_renew(&seen(LEASE_MS + 1));
        assert!(matches!(step, Step::Fence(_)));
    }

    #[test]
    fn skew_fences_at_once() {
        let lease = Lease::default();
        let o = Observed {
            skew_ms: MAX_SKEW_MS + 1,
            ..seen(0)
        };
        assert!(matches!(lease.before_renew(&o), Step::Fence(_)));
    }
}
