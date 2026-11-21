// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bucket cluster's decisions, with no I/O: no async, clocks,
//! randomness, locks or dependencies beyond std. Time is an argument. The
//! executor (`cluster`) performs what these types decide.

#![deny(
    clippy::expect_used,
    clippy::panic,
    clippy::unreachable,
    clippy::unwrap_used
)]

pub mod cache;
pub mod handoff;
pub mod lease;
pub mod watchdog;

/// What a node knows of the cluster's membership, for what routes by it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Membership {
    /// When the latest listing started, and its nodes.
    pub listing: Option<(u64, Vec<String>)>,
    /// When this node's registration answered.
    pub registered: Option<u64>,
    /// [`lease::Lease::holds_until`].
    pub lease_until: u64,
    /// Whether this node drains.
    pub draining: bool,
}
