// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::collections::HashSet;
use std::rc::Rc;

use serde::Deserialize;
use serde::Serialize;

use crate::Bucket;
use crate::Error;
use crate::Guard;
use crate::Registry;
use crate::Result;
use crate::registry::NodeRecord;

/// One Durable Object's record in `do/<namespace>/<actor>/own.json`, the only
/// authority for who owns it and whose log holds its latest commits.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
struct OwnerRecord {
    /// The owning instance (`NodeRecord::node`); empty for none.
    node: String,
    /// The owner's epoch: its session of the log.
    epoch: u64,
    /// The last epoch that opened. A claim writes the next `epoch` and
    /// copies `from` from the record it replaced; the opening write sets it
    /// to `epoch`. So an epoch is recovering while `from != epoch`, and
    /// recovers `from`'s log (none when 0).
    from: u64,
}

impl OwnerRecord {
    /// `node`'s claim on the next epoch. A claimant that died mid-recovery
    /// leaves `from` unchanged.
    fn successor(&self, node: &str) -> Self {
        Self {
            node: node.to_owned(),
            epoch: self.epoch + 1,
            from: self.from,
        }
    }

    fn owned(self) -> Owned {
        Owned {
            recover: (self.from != self.epoch).then_some(self.from),
            epoch: self.epoch,
        }
    }
}

/// This instance's claim on an object.
#[derive(Debug, PartialEq, Eq)]
pub struct Owned {
    pub epoch: u64,
    /// The epoch whose log a new owner recovers (0: none). `None` when this
    /// instance already opened the epoch.
    pub recover: Option<u64>,
}

/// Who serves an object.
#[derive(Debug, PartialEq, Eq)]
pub enum Claim {
    Owned(Owned),
    /// A live peer owns it; send the request there. A `stale` route is
    /// still used, and the caller refreshes it in the background.
    Route {
        node: String,
        address: String,
        stale: bool,
    },
    /// This instance owns it at this epoch but drains: it hands the object
    /// off (`release`), and the request goes on to whoever claims it.
    Releasing(u64),
}

/// Claims, routes and releases Durable Objects through their owner records.
pub struct Ownership {
    bucket: Bucket,
    registry: Rc<Registry>,
    /// Whether this instance may claim objects: off once it drains.
    pub may_claim: Cell<bool>,
    /// Where each routed object's owner was, and when that was read.
    routes: RefCell<HashMap<String, Route>>,
    /// The peer each object's last route led to, which answered that it
    /// does not serve the object (`forget_route`): until a claim of the
    /// object succeeds, a route there is not used, and the claim reads that
    /// peer's own record, not the listing.
    refused: RefCell<HashMap<String, String>>,
    /// The record each claim wrote, and the guard that it is still the
    /// current one, so `open` changes it without reading it back. A claim
    /// of an object with no record writes nothing: `open` creates the
    /// record, open, in one conditional write, as celld claims a new cell.
    claimed: RefCell<HashMap<String, (OwnerRecord, Guard)>>,
    /// Objects this draining instance owns but failed to release: like the
    /// rest once it left, they go to whoever claims them.
    abandoned: RefCell<HashSet<String>>,
}

struct Route {
    node: String,
    address: String,
    read_ms: u64,
}

/// How old a route gets before a use of it also rereads the owner record.
/// A route is used for as long as its node is in the registry listing, as
/// celld does: rereading on the request path put a bucket read on one
/// request in every interval, and dropping the route at a time limit does
/// nothing a dead owner's lease does not already do. A route to a live
/// former owner costs a hop, which that owner routes on.
const ROUTE_REFRESH_MS: u64 = 5_000;

/// How many times a claim retries when another instance keeps winning.
const ATTEMPTS: usize = 8;

fn draining(key: &str) -> Error {
    Error::NotHere(format!("{key}: this node is draining"))
}

fn owner_key(namespace: &str, actor: &str) -> String {
    format!("{}/own.json", crate::object_prefix(namespace, actor))
}

impl Ownership {
    pub fn new(bucket: Bucket, registry: Rc<Registry>) -> Self {
        Self {
            bucket,
            registry,
            may_claim: Cell::new(true),
            routes: RefCell::new(HashMap::new()),
            refused: RefCell::new(HashMap::new()),
            claimed: RefCell::new(HashMap::new()),
            abandoned: RefCell::new(HashSet::new()),
        }
    }

    /// Forgets that `node` serves the object, after it answered that it does
    /// not. The route there goes, and the next claim reads the owner record
    /// and, if that still names `node`, `node`'s own record: the listing may
    /// predate its drain.
    pub fn forget_route(&self, namespace: &str, actor: &str, node: &str) {
        let key = owner_key(namespace, actor);
        let mut routes = self.routes.borrow_mut();
        if routes.get(&key).is_some_and(|route| route.node == node) {
            routes.remove(&key);
        }
        self.refused.borrow_mut().insert(key, node.to_owned());
    }

    /// Own the object, or learn which live peer does. A record with no owner,
    /// or one naming an instance whose lease expired, is taken over with the
    /// next epoch; the conditional write makes exactly one claimant win.
    pub async fn acquire_or_route(
        &self,
        namespace: &str,
        actor: &str,
        now_ms: u64,
    ) -> Result<Claim> {
        let key = owner_key(namespace, actor);
        let refused = self.refused.borrow().get(&key).cloned();
        if let Some(node) = &refused {
            // Put back since by a concurrent claim or a refresh.
            let mut routes = self.routes.borrow_mut();
            if routes.get(&key).is_some_and(|route| &route.node == node) {
                routes.remove(&key);
            }
        }
        let claim = self.claim(key.clone(), refused.as_deref(), now_ms).await;
        if claim.is_ok() && self.refused.borrow().get(&key) == refused.as_ref() {
            self.refused.borrow_mut().remove(&key);
        }
        claim
    }

    async fn claim(&self, key: String, refused: Option<&str>, now_ms: u64) -> Result<Claim> {
        let me = self.registry.node();
        // A route whose node a stale listing no longer shows: its own record
        // decides, not the object's.
        let unlisted = self.routes.borrow().get(&key).map(|r| r.node.clone());
        if let Some(node) = unlisted
            && !self.registry.listed_live(&node, now_ms)
        {
            self.registry.live(&node, now_ms).await?;
        }
        if let Some(route) = self.routes.borrow_mut().get_mut(&key)
            && self.registry.listed_live(&route.node, now_ms)
        {
            // One refresh per interval: the first use past it takes it.
            let stale = now_ms >= route.read_ms + ROUTE_REFRESH_MS;
            if stale {
                route.read_ms = now_ms;
            }
            return Ok(Claim::Route {
                node: route.node.clone(),
                address: route.address.clone(),
                stale,
            });
        }
        for _ in 0..ATTEMPTS {
            let (previous, guard) = match self.bucket.get(&key).await? {
                None => (OwnerRecord::default(), Guard::Absent),
                Some((body, version)) => {
                    let record: OwnerRecord = serde_json::from_slice(&body)?;
                    if record.node == me && self.may_claim.get() {
                        return Ok(Claim::Owned(record.owned()));
                    }
                    // A draining node lets its objects go, its own too. One
                    // it failed to let go, or any once it left, it serves no
                    // more: whoever claims it next takes it over.
                    let let_go = self.registry.left() || self.abandoned.borrow().contains(&key);
                    if record.node == me && !let_go {
                        return Ok(Claim::Releasing(record.epoch));
                    }
                    let peer = if record.node.is_empty() || record.node == me {
                        None
                    } else if refused == Some(record.node.as_str()) {
                        self.registry.live_now(&record.node, now_ms).await?
                    } else {
                        self.registry.live(&record.node, now_ms).await?
                    };
                    if let Some(peer) = peer {
                        // An earlier process at our address, restarted
                        // before its lease ran out: a route there loops back
                        // here. Its lease, not a route, ends this.
                        if peer.address == self.registry.advertise() {
                            return Err(Error::NotHere(format!(
                                "{key}: owned by this address's previous instance"
                            )));
                        }
                        return Ok(self.route(key, peer, now_ms));
                    }
                    (record, Guard::Match(version))
                }
            };
            // No live instance owns it, and a draining one claims nothing:
            // a peer does.
            if !self.may_claim.get() {
                return self.successor(&key, refused, now_ms).await;
            }
            let record = previous.successor(me);
            // A new object opens at its claim, where a loser still routes
            // (`open_fresh`).
            let fresh = matches!(guard, Guard::Absent);
            let written = if fresh {
                Some(Guard::Absent)
            } else {
                let body = serde_json::to_vec(&record)?;
                let version = self.bucket.put_if(&key, body, guard).await?;
                version.map(Guard::Match)
            };
            // Draining may have started during the write: the object it
            // claimed is let go like the others this node owns.
            if !self.may_claim.get() && written.is_some() {
                return Ok(Claim::Releasing(record.epoch));
            }
            if let Some(guard) = written {
                self.claimed
                    .borrow_mut()
                    .insert(key, (record.clone(), guard));
                return Ok(Claim::Owned(record.owned()));
            }
        }
        Err(Error::Contended(key))
    }

    /// Where a draining instance sends a request for an object no live
    /// instance owns: the serving peer that `handoff::successor` picks,
    /// which claims it. Not cached: the peer may lose the claim to another.
    /// A peer that `refused` it, or could not be reached, is passed over
    /// while there is another, and its own record read, which drops it from
    /// the listing if it died. A listing that names no peer is read again,
    /// as it may predate them.
    async fn successor(&self, key: &str, refused: Option<&str>, now_ms: u64) -> Result<Claim> {
        if let Some(node) = refused {
            self.registry.live_now(node, now_ms).await?;
        }
        let mut pick = self.pick_successor(key, refused, now_ms);
        if pick.is_none() {
            self.registry.peers(now_ms).await?;
            pick = self.pick_successor(key, refused, now_ms);
        }
        // The only peer left may have refused for what it knew then.
        let pick = pick.or_else(|| self.pick_successor(key, None, now_ms));
        let (node, address) = pick.ok_or_else(|| draining(key))?;
        Ok(Claim::Route {
            node,
            address,
            stale: false,
        })
    }

    fn pick_successor(
        &self,
        key: &str,
        refused: Option<&str>,
        now_ms: u64,
    ) -> Option<(String, String)> {
        let me = self.registry.node();
        let serving = self.registry.serving_nodes(now_ms);
        let peers = serving
            .iter()
            .map(String::as_str)
            .filter(|node| *node != me && Some(*node) != refused);
        let node = cluster_logic::handoff::successor(key, peers)?;
        Some((node.to_owned(), self.registry.address(node)?))
    }

    /// Gives up on releasing an object this draining instance owns: requests
    /// for it go to whoever claims it, as once this instance left.
    pub fn abandon(&self, namespace: &str, actor: &str) {
        self.abandoned
            .borrow_mut()
            .insert(owner_key(namespace, actor));
    }

    /// Hand off an object this instance owns at `epoch` and no longer
    /// writes: the record keeps its epochs and names no owner, so the next
    /// claim anywhere takes the object over, recovering this epoch's log
    /// at the next. Answers whether the record still named this owner.
    pub async fn release(&self, namespace: &str, actor: &str, epoch: u64) -> Result<bool> {
        let key = owner_key(namespace, actor);
        self.claimed.borrow_mut().remove(&key);
        let Some((body, version)) = self.bucket.get(&key).await? else {
            return Ok(false);
        };
        let record: OwnerRecord = serde_json::from_slice(&body)?;
        if record.node != self.registry.node() || record.epoch != epoch {
            return Ok(false);
        }
        let released = OwnerRecord {
            node: String::new(),
            ..record
        };
        let body = serde_json::to_vec(&released)?;
        let written = self
            .bucket
            .put_if(&key, body, Guard::Match(version))
            .await?;
        Ok(written.is_some())
    }

    fn route(&self, key: String, peer: NodeRecord, now_ms: u64) -> Claim {
        let claim = Claim::Route {
            node: peer.node.clone(),
            address: peer.address.clone(),
            stale: false,
        };
        let route = Route {
            node: peer.node,
            address: peer.address,
            read_ms: now_ms,
        };
        self.routes.borrow_mut().insert(key, route);
        claim
    }

    /// Reread a stale route's owner record. The route goes if no live peer
    /// owns the object any more; the next request then claims or routes.
    pub async fn refresh_route(&self, namespace: &str, actor: &str, now_ms: u64) -> Result<()> {
        let key = owner_key(namespace, actor);
        let node = match self.bucket.get(&key).await? {
            Some((body, _)) => serde_json::from_slice::<OwnerRecord>(&body)?.node,
            None => String::new(),
        };
        let peer = if node.is_empty() || node == self.registry.node() {
            None
        } else {
            self.registry.live(&node, now_ms).await?
        };
        match peer {
            Some(peer) => drop(self.route(key, peer, now_ms)),
            None => drop(self.routes.borrow_mut().remove(&key)),
        }
        Ok(())
    }

    /// Open the epoch this instance claimed and recovered. Answers whether
    /// the record still named this claim; if not, the epoch never opens.
    pub async fn open(&self, namespace: &str, actor: &str, epoch: u64) -> Result<bool> {
        let key = owner_key(namespace, actor);
        let claimed = self.claimed.borrow_mut().remove(&key);
        let (mut record, guard) = if let Some(claimed) = claimed {
            claimed
        } else {
            let Some((body, version)) = self.bucket.get(&key).await? else {
                return Ok(false);
            };
            (serde_json::from_slice(&body)?, Guard::Match(version))
        };
        if record.node != self.registry.node() || record.epoch != epoch || record.from == epoch {
            return Ok(false);
        }
        record.from = epoch;
        let body = serde_json::to_vec(&record)?;
        Ok(self.bucket.put_if(&key, body, guard).await?.is_some())
    }

    /// Move this instance's open epoch to the next, which recovers the one
    /// it leaves like any other. `None` when the record no longer names this
    /// open epoch.
    pub async fn advance(&self, namespace: &str, actor: &str, epoch: u64) -> Result<Option<Owned>> {
        let key = owner_key(namespace, actor);
        let Some((body, version)) = self.bucket.get(&key).await? else {
            return Ok(None);
        };
        let previous: OwnerRecord = serde_json::from_slice(&body)?;
        if previous.node != self.registry.node()
            || previous.epoch != epoch
            || previous.from != epoch
        {
            return Ok(None);
        }
        let record = previous.successor(&previous.node);
        let body = serde_json::to_vec(&record)?;
        let Some(version) = self
            .bucket
            .put_if(&key, body, Guard::Match(version))
            .await?
        else {
            return Ok(None);
        };
        self.claimed
            .borrow_mut()
            .insert(key, (record.clone(), Guard::Match(version)));
        Ok(Some(record.owned()))
    }

    /// The epoch own.json names now: the re-read after a bucket upload that
    /// makes the upload an acknowledgement (spec: `TierAck`).
    pub async fn current_epoch(&self, namespace: &str, actor: &str) -> Result<Option<u64>> {
        Ok(match self.bucket.get(&owner_key(namespace, actor)).await? {
            None => None,
            Some((body, _)) => Some(serde_json::from_slice::<OwnerRecord>(&body)?.epoch),
        })
    }
}

#[cfg(test)]
#[path = "ownership-test.rs"]
mod tests;
