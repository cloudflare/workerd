// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A draining instance's handoff: how its stop deadline is spent, which
//! object it hands off first, and which peer it sends a request for an
//! object no live instance owns.
//!
//! On SIGTERM an instance hands its objects to its peers, busiest first,
//! while it keeps serving the rest. It then leaves the cluster, lets its
//! open connections finish, and stops its containers. One stop deadline,
//! measured from the signal, bounds all of it:
//!
//! - until `in_flight_ms`, work in flight on an object being handed off
//!   may finish; past it, that work is cancelled;
//! - at `handoff_ms` the handoff ends, and the instance stops the objects
//!   it did not release;
//! - by `release_ms` it has stopped them all, and leaves the cluster: an
//!   object it did not release recovers on a peer then, as after a crash.
//!   While objects it could not stop in time still run, it stays a member:
//!   peers take over every object it did not release once its lease
//!   expires, after it exited;
//! - until `grace_ms` at least, the instance accepts requests, as load
//!   balancers take a while to stop sending them;
//! - at `connections_ms` the instance stops waiting for its connections,
//!   and the containers' cleanup gets what is left of the deadline.

/// The stop deadline when the config sets none: within the 10 s that
/// `docker stop` and Docker Compose give a stopping container by default,
/// with room for the process to exit.
pub const DEFAULT_STOP_MS: u64 = 8_000;

/// How long an instance accepts requests after the signal at least, unless
/// its handoff ends before.
pub const GRACE_MS: u64 = 5_000;

/// How one stop deadline is spent, as offsets from the drain signal.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Budget {
    pub stop_ms: u64,
}

impl Budget {
    /// Work in flight on an object being handed off is cancelled past this.
    #[must_use]
    pub const fn in_flight_ms(&self) -> u64 {
        self.stop_ms / 2
    }

    /// The handoff ends; the instance stops the objects it did not release.
    #[must_use]
    pub const fn handoff_ms(&self) -> u64 {
        self.stop_ms * 3 / 4
    }

    /// The instance stops no more objects, and leaves the cluster, which
    /// may take until `connections_ms`.
    #[must_use]
    pub const fn release_ms(&self) -> u64 {
        self.stop_ms * 33 / 40
    }

    /// Accepting requests may stop, if the handoff ended.
    #[must_use]
    pub const fn grace_ms(&self) -> u64 {
        let handoff = self.handoff_ms();
        if GRACE_MS < handoff {
            GRACE_MS
        } else {
            handoff
        }
    }

    /// The instance stops waiting for its open connections; the containers'
    /// cleanup takes the rest.
    #[must_use]
    pub const fn connections_ms(&self) -> u64 {
        self.stop_ms * 9 / 10
    }
}

impl Default for Budget {
    fn default() -> Self {
        Self {
            stop_ms: DEFAULT_STOP_MS,
        }
    }
}

/// How busy an object has been: its requests, each counting half as much
/// for every `HALF_LIFE_MS` since it started.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Heat {
    score: f64,
    at_ms: u64,
}

pub const HALF_LIFE_MS: u32 = 10_000;

impl Heat {
    /// A request started at `now_ms`.
    pub fn hit(&mut self, now_ms: u64) {
        self.score = self.at(now_ms) + 1.0;
        self.at_ms = self.at_ms.max(now_ms);
    }

    /// The heat at `now_ms`.
    #[must_use]
    pub fn at(&self, now_ms: u64) -> f64 {
        let age = u32::try_from(now_ms.saturating_sub(self.at_ms)).unwrap_or(u32::MAX);
        self.score * 0.5f64.powf(f64::from(age) / f64::from(HALF_LIFE_MS))
    }
}

/// The order a drain hands objects off in, as indices into `heats`.
///
/// The busiest go first, whose callers would otherwise wait longest, and the
/// idle ones last, which cost a recovery the least if the deadline comes
/// first.
#[must_use]
pub fn busiest_first(heats: &[Heat], now_ms: u64) -> Vec<usize> {
    let mut order: Vec<usize> = (0..heats.len()).collect();
    order.sort_by(|&a, &b| heats[b].at(now_ms).total_cmp(&heats[a].at(now_ms)));
    order
}

/// The peer a draining instance sends a request for `object` to.
///
/// When no live instance owns the object, the peer claims it: of `peers`,
/// the one with the highest weight for the object. Every request for one
/// object goes to the same peer, and objects spread evenly over the peers.
pub fn successor<'a>(object: &str, peers: impl IntoIterator<Item = &'a str>) -> Option<&'a str> {
    peers
        .into_iter()
        .max_by_key(|peer| (weight(object, peer), *peer))
}

/// FNV-1a over the object and the peer, then the finalizer of `SplitMix64`,
/// which FNV's weak mixing of the last bytes needs.
fn weight(object: &str, peer: &str) -> u64 {
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    let bytes = object.bytes().chain([0xff]).chain(peer.bytes());
    for byte in bytes {
        hash = (hash ^ u64::from(byte)).wrapping_mul(0x0100_0000_01b3);
    }
    hash = (hash ^ (hash >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    hash = (hash ^ (hash >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    hash ^ (hash >> 31)
}

#[cfg(test)]
#[path = "handoff-test.rs"]
mod tests;
