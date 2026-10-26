// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A Durable Object's commits, cut from the writes `SQLite` makes to its WAL
//! as workerd's VFS reports them: one LTX entry per transaction.
//!
//! `SQLite` writes a frame as its 24-byte header and then its page, at offsets
//! that follow the 32-byte WAL header, and the header of a transaction's
//! last frame carries the database's size, which marks the commit. A write
//! at offset 0 starts a new WAL generation, after a checkpoint has copied
//! every earlier frame into the database file. So the frames since the
//! last commit are buffered, and the write that completes a commit frame
//! completes one transaction. A rolled-back transaction's frames start
//! where the next one's do, which overwrites them.

use std::collections::BTreeMap;
use std::collections::HashMap;
use std::fs::File;
use std::io::Read;
use std::io::Seek;
use std::io::SeekFrom;
use std::io::Write;
use std::path::Path;
use std::path::PathBuf;

use crate::Error;
use crate::Result;
use crate::Txid;
use crate::ltx::TXID;
use crate::ltx::codec::Encoder;
use crate::ltx::ltx::HEADER_FLAG_NO_CHECKSUM;
use crate::ltx::ltx::Header;
use crate::ltx::ltx::PageHeader;
use crate::ltx::ltx::VERSION;
use crate::ltx::ltx::encode_file;
use crate::ltx::ltx::lock_pgno;

const WAL_HEADER: u64 = 32;
const FRAME_HEADER: usize = 24;

pub struct Capture {
    /// The database file.
    path: PathBuf,
    /// The chain's entries not yet in the bucket or this node's journal, by
    /// txid.
    entries: BTreeMap<Txid, Vec<u8>>,
    last: Txid,
    page_size: usize,
    /// Where the next transaction's first frame goes: 0 before a WAL header.
    start: u64,
    /// What was written from `start` on.
    frames: Vec<u8>,
    /// Where this WAL generation's latest committed version of each page
    /// is, for a snapshot, and the database's size at the last commit.
    pages: HashMap<u32, u64>,
    commit: u32,
    /// The database was deleted: the next commit is a whole image, since a
    /// new database's first page reaches the file, not the WAL.
    fresh: bool,
    /// Why the capture can no longer be trusted, if it cannot.
    lost: Option<String>,
}

impl Capture {
    /// Capture the database at `path`, which no connection has open yet:
    /// what it holds now is the snapshot at txid 1, held in memory.
    #[cfg(test)]
    pub fn open(path: &Path) -> Result<Self> {
        let mut capture = Self::resume(path, 0);
        capture.restart()?;
        Ok(capture)
    }

    /// Continue a chain through `last` over the database at `path`, which
    /// holds exactly that chain and no connection has open yet: an object
    /// woken where it was evicted.
    pub fn resume(path: &Path, last: Txid) -> Self {
        Self {
            path: path.to_owned(),
            entries: BTreeMap::new(),
            last,
            page_size: 0,
            start: 0,
            frames: Vec::new(),
            pages: HashMap::new(),
            commit: 0,
            fresh: false,
            lost: None,
        }
    }

    /// Start a new chain from a snapshot of what is committed now.
    #[cfg(test)]
    pub fn restart(&mut self) -> Result<()> {
        self.entries.clear();
        self.last = 0;
        if let Some(snapshot) = self.snapshot(1, 1)? {
            self.last = 1;
            self.entries.insert(1, snapshot);
        }
        Ok(())
    }

    /// The database's files were deleted: the chain continues with an empty
    /// database, and the next commit carries every page.
    pub fn reset(&mut self) {
        (self.start, self.pages, self.commit) = (0, HashMap::new(), 0);
        self.frames.clear();
        self.fresh = true;
        if self.last == 0 {
            return;
        }
        let page_size = if self.page_size == 0 {
            4096
        } else {
            self.page_size
        };
        self.last += 1;
        match encode(self.last, self.last, page_size, 0, &[]) {
            Ok(entry) => drop(self.entries.insert(self.last, entry)),
            Err(e) => self.lost = Some(format!("txid {}: {e}", self.last)),
        }
    }

    /// The database file overlaid with this WAL generation's committed
    /// pages, read back from the WAL, as one entry covering `first` through
    /// `last`. `None` for an empty database.
    fn snapshot(&mut self, first: Txid, last: Txid) -> Result<Option<Vec<u8>>> {
        let mut image = match std::fs::read(&self.path) {
            Ok(bytes) => bytes,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Vec::new(),
            Err(e) => return Err(crate::ltx::Error::from(e).into()),
        };
        let page_size = match image.get(16..18) {
            Some(&[0, 1]) => 65536,
            Some(size) => usize::from(u16::from_be_bytes([size[0], size[1]])),
            None => self.page_size,
        };
        if page_size == 0 {
            return Ok(None);
        }
        self.page_size = page_size;
        let mut wal = self.path.as_os_str().to_owned();
        wal.push("-wal");
        let wal = match File::open(wal) {
            // Closing the last connection checkpoints the WAL into the
            // database file and deletes it: the file holds every page, until
            // a reopened connection writes a new WAL header.
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
            Ok(wal) if wal.metadata().is_ok_and(|m| m.len() == 0) => None,
            wal => Some(wal.map_err(crate::ltx::Error::from)?),
        };
        if let Some(mut wal) = wal.filter(|_| !self.pages.is_empty()) {
            for (&pgno, &offset) in &self.pages {
                let at = (pgno as usize - 1) * page_size;
                if image.len() < at + page_size {
                    image.resize(at + page_size, 0);
                }
                wal.seek(SeekFrom::Start(offset))
                    .and_then(|_| wal.read_exact(&mut image[at..at + page_size]))
                    .map_err(crate::ltx::Error::from)?;
            }
            image.truncate(self.commit as usize * page_size);
        }
        let lock = lock_pgno(page_size as u32);
        let pages: Vec<(u32, Vec<u8>)> = (1..)
            .zip(image.chunks_exact(page_size))
            .filter(|(pgno, _)| *pgno != lock)
            .map(|(pgno, page)| (pgno, page.to_vec()))
            .collect();
        let commit = (image.len() / page_size) as u32;
        (commit > 0)
            .then(|| encode(first, last, page_size, commit, &pages))
            .transpose()
    }

    /// A write `SQLite` made to the WAL.
    pub fn write(&mut self, offset: u64, data: &[u8]) {
        if offset == 0 {
            // A new generation: a checkpoint copied the last one into the
            // database file.
            if let Some(size) = data.get(8..12) {
                self.page_size = u32::from_be_bytes([size[0], size[1], size[2], size[3]]) as usize;
                (self.start, self.pages) = (WAL_HEADER, HashMap::new());
                self.frames.clear();
            }
            return;
        }
        if self.start == 0 || offset < self.start {
            self.lost = Some(format!("a WAL write at {offset}, before {}", self.start));
            return;
        }
        let at = usize::try_from(offset - self.start).unwrap_or(usize::MAX);
        let end = at.saturating_add(data.len());
        if self.frames.len() < end {
            self.frames.resize(end, 0);
        }
        self.frames[at..end].copy_from_slice(data);
        let size = FRAME_HEADER + self.page_size;
        if end % size != 0 {
            return;
        }
        let header = &self.frames[end - size..end - size + FRAME_HEADER];
        let commit = u32::from_be_bytes([header[4], header[5], header[6], header[7]]);
        if commit != 0 {
            self.commit_frames(end, commit);
        }
    }

    /// The frames before `end` are one committed transaction.
    fn commit_frames(&mut self, end: usize, commit: u32) {
        let size = FRAME_HEADER + self.page_size;
        let mut pages = BTreeMap::new();
        for (i, frame) in self.frames[..end].chunks_exact(size).enumerate() {
            let pgno = u32::from_be_bytes([frame[0], frame[1], frame[2], frame[3]]);
            self.pages
                .insert(pgno, self.start + (i * size + FRAME_HEADER) as u64);
            if pgno <= commit {
                pages.insert(pgno, frame[FRAME_HEADER..].to_vec());
            }
        }
        let pages: Vec<(u32, Vec<u8>)> = pages.into_iter().collect();
        self.start += end as u64;
        self.frames.drain(..end);
        self.commit = commit;
        // A chain with no snapshot yet starts with one: a new database's
        // first page reaches the file, not the WAL, when WAL mode is set.
        let entry = if self.last == 0 || self.fresh {
            self.fresh = false;
            let first = if self.last == 0 { 1 } else { self.last + 1 };
            self.snapshot(first, self.last + 1).and_then(|s| {
                s.ok_or_else(|| Error::Replica("a commit to an empty database".into()))
            })
        } else {
            encode(self.last + 1, self.last + 1, self.page_size, commit, &pages)
        };
        self.last += 1;
        match entry {
            Ok(entry) => drop(self.entries.insert(self.last, entry)),
            Err(e) => self.lost = Some(format!("txid {}: {e}", self.last)),
        }
    }

    /// The last committed txid, or why the capture cannot be trusted.
    pub fn last(&self) -> Result<Txid> {
        match &self.lost {
            None => Ok(self.last),
            Some(why) => Err(Error::Replica(format!("capture lost: {why}"))),
        }
    }

    pub fn entry(&self, txid: Txid) -> Result<Vec<u8>> {
        self.entries
            .get(&txid)
            .cloned()
            .ok_or_else(|| Error::Replica(format!("no captured entry {txid}")))
    }

    /// The bucket, or this node's journal, holds everything through `txid`.
    pub fn forget(&mut self, txid: Txid) {
        self.entries.retain(|t, _| *t > txid);
    }
}

/// Encodes the database at `path`, which no connection has open, as the
/// snapshot at txid 1 into `out`, a page at a time: its size, or `None` for
/// an empty database. Closing the last connection checkpointed the WAL into
/// the file.
pub fn snapshot_file(path: &Path, out: &Path) -> Result<Option<u64>> {
    let io = |e: std::io::Error| Error::from(crate::ltx::Error::from(e));
    let file = match File::open(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        file => file.map_err(io)?,
    };
    let len = file.metadata().map_err(io)?.len();
    let mut reader = std::io::BufReader::new(file);
    let mut first = [0; 100];
    if len < first.len() as u64 {
        return Ok(None);
    }
    reader.read_exact(&mut first).map_err(io)?;
    let page_size = match u16::from_be_bytes([first[16], first[17]]) {
        1 => 65536,
        size => usize::from(size),
    };
    let header = Header {
        version: VERSION,
        flags: HEADER_FLAG_NO_CHECKSUM,
        page_size: page_size as u32,
        commit: (len / page_size as u64) as u32,
        min_txid: TXID(1),
        max_txid: TXID(1),
        ..Header::default()
    };
    let mut writer = std::io::BufWriter::new(File::create(out).map_err(io)?);
    let mut encoder = Encoder::new(&mut writer);
    encoder.encode_header(header)?;
    let mut page = vec![0; page_size];
    page[..first.len()].copy_from_slice(&first);
    reader.read_exact(&mut page[first.len()..]).map_err(io)?;
    for pgno in 1..=header.commit {
        if pgno > 1 {
            reader.read_exact(&mut page).map_err(io)?;
        }
        if pgno != lock_pgno(header.page_size) {
            encoder.encode_page(PageHeader { pgno, flags: 0 }, &page)?;
        }
    }
    encoder.close(0)?;
    writer.flush().map_err(io)?;
    Ok(Some(std::fs::metadata(out).map_err(io)?.len()))
}

fn encode(
    first: Txid,
    last: Txid,
    page_size: usize,
    commit: u32,
    pages: &[(u32, Vec<u8>)],
) -> Result<Vec<u8>> {
    let header = Header {
        version: VERSION,
        flags: HEADER_FLAG_NO_CHECKSUM,
        page_size: page_size as u32,
        commit,
        min_txid: TXID(first),
        max_txid: TXID(last),
        ..Header::default()
    };
    Ok(encode_file(&header, pages, 0)?)
}
