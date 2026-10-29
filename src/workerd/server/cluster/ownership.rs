// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
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
}

/// Claims, routes and releases Durable Objects through their owner records.
pub struct Ownership {
    bucket: Bucket,
    registry: Rc<Registry>,
    /// Whether this instance may claim objects: off once it drains.
    pub may_claim: Cell<bool>,
    /// Where each routed object's owner was, and when that was read.
    routes: RefCell<HashMap<String, Route>>,
    /// The record each claim wrote, and the guard that it is still the
    /// current one, so `open` changes it without reading it back. A claim
    /// of an object with no record writes nothing: `open` creates the
    /// record, open, in one conditional write, as celld claims a new cell.
    claimed: RefCell<HashMap<String, (OwnerRecord, Guard)>>,
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
    Error::Replica(format!("{key}: this node is draining"))
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
            claimed: RefCell::new(HashMap::new()),
        }
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
                    if record.node == me {
                        // A draining node lets its objects go, its own too.
                        if !self.may_claim.get() {
                            return Err(draining(&key));
                        }
                        return Ok(Claim::Owned(record.owned()));
                    }
                    if !record.node.is_empty()
                        && let Some(peer) = self.registry.live(&record.node, now_ms).await?
                    {
                        // An earlier process at our address, restarted
                        // before its lease ran out: a route there loops back
                        // here. Its lease, not a route, ends this.
                        if peer.address == self.registry.advertise() {
                            return Err(Error::Replica(format!(
                                "{key}: owned by this address's previous instance"
                            )));
                        }
                        return Ok(self.route(key, peer, now_ms));
                    }
                    (record, Guard::Match(version))
                }
            };
            if !self.may_claim.get() {
                return Err(draining(&key));
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
            if let Some(guard) = written {
                self.claimed
                    .borrow_mut()
                    .insert(key, (record.clone(), guard));
                return Ok(Claim::Owned(record.owned()));
            }
        }
        Err(Error::Contended(key))
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
mod tests {
    #![allow(clippy::unwrap_used)]

    use std::rc::Rc;

    use futures::executor::block_on;

    use crate::Bucket;
    use crate::Claim;
    use crate::Error;
    use crate::Ownership;
    use crate::Registry;
    use crate::ownership::OwnerRecord;
    use crate::ownership::owner_key;

    const TTL: u64 = 10_000;

    /// Matches any `Owned` claim at `epoch`.
    fn owned(epoch: u64) -> EpochOf {
        EpochOf(epoch)
    }

    #[derive(Debug)]
    struct EpochOf(u64);

    impl PartialEq<EpochOf> for Claim {
        fn eq(&self, other: &EpochOf) -> bool {
            matches!(self, Self::Owned(o) if o.epoch == other.0)
        }
    }

    fn node(store: &Bucket, name: &str) -> (Rc<Registry>, Ownership) {
        let bucket = store.at("cluster");
        let registry = Rc::new(Registry::new(
            bucket.clone(),
            name,
            &format!("{name}:1"),
            TTL,
        ));
        (registry.clone(), Ownership::new(bucket, registry))
    }

    /// `own` claims and opens a new object `x` at epoch 1: a new object's
    /// record appears when its epoch opens.
    async fn open_x(own: &Ownership) {
        let claim = own.acquire_or_route("ns", "x", 0).await;
        assert_eq!(claim.unwrap(), owned(1));
        assert!(own.open("ns", "x", 1).await.unwrap());
    }

    #[test]
    fn peers_are_the_live_registrations() {
        block_on(async {
            let store = Bucket::memory("");
            let (a, _) = node(&store, "a");
            let (b, _) = node(&store, "b");
            a.renew(0).await.unwrap();
            b.renew(5_000).await.unwrap();
            let names = |peers: Vec<crate::NodeRecord>| {
                peers.into_iter().map(|peer| peer.node).collect::<Vec<_>>()
            };
            assert_eq!(names(a.peers(1_000).await.unwrap()), ["a", "b"]);
            // a's lease ran out at 10s; b's holds until 15s.
            assert_eq!(names(a.peers(12_000).await.unwrap()), ["b"]);
            b.deregister().await.unwrap();
            assert!(a.peers(12_000).await.unwrap().is_empty());
        });
    }

    #[test]
    fn a_restart_never_routes_to_its_previous_instance() {
        block_on(async {
            let store = Bucket::memory("");
            let (a, a_own) = node(&store, "a");
            a.renew(0).await.unwrap();
            open_x(&a_own).await;
            // The process restarts at a's address with a fresh node id while
            // a's record is still live: routing there would reach itself.
            let bucket = store.at("cluster");
            let a_ = Rc::new(Registry::new(bucket.clone(), "a_", "a:1", TTL));
            a_.renew(1_000).await.unwrap();
            let a_own_ = Ownership::new(bucket, a_);
            let claim = a_own_.acquire_or_route("ns", "x", 1_000).await;
            assert!(matches!(claim, Err(Error::Replica(_))), "{claim:?}");
            // Once a's lease ends, the restart takes it over.
            let claim = a_own_.acquire_or_route("ns", "x", 11_000).await;
            assert_eq!(claim.unwrap(), owned(2));
        });
    }

    #[test]
    fn a_rewritten_node_record_fences_its_instance() {
        block_on(async {
            let store = Bucket::memory("");
            let (a, _) = node(&store, "a");
            let (impostor, _) = node(&store, "a");
            a.renew(0).await.unwrap();
            // A second process with the same identity cannot publish over it.
            assert!(matches!(impostor.renew(0).await, Err(Error::Fenced(_))));
            a.renew(1_000).await.unwrap();
        });
    }

    #[test]
    fn a_claim_owns_an_unowned_object_and_routes_to_a_live_owner() {
        block_on(async {
            let store = Bucket::memory("");
            let (a, a_own) = node(&store, "a");
            let (b, b_own) = node(&store, "b");
            a.renew(0).await.unwrap();
            b.renew(0).await.unwrap();
            open_x(&a_own).await;
            // Asking again keeps it, at the same epoch.
            assert_eq!(
                a_own.acquire_or_route("ns", "x", 1).await.unwrap(),
                owned(1)
            );
            assert_eq!(
                b_own.acquire_or_route("ns", "x", 1).await.unwrap(),
                Claim::Route {
                    node: "a".into(),
                    address: "a:1".into(),
                    stale: false,
                }
            );
        });
    }

    #[test]
    fn a_stale_listing_keeps_a_route_to_a_live_owner() {
        block_on(async {
            let store = Bucket::memory("");
            let (a, a_own) = node(&store, "a");
            let (b, b_own) = node(&store, "b");
            a.renew(0).await.unwrap();
            b.renew(0).await.unwrap();
            open_x(&a_own).await;
            let route = b_own.acquire_or_route("ns", "x", 1).await;
            assert!(matches!(route.unwrap(), Claim::Route { .. }));
            // a renews, but b lists nothing more: what b knows of a expires.
            a.renew(8_000).await.unwrap();
            // With the record gone a reread would claim x, so a route shows
            // that only a's own record was read.
            store.delete("cluster/do/ns/x/own.json").await.unwrap();
            assert_eq!(
                b_own.acquire_or_route("ns", "x", 12_000).await.unwrap(),
                Claim::Route {
                    node: "a".into(),
                    address: "a:1".into(),
                    stale: true,
                }
            );
        });
    }

    #[test]
    fn a_dead_owners_object_is_taken_over_at_the_next_epoch() {
        block_on(async {
            let store = Bucket::memory("");
            let (a, a_own) = node(&store, "a");
            let (b, b_own) = node(&store, "b");
            a.renew(0).await.unwrap();
            b.renew(0).await.unwrap();
            open_x(&a_own).await;
            // a stops renewing; b renews past a's expiry and takes x over.
            b.renew(9_000).await.unwrap();
            assert_eq!(
                b_own.acquire_or_route("ns", "x", 11_000).await.unwrap(),
                owned(2)
            );
            assert_eq!(
                a_own.acquire_or_route("ns", "x", 11_000).await.unwrap(),
                Claim::Route {
                    node: "b".into(),
                    address: "b:1".into(),
                    stale: false,
                }
            );
        });
    }

    #[test]
    fn an_owner_advances_only_its_open_epoch() {
        block_on(async {
            let store = Bucket::memory("");
            let (a, a_own) = node(&store, "a");
            a.renew(0).await.unwrap();
            open_x(&a_own).await;
            // The next epoch recovers the one it left, and does not advance
            // until it opens.
            let next = a_own.advance("ns", "x", 1).await.unwrap().unwrap();
            assert_eq!((next.epoch, next.recover), (2, Some(1)));
            assert!(a_own.advance("ns", "x", 2).await.unwrap().is_none());
            assert!(a_own.open("ns", "x", 2).await.unwrap());
            // A record changed since is not advanced over.
            vacate(&store).await;
            assert!(a_own.advance("ns", "x", 2).await.unwrap().is_none());
        });
    }

    /// Clear the owner of `x`, as another writer might, under a cached read.
    async fn vacate(store: &Bucket) {
        let bucket = store.at("cluster");
        let key = owner_key("ns", "x");
        let (body, _) = bucket.get(&key).await.unwrap().unwrap();
        let mut record: OwnerRecord = serde_json::from_slice(&body).unwrap();
        record.node = String::new();
        bucket
            .put(&key, serde_json::to_vec(&record).unwrap(), &[])
            .await
            .unwrap();
    }

    #[test]
    fn concurrent_claims_open_exactly_one_epoch() {
        block_on(async {
            let store = Bucket::memory("");
            let nodes: Vec<_> = (0..8).map(|i| node(&store, &format!("n{i}"))).collect();
            for (registry, _) in &nodes {
                registry.renew(0).await.unwrap();
            }
            let claims = futures::future::join_all(
                nodes
                    .iter()
                    .map(|(_, own)| own.acquire_or_route("ns", "x", 0)),
            )
            .await;
            // A new object's claims all succeed; the conditional write that
            // opens the epoch has exactly one winner.
            assert!(claims.iter().all(|c| matches!(c, Ok(Claim::Owned(_)))));
            let opens =
                futures::future::join_all(nodes.iter().map(|(_, own)| own.open("ns", "x", 1)))
                    .await;
            let owners = opens.iter().filter(|o| matches!(o, Ok(true))).count();
            assert_eq!(owners, 1);
        });
    }

    #[test]
    fn a_takeover_recovers_the_last_epoch_that_opened() {
        block_on(async {
            let store = Bucket::memory("");
            let (a, a_own) = node(&store, "a");
            let (b, b_own) = node(&store, "b");
            let (c, c_own) = node(&store, "c");
            for r in [&a, &b, &c] {
                r.renew(0).await.unwrap();
            }
            let Claim::Owned(first) = a_own.acquire_or_route("ns", "x", 0).await.unwrap() else {
                panic!("a should own x")
            };
            assert_eq!(first.recover, Some(0));
            assert!(a_own.open("ns", "x", 1).await.unwrap());
            // Only the claimant, at its epoch, opens.
            assert!(!b_own.open("ns", "x", 1).await.unwrap());

            // a's lease runs out; b claims epoch 2 and dies before opening.
            b.renew(9_000).await.unwrap();
            c.renew(9_000).await.unwrap();
            let Claim::Owned(second) = b_own.acquire_or_route("ns", "x", 11_000).await.unwrap()
            else {
                panic!("b should own x")
            };
            assert_eq!(second.recover, Some(1));

            // c takes over from b's unopened epoch: it still recovers epoch 1.
            c.renew(20_000).await.unwrap();
            let Claim::Owned(third) = c_own.acquire_or_route("ns", "x", 21_000).await.unwrap()
            else {
                panic!("c should own x")
            };
            assert_eq!(third.epoch, 3);
            assert_eq!(third.recover, Some(1));
        });
    }

    #[test]
    fn a_listed_owners_route_is_used_and_refreshed_in_the_background() {
        block_on(async {
            let store = Bucket::memory("");
            let (a, a_own) = node(&store, "a");
            let (b, b_own) = node(&store, "b");
            a.renew(0).await.unwrap();
            b.renew(0).await.unwrap();
            b.peers(0).await.unwrap();
            open_x(&a_own).await;
            let to_a = |stale| Claim::Route {
                node: "a".into(),
                address: "a:1".into(),
                stale,
            };
            let route = async |now| b_own.acquire_or_route("ns", "x", now).await.unwrap();
            assert_eq!(route(1_000).await, to_a(false));
            // x loses its owner; b still routes to a until it refreshes.
            vacate(&store).await;
            assert_eq!(route(5_000).await, to_a(false));
            // The first use past the interval is stale, and only the first.
            assert_eq!(route(6_000).await, to_a(true));
            assert_eq!(route(6_001).await, to_a(false));
            // The refresh finds no owner, so b claims x.
            b_own.refresh_route("ns", "x", 6_002).await.unwrap();
            assert_eq!(route(6_003).await, owned(2));
        });
    }

    #[test]
    fn a_route_ends_with_its_owners_listed_lease() {
        block_on(async {
            let store = Bucket::memory("");
            let (a, a_own) = node(&store, "a");
            let (b, b_own) = node(&store, "b");
            a.renew(0).await.unwrap();
            b.renew(0).await.unwrap();
            b.peers(0).await.unwrap();
            open_x(&a_own).await;
            b_own.acquire_or_route("ns", "x", 1).await.unwrap();
            // a dies; b's route goes when a's listed lease ends.
            b.renew(9_000).await.unwrap();
            assert_eq!(
                b_own.acquire_or_route("ns", "x", 11_000).await.unwrap(),
                owned(2)
            );
        });
    }
}
