// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The owner's side of an object: taking it over, the commit path, and
//! keeping its chain in the bucket compact.
//!
//! A commit is acknowledged once the bucket holds it and own.json still
//! names the epoch after the upload. Each epoch's commits form a chain of
//! LTX files under `do/<ns>/<actor>/ltx/e<epoch>`, starting with a snapshot
//! of the state the epoch inherited; a takeover restores the last opened
//! epoch's chain.

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
use crate::Txid;
use crate::capture::Capture;
use crate::ltx::BucketClient;
use crate::ltx::SNAPSHOT_LEVEL;
use crate::ltx::TXID;
use crate::ltx::client::ReplicaClient;
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
    /// Through where commits are in the bucket, by epoch, then txid.
    acked: tokio::sync::watch::Sender<(Epoch, Txid)>,
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

    /// The bucket holds everything through `through`.
    fn tiered(&self, object: &mut Object, through: Txid, file: Option<File>) {
        object.tiered = object.tiered.max(through);
        object.files.extend(file);
        self.capture.borrow_mut().forget(through);
    }

    fn ack(&self, epoch: Epoch, through: Txid) {
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
    /// How far the epoch's chain in the bucket is contiguous.
    tiered: Txid,
    /// The files of this epoch's chain. Only this owner writes the epoch's
    /// prefix, so compaction deletes from here rather than listing it.
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
    /// Objects evicted with the bucket holding their epoch's chain: this
    /// node's database file is that chain's state.
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

    fn client(&self, ns: &str, actor: &str, epoch: Epoch) -> BucketClient {
        let path = format!("{}do/{ns}/{actor}/ltx/e{epoch}", self.bucket.prefix());
        BucketClient::new(self.bucket.at(&path))
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
            // epoch's chain over the file, with no write to the bucket. A
            // dormant entry forgotten since the claim fails this attempt;
            // the retry recovers at the next epoch.
            let dormant = self.dormant.borrow_mut().remove(&key);
            let Some(object) = dormant.filter(|o| o.epoch == owned.epoch && path.exists()) else {
                return Err(Error::Contended(format!(
                    "{key}: not open here, nor dormant"
                )));
            };
            let capture = Capture::resume(path, object.tiered);
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
                let from = self.client(ns, actor, recover);
                let end = end_of(&from).await?;
                tracing::info!(
                    "cluster: {ns}/{actor} epoch {} recovers {recover}: bucket through {end}",
                    owned.epoch
                );
                // Nothing in the bucket: an object that committed nothing.
                if end > 0 {
                    crate::ltx::restore(&from, path, TXID(end)).await?;
                }
            }
        }
        // The state the epoch inherits is its snapshot at txid 1, encoded
        // from the file on the blocking pool; a new object has none.
        let cache = open_cache(path, owned.epoch)?;
        let snapshot = File {
            level: 0,
            first: 1,
            last: 1,
            size: 0,
        };
        let size = if self.opened.borrow().contains_key(&key) {
            None
        } else {
            let (path, out) = (path.to_owned(), cache.join(snapshot.name()));
            blocking(move || crate::capture::snapshot_file(&path, &out)).await?
        };
        let snapshot = size.map(|size| File { size, ..snapshot });
        let capture = Capture::resume(path, snapshot.map_or(0, |f| f.last));
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

    /// Open a claimed epoch over the recovered state. Its chain starts with
    /// the state's snapshot at txid 1 (none for an empty database), which
    /// the bucket holds before the epoch opens.
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
                client: self.client(ns, actor, owned.epoch),
                cache,
            },
            tiered: 0,
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
            object.tiered = 1;
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
            let staged = stage(|t| slot.capture.borrow().entry(t), guard.tiered, round)?;
            let (ns, actor) = (guard.ns.clone(), guard.actor.clone());
            (ns, actor, guard.epoch, round, staged, guard.replica.clone())
        };
        let file = put(&replica, 0, staged).await?;
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

    /// Compact every owned object's chain that grew long enough. Runs off
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
        let staged = stage(|t| object.capture.borrow().entry(t), guard.tiered, through);
        let Ok(Some(staged)) = staged else {
            return;
        };
        let replica = guard.replica.clone();
        drop(guard);
        let Ok(file) = put(&replica, 0, Some(staged)).await else {
            return;
        };
        let mut guard = object.object.lock().await;
        object.tiered(&mut guard, through, file);
    }

    /// Write a snapshot of an epoch's chain at `SNAPSHOT_LEVEL` and delete
    /// what it covers, so a takeover's restore reads one file, not one per
    /// upload. The snapshot merges the chain's files, never the live
    /// database, so the merge runs off the event loop.
    async fn compact(&self, slot: &Slot) {
        let (replica, epoch, chain) = {
            let guard = slot.object.lock().await;
            let Some(chain) = chain(&guard.files) else {
                return;
            };
            (guard.replica.clone(), guard.epoch, chain)
        };
        let Ok(snapshot) = merged(&replica, &chain).await else {
            return;
        };
        let last = snapshot.last;
        let Ok(snapshot) = upload(&replica, snapshot).await else {
            return;
        };
        let replaced: Vec<File> = {
            let mut guard = slot.object.lock().await;
            if guard.epoch != epoch {
                return;
            }
            slot.tiered(&mut guard, last, Some(snapshot));
            let (replaced, kept) = std::mem::take(&mut guard.files)
                .into_iter()
                .partition(|f| f.last <= last && *f != snapshot);
            guard.files = kept;
            replaced
        };
        let replica = &replica;
        stream::iter(replaced)
            .for_each_concurrent(TIER_CONCURRENCY, |f| async move {
                let _ = std::fs::remove_file(replica.cache.join(f.name()));
                let _ = replica
                    .client
                    .delete_ltx_file(f.level, TXID(f.first), TXID(f.last))
                    .await;
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
        let guard = slot.object.lock().await;
        let last = slot.capture.borrow().last();
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

    /// An owned object's database was deleted: its chain continues with an
    /// empty database, which the next commit makes durable.
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

/// Where an epoch's chain in the bucket is contiguous from its start
/// through.
async fn end_of(client: &BucketClient) -> Result<Txid> {
    let mut end = 0;
    for info in crate::ltx::plan(client, TXID(0)).await? {
        if info.min_txid.0 > end + 1 {
            break;
        }
        end = end.max(info.max_txid.0);
    }
    Ok(end)
}

/// One LTX file covering txids `first..=last`, ready to upload.
struct Staged {
    first: Txid,
    last: Txid,
    bytes: Vec<u8>,
}

/// The capture's entries after `from` through `through`, merged into one
/// LTX file, as litestream compacts.
fn stage(
    entry: impl Fn(Txid) -> Result<Vec<u8>>,
    from: Txid,
    through: Txid,
) -> Result<Option<Staged>> {
    if through <= from {
        return Ok(None);
    }
    let files = (from + 1..=through)
        .map(entry)
        .collect::<Result<Vec<_>>>()?;
    Ok(Some(Staged {
        first: from + 1,
        last: through,
        bytes: crate::ltx::ltx::merge_files(&files)?,
    }))
}

/// The files a restore reads, in order, as `ltx::plan` picks them; `None`
/// unless they run unbroken from txid 1.
fn chain(files: &[File]) -> Option<Vec<File>> {
    let mut files = files.to_vec();
    files.sort_by_key(|f| (f.first, std::cmp::Reverse(f.last)));
    let mut reached = 0;
    let mut chain = Vec::new();
    for f in files {
        if f.first > reached + 1 {
            return None;
        }
        if f.last > reached {
            reached = f.last;
            chain.push(f);
        }
    }
    (!chain.is_empty()).then_some(chain)
}

/// `chain`'s files merged into this node's copy of a snapshot covering
/// txid 1 through the chain's end, a page at a time on the blocking pool.
/// A missing copy is fetched from the bucket first.
async fn merged(replica: &Replica, chain: &[File]) -> Result<File> {
    let paths: Vec<_> = chain.iter().map(|f| replica.cache.join(f.name())).collect();
    for (f, path) in chain.iter().zip(&paths) {
        if !path.exists() {
            let (first, last, tmp) = (TXID(f.first), TXID(f.last), tmp_path(path));
            replica
                .client
                .open_ltx_file(f.level, first, last, &tmp)
                .await?;
            std::fs::rename(&tmp, path).map_err(crate::ltx::Error::from)?;
        }
    }
    let snapshot = File {
        level: SNAPSHOT_LEVEL,
        first: 1,
        last: chain.last().map_or(0, |f| f.last),
        size: 0,
    };
    let out = replica.cache.join(snapshot.name());
    let size = blocking(move || {
        let io = |e| crate::Error::from(crate::ltx::Error::from(e));
        let files = paths.iter().map(std::fs::File::open);
        let readers: Vec<_> = files
            .map(|f| f.map(std::io::BufReader::new))
            .collect::<std::io::Result<_>>()
            .map_err(io)?;
        let tmp = tmp_path(&out);
        let mut writer = std::io::BufWriter::new(std::fs::File::create(&tmp).map_err(io)?);
        crate::ltx::ltx::merge_into(readers, &mut writer)?;
        std::io::Write::flush(&mut writer).map_err(io)?;
        std::fs::rename(&tmp, &out).map_err(io)?;
        Ok(std::fs::metadata(&out).map_err(io)?.len())
    })
    .await?;
    Ok(File { size, ..snapshot })
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

async fn blocking<T: Send + 'static>(f: impl FnOnce() -> Result<T> + Send + 'static) -> Result<T> {
    tokio::task::spawn_blocking(f)
        .await
        .map_err(|e| Error::Replica(format!("blocking task: {e}")))?
}

/// Where an epoch's chain is: the bucket, and this node's copy of it.
#[derive(Clone)]
struct Replica {
    client: BucketClient,
    /// `<database>.ltx/e<epoch>`.
    cache: PathBuf,
}

/// The copies' directory for `epoch` of the database at `path`, created
/// empty. Other epochs' copies are removed: a file name repeats in every
/// epoch's chain.
fn open_cache(path: &Path, epoch: Epoch) -> Result<PathBuf> {
    let root = cache_root(path);
    remove_dir(&root)?;
    let cache = root.join(format!("e{epoch}"));
    std::fs::create_dir_all(&cache).map_err(crate::ltx::Error::from)?;
    Ok(cache)
}

fn cache_root(path: &Path) -> PathBuf {
    let mut root = path.as_os_str().to_owned();
    root.push(".ltx");
    root.into()
}

fn remove_dir(path: &Path) -> Result<()> {
    match std::fs::remove_dir_all(path) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => {
            Err(crate::ltx::Error::from(e).into())
        }
        _ => Ok(()),
    }
}

/// One LTX file in an epoch's chain.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct File {
    level: i32,
    first: Txid,
    last: Txid,
    size: u64,
}

impl File {
    fn name(&self) -> String {
        format!(
            "{:04x}-{:016x}-{:016x}.ltx",
            self.level, self.first, self.last
        )
    }
}

/// L0 files a snapshot waits for, so a small object does not snapshot on
/// nearly every commit.
const SNAPSHOT_MIN_FILES: usize = 32;

/// Whether `files` call for a snapshot: the L0 bytes since the last one
/// reach its size, so bucket writes stay within about twice
/// the log whatever the database's size.
fn snapshot_due(files: &[File]) -> bool {
    let image: u64 = files
        .iter()
        .filter(|f| f.level == SNAPSHOT_LEVEL)
        .map(|f| f.size)
        .max()
        .unwrap_or(0);
    let l0: Vec<_> = files.iter().filter(|f| f.level == 0).collect();
    l0.len() >= SNAPSHOT_MIN_FILES && l0.iter().map(|f| f.size).sum::<u64>() >= image
}

/// Uploads this node's copy of `file`.
async fn upload(replica: &Replica, file: File) -> Result<File> {
    let (first, last) = (TXID(file.first), TXID(file.last));
    let path = replica.cache.join(file.name());
    replica
        .client
        .write_ltx_file(file.level, first, last, &path)
        .await?;
    Ok(file)
}

/// Keeps `staged`, if any, as this node's copy at `level`, then uploads it.
async fn put(replica: &Replica, level: i32, staged: Option<Staged>) -> Result<Option<File>> {
    let Some(Staged { first, last, bytes }) = staged else {
        return Ok(None);
    };
    let file = File {
        level,
        first,
        last,
        size: bytes.len() as u64,
    };
    let path = replica.cache.join(file.name());
    blocking(move || Ok(keep(&path, &bytes).map_err(crate::ltx::Error::from)?)).await?;
    upload(replica, file).await.map(Some)
}

fn remove_file(path: &Path) -> Result<()> {
    match std::fs::remove_file(path) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => {
            Err(crate::ltx::Error::from(e).into())
        }
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

    fn file(level: i32, size: u64) -> File {
        File {
            level,
            first: 1,
            last: 1,
            size,
        }
    }

    fn span(level: i32, first: Txid, last: Txid) -> File {
        File {
            level,
            first,
            last,
            size: 0,
        }
    }

    #[test]
    fn a_chain_is_what_a_restore_reads() {
        let snapshot = span(SNAPSHOT_LEVEL, 1, 4);
        let (narrow, wide) = (span(0, 5, 6), span(0, 5, 8));
        let chain = chain(&[span(0, 9, 9), narrow, snapshot, wide]);
        assert_eq!(chain, Some(vec![snapshot, wide, span(0, 9, 9)]));
        assert_eq!(super::chain(&[snapshot, span(0, 6, 6)]), None, "a gap");
        assert_eq!(super::chain(&[]), None);
    }

    #[test]
    fn a_compaction_merges_the_chain() {
        let ltx = |first: Txid, last: Txid, commit: u32, pages: &[(u32, u8)]| {
            let header = crate::ltx::ltx::Header {
                version: crate::ltx::ltx::VERSION,
                flags: crate::ltx::ltx::HEADER_FLAG_NO_CHECKSUM,
                page_size: 512,
                commit,
                min_txid: TXID(first),
                max_txid: TXID(last),
                ..Default::default()
            };
            let pages: Vec<_> = pages.iter().map(|&(p, b)| (p, vec![b; 512])).collect();
            crate::ltx::ltx::encode_file(&header, &pages, 0).unwrap()
        };
        let dir = std::env::temp_dir().join(format!("owner-test-{}", std::process::id()));
        let replica = Replica {
            client: BucketClient::new(Bucket::memory("e1")),
            cache: open_cache(&dir.join("a.sqlite"), 1).unwrap(),
        };
        let files = [
            (span(0, 1, 1), ltx(1, 1, 3, &[(1, 1), (2, 1), (3, 1)])),
            (span(0, 2, 3), ltx(2, 3, 3, &[(2, 2)])),
            (span(0, 4, 4), ltx(4, 4, 2, &[(1, 4)])),
        ];
        let runtime = tokio::runtime::Builder::new_current_thread()
            .build()
            .unwrap();
        let snapshot = runtime.block_on(async {
            // The first two only here, the last only in the bucket.
            for (f, bytes) in &files[..2] {
                std::fs::write(replica.cache.join(f.name()), bytes).unwrap();
            }
            let (f, bytes) = &files[2];
            let (first, last, from) = (TXID(f.first), TXID(f.last), dir.join("up"));
            std::fs::write(&from, bytes).unwrap();
            replica
                .client
                .write_ltx_file(f.level, first, last, &from)
                .await
                .unwrap();
            let chain: Vec<_> = files.iter().map(|(f, _)| *f).collect();
            merged(&replica, &chain).await.unwrap()
        });
        let bytes = std::fs::read(replica.cache.join(snapshot.name())).unwrap();
        std::fs::remove_dir_all(&dir).unwrap();
        assert_eq!((snapshot.first, snapshot.last), (1, 4));
        let mut merge = crate::ltx::ltx::Merge::new(vec![bytes.as_slice()]).unwrap();
        assert_eq!(merge.header.commit, 2, "truncated by the last commit");
        let mut firsts = Vec::new();
        while let Some((pgno, page)) = merge.next().unwrap() {
            firsts.push((pgno, page[0]));
        }
        assert_eq!(firsts, [(1, 4), (2, 2)]);
    }

    #[test]
    fn a_snapshot_is_due_when_the_log_reaches_its_size() {
        let mut files = vec![file(SNAPSHOT_LEVEL, 1000)];
        files.extend((0..SNAPSHOT_MIN_FILES).map(|_| file(0, 30)));
        assert!(
            !snapshot_due(&files),
            "960 bytes of log under a 1000-byte image"
        );
        files.push(file(0, 40));
        assert!(snapshot_due(&files));
        let few: Vec<_> = (0..SNAPSHOT_MIN_FILES - 1)
            .map(|_| file(0, 1 << 20))
            .collect();
        assert!(!snapshot_due(&few), "fewer files than the floor");
    }
}
