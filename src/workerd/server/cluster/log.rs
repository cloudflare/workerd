// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An object's log in the bucket (`frames.capnp`), laid out as the storage
//! relay's: one `sqlitePages` lane, the database, whose frames are the
//! pages each commit wrote.
//!
//! Under `do/<namespace>/<actor>/`, each file one LZ4 frame:
//!
//! - `log/<session>/<end>`: a batch, the frames of a run of commits up to
//!   frame `end`, each run of page frames merged to one.
//! - `snapshot/<session>/<frame>`: a snapshot, the log continuing from
//!   `frame`.
//!
//! Numbers are length-prefixed hex, sessions and snapshot frames
//! descending, so a listing comes newest first. A session's state is its
//! newest snapshot and the batches after it; a frame a batch before
//! already applied is skipped by its sequence.

use std::collections::BTreeMap;
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

/// Where a session's next frame goes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Cursor {
    pub session: u32,
    pub sequence: Sequence,
    /// Whether the database lane exists.
    pub lane: bool,
}

/// Pages by number.
pub type Pages<'a> = BTreeMap<u32, &'a [u8]>;

#[derive(Clone, Copy)]
pub enum Body<'a> {
    NewLane,
    DeleteLane,
    Initialize(&'a [u8]),
    /// A commit's pages, and the database's size after, in pages.
    Pages(u32, &'a Pages<'a>),
}

/// Appends a frame to `out` at `cursor`, and moves it past.
pub fn push(out: &mut Vec<u8>, cursor: &mut Cursor, body: Body) {
    match body {
        Body::NewLane => cursor.lane = true,
        Body::DeleteLane => cursor.lane = false,
        _ => {}
    }
    let (session, sequence) = (cursor.session, cursor.sequence);
    frame_at(out, session, sequence..=sequence, body);
    cursor.sequence += 1;
}

/// Appends a frame covering `sequences` to `out`.
fn frame_at(
    out: &mut Vec<u8>,
    session: u32,
    sequences: std::ops::RangeInclusive<Sequence>,
    body: Body,
) {
    let mut message = capnp::message::Builder::new_default();
    let mut f = message.init_root::<frame::Builder>();
    f.set_session(session);
    f.set_sequence(*sequences.end());
    match body {
        Body::NewLane => {
            let mut lane = f.init_new_lane();
            lane.set_name(LANE);
            lane.set_sqlite_pages(());
        }
        Body::DeleteLane => f.set_delete_lane(()),
        Body::Initialize(image) => f.set_initialize(image),
        Body::Pages(commit, pages) => {
            let mut group = f.init_pages();
            group.set_first(*sequences.start());
            group.set_commit(commit);
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

/// `batch`, a run of frames, with each run of page frames merged to one
/// with the newest version of each page.
pub fn merge(batch: &[u8]) -> Result<Vec<u8>> {
    let mut messages = Vec::new();
    let mut r = batch;
    while let Some(message) = capnp::serialize::try_read_message(&mut r, options())? {
        messages.push(message);
    }
    let mut out = Vec::new();
    // The run's session, first and last sequence, size, and pages.
    let mut run: Option<(u32, Sequence, Sequence, u32, Pages)> = None;
    let flush = |out: &mut Vec<u8>, run: &mut Option<(u32, Sequence, Sequence, u32, Pages)>| {
        if let Some((session, first, last, commit, mut pages)) = run.take() {
            pages.retain(|&number, _| number <= commit);
            frame_at(out, session, first..=last, Body::Pages(commit, &pages));
        }
    };
    for message in &messages {
        let f = message.get_root::<frame::Reader>()?;
        let (session, sequence) = (f.get_session(), f.get_sequence());
        if let frame::Pages(group) = f.which()? {
            let first = group.get_first();
            let r = run.get_or_insert_with(|| (session, first, sequence, 0, BTreeMap::new()));
            (r.2, r.3) = (sequence, group.get_commit());
            for page in group.get_list()? {
                r.4.insert(page.get_number(), page.get_data()?);
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

/// Bounds a message, so that a corrupt one fails rather than allocating
/// whatever its segment table claims: 1 GiB, far more than a batch holds.
fn options() -> capnp::message::ReaderOptions {
    *capnp::message::ReaderOptions::new().traversal_limit_in_words(Some(1 << 27))
}

/// Fails unless `file` is one whole LZ4 frame, through its end mark and
/// checksum: the decoder takes a frame cut at a block boundary for a
/// whole, shorter one.
fn check_whole(file: &mut std::fs::File) -> Result<()> {
    let truncated = || Error::Replica("a truncated file".into());
    let len = file.metadata()?.len();
    let mut header = [0u8; 6];
    file.read_exact(&mut header).map_err(|_| truncated())?;
    let flags = header[4];
    let bit = |mask: u8, n: u64| if flags & mask != 0 { n } else { 0 };
    let mut at = 7 + bit(0x08, 8) + bit(0x01, 4);
    loop {
        let mut size = [0u8; 4];
        file.seek(SeekFrom::Start(at))?;
        file.read_exact(&mut size).map_err(|_| truncated())?;
        at += 4;
        let size = u32::from_le_bytes(size) & 0x7fff_ffff;
        if size == 0 {
            break;
        }
        at += u64::from(size) + bit(0x10, 4);
    }
    if at + bit(0x04, 4) != len {
        return Err(truncated());
    }
    file.seek(SeekFrom::Start(0))?;
    Ok(())
}

/// The database lane, replayed into a file.
pub struct Replay {
    db: std::fs::File,
    live: bool,
    /// The next frame, by session and sequence.
    next: Option<(u32, Sequence)>,
    /// The furthest end of a batch skipped for starting past `next`, which
    /// a later batch must reach.
    skipped: Option<(u32, Sequence)>,
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
            next: None,
            skipped: None,
            log_bytes: 0,
        })
    }

    /// Applies `f`, the local copy at `path`. A batch that does not reach
    /// back to the next frame is skipped: a later, wider one must cover it.
    pub fn apply(&mut self, f: &File, path: &Path) -> Result<()> {
        let mut file = std::fs::File::open(path)?;
        check_whole(&mut file)?;
        let size = file.metadata()?.len();
        let mut r = BufReader::new(lz4_flex::frame::FrameDecoder::new(file));
        if f.snapshot {
            return self.snapshot(&mut r);
        }
        let end = (f.session, f.frame);
        while let Some(message) = capnp::serialize::try_read_message(&mut r, options())? {
            let f = message.get_root::<frame::Reader>()?;
            let (session, last) = (f.get_session(), f.get_sequence());
            let first = match f.which()? {
                frame::Pages(group) => group.get_first(),
                _ => last,
            };
            let next = self.next.unwrap_or((session, 0));
            if (session, last) < next {
                continue;
            }
            if (session, first) > next {
                self.skipped = self.skipped.max(Some(end));
                return Ok(());
            }
            // Pages a frame before already applied are rewritten with the
            // same or newer versions.
            self.next = Some((session, last + 1));
            self.frame(f)?;
        }
        self.log_bytes += size;
        Ok(())
    }

    fn snapshot(&mut self, r: &mut impl Read) -> Result<()> {
        let message = capnp::serialize::read_message(&mut *r, options())?;
        let header = message.get_root::<snapshot_header::Reader>()?;
        self.next = Some((header.get_session(), header.get_frame()));
        (self.live, self.log_bytes) = (false, 0);
        self.db.set_len(0)?;
        self.db.seek(SeekFrom::Start(0))?;
        let lanes = header.get_lanes()?;
        if lanes.len() > 1 {
            return Err(Error::Replica("a snapshot of more than one lane".into()));
        }
        for lane in lanes {
            if let snapshot_header::lane::Sqlite(sqlite) = lane.which()? {
                let size = sqlite.get_database_size();
                if std::io::copy(&mut r.take(size), &mut self.db)? != size {
                    return Err(Error::Replica(
                        "a snapshot shorter than its database".into(),
                    ));
                }
                self.live = true;
            }
        }
        // Read to the frame's end, where the decoder checks the checksum.
        if std::io::copy(r, &mut std::io::sink())? != 0 {
            return Err(Error::Replica("a snapshot longer than its lanes".into()));
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
            frame::Initialize(image) => {
                self.db.set_len(0)?;
                self.db.seek(SeekFrom::Start(0))?;
                self.db.write_all(image?)?;
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
            _ => return Err(Error::Replica("an unsupported frame".into())),
        }
        Ok(())
    }

    /// Fails if a skipped batch's frames were never replayed: a batch
    /// between is missing.
    fn check_gaps(&self) -> Result<()> {
        match self.skipped {
            Some(end) if self.next.is_none_or(|next| next < end) => Err(Error::Replica(format!(
                "the log is missing frames before {}",
                end.1
            ))),
            _ => Ok(()),
        }
    }

    /// The replayed database, whole, and the frame the log continues from.
    pub fn finish(self) -> Result<Sequence> {
        self.check_gaps()?;
        self.db.sync_data()?;
        Ok(self.next.map_or(0, |n| n.1))
    }

    /// Writes the replayed state as a snapshot into `dir`.
    pub fn write_snapshot(mut self, dir: &Path) -> Result<File> {
        self.check_gaps()?;
        let (session, frame) = self.next.unwrap_or_default();
        let size = self.db.seek(SeekFrom::End(0))?;
        self.db.seek(SeekFrom::Start(0))?;
        let header = Header {
            session,
            frame,
            justification: SnapshotJustification::ClientRequested,
            log_bytes: self.log_bytes,
        };
        write_snapshot(dir, &header, self.live.then_some(size), |w| {
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

/// Writes a snapshot into `dir`: the database lane, if it exists, of `size`
/// bytes that `payload` writes.
fn write_snapshot(
    dir: &Path,
    h: &Header,
    size: Option<u64>,
    payload: impl FnOnce(&mut dyn Write) -> Result<()>,
) -> Result<File> {
    let mut message = capnp::message::Builder::new_default();
    let mut header = message.init_root::<snapshot_header::Builder>();
    header.set_session(h.session);
    header.set_frame(h.frame);
    header.set_justification(h.justification);
    header.set_log_bytes_since_last_snapshot(h.log_bytes);
    let mut lanes = header.init_lanes(u32::from(size.is_some()));
    if let Some(size) = size {
        let mut lane = lanes.reborrow().get(0);
        lane.set_name(LANE);
        lane.init_sqlite().set_database_size(size);
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
    let file = write_snapshot(dir, &header, Some(size), |w| {
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
        self.list(&format!("/{}/", descending(session.into())))
            .await
    }

    /// The files of every session before `session`.
    pub async fn older(&self, session: u32) -> Result<Vec<File>> {
        let files = self.list("/").await?;
        Ok(files.into_iter().filter(|f| f.session < session).collect())
    }

    /// The files under `<kind><under>`, for both kinds.
    async fn list(&self, under: &str) -> Result<Vec<File>> {
        let mut files = Vec::new();
        for snapshot in [false, true] {
            let kind = if snapshot { "snapshot" } else { "log" };
            for entry in self.bucket.list(&format!("{kind}{under}")).await? {
                let mut parts = entry.key.rsplit('/');
                let number = |p: Option<&str>, asc| {
                    p.and_then(|p| parse(p, asc))
                        .and_then(|n| u32::try_from(n).ok())
                };
                let frame = number(parts.next(), !snapshot);
                let session = number(parts.next(), false);
                if let (Some(frame), Some(session)) = (frame, session) {
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
