// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Which loop thread a Durable Object lives on (`workerd serve --threads=<N>`, threads.rs): the
//! policy the hub (hub.rs) answers by.
//!
//! An actor has one home thread for the life of the process: a hash of what names its namespace
//! and of its key, reduced to the number of loop threads. It needs no table, so a stub made on
//! any thread reaches the same actor. A facet lives with its root, whose key is the one hashed.
//! The hash has a fixed key, but nothing persists a home: a process started with another number
//! of threads places every actor anew.

/// How a process's loop threads place actors.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Placement {
    /// How many loop threads the process has.
    pub count: usize,
    /// Whether actors are spread over the threads; otherwise every actor's home is the first.
    pub spread: bool,
}

impl Placement {
    /// The home thread of the actor `key` of the namespace `scope` names.
    #[must_use]
    pub fn home(&self, scope: &Scope<'_>, key: &str) -> usize {
        if self.spread {
            home(scope, key, self.count)
        } else {
            0
        }
    }
}

/// What names a namespace on every loop thread alike.
#[derive(Clone, Copy, Debug)]
pub enum Scope<'a> {
    /// A durable namespace, by its `uniqueKey`.
    Durable(&'a str),
    /// An ephemeral namespace, by its worker service and class.
    Ephemeral(&'a str, &'a str),
}

/// The home thread, of `count`, of the actor `key` (the hex id of a durable actor, the name of
/// an ephemeral one) of the namespace `scope` names.
#[must_use]
pub fn home(scope: &Scope<'_>, key: &str, count: usize) -> usize {
    if count <= 1 {
        return 0;
    }
    let mut hash = Fnv::default();
    match scope {
        Scope::Durable(unique_key) => {
            hash.tag(b'd');
            hash.part(unique_key);
        }
        Scope::Ephemeral(service, class) => {
            hash.tag(b'e');
            hash.part(service);
            hash.part(class);
        }
    }
    hash.part(key);
    // The high bits of the product: every bit of the hash takes part, for any `count`.
    let scaled = u128::from(hash.finish()) * count as u128;
    usize::try_from(scaled >> 64).unwrap_or(0)
}

/// FNV-1a, 64 bits, with a finishing mix: the plain hash's high bits change little between
/// similar inputs, and the reduction above reads the high bits.
struct Fnv(u64);

impl Default for Fnv {
    fn default() -> Self {
        Self(0xcbf2_9ce4_8422_2325)
    }
}

impl Fnv {
    fn byte(&mut self, byte: u8) {
        self.0 = (self.0 ^ u64::from(byte)).wrapping_mul(0x0000_0100_0000_01b3);
    }

    fn tag(&mut self, tag: u8) {
        self.byte(tag);
    }

    /// One part of the input, with its length, so that the parts' boundaries are hashed too.
    fn part(&mut self, part: &str) {
        for byte in (part.len() as u64).to_le_bytes() {
            self.byte(byte);
        }
        for byte in part.bytes() {
            self.byte(byte);
        }
    }

    /// The finalizer of splitmix64.
    fn finish(&self) -> u64 {
        let mut mixed = self.0;
        mixed = (mixed ^ (mixed >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        mixed = (mixed ^ (mixed >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        mixed ^ (mixed >> 31)
    }
}

#[cfg(test)]
#[path = "placement-test.rs"]
mod tests;
