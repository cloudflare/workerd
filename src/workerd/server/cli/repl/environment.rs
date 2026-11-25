// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The JavaScript environments `workerd repl` evaluates input in.
//!
//! An environment is a worker that never receives an event (C++ `ReplWorker`, repl/repl-worker.h),
//! with one request open on it for as long as the environment lives: every input runs in that
//! request's `IoContext`, so that what an input starts, such as a timer, a fetch or a
//! `waitUntil()` task, goes on after it. The first input to run gives the global scope `workerd`
//! (api.rs), through which the user loads configs and starts other environments, and `_` and
//! `_error`, as Node's REPL has them.
//!
//! The environment holds what the `workerd` global loaded and started, and closes it when it
//! drains. An environment that `workerd.environment()` started is [`Nested`] in the one that
//! started it: what fails in its background counts as failing in its parent's, and its parent's
//! drain closes it.
//!
//! Everything here runs on the event loop's thread.

use std::cell::Cell;
use std::cell::RefCell;
use std::pin::Pin;
use std::rc::Rc;
use std::rc::Weak;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::task::Poll;
use std::task::Waker;

use compat_date::CompatDate;
use cxx::KjError;
use cxx::KjExceptionType;
use futures::FutureExt;
use futures::future::LocalBoxFuture;
use io_context::IoContext;
use jsg::ToJS;
use jsg::v8;
use kj_rs::KjOwn;
use kj_rs::KjRc;
use tokio::sync::Notify;

use super::api;
use super::bridge;
use super::bridge::ffi;
use super::input;
use super::input::Candidate;
use super::server;
use super::server::ReplConfigServer;
use super::server::ServerRegistry;
use super::server::Watching;

/// Evaluates to a function that, given `report`, reports promises that user code rejected and
/// nothing handled, such as `Promise.reject(1);`, calling `report` for each. The REPL handles the
/// promises it awaits itself.
const STARTUP_SCRIPT: &str = r"(report) => {
  addEventListener('unhandledrejection', (event) => {
    report();
    console.error('Uncaught (in promise)', event.reason);
  });
}";

/// Defines `_` and `_error` on the global scope, as Node's REPL does: the last input's result and
/// its last exception. Assigning one keeps the assigned value from then on. Evaluates to the
/// function the REPL records each outcome with, which user code can't reach.
const OUTCOMES_SCRIPT: &str = r"(() => {
  const slots = {
    _: { value: undefined, assigned: false },
    _error: { value: undefined, assigned: false },
  };
  for (const name of ['_', '_error']) {
    const slot = slots[name];
    Object.defineProperty(globalThis, name, {
      configurable: true,
      get() { return slot.value; },
      set(value) {
        if (!slot.assigned) console.warn(`Expression assignment to ${name} now disabled.`);
        slot.assigned = true;
        slot.value = value;
      },
    });
  }
  return (name, value) => {
    if (!slots[name].assigned) slots[name].value = value;
  };
})()";

/// The settings of an environment's JavaScript.
pub struct Options {
    /// Today's if none. Like `workerd test --compat-date`, any valid date is accepted, such as
    /// 2999-12-31 for every flag's behavior.
    pub compat_date: Option<String>,
    /// Added to the date's flags, with those [`compatibility_flags`] adds.
    pub compat_flags: Vec<String>,
    /// `--experimental`: lets the compatibility flags and the loaded configs use experimental
    /// features, and adds `service_binding_extra_handlers` to the flags.
    pub experimental: bool,
    /// Render results with ANSI colors.
    pub colors: bool,
}

/// What became of an input.
pub struct Evaluation {
    /// The text to print: the rendered result, or "Uncaught " followed by the rendered exception.
    pub text: String,
    /// The input threw, or the promise of an awaited candidate rejected, so `text` renders an
    /// exception.
    pub threw: bool,
}

/// A REPL session: the I/O and V8 that C++ sets up, and the environment input typed at the
/// console is evaluated in.
pub struct Session {
    environment: Environment,
    // Outlives the environment, and everything that started there: declared after it.
    _io: KjOwn<ffi::ReplSession>,
}

#[cfg(test)]
impl Session {
    /// Runs `function` in the session's environment, as an input runs.
    pub(super) fn run<T, Fut, F>(
        &self,
        function: F,
    ) -> Result<io_context::ReentryCallFuture<T>, KjError>
    where
        T: 'static,
        Fut: Future<Output = Result<T, cxx::KjException>> + 'static,
        F: for<'t> FnOnce(&mut jsg::Lock<'t>, &IoContext<'t>) -> jsg::Result<Fut> + 'static,
    {
        self.environment.run(function)
    }

    /// What the session's environment shares with its `workerd` global.
    pub(super) fn state(&self) -> Rc<EnvironmentState> {
        Rc::clone(&self.environment.state)
    }
}

impl Session {
    /// A session on the event loop `event_loop`, which it must not outlive, whose environments
    /// load configs through `registry`. An error if the process's settings or the environment's
    /// compatibility settings are not valid.
    pub fn new(
        event_loop: Pin<&mut ffi::TokioAsyncIoContext>,
        process: &ffi::ReplProcessOptions,
        options: Options,
        registry: ServerRegistry,
    ) -> Result<Self, KjError> {
        let mut io = ffi::repl_session_new(event_loop, process)?;
        let resources = ffi::repl_session_resources(io.as_mut());
        let environment = Environment::new(resources, registry, options)?;
        Ok(Self {
            environment,
            _io: io,
        })
    }

    /// [`Environment::evaluate`] in the session's environment.
    pub async fn evaluate(&self, candidates: Vec<Candidate>) -> Result<Evaluation, KjError> {
        self.environment.evaluate(candidates).await
    }

    /// [`Environment::property_names`] in the session's environment.
    pub async fn property_names(&self, base: String) -> Result<Vec<String>, KjError> {
        self.environment.property_names(base).await
    }

    /// Drains the session's environment ([`Environment::drain`]), then ends the session; whether
    /// anything failed in the background.
    pub async fn drain(self) -> Result<bool, KjError> {
        self.environment.drain().await
    }
}

/// An environment REPL input is evaluated in.
pub struct Environment {
    // Dropped undrained, released before the request, while its IoContext still exists. The drain
    // releases it once the request has ended, when the IoContext has already let go of it.
    waiting_for_input: RefCell<Option<KjOwn<io_context::PendingEvent>>>,
    // The environment's request, which owns its IoContext. None once drained.
    request: RefCell<Option<KjOwn<io_context::IncomingRequest>>>,
    state: Rc<EnvironmentState>,
    // Outlives the request, its IoContext and what that holds: declared after them.
    _worker: KjOwn<ffi::ReplWorker>,
}

/// What an environment shares with its `workerd` global (api.rs).
pub struct EnvironmentState {
    /// Loads the configs, here and in the environments this one starts.
    registry: ServerRegistry,
    /// What the environments this one starts are built from.
    resources: KjRc<ffi::ReplResources>,
    /// What the servers of the configs loaded here are built from.
    factory_source: KjRc<ffi::ReplFactorySource>,
    experimental: bool,
    colors: bool,
    /// The name input is compiled under: the URL of the worker's main module, so that a dynamic
    /// `import()` typed into the REPL resolves as one in the main module would.
    input_name: String,
    /// The global scope has `workerd`, `_` and `_error`.
    installed: Cell<bool>,
    /// Sets `_` or `_error` (its first argument) to an input's outcome (its second); see
    /// [`OUTCOMES_SCRIPT`]. None until installed.
    record_outcome: RefCell<Option<v8::Global<v8::Function>>>,
    /// Whether something failed in the background: see [`Environment::drain`].
    failed: BackgroundFailure,
    /// The loads and environment starts under way, which add to `servers` and `environments`
    /// when they finish. The drain waits for them, and none starts once it has begun.
    starts: Cell<u32>,
    drain_started: Cell<bool>,
    /// Notified once the drain has begun and no start is under way.
    starts_settled: Notify,
    /// The servers of the configs loaded and not yet closed.
    servers: RefCell<Vec<ReplConfigServer>>,
    /// The environments started and not yet closed.
    environments: RefCell<Vec<Rc<Nested>>>,
}

/// Whether something failed in an environment's background, which the JavaScript reporting a
/// rejection nothing handled sets from the function [`STARTUP_SCRIPT`] is given.
#[derive(Clone, Default)]
struct BackgroundFailure(Arc<AtomicBool>);

jsg::jsg_traced_unchecked!(BackgroundFailure);

impl BackgroundFailure {
    fn set(&self) {
        self.0.store(true, Ordering::Relaxed);
    }

    fn get(&self) -> bool {
        self.0.load(Ordering::Relaxed)
    }
}

impl Environment {
    /// An environment on the session's `resources`, whose configs load through `registry`. An
    /// error, naming the problems, if the compatibility settings are not valid.
    pub fn new(
        resources: KjRc<ffi::ReplResources>,
        registry: ServerRegistry,
        options: Options,
    ) -> Result<Self, KjError> {
        let compat_date = options
            .compat_date
            .unwrap_or_else(|| CompatDate::today().to_string());
        let mut worker = ffi::repl_worker_new(
            &resources,
            &ffi::ReplWorkerOptions {
                compat_date,
                compat_flags: compatibility_flags(&options.compat_flags, options.experimental),
                experimental: options.experimental,
            },
        )?;
        let mut request = ffi::repl_worker_new_request(worker.as_mut());
        io_context::incoming_request_delivered(
            request.as_mut(),
            "repl",
            io_context::delivered_location!(),
        )?;
        // The event the IoContext waits for between inputs: the next line of input. Without it, a
        // context with nothing pending is taken to be hung when the thread goes idle.
        let waiting_for_input =
            io_context::incoming_request_register_pending_event(request.as_mut())?;
        let state = Rc::new(EnvironmentState {
            registry,
            resources,
            factory_source: ffi::repl_worker_factory_source(worker.as_mut()),
            experimental: options.experimental,
            colors: options.colors,
            input_name: ffi::repl_worker_input_name(&worker).to_owned(),
            installed: Cell::new(false),
            record_outcome: RefCell::new(None),
            failed: BackgroundFailure::default(),
            starts: Cell::new(0),
            drain_started: Cell::new(false),
            starts_settled: Notify::new(),
            servers: RefCell::new(Vec::new()),
            environments: RefCell::new(Vec::new()),
        });
        Ok(Self {
            waiting_for_input: RefCell::new(Some(waiting_for_input)),
            request: RefCell::new(Some(request)),
            state,
            _worker: worker,
        })
    }

    /// Evaluates one input: the first of `candidates` that compiles ([`compile`]). A promise result
    /// is rendered as a promise unless the candidate is `awaited`; the event loop runs while one is
    /// awaited. An error only if the environment itself fails.
    pub async fn evaluate(&self, candidates: Vec<Candidate>) -> Result<Evaluation, KjError> {
        let state = Rc::clone(&self.state);
        let ran = self
            .run(move |lock, context| {
                let ran = run_input(lock, context, &state, &candidates)?;
                Ok(async move {
                    match ran {
                        Ran::Done(evaluation) => Ok(Ran::Done(evaluation)),
                        // The event loop runs meanwhile: timers fire, servers serve, output flows.
                        Ran::Awaiting { result, settled } => {
                            settled.await?;
                            Ok(Ran::Settled(result))
                        }
                        Ran::Settled(result) => Ok(Ran::Settled(result)),
                    }
                })
            })?
            .await?;
        let result = match ran {
            Ran::Done(evaluation) => return Ok(evaluation),
            Ran::Settled(result) | Ran::Awaiting { result, .. } => result,
        };
        let state = Rc::clone(&self.state);
        Ok(self
            .run(move |lock, _context| {
                let promise = result.as_local(lock);
                let value = promise.result(lock).unwrap_or_else(|| lock.undefined());
                // The input awaited the promise, so a rejection is thrown, as at the top level of
                // a module.
                let evaluation = if promise.state() == v8::PromiseState::Rejected {
                    state.record(lock, "_error", value)?;
                    Evaluation {
                        text: format!("Uncaught {}", describe_exception(lock, value)?),
                        threw: true,
                    }
                } else {
                    state.record(lock, "_", value)?;
                    Evaluation {
                        text: render(lock, value, state.colors)?,
                        threw: false,
                    }
                };
                Ok(std::future::ready(Ok(evaluation)))
            })?
            .await?)
    }

    /// The string-named properties, own and inherited, of the value of `base` (an identifier chain
    /// such as `a.b.c`), or of the global object if `base` is empty, for tab completion. Evaluating
    /// `base` may run property getters, as completion in Node's REPL does. Empty if `base` is not
    /// an identifier chain, its evaluation throws, or its value is null or undefined.
    pub async fn property_names(&self, base: String) -> Result<Vec<String>, KjError> {
        if !base.is_empty() && !is_identifier_chain(&base) {
            return Ok(Vec::new());
        }
        let state = Rc::clone(&self.state);
        Ok(self
            .run(move |lock, _context| {
                let names = lock.try_catch(|lock| {
                    let target = if base.is_empty() {
                        lock.global_object()
                    } else {
                        let source = js_string(lock, &format!("({base})"))?;
                        let name = js_string(lock, &state.input_name)?;
                        let value = v8::Script::compile(lock, source, name)?.run(lock)?;
                        if value.is_null_or_undefined() {
                            return Ok(Vec::new());
                        }
                        value.to_object(lock)?
                    };
                    let keys = target.property_names_with_prototypes(lock)?;
                    Ok(keys.into_iter().map(|key| key.to_string(lock)).collect())
                })?;
                // `base` does not evaluate (an undefined name, a throwing getter): nothing to
                // complete.
                Ok(std::future::ready(Ok(names.unwrap_or_default())))
            })?
            .await?)
    }

    /// Waits for the loads and environment starts under way, refusing new ones, then closes every
    /// loaded config's server and every environment this one started, then ends the request once
    /// its `waitUntil()` tasks are done. Must complete before the environment is dropped; nothing
    /// else may be called afterwards. Fails with the first failure to close, if any, once all of
    /// that is done. Otherwise, resolves to whether anything failed in the background, here or in
    /// an environment this one started: a timer callback that threw, a `waitUntil()` task that
    /// rejected, or a rejection nothing handled.
    pub async fn drain(&self) -> Result<bool, KjError> {
        let state = &self.state;
        // A start under way, such as a loadConfig() that wasn't awaited, adds what it started when
        // it finishes. Canceling one could drop a server that is running but not closed.
        state.drain_started.set(true);
        if state.starts.get() > 0 {
            state.starts_settled.notified().await;
        }

        // What fails to close does not keep the rest open or the request undrained.
        let mut failure = None;
        let servers = std::mem::take(&mut *state.servers.borrow_mut());
        for server in servers {
            if let Err(error) = server.close().await {
                failure.get_or_insert(error);
            }
        }
        let environments = std::mem::take(&mut *state.environments.borrow_mut());
        for environment in environments {
            if let Err(error) = environment.close().await {
                failure.get_or_insert(error);
            }
        }

        let request = self.request.borrow_mut().take();
        if let Some(request) = request {
            // A timer callback that threw counts, as a waitUntil() task that rejected does.
            if !io_context::incoming_request_drain(request).await? {
                state.failed.set();
            }
        }
        self.waiting_for_input.borrow_mut().take();

        match failure {
            Some(error) => Err(error),
            None => Ok(state.failed.get()),
        }
    }

    /// Runs `function` in the environment's request, having given the global scope what the REPL
    /// puts there if no input has yet.
    fn run<T, Fut, F>(&self, function: F) -> Result<io_context::ReentryCallFuture<T>, KjError>
    where
        T: 'static,
        Fut: Future<Output = Result<T, cxx::KjException>> + 'static,
        F: for<'t> FnOnce(&mut jsg::Lock<'t>, &IoContext<'t>) -> jsg::Result<Fut> + 'static,
    {
        let mut request = self.request.borrow_mut();
        let Some(request) = request.as_mut() else {
            return Err(KjError::new(
                KjExceptionType::Failed,
                "the REPL session has ended".to_owned(),
            ));
        };
        let state = Rc::clone(&self.state);
        Ok(io_context::incoming_request_run(
            request.as_mut(),
            move |lock, context| {
                // Set first: an install that fails part way, as on termination, is not run again,
                // which would add a second listener for unhandled rejections.
                if !state.installed.replace(true) {
                    state.install(lock, context)?;
                }
                function(lock, context)
            },
        ))
    }
}

impl EnvironmentState {
    /// Gives the global scope `workerd`, and `_` and `_error`, and reports the rejections nothing
    /// handles.
    fn install(
        self: &Rc<Self>,
        lock: &mut jsg::Lock<'_>,
        context: &IoContext<'_>,
    ) -> jsg::Result<()> {
        let control = api::ReplControl::new(context, Rc::clone(self))?;
        let control = jsg::Rc::new(lock, control).to_js(lock)?;
        lock.global_object().set(lock, "workerd", control)?;

        let startup = run_script(lock, STARTUP_SCRIPT, "repl-startup.js")?;
        let report = v8::Function::new(lock, self.failed.clone(), |lock, failed, _args| {
            failed.set();
            Ok(lock.undefined())
        })?;
        startup.call(lock, None, &[report.into()])?;

        let record = run_script(lock, OUTCOMES_SCRIPT, "repl-outcomes.js")?;
        *self.record_outcome.borrow_mut() = Some(record.to_global(lock));
        Ok(())
    }

    /// Records `value` as the last result (`name` is "_") or exception ("_error").
    fn record<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        name: &str,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<()> {
        let recorder = self
            .record_outcome
            .borrow()
            .as_ref()
            .map(|recorder| recorder.as_local(lock));
        if let Some(recorder) = recorder {
            // The recorder is the REPL's own, and does not throw.
            let name = js_string(lock, name)?;
            recorder.call(lock, None, &[name.into(), value])?;
        }
        Ok(())
    }

    /// Counts a load or environment start as under way until the result is dropped. A JSG error
    /// once the drain has begun.
    fn begin_start(self: &Rc<Self>) -> Result<Start, KjError> {
        if self.drain_started.get() {
            return Err(server::js_error("The environment is closing."));
        }
        self.starts.set(self.starts.get() + 1);
        Ok(Start(Rc::clone(self)))
    }

    /// Loads the config `path` names and starts serving it, with the constant `const_name` of a
    /// Cap'n Proto config, as `workerd.loadConfig()` asks. A JSG error, listing the problems, if
    /// the config does not load or has errors.
    pub fn load_config(
        self: &Rc<Self>,
        path: String,
        const_name: Option<String>,
        watch: Watching,
    ) -> impl Future<Output = Result<ReplConfigServer, KjError>> + 'static + use<> {
        let start = self.begin_start();
        let state = Rc::clone(self);
        async move {
            let _start = start?;
            // JavaScript asked for the load under the REPL's isolate lock; the server's workers
            // lock isolates of their own, so the load runs on the event loop.
            yield_now().await;
            let server = ReplConfigServer::load(
                &state.registry,
                state.factory_source.clone(),
                path,
                const_name,
                watch,
            )
            .await?;
            // Drop the servers that finished closing since the last load. One still closing
            // stays, so that the drain waits for it.
            let mut servers = state.servers.borrow_mut();
            servers.retain(|server| !server.is_close_done());
            servers.push(server.clone());
            Ok(server)
        }
    }

    /// Starts an environment like this one, in an isolate of its own, with the compatibility
    /// settings `workerd.environment()` gives rather than this one's: today's date if none. A JSG
    /// error, naming the problems, if the settings are not valid.
    pub fn start_environment(
        self: &Rc<Self>,
        compat_date: Option<String>,
        compat_flags: Vec<String>,
    ) -> impl Future<Output = Result<Rc<Nested>, KjError>> + 'static + use<> {
        let start = self.begin_start();
        let state = Rc::clone(self);
        async move {
            let _start = start?;
            // JavaScript asked for it under the REPL's isolate lock; starting an isolate locks it.
            yield_now().await;
            let compat_date = compat_date.unwrap_or_else(|| CompatDate::today().to_string());
            let environment = Environment::new(
                state.resources.clone(),
                state.registry.clone(),
                Options {
                    compat_date: Some(compat_date.clone()),
                    compat_flags: compat_flags.clone(),
                    experimental: state.experimental,
                    // The rendering is a string in the REPL that started it, where escapes would
                    // show.
                    colors: false,
                },
            )
            // The settings' problems, which are the user's.
            .map_err(|error| server::js_error(error.description()))?;
            let nested = Rc::new(Nested {
                environment: RefCell::new(Some(Rc::new(environment))),
                compat_date,
                compat_flags,
                parent_failed: state.failed.clone(),
                closing: RefCell::new(None),
                close_done: Rc::new(Cell::new(false)),
                canceler: Canceler::default(),
            });
            // Drop the environments that finished closing since the last one started, as
            // load_config() drops servers.
            let mut environments = state.environments.borrow_mut();
            environments.retain(|environment| !environment.is_close_done());
            environments.push(Rc::clone(&nested));
            Ok(nested)
        }
    }
}

/// A load or an environment start under way; see [`EnvironmentState::begin_start`].
struct Start(Rc<EnvironmentState>);

impl Drop for Start {
    fn drop(&mut self) {
        let state = &self.0;
        state.starts.set(state.starts.get() - 1);
        if state.starts.get() == 0 && state.drain_started.get() {
            state.starts_settled.notify_one();
        }
    }
}

/// An environment another started with compatibility settings of its own. What fails in its
/// background counts as failing in its parent's. The parent's drain waits for its close.
pub struct Nested {
    /// None once closing has begun.
    environment: RefCell<Option<Rc<Environment>>>,
    compat_date: String,
    compat_flags: Vec<String>,
    parent_failed: BackgroundFailure,
    /// The close, once it has begun.
    closing: RefCell<Option<server::Closing>>,
    /// The close has finished, or failed; set by the close task, which holds it.
    close_done: Rc<Cell<bool>>,
    /// Cuts short the evaluations under way when the close begins.
    canceler: Canceler,
}

impl Nested {
    /// The compatibility date the environment has, today's if none was given.
    pub fn compat_date(&self) -> &str {
        &self.compat_date
    }

    /// The compatibility flags it was given.
    pub fn compat_flags(&self) -> &[String] {
        &self.compat_flags
    }

    pub fn is_closed(&self) -> bool {
        self.closing.borrow().is_some()
    }

    fn is_close_done(&self) -> bool {
        self.close_done.get()
    }

    /// Evaluates `input` there as the REPL evaluates a line typed at it: a future for what the
    /// REPL would print, the rendered result or "Uncaught " and the exception. A JSG error if the
    /// environment is closed; the future fails with one if it closes meanwhile.
    pub fn evaluate(
        &self,
        input: &str,
    ) -> Result<impl Future<Output = Result<String, KjError>> + 'static + use<>, KjError> {
        let environment = self
            .environment
            .borrow()
            .clone()
            .filter(|_| !self.is_closed())
            .ok_or_else(closed_error)?;
        let candidates = input::candidates(input);
        Ok(self.canceler.wrap(
            async move {
                // Asked for under the REPL's isolate lock; the input runs under the environment's
                // own.
                yield_now().await;
                // Only the rendering crosses between environments.
                Ok(environment.evaluate(candidates).await?.text)
            }
            .boxed_local(),
        ))
    }

    /// Closes the environment's servers and ends its JavaScript, unless the close has begun; a
    /// future for the close, which holds the environment until it is done. The close goes on
    /// whether or not the future is awaited.
    pub fn close(self: &Rc<Self>) -> impl Future<Output = Result<(), KjError>> + 'static + use<> {
        let closing = self.closing.borrow().clone();
        let closing = closing.unwrap_or_else(|| {
            let closing = server::spawn_outcome(Rc::clone(self).close_environment());
            *self.closing.borrow_mut() = Some(closing.clone());
            closing
        });
        let nested = Rc::clone(self);
        async move {
            let outcome = closing.await.map_err(|failure| failure.to_error());
            drop(nested);
            outcome
        }
    }

    async fn close_environment(self: Rc<Self>) -> Result<(), KjError> {
        let close_done = Rc::clone(&self.close_done);
        let _done = Defer(Some(move || close_done.set(true)));
        // Asked for under the REPL's isolate lock; the environment's isolate is torn down without
        // it.
        yield_now().await;
        self.canceler.cancel();
        let environment = self.environment.borrow_mut().take();
        if let Some(environment) = environment
            && environment.drain().await?
        {
            self.parent_failed.set();
        }
        Ok(())
    }
}

/// The error of an evaluation in a closed environment.
fn closed_error() -> KjError {
    server::js_error("The environment was closed.")
}

/// Runs a closure when dropped.
struct Defer<F: FnOnce()>(Option<F>);

impl<F: FnOnce()> Drop for Defer<F> {
    fn drop(&mut self) {
        if let Some(f) = self.0.take() {
            f();
        }
    }
}

/// Cuts short the futures it wraps, as C++ `kj::Canceler` does: [`cancel`](Self::cancel) drops
/// each at once, with what it holds, and it then fails with [`closed_error`].
#[derive(Default)]
struct Canceler {
    slots: RefCell<Vec<Weak<CancelSlot>>>,
}

type Cancelable = LocalBoxFuture<'static, Result<String, KjError>>;

/// A future a [`Canceler`] wraps: None once canceled or done.
struct CancelSlot {
    future: RefCell<Option<Cancelable>>,
    waker: RefCell<Option<Waker>>,
}

impl Canceler {
    fn wrap(
        &self,
        future: Cancelable,
    ) -> impl Future<Output = Result<String, KjError>> + 'static + use<> {
        let slot = Rc::new(CancelSlot {
            future: RefCell::new(Some(future)),
            waker: RefCell::new(None),
        });
        let mut slots = self.slots.borrow_mut();
        slots.retain(|slot| slot.strong_count() > 0);
        slots.push(Rc::downgrade(&slot));
        std::future::poll_fn(move |cx| {
            let mut future = slot.future.borrow_mut();
            let Some(running) = future.as_mut() else {
                return Poll::Ready(Err(closed_error()));
            };
            match running.as_mut().poll(cx) {
                Poll::Ready(output) => {
                    *future = None;
                    Poll::Ready(output)
                }
                Poll::Pending => {
                    *slot.waker.borrow_mut() = Some(cx.waker().clone());
                    Poll::Pending
                }
            }
        })
    }

    fn cancel(&self) {
        let slots = std::mem::take(&mut *self.slots.borrow_mut());
        for slot in slots.iter().filter_map(Weak::upgrade) {
            // Dropped out of the cell, which a future's drop could reach.
            let canceled = slot.future.borrow_mut().take();
            drop(canceled);
            if let Some(waker) = slot.waker.borrow_mut().take() {
                waker.wake();
            }
        }
    }
}

/// What running an input came to, before any promise it awaits settles.
enum Ran {
    Done(Evaluation),
    /// The promise of an awaited candidate, and when it settles.
    Awaiting {
        result: v8::Global<v8::Promise>,
        settled: io_context::AwaitJs<()>,
    },
    /// The promise of an awaited candidate, settled.
    Settled(v8::Global<v8::Promise>),
}

/// Compiles and runs an input: what became of it, or the promise it awaits.
fn run_input<'s>(
    lock: &mut jsg::Lock<'s>,
    context: &IoContext<'s>,
    state: &EnvironmentState,
    candidates: &[Candidate],
) -> jsg::Result<Ran> {
    let ran = lock.try_catch(|lock| {
        let (script, awaited) = compile(lock, &state.input_name, candidates)?;
        let result = script.run(lock)?;
        let promise = v8::Local::<v8::Promise>::try_from(result);
        let Some(promise) = promise.ok().filter(|_| awaited) else {
            // A promise the user did not await renders as a promise: awaiting it could hang the
            // REPL on one that never settles.
            state.record(lock, "_", result)?;
            return Ok(Ran::Done(Evaluation {
                text: render(lock, result, state.colors)?,
                threw: false,
            }));
        };
        // Settling is all that matters here; evaluate() reads the outcome off the promise.
        let settled = promise.then_or_catch(
            lock,
            (),
            |lock, (), _| Ok(lock.undefined()),
            |lock, (), _| Ok(lock.undefined()),
        )?;
        let settled = context.await_js(lock, settled, |_, _| Ok(()))?;
        Ok(Ran::Awaiting {
            result: promise.to_global(lock),
            settled,
        })
    })?;
    match ran {
        Ok(ran) => Ok(ran),
        Err(exception) => {
            state.record(lock, "_error", exception)?;
            Ok(Ran::Done(Evaluation {
                text: format!("Uncaught {}", describe_exception(lock, exception)?),
                threw: true,
            }))
        }
    }
}

/// The script of the first of `candidates` that compiles; whether its completion value is to be
/// awaited.
///
/// The REPL tries an input's candidates in order. If none compiles, the syntax error of the first
/// that isn't `parenthesized` is thrown. A candidate that isn't awaited and uses `await` as a
/// name, as `await (x)` does in a script, is taken only if no later candidate compiles: an input
/// that says `await` means to await.
fn compile<'s>(
    lock: &mut jsg::Lock<'s>,
    name: &str,
    candidates: &[Candidate],
) -> jsg::Result<(v8::Local<'s, v8::Script>, bool)> {
    let Some(first) = candidates.first() else {
        return Err(jsg::Error::new_internal_error("nothing to evaluate"));
    };
    let name = js_string(lock, name)?;
    if candidates.len() > 1 {
        let mut passed_over = None;
        for candidate in candidates {
            let source = js_string(lock, &candidate.source)?;
            let Ok(script) = lock.try_catch(|lock| v8::Script::compile(lock, source, name))? else {
                continue;
            };
            if candidate.awaited || !uses_await_as_name(lock, &candidate.source, name)? {
                return Ok((script, candidate.awaited));
            }
            passed_over.get_or_insert((script, candidate.awaited));
        }
        // Nothing later compiles, as for an input that declares a name `await` and uses it.
        if let Some(compiled) = passed_over {
            return Ok(compiled);
        }
    }
    // None compiles, so the syntax error of the first that isn't parenthesized is thrown.
    let reported = candidates
        .iter()
        .find(|candidate| !candidate.parenthesized)
        .unwrap_or(first);
    let source = js_string(lock, &reported.source)?;
    Ok((v8::Script::compile(lock, source, name)?, reported.awaited))
}

/// Whether `source`, a script that compiles, uses `await` as a name outside the async functions in
/// it, as `await (x)` does: a script reads that as a call to a function named `await`. In a class
/// static block, `await` is neither a name nor an operator except in the functions the block
/// holds, so the script does not compile in one if it does. The block is strict mode code, so a
/// script only sloppy mode accepts counts as one that does.
fn uses_await_as_name<'s>(
    lock: &mut jsg::Lock<'s>,
    source: &str,
    name: v8::Local<'s, v8::String>,
) -> jsg::Result<bool> {
    // The line break keeps a line comment that ends the script from swallowing the block's end.
    let wrapped = js_string(lock, &format!("(class {{ static {{{source}\n}} }})"))?;
    Ok(lock
        .try_catch(|lock| v8::Script::compile(lock, wrapped, name))?
        .is_err())
}

/// Compiles and runs one of the REPL's own scripts, which evaluates to a function.
fn run_script<'s>(
    lock: &mut jsg::Lock<'s>,
    source: &str,
    name: &str,
) -> jsg::Result<v8::Local<'s, v8::Function>> {
    let source = js_string(lock, source)?;
    let name = js_string(lock, name)?;
    let value = v8::Script::compile(lock, source, name)?.run(lock)?;
    v8::Local::<v8::Function>::try_from(value)
        .map_err(|_| jsg::Error::new_internal_error("a REPL script did not evaluate to a function"))
}

/// `value` as the runtime's `inspect()` renders it, or as a string if inspecting it throws (a
/// throwing getter, a revoked proxy).
fn render<'s>(
    lock: &mut jsg::Lock<'s>,
    value: v8::Local<'s, v8::Value>,
    colors: bool,
) -> jsg::Result<String> {
    let inspected = lock.try_catch(|lock| bridge::inspect(lock, value, colors))?;
    if let Ok(text) = inspected {
        return Ok(text);
    }
    Ok(lock
        .try_catch(|lock| Ok(value.to_js_string(lock)?.to_string(lock)))?
        .unwrap_or_else(|_| "[value cannot be displayed]".to_owned()))
}

/// What the REPL prints after "Uncaught " for `exception`: an Error's stack, which starts with its
/// name and message, or the value as a string.
fn describe_exception<'s>(
    lock: &mut jsg::Lock<'s>,
    exception: v8::Local<'s, v8::Value>,
) -> jsg::Result<String> {
    Ok(lock
        .try_catch(|lock| {
            if let Ok(object) = v8::Local::<v8::Object>::try_from(exception) {
                let stack = object.get(lock, "stack")?;
                if stack.is_string() {
                    return Ok(stack.to_js_string(lock)?.to_string(lock));
                }
            }
            Ok(exception.to_js_string(lock)?.to_string(lock))
        })?
        .unwrap_or_else(|_| "[exception cannot be displayed]".to_owned()))
}

fn js_string<'s>(lock: &mut jsg::Lock<'s>, text: &str) -> jsg::Result<v8::Local<'s, v8::String>> {
    v8::String::new_from_str(lock, text)
        .ok_or_else(|| jsg::Error::new_range_error("Invalid string length"))
}

/// The compatibility flags of the REPL's JavaScript:
///
/// - The user's, `flags`.
/// - `nodejs_compat` unless they take a position on it, so that `await import('node:assert')`
///   works.
/// - `unhandled_rejection_after_microtask_checkpoint` unless they take a position on it. Without
///   it, an RPC call that fails at once hands back a promise already rejected, which the input
///   awaiting it handles only a microtask later, so the failure is reported twice: as the input's,
///   and as an unhandled rejection.
/// - With `--experimental`, `service_binding_extra_handlers`, so that a loaded config's services
///   take `scheduled()` and `queue()` calls. That lets JavaScript reach the V8 deserializer through
///   `queue()`'s `serializedBody`, which is why it is experimental; the REPL's user runs any code
///   they like.
///
/// Naming a flag the date already enables is harmless.
fn compatibility_flags(flags: &[String], experimental: bool) -> Vec<String> {
    let chosen = |names: &[&str]| flags.iter().any(|flag| names.contains(&flag.as_str()));
    let mut all = flags.to_vec();
    if !chosen(&[
        "nodejs_compat",
        "no_nodejs_compat",
        "nodejs_compat_v2",
        "no_nodejs_compat_v2",
    ]) {
        all.push("nodejs_compat".to_owned());
    }
    if !chosen(&[
        "unhandled_rejection_after_microtask_checkpoint",
        "no_unhandled_rejection_after_microtask_checkpoint",
    ]) {
        all.push("unhandled_rejection_after_microtask_checkpoint".to_owned());
    }
    // Naming a flag twice is an error.
    if experimental && !chosen(&["service_binding_extra_handlers"]) {
        all.push("service_binding_extra_handlers".to_owned());
    }
    all
}

/// `a`, `a.b`, `a.b.c`: names separated by single dots. Only such an expression is evaluated to
/// complete a property name; never a call, an index or a literal.
fn is_identifier_chain(text: &str) -> bool {
    !text.is_empty()
        && text.split('.').all(|name| {
            name.chars()
                .next()
                .is_some_and(|first| !first.is_ascii_digit())
                && name
                    .chars()
                    .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '$')
        })
}

/// Lets the event loop run once, as C++ `kj::yield()` does.
async fn yield_now() {
    let mut yielded = false;
    std::future::poll_fn(|cx| {
        if yielded {
            return Poll::Ready(());
        }
        yielded = true;
        cx.waker().wake_by_ref();
        Poll::Pending
    })
    .await;
}

#[cfg(test)]
#[path = "environment-test.rs"]
mod tests;
