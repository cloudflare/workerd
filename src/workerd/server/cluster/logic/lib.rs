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

pub mod lease;
