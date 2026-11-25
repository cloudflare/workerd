// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The front of `workerd serve --threads=<N>`: the server in front of the loop threads.
//!
//! With several loop threads no loop accepts connections. The front owns every stream socket of
//! the config. It has a tokio runtime of its own (`--front-threads` threads, [`DEFAULT_THREADS`]
//! unless given), on which it accepts, performs the TLS handshakes and speaks HTTP: hyper serves
//! each connection there (`kj_hyper::server::serve_connection_remote`), so reading, parsing,
//! framing and TLS records cost the loop threads nothing.
//!
//! What a connection brings is [`Work`], which the front puts in the [`queue`]: an HTTP request
//! (one per request, not per connection, so the requests of a keep-alive connection are served by
//! whichever loops take them), or a connection of a TCP socket. Work is `Send` and carries
//! nothing of KJ. A loop thread takes it and serves it on the socket's service
//! (`listen/dispatch.rs`):
//!
//! - **An HTTP request** is hyper's own (`kj_hyper::server::RemoteRequest`): the head, the body
//!   and the way back. The loop reads the body from hyper as the Worker asks for it and writes
//!   the response body a chunk at a time, each to the connection's task directly, so a slow
//!   client slows the Worker's writes, a slow Worker the client's upload, and neither side
//!   buffers more than a chunk. The response head can go out before the request body has ended.
//! - **An upgrade** (a WebSocket, a CONNECT tunnel, the capnp session over CONNECT) gives the
//!   loop the connection's transport itself, which hyper hands over: the loop makes a KJ stream
//!   of it, so `kj::WebSocket` frames it as on one thread. The transport stays registered with
//!   the front's runtime, whose driver wakes the loop for it; with TLS, the loop then does the
//!   record encryption of that connection.
//! - **A TCP connection** is the accepted transport, after the handshake of a socket with
//!   implicit TLS; the loop serves it as a `connect()` event. It stays a connection of the
//!   front, on a task that waits there, until that event has ended: the transport is
//!   registered with the front's runtime for as long as the loop uses it.
//!
//! A request in flight is a future on its connection's task ([`call`]). When the client goes
//! away hyper's serve loop drops it, which takes the request out of the queue if no loop has
//! taken it, and otherwise has the loop drop the call's KJ promise, as a connection served on
//! the loop itself would. When the loop is done with the call (or is gone) the future resolves.
//!
//! A TCP connection that is queued leaves the queue when its client goes away, and is closed,
//! where the system reports that for a connection nobody reads from (unix, for a client that
//! resets it; one that only shuts down its side may still want an answer). Once a loop has
//! taken it the connection is the loop's to end, as on one thread: the `connect()` handler
//! sees the hang-up through its socket.
//!
//! # Overload
//!
//! The queue holds a limited number of items that no loop has taken ([`DEFAULT_QUEUE_LIMIT`],
//! `--front-queue`). When loops are all busy, in JavaScript that does not yield for one, the
//! queue fills, and the front then holds its clients back instead of collecting what they
//! send: a request that finds the queue full waits on its connection's task, parsed and not
//! queued, and hyper reads nothing more from that connection, so TCP's flow control holds
//! the client; and the accept loops accept nothing until the queue has room, so new
//! connections wait in the listen backlog, as they do when the one thread of a server
//! without a front is busy. Room is given in the order it was asked for, so the requests
//! that wait are queued ahead of connections not yet accepted. Nothing is answered with an
//! error and nothing times out: a request waits for as long as its client does, and leaves
//! when the client goes away, queued or not. A connection of a TCP socket waits the same
//! way, accepted and not queued. Requests and connections that a loop has taken do not
//! count, however long they last.
//!
//! # Life
//!
//! [`Front::new`] makes the runtime, on a thread of its own that owns it, and registers the
//! listeners there; nothing is accepted before [`Front::serve`], which the first loop thread
//! calls once every loop can take work and it has written its `listen` events.
//! [`Front::drain`] stops the accept loops and shuts the HTTP connections down gracefully (an
//! idle one closes, one with a request in flight closes after its response); a TCP connection
//! lasts until its `connect()` event ends, queued or taken, as it does on one thread. Once
//! every connection has ended, the queue is closed, which is what ends the loops' takers after
//! the work that was still queued has been served.
//!
//! [`Front::stop`] drops the accept loops and every connection without waiting, which has the
//! loops drop the calls they took. The runtime stays: a loop that still holds a transport (a
//! TCP connection, a WebSocket) is woken for it by the front's runtime, so the first loop
//! thread stops the front, then stops and joins the other loops, and only then drops the
//! `Front`, which ends the runtime and joins its threads.
//!
//! The front logs nothing itself (its threads are not set up for the process's logger): what a
//! connection failed with goes to the first loop thread ([`Front::watch`]), which logs it. A
//! failed accept loop, or a front task that panicked, fails that future, and with it the server,
//! as a listener that fails does.

pub mod queue;

use std::future::poll_fn;
use std::net::SocketAddr;
use std::sync::Arc;
use std::task::Poll;
use std::thread::JoinHandle;

use futures::FutureExt;
use futures::future::Either;
use futures::future::FusedFuture;
use futures::future::LocalBoxFuture;
use kj_hyper::Hangup;
use kj_hyper::server::RemoteRequest;
use kj_hyper::server::ServerSettings;
use kj_hyper::server::Shutdown;
use kj_hyper::server::serve_connection_remote;
use kj_rs_io::ListenerSockets;
use kj_rs_io::TokioListener;
use tokio::sync::mpsc;
use tokio::sync::oneshot;
use tokio::sync::watch;
use tokio::task::JoinSet;

use crate::Error;
use crate::Result;
use crate::listen;
use crate::listen::Accepted;
use crate::listen::Transport;
use crate::threads::Phase;

/// The front's threads when `--front-threads` does not say: accepting, TLS and HTTP framing are
/// a fraction of what a request costs a loop thread, so a few threads feed many loops.
pub const DEFAULT_THREADS: usize = 2;

/// How many requests and TCP connections the queue holds when `--front-queue` does not say.
///
/// The queue is where work waits for a loop that is free, and a loop takes one item each time
/// it runs out of other things to do, so a queue deeper than there are loops serves nothing
/// sooner: what it holds beyond that is only waiting that the server has already taken on.
/// What each item holds is a parsed request head and, for a client that sends one request
/// per connection, two descriptors (the socket and the one that watches it for a hang-up).
/// This many leave a server whose loops are all busy well below the 1024 descriptors a
/// process commonly gets, and are several times the loops of the largest machine.
pub const DEFAULT_QUEUE_LIMIT: usize = 256;

/// Connection failures waiting to be logged; further ones are dropped.
const PENDING_LOG_LINES: usize = 256;

/// The queue between the front and the loop threads.
pub type Queue = queue::Queue<Work>;

/// A loop thread's end of the queue.
pub type Worker = queue::Worker<Work>;

/// What the front queues for a loop thread to serve.
#[expect(
    clippy::large_enum_variant,
    reason = "an item is moved twice, into the queue and out of it; a box would cost every \
              request an allocation"
)]
pub enum Work {
    Http(HttpWork),
    Tcp(TcpWork),
}

/// One request of an HTTP connection.
pub struct HttpWork {
    /// The index, in the config, of the socket the request came in on.
    pub socket: usize,
    pub request: RemoteRequest,
    /// The cf blob that describes the peer, for a socket whose options name no header for it.
    pub cf_blob: Option<Arc<str>>,
    /// The call's tie to its connection: closed once the connection has dropped the call, and
    /// dropped by the loop when the call has finished.
    pub call: oneshot::Sender<()>,
}

/// One connection of a TCP socket, to be served as a `connect()` event.
pub struct TcpWork {
    /// The index, in the config, of the socket the connection was accepted on.
    pub socket: usize,
    /// The connection, past the handshake of a socket with implicit TLS.
    pub io: Transport,
    pub hangup: Hangup,
    pub peer: Option<SocketAddr>,
    /// The socket's TLS configuration if its `connect()` handler may upgrade the connection.
    pub starttls: Option<Arc<rustls::ServerConfig>>,
    /// The connection's tie to the front, as an HTTP call's: closed once the front has dropped
    /// the connection, and dropped by the loop when the `connect()` event has ended.
    pub call: oneshot::Sender<()>,
}

/// What the front does with the connections of a stream socket.
pub enum Protocol {
    /// HTTP, after a TLS handshake if `tls` is given. `peer_cf_blob` is whether a request gets
    /// a cf blob describing the peer (the socket's options name no header that carries one).
    Http {
        tls: Option<Arc<rustls::ServerConfig>>,
        peer_cf_blob: bool,
    },
    /// Raw TCP.
    Tcp { tls: Option<listen::TcpTls> },
}

/// A stream socket of the config, for the front to accept from.
pub struct FrontSocket {
    /// The socket's index in the config.
    pub index: usize,
    pub sockets: ListenerSockets,
    pub protocol: Protocol,
}

/// A connection's failure, for the first loop thread to log: what failed, and how.
type LogLine = (&'static str, Error);

/// What the front tells the first loop thread.
struct Events {
    /// The failures of connections. Closed once the front's runtime is gone.
    log: mpsc::Receiver<LogLine>,
    /// How the front ended: drained, with its queue closed, or failed. Dropped unsent by a
    /// front that ended in neither way.
    outcome: oneshot::Receiver<Result<()>>,
}

/// The front, for the first loop thread. Dropping it stops it.
pub struct Front {
    phase: watch::Sender<Phase>,
    events: Option<Events>,
    /// Dropped to have the front's thread drop its runtime.
    release: Option<oneshot::Sender<std::convert::Infallible>>,
    thread: Option<JoinHandle<()>>,
}

impl Front {
    /// Starts the front's runtime with `threads` threads and registers `sockets` with it; no
    /// connection is accepted yet. What it accepts later goes into `queue`.
    pub async fn new(threads: usize, queue: Queue, sockets: Vec<FrontSocket>) -> Result<Self> {
        let phase = watch::Sender::new(Phase::Starting);
        let (log, log_lines) = mpsc::channel(PENDING_LOG_LINES);
        let (end, outcome) = oneshot::channel();
        let (registered, registration) = oneshot::channel();
        let (release, released) = oneshot::channel();
        let main = Main {
            queue,
            phase: phase.subscribe(),
            log,
        };
        let thread = std::thread::Builder::new()
            .name("front".to_owned())
            .spawn(move || main.run_thread(threads, sockets, registered, end, released))
            .map_err(|e| kj::failed!("starting the front: {e}"))?;
        let front = Self {
            phase,
            events: Some(Events {
                log: log_lines,
                outcome,
            }),
            release: Some(release),
            thread: Some(thread),
        };
        match registration.await {
            Ok(result) => result?,
            Err(_) => return Err(kj::failed!("the front ended while starting")),
        }
        Ok(front)
    }

    /// Lets the front accept connections.
    pub fn serve(&self) {
        self.advance(Phase::Serving);
    }

    /// Has the front stop accepting and shut its connections down gracefully.
    pub fn drain(&self) {
        self.advance(Phase::Draining);
    }

    /// Has the front drop its accept loops and its connections, without waiting for either.
    /// Its runtime stays up, for the transports the loops hold, until the `Front` is dropped.
    pub fn stop(&self) {
        self.advance(Phase::Stopping);
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

    /// Watches the front for the rest of the run, on the first loop thread: logs the failures
    /// of its connections, fails if the front does or ends without having drained, and resolves
    /// once it has drained, which is when its connections have all ended and the queue is
    /// closed.
    ///
    /// # Panics
    ///
    /// When called a second time.
    #[must_use]
    pub fn watch(&mut self) -> LocalBoxFuture<'static, Result<()>> {
        let events = self.events.take();
        Box::pin(async move {
            let Some(Events { mut log, outcome }) = events else {
                unreachable!("the front is watched once");
            };
            // A front that ended without saying how did not drain: its thread panicked.
            let ended = |outcome: std::result::Result<Result<()>, _>| match outcome {
                Ok(result) => result,
                Err(oneshot::error::RecvError { .. }) => {
                    Err(kj::failed!("the front ended without draining"))
                }
            };
            let mut outcome = outcome.fuse();
            loop {
                let mut line = std::pin::pin!(log.recv().fuse());
                futures::select_biased! {
                    line = line => match line {
                        Some((what, error)) => listen::log_connection_error(what, &error),
                        // The runtime is gone, and with it the front's thread.
                        None => break,
                    },
                    // A failure is the server's at once; after a drain, the lines the
                    // connections left are still logged.
                    outcome = outcome => ended(outcome)?,
                }
            }
            if outcome.is_terminated() {
                return Ok(());
            }
            ended(outcome.await)
        })
    }
}

impl Drop for Front {
    fn drop(&mut self) {
        self.stop();
        drop(self.release.take());
        if let Some(thread) = self.thread.take() {
            // The thread ends once its runtime has: a task that panicked did not take it down.
            let _ = thread.join();
        }
    }
}

/// What the front's tasks share.
struct Shared {
    queue: Queue,
    phase: watch::Receiver<Phase>,
    log: mpsc::Sender<LogLine>,
    /// Shuts down every HTTP connection once the front drains.
    shutdown: Shutdown,
}

impl Shared {
    /// Resolves once the front is in `phase` or a later one, with the one it is in. A `Front`
    /// that is gone is `Stopping`.
    async fn reached(&self, phase: Phase) -> Phase {
        let mut current = self.phase.clone();
        match current.wait_for(|current| *current >= phase).await {
            Ok(current) => *current,
            Err(_) => Phase::Stopping,
        }
    }

    /// Resolves once the front stops accepting.
    async fn drained(&self) {
        self.reached(Phase::Draining).await;
    }

    /// Has the first loop thread log a connection's failure, as a loop logs its own
    /// connections': a peer that went away is not news.
    fn log(&self, what: &'static str, error: Error) {
        if error.exception_type() != cxx::KjExceptionType::Disconnected {
            // Past what is pending, lines are dropped rather than queued without bound.
            let _ = self.log.try_send((what, error));
        }
    }
}

/// The front's main task, on its own thread.
struct Main {
    queue: Queue,
    phase: watch::Receiver<Phase>,
    log: mpsc::Sender<LogLine>,
}

impl Main {
    /// The front's thread: builds the runtime, runs the front on it, and drops it once
    /// `released` says that the `Front` is gone, which stops its threads. `registered` is told
    /// whether the listeners could be made, and `end` how the front ended.
    fn run_thread(
        self,
        threads: usize,
        sockets: Vec<FrontSocket>,
        registered: oneshot::Sender<Result<()>>,
        end: oneshot::Sender<Result<()>>,
        released: oneshot::Receiver<std::convert::Infallible>,
    ) {
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(threads.max(1))
            .thread_name("front")
            .enable_all()
            .build();
        let runtime = match runtime {
            Ok(runtime) => runtime,
            Err(e) => {
                let _ = registered.send(Err(kj::failed!("starting the front's runtime: {e}")));
                return;
            }
        };
        let outcome = runtime.block_on(self.run(sockets, registered));
        // Nobody listens once the first loop thread is ending.
        let _ = end.send(outcome);
        // The front has drained, or its tasks are being dropped: either way the loops may still
        // hold transports that the runtime's driver wakes them for, until the `Front` is
        // dropped.
        let _ = released.blocking_recv();
    }

    /// Registers the listeners, then accepts from each until the front drains, and waits for
    /// the connections. Returns early when the front is told to stop, which drops the accept
    /// loops and, with them, the connections.
    async fn run(
        self,
        sockets: Vec<FrontSocket>,
        registered: oneshot::Sender<Result<()>>,
    ) -> Result<()> {
        let listeners: Result<Vec<_>> = sockets
            .into_iter()
            .map(|socket| {
                let listener = socket.sockets.listen()?;
                Ok((socket.index, listener, socket.protocol))
            })
            .collect();
        let listeners = match listeners {
            Ok(listeners) => {
                let _ = registered.send(Ok(()));
                listeners
            }
            Err(error) => {
                let _ = registered.send(Err(error));
                return Ok(());
            }
        };
        let shared = Arc::new(Shared {
            queue: self.queue,
            phase: self.phase,
            log: self.log,
            shutdown: Shutdown::new(),
        });
        if shared.reached(Phase::Serving).await == Phase::Stopping {
            return Ok(());
        }
        let mut accepting = JoinSet::new();
        for (index, listener, protocol) in listeners {
            accepting.spawn(accept_loop(Arc::clone(&shared), index, listener, protocol));
        }
        let serve = async {
            let ended = async {
                while let Some(joined) = accepting.join_next().await {
                    match joined {
                        Ok(result) => result?,
                        Err(e) => return Err(kj::failed!("a task of the front failed: {e}")),
                    }
                }
                Ok(())
            };
            let mut ended = std::pin::pin!(ended);
            let drained = std::pin::pin!(shared.drained());
            if let Either::Left((result, _)) =
                futures::future::select(ended.as_mut(), drained).await
            {
                result?;
                // The accept loops end by themselves only once the front drains, which they
                // may have seen, and finished, before this task did.
                if *shared.phase.borrow() < Phase::Draining {
                    return Err(kj::failed!("the front stopped accepting"));
                }
                return Ok(());
            }
            // Idle connections close, and the others after their responses.
            shared.shutdown.shutdown();
            ended.await
        };
        let stop = std::pin::pin!(shared.reached(Phase::Stopping));
        match futures::future::select(std::pin::pin!(serve), stop).await {
            Either::Left((result, _)) => {
                result?;
                // Every connection has ended: nothing more will be queued.
                shared.queue.close();
                Ok(())
            }
            Either::Right(_) => Ok(()),
        }
    }
}

/// Accepts from `listener` until the front drains, each connection a task, then waits for the
/// connections. A failure to accept is the front's; a connection's is logged. Dropping the
/// future drops the connections.
async fn accept_loop(
    shared: Arc<Shared>,
    index: usize,
    listener: Box<TokioListener>,
    protocol: Protocol,
) -> Result<()> {
    /// What the loop waited for.
    enum Next<A> {
        Drained,
        Ended,
        Accepted(A),
    }
    let protocol = Arc::new(protocol);
    let what = match *protocol {
        Protocol::Http { .. } => "HTTP connection failed",
        Protocol::Tcp { .. } => "TCP connection failed",
    };
    // The connections' tasks, which end with this one if it is dropped.
    let mut connections = JoinSet::new();
    {
        let mut drained = std::pin::pin!(shared.drained().fuse());
        loop {
            let next = {
                // A full queue is not given more: the connection waits in the listen backlog.
                let accepting = std::pin::pin!(
                    async {
                        shared.queue.room().await;
                        listener.accept().await
                    }
                    .fuse()
                );
                // The tasks that have ended are taken out as they do.
                let ended = std::pin::pin!(
                    poll_fn(|cx| match connections.poll_join_next(cx) {
                        Poll::Ready(Some(_)) => Poll::Ready(()),
                        Poll::Ready(None) | Poll::Pending => Poll::Pending,
                    })
                    .fuse()
                );
                let (mut accepting, mut ended) = (accepting, ended);
                futures::select_biased! {
                    () = drained => Next::Drained,
                    () = ended => Next::Ended,
                    accepted = accepting => Next::Accepted(accepted),
                }
            };
            let (socket, peer) = match next {
                Next::Drained => break,
                Next::Ended => continue,
                Next::Accepted(accepted) => accepted?,
            };
            let accepted = Accepted::Socket(socket, peer);
            let (shared, protocol) = (Arc::clone(&shared), Arc::clone(&protocol));
            connections.spawn(async move {
                if let Err(error) = connection(&shared, index, &protocol, accepted).await {
                    shared.log(what, error);
                }
            });
        }
    }
    drop(listener);
    while connections.join_next().await.is_some() {}
    Ok(())
}

/// One accepted connection, until it has ended.
async fn connection(
    shared: &Arc<Shared>,
    index: usize,
    protocol: &Protocol,
    accepted: Accepted,
) -> Result<()> {
    let hangup = listen::hangup(&accepted);
    match protocol {
        Protocol::Http { tls, peer_cf_blob } => {
            let cf_blob: Option<Arc<str>> = peer_cf_blob
                .then(|| listen::peer_cf_blob(&accepted))
                .flatten()
                .map(Arc::from);
            let io = listen::transport(accepted, tls.as_ref(), shared.drained()).await?;
            let dispatch = |request| {
                let (call, finished) = oneshot::channel();
                let work = Work::Http(HttpWork {
                    socket: index,
                    request,
                    cf_blob: cf_blob.clone(),
                    call,
                });
                self::call(shared.queue.clone(), work, finished).boxed()
            };
            let timeout = ServerSettings::default().header_timeout;
            let shutdown = &shared.shutdown;
            Ok(serve_connection_remote(io, hangup, timeout, shutdown, dispatch).await?)
        }
        Protocol::Tcp { tls } => {
            let peer = match &accepted {
                Accepted::Socket(_, peer) => *peer,
                Accepted::Loopback(_) => None,
            };
            let (implicit, starttls) = match tls {
                Some(listen::TcpTls::Implicit(config)) => (Some(config), None),
                Some(listen::TcpTls::StartTls(config)) => (None, Some(Arc::clone(config))),
                None => (None, None),
            };
            let io = listen::transport(accepted, implicit, shared.drained()).await?;
            // The loop that takes the connection gets the signal with it; this task watches
            // it while no loop has.
            let gone = hangup.shared();
            let (call, mut finished) = oneshot::channel();
            let work = TcpWork {
                socket: index,
                io,
                hangup: gone.clone().boxed(),
                peer,
                starttls,
                call,
            };
            // A client that goes away before the queue has room for its connection was never
            // queued: the connection goes with the push.
            let ticket = {
                let push = std::pin::pin!(shared.queue.push(Work::Tcp(work)));
                match futures::future::select(push, gone.clone()).await {
                    Either::Left((ticket, _)) => ticket,
                    Either::Right((hung_up, _)) => return hung_up,
                }
            };
            // The connection is this task's until the loop that takes it has served it to its
            // end, which is when the loop drops its end of `finished`.
            if let Either::Right((hung_up, _)) = futures::future::select(&mut finished, gone).await
            {
                // The client is gone. A connection that is still queued leaves the queue, which
                // closes it; one that a loop took is the loop's to end.
                drop(ticket);
                let _ = finished.await;
                return hung_up;
            }
            Ok(())
        }
    }
}

/// A request in flight, for its connection: queues `work` once the queue has room for it,
/// and resolves once the loop that served it is done with the call, which is when it drops the
/// other end of `finished`. Dropping the future takes the request out of the queue, or has the
/// loop drop the call; one that still waited for room is just dropped.
async fn call(queue: Queue, work: Work, finished: oneshot::Receiver<()>) {
    let ticket = queue.push(work).await;
    // Resolves when the loop drops its end, whether or not the call finished.
    let _ = finished.await;
    drop(ticket);
}

#[cfg(test)]
#[path = "mod-test.rs"]
mod tests;
