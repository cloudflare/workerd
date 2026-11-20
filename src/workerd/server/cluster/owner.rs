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
use std::collections::HashSet;
use std::path::Path;
use std::path::PathBuf;
use std::rc::Rc;
use std::sync::Arc;

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
use crate::log::DB;
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

/// Where an object's databases are on this node, which workerd decides.
pub trait Layout {
    /// The database file of lane `lane`, its facet numbered if new.
    fn place(&self, lane: &str) -> Result<PathBuf>;
    /// Removes every local file of the object: databases, WALs, and
    /// whatever numbers its facets. Answers the removal, which runs on the
    /// blocking pool; what must happen on this thread happens first.
    fn clear(&self) -> Result<Removal>;
}

/// File system work that runs off the event loop.
pub type Removal = Box<dyn FnOnce() -> Result<()> + Send>;

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
    /// Where the capture was at eviction, and its databases.
    cursor: Cursor,
    databases: Vec<(String, PathBuf)>,
    /// The files of this epoch's session. Only this owner writes it, so
    /// compaction deletes from here rather than listing it.
    files: Vec<File>,
    /// Whether earlier sessions may remain in the bucket, which no restore
    /// reads once this epoch opened.
    prune: bool,
}

pub struct Owners {
    bucket: Bucket,
    objects: RefCell<HashMap<String, Rc<Slot>>>,
    /// New objects' first epochs, opened at the claim, until their databases
    /// open.
    opened: RefCell<HashMap<String, Epoch>>,
    /// Objects workerd closed, until their eviction.
    retiring: RefCell<HashMap<String, Rc<Slot>>>,
    /// Evictions running, by object: the epoch, and a receiver whose sender
    /// the eviction drops once it has settled whether the object is dormant.
    evicting: Evictions,
    /// Evictions in flight. Objects written together go idle together, and
    /// a node's lease renewals must not wait behind all their uploads.
    evictions: tokio::sync::Semaphore,
    /// Objects evicted with the bucket holding their epoch's session: this
    /// node's database file is that session's state.
    dormant: RefCell<HashMap<String, Object>>,
    /// Objects whose compaction is in flight.
    compacting: RefCell<HashSet<String>>,
    /// Per object, the lock of `Hold`.
    holds: Arc<Holds>,
    /// The restores begun, each with a spool of its own (`open_spool`).
    restores: Cell<u64>,
}

/// Each object's `Hold` lock, while anything holds or awaits it.
type Holds = std::sync::Mutex<HashMap<String, std::sync::Weak<HoldLock>>>;

type HoldLock = tokio::sync::Mutex<HoldEntry>;

/// What a `Hold` lock guards: its entry in `Holds`, which goes with the
/// lock's last reference, on whichever thread drops it, a blocking task's
/// included.
struct HoldEntry {
    holds: Arc<Holds>,
    key: String,
}

impl Drop for HoldEntry {
    fn drop(&mut self) {
        let mut holds = self
            .holds
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        // Unless a holder made the object a new lock since.
        if holds.get(&self.key).is_some_and(|w| w.strong_count() == 0) {
            holds.remove(&self.key);
        }
    }
}

/// A hold on an object's local files, for a takeover or the sync of an
/// eviction. Each task the holder starts on the blocking pool shares it, so
/// that a holder dropped meanwhile, a takeover cancelled, holds the object
/// until the last such task has finished: the next holder waits, and never
/// removes, moves or reads the files beside a task of the last. The files'
/// paths are the object's whatever the epoch, so an epoch cannot tell them
/// apart.
struct Hold(Arc<tokio::sync::OwnedMutexGuard<HoldEntry>>);

impl Hold {
    async fn new(owners: &Owners, key: &str) -> Self {
        let lock = {
            let mut holds = owners
                .holds
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            let held = holds.get(key).and_then(std::sync::Weak::upgrade);
            held.unwrap_or_else(|| {
                let lock = Arc::new(HoldLock::new(HoldEntry {
                    holds: Arc::clone(&owners.holds),
                    key: key.to_owned(),
                }));
                holds.insert(key.to_owned(), Arc::downgrade(&lock));
                lock
            })
        };
        Self(Arc::new(lock.lock_owned().await))
    }

    /// A share of the hold, for a task the holder starts.
    fn share(&self) -> Share {
        Arc::clone(&self.0) as Share
    }

    async fn blocking<T: Send + 'static>(
        &self,
        f: impl FnOnce() -> Result<T> + Send + 'static,
    ) -> Result<T> {
        blocking_sharing(&self.share(), f).await
    }
}

/// Evictions running, by object (`Owners::evicting`).
type Evictions = Rc<RefCell<HashMap<String, (Epoch, tokio::sync::watch::Receiver<()>)>>>;

/// An object's eviction in flight, until this is dropped (`Owners::evicted`).
pub struct Evicting {
    running: Evictions,
    key: String,
    settling: tokio::sync::watch::Receiver<()>,
    _settled: tokio::sync::watch::Sender<()>,
}

impl Evicting {
    fn new(owners: &Owners, key: String, epoch: Epoch) -> Self {
        let (settled, settling) = tokio::sync::watch::channel(());
        let entry = (epoch, settling.clone());
        owners.evicting.borrow_mut().insert(key.clone(), entry);
        Self {
            running: Rc::clone(&owners.evicting),
            key,
            settling,
            _settled: settled,
        }
    }
}

impl Drop for Evicting {
    fn drop(&mut self) {
        let mut evicting = self.running.borrow_mut();
        // Unless a later eviction of the object replaced it.
        if evicting
            .get(&self.key)
            .is_some_and(|(_, settling)| settling.same_channel(&self.settling))
        {
            evicting.remove(&self.key);
        }
    }
}

/// An object's compaction in flight, until this is dropped.
struct Compacting<'a> {
    owners: &'a Owners,
    key: String,
}

impl Drop for Compacting<'_> {
    fn drop(&mut self) {
        self.owners.compacting.borrow_mut().remove(&self.key);
    }
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
            evicting: Rc::default(),
            evictions: tokio::sync::Semaphore::new(EVICT_CONCURRENCY),
            dormant: RefCell::new(HashMap::new()),
            compacting: RefCell::new(HashSet::new()),
            holds: Arc::default(),
            restores: Cell::new(0),
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
    /// databases where `layout` places them, ready for workerd to open.
    pub async fn take_over(
        &self,
        ownership: &Ownership,
        ns: &str,
        actor: &str,
        owned: &Owned,
        layout: &dyn Layout,
    ) -> Result<()> {
        let hold = Hold::new(self, &object_key(ns, actor)).await;
        self.take_over_held(ownership, ns, actor, owned, layout, &hold)
            .await
    }

    async fn take_over_held(
        &self,
        ownership: &Ownership,
        ns: &str,
        actor: &str,
        owned: &Owned,
        layout: &dyn Layout,
        hold: &Hold,
    ) -> Result<()> {
        let key = object_key(ns, actor);
        let path = &layout.place(DB)?;
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
                return Err(Error::Replica(format!("{key}: not open here, nor dormant")));
            };
            let capture = Capture::resume(object.cursor.clone(), &object.databases);
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
        let mut databases = vec![(DB.to_owned(), path.clone())];
        if local {
            let dormant = self.dormant.borrow().get(&key).map(|o| o.databases.clone());
            databases = dormant.unwrap_or(databases);
        } else {
            // An eviction still running must not leave the files it no
            // longer describes dormant.
            self.retiring.borrow_mut().remove(&key);
            self.dormant.borrow_mut().remove(&key);
            hold.blocking(layout.clear()?).await?;
            if let Some(recover) = recover {
                // Nothing in the bucket: an object that committed nothing.
                let log = self.client(ns, actor);
                let spools = crate::log::with_suffix(path, ".restore");
                let spool = {
                    let restore = self.restores.get();
                    self.restores.set(restore + 1);
                    let spools = spools.clone();
                    hold.blocking(move || Ok(open_spool(&spools, restore)?))
                        .await?
                };
                let restored =
                    crate::log::restore(&log, recover as u32, &spool, &hold.share()).await;
                let placed = match restored {
                    Ok((end, lanes)) => place(layout, lanes, hold).await.map(|placed| {
                        databases = placed;
                        end
                    }),
                    Err(e) => Err(e),
                };
                // As `open_spool` would: the restore may have left downloads
                // behind if it failed.
                hold.blocking(move || {
                    let _ = std::fs::remove_dir_all(&spool);
                    let _ = std::fs::remove_dir(&spools);
                    Ok(())
                })
                .await?;
                let end = placed?;
                tracing::info!(
                    "cluster: {ns}/{actor} epoch {} recovers {recover}: bucket up to {end}, \
                     {} databases",
                    owned.epoch,
                    databases.len()
                );
            }
        }
        // The state the epoch inherits is its session's snapshot at frame
        // 0, written from the files on the blocking pool; a new object has
        // none.
        let cache = {
            let (root, epoch) = (cache_root(path), owned.epoch);
            hold.blocking(move || Ok(open_epoch_dir(&root, epoch)?))
                .await?
        };
        let session = owned.epoch as u32;
        let (snapshot, lanes) = if self.opened.borrow().contains_key(&key) {
            (None, Vec::new())
        } else {
            let (databases, cache) = (databases.clone(), cache.clone());
            hold.blocking(move || crate::log::snapshot_file(&databases, &cache, session))
                .await?
        };
        let cursor = Cursor {
            session,
            lanes,
            ..Cursor::default()
        };
        let capture = Capture::resume(cursor, &databases);
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
            databases: Vec::new(),
            files: Vec::new(),
            prune: false,
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
            return Err(Error::Replica(format!(
                "{ns}/{actor}: epoch {} was superseded before it opened",
                owned.epoch
            )));
        }
        tracing::info!("cluster: {ns}/{actor} epoch {} open", owned.epoch);
        object.prune = true;
        Ok(object)
    }

    /// Capture every commit since the last call and make it durable in the
    /// bucket.
    ///
    /// Commits upload as a group. Each captures its own transactions, then
    /// one at a time uploads everything captured so far on behalf of all,
    /// so a hot object pays an upload per group, not per transaction.
    pub async fn commit(&self, ownership: &Ownership, ns: &str, actor: &str) -> Result<()> {
        let slot = self.slot(ns, actor)?;
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
            return Err(Error::Replica(format!(
                "{ns}/{actor}: epoch {epoch} was superseded"
            )));
        }
        slot.ack(epoch, round);
        Ok(())
    }

    /// Delete the sessions before each newly opened epoch's, and snapshot
    /// every owned object whose log outgrew its database. Runs off the
    /// commit path.
    pub async fn tier(&self) {
        let objects: Vec<_> = self.objects.borrow().clone().into_iter().collect();
        stream::iter(objects)
            .for_each_concurrent(TIER_CONCURRENCY, |(key, object)| async move {
                // Marked before anything waits, and only while the object is
                // served here: a handoff retires the object first, then waits
                // for the mark to go before it releases it (`busy`).
                let Some(_compacting) = self.start_compacting(&key, &object) else {
                    return;
                };
                let (due, prune) = {
                    let mut guard = object.object.lock().await;
                    let prune = std::mem::take(&mut guard.prune);
                    (
                        snapshot_due(&guard.files),
                        prune.then(|| guard.replica.clone()),
                    )
                };
                if let Some(replica) = prune {
                    prune_older(&replica).await;
                }
                if due {
                    self.compact(&key, &object).await;
                }
            })
            .await;
    }

    /// Whether `slot` is still the object `key` this node serves.
    fn serves(&self, key: &str, slot: &Rc<Slot>) -> bool {
        let objects = self.objects.borrow();
        objects
            .get(key)
            .is_some_and(|served| Rc::ptr_eq(served, slot))
    }

    /// Marks object `key` as compacting, unless this node no longer serves
    /// it as `slot` or another compaction of it is in flight.
    fn start_compacting(&self, key: &str, slot: &Rc<Slot>) -> Option<Compacting<'_>> {
        if !self.serves(key, slot) || !self.compacting.borrow_mut().insert(key.to_owned()) {
            return None;
        }
        Some(Compacting {
            owners: self,
            key: key.to_owned(),
        })
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
    /// never the live database, so it runs off the event loop. It stops once
    /// the object is no longer served here.
    async fn compact(&self, key: &str, slot: &Rc<Slot>) {
        let (replica, epoch, plan) = {
            let guard = slot.object.lock().await;
            (guard.replica.clone(), guard.epoch, plan(&guard.files))
        };
        let snapshot = match merged(&replica, &plan).await {
            Ok(snapshot) => snapshot,
            Err(e) => return tracing::warn!("cluster: {key} compaction failed: {e}"),
        };
        let from = plan.first().filter(|f| f.snapshot).map_or(0, |f| f.frame);
        if snapshot.frame <= from {
            return tracing::warn!("cluster: {key} log has a gap after {from}");
        }
        if !self.serves(key, slot) {
            return;
        }
        let snapshot = match upload(&replica, snapshot).await {
            Ok(snapshot) => snapshot,
            Err(e) => return tracing::warn!("cluster: {key} snapshot upload failed: {e}"),
        };
        let replaced: Vec<File> = {
            let mut guard = slot.object.lock().await;
            if guard.epoch != epoch || !self.serves(key, slot) {
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

    /// Runs `f` while holding object `ns/actor`'s lock, as an upload would.
    #[cfg(test)]
    pub(crate) async fn while_locked<T>(
        &self,
        ns: &str,
        actor: &str,
        f: impl Future<Output = T>,
    ) -> T {
        let slot = self.objects.borrow()[&object_key(ns, actor)].clone();
        let _guard = slot.object.lock().await;
        f.await
    }

    /// Whether an object is open here, or opened by its claim.
    pub fn holds(&self, ns: &str, actor: &str) -> bool {
        let key = object_key(ns, actor);
        self.objects.borrow().contains_key(&key) || self.opened.borrow().contains_key(&key)
    }

    /// Whether an object is open here, opened by its claim, uploading its
    /// eviction, or compacting.
    pub fn busy(&self, ns: &str, actor: &str) -> bool {
        let key = object_key(ns, actor);
        self.holds(ns, actor)
            || self.retiring.borrow().contains_key(&key)
            || self.compacting.borrow().contains(&key)
    }

    /// Forget an object this node handed off: its dormant state and its
    /// copies of the log.
    pub fn forget(&self, ns: &str, actor: &str) {
        let dormant = self.dormant.borrow_mut().remove(&object_key(ns, actor));
        if let Some(root) = dormant.as_ref().and_then(|o| o.replica.cache.parent())
            && let Err(e) = remove_dir(root)
        {
            tracing::warn!("cluster: {ns}/{actor}: removing its log copies: {e}");
        }
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
    ///
    /// Its files are synced first. Workerd writes a clustered object's files
    /// without syncing them, the bucket holding each commit, so a write the
    /// OS failed to write back would surface nowhere: the wake would read
    /// what the OS kept in its stead. The sync reports it, and the object
    /// is not kept; the next request restores it from the bucket.
    pub async fn evict(&self, ns: &str, actor: &str, epoch: Epoch) {
        let key = object_key(ns, actor);
        let _evicting = Evicting::new(self, key.clone(), epoch);
        let _permit = self.evictions.acquire().await;
        let Some(slot) = self.retiring.borrow().get(&key).cloned() else {
            return;
        };
        // Not held: an upload can take long, and a takeover here, at the next
        // epoch, need not wait for it (`open_epoch_dir`).
        self.tier_one(&slot).await;
        let databases = slot.capture.borrow().databases();
        let synced = Hold::new(self, &key)
            .await
            .blocking(move || sync_object(&databases))
            .await;
        // Still retiring: no takeover here cleared its files meanwhile.
        let current = self.retiring.borrow().get(&key).cloned();
        if !current.is_some_and(|r| Rc::ptr_eq(&r, &slot)) {
            return;
        }
        self.retiring.borrow_mut().remove(&key);
        if let Err(e) = synced {
            return tracing::warn!("cluster: {key} is not kept, its files failed to sync: {e}");
        }
        let mut guard = slot.object.lock().await;
        let last = slot.capture.borrow().last();
        guard.cursor = slot.capture.borrow().cursor();
        guard.databases = slot.capture.borrow().databases();
        if guard.epoch == epoch && last.is_ok_and(|last| guard.tiered >= last) {
            // Bounded: a forgotten entry costs its object a restore.
            let mut map = self.dormant.borrow_mut();
            if map.len() >= DORMANT_MAX {
                map.clear();
            }
            map.insert(key, guard.clone());
        }
    }

    /// Counts an eviction of an object at `epoch` as running until the
    /// guard is dropped, from before the eviction itself starts (`evict`
    /// counts it from its own start).
    pub fn evicting(&self, ns: &str, actor: &str, epoch: Epoch) -> Evicting {
        Evicting::new(self, object_key(ns, actor), epoch)
    }

    /// Waits for an eviction here of an object at `epoch`, if one is
    /// running, to settle whether the object is `dormant` at the epoch. Its
    /// sync can outlast the object's idle time: a request in the meantime
    /// would otherwise find the object neither open nor dormant, and recover
    /// it at the next epoch. An eviction awaits nothing a claim holds.
    pub async fn evicted(&self, ns: &str, actor: &str, epoch: Epoch) {
        let evicting = self.evicting.borrow().get(&object_key(ns, actor)).cloned();
        if let Some((evicted, mut settling)) = evicting
            && evicted == epoch
        {
            // Fails once the eviction drops the sender.
            let _ = settling.changed().await;
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

    fn slot(&self, ns: &str, actor: &str) -> Result<Rc<Slot>> {
        let slot = self.objects.borrow().get(&object_key(ns, actor)).cloned();
        slot.ok_or_else(|| Error::Replica(format!("{ns}/{actor} is not open here")))
    }

    /// Capture the database at `path`, lane `lane` of an owned object, too.
    pub fn open_lane(&self, ns: &str, actor: &str, path: &Path, lane: &str) -> Result<()> {
        let slot = self.slot(ns, actor)?;
        slot.capture.borrow_mut().open_lane(path, lane);
        Ok(())
    }

    /// An owned object's facet with lane prefix `prefix` was deleted with
    /// its descendants: the next commit makes it durable.
    pub fn delete_lanes(&self, ns: &str, actor: &str, prefix: &str) -> Result<()> {
        let slot = self.slot(ns, actor)?;
        slot.capture.borrow_mut().delete_lanes(prefix);
        Ok(())
    }

    /// An owned object's facet with lane prefix `dst` was replaced with
    /// its descendants by copies of the facet with prefix `src`'s: the next
    /// commit makes it durable. The copies are captured from now, started
    /// or not, so a dormant takeover's snapshot holds them.
    pub fn clone_lanes(
        &self,
        ns: &str,
        actor: &str,
        src: &str,
        dst: &str,
        layout: &dyn Layout,
    ) -> Result<()> {
        let slot = self.slot(ns, actor)?;
        let mut capture = slot.capture.borrow_mut();
        for lane in capture.clone_lanes(src, dst) {
            capture.open_lane(&layout.place(&lane)?, &lane);
        }
        Ok(())
    }

    /// An owned object's database `file` was deleted, with its facet's
    /// descendants: its session continues with an empty database, which
    /// the next commit makes durable.
    pub fn database_reset(&self, ns: &str, actor: &str, file: &str) {
        if let Some(slot) = self.objects.borrow().get(&object_key(ns, actor)) {
            slot.capture.borrow_mut().reset(file);
        }
    }

    /// A write `SQLite` made to the WAL of an owned object's database
    /// `file`.
    pub fn wal_write(&self, ns: &str, actor: &str, file: &str, offset: u64, data: &[u8]) {
        if let Some(slot) = self.objects.borrow().get(&object_key(ns, actor)) {
            slot.capture.borrow_mut().write(file, offset, data);
        }
    }
}

/// The capture's frames from `from` up to `to`, merged into one batch.
fn stage(capture: &Capture, from: Sequence, to: Sequence) -> Result<Option<Vec<u8>>> {
    if to <= from {
        return Ok(None);
    }
    let batch = crate::log::merge(&capture.batch(from, to)?)?;
    Ok(Some(crate::log::compress(&batch)?))
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

/// What a task keeps while it runs: a share of a hold on its files.
pub type Share = Arc<dyn std::any::Any + Send + Sync>;

/// `blocking`, keeping a clone of `share` until `f` returns.
pub async fn blocking_sharing<T: Send + 'static>(
    share: &Share,
    f: impl FnOnce() -> Result<T> + Send + 'static,
) -> Result<T> {
    let share = Arc::clone(share);
    blocking(move || {
        let _share = share;
        f()
    })
    .await
}

pub async fn blocking<T: Send + 'static>(
    f: impl FnOnce() -> Result<T> + Send + 'static,
) -> Result<T> {
    // Tests run on one thread, the simulation outside tokio.
    if cfg!(test) {
        return f();
    }
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

/// `root`'s directory for `epoch`, created empty: a database's copies,
/// `<database>.log/e<epoch>`. Later epochs' are kept. Earlier epochs' are
/// removed if they can be, and anything else there: what a takeover holds
/// (`Hold`) writes only its own epoch's, but an earlier epoch's eviction may
/// still be uploading from its copies or its compaction writing them, and
/// on Windows a file open there cannot be removed. Nothing reads an earlier
/// epoch's, so what is left is only garbage. This epoch's must go: a
/// compaction trusts a copy it finds there.
fn open_epoch_dir(root: &Path, epoch: Epoch) -> std::io::Result<PathBuf> {
    let name = format!("e{epoch}");
    let later = |name: &std::ffi::OsStr| {
        let n = name.to_str().and_then(|n| n.strip_prefix('e'));
        n.and_then(|n| n.parse::<Epoch>().ok())
            .is_some_and(|n| n > epoch)
    };
    let entries = match std::fs::read_dir(root) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
        entries => Some(entries?),
    };
    for entry in entries.into_iter().flatten() {
        let entry = entry?;
        if later(&entry.file_name()) {
            continue;
        }
        let removed = if entry.file_type()?.is_dir() {
            std::fs::remove_dir_all(entry.path())
        } else {
            std::fs::remove_file(entry.path())
        };
        match removed {
            Err(e) if entry.file_name() == *name && e.kind() != std::io::ErrorKind::NotFound => {
                return Err(e);
            }
            _ => {}
        }
    }
    let dir = root.join(name);
    std::fs::create_dir_all(&dir)?;
    Ok(dir)
}

/// Sync an object's files to disk: each database, its WAL, and the root's
/// facet index, `<key>.facets` beside `<key>.sqlite`.
fn sync_object(databases: &[(String, PathBuf)]) -> Result<()> {
    for (lane, path) in databases {
        let wal = crate::log::with_suffix(path, "-wal");
        let index = (lane == DB).then(|| path.with_extension("facets"));
        for file in [Some(path.clone()), Some(wal), index].into_iter().flatten() {
            // Written to, for Windows to flush it.
            match std::fs::OpenOptions::new().write(true).open(&file) {
                Ok(file) => sync_file(&file)?,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(e.into()),
            }
        }
    }
    Ok(())
}

/// `file.sync_data()`, but in this crate's tests, whose files are scratch.
/// On macOS and Windows a sync waits for the device to flush, and the owner
/// simulation's hundreds of evictions each sync their files.
fn sync_file(file: &std::fs::File) -> std::io::Result<()> {
    if cfg!(test) { Ok(()) } else { file.sync_data() }
}

/// Restore `restore`'s spool, `<database>.restore/<restore>`, created
/// empty. A restore's tasks share its takeover's hold (`Hold::share`), but
/// for its downloads' writers, which the bucket client starts and nothing
/// awaits once a takeover is cancelled; such a writer writes only its own
/// restore's spool. The others are removed if they can be: nothing reads
/// them, and on Windows a file open there cannot be removed.
fn open_spool(spools: &Path, restore: u64) -> std::io::Result<PathBuf> {
    for entry in std::fs::read_dir(spools).into_iter().flatten().flatten() {
        let _ = std::fs::remove_dir_all(entry.path());
    }
    let spool = spools.join(restore.to_string());
    std::fs::create_dir_all(&spool)?;
    Ok(spool)
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
/// Delete the sessions before `replica`'s. A superseded owner still
/// uploading may leave files behind, which the next epoch deletes.
async fn prune_older(replica: &Replica) {
    let files = match replica.client.older(replica.session).await {
        Ok(files) => files,
        Err(e) => return tracing::warn!("cluster: pruning old sessions: {e}"),
    };
    stream::iter(files)
        .for_each_concurrent(TIER_CONCURRENCY, |f| async move {
            let _ = replica.client.delete(&f).await;
        })
        .await;
}

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

/// Moves restored `lanes` to where `layout` places them, the moves on the
/// blocking pool. Placing stays on this thread: it numbers new facets in
/// the namespace's storage, which lives here, with a write but no sync in a
/// cluster. Returns each lane's name and database.
async fn place(
    layout: &dyn Layout,
    lanes: Vec<(String, PathBuf)>,
    hold: &Hold,
) -> Result<Vec<(String, PathBuf)>> {
    let mut databases = Vec::new();
    let mut moves = Vec::new();
    for (lane, file) in lanes {
        let path = layout.place(&lane)?;
        moves.push((file, path.clone()));
        databases.push((lane, path));
    }
    if !databases.iter().any(|(lane, _)| lane == DB) {
        databases.push((DB.to_owned(), layout.place(DB)?));
    }
    hold.blocking(move || {
        for (file, path) in &moves {
            std::fs::rename(file, path)?;
        }
        Ok(())
    })
    .await?;
    Ok(databases)
}

#[cfg(test)]
#[path = "owner-test.rs"]
mod sim;

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
    fn a_hold_is_forgotten_once_its_last_task_is_done() {
        let owners = Owners::new(crate::dst::Harness::new(0).bucket);
        let hold = futures::executor::block_on(Hold::new(&owners, "ns/a"));
        // A takeover cancelled while its blocking task, which shares the
        // hold, runs on.
        let task = hold.share();
        drop(hold);
        assert_eq!(owners.holds.lock().unwrap().len(), 1);
        std::thread::spawn(move || drop(task)).join().unwrap();
        assert!(owners.holds.lock().unwrap().is_empty());
        // The next holder makes it anew, and forgets it in turn.
        drop(futures::executor::block_on(Hold::new(&owners, "ns/a")));
        assert!(owners.holds.lock().unwrap().is_empty());
    }

    #[test]
    fn a_claim_awaits_an_eviction_of_its_epoch_only() {
        use futures::FutureExt;
        let owners = Owners::new(crate::dst::Harness::new(0).bucket);
        let eviction = Evicting::new(&owners, object_key("ns", "a"), 1);
        assert!(owners.evicted("ns", "a", 2).now_or_never().is_some());
        assert!(owners.evicted("ns", "b", 1).now_or_never().is_some());
        let mut evicted = std::pin::pin!(owners.evicted("ns", "a", 1));
        assert!(evicted.as_mut().now_or_never().is_none());
        // A later eviction of the object, which the first's end leaves.
        let later = Evicting::new(&owners, object_key("ns", "a"), 1);
        drop(eviction);
        assert!(evicted.now_or_never().is_some());
        assert!(owners.evicted("ns", "a", 1).now_or_never().is_none());
        drop(later);
        assert!(owners.evicted("ns", "a", 1).now_or_never().is_some());
        assert!(owners.evicting.borrow().is_empty());
    }

    #[test]
    fn a_task_sharing_a_hold_keeps_the_object_held() {
        use futures::FutureExt;
        let owners = Owners::new(crate::dst::Harness::new(0).bucket);
        let hold = futures::executor::block_on(Hold::new(&owners, "ns/a"));
        // A restore's replay, still running when its takeover is cancelled.
        let replay = hold.share();
        drop(hold);
        let mut next = std::pin::pin!(Hold::new(&owners, "ns/a"));
        assert!(next.as_mut().now_or_never().is_none());
        drop(replay);
        assert!(next.now_or_never().is_some());
    }

    #[test]
    fn a_restore_spool_is_its_own() {
        let tmp = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
        let spools = tmp.join(format!("spools-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&spools);
        // An earlier restore's, cancelled with its downloads in flight.
        std::fs::create_dir_all(spools.join("3/files")).unwrap();
        std::fs::write(spools.join("3/files/0"), "x").unwrap();
        let spool = open_spool(&spools, 4).unwrap();
        assert_eq!(spool, spools.join("4"));
        let left: Vec<_> = std::fs::read_dir(&spools)
            .unwrap()
            .map(|e| e.unwrap().file_name().into_string().unwrap())
            .collect();
        assert_eq!(left, ["4"]);
        assert_eq!(std::fs::read_dir(&spool).unwrap().count(), 0);
        std::fs::remove_dir_all(&spools).unwrap();
    }

    #[test]
    fn an_epoch_dir_starts_empty_and_keeps_later_epochs() {
        let tmp = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
        let root = tmp.join(format!("epoch-dir-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        for (dir, file) in [("e3", "a"), ("e5", "b"), ("e7", "c")] {
            std::fs::create_dir_all(root.join(dir)).unwrap();
            std::fs::write(root.join(dir).join(file), "x").unwrap();
        }
        std::fs::write(root.join("junk"), "x").unwrap();
        let dir = open_epoch_dir(&root, 5).unwrap();
        assert_eq!(dir, root.join("e5"));
        assert_eq!(std::fs::read_dir(&dir).unwrap().count(), 0);
        let mut left: Vec<_> = std::fs::read_dir(&root)
            .unwrap()
            .map(|e| e.unwrap().file_name().into_string().unwrap())
            .collect();
        left.sort();
        assert_eq!(left, ["e5", "e7"]);
        assert!(root.join("e7/c").exists());
        std::fs::remove_dir_all(&root).unwrap();
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
