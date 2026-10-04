// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `serve` and `test` commands: the command line's Rust half, and the process's schedule.
//!
//! The command line (cli/lib.rs) parses the arguments into [`RunOptions`] and calls
//! [`serve`] / [`test`]. Each creates the event loop -- a `kj_rs_tokio::Runtime`, the KJ loop
//! with tokio running whenever it sleeps -- has the C++ bootstrap
//! (bootstrap.c++) set up the process (logging, perfetto, V8) and build the worker factory on that
//! loop, then blocks on `server_serve` / `server_test`, which run the server
//! (`run.rs`). Every config error is printed, and the server then does not serve; unless
//! `--watch` is on, in which case it serves what it can, waits for the config to change and the
//! process re-executes itself.
//!
//! Exit: status 0 when the run completes, 1 if any error was reported or the run failed (the
//! failure is printed as `*** Uncaught exception ***`). However the run ended, the factory has
//! been dropped by then and, with it, V8 and the perfetto session. Unless `KJ_CLEAN_SHUTDOWN` is
//! set the process ends right there, without running the remaining destructors, like
//! `kj::ProcessContext::exit()`; with it, the Runtime is torn down and `main` returns the status.

use std::convert::Infallible;
use std::future::Future;
use std::rc::Rc;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;

use futures::future::Either;
use futures::future::LocalBoxFuture;
use kj_rs::KjOwn;

use crate::Result;
use crate::bridge::ffi;
use crate::channels::AbortReason;
use crate::config::Factory;
use crate::config::Reporter;
use crate::log::Severity;
use crate::run::RunOptions;

/// `--watch`: fails if watching does; a config change replaces the process with a fresh run of
/// the same command line instead.
pub type Watch = LocalBoxFuture<'static, Result<Infallible>>;

/// Whether an error was reported (`error`): the exit status is then 1.
static HAD_ERRORS: AtomicBool = AtomicBool::new(false);

/// Runs `workerd serve` (see the module doc); the exit status, if the process is to go on.
pub fn serve(
    verbose: bool,
    config: Vec<u64>,
    options: ffi::ServeOrTestOptions,
    run_options: RunOptions,
    watch: Option<Watch>,
) -> Result<i32> {
    let command = move |factory, structured| server_serve(factory, run_options, watch, structured);
    run(verbose, config, options, None, command)
}

/// Runs `workerd test` (see the module doc); the exit status, if the process is to go on.
/// `patterns` are the `<service-pattern>` and `<entrypoint-pattern>` globs.
pub fn test(
    verbose: bool,
    config: Vec<u64>,
    options: ffi::ServeOrTestOptions,
    test: ffi::TestOptions,
    run_options: RunOptions,
    patterns: (String, String),
    watch: Option<Watch>,
) -> Result<i32> {
    let command =
        move |factory, structured| server_test(factory, run_options, patterns, watch, structured);
    run(verbose, config, options, Some(test), command)
}

/// A command's Rust half, for the C++ frame that sets up the process's logging around it
/// (`with_process_context`), which passes it the config and whether logging is structured.
pub struct PendingCommand(Box<dyn FnOnce(Vec<u64>, bool) -> Result<i32>>);

#[expect(
    clippy::boxed_local,
    reason = "the bridge hands an opaque Rust type to C++ and back as a Box"
)]
pub fn run_pending_command(
    command: Box<PendingCommand>,
    config: Vec<u64>,
    structured_logging: bool,
) -> Result<i32> {
    (command.0)(config, structured_logging)
}

/// The process's schedule (see the module doc): the logging, the loop, the bootstrap, the
/// command, the exit.
fn run<Fut: Future<Output = Result<()>> + 'static>(
    verbose: bool,
    config: Vec<u64>,
    options: ffi::ServeOrTestOptions,
    test: Option<ffi::TestOptions>,
    command: impl FnOnce(KjOwn<ffi::WorkerFactory>, bool) -> Fut + 'static,
) -> Result<i32> {
    let command = move |config, structured| {
        let mut runtime = kj_rs_tokio::Runtime::new()?;
        let factory = ffi::bootstrap(runtime.context(), config, &options, test.as_ref().into())?;
        // The command owns the factory and drops it when it completes (`shutdown`); if it does not
        // complete, `block_on` drops it, and the factory with it, before returning.
        let result = runtime.block_on(command(factory, structured));
        if let Err(failure) = result.and_then(|result| result) {
            let failure = ffi::exception_text(&AbortReason(Some(failure)));
            let message = format!("*** Uncaught exception ***\n{failure}");
            error(structured, &message);
        }
        let code = i32::from(HAD_ERRORS.load(Ordering::Relaxed));
        if std::env::var_os("KJ_CLEAN_SHUTDOWN").is_none() {
            ffi::cli_exit(code);
        }
        drop(runtime);
        Ok(code)
    };
    let command = Box::new(PendingCommand(Box::new(command)));
    Ok(ffi::with_process_context(verbose, config, command)?)
}

/// Reports an error -- a config error, failed tests, the run's failure -- and makes the exit
/// status 1.
fn error(structured: bool, message: &str) {
    HAD_ERRORS.store(true, Ordering::Relaxed);
    report(structured, Severity::Error, message);
}

/// Writes one of the command line's own messages to stderr: as it is, or, under structured
/// logging, as a line of the JSON logger's format. stderr is where what supervises the process
/// looks for the reason of a failed start; the logger's own lines go to stdout.
fn report(structured: bool, severity: Severity, message: &str) {
    if structured {
        ffi::json_log_to_stderr(severity as u8, file!(), line!(), message);
    } else {
        eprintln!("{message}");
    }
}

/// Where config errors and warnings go (see `report`). With `--watch` the server serves despite
/// the errors, since this is a development server and someone is about to fix the config.
fn reporter(watching: bool, structured: bool) -> Reporter {
    Reporter::new(
        Box::new(move |message| error(structured, &message)),
        Box::new(move |message| report(structured, Severity::Warning, &message)),
        watching,
    )
}

/// Runs `command` alongside the watcher, if any: a config change reloads the process instead.
async fn with_watch<T>(
    command: impl Future<Output = Result<T>>,
    watch: Option<&mut Watch>,
) -> Result<T> {
    let Some(watch) = watch else {
        return command.await;
    };
    let command = std::pin::pin!(command);
    match futures::future::select(command, watch).await {
        Either::Left((result, _)) => result,
        Either::Right((failed, _)) => failed.map(|never| match never {}),
    }
}

/// Resolves on SIGTERM: the signal to drain gracefully. The handler is installed by the call, so
/// that a signal during startup drains the server once it is up. Never on Windows.
fn sigterm() -> impl Future<Output = ()> {
    #[cfg(unix)]
    let signal = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate());
    async move {
        #[cfg(unix)]
        match signal {
            Ok(mut signal) => {
                signal.recv().await;
            }
            Err(e) => {
                tracing::error!("SIGTERM handler: {e}");
                std::future::pending::<()>().await;
            }
        }
        #[cfg(not(unix))]
        std::future::pending::<()>().await;
    }
}

/// `workerd serve`: serves `factory`'s config until SIGTERM drains it (or a `--watch` reload
/// replaces the process). A listener failure is the error.
async fn server_serve(
    factory: KjOwn<ffi::WorkerFactory>,
    options: RunOptions,
    mut watch: Option<Watch>,
    structured: bool,
) -> Result<()> {
    crate::log::install();
    let report = reporter(watch.is_some(), structured);
    let factory = Rc::new(Factory::new(factory));
    let result = with_watch(
        crate::run::run(Rc::clone(&factory), options, report, sigterm()),
        watch.as_mut(),
    )
    .await;
    let shutdown = shutdown(factory).await;
    result?;
    shutdown
}

/// Tears the run down once the server is gone: ends the factory's background tasks, then drops
/// the factory and, with it, the last of the isolates, V8 and the perfetto session. A factory
/// still shared at this point is a leaked service.
pub(crate) async fn shutdown(factory: Rc<Factory>) -> Result<()> {
    factory.settle_tasks().await?;
    if Rc::try_unwrap(factory).is_err() {
        tracing::error!(
            "the worker factory is still referenced after the server was dropped; a service leaked"
        );
    }
    Ok(())
}

/// `workerd test`: runs the tests of `factory`'s config. Under `--watch`, waits for a change
/// afterwards instead of returning.
async fn server_test(
    factory: KjOwn<ffi::WorkerFactory>,
    options: RunOptions,
    (service_pattern, entrypoint_pattern): (String, String),
    mut watch: Option<Watch>,
    structured: bool,
) -> Result<()> {
    crate::log::install();
    let report = reporter(watch.is_some(), structured);
    let factory = Rc::new(Factory::new(factory));
    // Loopback sockets are for tests only.
    factory.loopback().enable();
    let result = with_watch(
        crate::run::test(
            Rc::clone(&factory),
            options,
            report,
            &service_pattern,
            &entrypoint_pattern,
        ),
        watch.as_mut(),
    )
    .await;
    let shutdown = shutdown(factory).await;
    let passed = result?;
    shutdown?;
    if passed == Some(false) {
        error(structured, "Tests failed!");
    }
    if let Some(watch) = watch {
        // Under --watch the process stays for the next change rather than exiting.
        watch.await?;
    }
    Ok(())
}
