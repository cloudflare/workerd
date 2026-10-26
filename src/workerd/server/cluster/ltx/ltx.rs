//! ltx.rs — LTX (Lite Transaction) file reader/writer + CRC64-ISO checksums.
//!
//! Ported from ltx@v0.5.2 `ltx.go`, `checksum.go`, `encoder.go`, `decoder.go`
//! and litestream@v0.5.11 `v3.go`. `reference/ltx-format.md` describes the
//! authoritative byte layout.
//!
//! The reader decodes a complete LTX file with either v0.5.2 LZ4 blocks or the
//! older LZ4 frames. It verifies the CRC64-ISO file checksum and the rolling
//! snapshot checksum. The v0.5.2 writer emits exact upstream bytes, while the
//! default writer preserves the legacy layout during the staged rollout.

use std::time::SystemTime;

use crate::ltx::CHECKSUM_FLAG;
use crate::ltx::Checksum;
use crate::ltx::TXID;
use crate::ltx::error::Error;
use crate::ltx::error::Result;

// ── Constants (ltx@v0.5.2 ltx.go:18-55) ──────────────────────────────────────

/// First 4 bytes of every LTX file.
pub const MAGIC: &[u8; 4] = b"LTX1";
/// Current LTX file format version.
pub const VERSION: i32 = 3;
pub const HEADER_SIZE: usize = 100;
pub const PAGE_HEADER_SIZE: usize = 6;
pub const TRAILER_SIZE: usize = 16;
pub const CHECKSUM_SIZE: usize = 8;

/// Header flag: checksums are not tracked for this file.
pub const HEADER_FLAG_NO_CHECKSUM: u32 = 1 << 1;
pub const HEADER_FLAG_MASK: u32 = HEADER_FLAG_NO_CHECKSUM;

/// A four-byte compressed-size field follows the page header, and the page
/// uses raw LZ4 block compression. Files written before ltx v0.5.2 omit this
/// flag and contain one LZ4 frame per page.
pub const PAGE_HEADER_FLAG_SIZE: u16 = 1 << 0;
pub const PAGE_HEADER_FLAG_MASK: u16 = PAGE_HEADER_FLAG_SIZE;

/// SQLite PENDING_BYTE offset; the lock page derives from it.
pub const PENDING_BYTE: i64 = 0x4000_0000;

fn corrupt(msg: impl Into<String>) -> Error {
    // Wrap a format error as LTXCorrupted, matching litestream's classification
    // of malformed LTX content (litestream.go ErrLTXCorrupted).
    let _ = msg;
    Error::LTXCorrupted
}

/// Returns the lock page number for a given page size (ltx.go:494).
///
/// `page_size` is expected to be a validated SQLite page size (a power of two in
/// `[512, 65536]`); for any such value the result is identical to Go's
/// `LockPgno` (`PENDING_BYTE / page_size + 1`). A `page_size` of `0` — which
/// only reaches here via an unvalidated/adversarial header — would make the
/// underlying integer divide panic in both Go and Rust, so we guard it and
/// return `0` (never a real page number) instead of dividing. All in-crate
/// callers validate the header first, mirroring Go's `DecodeHeader`-before-
/// `LockPgno` ordering, so this guard is reached only by a direct external call.
pub fn lock_pgno(page_size: u32) -> u32 {
    if page_size == 0 {
        return 0;
    }
    (PENDING_BYTE / page_size as i64) as u32 + 1
}

// ── CRC64-ISO (checksum.go:177 `crc64.MakeTable(crc64.ISO)`) ──────────────────

/// Streaming CRC64-ISO hasher matching Go's `hash/crc64` digest semantics
/// (CRC-64/GO-ISO). celld uses `crc-fast` for SIMD folding; this is Go's own
/// byte-at-a-time table walk (crc64.go `update`), which avoids a dependency.
#[derive(Clone, Default)]
pub struct Crc64 {
    crc: u64,
}

/// Go's `crc64.MakeTable(crc64.ISO)`: the reflected polynomial table.
const CRC64_ISO_TABLE: [u64; 256] = {
    let mut table = [0u64; 256];
    let mut i = 0;
    while i < 256 {
        let mut crc = i as u64;
        let mut bit = 0;
        while bit < 8 {
            crc = if crc & 1 == 1 {
                (crc >> 1) ^ 0xD800_0000_0000_0000
            } else {
                crc >> 1
            };
            bit += 1;
        }
        table[i] = crc;
        i += 1;
    }
    table
};

impl Crc64 {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn update(&mut self, data: &[u8]) {
        let mut crc = !self.crc;
        for &b in data {
            crc = CRC64_ISO_TABLE[(crc as u8 ^ b) as usize] ^ (crc >> 8);
        }
        self.crc = !crc;
    }

    pub fn sum64(&self) -> u64 {
        self.crc
    }
}

/// CRC64 checksum of a single page combined with its page number, with the
/// ChecksumFlag set (checksum.go:106-116). Input is `BE_u32(pgno) ++ data`.
pub fn checksum_page(pgno: u32, data: &[u8]) -> Checksum {
    let mut h = Crc64::new();
    h.update(&pgno.to_be_bytes());
    h.update(data);
    CHECKSUM_FLAG | h.sum64()
}

// ── Header / PageHeader / Trailer ─────────────────────────────────────────────

/// LTX file header (100 bytes). Ported from ltx@v0.5.1 ltx.go:179-326.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Header {
    pub version: i32,
    pub flags: u32,
    pub page_size: u32,
    pub commit: u32,
    pub min_txid: TXID,
    pub max_txid: TXID,
    pub timestamp: i64,
    pub pre_apply_checksum: Checksum,
    pub wal_offset: i64,
    pub wal_size: i64,
    pub wal_salt1: u32,
    pub wal_salt2: u32,
    pub node_id: u64,
}

impl Header {
    /// True if this header begins a complete database snapshot (MinTXID == 1).
    pub fn is_snapshot(&self) -> bool {
        self.min_txid == TXID(1)
    }

    /// True if checksum tracking is disabled for this file.
    pub fn no_checksum(&self) -> bool {
        self.flags & HEADER_FLAG_NO_CHECKSUM != 0
    }

    /// Decodes a header from a 100-byte slice (ltx.go:302-326). All big-endian.
    pub fn parse(b: &[u8]) -> Result<Header> {
        if b.len() < HEADER_SIZE {
            return Err(corrupt("short header"));
        }
        if &b[0..4] != MAGIC {
            return Err(corrupt("bad magic"));
        }
        Ok(Header {
            version: VERSION,
            flags: u32_be(&b[4..]),
            page_size: u32_be(&b[8..]),
            commit: u32_be(&b[12..]),
            min_txid: TXID(u64_be(&b[16..])),
            max_txid: TXID(u64_be(&b[24..])),
            timestamp: u64_be(&b[32..]) as i64,
            pre_apply_checksum: u64_be(&b[40..]),
            wal_offset: u64_be(&b[48..]) as i64,
            wal_size: u64_be(&b[56..]) as i64,
            wal_salt1: u32_be(&b[64..]),
            wal_salt2: u32_be(&b[68..]),
            node_id: u64_be(&b[72..]),
        })
    }

    /// Encodes the header to 100 bytes (ltx.go:283-299).
    pub fn marshal(&self) -> [u8; HEADER_SIZE] {
        let mut b = [0u8; HEADER_SIZE];
        b[0..4].copy_from_slice(MAGIC);
        b[4..8].copy_from_slice(&self.flags.to_be_bytes());
        b[8..12].copy_from_slice(&self.page_size.to_be_bytes());
        b[12..16].copy_from_slice(&self.commit.to_be_bytes());
        b[16..24].copy_from_slice(&self.min_txid.0.to_be_bytes());
        b[24..32].copy_from_slice(&self.max_txid.0.to_be_bytes());
        b[32..40].copy_from_slice(&(self.timestamp as u64).to_be_bytes());
        b[40..48].copy_from_slice(&self.pre_apply_checksum.to_be_bytes());
        b[48..56].copy_from_slice(&(self.wal_offset as u64).to_be_bytes());
        b[56..64].copy_from_slice(&(self.wal_size as u64).to_be_bytes());
        b[64..68].copy_from_slice(&self.wal_salt1.to_be_bytes());
        b[68..72].copy_from_slice(&self.wal_salt2.to_be_bytes());
        b[72..80].copy_from_slice(&self.node_id.to_be_bytes());
        b
    }

    /// Validates header invariants (ltx.go:208-267).
    pub fn validate(&self) -> Result<()> {
        if self.version != VERSION {
            return Err(corrupt("invalid version"));
        }
        if self.flags != (self.flags & HEADER_FLAG_MASK) {
            return Err(corrupt("invalid flags"));
        }
        if !is_valid_page_size(self.page_size) {
            return Err(corrupt("invalid page size"));
        }
        if self.min_txid == TXID(0) {
            return Err(corrupt("minimum transaction id required"));
        }
        if self.max_txid == TXID(0) {
            return Err(corrupt("maximum transaction id required"));
        }
        if self.min_txid > self.max_txid {
            return Err(corrupt("transaction ids out of order"));
        }
        if self.wal_offset < 0 {
            return Err(corrupt("wal offset cannot be negative"));
        }
        if self.wal_size < 0 {
            return Err(corrupt("wal size cannot be negative"));
        }
        if (self.wal_salt1 != 0 || self.wal_salt2 != 0) && self.wal_offset == 0 {
            return Err(corrupt("wal offset required if salt exists"));
        }
        if self.wal_offset == 0 && self.wal_size != 0 {
            return Err(corrupt("wal offset required if wal size exists"));
        }
        if self.is_snapshot() {
            if self.pre_apply_checksum != 0 {
                return Err(corrupt("pre-apply checksum must be zero on snapshots"));
            }
        } else if self.no_checksum() {
            if self.pre_apply_checksum != 0 {
                return Err(corrupt("pre-apply checksum not allowed"));
            }
        } else {
            if self.pre_apply_checksum == 0 {
                return Err(corrupt("pre-apply checksum required on non-snapshot files"));
            }
            if self.pre_apply_checksum & CHECKSUM_FLAG == 0 {
                return Err(corrupt("invalid pre-apply checksum format"));
            }
        }
        Ok(())
    }
}

/// Per-page header (6 bytes). Ported from ltx.go:408-447.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct PageHeader {
    pub pgno: u32,
    pub flags: u16,
}

impl PageHeader {
    pub fn is_zero(&self) -> bool {
        self.pgno == 0 && self.flags == 0
    }

    pub fn parse(b: &[u8]) -> Result<PageHeader> {
        if b.len() < PAGE_HEADER_SIZE {
            return Err(corrupt("short page header"));
        }
        Ok(PageHeader {
            pgno: u32_be(&b[0..]),
            flags: u16_be(&b[4..]),
        })
    }

    pub fn marshal(&self) -> [u8; PAGE_HEADER_SIZE] {
        let mut b = [0u8; PAGE_HEADER_SIZE];
        b[0..4].copy_from_slice(&self.pgno.to_be_bytes());
        b[4..6].copy_from_slice(&self.flags.to_be_bytes());
        b
    }

    pub fn validate(&self) -> Result<()> {
        if self.pgno == 0 {
            return Err(corrupt("page number required"));
        }
        if self.flags != (self.flags & PAGE_HEADER_FLAG_MASK) {
            return Err(corrupt("invalid page header flags"));
        }
        Ok(())
    }
}

/// File trailer (16 bytes). Ported from ltx.go:348-393.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Trailer {
    pub post_apply_checksum: Checksum,
    pub file_checksum: Checksum,
}

impl Trailer {
    /// Validates the checksum fields against the header checksum mode.
    pub fn validate(&self, header: Header) -> Result<()> {
        if header.no_checksum() {
            if self.post_apply_checksum != 0 {
                return Err(corrupt("post-apply checksum not allowed"));
            }
        } else if self.post_apply_checksum == 0 || self.post_apply_checksum & CHECKSUM_FLAG == 0 {
            return Err(corrupt("invalid post-apply checksum"));
        }

        if self.file_checksum == 0 || self.file_checksum & CHECKSUM_FLAG == 0 {
            return Err(corrupt("invalid file checksum"));
        }
        Ok(())
    }

    pub fn parse(b: &[u8]) -> Result<Trailer> {
        if b.len() < TRAILER_SIZE {
            return Err(corrupt("short trailer"));
        }
        Ok(Trailer {
            post_apply_checksum: u64_be(&b[0..]),
            file_checksum: u64_be(&b[8..]),
        })
    }

    pub fn marshal(&self) -> [u8; TRAILER_SIZE] {
        let mut b = [0u8; TRAILER_SIZE];
        b[0..8].copy_from_slice(&self.post_apply_checksum.to_be_bytes());
        b[8..16].copy_from_slice(&self.file_checksum.to_be_bytes());
        b
    }
}

/// True if `sz` is a power of two in [512, 65536] (ltx.go:399-406).
pub fn is_valid_page_size(sz: u32) -> bool {
    let mut i = 512u32;
    while i <= 65536 {
        if sz == i {
            return true;
        }
        i *= 2;
    }
    false
}

/// Formats an LTX filename for a transaction range (ltx.go:487-489).
pub fn format_filename(min_txid: TXID, max_txid: TXID) -> String {
    format!("{}-{}.ltx", min_txid, max_txid)
}

/// Parses a `<min>-<max>.ltx` filename (ltx.go:450-459).
pub fn parse_filename(name: &str) -> Result<(TXID, TXID)> {
    let stem = name
        .strip_suffix(".ltx")
        .ok_or_else(|| corrupt("invalid ltx filename"))?;
    let (a, b) = stem
        .split_once('-')
        .ok_or_else(|| corrupt("invalid ltx filename"))?;
    if a.len() != 16 || b.len() != 16 {
        return Err(corrupt("invalid ltx filename"));
    }
    let min = u64::from_str_radix(a, 16).map_err(|_| corrupt("invalid ltx filename"))?;
    let max = u64::from_str_radix(b, 16).map_err(|_| corrupt("invalid ltx filename"))?;
    Ok((TXID(min), TXID(max)))
}

// ── FileInfo ──────────────────────────────────────────────────────────────────

/// Metadata about an LTX file on a replica. Ported from ltx@v0.5.1 ltx.go:571-596.
///
/// `pre_apply_checksum`/`post_apply_checksum` are populated when known (e.g. by
/// decoding) and are zero when a file is discovered by a bare directory/bucket
/// listing. Listings leave `size` and `created_at` unset, and write results use
/// the LTX header timestamp.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct FileInfo {
    pub level: i32,
    pub min_txid: TXID,
    pub max_txid: TXID,
    pub pre_apply_checksum: Checksum,
    pub post_apply_checksum: Checksum,
    pub size: i64,
    pub created_at: Option<SystemTime>,
}

// ── Decoder ──────────────────────────────────────────────────────────────────

// ── Encoder (round-trip; byte-fidelity vs the real binary is D1's job) ────────

/// Encodes a complete LTX file with the legacy LZ4 frame representation,
/// which the decoder reads alongside the v0.5.2 block
/// representation.
pub fn encode_file(
    header: &Header,
    pages: &[(u32, Vec<u8>)],
    post_apply_checksum: Checksum,
) -> Result<Vec<u8>> {
    let mut encoder = crate::ltx::codec::Encoder::new(Vec::new());
    encoder.encode_header(*header)?;
    for (page_number, data) in pages {
        encoder.encode_page(
            PageHeader {
                pgno: *page_number,
                flags: 0,
            },
            data,
        )?;
    }
    encoder.close(post_apply_checksum)?;
    Ok(encoder.writer)
}

/// Merges consecutive LTX files into one covering all their txids, as
/// ltx's compactor does.
pub fn merge_files(files: &[Vec<u8>]) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    merge_into(files.iter().map(Vec::as_slice).collect(), &mut out)?;
    Ok(out)
}

/// `merge_files`, from readers to a writer, a page at a time.
pub fn merge_into<R: std::io::Read>(inputs: Vec<R>, out: impl std::io::Write) -> Result<()> {
    let mut merge = Merge::new(inputs)?;
    let mut encoder = crate::ltx::codec::Encoder::new(out);
    encoder.encode_header(merge.header)?;
    while let Some((pgno, data)) = merge.next()? {
        encoder.encode_page(PageHeader { pgno, flags: 0 }, data)?;
    }
    encoder.close(merge.finish()?)
}

/// Consecutive LTX files, oldest first, read as one: the latest version of
/// each page, through the last file's commit. Every file keeps its pages in
/// ascending order, so this holds a page per file, never a database.
pub struct Merge<R> {
    /// The merged file's header.
    pub header: Header,
    inputs: Vec<Input<R>>,
    page: Vec<u8>,
}

struct Input<R> {
    decoder: crate::ltx::codec::Decoder<R>,
    /// The page `page` holds; `None` once the file's pages are read.
    pgno: Option<u32>,
    page: Vec<u8>,
}

impl<R: std::io::Read> Input<R> {
    fn advance(&mut self) -> Result<()> {
        self.pgno = self.decoder.decode_page(&mut self.page)?.map(|p| p.pgno);
        Ok(())
    }
}

impl<R: std::io::Read> Merge<R> {
    pub fn new(readers: Vec<R>) -> Result<Self> {
        let mut inputs = Vec::new();
        for reader in readers {
            let mut decoder = crate::ltx::codec::Decoder::new(reader);
            decoder.decode_header()?;
            let page = vec![0; decoder.header.page_size as usize];
            inputs.push(Input {
                decoder,
                pgno: None,
                page,
            });
        }
        let (Some(first), Some(last)) = (inputs.first(), inputs.last()) else {
            return Err(corrupt("nothing to merge"));
        };
        let (first, last) = (first.decoder.header, last.decoder.header);
        if inputs
            .iter()
            .any(|i| i.decoder.header.page_size != last.page_size)
        {
            return Err(corrupt("mismatched page sizes"));
        }
        let header = Header {
            flags: first.flags,
            min_txid: first.min_txid,
            pre_apply_checksum: first.pre_apply_checksum,
            ..last
        };
        for input in &mut inputs {
            input.advance()?;
        }
        let page = vec![0; header.page_size as usize];
        Ok(Self {
            header,
            inputs,
            page,
        })
    }

    /// The next page, in ascending order; `None` after the last.
    pub fn next(&mut self) -> Result<Option<(u32, &[u8])>> {
        loop {
            let Some(pgno) = self.inputs.iter().filter_map(|i| i.pgno).min() else {
                return Ok(None);
            };
            let newest = self.inputs.iter_mut().rev().find(|i| i.pgno == Some(pgno));
            if let Some(newest) = newest {
                std::mem::swap(&mut self.page, &mut newest.page);
            }
            for input in &mut self.inputs {
                if input.pgno == Some(pgno) {
                    input.advance()?;
                }
            }
            if pgno <= self.header.commit {
                return Ok(Some((pgno, &self.page)));
            }
        }
    }

    /// Checks every file's trailer: the last file's post-apply checksum.
    pub fn finish(mut self) -> Result<Checksum> {
        for input in &mut self.inputs {
            input.decoder.close()?;
        }
        Ok(self
            .inputs
            .last()
            .map_or(0, |i| i.decoder.trailer.post_apply_checksum))
    }
}

// ── small byte / varint helpers ──────────────────────────────────────────────

fn u16_be(b: &[u8]) -> u16 {
    u16::from_be_bytes([b[0], b[1]])
}
fn u32_be(b: &[u8]) -> u32 {
    u32::from_be_bytes([b[0], b[1], b[2], b[3]])
}
fn u64_be(b: &[u8]) -> u64 {
    u64::from_be_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]])
}

// ── Tests ─────────────────────────────────────────────────────────────────────
