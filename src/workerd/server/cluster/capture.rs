// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A Durable Object's commits, cut from the writes `SQLite` makes to its WAL
//! as workerd's VFS reports them: one log entry, a frame of the pages it
//! wrote, per transaction.
//!
//! `SQLite` writes a frame as its 24-byte header and then its page, at offsets
//! that follow the 32-byte WAL header, and the header of a transaction's
//! last frame carries the database's size, which marks the commit. A write
//! at offset 0 starts a new WAL, after a checkpoint has copied every
//! earlier frame into the database file. So the frames since the last
//! commit are buffered, and the write that completes a commit frame
//! completes one transaction. A rolled-back transaction's frames start
//! where the next one's do, which overwrites them.

use std::collections::BTreeMap;
use std::path::Path;
use std::path::PathBuf;

use crate::Error;
use crate::Result;
use crate::Txid;
use crate::log::Body;
use crate::log::Cursor;
use crate::log::push;

const WAL_HEADER: u64 = 32;
const FRAME_HEADER: usize = 24;

pub struct Capture {
    /// The database file.
    path: PathBuf,
    /// The chain's entries not yet in the bucket, by txid.
    entries: BTreeMap<Txid, Vec<u8>>,
    last: Txid,
    cursor: Cursor,
    page_size: usize,
    /// Where the next transaction's first frame goes: 0 before a WAL header.
    start: u64,
    /// What was written from `start` on.
    frames: Vec<u8>,
    /// The lane does not exist: the next commit creates it with every page
    /// of the file, since a new database's first page reaches the file, not
    /// the WAL.
    fresh: bool,
    /// Why the capture can no longer be trusted, if it cannot.
    lost: Option<String>,
}

impl Capture {
    /// Capture the database at `path`, which no connection has open yet:
    /// what it holds now is txid 1.
    #[cfg(test)]
    pub fn open(path: &Path) -> Result<Self> {
        let mut capture = Self::resume(path, 0, Cursor::default());
        capture.restart()?;
        Ok(capture)
    }

    /// Continue a chain through `last`, at `cursor`, over the database at
    /// `path`, which holds exactly that chain and no connection has open
    /// yet. A chain at 0 starts with the next commit.
    pub fn resume(path: &Path, last: Txid, cursor: Cursor) -> Self {
        Self {
            path: path.to_owned(),
            entries: BTreeMap::new(),
            last,
            cursor,
            page_size: 0,
            start: 0,
            frames: Vec::new(),
            fresh: last == 0,
            lost: None,
        }
    }

    /// Start a new chain from the database file, which holds every commit:
    /// no WAL, or one a checkpoint truncated.
    #[cfg(test)]
    pub fn restart(&mut self) -> Result<()> {
        self.entries.clear();
        (self.last, self.cursor.sequence) = (0, 0);
        let image = match std::fs::read(&self.path) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Vec::new(),
            image => image?,
        };
        self.fresh = image.is_empty();
        if !self.fresh {
            let commit = file_pages(&image);
            let mut entry = Vec::new();
            push(&mut entry, &mut self.cursor, Body::NewLane);
            push(&mut entry, &mut self.cursor, Body::Pages(&commit));
            self.last = 1;
            self.entries.insert(1, entry);
        }
        Ok(())
    }

    /// The database's files were deleted: the chain continues without the
    /// lane, and the next commit creates it.
    pub fn reset(&mut self) {
        self.start = 0;
        self.frames.clear();
        self.fresh = true;
        if self.last == 0 {
            return;
        }
        let mut entry = Vec::new();
        push(&mut entry, &mut self.cursor, Body::DeleteLane);
        self.last += 1;
        self.entries.insert(self.last, entry);
    }

    /// A write `SQLite` made to the WAL.
    pub fn write(&mut self, offset: u64, data: &[u8]) {
        if offset == 0 {
            // A new WAL: a checkpoint copied the last one into the file.
            if let Some(size) = data.get(8..12) {
                self.page_size = u32::from_be_bytes([size[0], size[1], size[2], size[3]]) as usize;
                self.start = WAL_HEADER;
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
        if !end.is_multiple_of(size) {
            return;
        }
        let header = &self.frames[end - size..end - size + FRAME_HEADER];
        let commit = u32::from_be_bytes([header[4], header[5], header[6], header[7]]);
        if commit != 0 {
            self.commit(end, commit);
        }
    }

    /// The frames before `end` are one committed transaction.
    fn commit(&mut self, end: usize, commit: u32) {
        let mut entry = Vec::new();
        let image = if self.fresh {
            self.fresh = false;
            std::fs::read(&self.path).unwrap_or_else(|e| {
                self.lost = Some(format!("the new database: {e}"));
                Vec::new()
            })
        } else {
            Vec::new()
        };
        let (_, mut pages) = file_pages(&image);
        if !image.is_empty() {
            push(&mut entry, &mut self.cursor, Body::NewLane);
        }
        let size = FRAME_HEADER + self.page_size;
        for frame in self.frames[..end].chunks_exact(size) {
            let pgno = u32::from_be_bytes([frame[0], frame[1], frame[2], frame[3]]);
            pages.insert(pgno, &frame[FRAME_HEADER..]);
        }
        pages.retain(|&pgno, _| pgno <= commit);
        push(&mut entry, &mut self.cursor, Body::Pages(&(commit, pages)));
        self.start += end as u64;
        self.frames.drain(..end);
        self.last += 1;
        self.entries.insert(self.last, entry);
    }

    /// The last committed txid, or why the capture cannot be trusted.
    pub fn last(&self) -> Result<Txid> {
        match &self.lost {
            None => Ok(self.last),
            Some(why) => Err(Error::Replica(format!("capture lost: {why}"))),
        }
    }

    pub fn cursor(&self) -> Cursor {
        self.cursor
    }

    pub fn entry(&self, txid: Txid) -> Result<Vec<u8>> {
        self.entries
            .get(&txid)
            .cloned()
            .ok_or_else(|| Error::Replica(format!("no captured entry {txid}")))
    }

    /// The bucket holds everything through `txid`.
    pub fn forget(&mut self, txid: Txid) {
        self.entries.retain(|t, _| *t > txid);
    }
}

/// A database file's pages and size, in pages.
fn file_pages(image: &[u8]) -> crate::log::Pages<'_> {
    let page_size = match image.get(16..18) {
        Some(&[0, 1]) => 65536,
        Some(size) => usize::from(u16::from_be_bytes([size[0], size[1]])),
        None => return (0, BTreeMap::new()),
    };
    let pages: BTreeMap<_, _> = (1..).zip(image.chunks_exact(page_size)).collect();
    (pages.len() as u32, pages)
}
