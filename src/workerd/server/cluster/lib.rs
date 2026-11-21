// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A Durable Object cluster whose shared state is a bucket instead of a
//! shared filesystem: an S3-compatible one (S3, R2, `MinIO`; `s3.rs`) or a
//! Google Cloud Storage one (`gcs.rs`).
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
//! - `alarms/<node>/<time>/<namespace>/<actor>/<seq>`: an alarm `node`
//!   wakes `actor` for no later than `time` (`alarms.rs`).
//! - `deploy/catalog.json`: the current version of every named deployment,
//!   which each node serves beside its own config; replaced only by a
//!   conditional write, so a deploy publishes atomically (`deploy.rs`).
//! - `deploy/<name>/<version>/{config.bin,manifest.json}`: one immutable
//!   version of a deployment, its services as an encoded `Config`; `version`
//!   is the SHA-256 of `config.bin` (`deploy.rs`, `compose.rs`).
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
mod client;
pub mod cluster;
pub mod compose;
pub mod deploy;
#[cfg(test)]
mod dst;
#[cfg(test)]
mod fake_http;
pub mod gcs;
mod google_auth;
mod http;
mod keeper;
mod log;
#[cfg(test)]
mod log_tests;
mod owner;
mod ownership;
mod registry;
pub mod s3;

pub use alarms::Adopted;
pub use bucket::Body;
pub use bucket::Bucket;
pub use bucket::Entry;
pub use bucket::Guard;
pub use bucket::Headers;
pub use bucket::Object;
pub use bucket::Page;
pub use bucket::Payload;
pub use bucket::Range;
pub use bucket::Store;
pub use bucket::Version;
pub use bucket::Written;
pub use bucket::refused;
pub use cluster::Cluster;
pub use cluster::Route;
pub use cluster::Setup;
pub use cluster::now_ms;
pub use cluster::open_bucket;
pub use keeper::Fence;
pub use keeper::Progress;
pub use keeper::Within;
pub use log::DB;
pub use log::facet_path;
pub use log::facet_prefix;
pub use owner::Layout;
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
    /// A claim or a commit could not proceed: its capture failed, the
    /// object is not open here, or another owner took it over.
    #[error("replica: {0}")]
    Replica(String),
    /// This instance does not serve the object and cannot claim it: it is draining, or the
    /// object's owner is this address's previous instance. Nothing ran, so a request may follow
    /// the object to whichever instance serves it next.
    #[error("{0}")]
    NotHere(String),
    /// A bucket request failed or answered with an unexpected status.
    #[error("bucket: {0}")]
    Http(String),
    /// The store refused a conditional write's guard, but an earlier attempt
    /// of the same request, or a copy sent beside it, may have been applied,
    /// and the refusal may be of that one's own write: whether the write
    /// landed is unknown. Reading the object back tells, unless a copy still
    /// in flight lands after the read.
    #[error("bucket: {0}; an earlier attempt may have landed")]
    OutcomeUnknown(String),
    #[error("record: {0}")]
    Codec(#[from] serde_json::Error),
    /// Another writer kept winning the conditional write to the key named.
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
    /// A deployment, or the catalog naming it, is refused: one line per
    /// reason.
    #[error("{0}")]
    Deploy(String),
    /// The store answered a request with an error `status` and S3's error
    /// `code`, for the calls whose refusals their callers tell apart: 416
    /// (`InvalidRange`) to a read of a range past the object's end; 404
    /// `NoSuchUpload`, 400 `InvalidPart`, `InvalidPartOrder` or
    /// `EntityTooSmall` to a multipart call. `what` names the request.
    #[error("bucket: {what}: {status} {code}")]
    Refused {
        what: String,
        status: u16,
        code: String,
    },
    /// The bucket returned an object larger than [`http::MAX_BODY`]. The value names the request.
    #[error("bucket: {0}: {too_large}", too_large = http::TOO_LARGE)]
    TooLarge(String),
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

/// MD5 (RFC 1321): S3's `ETag` of an object written in one request, and of
/// each part of a multipart one.
#[expect(clippy::many_single_char_names, reason = "the RFC's names")]
pub fn md5(data: &[u8]) -> [u8; 16] {
    const S: [u32; 16] = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
    #[rustfmt::skip]
    const K: [u32; 64] = [
        0xd76a_a478, 0xe8c7_b756, 0x2420_70db, 0xc1bd_ceee, 0xf57c_0faf, 0x4787_c62a,
        0xa830_4613, 0xfd46_9501, 0x6980_98d8, 0x8b44_f7af, 0xffff_5bb1, 0x895c_d7be,
        0x6b90_1122, 0xfd98_7193, 0xa679_438e, 0x49b4_0821, 0xf61e_2562, 0xc040_b340,
        0x265e_5a51, 0xe9b6_c7aa, 0xd62f_105d, 0x0244_1453, 0xd8a1_e681, 0xe7d3_fbc8,
        0x21e1_cde6, 0xc337_07d6, 0xf4d5_0d87, 0x455a_14ed, 0xa9e3_e905, 0xfcef_a3f8,
        0x676f_02d9, 0x8d2a_4c8a, 0xfffa_3942, 0x8771_f681, 0x6d9d_6122, 0xfde5_380c,
        0xa4be_ea44, 0x4bde_cfa9, 0xf6bb_4b60, 0xbebf_bc70, 0x289b_7ec6, 0xeaa1_27fa,
        0xd4ef_3085, 0x0488_1d05, 0xd9d4_d039, 0xe6db_99e5, 0x1fa2_7cf8, 0xc4ac_5665,
        0xf429_2244, 0x432a_ff97, 0xab94_23a7, 0xfc93_a039, 0x655b_59c3, 0x8f0c_cc92,
        0xffef_f47d, 0x8584_5dd1, 0x6fa8_7e4f, 0xfe2c_e6e0, 0xa301_4314, 0x4e08_11a1,
        0xf753_7e82, 0xbd3a_f235, 0x2ad7_d2bb, 0xeb86_d391,
    ];
    let mut message = data.to_vec();
    message.push(0x80);
    while message.len() % 64 != 56 {
        message.push(0);
    }
    message.extend((data.len() as u64).wrapping_mul(8).to_le_bytes());
    let mut h: [u32; 4] = [0x6745_2301, 0xefcd_ab89, 0x98ba_dcfe, 0x1032_5476];
    for block in message.chunks(64) {
        let word = |g: usize| {
            u32::from_le_bytes([
                block[4 * g],
                block[4 * g + 1],
                block[4 * g + 2],
                block[4 * g + 3],
            ])
        };
        let [mut a, mut b, mut c, mut d] = h;
        for (i, k) in K.iter().enumerate() {
            let (f, g) = match i / 16 {
                0 => ((b & c) | (!b & d), i),
                1 => ((d & b) | (!d & c), (5 * i + 1) % 16),
                2 => (b ^ c ^ d, (3 * i + 5) % 16),
                _ => (c ^ (b | !d), (7 * i) % 16),
            };
            let f = f.wrapping_add(a).wrapping_add(*k).wrapping_add(word(g));
            (a, d, c) = (d, c, b);
            b = b.wrapping_add(f.rotate_left(S[i / 16 * 4 + i % 4]));
        }
        for (h, v) in h.iter_mut().zip([a, b, c, d]) {
            *h = h.wrapping_add(v);
        }
    }
    let mut out = [0; 16];
    for (out, h) in out.chunks_mut(4).zip(h) {
        out.copy_from_slice(&h.to_le_bytes());
    }
    out
}

pub fn unhex(text: &str) -> Option<Vec<u8>> {
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(text.get(i..i + 2)?, 16).ok())
        .collect()
}
