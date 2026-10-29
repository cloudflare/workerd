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
fn the_nodes_with_entries_are_found_without_a_record() {
    block_on(async {
        let bucket = Bucket::memory("p");
        let index = Index::new(bucket.clone(), "n1", 0);
        for node in ["n1", "n3", "n2"] {
            index.alarms.put(node, &entry(1, "a"), None).await.unwrap();
        }
        bucket.put("alarmsx/n4/1", Vec::new(), &[]).await.unwrap();
        let unknown = index.unknown(&["n1".to_owned()]).await.unwrap();
        assert_eq!(unknown, ["n2", "n3"]);
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
        let (mut by_a, mut by_b) = (Vec::new(), Vec::new());
        a.adopt(&live, &dead_nodes, |x| by_a.push(x)).await;
        b.adopt(&live, &dead_nodes, |x| by_b.push(x)).await;
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

async fn adopt(index: &Index, live: &[String], dead: &[String]) -> usize {
    let mut n = 0;
    index.adopt(live, dead, |_| n += 1).await;
    n
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
        assert_eq!(adopt(&a, &live, &dead_nodes).await, 8);
        assert_eq!(
            Alarms::new(bucket.clone()).list("d").await.unwrap().len(),
            8
        );
        fail.set(false);
        // Again, which only schedules what is scheduled already.
        assert_eq!(adopt(&a, &live, &dead_nodes).await, 8);
        assert!(Alarms::new(bucket).list("d").await.unwrap().is_empty());
    });
}

/// Deterministic simulation (`crate::dst`): the real `Index` on several
/// nodes, among crashes, takeovers, arming, firing and adoption rounds. The
/// server's part (ownership, the local scheduler, commits, opening an
/// object) is modelled here. Once the faults stop, every alarm an object's
/// database holds must run.
mod dst {
    use std::cell::RefCell;
    use std::collections::BTreeMap;
    use std::collections::BTreeSet;
    use std::rc::Rc;

    use super::*;
    use crate::dst::Harness;
    use crate::dst::Store;

    const NS: &str = "ns";
    const OBJECTS: usize = 8;
    /// How long a crashed node's lease outlives it.
    const LEASE: u64 = 3;
    /// As the cluster sets it: twice the lease.
    const GRACE: u64 = 2 * LEASE;

    struct Node {
        id: String,
        alive: bool,
        lease_until: u64,
        /// The local scheduler.
        local: BTreeMap<String, u64>,
        /// Objects with a call in flight here, as an actor's input gate
        /// serializes them.
        busy: BTreeSet<String>,
        /// Whether an alarms round is running: the loop runs one at a time.
        round: bool,
    }

    #[derive(Default)]
    struct World {
        now: u64,
        nodes: Vec<Node>,
        owner: BTreeMap<String, usize>,
        log: Vec<String>,
    }

    impl World {
        fn routable(&self, n: usize) -> bool {
            let node = &self.nodes[n];
            node.alive || self.now < node.lease_until
        }
    }

    /// Where an alarm or request for `o` runs, seen from `n`.
    enum Route {
        Here,
        Claim,
        Remote(usize),
    }

    fn route(world: &World, n: usize, o: &str) -> Route {
        match world.owner.get(o) {
            Some(&m) if m == n => Route::Here,
            Some(&m) if world.routable(m) => Route::Remote(m),
            _ => Route::Claim,
        }
    }

    fn db_key(o: &str) -> String {
        format!("db/{o}")
    }

    struct Sim {
        dst: Harness,
        world: Rc<RefCell<World>>,
        indexes: Vec<Option<Rc<Index>>>,
        faults: bool,
    }

    impl Sim {
        fn new(seed: u64) -> Self {
            let mut sim = Self {
                dst: Harness::new(seed),
                world: Rc::default(),
                indexes: Vec::new(),
                faults: true,
            };
            for _ in 0..3 {
                sim.start();
            }
            sim
        }

        fn log(&self, line: &str) {
            let mut world = self.world.borrow_mut();
            let now = world.now;
            world.log.push(format!("t{now} {line}"));
        }

        fn start(&mut self) {
            let n = self.dst.node();
            let id = format!("n{n}");
            let index = Index::new(self.dst.bucket.clone(), &id, GRACE);
            self.indexes.push(Some(Rc::new(index)));
            self.world.borrow_mut().nodes.push(Node {
                id,
                alive: true,
                lease_until: 0,
                local: BTreeMap::new(),
                busy: BTreeSet::new(),
                round: false,
            });
            self.log(&format!("start n{n}"));
        }

        fn crash(&mut self, n: usize) {
            self.dst.crash(n);
            self.indexes[n] = None;
            let mut world = self.world.borrow_mut();
            let until = world.now + LEASE;
            let node = &mut world.nodes[n];
            node.alive = false;
            node.lease_until = until;
            node.local.clear();
            node.busy.clear();
            drop(world);
            self.log(&format!("crash n{n}"));
        }

        fn alive(&self) -> Vec<usize> {
            let world = self.world.borrow();
            (0..world.nodes.len())
                .filter(|&n| world.nodes[n].alive)
                .collect()
        }

        /// Opening `o` on `n`, its owner now: the alarm its database holds
        /// is scheduled and indexed here, the index write detached, so
        /// nothing notices its failure but `tidy`.
        fn open(world: &mut World, store: &Store, index: &Rc<Index>, n: usize, o: &str) {
            world.owner.insert(o.to_owned(), n);
            let Some(at) = store.peek(&db_key(o)).map(|b| parse_at(&b)) else {
                return;
            };
            world.nodes[n].local.insert(o.to_owned(), at);
            world.log.push(format!("open {o} on n{n}"));
            let index = Rc::clone(index);
            let o = o.to_owned();
            let task = async move {
                let _ = index.arm(NS, &o, None, Some(at)).await;
            };
            OPENS.with(|opens| opens.borrow_mut().push((n, Box::pin(task))));
        }

        /// Drive every task until it waits on the store, opens' included.
        fn run(&mut self) {
            self.dst.run();
            for (n, task) in OPENS.with(|opens| std::mem::take(&mut *opens.borrow_mut())) {
                self.dst.spawn(n, task);
            }
            self.dst.run();
        }

        /// One step: run, then do one thing.
        fn step(&mut self) {
            self.run();
            let alive = self.alive();
            let pending = self.dst.pending();
            match self.dst.rng.below(100) {
                0..45 if pending > 0 => self.decide(),
                45..50 if self.faults && alive.len() > 1 => {
                    let n = alive[self.dst.rng.below(alive.len())];
                    self.crash(n);
                    self.start();
                }
                50..62 if self.faults => {
                    let n = alive[self.dst.rng.below(alive.len())];
                    let o = format!("o{}", self.dst.rng.below(OBJECTS));
                    let at = self.world.borrow().now + self.dst.rng.below(30) as u64;
                    if self.dst.rng.chance(15) {
                        self.set(n, &o, None);
                    } else {
                        self.set(n, &o, Some(at));
                    }
                }
                62..67 if self.faults => {
                    let n = alive[self.dst.rng.below(alive.len())];
                    let o = format!("o{}", self.dst.rng.below(OBJECTS));
                    self.request(n, &o);
                }
                67..80 => {
                    let n = alive[self.dst.rng.below(alive.len())];
                    self.round(n);
                }
                80..92 => {
                    let n = alive[self.dst.rng.below(alive.len())];
                    self.fire(n);
                }
                _ => self.world.borrow_mut().now += 1,
            }
        }

        fn decide(&mut self) {
            let decided = self.dst.decide(self.faults);
            self.log(&decided);
        }

        /// A request to `o` through `n`: claims it if its owner is gone.
        fn request(&self, n: usize, o: &str) {
            let index = Rc::clone(self.indexes[n].as_ref().unwrap());
            let mut world = self.world.borrow_mut();
            if matches!(route(&world, n, o), Route::Claim) {
                Self::open(&mut world, &self.dst.store, &index, n, o);
            }
        }

        /// `setAlarm(at)`, or `deleteAlarm()` for `None`, on `o`'s owner as
        /// `ActorSqlite` runs it.
        fn set(&mut self, n: usize, o: &str, at: Option<u64>) {
            let index = Rc::clone(self.indexes[n].as_ref().unwrap());
            {
                let mut world = self.world.borrow_mut();
                match route(&world, n, o) {
                    Route::Here => {}
                    Route::Claim => Self::open(&mut world, &self.dst.store, &index, n, o),
                    Route::Remote(_) => return,
                }
                if !world.nodes[n].busy.insert(o.to_owned()) {
                    return;
                }
            }
            self.log(&format!("set {o} {at:?} on n{n}"));
            let (world, bucket, store) = (
                Rc::clone(&self.world),
                self.dst.bucket.clone(),
                Rc::clone(&self.dst.store),
            );
            let o = o.to_owned();
            self.dst.spawn(n, async move {
                let current = store.peek(&db_key(&o)).map(|b| parse_at(&b));
                let earlier = at.is_some_and(|at| current.is_none_or(|c| at < c));
                let commit = async {
                    match at {
                        Some(at) => bucket.put(&db_key(&o), at.to_string().into(), &[]).await,
                        None => bucket.delete(&db_key(&o)).await,
                    }
                };
                if earlier {
                    world.borrow_mut().nodes[n]
                        .local
                        .insert(o.clone(), at.unwrap());
                    // Indexed before the commit starts.
                    if index.arm(NS, &o, None, at).await.is_ok() {
                        let _ = commit.await;
                    }
                } else if commit.await.is_ok() {
                    {
                        let local = &mut world.borrow_mut().nodes[n].local;
                        match at {
                            Some(at) => local.insert(o.clone(), at),
                            None => local.remove(&o),
                        };
                    }
                    let _ = index.arm(NS, &o, None, at).await;
                }
                world.borrow_mut().nodes[n].busy.remove(&o);
            });
        }

        /// The alarms loop: adopt dead nodes' alarms, then tidy.
        fn round(&mut self, n: usize) {
            let index = Rc::clone(self.indexes[n].as_ref().unwrap());
            let (live, dead) = {
                let mut world = self.world.borrow_mut();
                if std::mem::replace(&mut world.nodes[n].round, true) {
                    return;
                }
                let world = &*world;
                let live = world.nodes.iter().filter(|x| x.alive);
                let dead = world
                    .nodes
                    .iter()
                    .filter(|x| !x.alive && world.now >= x.lease_until);
                (
                    live.map(|x| x.id.clone()).collect::<Vec<_>>(),
                    dead.map(|x| x.id.clone()).collect::<Vec<_>>(),
                )
            };
            let world = Rc::clone(&self.world);
            self.dst.spawn(n, async move {
                let schedule = |a: Adopted| {
                    let mut world = world.borrow_mut();
                    let adopted = format!("adopt {} {} on n{n}", a.actor, a.at_ms);
                    world.log.push(adopted);
                    let local = &mut world.nodes[n].local;
                    let at = local.get(&a.actor).map_or(a.at_ms, |&l| l.min(a.at_ms));
                    local.insert(a.actor, at);
                };
                index.adopt(&live, &dead, schedule).await;
                index.tidy().await;
                world.borrow_mut().nodes[n].round = false;
            });
        }

        /// The local scheduler fires `n`'s earliest due alarm, if any.
        fn fire(&mut self, n: usize) {
            let index = Rc::clone(self.indexes[n].as_ref().unwrap());
            let o = {
                let mut world = self.world.borrow_mut();
                let now = world.now;
                let node = &mut world.nodes[n];
                let due = node
                    .local
                    .iter()
                    .find(|(o, at)| **at <= now && !node.busy.contains(*o));
                let Some((o, _)) = due.map(|(o, at)| (o.clone(), *at)) else {
                    return;
                };
                node.local.remove(&o);
                match route(&world, n, &o) {
                    Route::Here => {}
                    Route::Claim => Self::open(&mut world, &self.dst.store, &index, n, &o),
                    Route::Remote(m) => {
                        world.log.push(format!("fire {o} on n{n}: on n{m}"));
                        let (now, owner) = (world.now, world.nodes[m].id.clone());
                        drop(world);
                        let world = Rc::clone(&self.world);
                        self.dst.spawn(n, async move {
                            // The scheduler retries it, unless dropped.
                            if index.elsewhere(NS, &o, &owner, now).await.unwrap_or(true) {
                                let mut world = world.borrow_mut();
                                let local = &mut world.nodes[n].local;
                                local.entry(o).or_insert(now + 2);
                            }
                        });
                        return;
                    }
                }
                // Opening may have rescheduled it.
                world.nodes[n].local.remove(&o);
                world.nodes[n].busy.insert(o.clone());
                o
            };
            let (world, bucket, store) = (
                Rc::clone(&self.world),
                self.dst.bucket.clone(),
                Rc::clone(&self.dst.store),
            );
            self.dst.spawn(n, async move {
                let now = world.borrow().now;
                match store.peek(&db_key(&o)).map(|b| parse_at(&b)) {
                    Some(at) if at <= now => {
                        world.borrow_mut().log.push(format!("run {o} {at} on n{n}"));
                        if bucket.delete(&db_key(&o)).await.is_ok() {
                            let _ = index.arm(NS, &o, None, None).await;
                        } else {
                            // The scheduler retries a failed alarm.
                            world.borrow_mut().nodes[n].local.insert(o.clone(), now + 1);
                        }
                    }
                    Some(at) => {
                        world.borrow_mut().nodes[n].local.insert(o.clone(), at);
                        let _ = index.arm(NS, &o, None, Some(at)).await;
                    }
                    None => {}
                }
                world.borrow_mut().nodes[n].busy.remove(&o);
            });
        }

        /// Stop faults and requests, which would reopen objects and so
        /// reschedule their alarms, crash every node once things are quiet,
        /// and run until every alarm had its chance; answers the objects
        /// whose alarm never ran.
        fn settle(&mut self) -> Vec<String> {
            self.faults = false;
            self.log("faults stop");
            for i in 0..40 {
                if i == 1 {
                    // Quiet now: crash every node at once, which leaves
                    // only the index to find each alarm by.
                    for n in self.alive() {
                        self.crash(n);
                        self.start();
                    }
                }
                // Drain every call, then let each node round and fire.
                loop {
                    self.run();
                    if self.dst.pending() == 0 {
                        break;
                    }
                    self.decide();
                }
                for n in self.alive() {
                    self.round(n);
                    self.fire(n);
                }
                self.world.borrow_mut().now += 10;
            }
            self.dst
                .store
                .keys("db/")
                .into_iter()
                .map(|k| k.strip_prefix("db/").unwrap().to_owned())
                .collect()
        }
    }

    type Task = std::pin::Pin<Box<dyn std::future::Future<Output = ()>>>;

    thread_local! {
        static OPENS: RefCell<Vec<(usize, Task)>> = RefCell::default();
    }

    fn parse_at(body: &[u8]) -> u64 {
        std::str::from_utf8(body).unwrap().parse().unwrap()
    }

    fn run(seed: u64, steps: usize) -> std::result::Result<(), String> {
        let mut sim = Sim::new(seed);
        for _ in 0..steps {
            sim.step();
        }
        let missed = sim.settle();
        if missed.is_empty() {
            return Ok(());
        }
        let world = sim.world.borrow();
        let mut log = world.log.join("\n");
        for (n, index) in sim.indexes.iter().enumerate() {
            let Some(index) = index else { continue };
            for o in &missed {
                let armed = index.armed.borrow();
                let a = armed.get(&(NS.to_owned(), o.clone()));
                let a = a.map(|a| (a.indexed.as_ref().map(|i| i.at_ms), a.want));
                let local = world.nodes[n].local.get(o);
                log += &format!("\nn{n} {o}: indexed/want {a:?} local {local:?}");
            }
        }
        log += &format!("\nbucket {:?}", sim.dst.store.keys(""));
        Err(format!("never ran {missed:?}\n{log}"))
    }

    /// 100 seeds by default, to fit CI's slowest builds.
    #[test]
    fn every_alarm_runs_whatever_fails() {
        crate::dst::seeds(100, |seed| run(seed, 400));
    }
}
