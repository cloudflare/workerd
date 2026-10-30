// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A Durable Object's commits, cut from the writes `SQLite` makes to its
//! databases' WALs as workerd's VFS reports them: per transaction, a frame
//! of `frames.capnp` on the database's lane with the pages it wrote.
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
use std::collections::HashMap;
use std::path::Path;
use std::path::PathBuf;

use crate::Error;
use crate::Result;
use crate::Sequence;
use crate::log::Body;
use crate::log::Cursor;
use crate::log::push;

const WAL_HEADER: u64 = 32;
const FRAME_HEADER: usize = 24;

/// An object's commits, across its databases, each a lane.
pub struct Capture {
    /// Each database's WAL, by the database's file name.
    wals: HashMap<String, Wal>,
    /// The session's frames not yet in the bucket, by commit, keyed by the
    /// frame after it.
    entries: BTreeMap<Sequence, Vec<u8>>,
    cursor: Cursor,
    /// Why the capture can no longer be trusted, if it cannot.
    lost: Option<String>,
}

/// One database's WAL, cut into transactions.
struct Wal {
    path: PathBuf,
    lane: String,
    page_size: usize,
    /// Where the next transaction's first frame goes: 0 before a WAL header.
    start: u64,
    /// What was written from `start` on.
    frames: Vec<u8>,
}

impl Wal {
    fn new(path: &Path, lane: &str) -> Self {
        Self {
            path: path.to_owned(),
            lane: lane.to_owned(),
            page_size: 0,
            start: 0,
            frames: Vec::new(),
        }
    }
}

fn file_name(path: &Path) -> String {
    path.file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default()
}

impl Capture {
    /// Capture the root database at `path`, which no connection has open
    /// yet, starting from what it holds now.
    #[cfg(test)]
    pub fn open(path: &Path) -> Result<Self> {
        let mut capture = Self::resume(Cursor::default(), &[]);
        capture.open_lane(path, crate::log::DB);
        capture.restart()?;
        Ok(capture)
    }

    /// Continue a session at `cursor` over `databases`, lane names and
    /// files that hold exactly the session's log so far, which no
    /// connection has open yet. A lane without a number is created from
    /// its file by its next commit, since a new database's first page
    /// reaches the file, not the WAL.
    pub fn resume(cursor: Cursor, databases: &[(String, PathBuf)]) -> Self {
        let mut capture = Self {
            wals: HashMap::new(),
            entries: BTreeMap::new(),
            cursor,
            lost: None,
        };
        for (lane, path) in databases {
            capture.open_lane(path, lane);
        }
        capture
    }

    /// Capture lane `lane`'s database at `path` too, before a connection
    /// opens it.
    pub fn open_lane(&mut self, path: &Path, lane: &str) {
        self.wals.insert(file_name(path), Wal::new(path, lane));
    }

    /// Each captured database's lane and file.
    pub fn databases(&self) -> Vec<(String, PathBuf)> {
        let mut databases: Vec<_> = self
            .wals
            .values()
            .map(|w| (w.lane.clone(), w.path.clone()))
            .collect();
        databases.sort();
        databases
    }

    /// Start the session over from the database files, which hold every
    /// commit: no WAL, or one a checkpoint truncated.
    #[cfg(test)]
    pub fn restart(&mut self) -> Result<()> {
        self.entries.clear();
        self.cursor = Cursor {
            session: self.cursor.session,
            ..Cursor::default()
        };
        let mut entry = Vec::new();
        for (lane, path) in self.databases() {
            let image = match std::fs::read(&path) {
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
                image => image?,
            };
            if !image.is_empty() {
                let n = push(&mut entry, &mut self.cursor, 0, Body::NewLane(&lane));
                push(&mut entry, &mut self.cursor, n, Body::Initialize(&image));
            }
        }
        if !entry.is_empty() {
            self.entries.insert(self.cursor.sequence, entry);
        }
        Ok(())
    }

    /// The database `file` was deleted, with its facet's descendants: the
    /// session continues without their lanes, and the database's next
    /// commit creates its lane.
    pub fn reset(&mut self, file: &str) {
        let Some(wal) = self.wals.get_mut(file) else {
            return self.lose(format!("a reset of {file}, which is not captured"));
        };
        (wal.start, wal.page_size) = (0, 0);
        wal.frames.clear();
        let lane = wal.lane.clone();
        let prefix = lane.strip_suffix(crate::log::DB).unwrap_or_default();
        self.delete(|l| l == lane || l.starts_with(prefix) && l[prefix.len()..].contains('/'));
    }

    /// The facet with lane prefix `prefix` was deleted with its
    /// descendants, their databases closed and their files removed.
    pub fn delete_lanes(&mut self, prefix: &str) {
        self.wals.retain(|_, w| !w.lane.starts_with(prefix));
        self.delete(|l| l.starts_with(prefix));
    }

    /// Delete the live lanes `doomed` picks, in one entry.
    fn delete(&mut self, doomed: impl Fn(&str) -> bool) {
        let mut entry = Vec::new();
        for n in 0..self.cursor.lanes.len() {
            if self.cursor.lanes[n].as_deref().is_some_and(&doomed) {
                push(&mut entry, &mut self.cursor, n as u32, Body::DeleteLane);
            }
        }
        if !entry.is_empty() {
            self.entries.insert(self.cursor.sequence, entry);
        }
    }

    fn lose(&mut self, why: String) {
        self.lost.get_or_insert(why);
    }

    /// A write `SQLite` made to the WAL of the database `file`.
    pub fn write(&mut self, file: &str, offset: u64, data: &[u8]) {
        let Some(wal) = self.wals.get_mut(file) else {
            return self.lose(format!("a write to {file}, which is not captured"));
        };
        if offset == 0 {
            // A new WAL: a checkpoint copied the last one into the file.
            if let Some(size) = data.get(8..12) {
                wal.page_size = u32::from_be_bytes([size[0], size[1], size[2], size[3]]) as usize;
                wal.start = WAL_HEADER;
                wal.frames.clear();
            }
            return;
        }
        if wal.start == 0 || offset < wal.start {
            let why = format!("a WAL write to {file} at {offset}, before {}", wal.start);
            return self.lose(why);
        }
        let at = usize::try_from(offset - wal.start).unwrap_or(usize::MAX);
        let end = at.saturating_add(data.len());
        if wal.frames.len() < end {
            wal.frames.resize(end, 0);
        }
        wal.frames[at..end].copy_from_slice(data);
        let size = FRAME_HEADER + wal.page_size;
        if !end.is_multiple_of(size) {
            return;
        }
        let header = &wal.frames[end - size..end - size + FRAME_HEADER];
        let commit = u32::from_be_bytes([header[4], header[5], header[6], header[7]]);
        if commit != 0 {
            self.commit(file, end, commit);
        }
    }

    /// The frames before `end` in the WAL of `file` are one committed
    /// transaction.
    fn commit(&mut self, file: &str, end: usize, commit: u32) {
        let Some(wal) = self.wals.get_mut(file) else {
            return;
        };
        let mut entry = Vec::new();
        let lane = match self.cursor.lane(&wal.lane) {
            Some(n) => n,
            None => match std::fs::read(&wal.path) {
                Ok(image) => {
                    let n = push(&mut entry, &mut self.cursor, 0, Body::NewLane(&wal.lane));
                    push(&mut entry, &mut self.cursor, n, Body::Initialize(&image));
                    n
                }
                Err(e) => {
                    self.lost
                        .get_or_insert_with(|| format!("the new database {file}: {e}"));
                    return;
                }
            },
        };
        let mut pages = BTreeMap::new();
        for frame in wal.frames[..end].chunks_exact(FRAME_HEADER + wal.page_size) {
            let pgno = u32::from_be_bytes([frame[0], frame[1], frame[2], frame[3]]);
            pages.insert(pgno, &frame[FRAME_HEADER..]);
        }
        pages.retain(|&pgno, _| pgno <= commit);
        push(
            &mut entry,
            &mut self.cursor,
            lane,
            Body::Pages(commit, &pages),
        );
        wal.start += end as u64;
        wal.frames.drain(..end);
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
        self.cursor.clone()
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

#[cfg(test)]
#[path = "capture-test.rs"]
mod tests;
