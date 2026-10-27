// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An object's log in the bucket (`frames.capnp`), laid out as the storage
//! relay's: one `sqlite` lane, the database, whose frames are the bytes
//! each commit wrote to the WAL.
//!
//! Under `do/<namespace>/<actor>/`, each file one LZ4 frame:
//!
//! - `log/<session>/<end>`: a batch, the frames of a run of commits, up to
//!   frame `end`.
//! - `snapshot/<session>/<frame>`: a snapshot, the log continuing from
//!   `frame`.
//!
//! Numbers are length-prefixed hex, sessions and snapshot frames
//! descending, so a listing comes newest first. A session's state is its
//! newest snapshot and the batches after it; a frame a batch before
//! already applied is skipped by its sequence.

use std::io::BufReader;
use std::io::Read;
use std::io::Seek;
use std::io::SeekFrom;
use std::io::Write;
use std::path::Path;
use std::path::PathBuf;

use frames_capnp::SnapshotJustification;
use frames_capnp::frame;
use frames_capnp::snapshot_header;
use futures::StreamExt;
use futures::TryStreamExt;
use futures::stream;

use crate::Bucket;
use crate::Error;
use crate::Result;
use crate::Sequence;

/// The database's lane.
const LANE: &str = "db";
/// Downloads in flight during a restore, to hide the store's round trips.
const RESTORE_CONCURRENCY: usize = 16;
const WAL_HEADER: usize = 32;
const FRAME_HEADER: usize = 24;

/// Where a session's next frame goes, and the database lane's state.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Cursor {
    pub session: u32,
    pub sequence: Sequence,
    /// Whether the lane exists.
    pub lane: bool,
    /// Where the lane's next write goes.
    pub end: u64,
    /// Where the current WAL starts.
    pub base: u64,
}

#[derive(Clone, Copy)]
pub enum Body<'a> {
    NewLane,
    DeleteLane,
    Initialize(&'a [u8]),
    Write(u64, &'a [u8]),
    /// A checkpoint consumed everything written so far.
    Consume,
}

/// Appends a frame to `out` at `cursor`, and moves it past.
pub fn push(out: &mut Vec<u8>, cursor: &mut Cursor, body: Body) {
    let mut message = capnp::message::Builder::new_default();
    let mut f = message.init_root::<frame::Builder>();
    f.set_session(cursor.session);
    f.set_sequence(cursor.sequence);
    match body {
        Body::NewLane => {
            let mut lane = f.init_new_lane();
            lane.set_name(LANE);
            lane.set_sqlite(());
            cursor.lane = true;
        }
        Body::DeleteLane => {
            f.set_delete_lane(());
            (cursor.lane, cursor.end, cursor.base) = (false, 0, 0);
        }
        Body::Initialize(image) => f.set_initialize(image),
        Body::Write(offset, data) => {
            let mut write = f.init_write();
            write.set_offset(offset);
            write.set_data(data);
            cursor.end = offset + data.len() as u64;
        }
        Body::Consume => {
            f.init_consume().set_offset(cursor.end);
            cursor.base = cursor.end;
        }
    }
    let _ = capnp::serialize::write_message(&mut *out, &message);
    cursor.sequence += 1;
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

fn be32(b: &[u8]) -> u32 {
    u32::from_be_bytes([b[0], b[1], b[2], b[3]])
}

/// The database lane, replayed into a file.
pub struct Replay {
    db: std::fs::File,
    live: bool,
    /// The WAL bytes not yet checkpointed, from `base` on.
    wal: Vec<u8>,
    base: u64,
    /// Whether `wal` starts with a WAL header, rather than a frame past a
    /// snapshot.
    header: bool,
    /// The next frame, by session and sequence.
    next: Option<(u32, Sequence)>,
    log_bytes: u64,
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
            wal: Vec::new(),
            base: 0,
            header: true,
            next: None,
            log_bytes: 0,
        })
    }

    /// Applies `f`, the local copy at `path`. A batch that does not reach
    /// back to the next frame is skipped: a later, wider one covers it.
    pub fn apply(&mut self, f: &File, path: &Path) -> Result<()> {
        let file = std::fs::File::open(path)?;
        let size = file.metadata()?.len();
        let mut r = BufReader::new(lz4_flex::frame::FrameDecoder::new(file));
        if f.snapshot {
            return self.snapshot(&mut r);
        }
        while let Some(message) = capnp::serialize::try_read_message(&mut r, options())? {
            let f = message.get_root::<frame::Reader>()?;
            let at = (f.get_session(), f.get_sequence());
            match self.next.unwrap_or((at.0, 0)) {
                next if at < next => continue,
                next if at > next => return Ok(()),
                _ => self.next = Some((at.0, at.1 + 1)),
            }
            self.frame(f)?;
        }
        self.log_bytes += size;
        Ok(())
    }

    fn snapshot(&mut self, r: &mut impl Read) -> Result<()> {
        let message = capnp::serialize::read_message(&mut *r, options())?;
        let header = message.get_root::<snapshot_header::Reader>()?;
        self.next = Some((header.get_session(), header.get_frame()));
        (self.wal, self.base, self.live, self.log_bytes) = (Vec::new(), 0, false, 0);
        self.db.set_len(0)?;
        self.db.seek(SeekFrom::Start(0))?;
        let lanes = header.get_lanes()?;
        if lanes.len() > 1 {
            return Err(Error::Replica("a snapshot of more than one lane".into()));
        }
        for lane in lanes {
            if let snapshot_header::lane::Sqlite(sqlite) = lane.which()? {
                std::io::copy(&mut r.take(sqlite.get_database_size()), &mut self.db)?;
                r.take(sqlite.get_wal_size()).read_to_end(&mut self.wal)?;
                (self.base, self.live) = (sqlite.get_wal_base_offset(), true);
                self.header = !self.wal.is_empty();
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
                (self.wal, self.base, self.header) = (Vec::new(), 0, true);
                self.live = matches!(f.which()?, frame::NewLane(_));
            }
            frame::Initialize(image) => {
                self.db.set_len(0)?;
                self.db.seek(SeekFrom::Start(0))?;
                self.db.write_all(image?)?;
            }
            frame::Write(w) => {
                if w.get_offset() != self.base + self.wal.len() as u64 {
                    return Err(Error::Replica(format!("a write at {}", w.get_offset())));
                }
                self.wal.extend_from_slice(w.get_data()?);
            }
            frame::Consume(c) => {
                self.checkpoint()?;
                (self.base, self.header) = (c.get_offset(), true);
            }
            _ => return Err(Error::Replica("an unsupported frame".into())),
        }
        Ok(())
    }

    /// Copies the WAL's committed pages into the database.
    fn checkpoint(&mut self) -> Result<()> {
        let wal = std::mem::take(&mut self.wal);
        self.base += wal.len() as u64;
        let (page_size, frames) = if std::mem::replace(&mut self.header, false) {
            let Some(size) = wal.get(8..12) else {
                return Ok(());
            };
            (be32(size) as usize, &wal[WAL_HEADER..])
        } else if wal.is_empty() {
            return Ok(());
        } else {
            // Past a snapshot, the database's header has the page size.
            let mut size = [0; 2];
            self.db.seek(SeekFrom::Start(16))?;
            self.db.read_exact(&mut size)?;
            let size = usize::from(u16::from_be_bytes(size));
            (if size == 1 { 1 << 16 } else { size }, &wal[..])
        };
        let frames: Vec<_> = frames.chunks_exact(FRAME_HEADER + page_size).collect();
        let Some(last) = frames.iter().rposition(|f| be32(&f[4..]) != 0) else {
            return Ok(());
        };
        for f in &frames[..=last] {
            let at = u64::from(be32(f) - 1) * page_size as u64;
            self.db.seek(SeekFrom::Start(at))?;
            self.db.write_all(&f[FRAME_HEADER..])?;
        }
        let pages = u64::from(be32(&frames[last][4..]));
        Ok(self.db.set_len(pages * page_size as u64)?)
    }

    /// The replayed database, whole, and the frame the log continues from.
    pub fn finish(mut self) -> Result<Sequence> {
        self.checkpoint()?;
        self.db.sync_data()?;
        Ok(self.next.map_or(0, |n| n.1))
    }

    /// Writes the replayed state as a snapshot into `dir`.
    pub fn write_snapshot(mut self, dir: &Path) -> Result<File> {
        self.checkpoint()?;
        let (session, frame) = self.next.unwrap_or_default();
        let size = self.db.seek(SeekFrom::End(0))?;
        self.db.seek(SeekFrom::Start(0))?;
        let lane = self.live.then_some((size, self.base));
        let header = Header {
            session,
            frame,
            justification: SnapshotJustification::ClientRequested,
            log_bytes: self.log_bytes,
        };
        write_snapshot(dir, &header, lane, |w| {
            Ok(std::io::copy(&mut self.db, w).map(drop)?)
        })
    }
}

struct Header {
    session: u32,
    frame: Sequence,
    justification: SnapshotJustification,
    log_bytes: u64,
}

/// Writes a snapshot into `dir`: the database lane, if it exists, as its
/// size and where the log's next write goes, with content `payload` writes.
fn write_snapshot(
    dir: &Path,
    h: &Header,
    lane: Option<(u64, u64)>,
    payload: impl FnOnce(&mut dyn Write) -> Result<()>,
) -> Result<File> {
    let mut message = capnp::message::Builder::new_default();
    let mut header = message.init_root::<snapshot_header::Builder>();
    header.set_session(h.session);
    header.set_frame(h.frame);
    header.set_justification(h.justification);
    header.set_log_bytes_since_last_snapshot(h.log_bytes);
    let mut lanes = header.init_lanes(u32::from(lane.is_some()));
    if let Some((size, base)) = lane {
        let mut l = lanes.reborrow().get(0);
        l.set_name(LANE);
        let mut sqlite = l.init_sqlite();
        sqlite.set_database_size(size);
        sqlite.set_wal_base_offset(base);
    }
    let file = File {
        snapshot: true,
        session: h.session,
        frame: h.frame,
        size: 0,
    };
    let out = dir.join(file.name());
    let tmp = with_suffix(&out, ".tmp");
    let mut encoder = encoder(std::io::BufWriter::new(std::fs::File::create(&tmp)?));
    capnp::serialize::write_message(&mut encoder, &message)?;
    payload(&mut encoder)?;
    let mut w = encoder
        .finish()
        .map_err(|e| Error::Replica(e.to_string()))?;
    w.flush()?;
    drop(w);
    std::fs::rename(&tmp, &out)?;
    let size = std::fs::metadata(&out)?.len();
    Ok(File { size, ..file })
}

/// Writes the database at `path`, which no connection has open, as the
/// snapshot `session` starts from into `dir`, or `None` for an empty
/// database. Closing the last connection checkpointed the WAL into the
/// file.
pub fn snapshot_file(path: &Path, dir: &Path, session: u32) -> Result<Option<File>> {
    let mut db = match std::fs::File::open(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        db => db?,
    };
    let size = db.metadata()?.len();
    if size < 100 {
        return Ok(None);
    }
    let header = Header {
        session,
        frame: 0,
        justification: SnapshotJustification::ColdStartMitigation,
        log_bytes: 0,
    };
    let file = write_snapshot(dir, &header, Some((size, 0)), |w| {
        Ok(std::io::copy(&mut db, w).map(drop)?)
    })?;
    Ok(Some(file))
}

/// Replays `files`, a plan with local copies, into a snapshot in `dir`.
pub fn compact(files: &[(File, PathBuf)], dir: &Path) -> Result<File> {
    let db = dir.join("compact.db");
    let mut replay = Replay::new(&db)?;
    for (f, path) in files {
        replay.apply(f, path)?;
    }
    let snapshot = replay.write_snapshot(dir);
    let _ = std::fs::remove_file(&db);
    snapshot
}

/// One file of a session's log: a batch up to frame `frame`, or a snapshot
/// the log continues from at `frame`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct File {
    pub snapshot: bool,
    pub session: u32,
    pub frame: Sequence,
    pub size: u64,
}

impl File {
    /// This node's copy's name.
    pub fn name(&self) -> String {
        let kind = if self.snapshot { "snapshot" } else { "log" };
        format!("{kind}-{:08x}-{:08x}", self.session, self.frame)
    }

    fn key(&self) -> String {
        let session = descending(self.session.into());
        if self.snapshot {
            format!("snapshot/{session}/{}", descending(self.frame.into()))
        } else {
            format!("log/{session}/{}", ascending(self.frame.into()))
        }
    }
}

/// Hex, prefixed with its length so names sort as numbers: `a0`, `a1`, ..,
/// `bff`.
fn ascending(v: u64) -> String {
    let hex = format!("{v:x}");
    format!("{}{hex}", char::from(b'a' + hex.len() as u8 - 1))
}

/// Likewise, sorting in reverse: `zf`, `ze`, .., `y00`.
fn descending(v: u64) -> String {
    let hex = format!("{v:x}");
    let digits: String = hex.bytes().map(|d| char::from(flip(d))).collect();
    format!("{}{digits}", char::from(b'z' + 1 - hex.len() as u8))
}

/// A hex digit's complement, `0` for `f`.
fn flip(digit: u8) -> u8 {
    let value = if digit > b'9' {
        digit - b'a' + 10
    } else {
        digit - b'0'
    };
    b"fedcba9876543210"[usize::from(value)]
}

fn parse(name: &str, ascending: bool) -> Option<u64> {
    let (&prefix, digits) = name.as_bytes().split_first()?;
    let len = if ascending {
        prefix.checked_sub(b'a')? + 1
    } else {
        (b'z' + 1).checked_sub(prefix)?
    };
    let valid = |d: &u8| d.is_ascii_digit() || (b'a'..=b'f').contains(d);
    if digits.len() != usize::from(len) || !digits.iter().all(valid) {
        return None;
    }
    let digits: String = digits
        .iter()
        .map(|&d| char::from(if ascending { d } else { flip(d) }))
        .collect();
    u64::from_str_radix(&digits, 16).ok()
}

/// What a restore of `files`, a session's, reads, in order: the newest
/// snapshot, then each batch past it.
pub fn plan(files: &[File]) -> Vec<File> {
    let snapshot = files.iter().filter(|f| f.snapshot).max_by_key(|f| f.frame);
    let from = snapshot.map_or(0, |f| f.frame);
    let mut batches: Vec<_> = files
        .iter()
        .filter(|f| !f.snapshot && f.frame > from)
        .copied()
        .collect();
    batches.sort_by_key(|f| f.frame);
    snapshot.into_iter().copied().chain(batches).collect()
}

/// An object's log in the bucket.
#[derive(Clone)]
pub struct Log {
    bucket: Bucket,
}

impl Log {
    pub fn new(bucket: Bucket) -> Self {
        Self { bucket }
    }

    /// The files of `session`.
    pub async fn files(&self, session: u32) -> Result<Vec<File>> {
        let mut files = Vec::new();
        for snapshot in [false, true] {
            let kind = if snapshot { "snapshot" } else { "log" };
            let prefix = format!("{kind}/{}/", descending(session.into()));
            for entry in self.bucket.list(&prefix).await? {
                let name = entry.key.rsplit('/').next().unwrap_or_default();
                if let Some(frame) = parse(name, !snapshot).and_then(|f| f.try_into().ok()) {
                    files.push(File {
                        snapshot,
                        session,
                        frame,
                        size: 0,
                    });
                }
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

/// Restores `session` of the log into `output`, which must not exist: the
/// plan's files spooled to `<output>.restore/`, then replayed on the
/// blocking pool. The frame the log continues from, 0 for an empty log,
/// which leaves no file.
pub async fn restore(log: &Log, session: u32, output: &Path) -> Result<Sequence> {
    let plan = plan(&log.files(session).await?);
    if plan.is_empty() {
        return Ok(0);
    }
    let spool = with_suffix(output, ".restore");
    std::fs::create_dir_all(&spool)?;
    let files: Vec<_> = (0..plan.len())
        .map(|i| (plan[i], spool.join(i.to_string())))
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
                    replay.apply(f, path)?;
                }
                replay.finish()
            })
            .await
        }
        Err(e) => Err(e),
    };
    let _ = std::fs::remove_dir_all(&spool);
    result
}

pub fn with_suffix(path: &Path, suffix: &str) -> PathBuf {
    let mut path = path.as_os_str().to_owned();
    path.push(suffix);
    path.into()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_sort_as_their_numbers() {
        let values = [0, 1, 0xf, 0x10, 0xff, 0x100, 0xffff_ffff];
        let up: Vec<_> = values.iter().map(|&v| ascending(v)).collect();
        let down: Vec<_> = values.iter().map(|&v| descending(v)).collect();
        assert_eq!(up[..3], ["a0", "a1", "af"]);
        assert_eq!(down[..3], ["zf", "ze", "z0"]);
        assert!(up.is_sorted());
        assert!(down.iter().rev().is_sorted());
        for (v, (up, down)) in values.iter().zip(up.iter().zip(&down)) {
            assert_eq!(parse(up, true), Some(*v));
            assert_eq!(parse(down, false), Some(*v));
        }
        assert_eq!(parse("b1", true), None);
    }

    fn file(snapshot: bool, frame: Sequence) -> File {
        File {
            snapshot,
            session: 1,
            frame,
            size: 0,
        }
    }

    #[test]
    fn a_plan_is_the_newest_snapshot_and_the_batches_past_it() {
        let (old, new) = (file(true, 0), file(true, 4));
        let files = [file(false, 9), file(false, 4), new, old, file(false, 6)];
        assert_eq!(plan(&files), [new, file(false, 6), file(false, 9)]);
        assert_eq!(plan(&files[..2]), [file(false, 4), file(false, 9)]);
        assert_eq!(plan(&[]), []);
    }
}
