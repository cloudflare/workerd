// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The loop threads of `workerd serve --threads=<N>` other than the first: the replicas.
//!
//! The first loop thread is the process's main thread and has everything a process has one of:
//! it binds the sockets, reports the config's errors, handles SIGTERM, writes the `--control-fd`
//! events, owns the front and the hub, and serves the UDP sockets, the cron triggers and the
//! debug port. In cluster mode it also has the instance's control plane (its record and lease,
//! the one table of the objects the instance claimed, the alarm index) and the peer port.
//!
//! No loop thread accepts from a stream socket. The front does (`front/mod.rs`): a tokio runtime
//! of its own that owns the HTTP, HTTPS and TCP sockets the first thread bound, accepts, performs
//! the TLS handshakes and speaks HTTP, and queues each request and each TCP connection. Every
//! loop thread, the first like the others, takes an item from that queue whenever it has nothing
//! else to run and serves it on the socket's service (`listen/dispatch.rs`), so the requests of
//! one connection are served by whichever threads are free.
//!
//! A replica is a thread with its own event loop (`kj_rs_tokio::Runtime`), its own
//! `WorkerFactory` over the process's `SharedFactory` (`bootstrap_loop`), and its own copy of the
//! service graph, built from the same config by the same code (`run::run_replica`): every Worker
//! is compiled once per thread, so a Worker's global scope runs on each and its global state is
//! per thread, and a service binding reaches the Worker of the same thread. So are the dynamic
//! Workers of a `workerLoader`: each thread loads its own, and under one name the code its
//! `getCode` callback returned on that thread, so if the callback can return different code for
//! one name, threads can run different code under it.
//!
//! **Durable Objects** are spread over the loop threads, the first like the others. An actor has
//! one home thread for the life of the process, which the threads' hub names (`hub.rs`: a hash
//! of the namespace and the actor's key over the threads, `placement.rs`), and its facets live
//! with it. Every thread's namespaces open the namespace's storage, but only the home thread
//! makes the actor's container, opens its databases, schedules its alarm (each thread has an
//! alarm database of its own in the namespace's directory, see `ActorNamespace::start_alarms`)
//! and keeps its hibernated WebSockets. On any other thread the channel of a stub is a
//! capability for the channel the home thread gives a stub of its own
//! (`actor::ForwardedChannel`), over the two threads' session ([`ThreadLink`]): a two-way capnp
//! `WorkerdDebugPort` session, as a cluster instance keeps with a peer, over an in-process pipe.
//! The hub makes the pipe of a pair of threads the first time either needs it and tells the
//! other through its inbox ([`take_mail`]); the two then talk directly, so a call for an actor
//! of another thread takes one hop, and no third thread, however busy, has a part in it. HTTP
//! with its bodies, WebSockets and JS RPC cross the session as capnp, in the order one stub's
//! calls were made; a stub or a service binding passed in a JS RPC call crosses as a channel
//! token, which every thread decodes with the process's key and resolves in its own graph. A
//! stub whose source is an object is restored (`restore()`) from another thread by an event of
//! the same session, whose parameters carry their capabilities as channel tokens too; the
//! object's `[restore]()` runs on its home thread. A thread that is sent for an actor the hub
//! places elsewhere refuses, and passes no call on.
//!
//! What acts on every actor is sent to every thread through the inboxes and is done once each
//! thread said so: `abortAllDurableObjects()`, `deleteAllDurableObjects()` and the eviction test
//! hooks (`hub::Mail`).
//!
//! In cluster mode (`cluster.rs`) every thread takes part in the instance: each has a data
//! plane for the objects whose home it is (their captures, uploads and takeovers, over a bucket
//! client and sessions to peers of its own), and asks the first thread's control plane, through
//! its inbox, for every change of who owns one; the control plane refuses a thread that asks
//! for an object that is not its own. Whether the lease holds and whether the instance drains
//! every thread reads from what they share, and the lease thread watches every thread's loop:
//! one that stands still fences the instance. A peer's call arrives on the first thread, which
//! counts it on the peer's session and passes it to the object's home as a peer's; an alarm
//! adopted from a dead peer is scheduled on its object's home; and when the instance drains,
//! every thread hands off its own objects. A `cache` service is the first thread's on every
//! thread (`services::ForwardedService`), with or without a cluster, and so is a `bucket` that
//! is a local store.
//!
//! What the threads share besides is [`Phase`], which the first thread sets and the replicas
//! follow, and the events a replica sends back: that it is ready, that it has drained, and how
//! it ended.
//!
//! - **Start.** The first thread builds its graph first. Only a config it accepts without an
//!   error gets replicas, which build theirs at the same time ([`Replicas::start`]); an error in
//!   a replica's graph is that thread's failure, not a config error reported again. (What the
//!   first thread attributed to a cluster deployment, a replica attributes to it too, and
//!   serves.) With how many threads there are settled, the first thread takes its seat at the
//!   hub ([`Replicas::seat`]) and starts its alarm schedulers, which first move the alarms an
//!   earlier process left to the database of the thread each belongs to now; only then do the
//!   replicas start, and theirs. A thread answers what the others send it once its graph is
//!   built (`Factory::serve_links`); what was sent before waits. Once every replica is ready,
//!   and so can take work, the first thread makes the front, which registers the listeners, and
//!   writes its `listen` events; then [`Replicas::serve`] lets the replicas serve and the front
//!   accepts.
//! - **Failure.** A replica that fails, or ends before the first thread quiesces the threads,
//!   fails the future [`Started::watch`] makes, which the first thread serves as one of its
//!   listeners: the process ends with that error as it does with a listener's. One that has
//!   ended before the first thread advertises its cluster lease or writes its `listen` events
//!   fails the start instead ([`Started::check`]). A session between two threads that ends
//!   before the process quiesced fails each of the two that notices (`Factory::link_lost`):
//!   the Durable Object calls between them are lost with it.
//! - **Drain.** The first thread tells the replicas ([`Replicas::drain`]) and the front, which
//!   stops accepting and shuts its HTTP connections down gracefully: an idle one closes, one
//!   with a request in flight after its response. A connection of a TCP socket is the front's
//!   until its `connect()` event has ended, and is waited for as a listener on one thread
//!   waits for its own. Requests and connections still queued are served. Once every
//!   connection has ended the front closes its queue, and a loop has drained when the queue has
//!   ended and what it took has finished. A replica that has drained says so and stays, with
//!   its actors, serving what the other threads send them; the first thread does the same.
//!   Once every thread has drained, or the first thread waits no longer (a cluster's stop
//!   deadline), it quiesces them ([`Replicas::quiesce`]): each thread then stops its actors'
//!   containers and ends. Work an actor starts after that (an alarm, a timer) fails its calls
//!   to other threads as disconnected. In a cluster the front accepts for as long as the first
//!   thread serves on, through its handoff and the grace period after it.
//! - **Teardown.** A replica drops its server, then its factory, then its loop, on its own
//!   thread, and waits for nothing of another thread to do so: what it has in flight there are
//!   futures it drops, and the pipes' wake-ups cross threads without waiting. The first thread
//!   drops what it took from the front itself, then stops the front, which drops every
//!   connection, then drops the `Replicas`, which tells those it has not quiesced to stop at
//!   once and joins every thread, and only then ends the front's runtime and joins its threads:
//!   a transport a loop holds (a TCP connection, a WebSocket) is registered with that runtime,
//!   which so outlives every loop's use of it. The first thread's server and its factory, and
//!   with it the process's `SharedFactory` and V8, outlive every other loop. A loop that stops
//!   drops the calls it had taken, whose connections close, and leaves what is queued to the
//!   others.

use std::cell::OnceCell;
use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::PoisonError;
use std::thread::JoinHandle;

use futures::FutureExt;
use futures::future::LocalBoxFuture;
use kj_rs::KjArc;
use kj_rs::KjOwn;
use kj_rs_tokio::PortPaced;
use tokio::io::DuplexStream;
use tokio::sync::mpsc;
use tokio::sync::watch;
use tokio::time::Instant;

use crate::Result;
use crate::actor::Caller;
use crate::bridge::ffi;
use crate::config::Deployments;
use crate::config::Factory;
use crate::config::Overrides;
use crate::config::Server;
use crate::front;
use crate::hub::Done;
use crate::hub::Hub;
use crate::hub::Mail;
use crate::hub::Seat;
use crate::listen::FrontSockets;
use crate::placement::Placement;

/// Where the process is in its life, for the replicas to follow. It only advances.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Phase {
    /// The threads are building their graphs; no connection is accepted.
    Starting,
    /// The front accepts connections, and every thread serves what it takes from its queue.
    Serving,
    /// No connection is accepted; what is queued and in flight is finished. Every thread still
    /// serves what the others send it.
    Draining,
    /// No thread has a connection left, or the process waits for them no longer: each thread
    /// stops its actors' containers and ends. A session between two threads may end from here
    /// on.
    Quiesced,
    /// The process is ending without waiting for anything.
    Stopping,
}

/// What a replica's thread tells the first.
enum Event {
    /// The thread built its graph and waits for [`Phase::Serving`].
    Ready,
    /// The thread, the one with this index, has finished what it took from the front's queue:
    /// it waits for [`Phase::Quiesced`], serving the other threads meanwhile.
    Drained(usize),
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
    index: usize,
    phase: watch::Receiver<Phase>,
    events: mpsc::UnboundedSender<Event>,
    /// When a thread that stops its containers gives up on them, if the process has such a
    /// time; set before [`Phase::Quiesced`].
    stop_by: Arc<Mutex<Option<Instant>>>,
}

impl Signals {
    /// Tells the first thread that this one is ready to serve.
    pub fn ready(&self) {
        // The first thread stopped listening only if it is ending, which the phase says.
        let _ = self.events.send(Event::Ready);
    }

    /// Tells the first thread that this one has no connection left.
    pub fn drained(&self) {
        // As for `ready`.
        let _ = self.events.send(Event::Drained(self.index));
    }

    /// The phase the process is in now, whatever this thread has seen of it.
    #[must_use]
    pub fn phase(&self) -> Phase {
        *self.phase.borrow()
    }

    /// Until when this thread may wait for its containers to stop, if not for as long as it
    /// takes. As of [`Phase::Quiesced`].
    #[must_use]
    pub fn stop_by(&self) -> Option<Instant> {
        *self.stop_by.lock().unwrap_or_else(PoisonError::into_inner)
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
    /// The replicas' seats, from when the first thread has taken its own until they start.
    seats: RefCell<Vec<Seat>>,
    shared: KjArc<ffi::SharedFactory>,
    phase: watch::Sender<Phase>,
    threads: RefCell<Vec<JoinHandle<()>>>,
    /// The hub of the run's loop threads, once they are started.
    hub: OnceCell<Arc<Hub>>,
    stop_by: Arc<Mutex<Option<Instant>>>,
}

impl Replicas {
    /// `count` replicas of the server of `factory`, the first loop thread's; none runs yet.
    #[must_use]
    pub fn new(count: usize, factory: &Factory) -> Self {
        Self {
            count,
            seats: RefCell::new(Vec::new()),
            shared: factory.shared(),
            phase: watch::Sender::new(Phase::Starting),
            threads: RefCell::new(Vec::new()),
            hub: OnceCell::new(),
            stop_by: Arc::default(),
        }
    }

    #[must_use]
    pub fn count(&self) -> usize {
        self.count
    }

    /// Makes the hub of the run's loop threads and seats the first, `factory`'s, which from
    /// then on places actors as the hub does. Before [`Self::start`], and before the first
    /// thread schedules an alarm.
    pub fn seat(&self, factory: &Rc<Factory>) -> Result<()> {
        let placement = Placement {
            count: self.count + 1,
            spread: true,
        };
        let (hub, seats) = Hub::new(placement);
        let _ = self.hub.set(hub);
        let mut seats = seats.into_iter();
        let first = seats
            .next()
            .ok_or_else(|| kj::failed!("the hub made no seat for the first loop thread"))?;
        factory.join(first)?;
        *self.seats.borrow_mut() = seats.collect();
        Ok(())
    }

    /// Starts the threads and waits until each has built its graph and can take work from
    /// `queue`, the front's, for `sockets`. A thread that fails to, or has ended by then, is
    /// the error.
    ///
    /// What it returns watches the threads from then on ([`Started`]).
    pub async fn start(
        &self,
        sockets: &FrontSockets,
        queue: &front::Queue,
        overrides: &Overrides,
        deployments: &Deployments,
        cluster: Option<&crate::cluster::Seat>,
    ) -> Result<Started> {
        let (events, received) = mpsc::unbounded_channel();
        let count = self.count;
        let seats = std::mem::take(&mut *self.seats.borrow_mut());
        if seats.len() != count {
            return Err(kj::failed!("the loop threads have no seats"));
        }
        for seat in seats {
            let index = seat.index;
            let replica = Replica {
                shared: self.shared.clone(),
                sockets: sockets.clone(),
                queue: queue.clone(),
                overrides: overrides.clone(),
                deployments: deployments.clone(),
                cluster: cluster.cloned(),
                signals: Signals {
                    index,
                    phase: self.phase.subscribe(),
                    events: events.clone(),
                    stop_by: Arc::clone(&self.stop_by),
                },
                seat,
            };
            let exit = ExitReport {
                index,
                events: events.clone(),
                result: None,
            };
            // The thread runs inside this thread's logger, as a thread KJ starts would: the
            // JSON logger, under structured logging.
            let within = crate::log::within_thread();
            let thread = std::thread::Builder::new()
                .name(format!("loop-{index}"))
                .stack_size(REPLICA_STACK_SIZE)
                .spawn(move || within(Box::new(move || replica.run_thread(exit))))
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

    /// Has the replicas stop their actors' containers and end, giving up on the containers at
    /// `stop_by` if that is a time. For once no thread has a connection left, or when the
    /// process stops waiting for them.
    pub fn quiesce(&self, stop_by: Option<Instant>) {
        *self.stop_by.lock().unwrap_or_else(PoisonError::into_inner) = stop_by;
        self.advance(Phase::Quiesced);
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
                Ok(Event::Ready | Event::Drained(_)) => {}
                Ok(Event::Exited(index, result)) => {
                    return Err(exit_error(index, result, "ended while starting"));
                }
                Err(mpsc::error::TryRecvError::Empty) => return Ok(()),
                Err(mpsc::error::TryRecvError::Disconnected) => return Err(lost_thread()),
            }
        }
    }

    /// Watches the threads until none has a connection left: fails when one fails, or ends
    /// before the first thread quiesces them, and resolves once every one has drained. The first
    /// thread serves it as one of its listeners.
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
            Some(Event::Drained(_)) => {}
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

/// Watches `serving` threads through their events: see [`Started::watch`].
async fn watch_threads(
    mut events: mpsc::UnboundedReceiver<Event>,
    phase: watch::Receiver<Phase>,
    mut serving: usize,
) -> Result<()> {
    let mut drained = std::collections::HashSet::new();
    while serving > 0 {
        match events.recv().await {
            Some(Event::Ready) => {}
            Some(Event::Drained(index)) => {
                if drained.insert(index) {
                    serving -= 1;
                }
            }
            Some(Event::Exited(index, result)) => {
                // A thread ends only once the process quiesced: until then the others may
                // still send it calls.
                if result.is_err() || *phase.borrow() < Phase::Quiesced {
                    return Err(exit_error(index, result, "stopped serving"));
                }
                if drained.insert(index) {
                    serving -= 1;
                }
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
    /// The stream sockets the front accepts from, and the queue it puts their work in.
    sockets: FrontSockets,
    queue: front::Queue,
    overrides: Overrides,
    /// The services of the config that came from cluster deployments.
    deployments: Deployments,
    /// In a cluster, what the instance's threads share of its membership.
    cluster: Option<crate::cluster::Seat>,
    signals: Signals,
    /// Which loop thread it is, with the hub and its inbox.
    seat: Seat,
}

impl Replica {
    /// The thread: the replica's whole life, reported through `exit`.
    fn run_thread(self, mut exit: ExitReport) {
        exit.result = Some(self.run());
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
            queue,
            overrides,
            deployments,
            cluster,
            signals,
            seat,
            ..
        } = self;
        let factory = Rc::new(Factory::new(factory));
        factory.join(seat)?;
        let serving = Rc::clone(&factory);
        let replica = crate::run::run_replica(
            serving,
            overrides,
            deployments,
            cluster,
            sockets,
            queue,
            &signals,
        );
        let result = replica.await;
        let shutdown = crate::entry::shutdown(factory).await;
        result?;
        shutdown
    }
}

// =====================================================================================
// Sessions

/// Each direction of a session's pipe buffers this much before the writer waits.
const SESSION_BUFFER_SIZE: usize = 256 * 1024;

/// One end of the in-process pipe that the session of two loop threads runs over.
///
/// It is made on the first thread and goes to the thread that uses it, which makes a KJ stream
/// of it on its own loop.
pub struct SessionEnd(pub(crate) DuplexStream);

impl SessionEnd {
    /// The pipe for the thread that uses this end. The other thread fills it whenever it is
    /// ahead, so its reads are paced to this thread's port (`kj_rs_tokio::PortPace`): a thread
    /// that reads a long message, such as the body of an upload to a Durable Object, still
    /// serves everything else it has.
    fn paced(self) -> PortPaced<DuplexStream> {
        PortPaced::new(self.0)
    }
}

/// The two ends of a session's pipe.
#[must_use]
pub fn session_pipe() -> (SessionEnd, SessionEnd) {
    let (a, b) = tokio::io::duplex(SESSION_BUFFER_SIZE);
    (SessionEnd(a), SessionEnd(b))
}

/// A loop thread's session with another loop thread of the process.
///
/// It is a two-way capnp `WorkerdDebugPort` session, the one a cluster node keeps with a peer,
/// over an in-process pipe the hub made for the two (`Hub::link`). Everything that crosses
/// between two loop threads as capabilities crosses one of these, and nothing outside this
/// type knows how.
///
/// Each end names an actor or a service of the other end's graph and starts events on it: a
/// stub to an actor of another thread is a capability of the session for the actor's channel
/// there (`actor::ForwardedChannel`). Calls made over one stub arrive in the order they were
/// made. Unlike a session with another instance, it carries `restore()` events
/// (`new_thread_link`). An end answers once its thread's graph is built
/// (`Factory::serve_links`); what the other sent before waits in the pipe.
pub struct ThreadLink {
    peer: usize,
    client: KjOwn<ffi::RpcClient>,
}

impl ThreadLink {
    /// The session with thread `peer` over `end`, on the loop of `factory`, this thread's.
    pub(crate) fn connect(
        factory: &ffi::WorkerFactory,
        peer: usize,
        end: SessionEnd,
    ) -> Result<Self> {
        let index = u32::try_from(peer).map_err(|_| kj::failed!("too many loop threads"))?;
        let stream = kj_hyper::into_kj_stream(end.paced());
        Ok(Self {
            peer,
            client: ffi::new_thread_link(factory, stream, index),
        })
    }

    /// The thread at the other end.
    #[must_use]
    pub fn peer(&self) -> usize {
        self.peer
    }

    /// The actor `key` (the hex id of a durable actor, derived from `name` if that is known;
    /// the name of an ephemeral one) of `class`'s namespace in worker `service`, as the other
    /// thread resolves it for requests of `caller`. That thread holds the actor's channel, as a
    /// stub there would, until this is dropped.
    pub fn actor(
        &self,
        service: &str,
        class: &str,
        key: &str,
        name: Option<&str>,
        caller: Caller,
    ) -> Result<KjOwn<ffi::RpcClient>> {
        let actor = ffi::rpc_client_debug_actor(
            &self.client,
            service,
            class,
            key,
            name.into(),
            caller.code(),
        );
        Ok(actor?)
    }

    /// The service `name` of the other thread's graph.
    pub fn service(&self, name: &str) -> Result<KjOwn<ffi::RpcClient>> {
        Ok(ffi::rpc_client_debug_entrypoint(&self.client, name)?)
    }

    /// Resolves once the session ended, which it does only when the other thread's loop is gone
    /// or the session failed.
    pub async fn disconnected(&self) {
        // However it ended.
        let _ = ffi::rpc_client_on_disconnect(&self.client).await;
    }
}

/// Takes what loop thread `factory`'s is sent through `inbox`, until the hub and every other
/// thread are gone.
pub async fn take_mail(factory: Rc<Factory>, mut inbox: mpsc::UnboundedReceiver<Mail>) {
    while let Some(mail) = inbox.recv().await {
        match mail {
            Mail::Link(peer) => {
                if let Err(error) = factory.link(peer) {
                    tracing::error!(
                        "taking the session with loop thread {peer}: {}",
                        error.description()
                    );
                }
            }
            Mail::AbortAll(reason, done) => {
                let reason = reason.map(|(kind, description)| crate::Error::new(kind, description));
                answer(done, &factory, |server| {
                    server.abort_all_actors(reason.as_ref());
                    Ok(())
                });
            }
            Mail::DeleteAll(reason, done) => {
                let reason = reason.map(|(kind, description)| crate::Error::new(kind, description));
                answer(done, &factory, |server| {
                    server.delete_all_actors(reason.as_ref())
                });
            }
            Mail::EvictAll {
                namespaces,
                hibernate,
                done,
            } => {
                // An eviction waits for the actor's requests, and the mail behind it must not.
                let serving = Rc::clone(&factory);
                factory.spawn_detached(async move {
                    let evicted = async {
                        let server = serving.server()?;
                        for (service, class) in &namespaces {
                            let namespace = server.actor_namespace(service, class)?;
                            namespace.evict_all_for_test(hibernate).await?;
                        }
                        Ok::<_, crate::Error>(())
                    };
                    let _ = done.send(evicted.await.map_err(|e| e.description().to_owned()));
                });
            }
            mail @ (Mail::Control { .. } | Mail::AdoptAlarm { .. } | Mail::HandOff { .. }) => {
                take_cluster_mail(&factory, mail);
            }
            Mail::Evict {
                service,
                class,
                key,
                hibernate,
                done,
            } => {
                let serving = Rc::clone(&factory);
                factory.spawn_detached(async move {
                    let evicted = async {
                        let namespace = serving.server()?.actor_namespace(&service, &class)?;
                        namespace.evict_for_test(&key, hibernate).await
                    };
                    let _ = done.send(evicted.await.map_err(|e| e.description().to_owned()));
                });
            }
        }
    }
}

/// Takes the mail of a cluster instance's loop threads: what another thread asks of the control
/// plane, which the first thread has, an adopted alarm for an object of this thread, and the
/// instance's drain.
fn take_cluster_mail(factory: &Rc<Factory>, mail: Mail) {
    match mail {
        Mail::Control { from, call, answer } => {
            let node = factory.server().and_then(|server| {
                (server.cluster().cloned())
                    .ok_or_else(|| kj::failed!("this loop thread is in no cluster"))
            });
            let node = match node {
                Ok(node) => node,
                Err(e) => {
                    if let Some(answer) = answer {
                        let failed = cluster::Error::Replica(e.description().to_owned());
                        let _ = answer.send(call.failed(failed));
                    }
                    return;
                }
            };
            match answer {
                // A call without an answer does not wait: it is done before the next mail
                // is taken, and so before what its thread asks next.
                None => drop(node.control(from, call).now_or_never()),
                Some(answer) => factory.spawn_detached(async move {
                    // The thread that asked may have ended.
                    let _ = answer.send(node.control(from, call).await);
                }),
            }
        }
        Mail::AdoptAlarm {
            namespace,
            actor,
            name,
            at_ms,
        } => {
            if let Ok(Some(node)) = factory.server().map(|server| server.cluster().cloned()) {
                node.adopt_alarm(&namespace, &actor, name.as_deref(), at_ms);
            } else {
                tracing::warn!("an adopted alarm reached a loop thread that is in no cluster");
            }
        }
        Mail::HandOff { deadlines, stopped } => {
            let serving = Rc::clone(factory);
            factory.spawn_detached(async move {
                let handed_off = async {
                    let server = serving.server()?;
                    let namespaces = server.clustered_namespaces();
                    Ok::<_, crate::Error>(crate::actor::hand_off_all(&namespaces, deadlines).await)
                };
                // A thread that cannot say counts as one with objects left.
                let _ = stopped.send(handed_off.await.unwrap_or(false));
            });
        }
        _ => {}
    }
}

/// Answers a mail through `done` with what `act` makes of this thread's server.
fn answer(done: Done, factory: &Factory, act: impl FnOnce(&Server) -> Result<()>) {
    let result = factory.server().and_then(|server| act(&server));
    // The thread that asked may have ended.
    let _ = done.send(result.map_err(|e| e.description().to_owned()));
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
