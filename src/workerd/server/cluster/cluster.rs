// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! One instance's view of the cluster: what the server calls.
//!
//! Runs on the server's loop thread, inside its tokio `current_thread`
//! runtime, but for the lease, which a thread of its own keeps
//! (`keeper.rs`).

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::path::Path;
use std::rc::Rc;
use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use cluster_logic::Membership;
use cluster_logic::lease::Timing;
use tokio::time::timeout;

use crate::Bucket;
use crate::Claim;
use crate::Error;
use crate::Guard;
use crate::Ownership;
use crate::Registry;
use crate::Result;
use crate::alarms::Adopted;
use crate::alarms::Index;
use crate::gcs::Gcs;
use crate::hmac_sha256;
use crate::http::Repeat;
use crate::http::prompt;
use crate::keeper;
use crate::keeper::Fence;
#[cfg(test)]
use crate::keeper::Keeper;
use crate::keeper::Lessee;
use crate::keeper::Progress;
use crate::keeper::Within;
use crate::namespace_hash;
use crate::owner::Layout;
use crate::owner::Owners;
use crate::ownership::Owned;
use crate::s3::S3;

/// The bucket layout's version, the last element of every key's prefix.
const FORMAT: &str = "v1";

/// Claims a new object retries when another node opened it first.
const FRESH_ATTEMPTS: usize = 3;

/// How often each node looks for orphaned alarms (`alarms::Index::round`).
const ORPHANS_EVERY_MS: u64 = 60 * 60 * 1000;

pub struct Setup {
    /// `s3://<bucket>/<prefix>` or `gs://<bucket>/<prefix>`.
    pub url: String,
    /// `name=value` bucket options (see `S3::new` and `Gcs::new`).
    pub options: Vec<String>,
    /// This instance's identity, fresh per process.
    pub node: String,
    /// Where peers reach this instance: `host:port`.
    pub address: String,
    /// This instance's lease: `Timing::DEFAULT`, unless a test shortens it.
    pub timing: Timing,
    /// The cluster secret: MACs every peer call.
    pub key: Vec<u8>,
}

/// Where a Durable Object runs.
pub enum Route {
    /// Here, at this ownership epoch: take it over with `restore_actor`.
    Owned(u64),
    /// On another instance. `stale`: the record names a node that looks
    /// dead, so the caller should `refresh_route` in the background.
    Remote {
        node: String,
        address: String,
        stale: bool,
    },
    /// Here at this epoch, but this instance drains: `release_actor`, then
    /// route the request again, to whoever claims the object.
    Releasing(u64),
}

pub struct Cluster {
    registry: Rc<Registry>,
    ownership: Ownership,
    owners: Owners,
    /// Claims won but not yet taken over, for `restore_actor`.
    claims: RefCell<HashMap<String, Owned>>,
    bucket: Bucket,
    key: Vec<u8>,
    lessee: Lessee,
    timing: Timing,
    /// When the registration, `join`'s renewal, answered.
    registered: Cell<Option<u64>>,
    alarms: Index,
}

impl Cluster {
    /// This instance's part in the cluster `setup` names, its lease kept on
    /// a thread of its own, which runs `within` the process's setup for its
    /// threads. `fence` ends the process once the lease cannot be kept, or
    /// once the loop it watches (`watch`) makes no progress.
    pub fn new(setup: Setup, fence: Fence, within: Within) -> Result<Rc<Self>> {
        let bucket = open_bucket(&setup.url, &setup.options)?;
        // The lease thread's client is its own: a bucket's client belongs
        // to the thread that made it.
        let (url, options) = (setup.url.clone(), setup.options.clone());
        let open = move || open_bucket(&url, &options);
        Self::threaded(bucket, open, setup, fence, within)
    }

    /// Over `bucket`, its lease kept on a thread of its own over the bucket
    /// `open` opens there.
    fn threaded(
        bucket: Bucket,
        open: impl FnOnce() -> Result<Bucket> + Send + 'static,
        setup: Setup,
        fence: Fence,
        within: Within,
    ) -> Result<Rc<Self>> {
        let lessee = Lessee::Thread(keeper::Thread::spawn(
            open,
            setup.node.clone(),
            setup.address.clone(),
            setup.timing,
            fence,
            within,
        )?);
        Ok(Self::with(bucket, setup, |_| lessee))
    }

    /// Over `bucket`, not the one `setup` names, its lease kept by whoever
    /// calls `renew`.
    #[cfg(test)]
    pub(crate) fn over(bucket: Bucket, setup: Setup) -> Rc<Self> {
        let timing = setup.timing;
        Self::with(bucket.clone(), setup, |registry| {
            Lessee::Here(Keeper::new(bucket, Rc::clone(registry), timing))
        })
    }

    fn with(
        bucket: Bucket,
        setup: Setup,
        lessee: impl FnOnce(&Rc<Registry>) -> Lessee,
    ) -> Rc<Self> {
        let registry = Registry::new(
            bucket.clone(),
            &setup.node,
            &setup.address,
            setup.timing.lease_ms,
        );
        let registry = Rc::new(registry.contended_pause(ms(setup.timing.contended_pause_ms)));
        Rc::new(Self {
            alarms: Index::new(
                bucket.clone(),
                &setup.node,
                2 * setup.timing.lease_ms,
                ORPHANS_EVERY_MS,
            ),
            owners: Owners::new(bucket.clone()),
            ownership: Ownership::new(bucket.clone(), registry.clone()),
            lessee: lessee(&registry),
            bucket,
            registry,
            claims: RefCell::new(HashMap::new()),
            key: setup.key,
            timing: setup.timing,
            registered: Cell::new(None),
        })
    }

    /// The bucket below `<prefix>services/<path>/`, apart from the cluster's
    /// own state (`ClusterBucket` in workerd.capnp).
    pub fn services(&self, path: &str) -> Bucket {
        self.bucket
            .at(&format!("{}services/{path}", self.bucket.prefix()))
    }

    pub fn node(&self) -> &str {
        self.registry.node()
    }

    /// Renews this node's record as of `now_ms`, on this thread.
    #[cfg(test)]
    pub(crate) async fn renew(&self, now_ms: u64) -> Result<u64> {
        self.registry.renew(now_ms).await
    }

    /// Expires this instance's membership: it renews its lease no more,
    /// and peers take over what it still owns and adopt its alarms.
    pub async fn deregister(&self) -> Result<()> {
        self.registry.leave();
        self.lessee.deregister().await
    }

    /// Live peers' records, as the bucket lists them now.
    pub async fn peers(&self, now_ms: u64) -> Result<Vec<crate::NodeRecord>> {
        self.registry.peers(now_ms).await
    }

    /// Where `node` listens, while the listing has it live.
    pub fn address(&self, node: &str, now_ms: u64) -> Option<String> {
        self.registry
            .listed_live(node, now_ms)
            .then(|| self.registry.address(node))?
    }

    pub async fn acquire_or_route(&self, ns: &str, actor: &str, now_ms: u64) -> Result<Route> {
        let mut claim = self.ownership.acquire_or_route(ns, actor, now_ms).await?;
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
            let next = self.ownership.advance(ns, actor, owned.epoch).await?;
            // Changed meanwhile, by another claim here: claim what it left.
            claim = match next {
                Some(next) => Claim::Owned(next),
                None => self.ownership.acquire_or_route(ns, actor, now_ms).await?,
            };
        }
        // A new object opens here, where losing the race still routes.
        for _ in 0..FRESH_ATTEMPTS {
            let Claim::Owned(owned) = &claim else { break };
            if owned.recover.is_none_or(|from| from != 0)
                || self
                    .owners
                    .open_fresh(&self.ownership, ns, actor, owned)
                    .await?
            {
                break;
            }
            claim = self.ownership.acquire_or_route(ns, actor, now_ms).await?;
        }
        if matches!(claim, Claim::Owned(_)) && self.is_draining() {
            return Err(crate::Error::NotHere(format!(
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
        let released = self.ownership.release(ns, actor, epoch).await?;
        if released {
            self.owners.forget(ns, actor);
            tracing::info!("cluster: {ns}/{actor} epoch {epoch} handed off");
        }
        Ok(released)
    }

    /// Gives up on handing off an object this draining instance failed to
    /// release (`Ownership::abandon`).
    pub fn abandon_actor(&self, ns: &str, actor: &str) {
        self.ownership.abandon(ns, actor);
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
        let owned = claim.filter(|owned| owned.epoch == epoch).ok_or_else(|| {
            crate::Error::Replica(format!("{ns}/{actor}: no claim at epoch {epoch}"))
        })?;
        self.owners
            .take_over(&self.ownership, ns, actor, &owned, layout)
            .await
    }

    /// Make an object's captured commits durable in the bucket.
    pub async fn sync_actor(&self, ns: &str, actor: &str) -> Result<()> {
        self.owners.commit(&self.ownership, ns, actor).await
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
        self.alarms
            .arm(&namespace_hash(ns), actor, name, at_ms)
            .await
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
        self.alarms
            .opened(&namespace_hash(ns), actor, name, at_ms)
            .await
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
        self.alarms
            .elsewhere(&namespace_hash(ns), actor, owner, now_ms)
            .await
    }

    /// One round of the alarms loop (`alarms::Index::round`).
    pub async fn alarms_round(&self, now_ms: u64, adopted: impl FnMut(Adopted)) {
        self.alarms.round(&self.registry, now_ms, adopted).await;
    }

    pub async fn refresh_route(&self, ns: &str, actor: &str, now_ms: u64) -> Result<()> {
        self.ownership.refresh_route(ns, actor, now_ms).await
    }

    /// `node` answered that it does not serve the object (`Ownership::forget_route`).
    pub fn forget_route(&self, ns: &str, actor: &str, node: &str) {
        self.ownership.forget_route(ns, actor, node);
    }

    /// The MAC a peer call carries. The cluster key is used only through
    /// labelled derivations, so no two uses share a key: this, with each
    /// message labelled, and the channel token key.
    pub fn mac(&self, data: &[u8]) -> Vec<u8> {
        hmac_sha256(&self.key, data)
    }
}

impl Cluster {
    /// Starts keeping this node's lease, for the life of the process: the
    /// lease thread renews it from here on, and ends the process once it
    /// cannot (`keeper::Fence`).
    fn keep_lease(&self) {
        match &self.lessee {
            Lessee::Thread(thread) => thread.maintain(),
            #[cfg(test)]
            Lessee::Here(_) => {}
        }
    }

    /// Lists the peers every `Timing::interval_ms` until this instance
    /// leaves the cluster. A stale listing only delays routing to a new
    /// peer, so the listing shares the loop with everything else, and its
    /// failure is logged, never fatal. Never returns.
    pub async fn list_loop(&self) {
        let timing = self.timing;
        loop {
            if self.registry.left() {
                std::future::pending::<()>().await;
            }
            match timeout(ms(timing.list_timeout_ms), self.list_round(&timing)).await {
                Ok(Ok(_)) => {}
                Ok(Err(e)) => tracing::warn!("listing the cluster failed: {e}"),
                Err(_) => tracing::warn!("listing the cluster timed out"),
            }
            tokio::time::sleep(ms(timing.interval_ms)).await;
        }
    }

    /// Compacts owned objects' logs. Never returns.
    pub async fn tier_loop(&self) {
        loop {
            tokio::time::sleep(TIER_INTERVAL).await;
            self.tier().await;
        }
    }

    /// Stops claiming before the claim reaches the bucket. The lease is
    /// still renewed, so that the objects this instance owns stay where
    /// they are until it hands them off; then `deregister`.
    pub fn start_draining(&self) {
        self.ownership.may_claim.set(false);
        self.lessee.start_draining();
    }

    /// Publishes that this instance drains, so that peers choose it for no
    /// object or alarm, and lists the peers, so that it chooses none that
    /// drains too. Peers that list it before learn of it from its answers.
    pub async fn announce_draining(&self) {
        self.lessee.announce_draining().await;
        let timing = self.timing;
        let _ = timeout(ms(timing.list_timeout_ms), self.list_round(&timing)).await;
    }

    /// A listing of the peers, its stalled requests sent again as a lease
    /// round's renewal's are (`Keeper::renew_round`), after a shorter wait.
    /// Its deletions may be: it deletes only long-dead node records, whose
    /// node ids no process takes again (`Registry::fetch`).
    async fn list_round(&self, timing: &Timing) -> Result<Vec<crate::NodeRecord>> {
        let repeat = &[Repeat::Read, Repeat::Delete];
        prompt(ms(timing.list_attempt_ms), repeat, self.peers(now_ms())).await
    }

    /// Whether `start_draining` ran: this node claims nothing more.
    pub fn is_draining(&self) -> bool {
        !self.ownership.may_claim.get()
    }

    /// The first renewal, before the node links its services, and the
    /// start of the rounds that keep the lease (`keep_lease`). Linking opens
    /// every namespace's storage, which opens only under a lease, and on a
    /// slow disk takes longer than one: the lease thread renews it
    /// meanwhile, whatever turns linking takes.
    pub async fn join(&self) -> Result<()> {
        let probe = format!("nodes/{}.probe", self.node());
        check_conditional_writes(&self.bucket, &probe).await?;
        self.lessee.take_lease().await?;
        self.registered.set(Some(now_ms()));
        self.keep_lease();
        Ok(())
    }

    /// From here on, the lease thread watches the event loop that
    /// `progress` counts, and fences the node once it makes no progress for
    /// `Timing::stall_ms`. For once the node serves: linking, after `join`,
    /// runs in turns as long as itself.
    pub fn watch(&self, progress: Progress) {
        self.lessee.watch(progress);
    }

    /// Whether this node's lease holds now: an actor database opens only
    /// while it does.
    pub fn lease_holds(&self) -> bool {
        self.lessee.held().holds(now_ms())
    }

    /// What this node knows of the membership, for what routes by it.
    pub fn membership(&self) -> Membership {
        Membership {
            listing: self.registry.listing(),
            registered: self.registered.get(),
            lease_until: self.lessee.held().holds_until(),
            draining: self.is_draining(),
        }
    }
}

/// How often owned objects' logs are checked for compaction.
const TIER_INTERVAL: Duration = Duration::from_secs(2);
/// How often a release checks whether an object's eviction ended.
const BUSY_POLL: Duration = Duration::from_millis(10);

/// The cluster's part of the bucket `url` names, under the layout's version.
///
/// `url` is `s3://<bucket>/<prefix>` (`s3.rs`) or `gs://<bucket>/<prefix>`
/// (`gcs.rs`); every key of the layout is relative to what this answers.
/// `options` are `name=value` (see `S3::new` and `Gcs::new`).
pub fn open_bucket(url: &str, options: &[String]) -> Result<Bucket> {
    let options: Vec<_> = options
        .iter()
        .filter_map(|option| option.split_once('='))
        .map(|(key, value)| (key.to_owned(), value.to_owned()))
        .collect();
    if url.starts_with("gs://") {
        let (gcs, prefix) = Gcs::new(url, &options)?;
        return Ok(Bucket::new(gcs, &format!("{prefix}/{FORMAT}")));
    }
    if !url.starts_with("s3://") {
        return Err(Error::Config(format!(
            "{url}: expected s3://<bucket>/<prefix> or gs://<bucket>/<prefix>"
        )));
    }
    let (s3, prefix) = S3::new(url, &options)?;
    Ok(Bucket::new(s3, &format!("{prefix}/{FORMAT}")))
}

const fn ms(ms: u64) -> Duration {
    Duration::from_millis(ms)
}

/// The object and file name of a database file, `<actor>.sqlite` or a
/// facet's `<actor>.<n>.sqlite`, under whatever directory SQLite was given.
fn object_file(path: &str) -> (&str, &str) {
    let name = path.rsplit(['/', '\\']).next().unwrap_or(path);
    (name.split('.').next().unwrap_or(name), name)
}

pub fn now_ms() -> u64 {
    let since = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    u64::try_from(since.as_millis()).unwrap_or(u64::MAX)
}

/// Refuse a store that ignores write preconditions: fencing and the
/// deployment catalog rest on them. GCS's S3 API, for one, ignores
/// `If-Match` on a PUT. `key` is an object only this check writes. Each
/// write's body differs, so that no two writes could share an S3 `ETag`,
/// and so that what the object holds says which write an unknown outcome
/// was (`Bucket::put_if_settled`).
pub(crate) async fn check_conditional_writes(bucket: &Bucket, key: &str) -> Result<()> {
    let put = |body: &[u8], guard| bucket.put_if_settled(key, body.to_vec(), guard);
    let created = put(b"1", Guard::Absent).await?;
    let again = put(b"2", Guard::Absent).await?;
    let (replaced, stale) = match &created {
        Some(first) => {
            let replaced = put(b"3", Guard::Match(first.clone())).await?;
            let stale = put(b"4", Guard::Match(first.clone())).await?;
            (replaced, stale)
        }
        None => (None, None),
    };
    bucket.delete(key).await?;
    if again.is_some() || stale.is_some() {
        return Err(Error::Config(
            "the bucket ignores conditional writes (If-None-Match and If-Match on S3, \
             x-goog-if-generation-match on Cloud Storage)"
                .into(),
        ));
    }
    // A store that compares versions as it should, but answers ones that
    // never match: a weak `ETag` (`W/"..."`) fails `If-Match`'s strong
    // comparison.
    if created.is_none() || replaced.is_none() {
        return Err(Error::Config(
            "the bucket refused a conditional write whose condition held: creating an absent \
             object, or replacing the version the bucket had just answered for it"
                .into(),
        ));
    }
    Ok(())
}

#[cfg(test)]
#[path = "cluster-test.rs"]
mod tests;
