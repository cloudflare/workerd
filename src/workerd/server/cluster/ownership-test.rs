// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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
        assert!(matches!(claim, Err(Error::NotHere(_))), "{claim:?}");
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
fn a_forgotten_route_is_read_again_from_its_node_record() {
    block_on(async {
        let store = Bucket::memory("");
        let (a, a_own) = node(&store, "a");
        let (b, b_own) = node(&store, "b");
        a.renew(0).await.unwrap();
        b.renew(0).await.unwrap();
        open_x(&a_own).await;
        let route_to_a = Claim::Route {
            node: "a".into(),
            address: "a:1".into(),
            stale: false,
        };
        assert_eq!(
            b_own.acquire_or_route("ns", "x", 1).await.unwrap(),
            route_to_a
        );
        // a drains and leaves. b's listing still shows it, and so does its
        // route, until a answers that it does not serve x.
        a.deregister().await.unwrap();
        assert_eq!(
            b_own.acquire_or_route("ns", "x", 2).await.unwrap(),
            route_to_a
        );
        b_own.forget_route("ns", "x", "a");
        assert_eq!(
            b_own.acquire_or_route("ns", "x", 3).await.unwrap(),
            owned(2)
        );
    });
}

#[test]
fn a_route_refreshed_after_its_node_refused_is_not_used() {
    block_on(async {
        let store = Bucket::memory("");
        let (a, a_own) = node(&store, "a");
        let (b, b_own) = node(&store, "b");
        a.renew(0).await.unwrap();
        b.renew(0).await.unwrap();
        open_x(&a_own).await;
        let route = b_own.acquire_or_route("ns", "x", 1).await;
        assert!(matches!(route.unwrap(), Claim::Route { .. }));
        a.deregister().await.unwrap();
        b_own.forget_route("ns", "x", "a");
        // A refresh that read the owner record before a answered puts the
        // route back, from the stale listing.
        b_own.refresh_route("ns", "x", 2).await.unwrap();
        assert_eq!(
            b_own.acquire_or_route("ns", "x", 3).await.unwrap(),
            owned(2)
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
            futures::future::join_all(nodes.iter().map(|(_, own)| own.open("ns", "x", 1))).await;
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
        let Claim::Owned(second) = b_own.acquire_or_route("ns", "x", 11_000).await.unwrap() else {
            panic!("b should own x")
        };
        assert_eq!(second.recover, Some(1));

        // c takes over from b's unopened epoch: it still recovers epoch 1.
        c.renew(20_000).await.unwrap();
        let Claim::Owned(third) = c_own.acquire_or_route("ns", "x", 21_000).await.unwrap() else {
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

#[test]
fn a_draining_owner_releases_its_object_to_the_next_claim() {
    block_on(async {
        let store = Bucket::memory("");
        let (a, a_own) = node(&store, "a");
        let (b, b_own) = node(&store, "b");
        a.renew(0).await.unwrap();
        b.renew(0).await.unwrap();
        open_x(&a_own).await;
        a_own.may_claim.set(false);
        // Its own object is to be handed off, not served.
        assert_eq!(
            a_own.acquire_or_route("ns", "x", 1).await.unwrap(),
            Claim::Releasing(1)
        );
        // Only the epoch a holds is released.
        assert!(!a_own.release("ns", "x", 2).await.unwrap());
        assert!(a_own.release("ns", "x", 1).await.unwrap());
        assert!(!a_own.release("ns", "x", 1).await.unwrap());
        // While a's lease still holds, b claims the next epoch, which
        // recovers a's.
        let Claim::Owned(next) = b_own.acquire_or_route("ns", "x", 2).await.unwrap() else {
            panic!("b should own x")
        };
        assert_eq!((next.epoch, next.recover), (2, Some(1)));
    });
}

#[test]
fn a_draining_node_sends_an_object_nobody_owns_to_a_peer() {
    block_on(async {
        let store = Bucket::memory("");
        let (a, a_own) = node(&store, "a");
        a.renew(0).await.unwrap();
        a.peers(0).await.unwrap();
        a_own.may_claim.set(false);
        // With no peer to claim it, nothing can serve it.
        let claim = a_own.acquire_or_route("ns", "x0", 1).await;
        assert!(matches!(claim, Err(Error::NotHere(_))), "{claim:?}");

        let peers: Vec<_> = ["b", "c"].iter().map(|n| node(&store, n)).collect();
        for (registry, _) in &peers {
            registry.renew(0).await.unwrap();
        }
        a.peers(1).await.unwrap();
        let mut routes = Vec::new();
        for i in 0..16 {
            let actor = format!("x{i}");
            let claim = a_own.acquire_or_route("ns", &actor, 2).await.unwrap();
            // The same peer every time.
            assert_eq!(
                a_own.acquire_or_route("ns", &actor, 2).await.unwrap(),
                claim
            );
            routes.push(claim);
        }
        for name in ["b", "c"] {
            let route = Claim::Route {
                node: name.into(),
                address: format!("{name}:1"),
                stale: false,
            };
            assert!(routes.contains(&route), "no object went to {name}");
        }
        // Nothing was claimed here; the peer a request goes to claims it.
        assert!(store.at("cluster").list("do/").await.unwrap().is_empty());
        let Claim::Route { node: peer, .. } = &routes[0] else {
            panic!("x0 should route to a peer")
        };
        let (_, own) = &peers[usize::from(peer == "c")];
        assert_eq!(own.acquire_or_route("ns", "x0", 3).await.unwrap(), owned(1));
    });
}

fn route_to(node: &str) -> Claim {
    Claim::Route {
        node: node.into(),
        address: format!("{node}:1"),
        stale: false,
    }
}

#[test]
fn a_draining_node_routes_past_draining_and_refusing_peers() {
    block_on(async {
        let store = Bucket::memory("");
        let (a, a_own) = node(&store, "a");
        let (b, _) = node(&store, "b");
        let (c, _) = node(&store, "c");
        for registry in [&a, &b, &c] {
            registry.renew(0).await.unwrap();
        }
        c.start_draining();
        c.renew(0).await.unwrap();
        a.peers(1).await.unwrap();
        a_own.may_claim.set(false);
        // c drains too: every object goes to b.
        for i in 0..8 {
            let claim = a_own.acquire_or_route("ns", &format!("x{i}"), 2).await;
            assert_eq!(claim.unwrap(), route_to("b"));
        }
        // b refused x0; with no other peer, it is asked again.
        a_own.forget_route("ns", "x0", "b");
        let claim = a_own.acquire_or_route("ns", "x0", 3).await;
        assert_eq!(claim.unwrap(), route_to("b"));
    });
}

#[test]
fn a_draining_node_sends_what_it_cannot_release_to_a_peer() {
    block_on(async {
        let store = Bucket::memory("");
        let (a, a_own) = node(&store, "a");
        let (b, _) = node(&store, "b");
        a.renew(0).await.unwrap();
        b.renew(0).await.unwrap();
        a.peers(0).await.unwrap();
        open_x(&a_own).await;
        assert_eq!(
            a_own.acquire_or_route("ns", "y", 0).await.unwrap(),
            owned(1)
        );
        assert!(a_own.open("ns", "y", 1).await.unwrap());
        a_own.may_claim.set(false);
        for actor in ["x", "y"] {
            let claim = a_own.acquire_or_route("ns", actor, 1).await;
            assert_eq!(claim.unwrap(), Claim::Releasing(1));
        }
        // One it failed to release, and all once it left, go to a peer.
        a_own.abandon("ns", "x");
        assert_eq!(
            a_own.acquire_or_route("ns", "x", 2).await.unwrap(),
            route_to("b")
        );
        a.deregister().await.unwrap();
        assert_eq!(
            a_own.acquire_or_route("ns", "y", 2).await.unwrap(),
            route_to("b")
        );
    });
}
