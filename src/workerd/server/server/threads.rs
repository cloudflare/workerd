// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The loop threads of `workerd serve --threads=<N>` other than the first: the replicas.
//!
//! The first loop thread is the process's main thread and has everything a process has one of:
//! it binds the sockets, reports the config's errors, handles SIGTERM, writes the `--control-fd`
//! events, owns the front, and serves the UDP sockets, the cron triggers and the debug port. It
//! also has the
//! Durable Objects: every actor with its storage, alarms, facets, container and hibernated
//! WebSockets, and in cluster mode the instance's whole part in the cluster (its lease, the
//! objects it owns, the peer port and sessions, the bucket's client).
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
//! What a replica lacks it reaches on the first thread, over its session with it
//! ([`ThreadLink`]): a two-way capnp `WorkerdDebugPort` session, as a cluster instance keeps
//! with a peer, over an in-process pipe. The threads' hub (`hub.rs`) says which thread an actor
//! lives on and makes the pipe of two threads' session the first time either needs it; the two
//! then talk directly. A replica's Durable Object
//! namespaces have no storage and no actors: the channel of a stub is a capability of the session
//! for the channel the first thread gives a stub of its own, so the first thread treats the
//! request of a replica as it treats a request it accepted itself, in a cluster too
//! (`actor::ForwardedChannel`). A `bucket` or `cache` service is forwarded the same way. HTTP
//! with its bodies, WebSockets and JS RPC cross the session as capnp; a stub or a service binding
//! passed in a JS RPC call crosses as a channel token, which every thread decodes with the
//! process's key and resolves in its own graph. A stub whose source is an object is restored
//! (`restore()`) from a replica by an event of the same session, whose parameters carry their
//! capabilities as channel tokens too; the object's `[restore]()` runs on the first thread.
//!
//! What the threads share besides is [`Phase`], which the first thread sets and the replicas
//! follow, and the events a replica sends back: that it is ready, and how it ended.
//!
//! - **Start.** The first thread builds its graph first. Only a config it accepts without an
//!   error gets replicas ([`Replicas::start`]), which build theirs at the same time; an error in
//!   a replica's graph is that thread's failure, not a config error reported again. (What the
//!   first thread attributed to a cluster deployment, a replica attributes to it too, and
//!   serves.) Once every replica is ready, and so can take work, the first thread makes the
//!   front, which registers the listeners, and writes its `listen` events; then
//!   [`Replicas::serve`] lets the replicas serve and the front accepts.
//! - **Failure.** A replica that fails, or stops serving before the server drains, fails the
//!   future [`Started::watch`] makes, which the first thread serves as one of its listeners:
//!   the process ends with that error as it does with a listener's. One that has ended before
//!   the first thread advertises its cluster lease or writes its `listen` events fails the
//!   start instead ([`Started::check`]). Its pipe closes with its
//!   loop, which cancels what it had in flight on the first thread, as a client hanging up
//!   does. A replica whose session ends while it serves fails the same way.
//! - **Drain.** The first thread tells the replicas ([`Replicas::drain`]) and the front, which
//!   stops accepting and shuts its HTTP connections down gracefully: an idle one closes, one
//!   with a request in flight after its response. A connection of a TCP socket is the front's
//!   until its `connect()` event has ended, and is waited for as a listener on one thread
//!   waits for its own. Requests and connections still queued are served. Once every
//!   connection has ended the front closes its queue, and a loop is done when the queue has
//!   ended and what it took has finished. The first thread keeps its actors and serves the
//!   sessions until every replica is, so a replica's requests in flight finish as its own do;
//!   only then does it stop its containers. In a cluster the front accepts for as long as the
//!   first thread serves on, through its handoff and the grace period after it.
//! - **Teardown.** A replica drops its server, then its factory, then its loop, on its own
//!   thread, and waits for nothing of the first thread to do so: what it has in flight there are
//!   futures it drops, and the pipe's wake-ups cross threads without waiting. The first thread
//!   drops what it took from the front itself, then stops the front, which drops every
//!   connection, then drops the `Replicas`, which tells those still running to stop at once and
//!   joins every thread, and only then ends the front's runtime and joins its threads: a
//!   transport a loop holds (a TCP connection, a WebSocket) is registered with that runtime,
//!   which so outlives every loop's use of it. The first thread's server, which serves the
//!   sessions, and its factory, and with it the process's `SharedFactory` and V8, outlive every
//!   other loop. A loop that stops drops the calls it had taken, whose connections close, and
//!   leaves what is queued to the others.

use std::cell::OnceCell;
use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;
use std::thread::JoinHandle;

use futures::future::LocalBoxFuture;
use kj_rs::KjArc;
use kj_rs::KjOwn;
use kj_rs_tokio::PortPaced;
use tokio::io::DuplexStream;
use tokio::sync::mpsc;
use tokio::sync::watch;

use crate::Result;
use crate::bridge::ffi;
use crate::config::Deployments;
use crate::config::Factory;
use crate::config::Overrides;
use crate::front;
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
    /// No connection is accepted; what is queued and in flight is finished.
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
    /// The hub of the run's loop threads, once they are started.
    hub: OnceCell<Arc<Hub>>,
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
            hub: OnceCell::new(),
        }
    }

    #[must_use]
    pub fn count(&self) -> usize {
        self.count
    }

    /// Starts the threads and waits until each has built its graph and can take work from
    /// `queue`, the front's, for `sockets`. A thread that fails to, or has ended by then, is
    /// the error.
    ///
    /// What it returns watches the threads from then on ([`Started`]).
    pub async fn start(
        &self,
        factory: &Rc<Factory>,
        sockets: &FrontSockets,
        queue: &front::Queue,
        overrides: &Overrides,
        deployments: &Deployments,
    ) -> Result<Started> {
        let (events, received) = mpsc::unbounded_channel();
        let count = self.count;
        // Every actor's home is the first thread.
        let placement = Placement {
            count: count + 1,
            spread: false,
        };
        let (hub, seats) = Hub::new(placement);
        let _ = self.hub.set(hub);
        let mut seats = seats.into_iter();
        let first = seats
            .next()
            .ok_or_else(|| kj::failed!("the hub made no seat for the first loop thread"))?;
        factory.join(first)?;
        for seat in seats {
            let index = seat.index;
            let replica = Replica {
                shared: self.shared.clone(),
                sockets: sockets.clone(),
                queue: queue.clone(),
                overrides: overrides.clone(),
                deployments: deployments.clone(),
                signals: Signals {
                    phase: self.phase.subscribe(),
                    events: events.clone(),
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
    /// The stream sockets the front accepts from, and the queue it puts their work in.
    sockets: FrontSockets,
    queue: front::Queue,
    overrides: Overrides,
    /// The services of the config that came from cluster deployments.
    deployments: Deployments,
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
            signals,
            seat,
            ..
        } = self;
        let factory = Rc::new(Factory::new(factory));
        factory.join(seat)?;
        let serving = Rc::clone(&factory);
        let replica =
            crate::run::run_replica(serving, overrides, deployments, sockets, queue, &signals);
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

/// A loop thread's session with another loop thread of the process: a two-way capnp
/// `WorkerdDebugPort` session, the one a cluster node keeps with a peer, over an in-process pipe
/// the hub made for the two (`Hub::link`). Everything that crosses between two loop threads as
/// capabilities crosses one of these, and nothing outside this type knows how.
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
    /// thread resolves it. That thread holds the actor's channel, as a stub there would, until
    /// this is dropped.
    pub fn actor(
        &self,
        service: &str,
        class: &str,
        key: &str,
        name: Option<&str>,
    ) -> Result<KjOwn<ffi::RpcClient>> {
        let actor = ffi::rpc_client_debug_actor(&self.client, service, class, key, name.into());
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
        }
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
