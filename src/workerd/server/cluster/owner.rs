// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The owner's side of an object: taking it over, the commit path, and
//! keeping its log in the bucket compact.
//!
//! A commit is acknowledged once the bucket holds it and own.json still
//! names the epoch after the upload. Each epoch writes its own session of
//! the object's log, starting with a snapshot of the state it inherited; a
//! takeover restores the last opened epoch's session.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::path::Path;
use std::path::PathBuf;
use std::rc::Rc;

use futures::StreamExt;
use futures::stream;

use crate::Bucket;
use crate::Epoch;
use crate::Error;
use crate::Ownership;
use crate::Result;
use crate::Sequence;
use crate::capture::Capture;
use crate::log::Cursor;
use crate::log::File;
use crate::log::Log;
use crate::log::plan;
use crate::ownership::Owned;

/// Objects compacting at once, and deletes in flight for one compaction.
const TIER_CONCURRENCY: usize = 64;
/// Evictions a node uploads at once.
const EVICT_CONCURRENCY: usize = 32;
/// Evicted objects whose database file this node remembers as current.
const DORMANT_MAX: usize = 1 << 16;

/// An owned object, its capture, and how far its commits are acknowledged.
struct Slot {
    object: tokio::sync::Mutex<Object>,
    /// Outside the lock: `SQLite` reports WAL writes synchronously, and may
    /// while an upload holds the lock.
    capture: RefCell<Capture>,
    /// Up to where commits are in the bucket, by epoch, then frame.
    acked: tokio::sync::watch::Sender<(Epoch, Sequence)>,
    /// Whether a commit is uploading on behalf of the others.
    pushing: Cell<bool>,
}

impl Slot {
    fn opened(object: Object, capture: Capture) -> Rc<Self> {
        let acked = (object.epoch, object.tiered);
        Rc::new(Self {
            object: tokio::sync::Mutex::new(object),
            capture: RefCell::new(capture),
            acked: tokio::sync::watch::Sender::new(acked),
            pushing: Cell::new(false),
        })
    }

    /// The bucket holds everything up to `through`.
    fn tiered(&self, object: &mut Object, through: Sequence, file: Option<File>) {
        object.tiered = object.tiered.max(through);
        object.files.extend(file);
        self.capture.borrow_mut().forget(through);
    }

    fn ack(&self, epoch: Epoch, through: Sequence) {
        self.acked.send_if_modified(|acked| {
            let more = (epoch, through) > *acked;
            *acked = (*acked).max((epoch, through));
            more
        });
    }
}

/// The uploading commit's turn. Ending it, even by cancellation, wakes the
/// waiting commits so one of them takes the next.
struct Pushing<'a>(&'a Slot);

impl Drop for Pushing<'_> {
    fn drop(&mut self) {
        self.0.pushing.set(false);
        self.0.acked.send_modify(|_| {});
    }
}

/// An object this node owns.
#[derive(Clone)]
struct Object {
    ns: String,
    actor: String,
    epoch: Epoch,
    replica: Replica,
    /// How far the epoch's session in the bucket is contiguous.
    tiered: Sequence,
    /// Where the capture was at eviction.
    cursor: Cursor,
    /// The files of this epoch's session. Only this owner writes it, so
    /// compaction deletes from here rather than listing it.
    files: Vec<File>,
}

pub struct Owners {
    bucket: Bucket,
    objects: RefCell<HashMap<String, Rc<Slot>>>,
    /// New objects' first epochs, opened at the claim, until their databases
    /// open.
    opened: RefCell<HashMap<String, Epoch>>,
    /// Objects workerd closed, until their eviction.
    retiring: RefCell<HashMap<String, Rc<Slot>>>,
    /// Evictions in flight. Objects written together go idle together, and
    /// a node's lease renewals must not wait behind all their uploads.
    evictions: tokio::sync::Semaphore,
    /// Objects evicted with the bucket holding their epoch's session: this
    /// node's database file is that session's state.
    dormant: RefCell<HashMap<String, Object>>,
}

fn object_key(ns: &str, actor: &str) -> String {
    format!("{ns}/{actor}")
}

impl Owners {
    pub fn new(bucket: Bucket) -> Self {
        Self {
            bucket,
            objects: RefCell::new(HashMap::new()),
            opened: RefCell::new(HashMap::new()),
            retiring: RefCell::new(HashMap::new()),
            evictions: tokio::sync::Semaphore::new(EVICT_CONCURRENCY),
            dormant: RefCell::new(HashMap::new()),
        }
    }

    fn client(&self, ns: &str, actor: &str) -> Log {
        let path = format!(
            "{}{}",
            self.bucket.prefix(),
            crate::object_prefix(ns, actor)
        );
        Log::new(self.bucket.at(&path))
    }

    /// Take the object over at `owned.epoch` and open the epoch, leaving its
    /// database at `path` ready for workerd to open.
    pub async fn take_over(
        &self,
        ownership: &Ownership,
        ns: &str,
        actor: &str,
        owned: &Owned,
        path: &Path,
    ) -> Result<()> {
        let key = object_key(ns, actor);
        if owned.recover.is_none() && !self.opened.borrow().contains_key(&key) {
            if self.objects.borrow().contains_key(&key) {
                return Ok(());
            }
            // Woken where it was evicted, own.json unchanged: continue the
            // epoch's session over the file, with no write to the bucket. A
            // dormant entry forgotten since the claim fails this attempt;
            // the retry recovers at the next epoch.
            let dormant = self.dormant.borrow_mut().remove(&key);
            let Some(object) = dormant.filter(|o| o.epoch == owned.epoch && path.exists()) else {
                return Err(Error::Contended(format!(
                    "{key}: not open here, nor dormant"
                )));
            };
            let capture = Capture::resume(path, object.cursor);
            tracing::info!(
                "cluster: {ns}/{actor} epoch {} resumes at {}",
                object.epoch,
                object.tiered
            );
            self.objects
                .borrow_mut()
                .insert(key, Slot::opened(object, capture));
            return Ok(());
        }
        // Woken here from the epoch this node evicted: its database file is
        // what the bucket holds, so the restore is skipped. The entry stays
        // until the epoch opens, so a concurrent claim here also finds the
        // object dormant.
        let dormant = self.dormant.borrow().get(&key).map(|o| o.epoch);
        let recover = owned.recover.filter(|&from| from != 0);
        let local = path.exists() && recover.is_some_and(|from| dormant == Some(from));
        if !local {
            remove_database(path)?;
            if let Some(recover) = recover {
                // Nothing in the bucket: an object that committed nothing.
                let log = self.client(ns, actor);
                let end = crate::log::restore(&log, recover as u32, path).await?;
                tracing::info!(
                    "cluster: {ns}/{actor} epoch {} recovers {recover}: bucket up to {end}",
                    owned.epoch
                );
            }
        }
        // The state the epoch inherits is its session's snapshot at frame
        // 0, written from the file on the blocking pool; a new object has
        // none.
        let cache = open_cache(path, owned.epoch)?;
        let session = owned.epoch as u32;
        let snapshot = if self.opened.borrow().contains_key(&key) {
            None
        } else {
            let (path, cache) = (path.to_owned(), cache.clone());
            blocking(move || crate::log::snapshot_file(&path, &cache, session)).await?
        };
        let cursor = Cursor {
            session,
            lane: snapshot.is_some(),
            ..Cursor::default()
        };
        let capture = Capture::resume(path, cursor);
        let object = self
            .open_epoch(ownership, ns, actor, owned, cache, snapshot)
            .await?;
        self.dormant.borrow_mut().remove(&key);
        self.objects
            .borrow_mut()
            .insert(key, Slot::opened(object, capture));
        Ok(())
    }

    /// Open a new object's first epoch at its claim, before a request
    /// reaches it: create own.json, open, in one conditional write. A
    /// claimant that loses the write answers `false` and routes to the
    /// winner. Nothing to snapshot: an epoch that committed nothing restores
    /// as an empty database.
    pub async fn open_fresh(
        &self,
        ownership: &Ownership,
        ns: &str,
        actor: &str,
        owned: &Owned,
    ) -> Result<bool> {
        if !ownership.open(ns, actor, owned.epoch).await? {
            return Ok(false);
        }
        tracing::info!("cluster: {ns}/{actor} epoch {} opens new", owned.epoch);
        self.opened
            .borrow_mut()
            .insert(object_key(ns, actor), owned.epoch);
        Ok(true)
    }

    /// Open a claimed epoch over the recovered state. Its session starts
    /// with the state's snapshot (none for an empty database), which the
    /// bucket holds before the epoch opens.
    async fn open_epoch(
        &self,
        ownership: &Ownership,
        ns: &str,
        actor: &str,
        owned: &Owned,
        cache: PathBuf,
        snapshot: Option<File>,
    ) -> Result<Object> {
        let mut object = Object {
            ns: ns.to_owned(),
            actor: actor.to_owned(),
            epoch: owned.epoch,
            replica: Replica {
                client: self.client(ns, actor),
                cache,
                session: owned.epoch as u32,
            },
            tiered: 0,
            cursor: Cursor::default(),
            files: Vec::new(),
        };
        if self
            .opened
            .borrow_mut()
            .remove(&object_key(ns, actor))
            .is_some()
        {
            // A new object, opened at its claim.
            return Ok(object);
        }
        if let Some(snapshot) = snapshot {
            object.files.push(upload(&object.replica, snapshot).await?);
        }
        if !ownership.open(ns, actor, owned.epoch).await? {
            return Err(Error::Contended(format!(
                "{ns}/{actor}: epoch {} was superseded before it opened",
                owned.epoch
            )));
        }
        tracing::info!("cluster: {ns}/{actor} epoch {} open", owned.epoch);
        Ok(object)
    }

    /// Capture every commit since the last call and make it durable in the
    /// bucket.
    ///
    /// Commits upload as a group. Each captures its own transactions, then
    /// one at a time uploads everything captured so far on behalf of all,
    /// so a hot object pays an upload per group, not per transaction.
    pub async fn commit(&self, ownership: &Ownership, ns: &str, actor: &str) -> Result<()> {
        let slot = self.objects.borrow().get(&object_key(ns, actor)).cloned();
        let slot =
            slot.ok_or_else(|| Error::Contended(format!("{ns}/{actor} is not open here")))?;
        let target = {
            let guard = slot.object.lock().await;
            let target = slot.capture.borrow().last()?;
            (guard.epoch, target)
        };
        let mut acked = slot.acked.subscribe();
        loop {
            if *acked.borrow_and_update() >= target {
                return Ok(());
            }
            if slot.pushing.replace(true) {
                let _ = acked.changed().await;
                continue;
            }
            let _pushing = Pushing(&slot);
            self.push(ownership, &slot).await?;
        }
    }

    /// Upload everything captured, and acknowledge it if own.json still
    /// names the epoch after the upload.
    async fn push(&self, ownership: &Ownership, slot: &Slot) -> Result<()> {
        let (ns, actor, epoch, round, staged, replica) = {
            let guard = slot.object.lock().await;
            let round = slot.capture.borrow().last()?;
            let staged = stage(&slot.capture.borrow(), guard.tiered, round)?;
            let (ns, actor) = (guard.ns.clone(), guard.actor.clone());
            (ns, actor, guard.epoch, round, staged, guard.replica.clone())
        };
        let file = put(&replica, round, staged).await?;
        {
            let mut guard = slot.object.lock().await;
            if guard.epoch == epoch {
                slot.tiered(&mut guard, round, file);
            }
        }
        if ownership.current_epoch(&ns, &actor).await? != Some(epoch) {
            return Err(Error::Contended(format!(
                "{ns}/{actor}: epoch {epoch} was superseded"
            )));
        }
        slot.ack(epoch, round);
        Ok(())
    }

    /// Snapshot every owned object whose log outgrew its database. Runs off
    /// the commit path.
    pub async fn tier(&self) {
        let objects: Vec<_> = self.objects.borrow().values().cloned().collect();
        stream::iter(objects)
            .for_each_concurrent(TIER_CONCURRENCY, |object| async move {
                let due = snapshot_due(&object.object.lock().await.files);
                if due {
                    self.compact(&object).await;
                }
            })
            .await;
    }

    /// Upload what the bucket lacks for one object.
    async fn tier_one(&self, object: &Slot) {
        let guard = object.object.lock().await;
        let Ok(through) = object.capture.borrow().last() else {
            return;
        };
        let staged = stage(&object.capture.borrow(), guard.tiered, through);
        let Ok(Some(staged)) = staged else {
            return;
        };
        let replica = guard.replica.clone();
        drop(guard);
        let Ok(file) = put(&replica, through, Some(staged)).await else {
            return;
        };
        let mut guard = object.object.lock().await;
        object.tiered(&mut guard, through, file);
    }

    /// Snapshot an epoch's session from its newest snapshot and the batches
    /// past it, and delete what the new one covers, so a takeover's restore
    /// reads one file, not one per upload. The snapshot replays the log,
    /// never the live database, so it runs off the event loop.
    async fn compact(&self, slot: &Slot) {
        let (replica, epoch, plan, key) = {
            let guard = slot.object.lock().await;
            let key = format!("{}/{}", guard.ns, guard.actor);
            (guard.replica.clone(), guard.epoch, plan(&guard.files), key)
        };
        let snapshot = match merged(&replica, &plan).await {
            Ok(snapshot) => snapshot,
            Err(e) => return tracing::warn!("cluster: {key} compaction failed: {e}"),
        };
        let from = plan.first().filter(|f| f.snapshot).map_or(0, |f| f.frame);
        if snapshot.frame <= from {
            return tracing::warn!("cluster: {key} log has a gap after {from}");
        }
        let snapshot = match upload(&replica, snapshot).await {
            Ok(snapshot) => snapshot,
            Err(e) => return tracing::warn!("cluster: {key} snapshot upload failed: {e}"),
        };
        let replaced: Vec<File> = {
            let mut guard = slot.object.lock().await;
            if guard.epoch != epoch {
                return;
            }
            slot.tiered(&mut guard, snapshot.frame, Some(snapshot));
            let (replaced, kept) = std::mem::take(&mut guard.files)
                .into_iter()
                .partition(|f| f.frame <= snapshot.frame && *f != snapshot);
            guard.files = kept;
            replaced
        };
        let replica = &replica;
        stream::iter(replaced)
            .for_each_concurrent(TIER_CONCURRENCY, |f| async move {
                let _ = std::fs::remove_file(replica.cache.join(f.name()));
                let _ = replica.client.delete(&f).await;
            })
            .await;
    }

    /// Whether an object is open here, or opened by its claim.
    pub fn holds(&self, ns: &str, actor: &str) -> bool {
        let key = object_key(ns, actor);
        self.objects.borrow().contains_key(&key) || self.opened.borrow().contains_key(&key)
    }

    /// Stop serving an object workerd closed, keeping it for its eviction.
    pub fn retire(&self, ns: &str, actor: &str) {
        let key = object_key(ns, actor);
        self.opened.borrow_mut().remove(&key);
        if let Some(slot) = self.objects.borrow_mut().remove(&key) {
            self.retiring.borrow_mut().insert(key, slot);
        }
    }

    /// Keep an object workerd evicted for being idle: upload what the bucket
    /// lacks, and remember that this node's file holds the epoch. The record
    /// still names this node, so other nodes route here, and the next
    /// request wakes it without a restore.
    pub async fn evict(&self, ns: &str, actor: &str, epoch: Epoch) {
        let _permit = self.evictions.acquire().await;
        let key = object_key(ns, actor);
        let Some(slot) = self.retiring.borrow_mut().remove(&key) else {
            return;
        };
        self.tier_one(&slot).await;
        let mut guard = slot.object.lock().await;
        let last = slot.capture.borrow().last();
        guard.cursor = slot.capture.borrow().cursor();
        if guard.epoch == epoch && last.is_ok_and(|last| guard.tiered >= last) {
            // Bounded: a forgotten entry costs its object a restore.
            let mut map = self.dormant.borrow_mut();
            if map.len() >= DORMANT_MAX {
                map.clear();
            }
            map.insert(key, guard.clone());
        }
    }

    /// The epoch this node evicted an object at, with the bucket holding it.
    pub fn dormant(&self, ns: &str, actor: &str) -> Option<Epoch> {
        let dormant = self.dormant.borrow();
        dormant.get(&object_key(ns, actor)).map(|o| o.epoch)
    }

    /// Forget an object that left this node.
    pub fn close(&self, ns: &str, actor: &str) {
        let key = object_key(ns, actor);
        self.objects.borrow_mut().remove(&key);
        self.opened.borrow_mut().remove(&key);
    }

    /// An owned object's database was deleted: its session continues with
    /// an empty database, which the next commit makes durable.
    pub fn database_reset(&self, ns: &str, actor: &str) {
        if let Some(slot) = self.objects.borrow().get(&object_key(ns, actor)) {
            slot.capture.borrow_mut().reset();
        }
    }

    /// A write `SQLite` made to an owned object's WAL.
    pub fn wal_write(&self, ns: &str, actor: &str, offset: u64, data: &[u8]) {
        if let Some(slot) = self.objects.borrow().get(&object_key(ns, actor)) {
            slot.capture.borrow_mut().write(offset, data);
        }
    }
}

/// The capture's frames from `from` up to `to`, as one batch.
fn stage(capture: &Capture, from: Sequence, to: Sequence) -> Result<Option<Vec<u8>>> {
    if to <= from {
        return Ok(None);
    }
    Ok(Some(crate::log::compress(&capture.batch(from, to)?)?))
}

/// `plan`'s files replayed into this node's copy of a snapshot, on the
/// blocking pool. A missing copy is fetched from the bucket first.
async fn merged(replica: &Replica, plan: &[File]) -> Result<File> {
    let paths: Vec<_> = plan.iter().map(|f| replica.cache.join(f.name())).collect();
    for (f, path) in plan.iter().zip(&paths) {
        if !path.exists() {
            let tmp = tmp_path(path);
            replica.client.get(f, &tmp).await?;
            std::fs::rename(&tmp, path)?;
        }
    }
    let files: Vec<_> = plan.iter().copied().zip(paths).collect();
    let cache = replica.cache.clone();
    blocking(move || crate::log::compact(&files, &cache)).await
}

/// Writes a copy whole and renames it into place, so a reader never sees
/// part of one.
fn keep(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    let tmp = tmp_path(path);
    std::fs::write(&tmp, bytes)?;
    std::fs::rename(&tmp, path)
}

/// A temporary name beside `path`, unique within the process: a push and a
/// tiering pass can stage the same file at once.
fn tmp_path(path: &Path) -> PathBuf {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(format!(".{n}.tmp"));
    tmp.into()
}

pub async fn blocking<T: Send + 'static>(
    f: impl FnOnce() -> Result<T> + Send + 'static,
) -> Result<T> {
    tokio::task::spawn_blocking(f)
        .await
        .map_err(|e| Error::Replica(format!("blocking task: {e}")))?
}

/// Where an epoch's session is: the bucket, and this node's copy of it.
#[derive(Clone)]
struct Replica {
    client: Log,
    /// `<database>.log/e<epoch>`.
    cache: PathBuf,
    session: u32,
}

/// The copies' directory for `epoch` of the database at `path`, created
/// empty. Other epochs' copies are removed.
fn open_cache(path: &Path, epoch: Epoch) -> Result<PathBuf> {
    let root = cache_root(path);
    remove_dir(&root)?;
    let cache = root.join(format!("e{epoch}"));
    std::fs::create_dir_all(&cache)?;
    Ok(cache)
}

fn cache_root(path: &Path) -> PathBuf {
    let mut root = path.as_os_str().to_owned();
    root.push(".log");
    root.into()
}

fn remove_dir(path: &Path) -> Result<()> {
    match std::fs::remove_dir_all(path) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(crate::Error::from(e)),
        _ => Ok(()),
    }
}

/// Whether `files`, a session's, call for a snapshot: the log since the
/// last one reached its size, so bucket writes stay within about twice the
/// log whatever the database's size.
fn snapshot_due(files: &[File]) -> bool {
    let (image, log) = plan(files).iter().fold((0, 0), |(image, log), f| {
        if f.snapshot {
            (f.size, log)
        } else {
            (image, log + f.size)
        }
    });
    log > 0 && log >= image
}

/// Uploads this node's copy of `file`.
async fn upload(replica: &Replica, file: File) -> Result<File> {
    let path = replica.cache.join(file.name());
    replica.client.put(&file, &path).await?;
    Ok(file)
}

/// Keeps `staged`, if any, as this node's copy of the batch up to `to`,
/// then uploads it.
async fn put(replica: &Replica, to: Sequence, staged: Option<Vec<u8>>) -> Result<Option<File>> {
    let Some(bytes) = staged else {
        return Ok(None);
    };
    let file = File {
        snapshot: false,
        session: replica.session,
        frame: to,
        size: bytes.len() as u64,
    };
    let path = replica.cache.join(file.name());
    blocking(move || Ok(keep(&path, &bytes)?)).await?;
    upload(replica, file).await.map(Some)
}

fn remove_file(path: &Path) -> Result<()> {
    match std::fs::remove_file(path) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(crate::Error::from(e)),
        _ => Ok(()),
    }
}

/// A local copy is never authoritative at takeover: every acknowledged
/// commit is in the bucket.
fn remove_database(path: &Path) -> Result<()> {
    for suffix in ["", "-wal", "-shm"] {
        let mut file = path.as_os_str().to_owned();
        file.push(suffix);
        remove_file(Path::new(&file))?;
    }
    remove_dir(&cache_root(path))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn file(snapshot: bool, frame: Sequence, size: u64) -> File {
        File {
            snapshot,
            session: 1,
            frame,
            size,
        }
    }

    #[test]
    fn a_snapshot_is_due_when_the_log_reaches_its_size() {
        let mut files = vec![file(true, 0, 1000), file(false, 3, 960)];
        assert!(!snapshot_due(&files), "960 bytes of log, a 1000-byte image");
        files.push(file(false, 5, 40));
        assert!(snapshot_due(&files));
        files.push(file(true, 5, 1000));
        assert!(!snapshot_due(&files), "a snapshot covers the log");
        assert!(snapshot_due(&[file(false, 2, 1)]), "no snapshot yet");
        assert!(!snapshot_due(&[]));
    }
}
