// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A Durable Object's commits, cut from the writes `SQLite` makes to its WAL
//! as workerd's VFS reports them: per transaction, the frames of
//! `frames.capnp` that write its bytes to the database lane.
//!
//! `SQLite` writes a frame as its 24-byte header and then its page, at offsets
//! that follow the 32-byte WAL header, and the header of a transaction's
//! last frame carries the database's size, which marks the commit. A write
//! at offset 0 starts a new WAL, after a checkpoint has copied every
//! earlier frame into the database file. So the bytes since the last commit
//! are buffered, and the write that completes a commit frame completes one
//! transaction: a write of those bytes at the lane's next offset, which
//! keeps growing across checkpoints, each marked by a consume. A
//! rolled-back transaction's frames start where the next one's do, which
//! overwrites them.

use std::collections::BTreeMap;
use std::path::Path;
use std::path::PathBuf;

use crate::Error;
use crate::Result;
use crate::Sequence;
use crate::log::Body;
use crate::log::Cursor;
use crate::log::push;

const WAL_HEADER: u64 = 32;
const FRAME_HEADER: u64 = 24;

pub struct Capture {
    /// The database file.
    path: PathBuf,
    /// The session's frames not yet in the bucket, by commit, keyed by the
    /// frame after it.
    entries: BTreeMap<Sequence, Vec<u8>>,
    cursor: Cursor,
    page_size: u64,
    /// Where in the WAL the next transaction's bytes start; `None` before a
    /// WAL header.
    start: Option<u64>,
    /// What was written from `start` on.
    pending: Vec<u8>,
    /// A checkpoint consumed the lane's WAL since the last commit.
    consumed: bool,
    /// Why the capture can no longer be trusted, if it cannot.
    lost: Option<String>,
}

impl Capture {
    /// Capture the database at `path`, which no connection has open yet,
    /// starting from what it holds now.
    #[cfg(test)]
    pub fn open(path: &Path) -> Result<Self> {
        let mut capture = Self::resume(path, Cursor::default());
        capture.restart()?;
        Ok(capture)
    }

    /// Continue a session at `cursor` over the database at `path`, which
    /// holds exactly the session's log so far and no connection has open
    /// yet. Without a lane, the next commit creates it from the file, since
    /// a new database's first page reaches the file, not the WAL.
    pub fn resume(path: &Path, cursor: Cursor) -> Self {
        Self {
            path: path.to_owned(),
            entries: BTreeMap::new(),
            cursor,
            page_size: 0,
            start: None,
            pending: Vec::new(),
            consumed: false,
            lost: None,
        }
    }

    /// Start the session over from the database file, which holds every
    /// commit: no WAL, or one a checkpoint truncated.
    #[cfg(test)]
    pub fn restart(&mut self) -> Result<()> {
        self.entries.clear();
        self.start = None;
        self.cursor = Cursor {
            session: self.cursor.session,
            ..Cursor::default()
        };
        let image = match std::fs::read(&self.path) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Vec::new(),
            image => image?,
        };
        if !image.is_empty() {
            let mut entry = Vec::new();
            push(&mut entry, &mut self.cursor, Body::NewLane);
            push(&mut entry, &mut self.cursor, Body::Initialize(&image));
            self.entries.insert(self.cursor.sequence, entry);
        }
        Ok(())
    }

    /// The database's files were deleted: the session continues without the
    /// lane, and the next commit creates it.
    pub fn reset(&mut self) {
        self.start = None;
        self.pending.clear();
        self.consumed = false;
        if !self.cursor.lane {
            return;
        }
        let mut entry = Vec::new();
        push(&mut entry, &mut self.cursor, Body::DeleteLane);
        self.entries.insert(self.cursor.sequence, entry);
    }

    /// A write `SQLite` made to the WAL.
    pub fn write(&mut self, offset: u64, data: &[u8]) {
        if offset == 0 {
            // A new WAL: a checkpoint copied the last one into the file.
            if let Some(size) = data.get(8..12) {
                self.page_size =
                    u64::from(u32::from_be_bytes([size[0], size[1], size[2], size[3]]));
                self.consumed |= self.cursor.end > self.cursor.base;
                self.start = Some(0);
                self.pending.clear();
            }
        }
        let Some(start) = self.start.filter(|&s| offset >= s) else {
            self.lost = Some(format!("a WAL write at {offset}, before {:?}", self.start));
            return;
        };
        let at = usize::try_from(offset - start).unwrap_or(usize::MAX);
        let end = at.saturating_add(data.len());
        if self.pending.len() < end {
            self.pending.resize(end, 0);
        }
        self.pending[at..end].copy_from_slice(data);
        let size = FRAME_HEADER + self.page_size;
        let wal_end = start + end as u64;
        if wal_end <= WAL_HEADER || !(wal_end - WAL_HEADER).is_multiple_of(size) {
            return;
        }
        let header = &self.pending[end - size as usize..];
        if u32::from_be_bytes([header[4], header[5], header[6], header[7]]) != 0 {
            self.commit(start, end);
        }
    }

    /// The bytes before `end` are one committed transaction.
    fn commit(&mut self, start: u64, end: usize) {
        let mut entry = Vec::new();
        if !self.cursor.lane {
            match std::fs::read(&self.path) {
                Ok(image) => {
                    push(&mut entry, &mut self.cursor, Body::NewLane);
                    push(&mut entry, &mut self.cursor, Body::Initialize(&image));
                }
                Err(e) => self.lost = Some(format!("the new database: {e}")),
            }
        }
        if std::mem::take(&mut self.consumed) {
            push(&mut entry, &mut self.cursor, Body::Consume);
        }
        let at = self.cursor.base + start;
        let bytes = &self.pending[..end];
        push(&mut entry, &mut self.cursor, Body::Write(at, bytes));
        self.start = Some(start + end as u64);
        self.pending.drain(..end);
        self.entries.insert(self.cursor.sequence, entry);
    }

    /// The frame after the last commit, or why the capture cannot be
    /// trusted.
    pub fn last(&self) -> Result<Sequence> {
        match &self.lost {
            None => Ok(self.cursor.sequence),
            Some(why) => Err(Error::Replica(format!("capture lost: {why}"))),
        }
    }

    pub fn cursor(&self) -> Cursor {
        self.cursor
    }

    /// The frames from `from` up to `to`, both after a commit.
    pub fn batch(&self, from: Sequence, to: Sequence) -> Result<Vec<u8>> {
        let mut entries = self.entries.range(from + 1..=to).peekable();
        if entries.peek().is_none() {
            return Err(Error::Replica(format!("no captured frames {from}..{to}")));
        }
        Ok(entries.flat_map(|(_, e)| e.iter().copied()).collect())
    }

    /// The bucket holds everything up to `to`.
    pub fn forget(&mut self, to: Sequence) {
        self.entries.retain(|&end, _| end > to);
    }
}
