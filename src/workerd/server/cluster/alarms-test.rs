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

fn entry(at_ms: u64, actor: &str) -> Entry {
    Entry {
        at_ms,
        namespace: "ns".into(),
        actor: actor.into(),
        seq: 0,
    }
}

/// `node`'s entries as time and actor, earliest first.
async fn listed(alarms: &Alarms, node: &str) -> Vec<(u64, String)> {
    let entries = alarms.list(node).await.unwrap();
    entries.into_iter().map(|e| (e.at_ms, e.actor)).collect()
}

fn at(at_ms: u64, actor: &str) -> (u64, String) {
    (at_ms, actor.to_owned())
}

#[test]
fn a_node_lists_its_entries_earliest_first() {
    block_on(async {
        let alarms = Alarms::new(Bucket::memory(""));
        // 0x100 sorts after 0xff only by length: the key's prefix sees to it.
        for e in [entry(0x100, "a"), entry(0xff, "b"), entry(7, "c")] {
            alarms.put("n1", &e, None).await.unwrap();
        }
        alarms.put("n2", &entry(1, "d"), None).await.unwrap();
        let listed = alarms.list("n1").await.unwrap();
        assert_eq!(listed, [entry(7, "c"), entry(0xff, "b"), entry(0x100, "a")]);
        alarms.delete("n1", &entry(0xff, "b")).await.unwrap();
        assert_eq!(alarms.list("n1").await.unwrap().len(), 2);
    });
}

#[test]
fn an_entry_carries_the_actors_name() {
    block_on(async {
        let alarms = Alarms::new(Bucket::memory(""));
        alarms
            .put("n1", &entry(1, "a"), Some("room"))
            .await
            .unwrap();
        alarms.put("n1", &entry(1, "b"), None).await.unwrap();
        let name = alarms.name("n1", &entry(1, "a")).await.unwrap();
        assert_eq!(name.as_deref(), Some("room"));
        assert_eq!(alarms.name("n1", &entry(1, "b")).await.unwrap(), None);
        assert_eq!(alarms.name("n1", &entry(2, "a")).await.unwrap(), None);
    });
}

#[test]
fn a_dead_nodes_entries_spread_and_only_its_share_moves() {
    let live: Vec<String> = ["a", "b", "c", "d"].map(String::from).into();
    let entries: Vec<_> = (0..400).map(|i| entry(1, &format!("{i:x}"))).collect();
    let before: Vec<_> = entries
        .iter()
        .map(|e| assignee(&live, e).unwrap())
        .collect();
    for node in &live {
        let share = before.iter().filter(|&&n| n == node).count();
        assert!((50..150).contains(&share), "{node} adopts {share} of 400");
    }
    // `d` dies too: only what it would have adopted moves.
    let fewer = &live[..3];
    for (e, was) in entries.iter().zip(&before) {
        let now = assignee(fewer, e).unwrap();
        assert!(*was == "d" || now == *was);
    }
    assert_eq!(assignee(&[], &entries[0]), None);
}

#[test]
fn arming_waits_only_for_new_or_earlier_alarms() {
    block_on(async {
        let bucket = Bucket::memory("");
        let alarms = Alarms::new(bucket.clone());
        let index = Index::new(bucket, "n1", 0);
        index.arm("ns", "a", Some("room"), Some(20)).await.unwrap();
        assert_eq!(listed(&alarms, "n1").await, [at(20, "a")]);
        // Earlier: indexed before arm resolves, the old entry gone on tidy.
        index.arm("ns", "a", Some("room"), Some(10)).await.unwrap();
        assert_eq!(listed(&alarms, "n1").await, [at(10, "a"), at(20, "a")]);
        index.tidy().await;
        assert_eq!(listed(&alarms, "n1").await, [at(10, "a")]);
        // Later: the early entry stays until tidy moves it.
        index.arm("ns", "a", Some("room"), Some(30)).await.unwrap();
        assert_eq!(listed(&alarms, "n1").await, [at(10, "a")]);
        index.tidy().await;
        assert_eq!(listed(&alarms, "n1").await, [at(30, "a")]);
        let entry = alarms.list("n1").await.unwrap().remove(0);
        let name = alarms.name("n1", &entry).await.unwrap();
        assert_eq!(name.as_deref(), Some("room"));
        // Cleared.
        index.arm("ns", "a", None, None).await.unwrap();
        index.tidy().await;
        assert!(alarms.list("n1").await.unwrap().is_empty());
        assert!(index.armed.borrow().is_empty());
    });
}

#[test]
fn moving_back_to_a_stale_entrys_time_keeps_an_entry() {
    block_on(async {
        let bucket = Bucket::memory("");
        let alarms = Alarms::new(bucket.clone());
        let index = Index::new(bucket, "n1", 0);
        index.arm("ns", "a", None, Some(59)).await.unwrap();
        index.arm("ns", "a", None, Some(50)).await.unwrap();
        // Back to 59: tidy writes 59 again and deletes the first 59 entry,
        // which must not be the new one.
        index.arm("ns", "a", None, Some(59)).await.unwrap();
        index.tidy().await;
        assert_eq!(listed(&alarms, "n1").await, [at(59, "a")]);
    });
}

#[test]
fn tidy_writes_again_what_failed() {
    block_on(async {
        let store = Flaky::default();
        let fail = std::rc::Rc::clone(&store.fail_puts);
        let bucket = Bucket::new(store, "");
        let alarms = Alarms::new(bucket.clone());
        let index = Index::new(bucket, "n1", 0);
        fail.set(true);
        assert!(index.arm("ns", "a", None, Some(20)).await.is_err());
        fail.set(false);
        index.tidy().await;
        assert_eq!(listed(&alarms, "n1").await, [at(20, "a")]);
    });
}

#[test]
fn survivors_split_a_dead_nodes_alarms() {
    block_on(async {
        let bucket = Bucket::memory("");
        let alarms = Alarms::new(bucket.clone());
        let dead = Index::new(bucket.clone(), "d", 0);
        for i in 0..40 {
            let name = (i % 2 == 0).then(|| format!("name{i}"));
            dead.arm("ns", &format!("{i:x}"), name.as_deref(), Some(100 + i))
                .await
                .unwrap();
        }
        let live: Vec<String> = ["a", "b"].map(String::from).into();
        let (a, b) = (
            Index::new(bucket.clone(), "a", 0),
            Index::new(bucket, "b", 0),
        );
        // `a` already owns object 0 with a later alarm: its index keeps
        // that, and the adopted one only wakes the object early.
        a.arm("ns", "0", Some("name0"), Some(500)).await.unwrap();
        let dead_nodes = ["d".to_owned()];
        let by_a = a.adopt(&live, &dead_nodes).await;
        let by_b = b.adopt(&live, &dead_nodes).await;
        assert!(alarms.list("d").await.unwrap().is_empty());
        let (in_a, in_b) = (
            alarms.list("a").await.unwrap(),
            alarms.list("b").await.unwrap(),
        );
        assert_eq!(by_a.len() + by_b.len(), 40);
        assert_eq!(in_a.len() + in_b.len(), 41);
        assert!(!by_a.is_empty() && !by_b.is_empty());
        let named = by_a.iter().chain(&by_b).find(|x| x.actor == "2").unwrap();
        assert_eq!(named.name.as_deref(), Some("name2"));
        assert_eq!(named.at_ms, 102);
    });
}

/// The memory store, but `alarms/d/` DELETEs fail while `fail` is set, and
/// every PUT while `fail_puts` is.
#[derive(Default)]
struct Flaky {
    inner: crate::bucket::memory::Memory,
    fail: std::rc::Rc<std::cell::Cell<bool>>,
    fail_puts: std::rc::Rc<std::cell::Cell<bool>>,
}

#[async_trait::async_trait(?Send)]
impl crate::Store for Flaky {
    async fn get(
        &self,
        key: &str,
        to: Option<&std::path::Path>,
    ) -> Result<Option<(Vec<u8>, crate::Version)>> {
        self.inner.get(key, to).await
    }
    async fn put(
        &self,
        key: &str,
        body: crate::bucket::Payload,
        guard: Option<crate::Guard>,
        metadata: &[(&str, String)],
    ) -> Result<Option<crate::Version>> {
        if self.fail_puts.get() {
            return Err(crate::Error::Http("503".into()));
        }
        self.inner.put(key, body, guard, metadata).await
    }
    async fn list(&self, prefix: &str) -> Result<Vec<crate::bucket::Entry>> {
        self.inner.list(prefix).await
    }
    async fn delete(&self, key: &str) -> Result<()> {
        if self.fail.get() && key.contains("alarms/d/") {
            return Err(crate::Error::Http("503".into()));
        }
        self.inner.delete(key).await
    }
}

#[test]
fn a_failed_adoption_still_schedules_and_retries() {
    block_on(async {
        let store = Flaky::default();
        let fail = std::rc::Rc::clone(&store.fail);
        let bucket = Bucket::new(store, "");
        let dead = Index::new(bucket.clone(), "d", 0);
        for i in 0..8 {
            dead.arm("ns", &format!("{i:x}"), None, Some(100 + i))
                .await
                .unwrap();
        }
        let (live, dead_nodes) = (["a".to_owned()], ["d".to_owned()]);
        let a = Index::new(bucket.clone(), "a", 0);
        fail.set(true);
        // Indexed here but still there: every one is scheduled once.
        assert_eq!(a.adopt(&live, &dead_nodes).await.len(), 8);
        assert_eq!(
            Alarms::new(bucket.clone()).list("d").await.unwrap().len(),
            8
        );
        fail.set(false);
        // Again, which only schedules what is scheduled already.
        assert_eq!(a.adopt(&live, &dead_nodes).await.len(), 8);
        assert!(Alarms::new(bucket).list("d").await.unwrap().is_empty());
    });
}
