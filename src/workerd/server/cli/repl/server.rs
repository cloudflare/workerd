// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The servers of the configs `workerd.loadConfig()` loads.
//!
//! Each config is served in-process by the Rust server (`InProcessServer`), as `workerd serve`
//! serves a config, on a worker factory of its own over the session's V8 and event loop, which C++
//! builds ([`ffi::repl_new_factory`]). Its stream sockets (HTTP, HTTPS, TCP) listen on in-process
//! (loopback) names instead of their addresses, so that the REPL can load configs whose ports are
//! taken, or load one config twice. Its UDP sockets bind their addresses. The process-wide parts
//! of a config, its V8 flags and autogates, are ignored with a warning: the REPL's process has its
//! own. Outbound connections reach the network, as `workerd serve`'s do.
//!
//! The channels to its services and sockets are the Rust server's (`in_process/host.rs`), which
//! rekeys headers between the REPL's header table and the factory's. Closing the server revokes
//! every channel it handed out, and every request to its workers still in progress, before the
//! server takes its factory apart, so that a JavaScript Fetcher or Response the REPL still holds
//! does not keep the factory alive.
//!
//! Reloading loads the config again and builds a new Rust server on it. The old one is revoked and
//! closed as closing would, and then the new one serves in its place: two servers of one config at
//! once would run two instances of each of its Durable Objects, and bind its UDP addresses twice.
//! A config that doesn't load, or that another server holds part of ([`Claims`]), leaves the old
//! server running. A server that doesn't start leaves nothing serving until a reload starts one.
//! Reloads run one at a time, in the order asked for. A watched config is reloaded whenever a file
//! the last load read changes, so one that failed is reloaded once it is fixed; the outcome is
//! printed to stderr.
//!
//! Loading a config refuses it if another server of the session holds what its server would,
//! until that server's close is done.
//!
//! Everything here runs on the event loop's thread. A reload or a close runs as a task of the
//! loop, so it goes on whether or not anyone awaits it, and outside the isolate lock of the
//! JavaScript that asked for it: the servers' workers lock isolates of their own.

use std::cell::Cell;
use std::cell::RefCell;
use std::fmt::Display;
use std::path::Component;
use std::path::PathBuf;
use std::rc::Rc;
use std::rc::Weak;

use capnp::message::ReaderOptions;
use cxx::KjError;
use cxx::KjExceptionType;
use frankenvalue::Frankenvalue;
use futures::FutureExt;
use futures::future::LocalBoxFuture;
use futures::future::Shared;
use kj_rs::KjOwn;
use kj_rs::KjRc;
use tokio::task::AbortHandle;
use workerd_capnp::config;
use workerd_capnp::service;
use workerd_capnp::socket;
use workerd_capnp::worker;
use workerd_server::channels::SubrequestChannel;
use workerd_server::config::capnp_error;
use workerd_server::config::text;
use workerd_server::in_process::InProcessServer;

use super::bridge::ffi;

type Result<T> = std::result::Result<T, KjError>;

/// Loads configs as encoded `Config` messages: as `workerd serve` does (session.rs), or as a test
/// does.
pub trait Loader {
    /// The config at `path`, with the constant `const_name` of a Cap'n Proto config. The error
    /// describes the problems, for the user: it becomes a JavaScript error's message.
    fn load(&self, path: &str, const_name: Option<&str>) -> std::result::Result<Vec<u64>, String>;

    /// A watch of no files yet.
    fn watch(&self) -> std::result::Result<Box<dyn Watch>, String>;
}

/// The files configs were loaded from, watched for changes.
pub trait Watch {
    /// [`Loader::load`], watching each file the load reads, whether or not it succeeds.
    fn load(&self, path: &str, const_name: Option<&str>) -> std::result::Result<Vec<u64>, String>;

    /// Resolves once a watched file has changed and changes have stopped arriving for a moment,
    /// as `workerd serve --watch` waits for them.
    fn changed(&self) -> LocalBoxFuture<'static, std::result::Result<(), String>>;
}

/// Whether a loaded config reloads whenever a file it was loaded from changes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Watching {
    Unwatched,
    Watched,
}

/// Awaited as each close of a server begins, so that a test can hold a close open.
pub type BeforeClose = Box<dyn Fn() -> LocalBoxFuture<'static, ()>>;

/// What the servers of one REPL session share, its environments' included: the loader, and what no
/// two of them may hold at once ([`Claims`]). A handle, an `Rc` inside.
#[derive(Clone)]
pub struct ServerRegistry(Rc<Session>);

struct Session {
    loader: Box<dyn Loader>,
    claims: Claims,
    before_close: Option<BeforeClose>,
}

impl ServerRegistry {
    pub fn new(loader: Box<dyn Loader>, before_close: Option<BeforeClose>) -> Self {
        Self(Rc::new(Session {
            loader,
            claims: Claims::default(),
            before_close,
        }))
    }
}

/// What no two servers of a REPL session may hold at once:
///
/// - The directory of each of their durable Durable Object namespaces. Two servers over one such
///   directory would run two instances of each of its Durable Objects on one database.
/// - The address of each of their UDP sockets that names a port. Both servers would bind it, and
///   the later would take every datagram sent to it.
#[derive(Default)]
struct Claims {
    claims: RefCell<Vec<ClaimEntry>>,
    next_id: Cell<u64>,
}

struct ClaimEntry {
    id: u64,
    /// The config of the server, as the user named it.
    path: String,
    /// Each a phrase, such as "stores Durable Objects in /storage/counter".
    held: Vec<String>,
}

/// What one server holds, until dropped.
struct Claim {
    session: Rc<Session>,
    id: u64,
}

impl Drop for Claim {
    fn drop(&mut self) {
        self.session
            .claims
            .claims
            .borrow_mut()
            .retain(|claim| claim.id != self.id);
    }
}

impl Session {
    /// Claims `held` for the server of the config `path`. A JSG error if a claim other than
    /// `except` holds one of them.
    fn claim(
        self: &Rc<Self>,
        path: &str,
        held: Vec<String>,
        except: Option<&Claim>,
    ) -> Result<Claim> {
        let claims = &self.claims;
        for other in claims.claims.borrow().iter() {
            if except.is_some_and(|skipped| skipped.id == other.id) {
                continue;
            }
            for resource in &held {
                if other.held.contains(resource) {
                    return Err(js_error(format!(
                        "{path} {resource}, as the server for {} does. Close that server first.",
                        other.path
                    )));
                }
            }
        }
        let id = claims.next_id.get();
        claims.next_id.set(id + 1);
        claims.claims.borrow_mut().push(ClaimEntry {
            id,
            path: path.to_owned(),
            held,
        });
        Ok(Claim {
            session: Rc::clone(self),
            id,
        })
    }
}

/// A config `workerd.loadConfig()` loaded, and the server that serves it. A handle, an `Rc` inside.
/// Must be closed before the last handle goes.
#[derive(Clone)]
pub struct ReplConfigServer(Rc<Inner>);

struct Inner {
    session: Rc<Session>,
    factory_source: KjRc<ffi::ReplFactorySource>,
    /// The config file, as the user named it.
    path: String,
    /// The constant that holds a Cap'n Proto config, if not the only one.
    const_name: Option<String>,
    /// Reload whenever a file the config was loaded from changes.
    watch: Watching,
    state: RefCell<State>,
}

struct State {
    /// The running server, which owns its factory. None once closed, while a reload replaces it,
    /// and after a reload whose new server did not start.
    server: Option<Box<InProcessServer>>,
    /// The warnings the server reported as it loaded the config, or last reloaded it.
    warnings: Vec<String>,
    /// A reload has closed the server and has yet to start the new one.
    replacing: bool,
    /// The last reload asked for, settled or not.
    reloading: Shared<LocalBoxFuture<'static, ()>>,
    /// The task that waits for a watched file to change, then asks for a reload. None if the
    /// config isn't watched, or once closing has begun.
    watching: Option<AbortHandle>,
    /// What the server holds, or would. None once nothing runs.
    claim: Option<Claim>,
    /// The close, once it has begun.
    closing: Option<Shared<LocalBoxFuture<'static, Outcome>>>,
    /// The close has finished, or failed.
    close_done: bool,
}

/// What a reload or a close came to, as a shared future hands it to each of its waiters.
pub(super) type Outcome = std::result::Result<(), Failure>;

/// A close under way, or done, for any number of waiters.
pub(super) type Closing = Shared<LocalBoxFuture<'static, Outcome>>;

#[derive(Clone)]
pub(super) struct Failure(Rc<KjError>);

impl Failure {
    pub(super) fn to_error(&self) -> KjError {
        KjError::new(self.0.exception_type(), self.0.description().to_owned())
    }
}

/// A server built and not yet serving, and its claim.
struct Prepared {
    server: Box<InProcessServer>,
    claim: Claim,
}

/// A started server, and the warnings it reported.
struct Started {
    server: Box<InProcessServer>,
    warnings: Vec<String>,
}

impl ReplConfigServer {
    /// Loads the config `path` names and starts serving it. Resolves once every socket listens. A
    /// JSG error listing the problems if it does not load, if it has errors, or if the server does
    /// not start; the server is then closed, and a failure to close is listed with them.
    pub async fn load(
        registry: &ServerRegistry,
        factory_source: KjRc<ffi::ReplFactorySource>,
        path: String,
        const_name: Option<String>,
        watch: Watching,
    ) -> Result<Self> {
        let session = Rc::clone(&registry.0);
        let watched = if watch == Watching::Watched {
            Some(session.loader.watch().map_err(js_error)?)
        } else {
            None
        };
        let prepared = prepare(
            &session,
            &factory_source,
            &path,
            const_name.as_deref(),
            watched.as_deref(),
            None,
        )?;
        let started = serve_or_close(&path, prepared.server).await?;
        let inner = Rc::new(Inner {
            session,
            factory_source,
            path,
            const_name,
            watch,
            state: RefCell::new(State {
                server: Some(started.server),
                warnings: started.warnings,
                replacing: false,
                reloading: futures::future::ready(()).boxed_local().shared(),
                watching: None,
                claim: Some(prepared.claim),
                closing: None,
                close_done: false,
            }),
        });
        if let Some(watched) = watched {
            inner.watch_for(watched);
        }
        Ok(Self(inner))
    }

    pub fn path(&self) -> &str {
        &self.0.path
    }

    /// Whether closing has begun.
    pub fn is_closed(&self) -> bool {
        self.0.is_closed()
    }

    /// Whether the close has finished, or failed; [`Self::is_closed`] is true from when it begins.
    pub fn is_close_done(&self) -> bool {
        self.0.state.borrow().close_done
    }

    /// How many handles hold the server, this one included.
    #[cfg(test)]
    pub fn holders(&self) -> usize {
        Rc::strong_count(&self.0)
    }

    /// Whether a change to a file the config was loaded from reloads it.
    pub fn is_watching(&self) -> bool {
        self.0.is_watching()
    }

    /// The warnings the server reported as it loaded the config, or last reloaded it.
    pub fn warnings(&self) -> Vec<String> {
        self.0.state.borrow().warnings.clone()
    }

    /// A channel to the config's service `name`: its `entrypoint` (the default if none), with
    /// `props` if given. A JSG error if there is no such service or entrypoint, or nothing serves
    /// the config.
    pub fn service(
        &self,
        name: &str,
        entrypoint: Option<&str>,
        props: Option<KjOwn<Frankenvalue>>,
    ) -> Result<Box<SubrequestChannel>> {
        self.0
            .with_open(|server| server.service(name, entrypoint.into(), props.into()))
    }

    /// A channel that sends HTTP requests to the config's socket `name`, as a client connecting
    /// to it would.
    pub fn socket(&self, name: &str) -> Result<Box<SubrequestChannel>> {
        self.0.with_open(|server| server.socket(name))
    }

    /// A channel to the Durable Object of class `class_name` that the config's service `service`
    /// exports, named by one of `name` (as `idFromName()` takes it) and `id` (as `idFromString()`
    /// does).
    pub fn durable_object(
        &self,
        service: &str,
        class_name: &str,
        name: Option<&str>,
        id: Option<&str>,
    ) -> Result<Box<SubrequestChannel>> {
        self.0
            .with_open(|server| server.actor(service, class_name, name.into(), id.into()))
    }

    /// Asks for a reload after the one asked for last: the config loaded again and served in place
    /// of what was loaded before, whose channels then fail. It fails with a JSG error listing the
    /// problems, leaving the server as it was, if the config does not load, or with one if the
    /// server is closed. Returns a future for the reload, which goes on whether or not it is
    /// awaited.
    pub fn reload(&self) -> impl Future<Output = Result<()>> + 'static + use<> {
        let reload = self.0.queue_reload(false);
        async move { reload.await.map_err(|failure| failure.to_error()) }
    }

    /// Begins the close, unless it has begun: drains the server and takes it apart. Returns a
    /// future for the close, its failure if it fails, which holds the server until the close is
    /// done; the close goes on whether or not it is awaited.
    pub fn close(&self) -> impl Future<Output = Result<()>> + 'static + use<> {
        let inner = &self.0;
        let closing = inner.state.borrow().closing.clone();
        let closing = closing.unwrap_or_else(|| {
            let closing = spawn_outcome(Rc::clone(inner).close_server());
            inner.state.borrow_mut().closing = Some(closing.clone());
            closing
        });
        let server = self.clone();
        async move {
            let outcome = closing.await.map_err(|failure| failure.to_error());
            drop(server);
            outcome
        }
    }
}

impl Inner {
    fn is_closed(&self) -> bool {
        self.state.borrow().closing.is_some()
    }

    fn is_watching(&self) -> bool {
        self.watch == Watching::Watched && !self.is_closed()
    }

    /// `call` on the running server; a JSG error saying why if nothing runs.
    fn with_open<T>(&self, call: impl FnOnce(&InProcessServer) -> Result<T>) -> Result<T> {
        let state = self.state.borrow();
        if state.closing.is_some() {
            return Err(self.closed_error());
        }
        match &state.server {
            Some(server) => call(server),
            None if state.replacing => Err(js_error(format!(
                "The server for {} is reloading.",
                self.path
            ))),
            None => Err(js_error(format!(
                "Nothing serves {}: its last reload failed to start the new server. Fix the \
                 config and reload().",
                self.path
            ))),
        }
    }

    fn closed_error(&self) -> KjError {
        js_error(format!("The server for {} was closed.", self.path))
    }

    /// Asks for a reload after the one asked for last; the reload, which fails if it does
    /// unless `report` prints its outcome instead.
    fn queue_reload(self: &Rc<Self>, report: bool) -> Shared<LocalBoxFuture<'static, Outcome>> {
        let previous = self.state.borrow().reloading.clone();
        let this = Rc::clone(self);
        let reload = spawn_outcome(async move {
            previous.await;
            this.reload(report).await
        });
        self.state.borrow_mut().reloading = reload.clone().map(drop).boxed_local().shared();
        reload
    }

    async fn reload(self: Rc<Self>, report: bool) -> Result<()> {
        if report && self.is_closed() {
            // Asked for by a change noticed just before the close.
            return Ok(());
        }
        let watched = if self.is_watching() {
            Some(self.session.loader.watch().map_err(js_error)?)
        } else {
            None
        };
        let replaced = Rc::clone(&self).replace_server(watched.as_deref()).await;
        // Whether or not the reload worked, the files it read are the ones to watch. The watch
        // starts once the reload is done, so a change waits for it rather than queueing another
        // behind it.
        if let Some(watched) = watched
            && !self.is_closed()
        {
            self.watch_for(watched);
        }

        match replaced {
            Ok(()) => {
                if report {
                    print_line(&format!(
                        "Reloaded {}: a file it was loaded from changed.",
                        self.path
                    ));
                }
                Ok(())
            }
            Err(error) => {
                let nothing_serves = self.state.borrow().server.is_none() && !self.is_closed();
                if nothing_serves {
                    // The reload closed the server it was to replace.
                    let outcome = format!(
                        "{} did not reload, and nothing serves it: {}",
                        self.path,
                        message_of(error.description())
                    );
                    if !report {
                        return Err(js_error(outcome));
                    }
                    print_line(&outcome);
                } else {
                    if !report {
                        return Err(error);
                    }
                    print_line(&format!(
                        "{} did not reload, and its server runs on: {}",
                        self.path,
                        message_of(error.description())
                    ));
                }
                Ok(())
            }
        }
    }

    /// Replaces the server with one of the config as it is now, `watched` watching the files the
    /// load reads.
    async fn replace_server(self: Rc<Self>, watched: Option<&dyn Watch>) -> Result<()> {
        if self.is_closed() {
            return Err(self.closed_error());
        }
        let next = {
            let state = self.state.borrow();
            prepare(
                &self.session,
                &self.factory_source,
                &self.path,
                self.const_name.as_deref(),
                watched,
                state.claim.as_ref(),
            )?
        };

        // The old server goes before the new one serves, and its claim stands until it has gone.
        // Closing waits for this reload, so nothing closes the old server meanwhile.
        let (previous, previous_claim) = {
            let mut state = self.state.borrow_mut();
            state.replacing = true;
            state.warnings.clear();
            let previous_claim = state.claim.replace(next.claim);
            (state.server.take(), previous_claim)
        };
        let outcome = async {
            if let Some(previous) = previous {
                let reason = format!(
                    "The server for {} was reloaded. Get a new one from its service(), socket() \
                     or durableObject().",
                    self.path
                );
                if let Some(before_close) = &self.session.before_close {
                    before_close().await;
                }
                previous.revoke(&reason);
                workerd_server::in_process::close_in_process_server(previous).await?;
            }
            drop(previous_claim);
            serve_or_close(&self.path, next.server).await
        }
        .await;

        let mut state = self.state.borrow_mut();
        state.replacing = false;
        match outcome {
            Ok(started) => {
                state.server = Some(started.server);
                state.warnings = started.warnings;
                Ok(())
            }
            Err(error) => {
                // Nothing serves unless the new server starts. It never ran, so dropping its
                // claim ends nothing.
                state.claim = None;
                Err(error)
            }
        }
    }

    /// Asks for a reload once a file `watched` watches changes.
    fn watch_for(self: &Rc<Self>, watched: Box<dyn Watch>) {
        let changed = watched.changed();
        let this = Rc::downgrade(self);
        let path = self.path.clone();
        let task = kj_rs_tokio::spawn(async move {
            let outcome = changed.await;
            drop(watched);
            match outcome {
                // The reload replaces this task, once this has returned: `reloading` holds the
                // reload.
                Ok(()) => {
                    if let Some(this) = Weak::upgrade(&this) {
                        drop(this.queue_reload(true));
                    }
                }
                Err(error) => print_line(&format!(
                    "Stopped watching the files of {path}: {}",
                    message_of(&error)
                )),
            }
        });
        self.state.borrow_mut().watching = Some(task.abort_handle());
    }

    async fn close_server(self: Rc<Self>) -> Result<()> {
        let outcome = self.close_and_revoke().await;
        let mut state = self.state.borrow_mut();
        state.claim = None;
        state.close_done = true;
        outcome
    }

    async fn close_and_revoke(&self) -> Result<()> {
        if let Some(before_close) = &self.session.before_close {
            before_close().await;
        }
        let (watching, reloading) = {
            let mut state = self.state.borrow_mut();
            (state.watching.take(), state.reloading.clone())
        };
        if let Some(watching) = watching {
            watching.abort();
        }
        // A reload under way finishes, and its server is the one closed. No reload starts after
        // this one: they refuse once closing has begun.
        reloading.await;
        let server = self.state.borrow_mut().server.take();
        if let Some(server) = server {
            // What the REPL holds of the server goes first: anything that still holds a request
            // or a channel would keep the factory alive.
            server.revoke(&format!("The server for {} was closed.", self.path));
            workerd_server::in_process::close_in_process_server(server).await?;
        }
        Ok(())
    }
}

impl Drop for Inner {
    fn drop(&mut self) {
        if self.state.get_mut().server.is_some() {
            eprintln!(
                "error: the REPL server for {} was dropped without being closed",
                self.path
            );
        }
    }
}

/// Runs `future` as a task of the event loop, so that it goes on whether or not anyone awaits
/// it; what it comes to, for any number of waiters.
pub(super) fn spawn_outcome(
    future: impl Future<Output = Result<()>> + 'static,
) -> Shared<LocalBoxFuture<'static, Outcome>> {
    let task = kj_rs_tokio::spawn(future);
    async move {
        match task.await {
            Ok(outcome) => outcome.map_err(|error| Failure(Rc::new(error))),
            Err(error) => Err(Failure(Rc::new(KjError::new(
                KjExceptionType::Failed,
                format!("the REPL server's task ended: {error}"),
            )))),
        }
    }
    .boxed_local()
    .shared()
}

/// Loads the config, with `watched` watching the files the load reads, claims what a server of it
/// would hold, `except` aside (the claim of a server it is to replace), and builds that server. A
/// JSG error if the config does not load, or another server holds what it would.
fn prepare(
    session: &Rc<Session>,
    factory_source: &ffi::ReplFactorySource,
    path: &str,
    const_name: Option<&str>,
    watched: Option<&dyn Watch>,
    except: Option<&Claim>,
) -> Result<Prepared> {
    let words = match watched {
        Some(watched) => watched.load(path, const_name),
        None => session.loader.load(path, const_name),
    }
    .map_err(js_error)?;
    let claim = session.claim(path, exclusive_resources(&words)?, except)?;
    // The server owns the factory for as long as it runs.
    let factory = ffi::repl_new_factory(factory_source, &words)?;
    Ok(Prepared {
        server: workerd_server::in_process::new_in_process_server(factory),
        claim,
    })
}

/// Serves `server`, the config `path`'s, and hands it back with the warnings it reported. A JSG
/// error listing the problems if it doesn't start, having closed it.
async fn serve_or_close(path: &str, server: Box<InProcessServer>) -> Result<Started> {
    let failure = server.start().await.err();
    let mut errors = Vec::new();
    let mut warnings = Vec::new();
    for report in server.take_reports() {
        if report.error {
            errors.push(report.message);
        } else {
            warnings.push(report.message);
        }
    }
    if let Some(failure) = failure
        && errors.is_empty()
    {
        errors.push(failure.description().to_owned());
    }
    if errors.is_empty() {
        return Ok(Started { server, warnings });
    }
    // The config's errors are why the load failed, so a failure to close joins them rather than
    // taking their place.
    if let Err(error) = workerd_server::in_process::close_in_process_server(server).await {
        errors.push(format!(
            "Closing the server then failed: {}",
            error.description()
        ));
    }
    Err(js_error(format!(
        "Failed to load {path}:\n  {}",
        errors.join("\n  ")
    )))
}

/// What a server of the config `words` would hold that no other server may ([`Claims`]):
///
/// - The directory of each durable Durable Object namespace, as the server finds it: the path of
///   the worker's `localDisk` service from the working directory, then the namespace's
///   `uniqueKey`.
/// - The address of each UDP socket that names a port, as the config writes it.
fn exclusive_resources(words: &[u64]) -> Result<Vec<String>> {
    let bytes: Vec<u8> = words.iter().flat_map(|word| word.to_le_bytes()).collect();
    let message = capnp::serialize::read_message(
        bytes.as_slice(),
        *ReaderOptions::new().traversal_limit_in_words(None),
    )
    .map_err(capnp_error)?;
    let config = message.get_root::<config::Reader>().map_err(capnp_error)?;
    let services = config.get_services().map_err(capnp_error)?;
    let disk_path = |name: &str| -> Result<Option<String>> {
        for service in services {
            if text(service.get_name())? == name
                && let Ok(service::Which::Disk(disk)) = service.which()
            {
                let disk = disk.map_err(capnp_error)?;
                if disk.has_path() {
                    return Ok(Some(text(disk.get_path())?));
                }
            }
        }
        Ok(None)
    };

    let mut held = Vec::new();
    for service in services {
        let Ok(service::Which::Worker(worker)) = service.which() else {
            continue;
        };
        let worker = worker.map_err(capnp_error)?;
        let Ok(worker::durable_object_storage::Which::LocalDisk(disk)) =
            worker.get_durable_object_storage().which()
        else {
            continue;
        };
        let Some(path) = disk_path(&text(disk)?)? else {
            continue;
        };
        let Some(directory) = from_working_directory(&path) else {
            continue;
        };
        for namespace in worker
            .get_durable_object_namespaces()
            .map_err(capnp_error)?
        {
            if let Ok(worker::durable_object_namespace::Which::UniqueKey(key)) = namespace.which() {
                held.push(format!(
                    "stores Durable Objects in {directory}/{}",
                    text(key)?
                ));
            }
        }
    }
    for socket in config.get_sockets().map_err(capnp_error)? {
        if let Ok(socket::Which::Udp(_)) = socket.which()
            && socket.has_address()
        {
            let address = text(socket.get_address())?;
            if names_port(&address) {
                held.push(format!("listens for UDP datagrams on {address}"));
            }
        }
    }
    Ok(held)
}

/// `path`, a native path, from the working directory, with `.` and `..` resolved as the
/// path's text says rather than through symbolic links; none if there is no working directory,
/// which the server reports.
fn from_working_directory(path: &str) -> Option<String> {
    let joined = std::env::current_dir().ok()?.join(path);
    let mut resolved = PathBuf::new();
    for component in joined.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                resolved.pop();
            }
            other => resolved.push(other),
        }
    }
    Some(resolved.display().to_string())
}

/// Whether the UDP socket address `address` names a port, rather than leaving the system to choose
/// one each time it is bound: whether it ends in a port other than 0, after a host, or after an
/// IPv6 address in brackets. A bare IPv6 address has colons of its own.
fn names_port(address: &str) -> bool {
    let mut host = address;
    if address.starts_with('[')
        && let Some(end) = address.find(']')
    {
        host = &address[end + 1..];
        if !host.starts_with(':') {
            return false;
        }
    }
    match host.rfind(':') {
        Some(colon) if !host[..colon].contains(':') => {
            // Port 0, however it is written, leaves the choice to the system.
            host[colon + 1..].chars().any(|c| c != '0')
        }
        _ => false,
    }
}

/// A JSG `Error` with `message`, as a KJ exception tunnels it to JavaScript.
pub(super) fn js_error(message: impl Display) -> KjError {
    KjError::new(KjExceptionType::Failed, format!("jsg.Error: {message}"))
}

/// A tunneled JSG error's message without its prefixes or its type, for the console and for
/// errors that report it: `jsg.Error: text`, and `expected ...; jsg.Error: text`, are "text".
/// Any other description is its own message.
fn message_of(description: &str) -> &str {
    let mut message = description;
    while let Some(rest) = message.strip_prefix("remote.") {
        message = rest;
    }
    let tunneled = message
        .find("; jsg")
        .map_or(message, |delimiter| &message[delimiter + 2..]);
    let Some(error) = tunneled
        .strip_prefix("jsg.")
        .or_else(|| tunneled.strip_prefix("jsg-internal."))
    else {
        return message;
    };
    // As JSG reads it: the type, then ": ".
    match error.split_once(':') {
        Some((_, text)) => text.trim_start_matches(' '),
        None => error,
    }
}

/// Writes a line to stderr, where the console writes its own messages.
fn print_line(line: &str) {
    eprintln!("{line}");
}

#[cfg(test)]
#[path = "server-test.rs"]
mod tests;
