// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `workerd serve --loops=<L>`: the loops as units on one work-stealing scheduler.
//!
//! Without `--loops`, a loop is a thread: `--threads=<N>` runs N of them, each with a KJ event
//! loop it parks in a tokio `current_thread` runtime of its own (`threads.rs`). With it, the
//! process has one tokio multi-thread runtime with `--threads` workers, and each of the L loops
//! is a unit (`kj_rs_tokio::LoopUnit`): a KJ event loop, its `WorkerFactory`, its service graph
//! and its connections, driven by one task. Whichever worker picks the task up enters the loop,
//! turns its events until none is left, and leaves it, so a loop that has work runs on whichever
//! thread is free, and there can be more loops than threads.
//!
//! What a loop is does not change: the first one, the home unit, is what `threads.rs` calls the
//! first loop thread, and has what a process has one of (the sockets' binding, the config's
//! errors, SIGTERM, the `--control-fd` events, UDP, cron, the debug port, every Durable Object
//! and the cluster); the others are replicas, built by `run::run_replica` over duplicates of the
//! home unit's listening sockets, and reach the home unit over the same in-process sessions.
//! Start, failure, drain and teardown follow `threads.rs` too, with a task where it has a
//! thread; this module has what differs.
//!
//! - **Placement.** Every unit accepts from its duplicate of each listening socket, all
//!   registered with the one runtime's I/O driver, so a new connection goes to whichever unit
//!   accepts it first, and stays with it. A unit that is busy running JavaScript is not
//!   accepting, so connections tend to go to units that are not. A request waits for the events
//!   ahead of it on its own unit, and for no other unit's: with L units over N threads, the
//!   connections that one long turn of JavaScript holds up are those of one unit, about 1/L of
//!   them.
//! - **Isolates.** The units share the Workers the config defines: each is compiled once
//!   (`--isolates=<R>`: R times), by the first unit to need the copy, and every unit serves it
//!   through a view of its own (`factory_new_worker`). One thread runs JavaScript in an isolate
//!   at a time, whichever units' requests wait for it, so a Worker with one copy uses one core
//!   for JavaScript at most. Unit `u` uses copy `u % R`; the home unit, where the Durable
//!   Objects are, uses copy 0. The dynamic Workers of a `workerLoader` are each unit's own.
//! - **Threads.** A unit is `Send` by an `unsafe` claim (kj-rs-tokio, `ffi.rs`), whose
//!   conditions the code on the loop must keep: no thread-local may hold a piece of a unit
//!   between polls, and what a unit shares with another is thread-safe. The server crate holds
//!   no thread-local and no global of its own besides the `tracing` subscriber and the error
//!   flag (`entry.rs`), which are process-wide and atomic; its `Rc` graph is reachable only
//!   from the unit's root future. A unit with a fiber (`HTMLRewriter`) is pinned: it keeps its
//!   thread, blocking it, until the fiber is done (`workerd/util/loop-pin.h`).
//! - **Logging.** A `kj::ExceptionCallback` belongs to the stack of one thread, so the JSON
//!   logger is set up around every entry of a thread into a unit ([`EntryScope`]). Log lines
//!   written by a worker thread outside a unit (tokio's own tasks) are not structured.
//! - **Teardown.** A unit's root future drops its server and its factory, in that order, on the
//!   loop; the poll that completes it then destroys the loop, still entered. The home unit
//!   waits for the others' tasks to end before it drops its own server (`threads::Replicas`),
//!   and the main thread, which blocks on the home unit's task, then shuts the runtime down and
//!   drops the last reference to the `SharedFactory`, and with it V8.

use std::future::Future;
use std::time::Duration;

use kj_rs::KjArc;
use kj_rs::KjOwn;
use kj_rs_tokio::LoopUnit;
use kj_rs_tokio::UnitScope;

use crate::Result;
use crate::bridge::ffi;

/// A worker thread's stack: what the main thread usually has (see
/// `threads::REPLICA_STACK_SIZE`), since any worker runs any unit.
const WORKER_STACK_SIZE: usize = 8 * 1024 * 1024;

/// How long a thread the runtime has no use for stays: long enough that the threads a pinned
/// unit takes (`tokio::task::block_in_place` hands the worker's place to another thread) are
/// reused rather than replaced. V8 keeps a record per thread that ever locked an isolate, so
/// threads that come and go would add up.
const THREAD_KEEP_ALIVE: Duration = Duration::from_hours(24);

/// One entry of a thread into a loop unit: the body to run inside the C++ frame that sets up
/// the thread's logging (`with_loop_entry_context`).
pub struct LoopEntry<'a>(pub(crate) &'a mut dyn FnMut());

pub fn run_loop_entry(entry: &mut LoopEntry<'_>) {
    (entry.0)();
}

/// What every thread has on its stack while it has a unit of this process entered: the JSON
/// logger, if logging is structured.
#[derive(Clone, Copy)]
pub struct EntryScope {
    structured: bool,
}

impl EntryScope {
    #[must_use]
    pub fn new(structured: bool) -> Self {
        Self { structured }
    }
}

impl UnitScope for EntryScope {
    fn enter(&self, body: &mut dyn FnMut()) {
        if self.structured {
            crate::bridge::with_loop_entry_context(true, body);
        } else {
            body();
        }
    }
}

/// The process's scheduler: a multi-thread runtime of `threads` workers.
pub fn runtime(threads: usize) -> Result<tokio::runtime::Runtime> {
    tokio::runtime::Builder::new_multi_thread()
        .worker_threads(threads)
        .thread_name("loop-worker")
        .thread_stack_size(WORKER_STACK_SIZE)
        .thread_keep_alive(THREAD_KEEP_ALIVE)
        .enable_all()
        .build()
        .map_err(|e| kj::failed!("starting the scheduler: {e}"))
}

/// What a unit is in its process.
#[derive(Clone, Copy)]
pub struct Role {
    /// Whether logging is structured.
    pub structured: bool,
    /// Whether this is the home unit, whose factory is the primary loop's.
    pub primary: bool,
    /// Which of the process's copies of each Worker the unit serves.
    pub isolate_copy: u32,
}

/// A unit of the process of `shared`: a loop with a factory on it, which `serve` gets to run
/// the unit's server on and drops before it completes.
pub fn unit<F>(
    shared: KjArc<ffi::SharedFactory>,
    role: Role,
    serve: impl FnOnce(KjOwn<ffi::WorkerFactory>) -> F + Send + 'static,
) -> LoopUnit<Result<()>, EntryScope>
where
    F: Future<Output = Result<()>> + 'static,
{
    LoopUnit::new(EntryScope::new(role.structured), move |context| {
        let factory = ffi::bootstrap_unit(&shared, context, role.primary, role.isolate_copy);
        // The reference this closure holds goes with it, here; the factory has its own.
        drop(shared);
        async move { serve(factory?).await }
    })
}
