// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The server run inside another program's event loop: `server-test.c++`, and each config
//! `workerd repl` loads (`repl/repl-server.c++`).
//!
//! The host builds the worker factory itself (its V8, timer, network and filesystem) and reaches
//! the server through the `loopback:` registry: the config's addresses are loopback names, and the
//! host connects to the server's sockets by name. A test also accepts, by name, the connections
//! the server makes, the `network` services' included (`Loopback::mock_internet`); the REPL's
//! servers reach the internet. Config errors and warnings queue for the host to take as they are
//! reported. Tests can mark services as coming from cluster deployments, as `workerd serve`
//! composes them; the server then serves those as a cluster node does, and their errors and
//! warnings queue with the rest.
//!
//! A server [`InProcessServer::start`] started serves its stream sockets on `loopback:` names
//! whatever their addresses, and hands its host channels to its services, its Durable Objects and
//! its http sockets (`host.rs`), which closing it revokes.

mod host;

use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::rc::Weak;

use futures::FutureExt;
use futures::future::Either;
use futures::future::RemoteHandle;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use tokio::sync::Notify;
use tokio::sync::mpsc;
use tokio::sync::oneshot;
use workerd_capnp::config;
use workerd_capnp::external_server;
use workerd_capnp::socket;

use self::host::Events;
use self::host::Revoker;
use crate::Result;
use crate::bench;
use crate::bridge::ffi;
use crate::channels::ServerHandle;
use crate::channels::SubrequestChannel;
use crate::config::ActorKey;
use crate::config::Deployments;
use crate::config::Factory;
use crate::config::Reporter;
use crate::config::Server;
use crate::config::capnp_error;
use crate::config::text;
use crate::hub::Hub;
use crate::hub::Seat;
use crate::listen::loopback::LoopbackListener;
use crate::placement::Placement;
use crate::run::RunOptions;
use crate::services::make_external_service;

pub struct InProcessServer {
    factory: Rc<Factory>,
    reports: mpsc::UnboundedSender<ffi::ConfigReport>,
    reported: RefCell<mpsc::UnboundedReceiver<ffi::ConfigReport>>,
    drain: Rc<Notify>,
    /// The run [`Self::start`] started, until the server is closed.
    serving: RefCell<Option<RemoteHandle<Result<()>>>>,
    /// The names the test accepts connections for.
    listeners: RefCell<HashMap<String, Rc<LoopbackListener>>>,
    /// The services marked as coming from deployments for the next run.
    deployments: RefCell<Deployments>,
    /// The server [`Self::start`] started, while it runs.
    server: RefCell<Weak<Server>>,
    /// Revokes the channels [`Self::service`] and [`Self::socket`] handed out.
    revoker: Revoker,
}

/// A server over `factory`. Its connections to public addresses reach the network, unless
/// [`InProcessServer::mock_internet`] is called first.
pub fn new_in_process_server(factory: KjOwn<ffi::WorkerFactory>) -> Box<InProcessServer> {
    crate::log::install();
    let factory = Rc::new(Factory::new(factory));
    factory.loopback().enable();
    let (reports, reported) = mpsc::unbounded_channel();
    Box::new(InProcessServer {
        factory,
        reports,
        reported: RefCell::new(reported),
        drain: Rc::new(Notify::new()),
        serving: RefCell::default(),
        listeners: RefCell::default(),
        deployments: RefCell::default(),
        server: RefCell::default(),
        revoker: Revoker::default(),
    })
}

/// Revokes what the host holds ([`InProcessServer::revoke`]), drains the run
/// [`InProcessServer::start`] started and waits for it to end.
///
/// Then ends the server's background tasks and drops the factory; a factory something still
/// holds is logged as an error. The run's failure, if it failed, is the error.
pub async fn close_in_process_server(server: Box<InProcessServer>) -> Result<()> {
    server.revoke("The server was closed.");
    let InProcessServer {
        factory,
        drain,
        serving,
        ..
    } = *server;
    let outcome = match serving.into_inner() {
        Some(serving) => {
            drain.notify_one();
            serving.await
        }
        None => Ok(()),
    };
    let shutdown = crate::entry::shutdown(factory).await;
    outcome?;
    shutdown
}

impl InProcessServer {
    fn reporter(&self) -> Reporter {
        let report = |error| {
            let reports = self.reports.clone();
            // The receiver lives as long as the sender: both are the server's.
            move |message| drop(reports.send(ffi::ConfigReport { error, message }))
        };
        let deployments = std::mem::take(&mut *self.deployments.borrow_mut());
        Reporter::new(Box::new(report(true)), Box::new(report(false)), true).with_deployments(
            deployments,
            Box::new(report(true)),
            Box::new(report(false)),
        )
    }

    /// Makes the server's connections to public addresses to loopback names instead, for a test to
    /// accept. Called before the server runs.
    pub fn mock_internet(&self) {
        self.factory.loopback().mock_internet();
    }

    /// Marks `service` in the config as defined by `deployment`.
    pub fn add_deployment_service(&self, deployment: &str, service: &str) {
        let mut deployments = self.deployments.borrow_mut();
        deployments.enabled = true;
        let deployment = deployment.to_owned();
        deployments.services.insert(service.to_owned(), deployment);
    }

    /// Marks absent `service` as defined by skipped deployment `deployment`.
    pub fn add_skipped_deployment_service(&self, deployment: &str, service: &str) {
        let mut deployments = self.deployments.borrow_mut();
        deployments.enabled = true;
        let deployment = deployment.to_owned();
        deployments.skipped.insert(service.to_owned(), deployment);
    }

    /// The next config error or warning the server reports.
    pub async fn next_report(&self) -> ffi::ConfigReport {
        // The borrow lasts one poll. The sender is this server's, so the channel never closes.
        match std::future::poll_fn(|cx| self.reported.borrow_mut().poll_recv(cx)).await {
            Some(report) => report,
            None => std::future::pending().await,
        }
    }

    /// The config errors and warnings reported so far and not yet taken.
    pub fn take_reports(&self) -> Vec<ffi::ConfigReport> {
        let mut reported = self.reported.borrow_mut();
        std::iter::from_fn(|| reported.try_recv().ok()).collect()
    }

    /// `workerd serve`, until [`Self::drain`]; `debug_port` is `--debug-port`, if not empty.
    pub async fn run(&self, debug_port: &str) -> Result<()> {
        let options = RunOptions {
            debug_port: (!debug_port.is_empty()).then(|| debug_port.to_owned()),
            ..RunOptions::default()
        };
        let factory = Rc::clone(&self.factory);
        let drain = futures::stream::unfold(&self.drain, |drain| async move {
            drain.notified().await;
            Some(((), drain))
        });
        crate::run::run(factory, options, self.reporter(), drain).await
    }

    /// `workerd serve`, until the server is closed ([`close_in_process_server`]). Resolves once
    /// every listener has started: the server is serving, with whatever config errors it
    /// reported. The run's failure to start is the error.
    ///
    /// The stream sockets listen on `loopback:` names, each its own name, rather than on their
    /// addresses. The config's process-wide settings, which the host set up for the process
    /// before the factory existed, are reported as warnings. Closing it drains at once: the
    /// config's `shutdownDelayMs` gives load balancers time to stop sending connections, and the
    /// host closes a server once it wants nothing more served.
    ///
    /// The run is a task of the event loop's `LocalSet`, as `workerd serve`'s is. It cannot be one
    /// of the factory's tasks (tasks.rs): the server it owns clears those when it drops.
    pub async fn start(&self) -> Result<()> {
        if self.serving.borrow().is_some() {
            return Err(kj::failed!("the in-process server was already started"));
        }
        let socket_addresses = {
            let message = self.factory.config()?;
            let config = message.get_root::<config::Reader>().map_err(capnp_error)?;
            self.warn_process_wide(config)?;
            loopback_sockets(config)?
        };
        let (started, listening) = oneshot::channel();
        let options = RunOptions {
            socket_addresses,
            started: Some(started),
            drain_at_once: true,
            ..RunOptions::default()
        };
        let drain = futures::stream::unfold(Rc::clone(&self.drain), |drain| async move {
            drain.notified().await;
            Some(((), drain))
        });
        let run = crate::run::run(Rc::clone(&self.factory), options, self.reporter(), drain);
        let (run, mut serving) = run.remote_handle();
        // Dropping `serving` cancels the run.
        drop(kj_rs_tokio::spawn(run));
        match futures::future::select(listening, &mut serving).await {
            Either::Left((Ok(server), _)) => {
                *self.server.borrow_mut() = server;
                *self.serving.borrow_mut() = Some(serving);
                Ok(())
            }
            // The run dropped the signal unsent: it has ended, or is about to.
            Either::Left((Err(_), _)) => serving.await.and_then(|()| Err(not_started())),
            Either::Right((result, _)) => result.and_then(|()| Err(not_started())),
        }
    }

    /// Reports a warning for each of `config`'s process-wide settings, which the host's process
    /// has set already.
    fn warn_process_wide(&self, config: config::Reader<'_>) -> Result<()> {
        let warn = |message: &str| {
            // The receiver lives as long as the sender: both are the server's.
            drop(self.reports.send(ffi::ConfigReport {
                error: false,
                message: message.to_owned(),
            }));
        };
        if config.has_v8_flags() && !config.get_v8_flags().map_err(capnp_error)?.is_empty() {
            warn(
                "The config's v8Flags are ignored: V8 is set up once per process, by the program \
                 that runs this server.",
            );
        }
        if config.has_autogates() && !config.get_autogates().map_err(capnp_error)?.is_empty() {
            warn(
                "The config's autogates are ignored: autogates are set once per process, by the \
                 program that runs this server.",
            );
        }
        Ok(())
    }

    /// The channel to `entrypoint` (the default without one) of the config's service `name`,
    /// with `props` bound if given: any service, as the debug port's `getEntrypoint` resolves it.
    /// Until revoked.
    pub fn service(
        &self,
        name: &str,
        entrypoint: KjMaybe<&str>,
        props: KjMaybe<KjOwn<ffi::Frankenvalue>>,
    ) -> Result<Box<SubrequestChannel>> {
        self.revoker.check()?;
        let server = self.server_with_service(name)?;
        let channel = server.resolve_debug_entrypoint(name, entrypoint, props)?;
        let channel = self
            .revoker
            .channel(channel.0, &self.factory, Events::All)?;
        Ok(SubrequestChannel::new(channel))
    }

    /// The channel to the object of the config's service `service`'s Durable Object class
    /// `class_name` that `name` or `id` names, as a binding to the class would reach it. Until
    /// revoked.
    pub fn actor(
        &self,
        service: &str,
        class_name: &str,
        name: KjMaybe<&str>,
        id: KjMaybe<&str>,
    ) -> Result<Box<SubrequestChannel>> {
        self.revoker.check()?;
        let key = match (Option::from(name), Option::from(id)) {
            (Some(name), None) => ActorKey::Name(name),
            (None, Some(id)) => ActorKey::Id(id),
            _ => {
                return Err(kj::failed!(
                    "jsg.TypeError: A Durable Object is named by one of a name and an id."
                ));
            }
        };
        let server = self.server_with_service(service)?;
        let (namespace, id) = server.resolve_actor_key(service, class_name, key)?;
        let channel =
            self.revoker
                .channel(namespace.channel(id, false), &self.factory, Events::All)?;
        Ok(SubrequestChannel::new(channel))
    }

    /// The server, which must have a service `name`. The debug port's lookups, which `service()`
    /// and `actor()` share, say that a missing service is a missing Worker.
    fn server_with_service(&self, name: &str) -> Result<ServerHandle> {
        let handle = ServerHandle(Weak::clone(&self.server.borrow()));
        if !handle
            .server()?
            .services()
            .any(|(service, _)| service == name)
        {
            return Err(kj::failed!(
                "jsg.Error: The config has no service named \"{name}\"."
            ));
        }
        Ok(handle)
    }

    /// HTTP requests to the config's http socket `name`, as a client of the socket sends them,
    /// over the socket's `loopback:` name. Until revoked.
    pub fn socket(&self, name: &str) -> Result<Box<SubrequestChannel>> {
        self.revoker.check()?;
        let message = self.factory.config()?;
        let config = message.get_root::<config::Reader>().map_err(capnp_error)?;
        let mut found = None;
        for socket in config.get_sockets().map_err(capnp_error)? {
            if text(socket.get_name())? == name {
                found = Some(matches!(socket.which(), Ok(socket::Which::Http(_))));
                break;
            }
        }
        match found {
            Some(true) => {}
            Some(false) => {
                return Err(kj::failed!(
                    "jsg.Error: The config's socket \"{name}\" is not an http socket."
                ));
            }
            None => {
                return Err(kj::failed!(
                    "jsg.Error: The config has no socket named \"{name}\"."
                ));
            }
        }
        let mut client = capnp::message::Builder::new_default();
        let mut conf = client.init_root::<external_server::Builder>();
        conf.set_address(format!("loopback:{name}"));
        conf.reborrow().init_http();
        let channel = make_external_service(name, conf.into_reader(), None, &self.factory)?;
        let channel = self.revoker.channel(channel, &self.factory, Events::Http)?;
        Ok(SubrequestChannel::new(channel))
    }

    /// Revokes every channel [`Self::service`] and [`Self::socket`] handed out, and every request
    /// started on one. Requests in progress fail with `reason`, and so do later ones and later
    /// calls to `service()` and `socket()`. The first reason stands.
    pub fn revoke(&self, reason: &str) {
        self.revoker.revoke(reason);
    }

    /// `workerd test`.
    pub async fn test(&self, service_pattern: &str, entrypoint_pattern: &str) -> Result<bool> {
        let passed = crate::run::test(
            Rc::clone(&self.factory),
            RunOptions::default(),
            self.reporter(),
            service_pattern,
            entrypoint_pattern,
        )
        .await?;
        Ok(passed == Some(true))
    }

    /// `workerd bench`, with `params` (an encoded `BenchParams` message).
    pub async fn bench(
        &self,
        service_pattern: &str,
        entrypoint_pattern: &str,
        params: &[u64],
    ) -> Result<ffi::BenchOutcome> {
        let results = crate::run::bench(
            Rc::clone(&self.factory),
            RunOptions::default(),
            self.reporter(),
            service_pattern,
            entrypoint_pattern,
            params.to_vec(),
        )
        .await?
        .unwrap_or_else(|| bench::Report::new(0));
        Ok(ffi::BenchOutcome {
            passed: results.passed(),
            report: results.to_words(),
        })
    }

    /// The drain signal, SIGTERM in `workerd serve`. Sent before [`Self::run`], or while the
    /// server starts, it is pending once the server is up, as a SIGTERM that arrives while the
    /// process starts is.
    pub fn drain(&self) {
        self.drain.notify_one();
    }

    /// Makes this server, which runs, the first of two loop threads (threads.rs), and returns
    /// the seat of the other, which reaches this one's actors and hosts none. Once.
    pub fn add_loop_thread(&self) -> Result<Box<Seat>> {
        // This server has run alone, so the actors it has stay where they are: every actor's
        // home is this thread.
        let placement = Placement {
            count: 2,
            spread: false,
        };
        let (_, seats) = Hub::new(placement);
        let mut seats = seats.into_iter();
        let (Some(first), Some(second)) = (seats.next(), seats.next()) else {
            return Err(kj::failed!("the hub made no seat for a loop thread"));
        };
        self.factory.join(first)?;
        Ok(Box::new(second))
    }

    /// Ends this server's part in its sessions with the other loop threads, as a session that
    /// failed would: for a test of what the thread at the other end does then.
    pub fn drop_loop_thread_links(&self) {
        self.factory.close_links();
    }

    /// Seats this server among the loop threads of the server that made `seat`. Before the
    /// server runs.
    #[expect(
        clippy::boxed_local,
        reason = "the bridge hands an opaque Rust type to C++ and back as a Box"
    )]
    pub fn join_loop_threads(&self, seat: Box<Seat>) -> Result<()> {
        self.factory.join(*seat)
    }

    /// A connection to the loopback name `name`, queued for whoever listens on it.
    pub fn connect(&self, name: &str) -> Result<KjOwn<ffi::AsyncIoStream>> {
        let stream = self.factory.loopback().connect(name)?;
        Ok(kj_hyper::into_kj_stream(stream))
    }

    /// The next connection made to the loopback name `name`.
    pub async fn accept(&self, name: &str) -> Result<KjOwn<ffi::AsyncIoStream>> {
        let listener = match self.listeners.borrow_mut().entry(name.to_owned()) {
            std::collections::hash_map::Entry::Occupied(entry) => Rc::clone(entry.get()),
            std::collections::hash_map::Entry::Vacant(entry) => {
                let listener = self.factory.loopback().listen(name)?;
                Rc::clone(entry.insert(Rc::new(listener)))
            }
        };
        Ok(kj_hyper::into_kj_stream(listener.accept().await?))
    }
}

/// The `loopback:` name of each of `config`'s stream sockets, by socket name.
fn loopback_sockets(config: config::Reader<'_>) -> Result<HashMap<String, String>> {
    let mut addresses = HashMap::new();
    for socket in config.get_sockets().map_err(capnp_error)? {
        match socket.which() {
            Ok(socket::Which::Http(_) | socket::Which::Https(_) | socket::Which::Tcp(_)) => {
                let name = text(socket.get_name())?;
                addresses.insert(name.clone(), format!("loopback:{name}"));
            }
            // The loopback registry carries streams; a UDP socket keeps its address. The run
            // reports a socket of a kind it does not know.
            Ok(socket::Which::Udp(_)) | Err(_) => {}
        }
    }
    Ok(addresses)
}

/// The error of a run that ended before it started listening without failing: one whose config the
/// `Reporter` refused.
fn not_started() -> crate::Error {
    kj::failed!("the server did not start; see its config errors")
}
