// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `workerd serve`, `workerd test` and `workerd bench`: bind the sockets, build the service
//! graph, listen, and either wait for the drain signal or run the tests or benchmarks.

use std::collections::HashMap;
use std::future::Future;
use std::io::Write;
use std::rc::Rc;
use std::time::Duration;

use futures::StreamExt;
use futures::future::Either;
use futures::future::LocalBoxFuture;
use futures::stream::FuturesUnordered;
use kj_rs::KjMaybe;
use tokio::sync::watch;
use worker::CxxWorkerInterface;
use worker::Interface;
use workerd_capnp::config;
use workerd_capnp::service;
use workerd_capnp::socket;

use crate::Result;
use crate::bench;
use crate::bindings::Designator;
use crate::bindings::len_u32;
use crate::bridge::ffi;
use crate::channels::Channel;
use crate::cluster;
use crate::cluster::ClusterNode;
use crate::config::Factory;
use crate::config::Overrides;
use crate::config::Reporter;
use crate::config::Server;
use crate::config::capnp_error;
use crate::config::text;
use crate::listen;
use crate::listen::BoundSocket;
use crate::listen::HttpSocket;
use crate::listen::ListenContext;
use crate::listen::StreamListener;
use crate::listen::TcpTls;
use crate::log;
use crate::services::network::tls_options;
use crate::services::rewriter::HttpRewriter;

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
    /// `--cluster-addr` and `--cluster-advertise`.
    pub cluster_address: Option<String>,
    pub cluster_advertise: Option<String>,
}

/// The control channel (`--control-fd`), written line by line; a failed write is logged. The
/// events are formatted by hand to keep their fields in a fixed order, `port` last: consumers
/// include shell scripts that cut the port off the end of the line.
struct Control(Option<std::fs::File>);

impl Control {
    fn write(&mut self, line: &str) {
        if let Some(file) = &mut self.0
            && let Err(e) = file.write_all(line.as_bytes())
        {
            tracing::error!("--control-fd: {e}");
        }
    }

    fn listen(&mut self, socket: &str, port: u16) {
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
}

/// Everything set up before either command diverges: the graph, the listeners, the drain
/// signal.
struct Running {
    server: Rc<Server>,
    /// The listener futures; each ends when the server has drained, or fails fatally.
    listeners: FuturesUnordered<LocalBoxFuture<'static, Result<()>>>,
    drain: watch::Sender<bool>,
}

/// Binds the sockets, starts the inspector, builds the service graph, and starts the listeners.
/// Config errors go to `report`; there is nothing to run (`None`) if it refuses to serve with
/// them. `for_test` exempts the `TEST_TMPDIR` directory override from the unmatched-override
/// check.
async fn start(
    factory: Rc<Factory>,
    mut options: RunOptions,
    report: Reporter,
    for_test: bool,
) -> Result<Option<Running>> {
    let message = factory.config()?;
    let config = message.get_root::<config::Reader>().map_err(capnp_error)?;
    let mut control = Control(options.control.take());

    let bound = listen::bind_sockets(
        config,
        &mut options.socket_addresses,
        &mut options.socket_fds,
        factory.loopback(),
        &report,
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

    // The inspector starts before any isolate exists, so that every isolate registers with it.
    if let Some(address) = &options.inspector {
        let port = ffi::factory_start_inspector(factory.raw(), address)?;
        control.listen_inspector(port);
    }

    // The cluster port is bound before the services, like the sockets: its address goes in the
    // lease record.
    let cluster_port = if config.has_cluster() {
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
    let listeners = FuturesUnordered::new();
    let mut ports = Vec::new();
    let sockets = config.get_sockets().map_err(capnp_error)?;
    for (sock, bound) in sockets.iter().zip(bound.sockets) {
        // A socket that failed to bind already reported its error.
        let Some((bound, address)) = bound else {
            continue;
        };
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
        ports.push((name, listen::bound_port(&bound)?));
        if let Some(listener) =
            listener(Rc::clone(&context), sock, bound, &address, channel, &server)?
        {
            listeners.push(listener);
        }
    }
    // Every config error is reported by now; a config that is refused gets no `listen` event.
    if server.report().refuses() {
        return Ok(None);
    }
    for (name, port) in ports {
        control.listen(&name, port);
    }
    if let (Some(node), Some((listener, _))) = (server.cluster(), cluster_port) {
        start_cluster(&factory, node, listener);
    }

    if let Some(address) = &options.debug_port {
        let listener = listen::listen(address, 0, factory.loopback()).await?;
        control.listen("debug-port", listener.port()?);
        listeners.push(Box::pin(listen::listen_debug_port(
            Rc::clone(&context),
            listener,
        )));
    }

    Ok(Some(Running {
        server,
        listeners,
        drain,
    }))
}

/// Runs this instance's part in the cluster: the peer port, the lease, and compaction. They run for
/// the life of the process; a lease the instance cannot keep ends it.
fn start_cluster(factory: &Rc<Factory>, node: &Rc<ClusterNode>, listener: tokio::net::TcpListener) {
    factory.spawn_detached(cluster::serve(listener, Rc::clone(node)));
    let cluster = Rc::clone(&node.cluster);
    factory.spawn_detached(async move {
        let reason = cluster.maintain().await;
        // Peers may take this instance's objects over once its lease lapses: it must stop
        // serving first.
        tracing::error!("leaving the cluster: {reason}");
        std::process::exit(1);
    });
    let cluster = Rc::clone(&node.cluster);
    factory.spawn_detached(async move { cluster.tier_loop().await });
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
        match conf.which() {
            Ok(service::Which::External(_)) => externals.retain(|n| *n != name),
            Ok(service::Which::Disk(_)) => directories.retain(|n| *n != name),
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
async fn serve_listeners(
    listeners: &mut FuturesUnordered<LocalBoxFuture<'static, Result<()>>>,
) -> Result<()> {
    while let Some(result) = listeners.next().await {
        result?;
    }
    Ok(())
}

/// `workerd serve`: serves until `drain` resolves, then stops accepting connections, lets the
/// open ones finish, and stops the actors' containers. A listener failing is fatal and is the
/// error returned.
pub async fn run(
    factory: Rc<Factory>,
    options: RunOptions,
    report: Reporter,
    drain: impl Future<Output = ()>,
) -> Result<()> {
    let Some(Running {
        server,
        mut listeners,
        drain: draining,
    }) = start(factory, options, report, false).await?
    else {
        return Ok(());
    };

    let drain = std::pin::pin!(drain);
    let serving = std::pin::pin!(serve_listeners(&mut listeners));
    // Draining stops the accept loops; the listeners then end once their connections have.
    match futures::future::select(serving, drain).await {
        Either::Left((result, _)) => result?,
        Either::Right(((), serving)) => {
            // A draining instance claims nothing more and lets its objects go, so peers take
            // them over while its connections finish.
            if let Some(node) = server.cluster() {
                node.cluster.start_draining();
                server.abort_all_actors(None);
            }
            draining.send_replace(true);
            serving.await?;
            if let Some(node) = server.cluster()
                && let Err(e) = node.cluster.deregister().await
            {
                tracing::warn!("leaving the cluster: {e}");
            }
        }
    }

    // All incoming requests have drained. Stop container-enabled actors so they cannot race
    // their terminal Docker cleanup, then wait for it while their namespaces remain available.
    let workers = server
        .services()
        .filter_map(|(_, service)| service.as_worker());
    for (_, namespace) in workers.flat_map(|worker| worker.namespaces()) {
        namespace.begin_container_cleanup();
    }
    Ok(ffi::factory_shutdown_containers(server.factory().raw()).await?)
}

/// The entrypoints `workerd test` or `workerd bench` runs a handler of: the channel of each,
/// and its name ("service" or "service:entrypoint").
type Targets = Vec<(Rc<dyn Channel>, String)>;

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
    let Some(running) = start(factory, options, report, true).await? else {
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
        if worker.has_handler(None, handler) && entrypoints.matches("default") {
            targets.push((service.channel(), name.to_owned()));
        }
        for entrypoint in worker.entrypoint_names() {
            if entrypoints.matches(entrypoint)
                && worker.has_handler(Some(entrypoint), handler)
                && let Some(channel) = worker.entrypoint(Some(entrypoint), None, false)
            {
                targets.push((channel, format!("{name}:{entrypoint}")));
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
            for (channel, name) in cases {
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

/// `workerd bench`: runs the `bench()` handler of every entrypoint matching the patterns (see
/// `start_local`), as `test()` runs `test()` handlers.
///
/// The handlers get `params`, an encoded `BenchParams` message (bench.capnp). Returns the
/// report, with a group for each handler; `None` for a config `report` refuses, whose handlers
/// do not run.
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

    let results = running
        .alongside(async move {
            let mut results = bench::Report::new(len_u32(&targets));
            for (channel, name) in targets {
                results.run_group(&*channel, &name, &params).await?;
            }
            Ok(results)
        })
        .await?;
    Ok(Some(results))
}
