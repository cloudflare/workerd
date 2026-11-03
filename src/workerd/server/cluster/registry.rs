// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;

use serde::Deserialize;
use serde::Serialize;

use crate::Bucket;
use crate::Error;
use crate::Guard;
use crate::Result;
use crate::Version;

/// One instance's record in `nodes/<node>.json`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct NodeRecord {
    /// The instance's identity: 16 random bytes, hex, new each start.
    pub node: String,
    /// Where peers reach it, `host:port`.
    pub address: String,
    /// The instance is live until this time and must renew before it.
    pub expires_ms: u64,
    /// The process that wrote it.
    #[serde(default)]
    pub instance: u64,
    /// The instance is stopping: it hands its objects off, and peers
    /// choose it for none.
    #[serde(default)]
    pub draining: bool,
}

/// This instance's membership: it publishes its own record and reads its
/// peers'. A peer whose lease expired is dead; nothing else is.
pub struct Registry {
    bucket: Bucket,
    node: String,
    address: String,
    ttl_ms: u64,
    /// The version of our own record we last wrote, so a renewal replaces
    /// exactly that and notices anyone else's write.
    own: RefCell<Option<Version>>,
    /// Random per process: a renewal whose answer was lost may still have
    /// landed, and this tells that write from another writer's.
    instance: u64,
    /// Live peers' records, as of the last listing.
    listed: RefCell<HashMap<String, NodeRecord>>,
    /// Dead peers whose records remain, as of the last listing.
    dead: RefCell<Vec<String>>,
    /// This instance left the cluster: it renews its record no more.
    left: Cell<bool>,
    /// This instance drains, which its record says from its next renewal.
    draining: Cell<bool>,
}

fn node_key(node: &str) -> String {
    format!("nodes/{node}.json")
}

impl Registry {
    pub fn new(bucket: Bucket, node: &str, address: &str, ttl_ms: u64) -> Self {
        Self {
            bucket,
            node: node.to_owned(),
            address: address.to_owned(),
            ttl_ms,
            own: RefCell::new(None),
            instance: std::hash::BuildHasher::hash_one(
                &std::collections::hash_map::RandomState::new(),
                std::time::SystemTime::now(),
            ),
            listed: RefCell::new(HashMap::new()),
            dead: RefCell::new(Vec::new()),
            left: Cell::new(false),
            draining: Cell::new(false),
        }
    }

    pub fn node(&self) -> &str {
        &self.node
    }

    /// Where this instance listens.
    pub fn advertise(&self) -> &str {
        &self.address
    }

    /// Where peer `node` listens, as of the last listing.
    pub fn address(&self, node: &str) -> Option<String> {
        self.listed.borrow().get(node).map(|r| r.address.clone())
    }

    /// Publish or renew this instance's record; answers the new expiry. The
    /// first write requires the key to be absent (the node id is fresh per
    /// process), and every later one requires our last version, so a record
    /// someone else wrote fences this process instead of being overwritten.
    pub async fn renew(&self, now_ms: u64) -> Result<u64> {
        let expires_ms = now_ms + self.ttl_ms;
        let key = node_key(&self.node);
        let mut guard = self
            .own
            .borrow()
            .clone()
            .map_or(Guard::Absent, Guard::Match);
        loop {
            // A renewal that lands after `deregister` would bring this
            // instance back to life.
            if self.left.get() {
                return Err(Error::NotHere(format!("{} left the cluster", self.node)));
            }
            // Built for each attempt, so that a renewal that races
            // `start_draining` retries with the flag set.
            let body = serde_json::to_vec(&NodeRecord {
                node: self.node.clone(),
                address: self.address.clone(),
                expires_ms,
                instance: self.instance,
                draining: self.draining.get(),
            })?;
            if let Some(version) = self.bucket.put_if(&key, body, guard).await? {
                self.own.replace(Some(version));
                return Ok(expires_ms);
            }
            // Not the version we know. If this process wrote it, earlier
            // renewals landed and only their answers were lost (timed-out
            // rounds on the fleet), and one may land between our read and
            // write: retry. Anything else is another writer.
            let Some((current, version)) = self.bucket.get(&key).await? else {
                return Err(Error::Fenced(self.node.clone()));
            };
            let current: NodeRecord = serde_json::from_slice(&current)?;
            if current.instance != self.instance {
                return Err(Error::Fenced(self.node.clone()));
            }
            guard = Guard::Match(version);
        }
    }

    /// Expire this instance's record on a clean shutdown, so peers stop
    /// routing to it before its lease would have run out and adopt its
    /// alarms. The record goes once they have. A renewal of ours that lands
    /// first is expired too; no renewal starts after this.
    pub async fn deregister(&self) -> Result<()> {
        self.left.set(true);
        let Some(mut version) = self.own.take() else {
            return Ok(());
        };
        let record = NodeRecord {
            node: self.node.clone(),
            address: self.address.clone(),
            expires_ms: 0,
            instance: self.instance,
            draining: true,
        };
        let body = serde_json::to_vec(&record)?;
        let key = node_key(&self.node);
        for _ in 0..DEREGISTER_ATTEMPTS {
            let guard = Guard::Match(version);
            if self
                .bucket
                .put_if(&key, body.clone(), guard)
                .await?
                .is_some()
            {
                return Ok(());
            }
            let Some((current, current_version)) = self.bucket.get(&key).await? else {
                return Ok(());
            };
            let current: NodeRecord = serde_json::from_slice(&current)?;
            if current.instance != self.instance {
                return Err(Error::Fenced(self.node.clone()));
            }
            version = current_version;
        }
        Err(Error::Contended(key))
    }

    /// Marks this instance's record as draining from its next renewal.
    pub fn start_draining(&self) {
        self.draining.set(true);
    }

    /// Whether `deregister` ran.
    pub fn left(&self) -> bool {
        self.left.get()
    }

    /// Whether `node` held a lease past `now_ms` at the last listing. No
    /// I/O: a route checks it on every use.
    pub fn listed_live(&self, node: &str, now_ms: u64) -> bool {
        self.listed
            .borrow()
            .get(node)
            .is_some_and(|record| record.expires_ms > now_ms)
    }

    /// A peer's record while its lease holds, from the last listing when it
    /// was there: routing asks on every request, and a GET per request was
    /// most of a routed request's time.
    pub async fn live(&self, node: &str, now_ms: u64) -> Result<Option<NodeRecord>> {
        if let Some(record) = self.listed.borrow().get(node)
            && record.expires_ms > now_ms
        {
            return Ok(Some(record.clone()));
        }
        self.fetch(node, now_ms).await
    }

    /// A peer's record while its lease holds, read now, whatever the last
    /// listing said.
    pub async fn live_now(&self, node: &str, now_ms: u64) -> Result<Option<NodeRecord>> {
        self.fetch(node, now_ms).await
    }

    /// A peer's record while its lease holds. One found dead leaves the
    /// listing, as its lease running out would, so that routes to it go
    /// before the next listing.
    async fn fetch(&self, node: &str, now_ms: u64) -> Result<Option<NodeRecord>> {
        let Some((body, _)) = self.bucket.get(&node_key(node)).await? else {
            self.listed.borrow_mut().remove(node);
            return Ok(None);
        };
        let record: NodeRecord = serde_json::from_slice(&body)?;
        if record.expires_ms + DEAD_RECORD_TTLS * self.ttl_ms < now_ms
            && self
                .bucket
                .list(&format!("alarms/{node}/"))
                .await?
                .is_empty()
        {
            // A fenced or crashed instance leaves its record, every restart
            // adds one, and every listing read them all: 40 after an
            // afternoon of fleet tests. One this long expired belongs to a
            // process that fenced long ago; were it still running, its next
            // renewal's conditional write would fail and fence it. It stays
            // while it has alarms, so that survivors find them.
            self.bucket.delete(&node_key(node)).await?;
        }
        if record.expires_ms <= now_ms {
            self.listed.borrow_mut().remove(node);
            return Ok(None);
        }
        // Until the next listing: one read serves every request that finds
        // the listing stale, and a stale listing is when the bucket is busy.
        self.listed
            .borrow_mut()
            .insert(node.to_owned(), record.clone());
        Ok(Some(record))
    }

    /// Every live instance, this one included when its lease holds.
    pub async fn peers(&self, now_ms: u64) -> Result<Vec<NodeRecord>> {
        // The records are read concurrently: a lease round is bounded, and
        // reading them one after another made it time out on R2's spikes.
        let listed = self.bucket.list("nodes/").await?;
        let nodes = listed.iter().filter_map(|entry| {
            entry
                .key
                .strip_prefix("nodes/")
                .and_then(|key| key.strip_suffix(".json"))
        });
        let nodes: Vec<_> = nodes.collect();
        let fetched = futures::future::join_all(nodes.iter().map(|n| self.fetch(n, now_ms))).await;
        let mut peers = Vec::new();
        for record in fetched {
            peers.extend(record?);
        }
        *self.listed.borrow_mut() = peers.iter().map(|p| (p.node.clone(), p.clone())).collect();
        *self.dead.borrow_mut() = nodes
            .into_iter()
            .filter(|n| !self.listed.borrow().contains_key(*n))
            .map(str::to_owned)
            .collect();
        Ok(peers)
    }

    /// The live nodes, this one included, as of the last listing.
    pub fn listed_nodes(&self, now_ms: u64) -> Vec<String> {
        let listed = self.listed.borrow();
        let live = listed.values().filter(|r| r.expires_ms > now_ms);
        live.map(|r| r.node.clone()).collect()
    }

    /// The live nodes that are not draining, as of the last listing: those
    /// that may take objects and alarms on.
    pub fn serving_nodes(&self, now_ms: u64) -> Vec<String> {
        let listed = self.listed.borrow();
        let live = listed
            .values()
            .filter(|r| r.expires_ms > now_ms && !r.draining);
        live.map(|r| r.node.clone()).collect()
    }

    /// Whether `node` has a record, live or not.
    pub async fn recorded(&self, node: &str) -> Result<bool> {
        Ok(self.bucket.get(&node_key(node)).await?.is_some())
    }

    /// Dead nodes whose records remain, as of the last listing.
    pub fn dead(&self) -> Vec<String> {
        self.dead.borrow().clone()
    }
}

/// How many lease lifetimes past its expiry a node record is deleted.
const DEAD_RECORD_TTLS: u64 = 10;
/// How many renewals of ours in flight a deregistration outlasts.
const DEREGISTER_ATTEMPTS: usize = 4;

#[cfg(test)]
#[path = "registry-test.rs"]
mod tests;
