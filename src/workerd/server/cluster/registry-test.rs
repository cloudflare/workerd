// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(clippy::unwrap_used)]

use super::*;

fn block_on<F: std::future::Future>(f: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
        .build()
        .unwrap()
        .block_on(f)
}

#[test]
fn a_renewal_whose_answer_was_lost_is_not_a_fence() {
    block_on(async {
        let bucket = Bucket::memory("");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        a.renew(0).await.unwrap();
        // The renewal at 1_000 lands, but its answer never arrives.
        let landed = NodeRecord {
            node: "a".into(),
            address: "a:1".into(),
            expires_ms: 11_000,
            instance: a.instance,
            draining: false,
        };
        let body = serde_json::to_vec(&landed).unwrap();
        bucket.put(&node_key("a"), body, &[]).await.unwrap();
        assert_eq!(a.renew(2_000).await.unwrap(), 12_000);
        // Someone else's write is still a fence.
        let other = NodeRecord {
            instance: a.instance + 1,
            ..landed
        };
        let body = serde_json::to_vec(&other).unwrap();
        bucket.put(&node_key("a"), body, &[]).await.unwrap();
        assert!(matches!(a.renew(3_000).await, Err(Error::Fenced(_))));
    });
}

/// A write's key and body.
type Write = (String, Vec<u8>);

/// Lands `late`, a write of ours a timed-out round left in flight, just
/// after serving the next GET.
#[derive(Default)]
struct Late {
    inner: crate::bucket::memory::Memory,
    late: std::rc::Rc<RefCell<Option<Write>>>,
}

#[async_trait::async_trait(?Send)]
impl crate::bucket::Store for Late {
    async fn get(
        &self,
        key: &str,
        to: Option<&std::path::Path>,
    ) -> Result<Option<(Vec<u8>, Version)>> {
        let got = self.inner.get(key, to).await;
        if let Some((key, body)) = self.late.take() {
            let body = crate::bucket::Payload::Bytes(body.into());
            self.inner.put(&key, body, None, &[]).await?;
        }
        got
    }
    async fn put(
        &self,
        key: &str,
        body: crate::bucket::Payload,
        guard: Option<Guard>,
        metadata: &[(&str, String)],
    ) -> Result<Option<Version>> {
        self.inner.put(key, body, guard, metadata).await
    }
    async fn list(&self, prefix: &str) -> Result<Vec<crate::bucket::Entry>> {
        self.inner.list(prefix).await
    }
    async fn delete(&self, key: &str) -> Result<()> {
        self.inner.delete(key).await
    }
}

#[test]
fn two_late_renewals_of_ours_are_not_a_fence() {
    block_on(async {
        let store = Late::default();
        let late = store.late.clone();
        let bucket = Bucket::new(store, "");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        a.renew(0).await.unwrap();
        let ours = |expires_ms| {
            let record = NodeRecord {
                node: "a".into(),
                address: "a:1".into(),
                expires_ms,
                instance: a.instance,
                draining: false,
            };
            serde_json::to_vec(&record).unwrap()
        };
        // Two timed-out rounds landed: one before this renewal's first
        // read, one just after it.
        bucket.put(&node_key("a"), ours(11_000), &[]).await.unwrap();
        late.replace(Some((node_key("a"), ours(12_000))));
        assert_eq!(a.renew(3_000).await.unwrap(), 13_000);
    });
}

#[test]
fn a_listing_deletes_records_long_expired() {
    block_on(async {
        let bucket = Bucket::memory("");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        let b = Registry::new(bucket.clone(), "b", "b:1", 10_000);
        let c = Registry::new(bucket.clone(), "c", "c:1", 10_000);
        a.renew(0).await.unwrap();
        b.renew(90_000).await.unwrap();
        c.renew(115_000).await.unwrap();
        // a expired at 10s, b at 100s: at 115s only a is ten lives dead.
        let live = c.peers(115_000).await.unwrap();
        assert_eq!(live.iter().map(|p| &*p.node).collect::<Vec<_>>(), ["c"]);
        let keys: Vec<_> = bucket.list("nodes/").await.unwrap();
        let keys: Vec<_> = keys.iter().map(|e| &*e.key).collect();
        assert_eq!(keys, ["nodes/b.json", "nodes/c.json"]);
    });
}

#[test]
fn a_deregistration_outlasts_a_renewal_of_ours_that_landed_first() {
    block_on(async {
        let bucket = Bucket::memory("");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        let b = Registry::new(bucket.clone(), "b", "b:1", 10_000);
        a.renew(0).await.unwrap();
        // A timed-out renewal of a's lands before the deregistration.
        let landed = NodeRecord {
            node: "a".into(),
            address: "a:1".into(),
            expires_ms: 11_000,
            instance: a.instance,
            draining: false,
        };
        let body = serde_json::to_vec(&landed).unwrap();
        bucket.put(&node_key("a"), body, &[]).await.unwrap();
        a.deregister().await.unwrap();
        b.renew(1).await.unwrap();
        let live = b.peers(1).await.unwrap();
        assert_eq!(live.iter().map(|p| &*p.node).collect::<Vec<_>>(), ["b"]);
        // No renewal brings a back.
        assert!(matches!(a.renew(2).await, Err(Error::NotHere(_))));
        let live = b.peers(2).await.unwrap();
        assert_eq!(live.iter().map(|p| &*p.node).collect::<Vec<_>>(), ["b"]);
    });
}

#[test]
fn a_draining_node_serves_no_more_in_its_peers_listings() {
    block_on(async {
        let bucket = Bucket::memory("");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        let b = Registry::new(bucket.clone(), "b", "b:1", 10_000);
        a.renew(0).await.unwrap();
        b.renew(0).await.unwrap();
        b.peers(1).await.unwrap();
        let mut serving = b.serving_nodes(1);
        serving.sort();
        assert_eq!(serving, ["a", "b"]);
        a.start_draining();
        a.renew(2).await.unwrap();
        b.peers(3).await.unwrap();
        // Still live, so what it owns routes there, but chosen for nothing.
        assert!(b.listed_live("a", 3));
        assert_eq!(b.serving_nodes(3), ["b"]);
    });
}
