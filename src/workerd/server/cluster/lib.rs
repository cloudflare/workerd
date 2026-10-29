// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A Durable Object cluster whose shared state is an S3-compatible bucket
//! (S3, R2, `MinIO`) instead of a shared filesystem.
//!
//! Everything lives under `v1/` in the configured prefix, so a later format
//! can tell an old bucket from its own:
//!
//! - `nodes/<node>.json`: one per running instance, with its address and a
//!   lease expiry the instance renews. A peer is live while its lease is.
//! - `do/<namespace>/<actor>/own.json`: one per Durable Object, naming its
//!   owner and an ownership epoch. A claim is a conditional write; taking an
//!   object over from an owner whose lease expired bumps the epoch.
//! - `do/<namespace>/<actor>/{log,snapshot}/<epoch>/..`: the object's log
//!   (`log.rs`), a session per epoch. A takeover restores the last opened
//!   epoch's session and starts the next from a snapshot of it, so a late
//!   write from the old owner lands where nothing reads it.
//! - `alarms/<node>/<time>/<namespace>/<actor>`: an alarm `node` wakes
//!   `actor` for no later than `time` (`alarms.rs`).
//!
//! `<namespace>` is a hash of the namespace's `uniqueKey`, and `<actor>` the
//! actor ID's hex, so no key's length depends on a name.
//!
//! The clock is an argument everywhere (`now_ms`), so a test decides time.

#![deny(
    clippy::expect_used,
    clippy::panic,
    clippy::unreachable,
    clippy::unwrap_used
)]

mod alarms;
mod bucket;
mod capture;
pub mod cluster;
mod http;
mod log;
#[cfg(test)]
mod log_tests;
mod owner;
mod ownership;
mod registry;
pub mod s3;

pub use alarms::Adopted;
pub use bucket::Bucket;
pub use bucket::Guard;
pub use bucket::Store;
pub use bucket::Version;
pub use cluster::Cluster;
pub use cluster::Route;
pub use cluster::Setup;
pub use cluster::now_ms;
pub use owner::Owners;
pub use ownership::Claim;
pub use ownership::Ownership;
pub use registry::NodeRecord;
pub use registry::Registry;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// The bucket URL or an option is missing or malformed.
    #[error("bucket config: {0}")]
    Config(String),
    /// A claim or a commit's capture could not proceed.
    #[error("replica: {0}")]
    Replica(String),
    /// A bucket request failed or answered with an unexpected status.
    #[error("bucket: {0}")]
    Http(String),
    #[error("record: {0}")]
    Codec(#[from] serde_json::Error),
    /// Another writer kept winning the conditional write.
    #[error("{0} is contended")]
    Contended(String),
    /// Someone else rewrote this node's own record: this process no longer
    /// holds its identity and must stop serving.
    #[error("node record {0} was rewritten by another writer")]
    Fenced(String),
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
    /// A log or snapshot does not decode.
    #[error("log: {0}")]
    Log(#[from] capnp::Error),
    #[error("log: {0}")]
    Schema(#[from] capnp::NotInSchema),
}

pub type Result<T> = std::result::Result<T, Error>;

/// An ownership epoch: each takeover of an object bumps it.
pub type Epoch = u64;
/// A position in an epoch's session of the log: the sequence of the frame
/// after it.
pub type Sequence = u32;

/// Where an object's records are: `do/<namespace>/<actor>`, the namespace
/// as the first 16 bytes of a labelled SHA-256 of its tenant, empty for
/// now, and `uniqueKey`, in base64url.
pub(crate) fn object_prefix(ns: &str, actor: &str) -> String {
    format!("do/{}/{actor}", namespace_hash(ns))
}

/// A namespace's part of its objects' keys.
pub fn namespace_hash(ns: &str) -> String {
    use base64::Engine;
    let mut data = Vec::new();
    for part in ["workerd cluster namespace v1", "", ns] {
        data.extend((part.len() as u64).to_be_bytes());
        data.extend(part.as_bytes());
    }
    base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(&sha256(&data)[..16])
}

pub(crate) fn sha256(data: &[u8]) -> Vec<u8> {
    ring::digest::digest(&ring::digest::SHA256, data)
        .as_ref()
        .to_vec()
}

pub(crate) fn hmac_sha256(key: &[u8], data: &[u8]) -> Vec<u8> {
    let key = ring::hmac::Key::new(ring::hmac::HMAC_SHA256, key);
    ring::hmac::sign(&key, data).as_ref().to_vec()
}

pub fn hex(bytes: &[u8]) -> String {
    use std::fmt::Write;
    bytes.iter().fold(String::new(), |mut out, byte| {
        let _ = write!(out, "{byte:02x}");
        out
    })
}

pub fn unhex(text: &str) -> Option<Vec<u8>> {
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(text.get(i..i + 2)?, 16).ok())
        .collect()
}
