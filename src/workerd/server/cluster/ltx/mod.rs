// Ported from celld's `celld-ltx` crate (Apache License 2.0), which was
// seeded from rustyriver and carries:
//
// - rustyriver (https://github.com/mikenomitch/rustyriver): Copyright 2026 The
//   rustyriver authors, Apache License 2.0.
// - Litestream (https://github.com/benbjohnson/litestream), tag v0.5.11:
//   Copyright (c) Ben Johnson and the Litestream authors, Apache License 2.0.
// - LTX (https://github.com/superfly/ltx), tag v0.5.2: Copyright (c)
//   Superfly, Inc., Apache License 2.0.

//! LTX replication for a `SQLite` database: WAL capture into LTX segments,
//! upload to an object store, and restore.
//!
//! Ported from celld's `celld-ltx` crate (Apache-2.0), itself a from-scratch
//! Rust reimplementation of Litestream v0.5.11 (Apache-2.0, Ben Johnson) with
//! the block format of LTX v0.5.2 (Apache-2.0), by way of rustyriver. Only
//! the LTX format, the replica client and restore are ported; capture is
//! `capture.rs` and compaction is the owner's (`owner.rs`). Paged restore
//! and bundles stay in celld.
//!
//! # Core position and identity helpers
//!
//! Ported from litestream@v0.5.11 litestream.go:18-203 and
//! ltx@v0.5.2 ltx.go:66-145.

// The modules below keep celld's source nearly line for line so a later
// diff against celld stays readable. They are therefore exempt from the
// pedantic and nursery style groups; the crate's deny lints still apply.
#[expect(
    clippy::pedantic,
    clippy::nursery,
    reason = "ported from celld; kept diffable"
)]
pub mod client;
#[expect(
    clippy::pedantic,
    clippy::nursery,
    reason = "ported from celld; kept diffable"
)]
pub(crate) mod codec;
pub mod error;
#[expect(
    clippy::pedantic,
    clippy::nursery,
    clippy::module_inception,
    reason = "ported from celld; kept diffable"
)]
pub mod ltx;
#[expect(clippy::nursery, reason = "ported from celld; kept diffable")]
pub mod replica;

pub use client::bucket::BucketClient;
pub use error::Error;
pub use replica::SNAPSHOT_LEVEL;
pub use replica::plan;
pub use replica::restore;

// ── TXID ──────────────────────────────────────────────────────────────────────

/// A monotonically increasing transaction identifier.
///
/// Represented as a `u64` and formatted as a zero-padded 16-character
/// lowercase hex string (e.g. `"0000000000000001"`).  This matches the
/// on-disk LTX filename convention and `ltx.TXID.String()` in
/// ltx@v0.5.1 ltx.go:142-144.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
#[expect(clippy::upper_case_acronyms, reason = "Litestream's name for it")]
pub struct TXID(pub u64);

impl std::fmt::Display for TXID {
    /// Formats as a 16-digit lowercase hex string, zero-padded.
    ///
    /// Ported from `TXID.String()` in ltx@v0.5.1 ltx.go:142.
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:016x}", self.0)
    }
}

// ── Checksum ──────────────────────────────────────────────────────────────────

/// A 64-bit LTX rolling / file checksum.
///
/// The high bit is the `ChecksumFlag` sentinel used by the LTX format to
/// distinguish a real checksum from an absent one.
///
/// Ported from ltx@v0.5.1 ltx.go (type alias `Checksum uint64`).
pub type Checksum = u64;

/// Flag bit set on every valid non-zero LTX checksum.
///
/// Ported from `ChecksumFlag` in ltx@v0.5.1 ltx.go:55.
pub const CHECKSUM_FLAG: Checksum = 1 << 63;
