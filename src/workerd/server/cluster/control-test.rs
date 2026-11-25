// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::future::Future;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use super::*;

thread_local! {
    /// Whether the control planes made on this thread yield (`yielding`).
    static YIELDING: Cell<bool> = const { Cell::new(false) };
}

/// While the guard lives, every `Cluster` a test makes on this thread reaches
/// its control plane as a data plane of another thread does: each call gives
/// way to whatever else is ready before it starts and before its answer is
/// used, so what the caller read before the call may have changed.
pub fn yielding() -> impl Drop {
    struct Guard(bool);
    impl Drop for Guard {
        fn drop(&mut self) {
            YIELDING.with(|yielding| yielding.set(self.0));
        }
    }
    Guard(YIELDING.with(|yielding| yielding.replace(true)))
}

/// `control`, or while `yielding` is in effect, `control` behind `Yielding`.
pub fn wrap(control: Rc<dyn Control>) -> Rc<dyn Control> {
    if YIELDING.with(Cell::get) {
        Rc::new(Yielding(control))
    } else {
        control
    }
}

/// Pending once, then ready: gives way to every other task that is ready.
struct GiveWay(bool);

impl Future for GiveWay {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if self.0 {
            return Poll::Ready(());
        }
        self.0 = true;
        cx.waker().wake_by_ref();
        Poll::Pending
    }
}

async fn give_way() {
    GiveWay(false).await;
}

/// A control plane that is a turn of the loop away in each direction.
struct Yielding(Rc<dyn Control>);

#[async_trait(?Send)]
impl Open for Yielding {
    async fn open(&self, ns: &str, actor: &str, epoch: u64) -> Result<bool> {
        give_way().await;
        let opened = self.0.open(ns, actor, epoch).await;
        give_way().await;
        opened
    }

    async fn current_epoch(&self, ns: &str, actor: &str) -> Result<Option<u64>> {
        self.0.current_epoch(ns, actor).await
    }
}

#[async_trait(?Send)]
impl Control for Yielding {
    async fn claim(&self, ns: &str, actor: &str, now_ms: u64) -> Result<Claim> {
        give_way().await;
        let claim = self.0.claim(ns, actor, now_ms).await;
        give_way().await;
        claim
    }

    async fn advance(&self, ns: &str, actor: &str, epoch: u64) -> Result<Option<Owned>> {
        give_way().await;
        let next = self.0.advance(ns, actor, epoch).await;
        give_way().await;
        next
    }

    async fn release(&self, ns: &str, actor: &str, epoch: u64) -> Result<bool> {
        give_way().await;
        let released = self.0.release(ns, actor, epoch).await;
        give_way().await;
        released
    }

    fn abandon(&self, ns: &str, actor: &str) {
        self.0.abandon(ns, actor);
    }

    fn forget_route(&self, ns: &str, actor: &str, node: &str) {
        self.0.forget_route(ns, actor, node);
    }

    async fn refresh_route(&self, ns: &str, actor: &str, now_ms: u64) -> Result<()> {
        give_way().await;
        self.0.refresh_route(ns, actor, now_ms).await
    }

    async fn arm_alarm(
        &self,
        ns: &str,
        actor: &str,
        name: Option<&str>,
        at_ms: Option<u64>,
    ) -> Result<()> {
        give_way().await;
        self.0.arm_alarm(ns, actor, name, at_ms).await
    }

    async fn opened_alarm(
        &self,
        ns: &str,
        actor: &str,
        name: Option<&str>,
        at_ms: u64,
    ) -> Result<()> {
        give_way().await;
        self.0.opened_alarm(ns, actor, name, at_ms).await
    }

    async fn alarm_elsewhere(
        &self,
        ns: &str,
        actor: &str,
        owner: &str,
        now_ms: u64,
    ) -> Result<bool> {
        give_way().await;
        self.0.alarm_elsewhere(ns, actor, owner, now_ms).await
    }
}

fn cluster(node: &str) -> Rc<crate::Cluster> {
    let setup = crate::Setup {
        url: "s3://bucket/prefix".into(),
        options: Vec::new(),
        node: node.into(),
        address: format!("{node}:1"),
        timing: cluster_logic::lease::Timing::DEFAULT,
        key: b"key".to_vec(),
    };
    crate::Cluster::over(Bucket::memory(""), setup)
}

#[test]
fn what_every_thread_reads_crosses_threads() {
    fn send_sync<T: Send + Sync + Clone>() {}
    send_sync::<Shared>();
}

#[test]
fn every_thread_sees_the_drain_before_the_instance_stops_claiming() {
    let cluster = cluster("a");
    let elsewhere = cluster.shared().clone();
    assert!(!elsewhere.is_draining());
    assert!(!cluster.member().is_draining());
    cluster.start_draining();
    assert!(elsewhere.is_draining());
    assert!(cluster.member().is_draining());
    assert!(cluster.is_draining());
}

#[test]
fn the_mac_is_the_keys_on_every_thread() {
    let cluster = cluster("a");
    let shared = cluster.shared().clone();
    assert_eq!(shared.mac(b"data"), hmac_sha256(b"key", b"data"));
    assert_eq!(shared.mac(b"data"), cluster.mac(b"data"));
    assert_eq!(cluster.member().mac(b"data"), cluster.mac(b"data"));
    assert_ne!(shared.mac(b"data"), shared.mac(b"other"));
    assert_eq!(shared.node(), "a");
    assert_eq!(cluster.member().node(), "a");
}

/// A data plane other than the instance's own claims through the instance's control plane,
/// and a draining instance's refuses where the claim is made.
#[test]
fn a_member_claims_through_the_control_plane() {
    let cluster = cluster("a");
    let bucket = Bucket::memory("");
    let member = Member::new(bucket, cluster.shared().clone(), cluster.control());
    let claim = futures::executor::block_on(cluster.control().claim("ns", "x", 0)).unwrap();
    let Claim::Owned(owned) = claim else {
        panic!("a new object is claimed");
    };
    assert_eq!((owned.epoch, owned.recover), (1, Some(0)));
    cluster.start_draining();
    let refused = futures::executor::block_on(member.acquire_or_route("ns", "y", 0));
    assert!(
        matches!(refused, Err(Error::NotHere(_))),
        "a draining instance claimed"
    );
}
