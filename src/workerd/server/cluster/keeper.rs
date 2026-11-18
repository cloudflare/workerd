// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Keeps this node's lease: renews its record in the bucket and decides
//! when the node must stop serving.
//!
//! A serving node keeps its lease on a thread of its own (`Thread`), with
//! its own tokio runtime and its own bucket client, so that a renewal waits
//! on the bucket alone. The server's event loop runs JavaScript, SQLite and
//! file work in turns of any length, and a renewal that waited for a turn
//! to end would lose the lease, and with it every object the node owns, to
//! one long turn.
//!
//! The lease thread shares two things with the loop thread: the lease's
//! expiry (`Held`), which the loop reads before it opens storage or starts
//! an actor, and the commands the loop sends it (`Command`). It writes the
//! node's record and nothing else; the loop reads the peers' records with a
//! registry of its own.
//!
//! The lease therefore says that the process runs and reaches the bucket,
//! not that its event loop takes turns. So once the node serves, the lease
//! thread also watches the loop (`cluster_logic::watchdog`): it reads a
//! count the loop advances while it makes progress (`Progress`), once a
//! round, and a count that stands still for `Timing::stall_ms` fences the
//! node as a lease that cannot be kept does. Not before: linking, after
//! `join`, runs in turns that last as long as it does.

use std::cell::Cell;
use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;
use std::time::Duration;

use cluster_logic::lease;
use cluster_logic::lease::Lease;
use cluster_logic::lease::Step;
use cluster_logic::lease::Timing;
use cluster_logic::watchdog::Watchdog;
use tokio::sync::mpsc;
use tokio::sync::oneshot;
use tokio::time::timeout;

use crate::Bucket;
use crate::Error;
use crate::Registry;
use crate::Result;
use crate::cluster::now_ms;
use crate::http::Repeat;
use crate::http::prompt;

/// The lease's expiry, as its keeper last renewed it: what the loop thread
/// reads. Clones share it.
#[derive(Clone)]
pub struct Held {
    /// 0 before the first renewal.
    until_ms: Arc<AtomicU64>,
    /// Whether the instance left the cluster (`Keeper::deregister`).
    left: Arc<AtomicBool>,
    timing: Timing,
}

impl Held {
    fn new(timing: Timing) -> Self {
        Self {
            until_ms: Arc::default(),
            left: Arc::default(),
            timing,
        }
    }

    /// Whether the lease holds at `now_ms`, with peers' clocks allowed for.
    pub fn holds(&self, now_ms: u64) -> bool {
        self.timing
            .holds(self.until_ms.load(Ordering::Acquire), now_ms)
    }

    /// How much longer than `now_ms` the lease holds: zero once it does
    /// not.
    pub fn remaining(&self, now_ms: u64) -> Duration {
        let until_ms = self.until_ms.load(Ordering::Acquire);
        ms(self.timing.remaining_ms(until_ms, now_ms))
    }

    /// [`Lease::holds_until`] of the lease its keeper keeps.
    pub fn holds_until(&self) -> u64 {
        if self.left.load(Ordering::Acquire) {
            return 0;
        }
        self.timing
            .holds_until(self.until_ms.load(Ordering::Acquire))
    }
}

/// The lease and the node's record, on whichever thread renews them.
pub struct Keeper {
    /// Writes this node's record.
    registry: Rc<Registry>,
    /// Measures this node's clock against the bucket's.
    bucket: Bucket,
    lease: RefCell<Lease>,
    held: Held,
    /// The loop the rounds watch, from `watch` on.
    watched: RefCell<Option<Progress>>,
}

impl Keeper {
    pub fn new(bucket: Bucket, registry: Rc<Registry>, timing: Timing) -> Self {
        Self {
            registry,
            bucket,
            lease: RefCell::new(Lease::new(timing)),
            held: Held::new(timing),
            watched: RefCell::new(None),
        }
    }

    /// From here on, the rounds watch the loop that `progress` counts.
    pub fn watch(&self, progress: Progress) {
        *self.watched.borrow_mut() = Some(progress);
    }

    pub fn held(&self) -> Held {
        self.held.clone()
    }

    /// Whether this instance left the cluster (`deregister`): it renews its
    /// lease no more. Answers no if the lease is borrowed, so that a drop
    /// that asks never panics.
    fn left(&self) -> bool {
        self.lease.try_borrow().is_ok_and(|lease| lease.left)
    }

    /// How far this node's wall clock is outside the bucket's, in ms; 0
    /// before any response.
    fn clock_skew_ms(&self) -> i64 {
        self.bucket.clock_skew_ms().unwrap_or(0)
    }

    /// Reports a renewal's outcome to the lease, and a new expiry to
    /// whoever reads `Held`.
    fn renewed(&self, result: std::result::Result<u64, String>) -> Step {
        self.renewed_at(now_ms(), result)
    }

    /// As `renewed`, for a renewal that answered at `now_ms`.
    pub(crate) fn renewed_at(&self, now_ms: u64, result: std::result::Result<u64, String>) -> Step {
        if let Ok(until_ms) = result {
            self.held.until_ms.store(until_ms, Ordering::Release);
        }
        self.lease.borrow_mut().renewed(now_ms, result)
    }

    /// Keeps the lease until the node must stop serving, or until the
    /// loop it watches (`watch`) makes no progress: answers why. Runs for
    /// the life of the process.
    pub async fn maintain(&self) -> String {
        let timing = self.held.timing;
        let started = std::time::Instant::now();
        // Its clock starts at its first observation, once there is a loop
        // to watch.
        let mut watchdog = Watchdog::new(&timing);
        loop {
            if self.lease.borrow().left {
                std::future::pending::<()>().await;
            }
            let observed = lease::Observed {
                now_ms: now_ms(),
                skew_ms: self.clock_skew_ms(),
            };
            let mut step = self.lease.borrow().before_renew(&observed);
            if step == Step::Renew {
                let renew = timeout(ms(timing.renew_timeout_ms), self.renew_round());
                let result = match renew.await {
                    Ok(Ok(until)) => Ok(until),
                    Ok(Err(e)) => Err(e.to_string()),
                    Err(_) => Err("renewal timed out".into()),
                };
                // The instance left meanwhile, which may have failed this
                // renewal: what it found no longer matters.
                if self.lease.borrow().left {
                    continue;
                }
                step = self.renewed(result);
            }
            let delay = match step {
                Step::Fence(reason) => return lease_lost(reason),
                Step::Retry(reason) => {
                    tracing::warn!("renewing the cluster lease failed; retrying: {reason}");
                    timing.retry_ms
                }
                Step::Renew => timing.interval_ms,
            };
            // After the lease: a round that a stop of the whole process
            // delayed finds the lease lapsed, and says so.
            let now_ms = u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX);
            let stalled = {
                let watched = self.watched.borrow();
                let progress = watched.as_ref();
                progress.and_then(|progress| watchdog.observe(now_ms, progress()))
            };
            if let Some(reason) = stalled {
                return reason;
            }
            tokio::time::sleep(ms(delay)).await;
        }
    }

    /// A lease round's renewal. A request of it that stalls is sent once
    /// more (`http::prompt`), so that one slow answer does not time the
    /// round out: two rounds timed out in a row fence the node. The
    /// renewal's guarded writes may be: it rereads on a refusal
    /// (`Registry::renew`).
    async fn renew_round(&self) -> Result<u64> {
        let repeat = &[Repeat::Read, Repeat::GuardedWrite];
        let attempt = ms(self.held.timing.renew_attempt_ms);
        prompt(attempt, repeat, self.registry.renew(now_ms())).await
    }

    /// Renews the lease outside the maintenance rounds, whether or not it
    /// lapsed: as the node joins, before the rounds start.
    pub async fn take_lease(&self) -> Result<()> {
        let until = self.registry.renew(now_ms()).await?;
        self.renewed(Ok(until));
        Ok(())
    }

    /// The record says that this instance drains from its next renewal.
    pub fn start_draining(&self) {
        self.registry.start_draining();
    }

    /// Publishes that this instance drains, so that peers choose it for no
    /// object or alarm.
    pub async fn announce_draining(&self) {
        let renew = timeout(ms(self.held.timing.renew_timeout_ms), self.renew_round());
        match renew.await {
            Ok(Ok(_)) => {}
            // The node left the cluster first: `deregister`, which runs at
            // once on a node with nothing to hand off, published its record
            // expired and draining.
            Ok(Err(Error::NotHere(_))) if self.registry.left() => {}
            Ok(Err(e)) => tracing::warn!("cluster: publishing the drain failed: {e}"),
            Err(_) => tracing::warn!("cluster: publishing the drain timed out"),
        }
    }

    /// Expires this instance's membership: it renews its lease no more.
    pub async fn deregister(&self) -> Result<()> {
        self.lease.borrow_mut().left = true;
        self.held.left.store(true, Ordering::Release);
        self.registry.deregister().await
    }
}

/// What the loop thread asks of the lease thread. Each runs as a task
/// beside the maintenance rounds, so none waits for a round's renewal.
enum Command {
    /// `Keeper::take_lease`.
    Take(oneshot::Sender<Result<()>>),
    /// Start `Keeper::maintain`, once.
    Maintain,
    /// `Keeper::watch`.
    Watch(Progress),
    /// `Keeper::start_draining`.
    Drain,
    /// `Keeper::announce_draining`.
    Announce(oneshot::Sender<()>),
    /// `Keeper::deregister`.
    Leave(oneshot::Sender<Result<()>>),
    /// End the thread, cancelling whatever it was doing.
    Stop,
    /// Panic in the thread's body, outside any task.
    #[cfg(test)]
    Panic,
}

/// Called on the lease thread, at most once, when the lease cannot be kept
/// or the loop makes no progress.
///
/// That is when its rounds end, or when the thread ends while they run
/// without being told to stop. Called with the reason, which says which
/// ("its lease cannot be kept: ...", "its event loop made no progress ..."),
/// and how much longer the lease holds, it must end the process itself,
/// within that time.
///
/// The process must be gone before peers may take the node's objects over,
/// and the loop thread may be in a turn that never ends.
pub type Fence = Box<dyn FnOnce(String, Duration) + Send>;

/// Runs the lease thread's body, on the lease thread, inside what the
/// process sets up for a thread of its own: its logger.
pub type Within = Box<dyn FnOnce(Box<dyn FnOnce() + Send>) + Send>;

/// Reads, on the lease thread, a count that the server's event loop
/// advances while it makes progress.
///
/// It stands still while one turn holds the loop, and through the busy
/// stretches that `kj_rs_tokio::Progress` names.
pub type Progress = Box<dyn Fn() -> u64 + Send>;

/// How long a lease thread told to stop may take to end. Stopping cancels
/// its requests, but tokio waits for work on its blocking pool, such as a
/// DNS lookup, that no cancellation reaches: the thread leaves such work
/// behind after `SHUTDOWN`, and its owner leaves the thread behind after
/// `STOP`.
const SHUTDOWN: Duration = Duration::from_millis(500);
const STOP: Duration = Duration::from_secs(1);

/// The lease thread, as the loop thread holds it. Dropping it ends the
/// thread and waits for it, within `STOP`.
pub struct Thread {
    commands: mpsc::UnboundedSender<Command>,
    held: Held,
    handle: Option<std::thread::JoinHandle<()>>,
}

impl Thread {
    /// Starts the lease thread for `node` at `address`, its body `within`
    /// the process's setup. `open` opens the thread's bucket client on it;
    /// what it answers is this call's answer.
    pub fn spawn(
        open: impl FnOnce() -> Result<Bucket> + Send + 'static,
        node: String,
        address: String,
        timing: Timing,
        fence: Fence,
        within: Within,
    ) -> Result<Self> {
        let (commands, received) = mpsc::unbounded_channel();
        let (started, start) = std::sync::mpsc::channel();
        let thread = std::thread::Builder::new().name("cluster-lease".into());
        let thread = thread.spawn(move || {
            within(Box::new(move || {
                let runtime = tokio::runtime::Builder::new_current_thread()
                    .enable_all()
                    .build();
                let keeper = runtime.map_err(Error::Io).and_then(|runtime| {
                    // A bucket's client is made inside the runtime it runs on.
                    let bucket = {
                        let _runtime = runtime.enter();
                        open()?
                    };
                    let registry = Registry::new(bucket.clone(), &node, &address, timing.lease_ms)
                        .contended_pause(ms(timing.contended_pause_ms));
                    Ok((runtime, Keeper::new(bucket, Rc::new(registry), timing)))
                });
                let (runtime, keeper) = match keeper {
                    Ok(keeper) => keeper,
                    Err(e) => {
                        let _ = started.send(Err(e));
                        return;
                    }
                };
                let _ = started.send(Ok(keeper.held()));
                let keeper = Rc::new(keeper);
                let armed = Armed::default();
                let tasks = tokio::task::LocalSet::new();
                // Declared after the tasks, so that a panic drops it first.
                let mut watch = Watch {
                    keeper: Rc::clone(&keeper),
                    armed: armed.clone(),
                    stopped: false,
                };
                let serving = serve(keeper, received, fence, armed);
                watch.stopped = tasks.block_on(&runtime, serving);
                // A body that ends any other way fences here, before it
                // waits for the tasks and the runtime to end.
                drop(watch);
                // The tasks' requests end with them.
                drop(tasks);
                runtime.shutdown_timeout(SHUTDOWN);
            }));
        })?;
        let held = start.recv().map_err(|_| stopped())??;
        Ok(Self {
            commands,
            held,
            handle: Some(thread),
        })
    }

    pub fn held(&self) -> &Held {
        &self.held
    }

    pub async fn take_lease(&self) -> Result<()> {
        self.ask(Command::Take).await?
    }

    /// Starts the maintenance rounds: from here on, the lease is renewed
    /// until the node leaves, and a lease that cannot be kept ends the
    /// process (`Fence`).
    pub fn maintain(&self) {
        self.send(Command::Maintain);
    }

    /// From here on, the rounds watch the loop that `progress` counts: one
    /// that makes no progress for `Timing::stall_ms` ends the process
    /// (`Fence`).
    pub fn watch(&self, progress: Progress) {
        self.send(Command::Watch(progress));
    }

    pub fn start_draining(&self) {
        self.send(Command::Drain);
    }

    pub async fn announce_draining(&self) {
        if self.ask(Command::Announce).await.is_err() {
            tracing::warn!("cluster: publishing the drain failed: {}", stopped());
        }
    }

    pub async fn deregister(&self) -> Result<()> {
        self.ask(Command::Leave).await?
    }

    fn send(&self, command: Command) {
        // A lease thread that is gone answers nothing, which `ask` reports;
        // what it no longer renews, `Held` stops holding.
        let _ = self.commands.send(command);
    }

    /// Sends the command `command` makes of a reply's sender, and waits
    /// for the reply.
    async fn ask<T>(&self, command: impl FnOnce(oneshot::Sender<T>) -> Command) -> Result<T> {
        let (reply, answer) = oneshot::channel();
        self.send(command(reply));
        answer.await.map_err(|_| stopped())
    }
}

impl Drop for Thread {
    fn drop(&mut self) {
        self.send(Command::Stop);
        let Some(handle) = self.handle.take() else {
            return;
        };
        // A thread that does not end in time is left to the process's exit.
        let deadline = std::time::Instant::now() + STOP;
        while !handle.is_finished() {
            if std::time::Instant::now() >= deadline {
                return;
            }
            std::thread::sleep(ms(1));
        }
        let _ = handle.join();
    }
}

/// What fences a node whose lease cannot be kept, for `reason`.
fn lease_lost(reason: impl std::fmt::Display) -> String {
    format!("its lease cannot be kept: {reason}")
}

fn stopped() -> Error {
    Error::Replica("the cluster lease thread stopped".into())
}

/// The fence, from when the maintenance rounds start: called by whichever
/// ends first, the rounds or the lease thread (`Watch`). Clones share it.
#[derive(Clone, Default)]
struct Armed(Rc<Cell<Option<Fence>>>);

impl Armed {
    fn arm(&self, fence: Fence) {
        self.0.set(Some(fence));
    }

    /// Calls the fence, unless it is not armed or was called already.
    fn fence(&self, reason: String, held: &Held) {
        if let Some(fence) = self.0.take() {
            fence(reason, held.remaining(now_ms()));
        }
    }
}

/// Fences, as it drops, a lease thread whose body ends other than by
/// `Command::Stop`: by a panic outside the tasks, or with its commands' channel
/// closed. Renewals that end with the thread end the process, as do rounds
/// that end. A node whose rounds have not started, or that left the cluster,
/// renews no lease that needs keeping, and is not fenced.
struct Watch {
    keeper: Rc<Keeper>,
    armed: Armed,
    /// The body ends by `Command::Stop`.
    stopped: bool,
}

impl Drop for Watch {
    fn drop(&mut self) {
        if !self.stopped && !self.keeper.left() {
            let reason = lease_lost("the lease thread stopped");
            self.armed.fence(reason, &self.keeper.held);
        }
    }
}

/// The lease thread's work: runs each command as a task of its own until
/// the loop thread drops the `Thread`. Answers whether it ended by
/// `Command::Stop`.
async fn serve(
    keeper: Rc<Keeper>,
    mut commands: mpsc::UnboundedReceiver<Command>,
    fence: Fence,
    armed: Armed,
) -> bool {
    let mut fence = Some(fence);
    while let Some(command) = commands.recv().await {
        let keeper = Rc::clone(&keeper);
        match command {
            Command::Take(reply) => {
                tokio::task::spawn_local(async move {
                    let _ = reply.send(keeper.take_lease().await);
                });
            }
            Command::Maintain => {
                let Some(fence) = fence.take() else { continue };
                armed.arm(fence);
                let armed = armed.clone();
                let held = keeper.held();
                let rounds = tokio::task::spawn_local(async move { keeper.maintain().await });
                tokio::task::spawn_local(async move {
                    // Rounds that ended any other way renew no more either.
                    let reason = rounds
                        .await
                        .unwrap_or_else(|e| lease_lost(format!("the lease's task: {e}")));
                    armed.fence(reason, &held);
                });
            }
            Command::Watch(progress) => keeper.watch(progress),
            Command::Drain => keeper.start_draining(),
            Command::Announce(reply) => {
                tokio::task::spawn_local(async move {
                    keeper.announce_draining().await;
                    let _ = reply.send(());
                });
            }
            Command::Leave(reply) => {
                tokio::task::spawn_local(async move {
                    let _ = reply.send(keeper.deregister().await);
                });
            }
            Command::Stop => return true,
            #[cfg(test)]
            Command::Panic => panic!("the lease thread panics"),
        }
    }
    false
}

/// Where this node's lease is kept.
pub enum Lessee {
    /// On the lease thread: a serving node's.
    Thread(Thread),
    /// On the caller's thread, with the cluster's own bucket and registry:
    /// a test's, which decides the time of each renewal itself and has no
    /// rounds.
    #[cfg(test)]
    Here(Keeper),
}

impl Lessee {
    pub fn held(&self) -> &Held {
        match self {
            Self::Thread(thread) => thread.held(),
            #[cfg(test)]
            Self::Here(keeper) => &keeper.held,
        }
    }

    pub async fn take_lease(&self) -> Result<()> {
        match self {
            Self::Thread(thread) => thread.take_lease().await,
            #[cfg(test)]
            Self::Here(keeper) => keeper.take_lease().await,
        }
    }

    pub fn watch(&self, progress: Progress) {
        match self {
            Self::Thread(thread) => thread.watch(progress),
            #[cfg(test)]
            Self::Here(keeper) => keeper.watch(progress),
        }
    }

    pub fn start_draining(&self) {
        match self {
            Self::Thread(thread) => thread.start_draining(),
            #[cfg(test)]
            Self::Here(keeper) => keeper.start_draining(),
        }
    }

    pub async fn announce_draining(&self) {
        match self {
            Self::Thread(thread) => thread.announce_draining().await,
            #[cfg(test)]
            Self::Here(keeper) => keeper.announce_draining().await,
        }
    }

    pub async fn deregister(&self) -> Result<()> {
        match self {
            Self::Thread(thread) => thread.deregister().await,
            #[cfg(test)]
            Self::Here(keeper) => keeper.deregister().await,
        }
    }
}

const fn ms(ms: u64) -> Duration {
    Duration::from_millis(ms)
}

#[cfg(test)]
#[path = "keeper-test.rs"]
mod tests;
