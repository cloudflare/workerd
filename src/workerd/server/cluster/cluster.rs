// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! One instance's view of the cluster: what the server calls. Runs on the
//! server's loop thread, inside its tokio `current_thread` runtime.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::path::Path;
use std::rc::Rc;
use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use cluster_logic::lease;
use cluster_logic::lease::Lease;
use cluster_logic::lease::Step;
use tokio::time::timeout;

use crate::Bucket;
use crate::Claim;
use crate::Error;
use crate::Guard;
use crate::Ownership;
use crate::Registry;
use crate::Result;
use crate::Version;
use crate::alarms::Adopted;
use crate::alarms::Index;
use crate::hmac_sha256;
use crate::namespace_hash;
use crate::owner::Owners;
use crate::ownership::Owned;
use crate::s3::S3;

/// The bucket layout's version, the last element of every key's prefix.
const FORMAT: &str = "v1";

/// Claims a new object retries when another node opened it first.
const FRESH_ATTEMPTS: usize = 3;

/// How often each node looks for alarms whose node has no record
/// (`Cluster::orphans`): one delimited LIST of `alarms/`.
const ORPHANS_EVERY_MS: u64 = 60 * 60 * 1000;

pub struct Setup {
    /// `s3://<bucket>/<prefix>`.
    pub url: String,
    /// `name=value` bucket options (see `S3::new`).
    pub options: Vec<String>,
    /// This instance's identity, fresh per process.
    pub node: String,
    /// Where peers reach this instance: `host:port`.
    pub address: String,
    pub ttl_ms: u64,
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
}

pub struct Cluster {
    registry: Rc<Registry>,
    ownership: Ownership,
    owners: Owners,
    /// Claims won but not yet taken over, for `restore_actor`.
    claims: RefCell<HashMap<String, Owned>>,
    bucket: Bucket,
    key: Vec<u8>,
    lease: RefCell<Lease>,
    alarms: Index,
    /// When the next round looks for orphaned alarms.
    orphans_at: Cell<u64>,
}

impl Cluster {
    pub fn new(setup: Setup) -> Result<Rc<Self>> {
        let options: Vec<_> = setup
            .options
            .iter()
            .filter_map(|option| option.split_once('='))
            .map(|(key, value)| (key.to_owned(), value.to_owned()))
            .collect();
        let (s3, prefix) = S3::new(&setup.url, &options)?;
        let bucket = Bucket::new(s3, &format!("{prefix}/{FORMAT}"));
        let registry = Rc::new(Registry::new(
            bucket.clone(),
            &setup.node,
            &setup.address,
            setup.ttl_ms,
        ));
        Ok(Rc::new(Self {
            alarms: Index::new(bucket.clone(), &setup.node, 2 * setup.ttl_ms),
            orphans_at: Cell::new(0),
            owners: Owners::new(bucket.clone()),
            ownership: Ownership::new(bucket.clone(), registry.clone()),
            bucket,
            registry,
            claims: RefCell::new(HashMap::new()),
            key: setup.key,
            lease: RefCell::new(Lease::default()),
        }))
    }

    pub fn node(&self) -> &str {
        self.registry.node()
    }

    pub async fn renew(&self, now_ms: u64) -> Result<u64> {
        self.registry.renew(now_ms).await
    }

    /// How far this node's wall clock is outside the bucket's, in ms; 0
    /// before any response.
    pub fn clock_skew_ms(&self) -> i64 {
        self.bucket.clock_skew_ms().unwrap_or(0)
    }

    pub async fn deregister(&self) -> Result<()> {
        self.registry.deregister().await
    }

    /// Live peers' records, as the bucket lists them now.
    pub async fn peers(&self, now_ms: u64) -> Result<Vec<crate::NodeRecord>> {
        self.registry.peers(now_ms).await
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
        })
    }

    /// Take a claimed object over: recover what earlier epochs committed
    /// into the database at `path`, and open the claimed epoch.
    pub async fn restore_actor(&self, ns: &str, actor: &str, epoch: u64, path: &str) -> Result<()> {
        let claim = self.claims.borrow_mut().remove(&format!("{ns}/{actor}"));
        let owned = claim.filter(|owned| owned.epoch == epoch).ok_or_else(|| {
            crate::Error::Replica(format!("{ns}/{actor}: no claim at epoch {epoch}"))
        })?;
        self.owners
            .take_over(&self.ownership, ns, actor, &owned, Path::new(path))
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

    /// An owned object's database was deleted, before anything writes a
    /// new one.
    pub fn database_reset(&self, ns: &str, actor: &str) {
        self.owners.database_reset(ns, actor);
    }

    /// A write SQLite made to a WAL file in namespace `ns`'s storage.
    pub fn wal_write(&self, ns: &str, file: &str, offset: u64, data: &[u8]) {
        // `<actor>.sqlite-wal`, under whatever directory SQLite was given.
        let name = file.rsplit('/').next().unwrap_or(file);
        if let Some(actor) = name.strip_suffix(".sqlite-wal") {
            self.owners.wal_write(ns, actor, offset, data);
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

    /// Take this node's share of dead nodes' alarms, handing `adopted` each
    /// to schedule here, then bring this node's entries in line with its
    /// alarms (`alarms::Index::tidy`).
    pub async fn alarms_round(&self, now_ms: u64, adopted: impl FnMut(Adopted)) {
        let live = self.registry.listed_nodes(now_ms);
        let mut dead = self.registry.dead();
        if !live.is_empty() && now_ms >= self.orphans_at.get() {
            self.orphans_at.set(now_ms + ORPHANS_EVERY_MS);
            match self.orphans(&live, &dead).await {
                Ok(orphans) => dead.extend(orphans),
                Err(e) => tracing::warn!("looking for orphaned alarms: {e}"),
            }
        }
        self.alarms.adopt(&live, &dead, adopted).await;
        self.alarms.tidy().await;
    }

    /// Nodes with alarms in the bucket but no record, which survivors find
    /// through no listing of `nodes/`. A node publishes its record before
    /// it indexes an alarm, and the registry keeps a dead node's record
    /// while it has alarms; this is the backstop for one gone regardless.
    async fn orphans(&self, live: &[String], dead: &[String]) -> Result<Vec<String>> {
        let known: Vec<_> = live.iter().chain(dead).cloned().collect();
        let mut orphans = Vec::new();
        for node in self.alarms.unknown(&known).await? {
            if !self.registry.recorded(&node).await? {
                tracing::warn!("adopting the alarms of {node}, which has no record");
                orphans.push(node);
            }
        }
        Ok(orphans)
    }

    pub async fn refresh_route(&self, ns: &str, actor: &str, now_ms: u64) -> Result<()> {
        self.ownership.refresh_route(ns, actor, now_ms).await
    }

    /// The MAC a peer call carries. The cluster key is used only through
    /// labelled derivations, so no two uses share a key: this, with each
    /// message labelled, and the channel token key.
    pub fn mac(&self, data: &[u8]) -> Vec<u8> {
        hmac_sha256(&self.key, data)
    }
}

impl Cluster {
    /// Keeps this node's lease, until the node must stop
    /// serving: answers why. Runs for the life of the process.
    pub async fn maintain(&self) -> String {
        loop {
            if self.lease.borrow().draining {
                std::future::pending::<()>().await;
            }
            let observed = lease::Observed {
                now_ms: now_ms(),
                skew_ms: self.clock_skew_ms(),
            };
            let mut step = self.lease.borrow().before_renew(&observed);
            if step == Step::Renew {
                let renew = timeout(ms(lease::RENEW_TIMEOUT_MS), self.renew(now_ms()));
                let result = match renew.await {
                    Ok(Ok(until)) => Ok(until),
                    Ok(Err(e)) => Err(e.to_string()),
                    Err(_) => Err("renewal timed out".into()),
                };
                // A drain began meanwhile, and its deregistration may have
                // failed this renewal: what it found no longer matters.
                if self.lease.borrow().draining {
                    continue;
                }
                step = self.lease.borrow_mut().renewed(now_ms(), result);
            }
            let delay = match step {
                Step::Fence(reason) => return reason,
                Step::Retry(reason) => {
                    tracing::warn!("renewing the cluster lease failed; retrying: {reason}");
                    lease::RETRY_MS
                }
                Step::Renew => lease::INTERVAL_MS,
            };
            // A stale listing only delays routing to a new peer: its
            // failure is logged, never fatal.
            match timeout(ms(lease::LIST_TIMEOUT_MS), self.peers(now_ms())).await {
                Ok(Ok(_)) => {}
                Ok(Err(e)) => tracing::warn!("listing the cluster failed: {e}"),
                Err(_) => tracing::warn!("listing the cluster timed out"),
            }
            tokio::time::sleep(ms(delay)).await;
        }
    }

    /// Compacts owned objects' logs. Never returns.
    pub async fn tier_loop(&self) {
        loop {
            tokio::time::sleep(TIER_INTERVAL).await;
            self.tier().await;
        }
    }

    /// Stops claiming before the claim reaches the bucket, and parks the
    /// lease loop; then `deregister`.
    pub fn start_draining(&self) {
        self.lease.borrow_mut().draining = true;
        self.ownership.may_claim.set(false);
    }

    /// The first renewal, before the node serves.
    pub async fn join(&self) -> Result<()> {
        check_conditional_writes(&self.bucket, self.node()).await?;
        let until = self.renew(now_ms()).await?;
        self.lease.borrow_mut().renewed(now_ms(), Ok(until));
        Ok(())
    }

    /// Whether this node's lease holds now: an actor database opens only
    /// while it does.
    pub fn lease_holds(&self) -> bool {
        self.lease.borrow().holds(now_ms())
    }
}

/// How often owned objects' logs are checked for compaction.
const TIER_INTERVAL: Duration = Duration::from_secs(2);

const fn ms(ms: u64) -> Duration {
    Duration::from_millis(ms)
}

pub fn now_ms() -> u64 {
    let since = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    u64::try_from(since.as_millis()).unwrap_or(u64::MAX)
}

/// Refuse a store that ignores write preconditions: fencing rests on
/// them. GCS's S3 API, for one, ignores `If-Match` on a PUT.
async fn check_conditional_writes(bucket: &Bucket, node: &str) -> Result<()> {
    let key = format!("nodes/{node}.probe");
    let stale = Guard::Match(Version("\"0\"".into()));
    let created = bucket.put_if(&key, vec![], Guard::Absent).await?;
    let again = bucket.put_if(&key, vec![], Guard::Absent).await?;
    let replaced = bucket.put_if(&key, vec![], stale).await?;
    bucket.delete(&key).await?;
    if created.is_none() || again.is_some() || replaced.is_some() {
        return Err(Error::Config(
            "the bucket ignores conditional writes (If-None-Match, If-Match)".into(),
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use futures::executor::block_on;

    use super::check_conditional_writes;
    use crate::Bucket;
    use crate::bucket::memory::Memory;

    #[test]
    fn a_bucket_that_ignores_preconditions_is_refused() {
        block_on(async {
            assert!(
                check_conditional_writes(&Bucket::memory(""), "n")
                    .await
                    .is_ok()
            );
            let lax = Memory::default();
            lax.ignores_guards.set(true);
            let lax = Bucket::new(lax, "");
            assert!(check_conditional_writes(&lax, "n").await.is_err());
        });
    }
}
