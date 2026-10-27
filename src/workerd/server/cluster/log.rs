// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An object's log in the bucket (`frames.capnp`): one lane, the `SQLite`
//! database, a `sqlitePages` lane whose frames are the pages each commit
//! wrote.
//!
//! An epoch's log is a chain of files under its prefix, each one LZ4
//! frame: `0000/<first>-<last>.log`, the frames of commits `first..=last`,
//! and `0009/0000000000000001-<last>.log`, a snapshot through `last`. A
//! restore is the newest snapshot and the files that extend it; a frame a
//! file before already applied is skipped by its sequence.

use std::collections::BTreeMap;
use std::io::BufReader;
use std::io::Read;
use std::io::Seek;
use std::io::SeekFrom;
use std::io::Write;
use std::path::Path;
use std::path::PathBuf;

use frames_capnp::frame;
use frames_capnp::snapshot_header;
use futures::StreamExt;
use futures::TryStreamExt;
use futures::stream;

use crate::Bucket;
use crate::Error;
use crate::Result;
use crate::Txid;

/// The level of snapshots, as litestream's.
pub const SNAPSHOT_LEVEL: i32 = 9;
/// The database's lane.
const LANE: &str = "db";
/// Downloads in flight during a restore, to hide the store's round trips.
const RESTORE_CONCURRENCY: usize = 16;

/// Where an epoch's next frame goes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Cursor {
    pub session: u32,
    pub sequence: u32,
}

/// A commit's pages, by number, and the database's size after, in pages.
pub type Pages<'a> = (u32, BTreeMap<u32, &'a [u8]>);

#[derive(Clone, Copy)]
pub enum Body<'a> {
    NewLane,
    DeleteLane,
    Pages(&'a Pages<'a>),
}

/// Appends a frame to `out` at `cursor`.
pub fn push(out: &mut Vec<u8>, cursor: &mut Cursor, body: Body) {
    frame_at(out, cursor.session, cursor.sequence, body);
    cursor.sequence += 1;
}

fn frame_at(out: &mut Vec<u8>, session: u32, sequence: u32, body: Body) {
    let mut message = capnp::message::Builder::new_default();
    let mut f = message.init_root::<frame::Builder>();
    f.set_session(session);
    f.set_sequence(sequence);
    match body {
        Body::NewLane => {
            let mut lane = f.init_new_lane();
            lane.set_name(LANE);
            lane.set_type(frames_capnp::lane_description::Type::SqlitePages);
        }
        Body::DeleteLane => f.set_delete_lane(()),
        Body::Pages((commit, pages)) => {
            let mut group = f.init_pages();
            group.set_commit(*commit);
            let mut list = group.init_list(pages.len() as u32);
            for (i, (&number, data)) in pages.iter().enumerate() {
                let mut page = list.reborrow().get(i as u32);
                page.set_number(number);
                page.set_data(data);
            }
        }
    }
    let _ = capnp::serialize::write_message(&mut *out, &message);
}

/// Merges the frames of `entries`, consecutive commits, so each run of
/// page frames becomes one with the newest version of each page.
pub fn merge(entries: &[Vec<u8>]) -> Result<Vec<u8>> {
    let mut messages = Vec::new();
    for entry in entries {
        let mut r = entry.as_slice();
        while let Some(message) = capnp::serialize::try_read_message(&mut r, options())? {
            messages.push(message);
        }
    }
    let mut out = Vec::new();
    let mut run: Option<(u32, u32, Pages)> = None;
    let flush = |out: &mut Vec<u8>, run: &mut Option<(u32, u32, Pages)>| {
        if let Some((session, sequence, (commit, mut pages))) = run.take() {
            pages.retain(|&number, _| number <= commit);
            frame_at(out, session, sequence, Body::Pages(&(commit, pages)));
        }
    };
    for message in &messages {
        let f = message.get_root::<frame::Reader>()?;
        let (session, sequence) = (f.get_session(), f.get_sequence());
        if let frame::Pages(group) = f.which()? {
            let r = run.get_or_insert_with(|| (session, sequence, (0, BTreeMap::new())));
            (r.1, r.2.0) = (sequence, group.get_commit());
            for page in group.get_list()? {
                r.2.1.insert(page.get_number(), page.get_data()?);
            }
        } else {
            flush(&mut out, &mut run);
            let mut copy = capnp::message::Builder::new_default();
            copy.set_root(f)?;
            capnp::serialize::write_message(&mut out, &copy)?;
        }
    }
    flush(&mut out, &mut run);
    Ok(out)
}

/// LZ4-frames `bytes`, as every file in the bucket is.
pub fn compress(bytes: &[u8]) -> Result<Vec<u8>> {
    let mut encoder = encoder(Vec::new());
    encoder.write_all(bytes)?;
    encoder.finish().map_err(|e| Error::Replica(e.to_string()))
}

/// An LZ4 frame with a checksum of its content, which the decoder checks:
/// a corrupt copy fails to replay rather than replaying wrong.
fn encoder<W: Write>(w: W) -> lz4_flex::frame::FrameEncoder<W> {
    let info = lz4_flex::frame::FrameInfo::new().content_checksum(true);
    lz4_flex::frame::FrameEncoder::with_frame_info(info, w)
}

fn options() -> capnp::message::ReaderOptions {
    *capnp::message::ReaderOptions::new().traversal_limit_in_words(None)
}

/// The database lane, replayed into a file.
pub struct Replay {
    db: std::fs::File,
    live: bool,
    /// The next frame, by session and sequence.
    next: Option<(u32, u32)>,
}

impl Replay {
    pub fn new(db: &Path) -> Result<Self> {
        Ok(Self {
            db: std::fs::File::options()
                .read(true)
                .write(true)
                .create(true)
                .truncate(true)
                .open(db)?,
            live: false,
            next: None,
        })
    }

    /// Applies the file `path` of the chain at `level`.
    pub fn apply(&mut self, level: i32, path: &Path) -> Result<()> {
        let file = std::fs::File::open(path)?;
        let mut r = BufReader::new(lz4_flex::frame::FrameDecoder::new(file));
        if level == SNAPSHOT_LEVEL {
            return self.snapshot(&mut r);
        }
        while let Some(message) = capnp::serialize::try_read_message(&mut r, options())? {
            let f = message.get_root::<frame::Reader>()?;
            let at = (f.get_session(), f.get_sequence());
            if self.next.is_some_and(|next| at < next) {
                continue;
            }
            self.next = Some((at.0, at.1 + 1));
            self.frame(f)?;
        }
        Ok(())
    }

    fn snapshot(&mut self, r: &mut impl Read) -> Result<()> {
        let message = capnp::serialize::read_message(&mut *r, options())?;
        let header = message.get_root::<snapshot_header::Reader>()?;
        self.next = Some((header.get_session(), header.get_frame()));
        self.live = false;
        self.db.set_len(0)?;
        self.db.seek(SeekFrom::Start(0))?;
        let lanes = header.get_lanes()?;
        if lanes.len() > 1 {
            return Err(Error::Replica("a snapshot of more than one lane".into()));
        }
        for lane in lanes {
            if let snapshot_header::lane::SqlitePages(lane) = lane.which()? {
                std::io::copy(&mut r.take(lane.get_database_size()), &mut self.db)?;
                self.live = true;
            }
        }
        Ok(())
    }

    fn frame(&mut self, f: frame::Reader) -> Result<()> {
        if f.get_lane() != 0 {
            return Err(Error::Replica(format!("lane {}", f.get_lane())));
        }
        match f.which()? {
            frame::NewLane(_) | frame::DeleteLane(()) => {
                self.db.set_len(0)?;
                self.live = matches!(f.which()?, frame::NewLane(_));
            }
            frame::Pages(group) => {
                let mut page_size = 0;
                for page in group.get_list()? {
                    let data = page.get_data()?;
                    page_size = data.len() as u64;
                    let at = u64::from(page.get_number() - 1) * page_size;
                    self.db.seek(SeekFrom::Start(at))?;
                    self.db.write_all(data)?;
                }
                if page_size > 0 {
                    self.db.set_len(u64::from(group.get_commit()) * page_size)?;
                }
            }
        }
        Ok(())
    }

    /// The replayed database, whole.
    pub fn finish(self) -> Result<()> {
        Ok(self.db.sync_data()?)
    }

    /// Writes the replayed state as a snapshot to `out`.
    pub fn write_snapshot(mut self, out: &Path) -> Result<u64> {
        let (session, frame) = self.next.unwrap_or_default();
        let size = self.db.seek(SeekFrom::End(0))?;
        self.db.seek(SeekFrom::Start(0))?;
        let lane = self.live.then_some(size);
        write_snapshot(out, session, frame, lane, |w| {
            Ok(std::io::copy(&mut self.db, w).map(drop)?)
        })
    }
}

/// Writes a snapshot of the database lane, if it exists, of `size` bytes
/// that `payload` writes.
fn write_snapshot(
    out: &Path,
    session: u32,
    frame: u32,
    size: Option<u64>,
    payload: impl FnOnce(&mut dyn Write) -> Result<()>,
) -> Result<u64> {
    let mut message = capnp::message::Builder::new_default();
    let mut header = message.init_root::<snapshot_header::Builder>();
    header.set_session(session);
    header.set_frame(frame);
    let mut lanes = header.init_lanes(u32::from(size.is_some()));
    if let Some(size) = size {
        let mut lane = lanes.reborrow().get(0);
        lane.set_name(LANE);
        lane.init_sqlite_pages().set_database_size(size);
    }
    let tmp = with_suffix(out, ".tmp");
    let file = std::io::BufWriter::new(std::fs::File::create(&tmp)?);
    let mut encoder = encoder(file);
    capnp::serialize::write_message(&mut encoder, &message)?;
    payload(&mut encoder)?;
    let mut file = encoder
        .finish()
        .map_err(|e| Error::Replica(e.to_string()))?;
    file.flush()?;
    drop(file);
    std::fs::rename(&tmp, out)?;
    Ok(std::fs::metadata(out)?.len())
}

/// Writes the database at `path`, which no connection has open, as a
/// snapshot at the start of `session` into `out`: its size, or `None` for an
/// empty database. Closing the last connection checkpointed the WAL into
/// the file.
pub fn snapshot_file(path: &Path, out: &Path, session: u32) -> Result<Option<u64>> {
    let mut db = match std::fs::File::open(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        db => db?,
    };
    let size = db.metadata()?.len();
    if size < 100 {
        return Ok(None);
    }
    let size = write_snapshot(out, session, 0, Some(size), |w| {
        Ok(std::io::copy(&mut db, w).map(drop)?)
    })?;
    Ok(Some(size))
}

/// Replays the chain's files, local copies, into a snapshot at `out`.
pub fn compact(files: &[(File, PathBuf)], out: &Path) -> Result<u64> {
    let db = with_suffix(out, ".db");
    let mut replay = Replay::new(&db)?;
    for (f, path) in files {
        replay.apply(f.level, path)?;
    }
    let size = replay.write_snapshot(out);
    let _ = std::fs::remove_file(&db);
    size
}

/// One file of an epoch's chain: commits `first..=last` at level 0, or a
/// snapshot through `last` at `SNAPSHOT_LEVEL`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct File {
    pub level: i32,
    pub first: Txid,
    pub last: Txid,
    pub size: u64,
}

impl File {
    /// This node's copy's name.
    pub fn name(&self) -> String {
        format!(
            "{:04x}-{:016x}-{:016x}.log",
            self.level, self.first, self.last
        )
    }

    fn key(&self) -> String {
        format!(
            "{:04x}/{:016x}-{:016x}.log",
            self.level, self.first, self.last
        )
    }
}

/// The files a restore reads, in order: from txid 1 on, each file that
/// extends what came before, through the first gap. A file inside the range
/// already reached is skipped: tiering and bucket-acknowledged commits
/// upload concurrently, so a narrower range can land beside a wider one.
pub fn chain(files: &[File]) -> Vec<File> {
    let mut files = files.to_vec();
    files.sort_by_key(|f| (f.first, std::cmp::Reverse(f.last)));
    let mut reached = 0;
    let mut chain = Vec::new();
    for f in files {
        if f.first > reached + 1 {
            break;
        }
        if f.last > reached {
            reached = f.last;
            chain.push(f);
        }
    }
    chain
}

/// An epoch's log in the bucket.
#[derive(Clone)]
pub struct Log {
    bucket: Bucket,
}

impl Log {
    pub fn new(bucket: Bucket) -> Self {
        Self { bucket }
    }

    pub async fn files(&self) -> Result<Vec<File>> {
        let mut files = Vec::new();
        for level in [0, SNAPSHOT_LEVEL] {
            for entry in self.bucket.list(&format!("{level:04x}/")).await? {
                let name = entry.key.rsplit('/').next().unwrap_or_default();
                let range = name.strip_suffix(".log").and_then(|r| r.split_once('-'));
                let Some((first, last)) = range else {
                    continue;
                };
                let (Ok(first), Ok(last)) = (
                    u64::from_str_radix(first, 16),
                    u64::from_str_radix(last, 16),
                ) else {
                    continue;
                };
                files.push(File {
                    level,
                    first,
                    last,
                    size: 0,
                });
            }
        }
        Ok(files)
    }

    pub async fn get(&self, f: &File, to: &Path) -> Result<()> {
        if self.bucket.get_file(&f.key(), to).await? {
            Ok(())
        } else {
            Err(Error::Replica(format!("{} is missing", f.key())))
        }
    }

    pub async fn put(&self, f: &File, from: &Path) -> Result<()> {
        self.bucket.put_file(&f.key(), from, &[]).await
    }

    pub async fn delete(&self, f: &File) -> Result<()> {
        self.bucket.delete(&f.key()).await
    }
}

/// Restores the log into `output`, which must not exist: the chain's
/// files spooled to `<output>.restore/`, then replayed on the blocking
/// pool. The txid restored through, 0 for an empty log, which leaves no
/// file.
pub async fn restore(log: &Log, output: &Path) -> Result<Txid> {
    let chain = chain(&log.files().await?);
    let Some(end) = chain.last().map(|f| f.last) else {
        return Ok(0);
    };
    let spool = with_suffix(output, ".restore");
    std::fs::create_dir_all(&spool)?;
    let files: Vec<_> = (0..chain.len())
        .map(|i| (chain[i], spool.join(i.to_string())))
        .collect();
    let downloaded = stream::iter(&files)
        .map(|(f, path)| log.get(f, path))
        .buffer_unordered(RESTORE_CONCURRENCY)
        .try_collect::<()>()
        .await;
    let output = output.to_owned();
    let result = match downloaded {
        Ok(()) => {
            crate::owner::blocking(move || {
                let mut replay = Replay::new(&output)?;
                for (f, path) in &files {
                    replay.apply(f.level, path)?;
                }
                replay.finish()
            })
            .await
        }
        Err(e) => Err(e),
    };
    let _ = std::fs::remove_dir_all(&spool);
    result.map(|()| end)
}

pub fn with_suffix(path: &Path, suffix: &str) -> PathBuf {
    let mut path = path.as_os_str().to_owned();
    path.push(suffix);
    path.into()
}
