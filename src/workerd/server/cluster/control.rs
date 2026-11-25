// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The two parts of an instance's membership, and what joins them.
//!
//! An instance has one **control plane**: its record and lease, the one table
//! of the objects it claimed and the routes it knows (`Ownership`), and its
//! alarm index. It has a **data plane** for each thread that runs objects
//! ([`Member`]): the objects that thread has open, their captured commits and
//! their uploads (`Owners`), over a bucket client of its own. An object
//! belongs to one thread's data plane for as long as the process lives.
//!
//! Every change of who owns an object is the control plane's: a data plane
//! asks for it through [`Control`], and acts on the answer. What it is told
//! is true when it is told; like everything read from the bucket it may be
//! overtaken, which is why a commit is acknowledged only once the owner
//! record still names its epoch after the upload (`Owners::commit`), read by
//! the data plane itself.
//!
//! `Cluster` is an instance with both parts on one thread: it implements
//! [`Control`] itself. A server with several threads gives each of the
//! others a [`Member`] whose [`Control`] reaches the first thread's
//! `Cluster`, and tells them what any thread may read at any time through
//! [`Shared`]: whether the lease holds, and whether the instance drains.

use std::cell::RefCell;
use std::collections::HashMap;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::time::Duration;

use async_trait::async_trait;

use crate::Bucket;
use crate::Claim;
use crate::Error;
use crate::Result;
use crate::cluster::Route;
use crate::cluster::now_ms;
use crate::cluster::open_bucket;
use crate::hmac_sha256;
use crate::keeper::Held;
use crate::owner::Layout;
use crate::owner::Owners;
use crate::ownership::Owned;

/// What capturing an object asks about its owner record (`Owners`).
#[async_trait(?Send)]
pub trait Open {
    /// `Ownership::open`: the one change of an owner record that capturing
    /// an object waits for.
    async fn open(&self, ns: &str, actor: &str, epoch: u64) -> Result<bool>;
    /// `Ownership::current_epoch`: a read of the bucket, which a data plane
    /// makes with its own client (`current_epoch`).
    async fn current_epoch(&self, ns: &str, actor: &str) -> Result<Option<u64>>;
}

/// What a data plane asks of its instance's control plane. Each call is for
/// an object of the data plane that makes it.
#[async_trait(?Send)]
pub trait Control: Open {
    /// `Ownership::acquire_or_route`.
    async fn claim(&self, ns: &str, actor: &str, now_ms: u64) -> Result<Claim>;
    /// `Ownership::advance`.
    async fn advance(&self, ns: &str, actor: &str, epoch: u64) -> Result<Option<Owned>>;
    /// `Ownership::release`.
    async fn release(&self, ns: &str, actor: &str, epoch: u64) -> Result<bool>;
    /// `Ownership::abandon`. It is in effect before the caller's next call.
    fn abandon(&self, ns: &str, actor: &str);
    /// `Ownership::forget_route`. It is in effect before the caller's next
    /// call.
    fn forget_route(&self, ns: &str, actor: &str, node: &str);
    /// `Ownership::refresh_route`.
    async fn refresh_route(&self, ns: &str, actor: &str, now_ms: u64) -> Result<()>;
    /// `alarms::Index::arm`, for the namespace `ns`.
    async fn arm_alarm(
        &self,
        ns: &str,
        actor: &str,
        name: Option<&str>,
        at_ms: Option<u64>,
    ) -> Result<()>;
    /// `alarms::Index::opened`, for the namespace `ns`.
    async fn opened_alarm(
        &self,
        ns: &str,
        actor: &str,
        name: Option<&str>,
        at_ms: u64,
    ) -> Result<()>;
    /// `alarms::Index::elsewhere`, for the namespace `ns`.
    async fn alarm_elsewhere(
        &self,
        ns: &str,
        actor: &str,
        owner: &str,
        now_ms: u64,
    ) -> Result<bool>;
}

/// What every thread of an instance may read of its membership at any time,
/// and what a thread needs to open a data plane of its own. Clones share it.
#[derive(Clone)]
pub struct Shared {
    node: String,
    key: Arc<[u8]>,
    /// The lease as its keeper last renewed it: published after each
    /// renewal, so a reader sees it hold no longer than it does.
    held: Held,
    /// Set before the control plane stops claiming, and never cleared.
    draining: Arc<AtomicBool>,
    url: String,
    options: Vec<String>,
}

impl Shared {
    pub(crate) fn new(
        node: String,
        key: Vec<u8>,
        held: Held,
        url: String,
        options: Vec<String>,
    ) -> Self {
        Self {
            node,
            key: key.into(),
            held,
            draining: Arc::default(),
            url,
            options,
        }
    }

    /// This instance's id.
    pub fn node(&self) -> &str {
        &self.node
    }

    /// Whether this instance's lease holds now: an actor database opens only
    /// while it does.
    pub fn lease_holds(&self) -> bool {
        self.held.holds(now_ms())
    }

    /// Fails once this instance's lease has lapsed, as far as the calling
    /// thread can tell without asking anyone: an instance stalled past its
    /// lease may have been taken over, so running its objects, even to read
    /// their databases, could act on stale data.
    pub fn require_lease(&self) -> Result<()> {
        if self.lease_holds() {
            Ok(())
        } else {
            Err(Error::Lapsed)
        }
    }

    /// Whether the instance drains: it claims nothing more.
    pub fn is_draining(&self) -> bool {
        self.draining.load(Ordering::Acquire)
    }

    /// Marks the instance as draining. Before the control plane stops
    /// claiming, so that no thread takes a claim it was given for one it
    /// may keep.
    pub(crate) fn start_draining(&self) {
        self.draining.store(true, Ordering::Release);
    }

    /// The MAC a peer call carries (`Cluster::mac`).
    pub fn mac(&self, data: &[u8]) -> Vec<u8> {
        hmac_sha256(&self.key, data)
    }

    /// A client of the instance's bucket for the calling thread: a bucket's
    /// client belongs to the thread that made it.
    pub fn open_bucket(&self) -> Result<Bucket> {
        open_bucket(&self.url, &self.options)
    }
}

/// Claims a new object retries when another node opened it first.
const FRESH_ATTEMPTS: usize = 3;
/// How often owned objects' logs are checked for compaction.
const TIER_INTERVAL: Duration = Duration::from_secs(2);
/// How often a release checks whether an object's eviction ended.
const BUSY_POLL: Duration = Duration::from_millis(10);

/// One thread's data plane: the objects it has open, and its part in
/// claiming, taking over, committing and handing off each, which it asks its
/// instance's control plane for. See the module.
pub struct Member {
    control: Rc<dyn Control>,
    shared: Shared,
    owners: Owners,
    /// Claims won but not yet taken over, for `restore_actor`.
    claims: RefCell<HashMap<String, Owned>>,
    bucket: Bucket,
}

impl Member {
    /// A data plane over `bucket`, a client of the calling thread's, of the
    /// instance `shared` is of, whose control plane `control` reaches.
    pub fn new(bucket: Bucket, shared: Shared, control: Rc<dyn Control>) -> Self {
        Self {
            control,
            shared,
            owners: Owners::new(bucket.clone()),
            claims: RefCell::new(HashMap::new()),
            bucket,
        }
    }

    pub fn shared(&self) -> &Shared {
        &self.shared
    }

    pub fn node(&self) -> &str {
        self.shared.node()
    }

    pub fn mac(&self, data: &[u8]) -> Vec<u8> {
        self.shared.mac(data)
    }

    pub fn lease_holds(&self) -> bool {
        self.shared.lease_holds()
    }

    /// `Shared::require_lease`.
    pub fn require_lease(&self) -> Result<()> {
        self.shared.require_lease()
    }

    pub fn is_draining(&self) -> bool {
        self.shared.is_draining()
    }

    /// The bucket below `<prefix>services/<path>/`, apart from the cluster's
    /// own state (`ClusterBucket` in workerd.capnp).
    pub fn services(&self, path: &str) -> Bucket {
        self.bucket
            .at(&format!("{}services/{path}", self.bucket.prefix()))
    }

    pub async fn acquire_or_route(&self, ns: &str, actor: &str, now_ms: u64) -> Result<Route> {
        let mut claim = self.control.claim(ns, actor, now_ms).await?;
        // Named open here, but not open here. Evicted here with the bucket
        // holding the epoch: resume it (`take_over`); a stale owner that
        // writes on cannot acknowledge, since every acknowledgement rereads
        // own.json. Otherwise, dropped after a failure or evicted before its
        // upload completed: recover it at the next epoch.
        if let Claim::Owned(owned) = &claim
            && owned.recover.is_none()
            && !self.owners.holds(ns, actor)
            && self.owners.dormant(ns, actor) != Some(owned.epoch)
        {
            let next = self.control.advance(ns, actor, owned.epoch).await?;
            // Changed meanwhile, by another claim here: claim what it left.
            claim = match next {
                Some(next) => Claim::Owned(next),
                None => self.control.claim(ns, actor, now_ms).await?,
            };
        }
        // A new object opens here, where losing the race still routes.
        for _ in 0..FRESH_ATTEMPTS {
            let Claim::Owned(owned) = &claim else { break };
            if owned.recover.is_none_or(|from| from != 0)
                || self
                    .owners
                    .open_fresh(&*self.control, ns, actor, owned)
                    .await?
            {
                break;
            }
            claim = self.control.claim(ns, actor, now_ms).await?;
        }
        if matches!(claim, Claim::Owned(_)) && self.is_draining() {
            return Err(Error::NotHere(format!(
                "{ns}/{actor}: this node is draining"
            )));
        }
        Ok(match claim {
            Claim::Owned(owned) => {
                let epoch = owned.epoch;
                let key = format!("{ns}/{actor}");
                self.claims.borrow_mut().insert(key, owned);
                Route::Owned(epoch)
            }
            Claim::Route {
                node,
                address,
                stale,
            } => Route::Remote {
                node,
                address,
                stale,
            },
            Claim::Releasing(epoch) => Route::Releasing(epoch),
        })
    }

    /// Hand off an object this draining instance stopped, whose capture
    /// `close_actor` closed: upload what the bucket lacks, then write the
    /// owner record as unowned (`Ownership::release`). Nothing writes the
    /// object here any more, so no write can follow the release; the next
    /// owner's epoch fences anything else. Answers whether it was released.
    pub async fn hand_off_actor(&self, ns: &str, actor: &str, epoch: u64) -> Result<bool> {
        self.owners.retire(ns, actor);
        self.owners.evict(ns, actor, epoch).await;
        self.release_actor(ns, actor, epoch).await
    }

    /// Hand off an object this draining instance owns but does not serve,
    /// once no capture, eviction or compaction of it is in flight here: the
    /// next owner reads the files a compaction deletes. The caller bounds
    /// the wait.
    pub async fn release_actor(&self, ns: &str, actor: &str, epoch: u64) -> Result<bool> {
        while self.owners.busy(ns, actor) {
            tokio::time::sleep(BUSY_POLL).await;
        }
        let released = self.control.release(ns, actor, epoch).await?;
        if released {
            self.owners.forget(ns, actor);
            tracing::info!("cluster: {ns}/{actor} epoch {epoch} handed off");
        }
        Ok(released)
    }

    /// Gives up on handing off an object this draining instance failed to
    /// release (`Ownership::abandon`).
    pub fn abandon_actor(&self, ns: &str, actor: &str) {
        self.control.abandon(ns, actor);
    }

    /// Take a claimed object over: recover what earlier epochs committed
    /// into the databases `layout` places, and open the claimed epoch.
    pub async fn restore_actor(
        &self,
        ns: &str,
        actor: &str,
        epoch: u64,
        layout: &dyn Layout,
    ) -> Result<()> {
        let claim = self.claims.borrow_mut().remove(&format!("{ns}/{actor}"));
        let owned = claim
            .filter(|owned| owned.epoch == epoch)
            .ok_or_else(|| Error::Replica(format!("{ns}/{actor}: no claim at epoch {epoch}")))?;
        self.owners
            .take_over(&*self.control, ns, actor, &owned, layout)
            .await
    }

    /// Make an object's captured commits durable in the bucket.
    pub async fn sync_actor(&self, ns: &str, actor: &str) -> Result<()> {
        self.owners.commit(&*self.control, ns, actor).await
    }

    /// Stop capturing an object that left this node.
    pub fn close_actor(&self, ns: &str, actor: &str) {
        self.owners.retire(ns, actor);
    }

    /// Capture the database at `path` of an owned object's facet, lane
    /// `lane`, before it opens.
    pub fn open_lane(&self, ns: &str, actor: &str, path: &str, lane: &str) -> Result<()> {
        self.owners.open_lane(ns, actor, Path::new(path), lane)
    }

    /// An owned object's facet with lane prefix `prefix` and its
    /// descendants were deleted.
    pub fn delete_lanes(&self, ns: &str, actor: &str, prefix: &str) -> Result<()> {
        self.owners.delete_lanes(ns, actor, prefix)
    }

    /// An owned object's facet with lane prefix `dst` and its descendants
    /// were replaced by copies of the facet with prefix `src`'s.
    pub fn clone_lanes(
        &self,
        ns: &str,
        actor: &str,
        src: &str,
        dst: &str,
        layout: &dyn Layout,
    ) -> Result<()> {
        self.owners.clone_lanes(ns, actor, src, dst, layout)
    }

    /// One of namespace `ns`'s databases was deleted, before anything
    /// writes a new one.
    pub fn database_reset(&self, ns: &str, file: &str) {
        let (actor, file) = object_file(file);
        self.owners.database_reset(ns, actor, file);
    }

    /// A write SQLite made to a WAL file in namespace `ns`'s storage.
    pub fn wal_write(&self, ns: &str, file: &str, offset: u64, data: &[u8]) {
        if let Some(db) = file.strip_suffix("-wal") {
            let (actor, db) = object_file(db);
            self.owners.wal_write(ns, actor, db, offset, data);
        }
    }

    /// Compact owned objects' logs in the bucket.
    pub async fn tier(&self) {
        self.owners.tier().await;
    }

    /// Compacts owned objects' logs. Never returns.
    pub async fn tier_loop(&self) {
        loop {
            tokio::time::sleep(TIER_INTERVAL).await;
            self.tier().await;
        }
    }

    /// Keep an object workerd evicted, uploading what the bucket lacks.
    pub async fn evict_actor(&self, ns: &str, actor: &str, epoch: u64) {
        self.owners.evict(ns, actor, epoch).await;
    }

    /// Index an owned object's alarm, `None` once cleared, so that peers
    /// wake the object if this node dies (`alarms::Index::arm`).
    pub async fn arm_alarm(
        &self,
        ns: &str,
        actor: &str,
        name: Option<&str>,
        at_ms: Option<u64>,
    ) -> Result<()> {
        self.control.arm_alarm(ns, actor, name, at_ms).await
    }

    /// Index the alarm an owned object came with as it opened here
    /// (`alarms::Index::opened`).
    pub async fn opened_alarm(
        &self,
        ns: &str,
        actor: &str,
        name: Option<&str>,
        at_ms: u64,
    ) -> Result<()> {
        self.control.opened_alarm(ns, actor, name, at_ms).await
    }

    /// An indexed alarm fired here for an object `owner` owns:
    /// whether to fire it again later (`alarms::Index::elsewhere`).
    pub async fn alarm_elsewhere(
        &self,
        ns: &str,
        actor: &str,
        owner: &str,
        now_ms: u64,
    ) -> Result<bool> {
        self.control.alarm_elsewhere(ns, actor, owner, now_ms).await
    }

    pub async fn refresh_route(&self, ns: &str, actor: &str, now_ms: u64) -> Result<()> {
        self.control.refresh_route(ns, actor, now_ms).await
    }

    /// `node` answered that it does not serve the object (`Ownership::forget_route`).
    pub fn forget_route(&self, ns: &str, actor: &str, node: &str) {
        self.control.forget_route(ns, actor, node);
    }
}

/// The object and file name of a database file, `<actor>.sqlite` or a
/// facet's `<actor>.<n>.sqlite`, under whatever directory SQLite was given.
fn object_file(path: &str) -> (&str, &str) {
    let name = path.rsplit(['/', '\\']).next().unwrap_or(path);
    (name.split('.').next().unwrap_or(name), name)
}

#[cfg(test)]
#[path = "control-test.rs"]
pub mod tests;
