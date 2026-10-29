// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Deterministic simulation testing. A `Harness` runs a test's nodes as
//! tasks on one thread over a bucket whose every call waits for a verdict:
//! applied, failed before it applies, or failed after. A seed picks which
//! call is decided when, how, and what the test does in between, so a
//! failing seed replays exactly. Crashing a node drops its tasks wherever
//! they wait. The test brings the world it models and the invariant.

use std::cell::RefCell;
use std::rc::Rc;

use futures::FutureExt;
use futures::channel::oneshot;
use futures::executor::LocalPool;
use futures::future::AbortHandle;
use futures::future::Abortable;
use futures::task::LocalSpawnExt;

use crate::Bucket;
use crate::Error;
use crate::Guard;
use crate::Result;
use crate::Store as _;
use crate::Version;
use crate::bucket::Entry;
use crate::bucket::Payload;
use crate::bucket::memory::Memory;

pub struct Rng(u64);

impl Rng {
    pub fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    pub fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }

    pub fn chance(&mut self, percent: u64) -> bool {
        self.next() % 100 < percent
    }
}

enum Verdict {
    Apply,
    FailBefore,
    FailAfter,
}

/// A call waiting for its verdict.
struct Call {
    what: String,
    verdict: oneshot::Sender<Verdict>,
}

/// The memory store, each call held until the harness decides it.
#[derive(Default)]
pub struct Store {
    inner: Memory,
    calls: RefCell<Vec<Call>>,
}

impl Store {
    async fn decide(&self, what: String) -> Verdict {
        let (verdict, decided) = oneshot::channel();
        self.calls.borrow_mut().push(Call { what, verdict });
        decided.await.unwrap_or(Verdict::FailBefore)
    }

    /// The object at `key`, read past the verdicts.
    pub fn peek(&self, key: &str) -> Option<Vec<u8>> {
        let got = self.inner.get(key, None).now_or_never().unwrap();
        got.unwrap().map(|(body, _)| body)
    }

    /// The keys under `prefix`, read past the verdicts.
    pub fn keys(&self, prefix: &str) -> Vec<String> {
        let listed = self.inner.list(prefix).now_or_never().unwrap();
        listed.unwrap().into_iter().map(|e| e.key).collect()
    }
}

fn failed() -> Error {
    Error::Http("503 (simulated)".into())
}

/// The harness's handle on the store, shared by every bucket.
#[derive(Clone)]
struct Shared(Rc<Store>);

#[async_trait::async_trait(?Send)]
impl crate::Store for Shared {
    async fn get(
        &self,
        key: &str,
        to: Option<&std::path::Path>,
    ) -> Result<Option<(Vec<u8>, Version)>> {
        match self.0.decide(format!("get {key}")).await {
            Verdict::Apply => self.0.inner.get(key, to).await,
            _ => Err(failed()),
        }
    }

    async fn put(
        &self,
        key: &str,
        body: Payload,
        guard: Option<Guard>,
        metadata: &[(&str, String)],
    ) -> Result<Option<Version>> {
        let verdict = self.0.decide(format!("put {key}")).await;
        if matches!(verdict, Verdict::FailBefore) {
            return Err(failed());
        }
        let put = self.0.inner.put(key, body, guard, metadata).await;
        if matches!(verdict, Verdict::FailAfter) {
            return Err(failed());
        }
        put
    }

    async fn list(&self, prefix: &str) -> Result<Vec<Entry>> {
        match self.0.decide(format!("list {prefix}")).await {
            Verdict::Apply => self.0.inner.list(prefix).await,
            _ => Err(failed()),
        }
    }

    async fn delete(&self, key: &str) -> Result<()> {
        let verdict = self.0.decide(format!("delete {key}")).await;
        if matches!(verdict, Verdict::FailBefore) {
            return Err(failed());
        }
        self.0.inner.delete(key).await?;
        if matches!(verdict, Verdict::FailAfter) {
            return Err(failed());
        }
        Ok(())
    }
}

pub struct Harness {
    pub rng: Rng,
    pub store: Rc<Store>,
    /// Over `store`; clones share it.
    pub bucket: Bucket,
    /// Each node's tasks.
    aborts: Vec<Vec<AbortHandle>>,
    pool: LocalPool,
}

impl Harness {
    pub fn new(seed: u64) -> Self {
        let store = Rc::new(Store::default());
        Self {
            rng: Rng(seed.wrapping_mul(0x9e37_79b9_7f4a_7c15) | 1),
            bucket: Bucket::new(Shared(Rc::clone(&store)), ""),
            store,
            aborts: Vec::new(),
            pool: LocalPool::new(),
        }
    }

    /// A node to spawn tasks on; answers its number.
    pub fn node(&mut self) -> usize {
        self.aborts.push(Vec::new());
        self.aborts.len() - 1
    }

    /// Drop `n`'s tasks wherever they wait.
    pub fn crash(&mut self, n: usize) {
        for abort in self.aborts[n].drain(..) {
            abort.abort();
        }
    }

    pub fn spawn(&mut self, n: usize, task: impl Future<Output = ()> + 'static) {
        let (abort, registration) = AbortHandle::new_pair();
        self.aborts[n].push(abort);
        let task = Abortable::new(task, registration).map(drop);
        self.pool.spawner().spawn_local(task).unwrap();
    }

    /// Run every task until each waits on a verdict or has finished.
    pub fn run(&mut self) {
        self.pool.run_until_stalled();
        let mut calls = self.store.calls.borrow_mut();
        calls.retain(|c| !c.verdict.is_canceled());
    }

    /// How many calls wait for a verdict.
    pub fn pending(&self) -> usize {
        self.store.calls.borrow().len()
    }

    /// Decide one waiting call, failing it now and then with `faults`;
    /// answers what was decided, for the test's log.
    pub fn decide(&mut self, faults: bool) -> String {
        let i = self.rng.below(self.pending());
        let call = self.store.calls.borrow_mut().swap_remove(i);
        let (verdict, tag) = match self.rng.below(100) {
            _ if !faults => (Verdict::Apply, "ok"),
            0..8 => (Verdict::FailBefore, "fail"),
            8..16 => (Verdict::FailAfter, "fail-after"),
            _ => (Verdict::Apply, "ok"),
        };
        let _ = call.verdict.send(verdict);
        format!("{tag} {}", call.what)
    }
}

/// Run `DST_SEEDS` seeds (`default` if unset) from `DST_SEED` (0), panicking
/// with the first failure's report.
pub fn seeds(default: u64, run: impl Fn(u64) -> std::result::Result<(), String>) {
    let var = |name, or| std::env::var(name).map_or(or, |s| s.parse().unwrap());
    let first: u64 = var("DST_SEED", 0);
    for seed in first..first + var("DST_SEEDS", default) {
        if let Err(e) = run(seed) {
            panic!("seed {seed}: {e}");
        }
    }
}
