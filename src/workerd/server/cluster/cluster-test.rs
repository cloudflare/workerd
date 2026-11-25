// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::rc::Rc;
use std::sync::mpsc;
use std::time::Duration;

use cluster_logic::cache;
use cluster_logic::lease::Timing;
use futures::executor::block_on;

use super::Cluster;
use super::Setup;
use super::check_conditional_writes;
use crate::Bucket;
use crate::Error;
use crate::bucket::memory::Memory;

/// Linking, after `join`, opens storage, which on a slow disk takes longer
/// than a lease, in turns of the loop that let nothing else run: the lease
/// thread's rounds, which `join` starts, keep the lease held throughout,
/// and watch the loop only once the node serves (`Cluster::watch`).
#[test]
fn the_lease_holds_however_long_startup_takes_after_join() {
    // Rounds that fence only once the lease thread got no time for most of
    // the lease, and a loop that stands still for less than linking takes.
    let timing = Timing {
        lease_ms: 400,
        interval_ms: 10,
        renew_timeout_ms: 20,
        list_timeout_ms: 20,
        retry_ms: 5,
        renew_attempt_ms: 10,
        list_attempt_ms: 10,
        contended_pause_ms: 5,
        max_skew_ms: 5,
        stall_ms: 50,
    };
    let setup = Setup {
        url: String::new(),
        options: Vec::new(),
        node: "a".into(),
        address: "a:1".into(),
        timing,
        key: vec![0; 32],
    };
    let (fenced, fence) = mpsc::channel();
    let cluster = Cluster::threaded(
        Bucket::memory(""),
        || Ok(Bucket::memory("")),
        setup,
        Box::new(move |reason, _| {
            let _ = fenced.send(reason);
        }),
        Box::new(|body| body()),
    )
    .unwrap();
    block_on(cluster.join()).unwrap();
    // Linking, as one turn of the loop thread that outlasts the lease.
    std::thread::sleep(Duration::from_millis(2 * timing.lease_ms));
    // What an actor database checks before it opens (`require_lease`).
    assert!(cluster.lease_holds());
    assert!(fence.try_recv().is_err());
    // Serving, the loop that stands still from here on fences the node.
    cluster.watch(Box::new(|| 0));
    let reason = fence
        .recv_timeout(Duration::from_millis(10 * timing.lease_ms))
        .unwrap();
    assert!(
        reason.starts_with("its event loop made no progress for "),
        "{reason}"
    );
}

#[test]
fn a_bucket_that_ignores_preconditions_is_refused() {
    block_on(async {
        assert!(
            check_conditional_writes(&Bucket::memory(""), "n.probe", Duration::ZERO)
                .await
                .is_ok()
        );
        let message = |store: Memory| async {
            let bucket = Bucket::new(store, "");
            match check_conditional_writes(&bucket, "n.probe", Duration::ZERO).await {
                Err(Error::Config(message)) => message,
                other => panic!("expected a config error, not {other:?}"),
            }
        };
        let lax = Memory::default();
        lax.ignores_guards.set(true);
        assert!(
            message(lax)
                .await
                .starts_with("the bucket ignores conditional writes")
        );
        let weak = Memory::default();
        weak.never_matches.set(true);
        assert!(
            message(weak)
                .await
                .starts_with("the bucket refused a conditional write whose condition held")
        );
    });
}

/// Every probe write's outcome unknown: what the object holds says which
/// landed, so a store that keeps preconditions passes and one that ignores
/// them does not. A store whose versions never match is refused even so:
/// its refusals of a write whose guard held read as a copy still in
/// progress, which the write waits out, `PAUSE` at a time, and is refused.
#[test]
fn probe_writes_of_unknown_outcome_are_settled_by_reading_the_probe() {
    const PAUSE: Duration = Duration::from_millis(1);
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_time()
        .build()
        .unwrap();
    runtime.block_on(async {
        // More unknown outcomes than the probe's writes take, so that none
        // ends because they ran out; `check` counts the writes.
        const INJECTED: usize = 32;
        let check = |store: Memory| async {
            store
                .unknown
                .borrow_mut()
                .extend(["n.probe"; INJECTED].map(str::to_owned));
            let unknown = store.unknown.clone();
            let checked = check_conditional_writes(&Bucket::new(store, ""), "n.probe", PAUSE).await;
            let writes = INJECTED - unknown.borrow().len();
            (checked, writes)
        };
        let refused = |checked: &crate::Result<()>| match checked {
            Err(Error::Config(message)) => message.clone(),
            other => panic!("expected a config error, not {other:?}"),
        };
        let (checked, writes) = check(Memory::default()).await;
        checked.unwrap();
        assert_eq!(writes, 4);
        let lax = Memory::default();
        lax.ignores_guards.set(true);
        let (checked, writes) = check(lax).await;
        assert!(refused(&checked).starts_with("the bucket ignores conditional writes"));
        assert_eq!(writes, 4);
        // Each write whose guard held, the replacement and the stale one,
        // is written three times.
        let weak = Memory::default();
        weak.never_matches.set(true);
        let (checked, writes) = check(weak).await;
        assert!(
            refused(&checked)
                .starts_with("the bucket refused a conditional write whose condition held")
        );
        assert_eq!(writes, 8);
    });
}

fn cluster(bucket: &Bucket, node: &str) -> Rc<Cluster> {
    let setup = Setup {
        url: String::new(),
        options: Vec::new(),
        node: node.to_owned(),
        address: format!("{node}:1"),
        timing: Timing::DEFAULT,
        key: Vec::new(),
    };
    Cluster::over(bucket.clone(), setup)
}

/// A lease round's renewal, sent at `at`, as `Keeper::maintain` counts it.
async fn renew(cluster: &Cluster, at: u64) {
    let until = cluster.renew(at).await.unwrap();
    let super::Lessee::Here(keeper) = &cluster.lessee else {
        panic!("a test's cluster keeps its lease itself");
    };
    keeper.renewed_at(at, Ok(until));
}

/// `cache::Timing::view` of `membership` is `cache-test.rs`'s `Sim::leases`.
#[test]
fn the_cache_view_is_the_simulations() {
    block_on(async {
        let t = cache::Timing::DEFAULT;
        let bucket = Bucket::memory("");
        let (a, b) = (cluster(&bucket, "a"), cluster(&bucket, "b"));
        let none = (Vec::new(), false, false);
        let view = |at| t.view(at, a.membership());
        assert_eq!(view(super::now_ms()), none);
        a.join().await.unwrap();
        b.join().await.unwrap();
        // Registered, renewed, never listed.
        let r = a.registered.get().unwrap();
        assert_eq!(view(r), none);
        renew(&a, r).await;
        renew(&b, r).await;
        a.peers(r + 1).await.unwrap();
        let both = vec!["a".to_owned(), "b".to_owned()];
        assert_eq!(view(r + 1), (both.clone(), true, false));
        // Stale from the listing's start, the lease renewed meanwhile.
        renew(&a, r + 10_000).await;
        let stale = r + 1 + t.stale_ms;
        assert_eq!(view(stale - 1), (both.clone(), true, false));
        assert_eq!(view(stale), (both.clone(), false, false));
        // Active `join_ms` after the registration answered.
        renew(&a, r + 15_000).await;
        renew(&b, r + 15_000).await;
        a.peers(r + 20_000).await.unwrap();
        let joined = r + t.join_ms;
        assert_eq!(view(joined - 1), (both.clone(), true, false));
        assert_eq!(view(joined), (both.clone(), true, true));
        // Fenced a copy's lease before the lease's own margin: 20 s from
        // the renewal's send, less 4 s of skew and 1.04 s.
        a.peers(r + 28_000).await.unwrap();
        let fenced = r + 15_000 + 20_000 - 4_000 - t.lease_wait_ms();
        assert_eq!(view(fenced - 1), (both.clone(), true, true));
        assert_eq!(view(fenced), (both.clone(), false, false));
        // A listing has the live records: b's expired.
        renew(&a, r + 30_000).await;
        a.peers(r + 36_000).await.unwrap();
        let alone = vec!["a".to_owned()];
        assert_eq!(view(r + 36_000), (alone.clone(), true, true));
        // An older listing that ends later is not kept.
        a.peers(r + 30_000).await.unwrap();
        assert_eq!(view(r + 36_000), (alone.clone(), true, true));
        a.start_draining();
        assert_eq!(view(r + 36_000), (alone.clone(), true, false));
        a.deregister().await.unwrap();
        assert_eq!(view(r + 36_000), (alone, false, false));
    });
}
