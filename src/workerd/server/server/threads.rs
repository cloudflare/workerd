// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The loop threads of `workerd serve --threads=<N>` other than the first: the replicas.
//!
//! The first loop thread is the process's main thread and has everything a process has one of:
//! it binds the sockets, reports the config's errors, handles SIGTERM, writes the `--control-fd`
//! events, and serves the UDP sockets, the cron triggers and the debug port. It also has the
//! Durable Objects: every actor with its storage, alarms, facets, container and hibernated
//! WebSockets, and in cluster mode the instance's whole part in the cluster (its lease, the
//! objects it owns, the peer port and sessions, the bucket's client).
//!
//! A replica is a thread with its own event loop (`kj_rs_tokio::Runtime`), its own
//! `WorkerFactory` over the process's `SharedFactory` (`bootstrap_loop`), and its own copy of the
//! service graph, built from the same config by the same code (`run::run_replica`): every Worker
//! is compiled once per thread, so a Worker's global scope runs on each and its global state is
//! per thread, and a service binding reaches the Worker of the same thread. So are the dynamic
//! Workers of a `workerLoader`: each thread loads its own, and under one name the code its
//! `getCode` callback returned on that thread, so if the callback can return different code for
//! one name, threads can run different code under it. It accepts
//! connections from duplicates of the first thread's stream sockets, which share their accept
//! queues, so a connection is served by whichever thread accepts it.
//!
//! What a replica lacks it reaches on the first thread, over its session with it
//! ([`FirstThread`]): a capnp `WorkerdDebugPort` session, as a cluster instance keeps with a
//! peer, over an in-process pipe made when the replica starts. A replica's Durable Object
//! namespaces have no storage and no actors: the channel of a stub is a capability of the session
//! for the channel the first thread gives a stub of its own, so the first thread treats the
//! request of a replica as it treats a request it accepted itself, in a cluster too
//! (`actor::ForwardedChannel`). A `bucket` service is forwarded the same way. HTTP with its
//! bodies, WebSockets and JS RPC cross the session as capnp; a stub or a service binding passed
//! in a JS RPC call crosses as a channel token, which every thread decodes with the process's key
//! and resolves in its own graph. A stub whose source is an object is restored (`restore()`)
//! from a replica by an event of the same session, whose parameters carry their capabilities as
//! channel tokens too; the object's `[restore]()` runs on the first thread.
//!
//! What the threads share besides is [`Phase`], which the first thread sets and the replicas
//! follow, and the events a replica sends back: that it is ready, and how it ended.
//!
//! - **Start.** The first thread builds its graph first. Only a config it accepts without an
//!   error gets replicas ([`Replicas::start`]), which build theirs at the same time; an error in
//!   a replica's graph is that thread's failure, not a config error reported again. (What the
//!   first thread attributed to a cluster deployment, a replica attributes to it too, and
//!   serves.) Once every replica is ready the first thread writes its `listen` events and
//!   [`Replicas::serve`] lets the replicas accept, as it then does itself.
//! - **Failure.** A replica that fails, or stops serving before the server drains, fails the
//!   future [`Started::watch`] makes, which the first thread serves as one of its listeners:
//!   the process ends with that error as it does with a listener's. One that has ended before
//!   the first thread advertises its cluster lease or writes its `listen` events fails the
//!   start instead ([`Started::check`]). Its pipe closes with its
//!   loop, which cancels what it had in flight on the first thread, as a client hanging up
//!   does. A replica whose session ends while it serves fails the same way.
//! - **Drain.** [`Replicas::drain`] has every replica stop accepting and finish its connections
//!   as the first thread does; that future resolves once they all have. The first thread keeps
//!   its actors and serves the sessions until then, so a replica's requests in flight finish as
//!   its own do; only then does it stop its containers. In a cluster the replicas accept for as
//!   long as the first thread does, through its handoff and the grace period after it.
//! - **Teardown.** A replica drops its server, then its factory, then its loop, on its own
//!   thread, and waits for nothing of the first thread to do so: what it has in flight there are
//!   futures it drops, and the pipe's wake-ups cross threads without waiting. Dropping the
//!   `Replicas` tells those still running to stop at once and joins every thread, so the first
//!   thread's server, which serves the sessions, and its factory, and with it the process's
//!   `SharedFactory` and V8, outlive every other loop.

use std::cell::RefCell;
use std::rc::Rc;
use std::thread::JoinHandle;

use futures::future::LocalBoxFuture;
use kj_rs::KjArc;
use kj_rs::KjOwn;
use tokio::io::DuplexStream;
use tokio::sync::mpsc;
use tokio::sync::watch;

use crate::Result;
use crate::bridge::ffi;
use crate::config::Deployments;
use crate::config::Factory;
use crate::config::Overrides;
use crate::entry::LoopThread;
use crate::listen::DuplicateSockets;

/// Where the process is in its life, for the replicas to follow. It only advances.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Phase {
    /// The threads are building their graphs; none accepts connections.
    Starting,
    /// Every thread accepts connections.
    Serving,
    /// No thread accepts connections; each finishes the ones it has.
    Draining,
    /// The process is ending without waiting for connections.
    Stopping,
}

/// What a replica's thread tells the first.
enum Event {
    /// The thread built its graph and waits for [`Phase::Serving`].
    Ready,
    /// The thread ended, its loop gone: the index of the thread and how its run ended.
    Exited(usize, Result<()>),
}

/// The stack of a replica's thread: the 8 MiB a main thread usually gets (Linux's default
/// `ulimit -s`), not the 2 MiB of a thread Rust spawns, so that what runs on the first thread runs
/// on a replica too. V8 keeps JavaScript within its own limit, below either; the rest of the stack
/// is for native code that recurses, such as parsers and SQLite.
const REPLICA_STACK_SIZE: usize = 8 * 1024 * 1024;

/// A replica's end of what the threads share.
pub struct Signals {
    phase: watch::Receiver<Phase>,
    events: mpsc::UnboundedSender<Event>,
}

impl Signals {
    /// Tells the first thread that this one is ready to serve.
    pub fn ready(&self) {
        // The first thread stopped listening only if it is ending, which the phase says.
        let _ = self.events.send(Event::Ready);
    }

    /// Resolves once the process is in `phase` or a later one, with the one it is in. A first
    /// thread that is gone is `Stopping`.
    pub async fn reached(&self, phase: Phase) -> Phase {
        let mut current = self.phase.clone();
        match current.wait_for(|current| *current >= phase).await {
            Ok(current) => *current,
            Err(_) => Phase::Stopping,
        }
    }
}

/// The replicas of a run, for the first loop thread. Dropping it stops and joins them.
pub struct Replicas {
    count: usize,
    shared: KjArc<ffi::SharedFactory>,
    phase: watch::Sender<Phase>,
    threads: RefCell<Vec<JoinHandle<()>>>,
}

impl Replicas {
    /// `count` replicas of the server of `factory`, the first loop thread's; none runs yet.
    #[must_use]
    pub fn new(count: usize, factory: &Factory) -> Self {
        Self {
            count,
            shared: factory.shared(),
            phase: watch::Sender::new(Phase::Starting),
            threads: RefCell::new(Vec::new()),
        }
    }

    #[must_use]
    pub fn count(&self) -> usize {
        self.count
    }

    /// Starts the threads, one for each of `sockets`, and waits until each has built its graph.
    /// A thread that fails to, or has ended by then, is the error.
    ///
    /// What it returns watches the threads from then on ([`Started`]).
    pub async fn start(
        &self,
        factory: &Rc<Factory>,
        sockets: Vec<DuplicateSockets>,
        overrides: &Overrides,
        deployments: &Deployments,
    ) -> Result<Started> {
        let (events, received) = mpsc::unbounded_channel();
        let count = sockets.len();
        for (index, sockets) in (1..).zip(sockets) {
            let (ours, theirs) = session_pipe();
            factory.spawn_detached(serve_session(Rc::clone(factory), index, ours));
            let replica = Replica {
                shared: self.shared.clone(),
                sockets,
                overrides: overrides.clone(),
                deployments: deployments.clone(),
                signals: Signals {
                    phase: self.phase.subscribe(),
                    events: events.clone(),
                },
                session: theirs,
            };
            let exit = ExitReport {
                index,
                events: events.clone(),
                result: None,
            };
            let thread = std::thread::Builder::new()
                .name(format!("loop-{index}"))
                .stack_size(REPLICA_STACK_SIZE)
                .spawn(move || replica.run_thread(exit))
                .map_err(|e| kj::failed!("starting loop thread {index}: {e}"))?;
            self.threads.borrow_mut().push(thread);
        }
        drop(events);
        await_ready(received, self.phase.subscribe(), count).await
    }

    /// Lets the replicas accept connections.
    pub fn serve(&self) {
        self.advance(Phase::Serving);
    }

    /// Has the replicas stop accepting connections and finish the ones they have.
    pub fn drain(&self) {
        self.advance(Phase::Draining);
    }

    fn advance(&self, phase: Phase) {
        self.phase.send_if_modified(|current| {
            let advances = *current < phase;
            if advances {
                *current = phase;
            }
            advances
        });
    }
}

impl Drop for Replicas {
    fn drop(&mut self) {
        self.advance(Phase::Stopping);
        for thread in std::mem::take(self.threads.get_mut()) {
            // A thread that panicked aborted the process: a panic does not unwind through the
            // C++ frame a loop thread runs under.
            let _ = thread.join();
        }
    }
}

/// The error of the replica `index` that ended with `result`; `what` says what it did if that
/// was not an error.
fn exit_error(index: usize, result: Result<()>, what: &str) -> crate::Error {
    match result {
        Ok(()) => kj::failed!("loop thread {index} {what}"),
        Err(error) => {
            let description = format!("loop thread {index}: {}", error.description());
            let located = crate::Error::new(error.exception_type(), description);
            match (error.file(), error.line()) {
                (Some(file), Some(line)) => located.with_location(file.to_owned(), line),
                _ => located,
            }
        }
    }
}

/// The error of a thread that ended without saying how.
fn lost_thread() -> crate::Error {
    kj::failed!("a loop thread ended without reporting how")
}

/// The replicas once each has built its graph, as [`Replicas::start`] returns them.
pub struct Started {
    events: mpsc::UnboundedReceiver<Event>,
    phase: watch::Receiver<Phase>,
    count: usize,
}

impl Started {
    /// Fails with the error of a replica that has ended since, if one has. The first thread
    /// checks before it tells anyone that it serves: before it advertises its cluster lease, and
    /// before it writes its `listen` events.
    pub fn check(&mut self) -> Result<()> {
        loop {
            match self.events.try_recv() {
                Ok(Event::Ready) => {}
                Ok(Event::Exited(index, result)) => {
                    return Err(exit_error(index, result, "ended while starting"));
                }
                Err(mpsc::error::TryRecvError::Empty) => return Ok(()),
                Err(mpsc::error::TryRecvError::Disconnected) => return Err(lost_thread()),
            }
        }
    }

    /// Watches the threads for the rest of the run: fails when one fails or stops serving before
    /// the server drains, and resolves once every one has ended. The first thread serves it as one
    /// of its listeners.
    #[must_use]
    pub fn watch(self) -> LocalBoxFuture<'static, Result<()>> {
        Box::pin(watch_threads(self.events, self.phase, self.count))
    }
}

/// Waits through `events` until `count` threads are ready; a thread that ended first, or has
/// ended by then, is the error.
async fn await_ready(
    mut events: mpsc::UnboundedReceiver<Event>,
    phase: watch::Receiver<Phase>,
    count: usize,
) -> Result<Started> {
    let mut ready = 0;
    while ready < count {
        match events.recv().await {
            Some(Event::Ready) => ready += 1,
            Some(Event::Exited(index, result)) => {
                return Err(exit_error(index, result, "ended while starting"));
            }
            None => return Err(lost_thread()),
        }
    }
    let mut started = Started {
        events,
        phase,
        count,
    };
    started.check()?;
    Ok(started)
}

/// Watches `running` threads through their events: see [`Started::watch`].
async fn watch_threads(
    mut events: mpsc::UnboundedReceiver<Event>,
    phase: watch::Receiver<Phase>,
    mut running: usize,
) -> Result<()> {
    while running > 0 {
        match events.recv().await {
            Some(Event::Ready) => {}
            Some(Event::Exited(index, result)) => {
                if result.is_err() || *phase.borrow() < Phase::Draining {
                    return Err(exit_error(index, result, "stopped serving"));
                }
                running -= 1;
            }
            None => return Err(lost_thread()),
        }
    }
    Ok(())
}

/// Reports how a replica's thread ended when the thread is done with its loop, however it gets
/// there.
struct ExitReport {
    index: usize,
    events: mpsc::UnboundedSender<Event>,
    result: Option<Result<()>>,
}

impl Drop for ExitReport {
    fn drop(&mut self) {
        let index = self.index;
        let result = self
            .result
            .take()
            .unwrap_or_else(|| Err(kj::failed!("the thread panicked")));
        // Nobody listens once the first thread is ending.
        let _ = self.events.send(Event::Exited(index, result));
    }
}

/// One replica, on its way to its thread.
struct Replica {
    shared: KjArc<ffi::SharedFactory>,
    sockets: DuplicateSockets,
    overrides: Overrides,
    /// The services of the config that came from cluster deployments.
    deployments: Deployments,
    signals: Signals,
    /// Its end of its session with the first thread.
    session: SessionEnd,
}

impl Replica {
    /// The thread: the replica's whole life, reported through `exit`.
    fn run_thread(self, mut exit: ExitReport) {
        // The body borrows nothing of this frame, so it takes its own reference to the shared
        // factory; this frame's is for the C++ frame around the body.
        let shared = self.shared.clone();
        let body = LoopThread::new(move || self.run());
        let result = ffi::with_loop_thread_context(&shared, body);
        exit.result = Some(result.map_err(crate::Error::from));
    }

    /// The loop, the factory on it, and the server until it ends; dropped in reverse.
    fn run(self) -> Result<()> {
        let mut runtime = kj_rs_tokio::Runtime::new()?;
        let factory = ffi::bootstrap_loop(&self.shared, runtime.context())?;
        // `serve` owns the factory and drops it when it completes; if it does not complete,
        // `block_on` drops it, and the factory with it, before returning.
        let result = runtime.block_on(self.serve(factory));
        drop(runtime);
        result?
    }

    async fn serve(self, factory: KjOwn<ffi::WorkerFactory>) -> Result<()> {
        let Self {
            sockets,
            overrides,
            deployments,
            signals,
            session,
            ..
        } = self;
        let factory = Rc::new(Factory::new(factory));
        factory.forward_to_first_thread(session);
        let serving = Rc::clone(&factory);
        let result =
            crate::run::run_replica(serving, overrides, deployments, sockets, &signals).await;
        let shutdown = crate::entry::shutdown(factory).await;
        result?;
        shutdown
    }
}

// =====================================================================================
// Sessions

/// Each direction of a session's pipe buffers this much before the writer waits.
const SESSION_BUFFER_SIZE: usize = 256 * 1024;

/// One end of the in-process pipe that a replica's session with the first loop thread runs over.
///
/// It is made on the first thread and goes to the thread that uses it, which makes a KJ stream
/// of it on its own loop.
pub struct SessionEnd(DuplexStream);

/// The two ends of a session's pipe.
#[must_use]
pub fn session_pipe() -> (SessionEnd, SessionEnd) {
    let (a, b) = tokio::io::duplex(SESSION_BUFFER_SIZE);
    (SessionEnd(a), SessionEnd(b))
}

/// Serves, on the first loop thread, the session of replica `index` over `end`, until the
/// replica drops its end.
pub async fn serve_session(factory: Rc<Factory>, index: usize, end: SessionEnd) {
    let stream = kj_hyper::into_kj_stream(end.0);
    if let Err(error) = ffi::factory_accept_loop_session(factory.raw(), stream).await {
        let error = crate::Error::from(error);
        // The replica is told by its own end; this is for whoever reads the log.
        if error.exception_type() != cxx::KjExceptionType::Disconnected {
            tracing::error!(
                "the session of loop thread {index} failed: {}",
                error.description()
            );
        }
    }
}

/// A replica's session with the first loop thread, for the replica: a capnp `WorkerdDebugPort`
/// session, the one a cluster node keeps with a peer, over an in-process pipe.
///
/// A replica has no actors: each stub it makes is a capability of this session for the actor's
/// channel on the first thread, and each request on it an event started there
/// (`actor::ForwardedChannel`). Calls made over one stub arrive in the order they were made.
/// Unlike a session with another instance, it carries `restore()` events
/// (`new_loop_session_client`).
pub struct FirstThread {
    client: KjOwn<ffi::RpcClient>,
}

impl FirstThread {
    /// The session over `end`, on the loop of `factory`, this replica's.
    pub fn connect(factory: &ffi::WorkerFactory, end: SessionEnd) -> Self {
        Self {
            client: ffi::new_loop_session_client(factory, kj_hyper::into_kj_stream(end.0)),
        }
    }

    /// The actor `key` (the hex id of a durable actor, derived from `name` if that is known;
    /// the name of an ephemeral one) of `class`'s namespace in worker `service`, on the first
    /// thread. The first thread holds the actor's channel, as a stub there would, until this is
    /// dropped.
    pub fn actor(
        &self,
        service: &str,
        class: &str,
        key: &str,
        name: Option<&str>,
    ) -> KjOwn<ffi::RpcClient> {
        ffi::rpc_client_debug_actor(&self.client, service, class, key, name.into())
    }

    /// The service `name` of the first thread's graph.
    pub fn service(&self, name: &str) -> KjOwn<ffi::RpcClient> {
        ffi::rpc_client_debug_entrypoint(&self.client, name)
    }

    /// Resolves once the session ended, which it does only when the first thread's server is
    /// gone or the session failed.
    pub async fn disconnected(&self) {
        // However it ended.
        let _ = ffi::rpc_client_on_disconnect(&self.client).await;
    }
}

/// Why a run cannot be served by several loop threads, if it cannot: what a process has one of
/// and a replica would need. `inspector` is whether the run has one (`--inspector-addr`).
#[must_use]
pub fn obstacles(inspector: bool) -> Vec<String> {
    let mut obstacles = Vec::new();
    if inspector {
        obstacles.push(
            "The inspector (--inspector-addr) is not supported on more than one thread; serve \
             this config without --threads."
                .to_owned(),
        );
    }
    obstacles
}

#[cfg(test)]
#[path = "threads-test.rs"]
mod tests;
