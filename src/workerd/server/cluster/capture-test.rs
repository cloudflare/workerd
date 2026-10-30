// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A simulation of one object's databases, a root and facets, writing their
//! WALs as `SQLite` does: transactions, rolled-back ones, checkpoints,
//! facets created, deleted, reset and cloned. Batches upload as the owner's commits
//! and tiering race to, with stale starts and ends; compaction snapshots
//! what the bucket holds; each takeover restores and starts the next
//! session from a snapshot of the restored databases. Whatever a seed
//! does, a restore must be every lane exactly as of the last frame the
//! bucket holds contiguously.

use std::collections::BTreeMap;
use std::path::Path;
use std::path::PathBuf;

use super::Capture;
use crate::Bucket;
use crate::Sequence;
use crate::dst::Rng;
use crate::log::Cursor;
use crate::log::DB;
use crate::log::File;
use crate::log::Log;
use crate::log::facet_prefix;

const PAGE: usize = 512;
const NAMES: [&str; 4] = ["a", "b", "x/y", "%"];

/// One database: its file, and the committed frames of its WAL since the
/// last checkpoint.
struct Db {
    lane: String,
    path: PathBuf,
    file: Vec<u8>,
    wal: Vec<(u32, Vec<u8>)>,
    pages: u32,
    /// Where the next WAL write goes; 0 before the WAL header.
    offset: u64,
    /// Whether a commit has made its lane.
    committed: bool,
}

impl Db {
    /// A new database: `SQLite` writes its first page to the file.
    fn new(lane: &str, path: PathBuf, rng: &mut Rng) -> Self {
        let file = page(rng);
        std::fs::write(&path, &file).unwrap();
        Self {
            lane: lane.to_owned(),
            path,
            file,
            wal: Vec::new(),
            pages: 1,
            offset: 0,
            committed: false,
        }
    }

    fn name(&self) -> String {
        self.path.file_name().unwrap().to_str().unwrap().to_owned()
    }

    /// What a connection reads.
    fn state(&self) -> Vec<u8> {
        let mut db = self.file.clone();
        for (number, data) in &self.wal {
            let at = (*number as usize - 1) * PAGE;
            if db.len() < at + PAGE {
                db.resize(at + PAGE, 0);
            }
            db[at..at + PAGE].copy_from_slice(data);
        }
        db.resize(self.pages as usize * PAGE, 0);
        db
    }
}

fn page(rng: &mut Rng) -> Vec<u8> {
    vec![rng.next() as u8; PAGE]
}

struct Sim {
    rng: Rng,
    dir: PathBuf,
    log: Log,
    capture: Capture,
    dbs: Vec<Db>,
    files: u32,
    session: u32,
    /// The bucket's files of this session, and how far they are contiguous.
    uploaded: Vec<File>,
    tiered: Sequence,
    /// The ends uploads may start or stop at: earlier tiered positions and
    /// commits.
    starts: Vec<Sequence>,
    ends: Vec<Sequence>,
    /// Every lane's state by the frame after each commit.
    states: BTreeMap<Sequence, BTreeMap<String, Vec<u8>>>,
    trace: Vec<String>,
}

fn block<T>(f: impl Future<Output = T>) -> T {
    tokio::runtime::Builder::new_current_thread()
        .build()
        .unwrap()
        .block_on(f)
}

impl Sim {
    fn new(seed: u64) -> Self {
        let root = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
        let dir = root.join(format!("lanes-{seed}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let mut rng = Rng::new(seed);
        let root = Db::new(DB, dir.join("o.sqlite"), &mut rng);
        let capture = Capture::resume(Cursor::default(), &[(DB.into(), root.path.clone())]);
        let mut sim = Self {
            rng,
            log: Log::new(Bucket::memory("").at("o")),
            dir,
            capture,
            dbs: vec![root],
            files: 0,
            session: 0,
            uploaded: Vec::new(),
            tiered: 0,
            starts: vec![0],
            ends: Vec::new(),
            states: BTreeMap::new(),
            trace: Vec::new(),
        };
        sim.record();
        sim
    }

    fn last(&self) -> Sequence {
        self.capture.last().unwrap()
    }

    /// The state as of the capture's last frame, where an upload may end.
    fn record(&mut self) {
        let last = self.last();
        if last > 0 && self.ends.last() != Some(&last) {
            self.ends.push(last);
        }
        let lanes = self
            .dbs
            .iter()
            .filter(|d| d.committed)
            .map(|d| (d.lane.clone(), d.state()))
            .collect();
        self.states.insert(last, lanes);
    }

    fn write(&mut self, i: usize, data: &[u8]) {
        let (name, offset) = (self.dbs[i].name(), self.dbs[i].offset);
        self.capture.write(&name, offset, data);
        self.dbs[i].offset += data.len() as u64;
    }

    /// Frames of `pages` at the WAL's end, the last carrying `commit`.
    fn frames(&mut self, i: usize, pages: &[(u32, Vec<u8>)], commit: u32) {
        if self.dbs[i].offset == 0 {
            let mut header = vec![0; 32];
            header[8..12].copy_from_slice(&(PAGE as u32).to_be_bytes());
            self.write(i, &header);
        }
        for (k, (number, data)) in pages.iter().enumerate() {
            let mut header = vec![0; 24];
            header[0..4].copy_from_slice(&number.to_be_bytes());
            let size = if k + 1 == pages.len() { commit } else { 0 };
            header[4..8].copy_from_slice(&size.to_be_bytes());
            if self.rng.chance(50) {
                self.write(i, &header);
                self.write(i, data);
            } else {
                self.write(i, &[header, data.clone()].concat());
            }
        }
    }

    /// A database without a lane is captured once its facet starts.
    fn start(&mut self, i: usize) {
        if !self.dbs[i].committed {
            self.capture.open_lane(&self.dbs[i].path, &self.dbs[i].lane);
        }
    }

    fn commit(&mut self, i: usize) {
        self.start(i);
        let old = self.dbs[i].pages;
        let pages = match self.rng.below(10) {
            0 => old.saturating_sub(1).max(1),
            1..5 => old + 1,
            _ => old,
        };
        // As `SQLite` does, a transaction growing the database writes the
        // page it adds.
        let mut written = Vec::new();
        if pages > old {
            written.push((pages, page(&mut self.rng)));
        }
        for _ in 0..=self.rng.below(3) {
            written.push((
                1 + self.rng.below(pages as usize) as u32,
                page(&mut self.rng),
            ));
        }
        if self.rng.chance(10) {
            // A transaction that spills frames and rolls back: the next one
            // overwrites them.
            let at = self.dbs[i].offset;
            let doomed: Vec<_> = written
                .iter()
                .map(|(n, _)| (*n, page(&mut self.rng)))
                .collect();
            self.frames(i, &doomed, 0);
            self.dbs[i].offset = at.max(32);
        }
        self.frames(i, &written, pages);
        let db = &mut self.dbs[i];
        db.wal.extend(written);
        // Pages past the end are gone, never read again.
        db.wal.retain(|(n, _)| *n <= pages);
        db.file.truncate(pages as usize * PAGE);
        db.pages = pages;
        db.committed = true;
        self.trace.push(format!("commit {} {pages}", db.lane));
        self.record();
    }

    fn checkpoint(&mut self, i: usize) {
        let db = &mut self.dbs[i];
        db.file = db.state();
        db.wal.clear();
        db.offset = 0;
        std::fs::write(&db.path, &db.file).unwrap();
        self.trace.push(format!("checkpoint {}", db.lane));
    }

    fn new_facet(&mut self) {
        let parent = self.rng.below(self.dbs.len());
        let prefix = self.dbs[parent].lane.strip_suffix(DB).unwrap().to_owned();
        let name = NAMES[self.rng.below(NAMES.len())];
        let lane = prefix + &facet_prefix([name]) + DB;
        if prefix_depth(&lane) > 3 || self.dbs.iter().any(|d| d.lane == lane) {
            return;
        }
        self.files += 1;
        let path = self.dir.join(format!("o.{}.sqlite", self.files));
        self.capture.open_lane(&path, &lane);
        self.trace.push(format!("open {lane}"));
        self.dbs.push(Db::new(&lane, path, &mut self.rng));
    }

    /// Facet `i` deleted with its descendants.
    fn delete(&mut self, i: usize) {
        let prefix = self.dbs[i].lane.strip_suffix(DB).unwrap().to_owned();
        if prefix.is_empty() {
            return;
        }
        self.dbs.retain(|d| {
            let doomed = d.lane.starts_with(&prefix);
            if doomed {
                remove(&d.path);
            }
            !doomed
        });
        self.capture.delete_lanes(&prefix);
        self.trace.push(format!("delete {prefix}"));
        self.record();
    }

    /// Database `i`'s `deleteAll()`: it starts over, its descendants go.
    fn reset(&mut self, i: usize) {
        self.start(i);
        let lane = self.dbs[i].lane.clone();
        let prefix = lane.strip_suffix(DB).unwrap().to_owned();
        let name = self.dbs[i].name();
        let path = self.dbs[i].path.clone();
        self.dbs.retain(|d| {
            let doomed = d.lane.starts_with(&prefix);
            if doomed {
                remove(&d.path);
            }
            !doomed
        });
        self.capture.reset(&name);
        self.dbs.push(Db::new(&lane, path, &mut self.rng));
        self.trace.push(format!("reset {lane}"));
        self.record();
    }

    /// Facet `i` cloned over a sibling, with its descendants, each copy's
    /// WAL folded into its file.
    fn clone_facet(&mut self, i: usize) {
        let src = self.dbs[i].lane.strip_suffix(DB).unwrap().to_owned();
        let Some(parent) = src.strip_suffix('/') else {
            return;
        };
        let parent = parent
            .rsplit_once('/')
            .map_or(String::new(), |(p, _)| p.to_owned() + "/");
        let dst = parent + &facet_prefix([NAMES[self.rng.below(NAMES.len())]]);
        if dst == src {
            return;
        }
        self.dbs.retain(|d| {
            let doomed = d.lane.starts_with(&dst);
            if doomed {
                remove(&d.path);
            }
            !doomed
        });
        let copies: Vec<_> = (self.dbs.iter())
            .filter_map(|d| Some((d.lane.strip_prefix(&src)?, d)))
            .map(|(rest, d)| (dst.clone() + rest, d.state(), d.pages, d.committed))
            .collect();
        let opened = self.capture.clone_lanes(&src, &dst);
        for (lane, file, pages, committed) in copies {
            self.files += 1;
            let path = self.dir.join(format!("o.{}.sqlite", self.files));
            std::fs::write(&path, &file).unwrap();
            if opened.contains(&lane) {
                self.capture.open_lane(&path, &lane);
            }
            let wal = Vec::new();
            self.dbs.push(Db {
                lane,
                path,
                file,
                wal,
                pages,
                offset: 0,
                committed,
            });
        }
        self.trace.push(format!("clone {src} {dst}"));
        self.record();
    }

    /// What the capture holds from an earlier start to a commit uploads, as
    /// a commit and tiering racing leave it.
    fn upload(&mut self) {
        let from = self.starts[self.rng.below(self.starts.len())];
        let ends: Vec<_> = self.ends.iter().copied().filter(|&e| e > from).collect();
        if ends.is_empty() {
            return;
        }
        let to = ends[self.rng.below(ends.len())];
        let batch = crate::log::merge(&self.capture.batch(from, to).unwrap()).unwrap();
        let file = File {
            snapshot: false,
            session: self.session,
            frame: to,
            size: 0,
        };
        let path = self.dir.join("batch");
        std::fs::write(&path, crate::log::compress(&batch).unwrap()).unwrap();
        block(self.log.put(&file, &path)).unwrap();
        self.uploaded.push(file);
        self.trace.push(format!("upload {from}..{to}"));
        if from <= self.tiered && to > self.tiered {
            self.tiered = to;
            self.starts.push(to);
        }
    }

    /// Snapshot the bucket's session and delete what it covers.
    fn compact(&mut self) {
        let plan = crate::log::plan(&self.uploaded);
        if plan.len() < 2 {
            return;
        }
        let dir = self.dir.join("compact");
        std::fs::create_dir_all(&dir).unwrap();
        let mut files = Vec::new();
        for (i, f) in plan.iter().enumerate() {
            let path = dir.join(i.to_string());
            block(self.log.get(f, &path)).unwrap();
            files.push((*f, path));
        }
        let snapshot = crate::log::compact(&files, &dir).unwrap();
        block(self.log.put(&snapshot, &dir.join(snapshot.name()))).unwrap();
        for f in &self.uploaded {
            if f.frame <= snapshot.frame && !f.snapshot {
                block(self.log.delete(f)).unwrap();
            }
        }
        self.uploaded
            .retain(|f| f.frame > snapshot.frame || f.snapshot);
        self.uploaded.push(snapshot);
        self.trace.push(format!("compact to {}", snapshot.frame));
    }

    /// A takeover: restore, check, and open the next session from a
    /// snapshot of what was restored.
    fn take_over(&mut self) -> Result<(), String> {
        // A dormant takeover snapshots the captured databases: every live
        // lane must be one, started or not.
        let captured = self.capture.databases();
        if let Some(d) =
            (self.dbs.iter()).find(|d| d.committed && !captured.iter().any(|(l, _)| *l == d.lane))
        {
            return Err(format!("{} is live but not captured", d.lane));
        }
        let spool = self.dir.join(format!("restore-{}", self.session));
        let (end, lanes) = block(crate::log::restore(&self.log, self.session, &spool))
            .map_err(|e| format!("restore: {e}"))?;
        let got: BTreeMap<_, _> = lanes
            .iter()
            .map(|(lane, path)| (lane.clone(), std::fs::read(path).unwrap()))
            .collect();
        let want = &self.states[&self.tiered];
        if end != self.tiered || got != *want {
            let lanes = |m: &BTreeMap<String, Vec<u8>>| {
                let other = if std::ptr::eq(m, want) { &got } else { want };
                let diff = |l: &String, d: &Vec<u8>| {
                    let o = other.get(l)?;
                    let page = (0..d.len().max(o.len()) / PAGE)
                        .find(|p| d.get(p * PAGE) != o.get(p * PAGE))?;
                    Some(format!(" page {} {:?}", page + 1, d.get(page * PAGE)))
                };
                let show = |(l, d): (&String, &Vec<u8>)| {
                    format!("{l}:{}{}", d.len(), diff(l, d).unwrap_or_default())
                };
                m.iter().map(show).collect::<Vec<_>>()
            };
            return Err(format!(
                "session {} restored to {end} {:?}, want {} {:?}",
                self.session,
                lanes(&got),
                self.tiered,
                lanes(want)
            ));
        }
        for db in &self.dbs {
            remove(&db.path);
        }
        self.dbs.clear();
        for (lane, from) in lanes {
            self.files += 1;
            let path = self.dir.join(format!("o.{}.sqlite", self.files));
            std::fs::rename(&from, &path).unwrap();
            let file = std::fs::read(&path).unwrap();
            let pages = (file.len() / PAGE) as u32;
            self.dbs.push(Db {
                lane,
                path,
                file,
                wal: Vec::new(),
                pages,
                offset: 0,
                committed: true,
            });
        }
        if !self.dbs.iter().any(|d| d.lane == DB) {
            let root = Db::new(DB, self.dir.join("o.sqlite"), &mut self.rng);
            self.dbs.push(root);
        }
        let databases: Vec<_> = self
            .dbs
            .iter()
            .map(|d| (d.lane.clone(), d.path.clone()))
            .collect();
        self.session += 1;
        let (snapshot, lanes) =
            crate::log::snapshot_file(&databases, &self.dir, self.session).unwrap();
        // A database that never committed has a file but no lane yet.
        for db in &mut self.dbs {
            db.committed = lanes.iter().any(|l| l.as_deref() == Some(&db.lane));
        }
        self.uploaded.clear();
        if let Some(snapshot) = snapshot {
            block(self.log.put(&snapshot, &self.dir.join(snapshot.name()))).unwrap();
            self.uploaded.push(snapshot);
        }
        let cursor = Cursor {
            session: self.session,
            lanes,
            ..Cursor::default()
        };
        self.capture = Capture::resume(cursor, &databases);
        (self.tiered, self.starts, self.ends) = (0, vec![0], Vec::new());
        self.states.clear();
        self.record();
        self.trace
            .push(format!("take over at session {}", self.session));
        Ok(())
    }

    fn step(&mut self) {
        let i = self.rng.below(self.dbs.len());
        match self.rng.below(100) {
            0..45 => self.commit(i),
            45..55 => self.checkpoint(i),
            55..63 => self.new_facet(),
            63..66 => self.delete(i),
            66..68 => self.reset(i),
            68..71 => self.clone_facet(i),
            71..92 => self.upload(),
            _ => self.compact(),
        }
    }
}

fn prefix_depth(lane: &str) -> usize {
    lane.matches('/').count()
}

fn remove(path: &Path) {
    for suffix in ["", "-wal", "-shm"] {
        let _ = std::fs::remove_file(crate::log::with_suffix(path, suffix));
    }
}

fn run(seed: u64, steps: usize) -> Result<(), String> {
    let mut sim = Sim::new(seed);
    for epoch in 0..4 {
        for _ in 0..steps {
            sim.step();
        }
        if let Err(e) = sim.take_over() {
            let tail = sim.trace.len().saturating_sub(40);
            return Err(format!(
                "epoch {epoch}: {e}\n{}",
                sim.trace[tail..].join("\n")
            ));
        }
    }
    let _ = std::fs::remove_dir_all(&sim.dir);
    Ok(())
}

#[test]
fn every_lane_restores_as_the_bucket_holds_it() {
    crate::dst::seeds(30, |seed| run(seed, 150));
}
