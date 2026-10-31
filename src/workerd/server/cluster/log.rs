// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An object's log in the bucket (`frames.capnp`): a `sqlitePages` lane per
//! database, the root's and each facet's, whose frames are the pages each
//! commit wrote. Every lane shares
//! the object's sessions and sequence, so a restore is a prefix of the
//! commits of all its databases.
//!
//! A lane is named by its facet's path from the root, each name
//! URI-encoded and followed by `/`, then `db`: the root's lane is `db`,
//! its facet `a`'s `a/db`, and `a`'s facet `b`'s `a/b/db`. A facet's
//! subtree is the lanes under its prefix.
//!
//! Under `do/<namespace>/<actor>/`, each file gzipped:
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

/// A database's lane, after its facet's prefix.
pub const DB: &str = "db";
/// Downloads in flight during a restore, to hide the store's round trips.
const RESTORE_CONCURRENCY: usize = 16;

/// Where a session's next frame goes, and its lanes.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Cursor {
    pub session: u32,
    pub sequence: Sequence,
    /// Each lane's name by number, `None` for a number freed.
    pub lanes: Vec<Option<String>>,
}

impl Cursor {
    pub fn lane(&self, name: &str) -> Option<u32> {
        let n = self.lanes.iter().position(|l| l.as_deref() == Some(name))?;
        Some(n as u32)
    }

    /// The lowest free number.
    fn free(&self) -> u32 {
        self.lanes
            .iter()
            .position(Option::is_none)
            .unwrap_or(self.lanes.len()) as u32
    }
}

/// The prefix of the lanes of a facet with `path` from the root: empty for
/// the root.
pub fn facet_prefix<'a>(path: impl IntoIterator<Item = &'a str>) -> String {
    let mut prefix = String::new();
    for name in path {
        for b in name.bytes() {
            if b.is_ascii_alphanumeric() || b"-_.~".contains(&b) {
                prefix.push(char::from(b));
            } else {
                prefix += &format!("%{b:02X}");
            }
        }
        prefix.push('/');
    }
    prefix
}

/// The facet path of database lane `lane`: its names from the root.
pub fn facet_path(lane: &str) -> Option<Vec<String>> {
    let prefix = lane.strip_suffix(DB)?;
    let mut path = Vec::new();
    for part in prefix.split_terminator('/') {
        let mut name = Vec::new();
        let mut bytes = part.bytes();
        while let Some(b) = bytes.next() {
            if b == b'%' {
                let hex = [bytes.next()?, bytes.next()?];
                name.push(u8::from_str_radix(std::str::from_utf8(&hex).ok()?, 16).ok()?);
            } else {
                name.push(b);
            }
        }
        path.push(String::from_utf8(name).ok()?);
    }
    (prefix.is_empty() || prefix.ends_with('/')).then_some(path)
}

/// Pages by number.
pub type Pages<'a> = BTreeMap<u32, &'a [u8]>;

#[derive(Clone, Copy)]
pub enum Body<'a> {
    /// The lane named, at the lowest free number.
    NewLane(&'a str),
    /// The lane named, at the lowest free number, a copy of lane `from`.
    CloneLane(&'a str, u32),
    DeleteLane,
    Initialize(&'a [u8]),
    /// A commit's pages, and the database's size after, in pages.
    Pages(u32, &'a Pages<'a>),
}

/// Appends a frame on `lane` to `out` at `cursor`, and moves it past; the
/// lane of a new lane is its number. Returns the lane.
pub fn push(out: &mut Vec<u8>, cursor: &mut Cursor, lane: u32, body: Body) -> u32 {
    let lane = match body {
        Body::NewLane(name) | Body::CloneLane(name, _) => {
            let n = cursor.free();
            if n as usize == cursor.lanes.len() {
                cursor.lanes.push(None);
            }
            cursor.lanes[n as usize] = Some(name.to_owned());
            n
        }
        Body::DeleteLane => {
            cursor.lanes[lane as usize] = None;
            while cursor.lanes.last().is_some_and(Option::is_none) {
                cursor.lanes.pop();
            }
            lane
        }
        _ => lane,
    };
    let (session, sequence) = (cursor.session, cursor.sequence);
    frame_at(out, session, sequence..=sequence, lane, body);
    cursor.sequence += 1;
    lane
}

/// Appends a frame on `lane` covering `sequences` to `out`.
fn frame_at(
    out: &mut Vec<u8>,
    session: u32,
    sequences: std::ops::RangeInclusive<Sequence>,
    lane: u32,
    body: Body,
) {
    let mut message = capnp::message::Builder::new_default();
    let mut f = message.init_root::<frame::Builder>();
    f.set_session(session);
    f.set_sequence(*sequences.end());
    f.set_lane(lane);
    match body {
        Body::NewLane(name) => {
            let mut lane = f.init_new_lane();
            lane.set_name(name);
            lane.set_sqlite_pages(());
        }
        Body::CloneLane(name, from) => {
            let mut lane = f.init_clone_lane();
            lane.set_name(name);
            lane.set_from(from);
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

/// A run of page frames, merged: its session and sequences, and each
/// lane's size and newest pages, by first write.
struct Run<'a> {
    session: u32,
    first: Sequence,
    last: Sequence,
    lanes: Vec<(u32, u32, Pages<'a>)>,
}

/// `batch`, a run of frames, with each run of page frames merged to one
/// per lane with the newest version of each page. The lanes are separate
/// files and no restore stops inside a batch, so reordering them is
/// invisible; each merged frame carries the run's sequences.
pub fn merge(batch: &[u8]) -> Result<Vec<u8>> {
    let mut messages = Vec::new();
    let mut r = batch;
    while let Some(message) = capnp::serialize::try_read_message(&mut r, options())? {
        messages.push(message);
    }
    let mut out = Vec::new();
    let mut run: Option<Run> = None;
    let flush = |out: &mut Vec<u8>, run: &mut Option<Run>| {
        let Some(run) = run.take() else {
            return;
        };
        for (lane, commit, mut pages) in run.lanes {
            pages.retain(|&number, _| number <= commit);
            let body = Body::Pages(commit, &pages);
            frame_at(out, run.session, run.first..=run.last, lane, body);
        }
    };
    for message in &messages {
        let f = message.get_root::<frame::Reader>()?;
        let (session, sequence, lane) = (f.get_session(), f.get_sequence(), f.get_lane());
        if let frame::Pages(group) = f.which()? {
            let r = run.get_or_insert_with(|| Run {
                session,
                first: group.get_first(),
                last: sequence,
                lanes: Vec::new(),
            });
            r.last = sequence;
            let i = r.lanes.iter().position(|l| l.0 == lane).unwrap_or_else(|| {
                r.lanes.push((lane, 0, BTreeMap::new()));
                r.lanes.len() - 1
            });
            let l = &mut r.lanes[i];
            l.1 = group.get_commit();
            for page in group.get_list()? {
                l.2.insert(page.get_number(), page.get_data()?);
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

/// Gzips `bytes`, as every file in the bucket is.
pub fn compress(bytes: &[u8]) -> Result<Vec<u8>> {
    let mut encoder = encoder(Vec::new());
    encoder.write_all(bytes)?;
    Ok(encoder.finish()?)
}

/// Gzip ends with the content's CRC-32 and length, which the decoder
/// checks: a corrupt or truncated copy fails to replay rather than
/// replaying wrong or short.
fn encoder<W: Write>(w: W) -> flate2::write::GzEncoder<W> {
    flate2::write::GzEncoder::new(w, flate2::Compression::fast())
}

/// Bounds a message, so that a corrupt one fails rather than allocating
/// whatever its segment table claims: 1 GiB, far more than a batch holds.
fn options() -> capnp::message::ReaderOptions {
    *capnp::message::ReaderOptions::new().traversal_limit_in_words(Some(1 << 27))
}

/// A lane replayed into a file.
struct Lane {
    name: String,
    path: PathBuf,
    file: std::fs::File,
}

/// The lanes, replayed into files in a directory.
pub struct Replay {
    dir: PathBuf,
    lanes: Vec<Option<Lane>>,
    /// The next frame, by session and sequence.
    next: Option<(u32, Sequence)>,
    /// The furthest end of a batch skipped for starting past `next`, which
    /// a later batch must reach.
    skipped: Option<(u32, Sequence)>,
    log_bytes: u64,
}

impl Replay {
    /// Replays into `dir`, created empty.
    pub fn new(dir: &Path) -> Result<Self> {
        let _ = std::fs::remove_dir_all(dir);
        std::fs::create_dir_all(dir)?;
        Ok(Self {
            dir: dir.to_owned(),
            lanes: Vec::new(),
            next: None,
            skipped: None,
            log_bytes: 0,
        })
    }

    /// Lane `n`'s file, created empty, named `name`.
    fn create(&mut self, n: usize, name: &str) -> Result<&mut std::fs::File> {
        let path = self.dir.join(n.to_string());
        let file = std::fs::File::options()
            .read(true)
            .write(true)
            .create(true)
            .truncate(true)
            .open(&path)?;
        if self.lanes.len() <= n {
            self.lanes.resize_with(n + 1, || None);
        }
        let name = name.to_owned();
        let lane = self.lanes[n].insert(Lane { name, path, file });
        Ok(&mut lane.file)
    }

    fn lane(&mut self, n: u32) -> Result<&mut std::fs::File> {
        match self.lanes.get_mut(n as usize) {
            Some(Some(lane)) => Ok(&mut lane.file),
            _ => Err(Error::Replica(format!(
                "a frame on lane {n}, which is not live"
            ))),
        }
    }

    /// Applies `f`, the local copy at `path`. A batch that does not reach
    /// back to the next frame is skipped: a later, wider one must cover it.
    pub fn apply(&mut self, f: &File, path: &Path) -> Result<()> {
        let file = std::fs::File::open(path)?;
        let size = file.metadata()?.len();
        let mut r = BufReader::new(flate2::read::GzDecoder::new(file));
        if f.snapshot {
            return self.snapshot(&mut r);
        }
        let end = (f.session, f.frame);
        // The last frame's session and sequences, and whether it applied: a
        // merged run's frames, one per lane, share them.
        let mut run: Option<((u32, Sequence, Sequence), bool)> = None;
        while let Some(message) = capnp::serialize::try_read_message(&mut r, options())? {
            let f = message.get_root::<frame::Reader>()?;
            let (session, last) = (f.get_session(), f.get_sequence());
            let first = match f.which()? {
                frame::Pages(group) => group.get_first(),
                _ => last,
            };
            let key = (session, first, last);
            let applies = match run {
                Some((k, applies)) if k == key => applies,
                _ => {
                    let next = self.next.unwrap_or((session, 0));
                    if (session, last) < next {
                        false
                    } else if (session, first) > next {
                        self.skipped = self.skipped.max(Some(end));
                        return Ok(());
                    } else {
                        // Pages a frame before already applied are
                        // rewritten with the same or newer versions.
                        self.next = Some((session, last + 1));
                        true
                    }
                }
            };
            run = Some((key, applies));
            if applies {
                self.frame(f)?;
            }
        }
        self.log_bytes += size;
        Ok(())
    }

    fn snapshot(&mut self, r: &mut impl Read) -> Result<()> {
        let message = capnp::serialize::read_message(&mut *r, options())?;
        let header = message.get_root::<snapshot_header::Reader>()?;
        self.next = Some((header.get_session(), header.get_frame()));
        self.log_bytes = 0;
        self.lanes.clear();
        for (n, lane) in header.get_lanes()?.iter().enumerate() {
            match lane.which()? {
                snapshot_header::lane::Deleted(()) => {}
                snapshot_header::lane::Sqlite(sqlite) => {
                    let size = sqlite.get_database_size();
                    let name = text(lane.get_name()?)?;
                    let file = self.create(n, name)?;
                    if std::io::copy(&mut r.take(size), file)? != size {
                        return Err(Error::Replica(
                            "a snapshot shorter than its database".into(),
                        ));
                    }
                }
                _ => return Err(Error::Replica("an unsupported lane".into())),
            }
        }
        // Read to the end, where the decoder checks the checksum.
        if std::io::copy(r, &mut std::io::sink())? != 0 {
            return Err(Error::Replica("a snapshot longer than its lanes".into()));
        }
        Ok(())
    }

    fn frame(&mut self, f: frame::Reader) -> Result<()> {
        let n = f.get_lane();
        match f.which()? {
            frame::NewLane(lane) => {
                let name = text(lane?.get_name()?)?;
                self.create(n as usize, name)?;
            }
            frame::CloneLane(lane) => {
                self.lane(lane.get_from())?;
                let from = self.dir.join(lane.get_from().to_string());
                // Through `dyn Read`: a copy between files takes
                // `copy_file_range`, which the server's link lacks.
                let from: &mut dyn std::io::Read = &mut std::fs::File::open(from)?;
                let name = text(lane.get_name()?)?;
                std::io::copy(from, self.create(n as usize, name)?)?;
            }
            frame::DeleteLane(()) => {
                self.lane(n)?;
                if let Some(lane) = self.lanes[n as usize].take() {
                    drop(lane.file);
                    let _ = std::fs::remove_file(lane.path);
                }
            }
            frame::Initialize(image) => {
                let db = self.lane(n)?;
                db.set_len(0)?;
                db.seek(SeekFrom::Start(0))?;
                db.write_all(image?)?;
            }
            frame::Pages(group) => {
                let db = self.lane(n)?;
                let mut page_size = 0;
                for page in group.get_list()? {
                    let data = page.get_data()?;
                    page_size = data.len() as u64;
                    let at = u64::from(page.get_number() - 1) * page_size;
                    db.seek(SeekFrom::Start(at))?;
                    db.write_all(data)?;
                }
                if page_size > 0 {
                    db.set_len(u64::from(group.get_commit()) * page_size)?;
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

    /// The frame the log continues from, and each live lane's name and
    /// database, whole.
    pub fn finish(self) -> Result<(Sequence, Vec<(String, PathBuf)>)> {
        self.check_gaps()?;
        let mut lanes = Vec::new();
        for lane in self.lanes.into_iter().flatten() {
            lane.file.sync_data()?;
            lanes.push((lane.name, lane.path));
        }
        Ok((self.next.map_or(0, |n| n.1), lanes))
    }

    /// Writes the replayed state as a snapshot into `dir`.
    pub fn write_snapshot(mut self, dir: &Path) -> Result<File> {
        self.check_gaps()?;
        let (session, frame) = self.next.unwrap_or_default();
        let header = Header {
            session,
            frame,
            justification: SnapshotJustification::ClientRequested,
            log_bytes: self.log_bytes,
        };
        let mut lanes: Vec<_> = self
            .lanes
            .iter_mut()
            .map(|l| l.as_mut().map(|l| (l.name.clone(), &mut l.file)))
            .collect();
        write_snapshot(dir, &header, &mut lanes)
    }
}

fn text(t: capnp::text::Reader<'_>) -> Result<&str> {
    Ok(t.to_str().map_err(capnp::Error::from)?)
}

struct Header {
    session: u32,
    frame: Sequence,
    justification: SnapshotJustification,
    log_bytes: u64,
}

/// Writes a snapshot into `dir` of `lanes`, by number, each a name and its
/// database.
fn write_snapshot(
    dir: &Path,
    h: &Header,
    lanes: &mut [Option<(String, &mut std::fs::File)>],
) -> Result<File> {
    let mut message = capnp::message::Builder::new_default();
    let mut header = message.init_root::<snapshot_header::Builder>();
    header.set_session(h.session);
    header.set_frame(h.frame);
    header.set_justification(h.justification);
    header.set_log_bytes_since_last_snapshot(h.log_bytes);
    let mut list = header.init_lanes(lanes.len() as u32);
    for (n, lane) in lanes.iter_mut().enumerate() {
        let mut entry = list.reborrow().get(n as u32);
        match lane {
            Some((name, db)) => {
                entry.set_name(name.as_str());
                let size = db.seek(SeekFrom::End(0))?;
                entry.init_sqlite().set_database_size(size);
            }
            None => entry.set_deleted(()),
        }
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
    for (_, db) in lanes.iter_mut().flatten() {
        db.seek(SeekFrom::Start(0))?;
        std::io::copy(db, &mut encoder)?;
    }
    let mut w = encoder.finish()?;
    w.flush()?;
    drop(w);
    std::fs::rename(&tmp, &out)?;
    let size = std::fs::metadata(&out)?.len();
    Ok(File { size, ..file })
}

/// Writes `databases`, lane names and files no connection has open, as the
/// snapshot `session` starts from into `dir`, numbering the lanes from 0.
/// Returns it, `None` if every database is empty or missing, and the lanes
/// it holds. Closing the last connection checkpointed each WAL into its
/// file.
pub fn snapshot_file(
    databases: &[(String, PathBuf)],
    dir: &Path,
    session: u32,
) -> Result<(Option<File>, Vec<Option<String>>)> {
    let mut opened = Vec::new();
    for (name, path) in databases {
        let db = match std::fs::File::open(path) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
            db => db?,
        };
        if db.metadata()?.len() >= 100 {
            opened.push((name.clone(), db));
        }
    }
    if opened.is_empty() {
        return Ok((None, Vec::new()));
    }
    let header = Header {
        session,
        frame: 0,
        justification: SnapshotJustification::ColdStartMitigation,
        log_bytes: 0,
    };
    let mut lanes: Vec<_> = opened
        .iter_mut()
        .map(|(n, db)| Some((n.clone(), db)))
        .collect();
    let file = write_snapshot(dir, &header, &mut lanes)?;
    Ok((
        Some(file),
        opened.into_iter().map(|(n, _)| Some(n)).collect(),
    ))
}

/// Replays `files`, a plan with local copies, into a snapshot in `dir`.
pub fn compact(files: &[(File, PathBuf)], dir: &Path) -> Result<File> {
    let lanes = dir.join("compact");
    let mut replay = Replay::new(&lanes)?;
    for (f, path) in files {
        replay.apply(f, path)?;
    }
    let snapshot = replay.write_snapshot(dir);
    let _ = std::fs::remove_dir_all(&lanes);
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
pub fn ascending(v: u64) -> String {
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

pub fn parse(name: &str, ascending: bool) -> Option<u64> {
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

/// Restores `session` of the log into `spool`, created empty: the plan's
/// files downloaded, then replayed on the blocking pool. The frame the log
/// continues from, 0 for an empty log, and each live lane's name and
/// database, under `spool`.
pub async fn restore(
    log: &Log,
    session: u32,
    spool: &Path,
) -> Result<(Sequence, Vec<(String, PathBuf)>)> {
    let plan = plan(&log.files(session).await?);
    if plan.is_empty() {
        return Ok((0, Vec::new()));
    }
    let downloads = spool.join("files");
    std::fs::create_dir_all(&downloads)?;
    let files: Vec<_> = (0..plan.len())
        .map(|i| (plan[i], downloads.join(i.to_string())))
        .collect();
    stream::iter(&files)
        .map(|(f, path)| log.get(f, path))
        .buffer_unordered(RESTORE_CONCURRENCY)
        .try_collect::<()>()
        .await?;
    let lanes = spool.join("lanes");
    let restored = crate::owner::blocking(move || {
        let mut replay = Replay::new(&lanes)?;
        for (f, path) in &files {
            replay.apply(f, path)?;
        }
        replay.finish()
    })
    .await;
    let _ = std::fs::remove_dir_all(&downloads);
    restored
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
