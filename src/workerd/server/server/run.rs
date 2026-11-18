// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `workerd serve`, `workerd test` and `workerd bench`: bind the sockets, build the service
//! graph, listen, and either wait for the drain signal or run the tests or benchmarks.
//!
//! `workerd serve --threads=<N>` does this on the process's first loop thread (`run`), which
//! then starts the others, the replicas (`threads.rs`); each builds the graph again for itself
//! and listens on duplicates of the first thread's sockets (`run_replica`). The Durable Objects
//! and the cluster are the first thread's alone, which the replicas reach over their sessions
//! with it.

use std::collections::HashMap;
use std::future::Future;
use std::io::Write;
use std::pin::Pin;
use std::rc::Rc;
use std::rc::Weak;
use std::time::Duration;

use futures::FutureExt;
use futures::Stream;
use futures::StreamExt;
use futures::future::Either;
use futures::future::FusedFuture;
use futures::future::LocalBoxFuture;
use futures::stream::FuturesUnordered;
use kj_rs::KjMaybe;
use tokio::sync::oneshot;
use tokio::sync::watch;
use worker::CxxWorkerInterface;
use worker::Interface;
use workerd_capnp::config;
use workerd_capnp::service;
use workerd_capnp::socket;

use crate::Result;
use crate::actor;
use crate::bench;
use crate::bindings::Designator;
use crate::bindings::len_u32;
use crate::bridge::ffi;
use crate::channels::Channel;
use crate::cluster;
use crate::cluster::ClusterNode;
use crate::cluster::Ready;
use crate::config::Deployments;
use crate::config::Factory;
use crate::config::Overrides;
use crate::config::Reporter;
use crate::config::Server;
use crate::config::capnp_error;
use crate::config::text;
use crate::listen;
use crate::listen::BoundSocket;
use crate::listen::DuplicateSockets;
use crate::listen::HttpSocket;
use crate::listen::ListenContext;
use crate::listen::StreamListener;
use crate::listen::TcpTls;
use crate::log;
use crate::services::network::tls_options;
use crate::services::rewriter::HttpRewriter;
use crate::threads;
use crate::threads::Phase;
use crate::threads::Replicas;
use crate::threads::Signals;
use crate::worker::WorkerService;

/// Where `serve` prints a line for a person.
pub type Announce = Box<dyn Fn(&str)>;

/// The command line's choices for a run.
#[derive(Default)]
pub struct RunOptions {
    /// `--directory-path` and `--external-addr`; `--socket-addr` goes in `socket_addresses`.
    pub overrides: Overrides,
    /// `--socket-addr`, by socket name.
    pub socket_addresses: HashMap<String, String>,
    /// `--socket-fd`, by socket name: the listening sockets, as the descriptors duplicated for
    /// the server to own.
    pub socket_fds: HashMap<String, socket2::Socket>,
    /// `--inspector-addr`.
    pub inspector: Option<String>,
    /// `--control-fd`: where the `listen` events go, one JSON object per line.
    pub control: Option<std::fs::File>,
    /// `--debug-port`.
    pub debug_port: Option<String>,
    /// Where `serve` tells a person each socket's URL, unless a tool reads `--control-fd`.
    pub announce: Option<Announce>,
    /// `--cluster-addr` and `--cluster-advertise`.
    pub cluster_address: Option<String>,
    pub cluster_advertise: Option<String>,
    /// The source deployment of each appended service and of each service left absent because its
    /// deployment was skipped.
    pub deployments: Deployments,
    /// Sent the server once `run` has started every listener; dropped unsent if the config is
    /// refused or the start fails. `--control-fd`'s `listen` events, for a server embedded in this
    /// process. A run given it serves until drained, even with nothing to listen on.
    pub started: Option<oneshot::Sender<Weak<Server>>>,
    /// Drain as soon as told to, rather than serving on through `Config.shutdownDelayMs`.
    pub drain_at_once: bool,
    /// `--threads`, less the thread the server starts on: the loop threads to start besides it.
    pub replicas: usize,
}

/// The control channel (`--control-fd`), written line by line; a failed write is logged. The
/// events are formatted by hand to keep their fields in a fixed order, `port` last: consumers
/// include shell scripts that cut the port off the end of the line.
/// Without a control channel, the socket URLs go to `RunOptions::announce`.
struct Control(Option<std::fs::File>, Option<Announce>);

impl Control {
    fn write(&mut self, line: &str) {
        if let Some(file) = &mut self.0
            && let Err(e) = file.write_all(line.as_bytes())
        {
            tracing::error!("--control-fd: {e}");
        }
    }

    fn listen(&mut self, socket: &str, port: u16, url: Option<String>) {
        if let (None, Some(announce), Some(url)) = (&self.0, &self.1, url) {
            announce(&format!("Listening on {url}"));
        }
        let socket = serde_json::Value::from(socket);
        self.write(&format!(
            "{{\"event\":\"listen\",\"socket\":{socket},\"port\":{port}}}\n"
        ));
    }

    fn listen_inspector(&mut self, port: u16) {
        self.write(&format!(
            "{{\"event\":\"listen-inspector\",\"port\":{port}}}\n"
        ));
    }

    fn listen_cluster(&mut self, listener: &tokio::net::TcpListener) -> Result<()> {
        let port = listener
            .local_addr()
            .map_err(|e| kj::failed!("{e}"))?
            .port();
        self.write(&format!(
            "{{\"event\":\"listen-cluster\",\"port\":{port}}}\n"
        ));
        Ok(())
    }
}

/// The server's listener futures; each ends when the server has drained, or fails fatally.
type Listeners = FuturesUnordered<LocalBoxFuture<'static, Result<()>>>;

/// Everything set up before either command diverges: the graph, the listeners, the drain
/// signal.
struct Running {
    server: Rc<Server>,
    /// The listeners; the replicas, if any, are one of them (`threads::Started::watch`).
    listeners: Listeners,
    drain: watch::Sender<bool>,
    /// The other loop threads of the run, which end with it.
    replicas: Option<Replicas>,
}

/// The loop thread a server is started on.
enum Role {
    /// The process's first: it binds the sockets and has everything a process has one of.
    First,
    /// Another, which listens on duplicates of the first's sockets.
    Replica(DuplicateSockets),
}

/// Binds the sockets, starts the inspector, builds the service graph, and starts the listeners;
/// on the first loop thread of several, also starts the replicas and waits until each is ready.
/// Config errors go to `report`; there is nothing to run (`None`) if it refuses to serve with
/// them. `for_test` exempts the `TEST_TMPDIR` directory override from the unmatched-override
/// check.
///
/// A replica (see `threads.rs`) gets no `options` but the overrides, so it has no control
/// channel, inspector or debug port, it arms no cron trigger, and it does not join the cluster:
/// its factory forwards to the first loop thread what needs one (`Factory::first_thread`).
#[expect(clippy::too_many_lines, reason = "a server's start, in order")]
async fn start(
    factory: Rc<Factory>,
    mut options: RunOptions,
    report: Reporter,
    for_test: bool,
    role: Role,
) -> Result<Option<Running>> {
    let message = factory.config()?;
    let config = message.get_root::<config::Reader>().map_err(capnp_error)?;
    let mut control = Control(options.control.take(), options.announce.take());

    let is_replica = matches!(role, Role::Replica(_));
    let bound = sockets(role, config, &mut options, &factory, &report).await?;

    // What several threads cannot serve is a config error. A reporter that serves despite errors
    // (`--watch`) then serves on this thread alone, like any config with an error.
    let replicas = (options.replicas > 0).then(|| Replicas::new(options.replicas, &factory));
    if replicas.is_some() {
        for obstacle in threads::obstacles(options.inspector.is_some()) {
            report.error(obstacle);
        }
    }

    // The inspector starts before any isolate exists, so that every isolate registers with it.
    if let Some(address) = &options.inspector {
        let port = ffi::factory_start_inspector(factory.raw(), address)?;
        control.listen_inspector(port);
    }

    // The cluster port is bound before the services, like the sockets: its address goes in the
    // lease record.
    let cluster_port = if config.has_cluster() && !is_replica {
        let conf = config.get_cluster().map_err(capnp_error)?;
        let (address, advertise) = (&options.cluster_address, &options.cluster_advertise);
        Some(cluster::bind(conf, address.as_deref(), advertise.as_deref()).await?)
    } else {
        None
    };
    let server = Server::start(
        Rc::clone(&factory),
        &options.overrides,
        report,
        bound.inbound,
        cluster_port.as_ref().map(|(_, address)| address.clone()),
    )
    .await?;
    report_unmatched_overrides(&server, &options.overrides, config, for_test)?;

    let (drain, draining) = watch::channel(false);
    let context = Rc::new(ListenContext {
        factory: Rc::clone(&factory),
        draining,
    });
    let replica_count = replicas.as_ref().map_or(0, Replicas::count);
    let SocketListeners {
        listening,
        mut listeners,
        duplicates,
    } = socket_listeners(&context, config, bound.sockets, &server, replica_count)?;
    // Every config error is reported by now; a config that is refused gets no `listen` event.
    if server.report().refuses() {
        return Ok(None);
    }
    // The replicas build their graphs once this thread accepted the config, and are ready
    // before the process says it listens. Without a stream socket they would have nothing to
    // serve.
    let serves_streams = |sockets: &DuplicateSockets| sockets.iter().any(Option::is_some);
    let replicas =
        replicas.filter(|_| !server.report().had_errors() && duplicates.iter().any(serves_streams));
    let mut started = match &replicas {
        Some(replicas) => {
            let deployments = server.report().replica_deployments();
            Some(
                replicas
                    .start(&factory, duplicates, &options.overrides, &deployments)
                    .await?,
            )
        }
        None => None,
    };
    // The cluster's event goes before the sockets', so a consumer that stops reading at a
    // socket's has it.
    if let (Some(node), Some((listener, _))) = (server.cluster(), cluster_port) {
        if let Some(started) = &mut started {
            started.check()?;
        }
        start_cluster(&factory, node, listener, &mut control)?;
    }
    if let Some(mut started) = started {
        started.check()?;
        listeners.push(started.watch());
    }
    for socket in listening {
        control.listen(&socket.name, socket.port, socket.url);
    }
    if !is_replica {
        listeners.extend(crate::cron::start(&context, &server, config, for_test)?);
    }

    if let Some(address) = &options.debug_port {
        let listener = listen::listen(address, 0, factory.loopback()).await?;
        control.listen("debug-port", listener.port()?, None);
        listeners.push(Box::pin(listen::listen_debug_port(
            Rc::clone(&context),
            listener,
        )));
    }
    if let Some(started) = options.started.take() {
        // The host reaches an embedded server through its own channels too, so the server serves
        // until the drain even if no socket, trigger or port keeps a listener running.
        let mut draining = context.draining.clone();
        listeners.push(Box::pin(async move {
            // The sender is the run's, which outlives its listeners.
            let _ = draining.wait_for(|draining| *draining).await;
            Ok(())
        }));
        // The receiver may be gone; nothing else depends on the signal.
        let _ = started.send(Rc::downgrade(&server));
    }

    if let Some(replicas) = &replicas {
        replicas.serve();
    }
    Ok(Some(Running {
        server,
        listeners,
        drain,
        replicas,
    }))
}

/// The config's sockets for the thread of `role`: bound, with the command line's overrides for
/// them taken out of `options` and those that match no socket reported, or adopted.
async fn sockets(
    role: Role,
    config: config::Reader<'_>,
    options: &mut RunOptions,
    factory: &Factory,
    report: &Reporter,
) -> Result<listen::BoundSockets> {
    if let Role::Replica(sockets) = role {
        return listen::adopt_sockets(config, sockets);
    }
    let bound = listen::bind_sockets(
        config,
        &mut options.socket_addresses,
        &mut options.socket_fds,
        factory.loopback(),
        report,
    )
    .await?;
    for name in options
        .socket_addresses
        .keys()
        .chain(options.socket_fds.keys())
    {
        report.error(format!(
            "Config did not define any socket named \"{name}\" to match the override provided \
             on the command line."
        ));
    }
    Ok(bound)
}

/// Runs this instance's part in the cluster: the peer port, the peer listing, and compaction. They
/// run for the life of the process, as does the lease, which the lease thread keeps from `join` on;
/// a lease the instance cannot keep ends it (`ClusterNode::new`).
fn start_cluster(
    factory: &Rc<Factory>,
    node: &Rc<ClusterNode>,
    listener: tokio::net::TcpListener,
    control: &mut Control,
) -> Result<()> {
    control.listen_cluster(&listener)?;
    factory.spawn_detached(cluster::serve(listener, Rc::clone(node)));
    let cluster = Rc::clone(&node.cluster);
    factory.spawn_detached(async move { cluster.list_loop().await });
    let cluster = Rc::clone(&node.cluster);
    factory.spawn_detached(async move { cluster.tier_loop().await });
    let node = Rc::clone(node);
    factory.spawn_detached(async move { node.alarms_loop().await });
    Ok(())
}

/// A socket that listens, as its `listen` event reports it.
struct ListeningSocket {
    name: String,
    port: u16,
    url: Option<String>,
}

/// What `socket_listeners` makes of the bound sockets.
struct SocketListeners {
    /// The sockets that listen, for their `listen` events.
    listening: Vec<ListeningSocket>,
    listeners: Listeners,
    /// For each replica, its duplicates of the stream sockets.
    duplicates: Vec<DuplicateSockets>,
}

/// Starts the listener of every socket of the config that bound, and makes `replicas` duplicates
/// of each. A socket whose service does not resolve is a config error.
fn socket_listeners(
    context: &Rc<ListenContext>,
    config: config::Reader<'_>,
    bound: Vec<Option<(BoundSocket, String)>>,
    server: &Server,
    replicas: usize,
) -> Result<SocketListeners> {
    let mut listening = Vec::new();
    let listeners = Listeners::new();
    let mut duplicates: Vec<DuplicateSockets> = Vec::new();
    duplicates.resize_with(replicas, Vec::new);
    let sockets = config.get_sockets().map_err(capnp_error)?;
    for (sock, bound) in sockets.iter().zip(bound) {
        // A socket that failed to bind already reported its error; a replica has no datagram
        // sockets.
        let Some((bound, address)) = bound else {
            for sockets in &mut duplicates {
                sockets.push(None);
            }
            continue;
        };
        for sockets in &mut duplicates {
            let duplicate = listen::duplicate_listener(&bound)?;
            sockets.push(duplicate.map(|duplicate| (duplicate, address.clone())));
        }
        let name = text(sock.get_name())?;
        let mut errors = Vec::new();
        let designator = Designator::from_reader(
            sock.get_service().map_err(capnp_error)?,
            format!("Socket \"{name}\""),
            &mut errors,
        )
        .map_err(capnp_error)?;
        for error in errors {
            server.report().error(error);
        }
        let channel = server.lookup(&designator);
        let port = listen::bound_port(&bound)?;
        let url = listen::url(sock, &address, port);
        listening.push(ListeningSocket { name, port, url });
        if let Some(listener) =
            listener(Rc::clone(context), sock, bound, &address, channel, server)?
        {
            listeners.push(listener);
        }
    }
    Ok(SocketListeners {
        listening,
        listeners,
        duplicates,
    })
}

/// The listener for one bound socket, or none for a socket whose type is unknown (reported).
fn listener(
    context: Rc<ListenContext>,
    sock: socket::Reader<'_>,
    bound: BoundSocket,
    address: &str,
    channel: Rc<dyn Channel>,
    server: &Server,
) -> Result<Option<LocalBoxFuture<'static, Result<()>>>> {
    let name = text(sock.get_name())?;
    let tls = |options: workerd_capnp::tls_options::Reader<'_>| {
        kj_hyper::tls::server_config(&tls_options(options)?)
    };
    let http = |listener: StreamListener,
                options: workerd_capnp::http_options::Reader<'_>,
                physical_protocol: &'static str,
                tls: Option<std::sync::Arc<rustls::ServerConfig>>,
                context: Rc<ListenContext>,
                channel: Rc<dyn Channel>| {
        let socket = Rc::new(HttpSocket {
            channel,
            rewriter: Rc::new(HttpRewriter::new(options)?),
            physical_protocol,
            tls,
        });
        let listener: LocalBoxFuture<'static, Result<()>> =
            Box::pin(listen::listen_http(context, listener, socket));
        Ok::<_, crate::Error>(listener)
    };
    Ok(Some(match (sock.which(), bound) {
        (Ok(socket::Which::Http(options)), BoundSocket::Stream(listener)) => http(
            listener,
            options.map_err(capnp_error)?,
            "http",
            None,
            context,
            channel,
        )?,
        (Ok(socket::Which::Https(https)), BoundSocket::Stream(listener)) => {
            let tls = tls(https.get_tls_options().map_err(capnp_error)?)?;
            http(
                listener,
                https.get_options().map_err(capnp_error)?,
                "https",
                Some(tls),
                context,
                channel,
            )?
        }
        (Ok(socket::Which::Tcp(tcp)), BoundSocket::Stream(listener)) => {
            let tls = if tcp.has_tls_options() {
                let config = tls(tcp.get_tls_options().map_err(capnp_error)?)?;
                Some(match tcp.get_tls_mode() {
                    Ok(socket::TcpTlsMode::Implicit) => TcpTls::Implicit(config),
                    Ok(socket::TcpTlsMode::StartTls) => TcpTls::StartTls(config),
                    Err(capnp::NotInSchema(_)) => {
                        return Err(kj::failed!(
                            "Encountered unknown tlsMode in \"{name}\". Was the config compiled \
                             with a newer version of the schema?"
                        ));
                    }
                })
            } else {
                None
            };
            // The authority handed to the connect() handler is the endpoint as bound, so it is
            // truthful for a configured port of 0.
            let authority = format!("{}:{}", listen::host_of_address(address), listener.port()?);
            Box::pin(listen::listen_tcp(
                context, listener, channel, tls, authority,
            ))
        }
        (Ok(socket::Which::Udp(udp)), BoundSocket::Datagram(socket)) => {
            Box::pin(listen::udp::listen_udp(
                context,
                socket,
                channel,
                address.to_owned(),
                Duration::from_millis(udp.get_idle_timeout_ms().into()),
                udp.get_max_pending_bytes() as usize,
            ))
        }
        (Err(capnp::NotInSchema(_)), _) => {
            server.report().error(format!(
                "Encountered unknown socket type in \"{name}\". Was the config compiled with a \
                 newer version of the schema?"
            ));
            return Ok(None);
        }
        // bind_sockets binds a datagram socket for UDP and a stream listener otherwise.
        _ => return Err(kj::failed!("socket \"{name}\" was bound as the wrong kind")),
    }))
}

/// Reports the `--external-addr` and `--directory-path` overrides that name no service of their
/// kind.
fn report_unmatched_overrides(
    server: &Server,
    overrides: &Overrides,
    config: config::Reader<'_>,
    for_test: bool,
) -> Result<()> {
    let mut externals: Vec<&str> = overrides.externals.keys().map(String::as_str).collect();
    let mut directories: Vec<&str> = overrides.directories.keys().map(String::as_str).collect();
    for conf in config.get_services().map_err(capnp_error)? {
        let name = text(conf.get_name())?;
        // The overrides are for the node's own services, so whether one matches does not depend
        // on the bucket's deployments.
        if server.report().from_deployment(&name) {
            continue;
        }
        match conf.which() {
            Ok(service::Which::External(_)) => externals.retain(|n| *n != name),
            Ok(service::Which::Disk(_) | service::Which::Cache(_)) => {
                directories.retain(|n| *n != name);
            }
            _ => {}
        }
    }
    for name in externals {
        server.report().error(format!(
            "Config did not define any external service named \"{name}\" to match the override \
             provided on the command line."
        ));
    }
    for name in directories {
        // Due to a historical bug, `workerd test` didn't check for the existence of unmatched
        // overrides, and our own tests became dependent on the ability to override TEST_TMPDIR
        // even if it was not used in the config.
        if for_test && name == "TEST_TMPDIR" {
            continue;
        }
        server.report().error(format!(
            "Config did not define any disk service named \"{name}\" to match the override \
             provided on the command line."
        ));
    }
    Ok(())
}

/// Drives the listeners until they all end (the server drained) or one fails.
async fn serve_listeners(listeners: &mut Listeners) -> Result<()> {
    while let Some(result) = listeners.next().await {
        result?;
    }
    Ok(())
}

/// `workerd serve`: serves until told to drain, then stops accepting connections, lets the open
/// ones finish, and stops the actors' containers. A listener failing is fatal and is the error
/// returned.
///
/// Each item of `drain`, which never ends, is a drain signal. After the first, the server serves
/// on for the config's `shutdownDelayMs`, or until the next signal, then drains (`drain_due`).
///
/// In a cluster, the instance first hands its Durable Objects off while it keeps serving, then
/// leaves the cluster, and accepts requests for a grace period at least; the cluster's
/// `stopTimeoutMs` bounds all of it, from the end of the shutdown delay.
///
/// The replicas, if any, serve through all of that as this thread's listeners do: they stop
/// accepting when it does, and it waits for them, with its actors and their sessions up, as it
/// waits for its own connections. A request a replica has in an object that is handed off, or
/// sends to one, is this thread's own to the object (`ActorNamespace::channel`).
#[expect(clippy::too_many_lines, reason = "a server's life, from start to stop")]
pub async fn run(
    factory: Rc<Factory>,
    options: RunOptions,
    report: Reporter,
    drain: impl Stream<Item = ()>,
) -> Result<()> {
    let delay = if options.drain_at_once {
        Duration::ZERO
    } else {
        shutdown_delay(&factory)?
    };
    // The replicas are bound last, so they are dropped, and with that stopped and joined, before
    // this thread's server.
    let Some(Running {
        server,
        mut listeners,
        drain: draining,
        replicas,
    }) = start(factory, options, report, false, Role::First).await?
    else {
        return Ok(());
    };

    // A SIGTERM that arrived while the server started reaches `drain` only once the runtime has
    // polled its drivers, which a start that never had to wait may not have let it do. tokio wakes
    // a task that yields only after it has polled them, so the check below sees such a signal.
    tokio::task::yield_now().await;
    let mut drain = std::pin::pin!(drain);
    let pending = drain.next().now_or_never().is_some();
    // From here on the lease thread watches the event loop. The cluster port answers ready
    // until the instance is told to drain or this returns.
    let ready = server.cluster().map(ClusterNode::ready);
    let drain = std::pin::pin!(drain_due(server.factory(), delay, pending, ready, drain));
    let mut serving = std::pin::pin!(serve_listeners(&mut listeners).fuse());
    let mut stop = None;
    // Draining stops the accept loops; the listeners then end once their connections have.
    match futures::future::select(serving.as_mut(), drain).await {
        Either::Left((result, _)) => result?,
        Either::Right(((), _)) => {
            // A draining instance claims nothing more and hands its clustered objects off,
            // `preventEviction` or not, while it serves on: a request for one waits for it and
            // then goes where it runs next. Then it leaves the cluster, so peers take over what
            // it did not hand off, accepts requests until the grace period ends, as load
            // balancers may still send some, and lets its connections finish, its peers' sessions
            // last (`ClusterNode::stop_peer_sessions`).
            if let Some(node) = server.cluster() {
                let deadlines = node.start_draining();
                let workers = server
                    .services()
                    .filter_map(|(_, service)| service.as_worker());
                let namespaces: Vec<_> = workers
                    .flat_map(|worker| worker.namespaces())
                    .map(|(_, ns)| ns)
                    .filter(|ns| ns.cluster().is_some())
                    .collect();
                let handoff = std::pin::pin!(actor::hand_off_all(&namespaces, deadlines));
                let stopped = match futures::future::select(serving.as_mut(), handoff).await {
                    Either::Left((result, handoff)) => {
                        result?;
                        handoff.await
                    }
                    Either::Right((stopped, _)) => stopped,
                };
                // Released before the lease goes, so nothing is served after it, and gone no
                // sooner than `leave`, so no cache copy outlives it either. Until the
                // deregistration lands (milliseconds after that, or the lease's 20 s if it fails),
                // a peer that reads this node's record still finds it live and routes here again.
                // With objects still running, which stopping very many can leave, it stays a
                // member: peers take over every object it did not hand off once its lease
                // expires, after this process exited.
                if stopped {
                    let deregister = async {
                        tokio::time::sleep_until(deadlines.leave).await;
                        node.cluster.deregister().await
                    };
                    match tokio::time::timeout_at(deadlines.connections, deregister).await {
                        Ok(Ok(())) => {}
                        Ok(Err(e)) => tracing::warn!("leaving the cluster: {e}"),
                        Err(_) => tracing::warn!("leaving the cluster timed out"),
                    }
                } else {
                    tracing::warn!(
                        "Durable Objects still run at the release deadline: staying a cluster \
                         member, so that peers take over its objects once this node's lease \
                         expires"
                    );
                }
                let grace = std::pin::pin!(tokio::time::sleep_until(deadlines.grace));
                if let Either::Left((result, _)) =
                    futures::future::select(serving.as_mut(), grace).await
                {
                    result?;
                }
                stop = Some((Rc::clone(node), deadlines));
            }
            draining.send_replace(true);
            if let Some(replicas) = &replicas {
                replicas.drain();
            }
            if let Some((node, deadlines)) = &stop {
                let finished = async {
                    if !serving.is_terminated() {
                        serving.as_mut().await?;
                    }
                    node.stop_peer_sessions().await;
                    Ok::<_, crate::Error>(())
                };
                let finished = tokio::time::timeout_at(deadlines.connections, finished);
                if let Ok(result) = finished.await {
                    result?;
                } else {
                    tracing::warn!(
                        "stopping with requests in flight at the connections deadline: they end \
                         as the process exits"
                    );
                }
            } else {
                serving.await?;
            }
        }
    }

    // All incoming requests have drained. Stop container-enabled actors so they cannot race
    // their terminal Docker cleanup, then wait for it while their namespaces remain available.
    let workers = server
        .services()
        .filter_map(|(_, service)| service.as_worker());
    let until = stop.as_ref().map(|(_, deadlines)| deadlines.stop);
    for (_, namespace) in workers.flat_map(|worker| worker.namespaces()) {
        namespace.begin_container_cleanup(until).await;
    }
    let cleanup = ffi::factory_shutdown_containers(server.factory().raw());
    let Some((_, deadlines)) = stop else {
        return Ok(cleanup.await?);
    };
    if let Ok(result) = tokio::time::timeout_at(deadlines.stop, cleanup).await {
        return Ok(result?);
    }
    tracing::warn!("leaving containers behind at the stop deadline");
    Ok(())
}

/// The config's `shutdownDelayMs`.
fn shutdown_delay(factory: &Factory) -> Result<Duration> {
    let message = factory.config()?;
    let config = message.get_root::<config::Reader>().map_err(capnp_error)?;
    Ok(Duration::from_millis(config.get_shutdown_delay_ms().into()))
}

/// Resolves when `run` is to drain. That is at once if a signal was `pending` as the server
/// started: it was never ready, so nothing sends it traffic to serve through the delay. Otherwise
/// it is `delay` (`Config.shutdownDelayMs`) after the next item of `drain`, or on the item after
/// it, whichever comes first. Load balancers go on sending connections for a while, which are
/// served as before meanwhile, and a cluster node keeps its Durable Objects and its place in the
/// cluster. Its readiness probe fails from the signal on (`ready` is dropped), so that what routes
/// by it sends no more.
async fn drain_due(
    factory: &Factory,
    delay: Duration,
    pending: bool,
    ready: Option<Ready>,
    mut drain: Pin<&mut impl Stream<Item = ()>>,
) {
    if pending {
        return;
    }
    drain.next().await;
    drop(ready);
    if delay.is_zero() {
        return;
    }
    let seconds = delay.as_secs_f64();
    tracing::warn!(
        "drain signal received: serving for {seconds:.1} s more (shutdownDelayMs), then \
         draining; a second signal drains at once"
    );
    let delay = std::pin::pin!(factory.sleep(delay));
    futures::future::select(delay, drain.next()).await;
}

/// `workerd serve` on a loop thread other than the first (see `threads.rs`).
///
/// Builds the graph of `factory`'s config for this thread and, once the process serves, accepts
/// from `sockets` until it drains, then lets the open connections finish.
///
/// The first thread accepted the config, so an error in it here is this thread's failure. The
/// errors and warnings are the ones the first thread reported, and are not reported again.
pub async fn run_replica(
    factory: Rc<Factory>,
    overrides: Overrides,
    deployments: Deployments,
    sockets: DuplicateSockets,
    signals: &Signals,
) -> Result<()> {
    let errors = Rc::new(std::cell::RefCell::new(Vec::new()));
    // A deployment's errors leave this thread serving, as they left the first, with the same
    // services broken.
    let report = Reporter::new(
        Box::new({
            let errors = Rc::clone(&errors);
            move |error| errors.borrow_mut().push(error)
        }),
        Box::new(drop),
        false,
    )
    .with_deployments(deployments, Box::new(drop), Box::new(drop));
    let options = RunOptions {
        overrides,
        ..RunOptions::default()
    };
    let role = Role::Replica(sockets);
    let Some(Running {
        server: _server,
        mut listeners,
        drain: draining,
        ..
    }) = start(Rc::clone(&factory), options, report, false, role).await?
    else {
        return Err(kj::failed!(
            "the config has errors here that it did not have on the first loop thread: {}",
            errors.borrow().join(" ")
        ));
    };

    let serve = async {
        signals.ready();
        if signals.reached(Phase::Serving).await == Phase::Stopping {
            return Ok(());
        }
        let mut serving = std::pin::pin!(serve_listeners(&mut listeners));
        let drain = std::pin::pin!(signals.reached(Phase::Draining));
        match futures::future::select(serving.as_mut(), drain).await {
            // The listeners end only once they drain: this is a failure, or a thread that
            // stopped serving, which the first thread takes for one.
            Either::Left((result, _)) => return result,
            Either::Right((Phase::Stopping, _)) => return Ok(()),
            Either::Right(_) => {}
        }
        draining.send_replace(true);
        let stop = std::pin::pin!(signals.reached(Phase::Stopping));
        match futures::future::select(serving, stop).await {
            Either::Left((result, _)) => result,
            Either::Right(_) => Ok(()),
        }
    };
    // The first thread serves this thread's session until this thread has ended, so a session
    // that ends is one that failed, and this thread's Durable Object calls with it.
    let lost = async {
        match factory.first_thread() {
            Some(first) => first.disconnected().await,
            None => std::future::pending().await,
        }
    };
    match futures::future::select(std::pin::pin!(serve), std::pin::pin!(lost)).await {
        Either::Left((result, _)) => result,
        Either::Right(((), _)) => Err(kj::disconnected!(
            "the session with the first loop thread ended"
        )),
    }
}

/// An entrypoint `workerd test` or `workerd bench` runs a handler of.
struct Target {
    channel: Rc<dyn Channel>,
    /// "service" or "service:entrypoint".
    name: String,
    /// The entrypoint's service and its name.
    service: (String, Rc<WorkerService>),
}
type Targets = Vec<Target>;

/// Starts the server for `workerd test` or `workerd bench`, and selects the entrypoints that
/// match the patterns (globs as the `glob` crate reads them, matched against the whole name) and
/// export `handler`. `None` for a config `report` refuses, whose handlers do not run.
async fn start_local(
    factory: Rc<Factory>,
    options: RunOptions,
    report: Reporter,
    (service_pattern, entrypoint_pattern): (&str, &str),
    handler: &str,
) -> Result<Option<(Running, Targets)>> {
    let glob = |pattern: &str| {
        glob::Pattern::new(pattern).map_err(|e| kj::failed!("{handler} filter \"{pattern}\": {e}"))
    };
    let (services, entrypoints) = (glob(service_pattern)?, glob(entrypoint_pattern)?);
    let Some(running) = start(factory, options, report, true, Role::First).await? else {
        return Ok(None);
    };

    let mut targets = Vec::new();
    for (name, service) in running.server.services() {
        if !services.matches(name) {
            continue;
        }
        let Some(worker) = service.as_worker() else {
            continue;
        };
        let target = |channel: Rc<dyn Channel>, target_name: String| Target {
            channel,
            name: target_name,
            service: (name.to_owned(), Rc::clone(worker)),
        };
        if worker.has_handler(None, handler) && entrypoints.matches("default") {
            targets.push(target(service.channel(), name.to_owned()));
        }
        for entrypoint in worker.entrypoint_names() {
            if entrypoints.matches(entrypoint)
                && worker.has_handler(Some(entrypoint), handler)
                && let Some(channel) = worker.entrypoint(Some(entrypoint), None, false)
            {
                targets.push(target(channel, format!("{name}:{entrypoint}")));
            }
        }
    }
    Ok(Some((running, targets)))
}

impl Running {
    /// Runs `handlers` while the listeners serve; a listener failing meanwhile is fatal, as in
    /// `run()`.
    ///
    /// `handlers` runs as a task of the KJ loop: each handler starts in the turn after the one
    /// before it returned, ahead of the events that one left queued (its tail workers'
    /// deliveries), where this future would resume only once the loop is idle. The task should
    /// hold the channels, not the server, whose drop clears the tasks.
    async fn alongside<T: 'static>(
        &mut self,
        handlers: impl Future<Output = Result<T>> + 'static,
    ) -> Result<T> {
        let handlers = self.server.factory().spawn(handlers);
        let serving = std::pin::pin!(serve_listeners(&mut self.listeners));
        match futures::future::select(handlers, serving).await {
            Either::Left((result, _)) => result,
            Either::Right((result, handlers)) => {
                result?;
                handlers.await
            }
        }
    }
}

/// `workerd test`: runs the `test()` handler of every entrypoint matching the patterns (see
/// `start_local`).
///
/// The sockets listen meanwhile (tests can configure them). True if every test passed and there
/// was at least one; `None` for a config `report` refuses, whose tests do not run.
pub async fn test(
    factory: Rc<Factory>,
    options: RunOptions,
    report: Reporter,
    service_pattern: &str,
    entrypoint_pattern: &str,
) -> Result<Option<bool>> {
    let patterns = (service_pattern, entrypoint_pattern);
    let Some((mut running, cases)) =
        start_local(factory, options, report, patterns, "test").await?
    else {
        return Ok(None);
    };

    // Test harnesses match these lines; they are debug-level because info logging is
    // optional and a warning or error would confuse people.
    let run_one = |channel: Rc<dyn Channel>, name: String| async move {
        log::debug(&format!("[ TEST ] {name}"));
        let metadata = ffi::new_request_metadata(KjMaybe::None, KjMaybe::None);
        let mut worker = CxxWorkerInterface::new(channel.start_request(metadata)?);
        let start = std::time::Instant::now();
        let result = worker.test().await?;
        let duration = start.elapsed();
        let verdict = if result { "PASS" } else { "FAIL" };
        log::debug(&format!("[ {verdict} ] {name} ({duration:?})"));
        Ok::<bool, crate::Error>(result)
    };

    let (passed, failed) = running
        .alongside(async move {
            let (mut passed, mut failed) = (0u32, 0u32);
            for Target { channel, name, .. } in cases {
                if run_one(channel, name).await? {
                    passed += 1;
                } else {
                    failed += 1;
                }
            }
            Ok((passed, failed))
        })
        .await?;

    if passed + failed == 0 {
        tracing::error!("No tests found!");
    }
    Ok(Some(passed > 0 && failed == 0))
}

/// `workerd bench`: runs the `bench()` handler of every entrypoint matching the patterns.
///
/// The entrypoints are selected as in `start_local`, and their handlers run as `test()` runs
/// `test()` handlers. Then the startup of each of their services' workers is timed in new
/// isolates.
///
/// The handlers and the startup timing get `params`, an encoded `BenchParams` message
/// (bench.capnp). Returns the report, with a group for each handler, then a startup group for
/// each service; `None` for a config `report` refuses, whose handlers do not run. The startup
/// groups go last so that the isolates they create and destroy don't disturb the handlers'
/// measurements.
pub async fn bench(
    factory: Rc<Factory>,
    options: RunOptions,
    report: Reporter,
    service_pattern: &str,
    entrypoint_pattern: &str,
    params: Vec<u64>,
) -> Result<Option<bench::Report>> {
    let patterns = (service_pattern, entrypoint_pattern);
    let Some((mut running, targets)) =
        start_local(factory, options, report, patterns, "bench").await?
    else {
        return Ok(None);
    };
    if targets.is_empty() {
        tracing::error!("No benchmarks found!");
        return Ok(Some(bench::Report::new(0)));
    }

    let mut services: Vec<(String, Rc<WorkerService>)> = Vec::new();
    if !bench::skips_startup(&params)? {
        for target in &targets {
            if !services.iter().any(|(name, _)| *name == target.service.0) {
                services.push(target.service.clone());
            }
        }
    }
    let results = running
        .alongside(async move {
            let groups = len_u32(&targets) + len_u32(&services);
            let mut results = bench::Report::new(groups);
            for Target { channel, name, .. } in targets {
                results.run_group(&*channel, &name, &params).await?;
            }
            for (name, worker) in services {
                results.run_startup(&worker, &name, &params).await?;
            }
            Ok(results)
        })
        .await?;
    Ok(Some(results))
}
