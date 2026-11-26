// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A REPL session on the test thread's event loop, driven as the console drives it: through the
//! bridge, with V8, the session's environment and the servers of the configs it loads.

use std::cell::RefCell;
use std::collections::HashMap;
use std::path::PathBuf;
use std::pin::pin;
use std::rc::Rc;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;
use std::time::Duration;

use futures::FutureExt;
use futures::future::LocalBoxFuture;
use tokio::sync::oneshot;

use super::input::Candidate;
use super::load_config;
use super::server;
use super::server::ServerRegistry;
use crate::repl::api;
use crate::repl::bridge;
use crate::repl::environment;
use crate::repl::environment::Evaluation;
use crate::repl::environment::Session;

/// A config with a worker that answers fetches with what it was asked, takes scheduled and queue
/// events, has an entrypoint with props, and an http socket in front of it.
const WORKER_CONFIG: &str = r#"(
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [
          ( name = "worker.js",
            esModule =
              `import { WorkerEntrypoint } from "cloudflare:workers";
              `export default {
              `  async fetch(request, env) {
              `    const url = new URL(request.url);
              `    const header = request.headers.get("x-test") ?? "none";
              `    return new Response(`${env.GREETING} ${request.method} ${url.pathname} ${header}`,
              `        { headers: { "x-reply": "yes" } });
              `  },
              `  scheduled(controller) {},
              `  queue(batch) { batch.ackAll(); },
              `};
              `export class Api extends WorkerEntrypoint {
              `  add(a, b) { return a + b; }
              `  whoami() { return this.ctx.props.user; }
              `}
          ),
        ],
        bindings = [ (name = "GREETING", text = "hello") ],
      ),
    ),
  ],
  sockets = [ (name = "http", address = "*:0", http = (), service = "main") ],
)"#;

/// A config with a worker and no sockets, which only the REPL reaches.
const NO_SOCKETS_CONFIG: &str = r#"(
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [
          ( name = "worker.js",
            esModule = `export default { fetch() { return new Response("hello"); } };
          ),
        ],
      ),
    ),
  ],
)"#;

/// A config the server reports errors in.
const BROKEN_CONFIG: &str = r#"(
  sockets = [ (name = "http", address = "*:0", http = (), service = "missing") ],
)"#;

/// A config whose worker's script throws as it starts.
const THROWING_SCRIPT_CONFIG: &str = r#"(
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [ (name = "worker.js", esModule = "throw new Error('broken');") ],
      ),
    ),
  ],
)"#;

/// A config with the process-wide settings the REPL ignores.
const PROCESS_WIDE_CONFIG: &str = r#"(
  v8Flags = ["--expose-gc"],
  services = [ (name = "main", external = (address = "localhost:1")) ],
)"#;

/// A config whose worker answers every fetch with `text`.
fn config_answering(text: &str) -> String {
    format!(
        r#"(
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [
          ( name = "worker.js",
            esModule = "export default {{ fetch() {{ return new Response('{text}'); }} }};" ),
        ],
      ),
    ),
  ],
)"#
    )
}

/// A config whose worker exports a Durable Object class, Counter, stored in `storage_dir`, and an
/// ephemeral one, Scratch. `module_end` ends the worker's module.
fn durable_objects_config(storage_dir: &str, module_end: &str) -> String {
    format!(
        r#"(
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [
          ( name = "worker.js",
            esModule =
              `import {{ DurableObject }} from "cloudflare:workers";
              `export class Counter extends DurableObject {{
              `  hits = 0;
              `  async increment() {{
              `    const total = ((await this.ctx.storage.get("total")) ?? 0) + 1;
              `    await this.ctx.storage.put("total", total);
              `    return `${{++this.hits}}/${{total}}`;
              `  }}
              `  id() {{ return this.ctx.id.toString(); }}
              `}}
              `export class Scratch extends DurableObject {{
              `  hits = 0;
              `  hit() {{ return ++this.hits; }}
              `}}
              `export default {{ fetch() {{ return new Response("ok"); }} }};
              `{module_end}
          ),
        ],
        durableObjectNamespaces = [
          (className = "Counter", uniqueKey = "counter"),
          (className = "Scratch", ephemeralLocal = void),
        ],
        durableObjectStorage = (localDisk = "storage"),
      ),
    ),
    (name = "storage", disk = (path = "{storage_dir}", writable = true)),
  ],
)"#
    )
}

/// A config whose socket "udp" listens for UDP datagrams on `address`.
fn udp_config(address: &str) -> String {
    format!(
        r#"(
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [ (name = "worker.js", esModule = "export default {{ connect() {{}} }};") ],
      ),
    ),
  ],
  sockets = [ (name = "udp", address = "{address}", udp = (), service = "main") ],
)"#
    )
}

const FETCH_MAIN: &str = "await (await s.service('main').fetch('http://x/')).text()";

/// The test's temporary directory.
fn test_tmpdir() -> PathBuf {
    std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from)
}

/// The directory `name` in the test's temporary directory, empty: without what a run before this
/// one stored.
fn empty_directory(name: &str) -> String {
    let directory = test_tmpdir().join(name);
    let _ = std::fs::remove_dir_all(&directory);
    std::fs::create_dir_all(&directory).expect("creating the directory");
    directory.display().to_string()
}

/// What the fixture's loader, watches and close hook share with the test.
#[derive(Default)]
struct Shared {
    /// The configs, by the path the REPL loads them by, as Cap'n Proto text.
    configs: RefCell<HashMap<String, String>>,
    /// Where loads write the configs, to compile them as `workerd serve` does.
    dir: PathBuf,
    /// Reports a change to the watch a server waits on last.
    change: RefCell<Option<oneshot::Sender<()>>>,
    /// Tells the test waiting for a server to wait on a watch.
    watch_awaited: RefCell<Option<oneshot::Sender<()>>>,
    /// The close [`Fixture::hold_next_close`] holds: what it waits on, and what to tell once it
    /// does.
    next_close: RefCell<Option<(oneshot::Receiver<()>, oneshot::Sender<()>)>>,
}

impl Shared {
    /// The config at `path`, compiled from its text by the console's loader.
    fn load(&self, path: &str) -> Result<Vec<u64>, String> {
        let Some(text) = self.configs.borrow().get(path).cloned() else {
            return Err(format!("{path}: No such file."));
        };
        let file = self.dir.join(path.replace(['/', '\\'], "_"));
        let schema = format!(
            "using Workerd = import \"/workerd/workerd.capnp\";\nconst config :Workerd.Config = \
             {text};\n"
        );
        std::fs::write(&file, schema).map_err(|error| error.to_string())?;
        load_config(
            &file.display().to_string(),
            None,
            super::Features::Stable,
            None,
        )
    }
}

struct TestLoader(Rc<Shared>);

impl server::Loader for TestLoader {
    fn load(&self, path: &str, _const_name: Option<&str>) -> Result<Vec<u64>, String> {
        self.0.load(path)
    }

    fn watch(&self) -> Result<Box<dyn server::Watch>, String> {
        Ok(Box::new(TestWatch(Rc::clone(&self.0))))
    }
}

/// A watch that reports a change when the test calls [`Fixture::change_watched_files`].
struct TestWatch(Rc<Shared>);

impl server::Watch for TestWatch {
    fn load(&self, path: &str, _const_name: Option<&str>) -> Result<Vec<u64>, String> {
        self.0.load(path)
    }

    fn changed(&self) -> LocalBoxFuture<'static, Result<(), String>> {
        let (change, changed) = oneshot::channel();
        *self.0.change.borrow_mut() = Some(change);
        if let Some(awaited) = self.0.watch_awaited.borrow_mut().take() {
            let _ = awaited.send(());
        }
        async move {
            match changed.await {
                Ok(()) => Ok(()),
                // A watch no change reaches waits on.
                Err(_) => std::future::pending().await,
            }
        }
        .boxed_local()
    }
}

/// The settings of the session's JavaScript.
#[derive(Clone, Copy, Default)]
struct Options {
    compat_date: &'static str,
    compat_flags: &'static [&'static str],
    experimental: bool,
}

/// A close [`Fixture::hold_next_close`] holds.
struct HeldClose {
    /// Resolves once the close has begun and waits.
    waiting: oneshot::Receiver<()>,
    /// Lets it go on.
    release: oneshot::Sender<()>,
}

/// A REPL session, whose configs are the texts the test sets by name. Drains the session when
/// dropped, unless the test did.
struct Fixture {
    session: Option<Session>,
    shared: Rc<Shared>,
    runtime: kj_rs_tokio::Runtime,
}

static FIXTURES: AtomicUsize = AtomicUsize::new(0);

impl Fixture {
    fn new() -> Self {
        Self::with(Options::default())
    }

    fn with(options: Options) -> Self {
        Self::try_with(options).expect("starting the session")
    }

    fn try_with(options: Options) -> Result<Self, cxx::KjError> {
        let dir = test_tmpdir().join(format!(
            "repl-configs-{}",
            FIXTURES.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&dir).expect("creating the configs' directory");
        let shared = Rc::new(Shared {
            dir,
            ..Shared::default()
        });
        for (path, text) in [
            ("worker.capnp", WORKER_CONFIG),
            ("no-sockets.capnp", NO_SOCKETS_CONFIG),
            ("broken.capnp", BROKEN_CONFIG),
            ("process-wide.capnp", PROCESS_WIDE_CONFIG),
        ] {
            shared
                .configs
                .borrow_mut()
                .insert(path.to_owned(), text.to_owned());
        }

        let hook = Rc::clone(&shared);
        let before_close: server::BeforeClose =
            Box::new(move || match hook.next_close.borrow_mut().take() {
                Some((held, waiting)) => {
                    let _ = waiting.send(());
                    held.map(drop).boxed_local()
                }
                None => futures::future::ready(()).boxed_local(),
            });
        let registry =
            ServerRegistry::new(Box::new(TestLoader(Rc::clone(&shared))), Some(before_close));
        let process = bridge::ffi::ReplProcessOptions {
            autogates: Vec::new(),
            v8_flags: Vec::new(),
        };
        let options = environment::Options {
            compat_date: Some(options.compat_date)
                .filter(|date| !date.is_empty())
                .map(str::to_owned),
            compat_flags: options.compat_flags.iter().map(|&f| f.to_owned()).collect(),
            experimental: options.experimental,
            colors: false,
        };
        let mut runtime = kj_rs_tokio::Runtime::new()?;
        let session = Session::new(runtime.context(), &process, options, registry)?;
        Ok(Self {
            session: Some(session),
            shared,
            runtime,
        })
    }

    fn set_config(&self, path: &str, text: &str) {
        self.shared
            .configs
            .borrow_mut()
            .insert(path.to_owned(), text.to_owned());
    }

    fn remove_config(&self, path: &str) {
        self.shared.configs.borrow_mut().remove(path);
    }

    /// Runs `call` on the session to completion. The event loop's futures are `'static`, so the
    /// session goes into the future and comes back out, as the console's loop holds it
    /// (session.rs).
    fn run<T: 'static>(&mut self, call: impl AsyncFnOnce(&Session) -> T + 'static) -> T {
        let session = self.session.take().expect("the session was drained");
        let (session, output) = self
            .runtime
            .block_on(async move {
                let output = call(&session).await;
                (session, output)
            })
            .expect("running the event loop");
        self.session = Some(session);
        output
    }

    fn evaluate(&mut self, candidates: Vec<Candidate>) -> Evaluation {
        self.run(async move |session| session.evaluate(candidates).await)
            .expect("evaluating")
    }

    /// Evaluates `input` as it is.
    fn eval(&mut self, input: &str) -> String {
        self.evaluate(vec![candidate(input, false)]).text
    }

    /// Evaluates `expression`, which may await, and renders what it settles to.
    fn eval_await(&mut self, expression: &str) -> String {
        self.evaluate(vec![awaiting(expression)]).text
    }

    /// Evaluates `input` as the REPL does an input that mentions `await`: as it is, then as an
    /// async function returning its value, then as an async function's statements (input.rs,
    /// without its rewriting of declarations or its parentheses around a leading `{`).
    fn eval_candidates(&mut self, input: &str) -> String {
        self.evaluate(vec![
            candidate(input, false),
            candidate(&format!("(async () => {{return ({input}\n)}})()"), true),
            candidate(&format!("(async () => {{{input}\n}})()"), true),
        ])
        .text
    }

    fn property_names(&mut self, base: &str) -> Vec<String> {
        let base = base.to_owned();
        self.run(async move |session| session.property_names(base).await)
            .expect("listing property names")
    }

    /// Drains the session, which dropping the fixture otherwise does; whether anything failed in
    /// the background.
    fn drain(&mut self) -> bool {
        let session = self.session.take().expect("the session was drained");
        self.runtime
            .block_on(session.drain())
            .expect("running the event loop")
            .expect("draining")
    }

    /// Reports a change to the files of the watched config, and waits for the reload it causes to
    /// be done: the server then waits on the watch the reload made.
    fn change_watched_files(&mut self) {
        let (awaited, waiting) = oneshot::channel();
        *self.shared.watch_awaited.borrow_mut() = Some(awaited);
        let change = self
            .shared
            .change
            .borrow_mut()
            .take()
            .expect("no server waits on a watch");
        change.send(()).expect("reporting the change");
        self.runtime
            .block_on(waiting)
            .expect("running the event loop")
            .expect("waiting for the reload");
    }

    /// Holds the next close of a server that the session loaded, in any environment, until
    /// released.
    fn hold_next_close(&self) -> HeldClose {
        let (release, held) = oneshot::channel();
        let (waiting_sender, waiting) = oneshot::channel();
        *self.shared.next_close.borrow_mut() = Some((held, waiting_sender));
        HeldClose { waiting, release }
    }

    fn wait(&mut self, receiver: oneshot::Receiver<()>) {
        self.runtime
            .block_on(receiver)
            .expect("running the event loop")
            .expect("waiting");
    }

    /// Drains the session while a close is held and the environment has let go of what it closes:
    /// the drain must wait for the close, which `release` lets go on, and then finish.
    fn expect_drain_waits_for(&mut self, release: oneshot::Sender<()>) {
        let session = self.session.take().expect("the session was drained");
        let failed = self
            .runtime
            .block_on(async move {
                let mut draining = pin!(session.drain());
                let early =
                    tokio::time::timeout(Duration::from_millis(100), draining.as_mut()).await;
                assert!(early.is_err(), "the drain did not wait for the close");
                release.send(()).expect("releasing the close");
                draining.await
            })
            .expect("running the event loop")
            .expect("draining");
        assert!(!failed);
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        if let Some(session) = self.session.take() {
            let drained = self.runtime.block_on(session.drain());
            if !std::thread::panicking() {
                drained.expect("running the event loop").expect("draining");
            }
        }
    }
}

fn candidate(source: &str, awaited: bool) -> Candidate {
    Candidate {
        source: source.to_owned(),
        awaited,
        parenthesized: false,
    }
}

fn awaiting(expression: &str) -> Candidate {
    candidate(&format!("(async () => ({expression}\n))()"), true)
}

#[track_caller]
fn assert_starts_with(text: &str, prefix: &str) {
    assert!(
        text.starts_with(prefix),
        "{text:?} does not start with {prefix:?}"
    );
}

#[test]
fn evaluates_input_and_keeps_state_between_inputs() {
    let mut fixture = Fixture::new();
    assert_eq!(fixture.eval("1 + 1"), "2");
    assert_eq!(fixture.eval("'a' + 'b'"), "'ab'");
    assert_eq!(
        fixture.eval("var a = 40; function f(x) { return x + 2; }"),
        "undefined"
    );
    assert_eq!(fixture.eval("f(a)"), "42");
    assert_eq!(fixture.eval("({ x: [1, 2] })"), "{ x: [ 1, 2 ] }");
    assert_eq!(
        fixture.eval("new URL('https://example.com/?q=1').searchParams.get('q')"),
        "'1'"
    );
}

#[test]
fn reports_exceptions_and_syntax_errors() {
    let mut fixture = Fixture::new();
    assert_starts_with(
        &fixture.eval("throw new TypeError('boom')"),
        "Uncaught TypeError: boom",
    );
    assert_eq!(fixture.eval("throw 1"), "Uncaught 1");
    assert_starts_with(&fixture.eval("1 +"), "Uncaught SyntaxError");
    // The environment goes on after a failed input.
    assert_eq!(fixture.eval("2"), "2");
}

#[test]
fn keeps_the_last_result_in_underscore_and_the_last_exception_in_underscore_error() {
    let mut fixture = Fixture::new();
    fixture.eval("6 * 7");
    assert_eq!(fixture.eval("_"), "42");
    assert_eq!(fixture.eval("_ + 1"), "43");
    fixture.eval("throw new RangeError('far')");
    assert_eq!(fixture.eval("_error.message"), "'far'");

    // An awaited input's outcome is what its promise settles to.
    fixture.eval_await("await Promise.resolve('later')");
    assert_eq!(fixture.eval("_"), "'later'");
    fixture.eval_await("await Promise.reject(new Error('no'))");
    assert_eq!(fixture.eval("_error.message"), "'no'");

    // Once assigned, `_` keeps the assigned value.
    assert_eq!(fixture.eval("_ = 'mine'"), "'mine'");
    fixture.eval("1");
    assert_eq!(fixture.eval("_"), "'mine'");
    // `_error` still follows exceptions.
    fixture.eval("throw 2");
    assert_eq!(fixture.eval("_error"), "2");
}

#[test]
fn says_whether_an_input_threw() {
    let mut fixture = Fixture::new();
    let mut threw =
        |source: &str, awaited: bool| fixture.evaluate(vec![candidate(source, awaited)]).threw;
    assert!(!threw("1 + 1", false));
    assert!(threw("null.x", false));
    assert!(threw("1 +", false));
    assert!(!threw("Promise.resolve(1)", true));
    assert!(threw("Promise.reject(new Error('no'))", true));
    // A result that renders as "Uncaught ..." is a result.
    assert_eq!(
        fixture.eval("class Uncaught {}; new Uncaught()"),
        "Uncaught {}"
    );
    assert!(
        !fixture
            .evaluate(vec![candidate("new Uncaught()", false)])
            .threw
    );
}

#[test]
fn tries_candidates_in_order_and_awaits_the_awaited_ones() {
    let mut fixture = Fixture::new();
    // The first does not compile; of the others, the first that does is evaluated.
    let evaluation = fixture.evaluate(vec![
        candidate("x = await f(", false),
        candidate("(async () => (await Promise.resolve(42)\n))()", true),
        candidate("'not reached'", false),
    ]);
    assert_eq!(evaluation.text, "42");

    assert_starts_with(
        &fixture.eval_await("await Promise.reject(new Error('no'))"),
        "Uncaught Error: no",
    );
    // A promise that is not awaited renders as a promise.
    assert_eq!(fixture.eval("Promise.resolve(1)"), "Promise { 1 }");
    // Timers run while an input is awaited.
    assert_eq!(
        fixture.eval_await("await new Promise(resolve => setTimeout(() => resolve('later'), 1))"),
        "'later'"
    );
}

#[test]
fn reports_the_syntax_error_of_an_input_not_of_its_parentheses() {
    let mut fixture = Fixture::new();
    let mut evaluate = |input: &str| {
        fixture
            .evaluate(vec![
                Candidate {
                    source: format!("({input}\n)"),
                    awaited: false,
                    parenthesized: true,
                },
                candidate(input, false),
            ])
            .text
    };
    // The parentheses make an object literal of what would be a block.
    assert_eq!(evaluate("{ a: 1, b: 2 }"), "{ a: 1, b: 2 }");
    assert_eq!(evaluate("{ let x = 1; x }"), "1");
    // In parentheses, this is "Unexpected identifier 'x'".
    assert_eq!(
        evaluate("{ let x = ; }"),
        "Uncaught SyntaxError: Unexpected token ';'"
    );
}

#[test]
fn awaits_an_input_that_a_script_would_read_as_using_a_name_await() {
    let mut fixture = Fixture::new();
    assert_eq!(fixture.eval_candidates("await (Promise.resolve(1))"), "1");
    assert_eq!(
        fixture.eval_candidates("await [Promise.resolve(2)]"),
        "[ Promise { 2 } ]"
    );
    assert_eq!(fixture.eval_candidates("await - 3"), "-3");
    assert_eq!(
        fixture.eval_candidates("if (true) { x = await (Promise.resolve(4)) }"),
        "undefined"
    );
    assert_eq!(fixture.eval("x"), "4");

    // An input that awaits only in async functions runs as it is, so its declarations are global.
    assert_eq!(
        fixture.eval_candidates("async function f() { return await (Promise.resolve(5)) }"),
        "undefined"
    );
    assert_eq!(fixture.eval_await("await f()"), "5");
    assert_eq!(
        fixture.eval_candidates("const g = async () => await (Promise.resolve(6))"),
        "undefined"
    );
    assert_eq!(fixture.eval_await("await g()"), "6");

    // An input that declares a name `await` runs as it is: nothing that awaits compiles.
    assert_eq!(
        fixture.eval_candidates("var await = (n) => n + 1; await (6)"),
        "7"
    );
}

#[test]
fn enables_nodejs_compat_unless_the_flags_take_a_position_on_it() {
    // A date before nodejs_compat was the default.
    let mut fixture = Fixture::with(Options {
        compat_date: "2024-01-01",
        ..Options::default()
    });
    assert_eq!(
        fixture.eval_await("(await import('node:path')).join('a', 'b')"),
        "'a/b'"
    );
    drop(fixture);

    let mut fixture = Fixture::with(Options {
        compat_date: "2024-01-01",
        compat_flags: &["no_nodejs_compat"],
        ..Options::default()
    });
    assert_starts_with(
        &fixture.eval_await("(await import('node:path')).join('a', 'b')"),
        "Uncaught Error: Module not found: node:path",
    );
}

/// How many times the REPL's JavaScript sees the failure of an RPC call through a stale Fetcher,
/// awaited at once, as an unhandled rejection. The REPL prints each as "Uncaught (in promise)",
/// besides the input's failure.
fn unhandled_stale_calls(fixture: &mut Fixture) -> String {
    fixture.set_config("reload.capnp", &config_answering("one"));
    fixture.eval_await("s = await workerd.loadConfig('reload.capnp')");
    fixture.eval("unhandled = 0; addEventListener('unhandledrejection', () => ++unhandled), 0");
    fixture.eval("f = s.service('main'), 0");
    fixture.eval_await("await s.reload()");
    // The call fails at once, and hands back a promise already rejected.
    assert_starts_with(
        &fixture.eval_await("await f.hello()"),
        "Uncaught Error: The server for reload.capnp was reloaded.",
    );
    fixture.eval_await("await new Promise((resolve) => setTimeout(resolve, 0)), unhandled")
}

#[test]
fn reports_an_awaited_call_that_fails_at_once_as_the_inputs_failure_alone() {
    // A date before unhandled_rejection_after_microtask_checkpoint was the default.
    let mut fixture = Fixture::with(Options {
        compat_date: "2025-06-01",
        ..Options::default()
    });
    assert_eq!(unhandled_stale_calls(&mut fixture), "0");
    drop(fixture);

    // Unless the flags take a position on it.
    let mut fixture = Fixture::with(Options {
        compat_date: "2025-06-01",
        compat_flags: &["no_unhandled_rejection_after_microtask_checkpoint"],
        ..Options::default()
    });
    assert_eq!(unhandled_stale_calls(&mut fixture), "1");
}

#[test]
fn rejects_invalid_compatibility_settings() {
    for options in [
        Options {
            compat_flags: &["no_such_flag"],
            ..Options::default()
        },
        Options {
            compat_date: "not-a-date",
            ..Options::default()
        },
    ] {
        let Err(error) = Fixture::try_with(options) else {
            panic!("the session started");
        };
        assert!(
            error.description().contains("Invalid compatibility flags"),
            "{}",
            error.description()
        );
    }
}

#[test]
fn lists_property_names_for_completion() {
    let mut fixture = Fixture::new();
    let globals = fixture.property_names("");
    assert!(globals.iter().any(|name| name == "workerd"));
    assert!(globals.iter().any(|name| name == "fetch"));

    let control = fixture.property_names("workerd");
    assert!(control.iter().any(|name| name == "loadConfig"));
    assert!(control.iter().any(|name| name == "servers"));

    fixture.eval("var thing = { inner: { deep: 1 } }");
    let inner = fixture.property_names("thing.inner");
    assert!(inner.iter().any(|name| name == "deep"));
    // Inherited names count.
    assert!(inner.iter().any(|name| name == "hasOwnProperty"));

    // Nothing for what is not an identifier chain, what does not evaluate, or null.
    assert!(fixture.property_names("f()").is_empty());
    assert!(fixture.property_names("noSuchName").is_empty());
    fixture.eval("var nothing = null");
    assert!(fixture.property_names("nothing").is_empty());
}

#[test]
fn loads_a_config_and_calls_its_services_and_sockets() {
    let mut fixture = Fixture::new();
    assert_eq!(
        fixture.eval_await("s = await workerd.loadConfig('worker.capnp')"),
        "ReplServer {\n  path: 'worker.capnp',\n  closed: false,\n  watching: false,\n  warnings: \
         [ [length]: 0 ]\n}"
    );
    assert_eq!(fixture.eval("workerd.servers.length"), "1");

    // Headers cross in both directions, though the REPL and the server key them differently.
    assert_eq!(
        fixture.eval_await(
            "(r = await s.service('main').fetch('http://x/a', { method: 'POST', headers: { \
             'x-test': 'sent' } })).headers.get('x-reply')"
        ),
        "'yes'"
    );
    assert_eq!(fixture.eval_await("await r.text()"), "'hello POST /a sent'");

    assert_eq!(
        fixture.eval_await("await s.service('main', { entrypoint: 'Api' }).add(2, 3)"),
        "5"
    );
    assert_eq!(
        fixture.eval_await(
            "await s.service('main', { entrypoint: 'Api', props: { user: 'ada' } }).whoami()"
        ),
        "'ada'"
    );

    assert_eq!(
        fixture.eval_await("await (await s.socket('http').fetch('http://x/b')).text()"),
        "'hello GET /b none'"
    );

    assert_starts_with(
        &fixture.eval("s.socket('nope')"),
        "Uncaught Error: The config has no socket named \"nope\".",
    );
    assert_starts_with(
        &fixture.eval("s.service('nope')"),
        "Uncaught Error: The config has no service named \"nope\".",
    );
}

#[test]
fn calls_a_services_scheduled_and_queue_handlers_with_experimental() {
    let scheduled = "(await s.service('main').scheduled({ cron: '* * * * *' })).outcome";
    let mut fixture = Fixture::with(Options {
        experimental: true,
        ..Options::default()
    });
    fixture.eval_await("s = await workerd.loadConfig('worker.capnp')");
    assert_eq!(fixture.eval_await(scheduled), "'ok'");
    assert_eq!(
        fixture.eval_await(
            "(await s.service('main').queue('q', [{ id: 'm', timestamp: new Date(), attempts: 1, \
             body: 'hi' }])).ackAll"
        ),
        "true"
    );
    drop(fixture);

    // Naming the flag as well is not naming it twice.
    let mut fixture = Fixture::with(Options {
        compat_flags: &["service_binding_extra_handlers"],
        experimental: true,
        ..Options::default()
    });
    fixture.eval_await("s = await workerd.loadConfig('worker.capnp')");
    assert_eq!(fixture.eval_await(scheduled), "'ok'");
    drop(fixture);

    let mut fixture = Fixture::new();
    fixture.eval_await("s = await workerd.loadConfig('worker.capnp')");
    // Without the flag, `scheduled` is an RPC call to the handler, which returns nothing.
    assert_eq!(
        fixture.eval_await("await s.service('main').scheduled({ cron: '* * * * *' })"),
        "undefined"
    );
}

#[test]
fn calls_the_services_of_a_config_with_no_sockets() {
    let mut fixture = Fixture::new();
    fixture.eval_await("s = await workerd.loadConfig('no-sockets.capnp')");
    // Lets the run of a server that has nothing to listen on end, if it does.
    fixture.eval_await("await scheduler.wait(1)");
    assert_eq!(
        fixture.eval_await("await (await s.service('main').fetch('http://x/')).text()"),
        "'hello'"
    );
}

#[test]
fn revokes_what_a_closed_server_handed_out() {
    let mut fixture = Fixture::new();
    fixture.eval_await("s = await workerd.loadConfig('worker.capnp')");
    fixture.eval("main = s.service('main'); api = s.service('main', { entrypoint: 'Api' })");
    // A response whose body is never read keeps its request open.
    assert_eq!(
        fixture.eval_await("(unread = await main.fetch('http://x/')).status"),
        "200"
    );

    assert_eq!(fixture.eval_await("await s.close()"), "undefined");
    assert_eq!(fixture.eval("s.closed"), "true");
    assert_eq!(fixture.eval("workerd.servers.length"), "0");
    // Closing again is fine.
    assert_eq!(fixture.eval_await("await s.close()"), "undefined");

    let closed = "Uncaught Error: The server for worker.capnp was closed.";
    assert_starts_with(&fixture.eval_await("await unread.text()"), closed);
    assert_starts_with(&fixture.eval_await("await main.fetch('http://x/')"), closed);
    assert_starts_with(&fixture.eval_await("await api.add(1, 2)"), closed);
    assert_starts_with(&fixture.eval("s.service('main')"), closed);
}

#[test]
fn reaches_a_configs_durable_objects_and_evicts_them() {
    // For the ephemeral class.
    let mut fixture = Fixture::with(Options {
        experimental: true,
        ..Options::default()
    });
    let storage_dir = empty_directory("repl-durable-objects");
    fixture.set_config(
        "durable-objects.capnp",
        &durable_objects_config(&storage_dir, ""),
    );
    assert_starts_with(
        &fixture.eval_await("s = await workerd.loadConfig('durable-objects.capnp')"),
        "ReplServer",
    );

    fixture.eval("alice = s.durableObject('main', 'Counter', 'alice')");
    assert_eq!(fixture.eval_await("await alice.increment()"), "'1/1'");
    assert_eq!(fixture.eval_await("await alice.increment()"), "'2/2'");
    // The same object, by name or by id; another name names another.
    assert_eq!(
        fixture
            .eval_await("await s.durableObject('main', 'Counter', { name: 'alice' }).increment()"),
        "'3/3'"
    );
    assert_eq!(
        fixture.eval_await(
            "await s.durableObject('main', 'Counter', { id: await alice.id() }).increment()"
        ),
        "'4/4'"
    );
    assert_eq!(
        fixture.eval_await("await s.durableObject('main', 'Counter', 'bob').increment()"),
        "'1/1'"
    );

    // Evicted, the object starts again from its storage.
    assert_eq!(
        fixture.eval_await("await workerd.evict(alice)"),
        "undefined"
    );
    assert_eq!(fixture.eval_await("await alice.increment()"), "'1/5'");
    assert_eq!(
        fixture.eval_await("await workerd.evict(alice, { webSockets: 'close' })"),
        "undefined"
    );
    assert_starts_with(
        &fixture.eval_await("await workerd.evict(alice)"),
        "Uncaught Error:",
    );
    // An option that isn't valid rejects, as `cloudflare:unsafe`'s evict() does.
    assert_eq!(
        fixture.eval(
            "evicting = workerd.evict(alice, { webSockets: 'bogus' }), evicting.catch(() => {}), \
             evicting instanceof Promise"
        ),
        "true"
    );
    assert_eq!(
        fixture.eval_await("await evicting"),
        "Uncaught TypeError: options.webSockets must be \"hibernate\" or \"close\"."
    );
    assert_starts_with(
        &fixture.eval_await("await workerd.evict(s.service('main'))"),
        "Uncaught Error: evict() can only be used on a Durable Object stub.",
    );

    // An ephemeral object has a name and no id.
    assert_eq!(
        fixture.eval_await("await s.durableObject('main', 'Scratch', 'x').hit()"),
        "1"
    );
    assert_eq!(
        fixture.eval_await("await s.durableObject('main', 'Scratch', 'x').hit()"),
        "2"
    );
    assert_starts_with(
        &fixture.eval("s.durableObject('main', 'Scratch', { id: 'x' })"),
        "Uncaught TypeError: The objects of the ephemeral Durable Object class \"Scratch\" have \
         names, not ids.",
    );

    assert_starts_with(
        &fixture.eval("s.durableObject('main', 'Counter', { id: 'x' })"),
        "Uncaught TypeError: Invalid Durable Object ID: must be 64 hex digits",
    );
    assert_starts_with(
        &fixture.eval("s.durableObject('main', 'Counter', {})"),
        "Uncaught TypeError: A Durable Object is named by one of a name and an id.",
    );
    assert_starts_with(
        &fixture.eval("s.durableObject('main', 'Nope', 'x')"),
        "Uncaught Error: Worker does not export a Durable Object class named \"Nope\"",
    );
    assert_starts_with(
        &fixture.eval("s.durableObject('nope', 'Counter', 'x')"),
        "Uncaught Error: The config has no service named \"nope\".",
    );

    fixture.eval_await("await s.close()");
    let closed = "Uncaught Error: The server for durable-objects.capnp was closed.";
    assert_starts_with(&fixture.eval_await("await alice.increment()"), closed);
    assert_starts_with(&fixture.eval_await("await workerd.evict(alice)"), closed);
}

#[test]
fn starts_environments_with_compatibility_settings_of_their_own() {
    let mut fixture = Fixture::with(Options {
        compat_date: "2025-01-01",
        ..Options::default()
    });
    // `navigator` came with 2022-03-21.
    fixture.eval_await("old = await workerd.environment({ compatibilityDate: '2022-01-01' })");
    assert_eq!(
        fixture.eval("old"),
        "ReplEnvironmentHandle {\n  compatibilityDate: '2022-01-01',\n  compatibilityFlags: [ \
         [length]: 0 ],\n  closed: false\n}"
    );
    assert_eq!(
        fixture.eval_await("await old.eval('typeof navigator')"),
        "\"'undefined'\""
    );
    assert_eq!(fixture.eval("typeof navigator"), "'object'");

    // Flags as well, and inputs that await; an environment keeps its state between inputs.
    fixture.eval_await(
        "flagged = await workerd.environment({ compatibilityDate: '2022-01-01', \
         compatibilityFlags: ['global_navigator'] })",
    );
    assert_eq!(
        fixture.eval_await("await flagged.eval('x = await Promise.resolve(typeof navigator)')"),
        "\"'object'\""
    );
    assert_eq!(
        fixture.eval_await("await flagged.eval('x')"),
        "\"'object'\""
    );
    assert_starts_with(
        &fixture.eval_await("await flagged.eval('throw new TypeError(`no`)')"),
        "'Uncaught TypeError: no",
    );

    // Today's date without one.
    fixture.eval_await("today = await workerd.environment()");
    assert_eq!(
        fixture.eval("today.compatibilityDate === new Date().toISOString().slice(0, 10)"),
        "true"
    );

    assert_starts_with(
        &fixture.eval_await("await workerd.environment({ compatibilityFlags: ['no_such_flag'] })"),
        "Uncaught Error: Invalid compatibility flags:",
    );

    // An evaluation the close cuts short fails, and so does every later one.
    fixture.eval("pending = old.eval('await new Promise(() => {})')");
    assert_eq!(fixture.eval_await("await old.close()"), "undefined");
    assert_eq!(fixture.eval("old.closed"), "true");
    let closed = "Uncaught Error: The environment was closed.";
    assert_starts_with(&fixture.eval_await("await pending"), closed);
    assert_starts_with(&fixture.eval_await("await old.eval('1')"), closed);
    assert_eq!(fixture.eval_await("await old.close()"), "undefined");
}

#[test]
fn reloads_a_config_in_place() {
    let mut fixture = Fixture::new();
    fixture.set_config("reload.capnp", &config_answering("one"));
    assert_eq!(
        fixture.eval_await("(s = await workerd.loadConfig('reload.capnp')).watching"),
        "false"
    );
    fixture.eval("main = s.service('main')");
    assert_eq!(fixture.eval_await(FETCH_MAIN), "'one'");

    fixture.set_config("reload.capnp", &config_answering("two"));
    assert_eq!(fixture.eval_await("(await s.reload()) === s"), "true");
    assert_eq!(fixture.eval_await(FETCH_MAIN), "'two'");
    assert_eq!(fixture.eval("workerd.servers.length"), "1");
    // What the replaced server handed out fails, saying how to get its replacement.
    assert_starts_with(
        &fixture.eval_await("await main.fetch('http://x/')"),
        "Uncaught Error: The server for reload.capnp was reloaded. Get a new one from its \
         service(), socket() or durableObject().",
    );

    // A config that no longer loads leaves the server as it was.
    fixture.remove_config("reload.capnp");
    assert_starts_with(
        &fixture.eval_await("await s.reload()"),
        "Uncaught Error: reload.capnp: No such file.",
    );
    assert_eq!(fixture.eval_await(FETCH_MAIN), "'two'");

    // One whose server doesn't start leaves nothing serving, since the old server closes first,
    // until a reload starts one.
    let no_server = "Uncaught Error: Nothing serves reload.capnp: its last reload failed to start \
                     the new server. Fix the config and reload().";
    fixture.set_config("reload.capnp", THROWING_SCRIPT_CONFIG);
    assert_starts_with(
        &fixture.eval_await("await s.reload()"),
        "Uncaught Error: reload.capnp did not reload, and nothing serves it: Failed to load \
         reload.capnp:",
    );
    assert_starts_with(&fixture.eval("s.service('main')"), no_server);
    assert_starts_with(&fixture.eval("s.socket('http')"), no_server);
    assert_eq!(
        fixture.eval("[s.closed, workerd.servers.length]"),
        "[ false, 1 ]"
    );
    fixture.remove_config("reload.capnp");
    assert_starts_with(
        &fixture.eval_await("await s.reload()"),
        "Uncaught Error: reload.capnp did not reload, and nothing serves it: reload.capnp: No \
         such file.",
    );
    fixture.set_config("reload.capnp", &config_answering("three"));
    assert_eq!(fixture.eval_await("(await s.reload()) === s"), "true");
    assert_eq!(fixture.eval_await(FETCH_MAIN), "'three'");

    fixture.eval_await("await s.close()");
    assert_starts_with(
        &fixture.eval_await("await s.reload()"),
        "Uncaught Error: The server for reload.capnp was closed.",
    );
}

#[test]
fn reloads_a_watched_config_when_a_file_it_was_loaded_from_changes() {
    let mut fixture = Fixture::new();
    fixture.set_config("reload.capnp", &config_answering("one"));
    assert_eq!(
        fixture
            .eval_await("(s = await workerd.loadConfig('reload.capnp', { watch: true })).watching"),
        "true"
    );
    assert_eq!(fixture.eval_await(FETCH_MAIN), "'one'");
    fixture.set_config("reload.capnp", &config_answering("two"));
    fixture.change_watched_files();
    assert_eq!(fixture.eval_await(FETCH_MAIN), "'two'");

    // A change that leaves the config unloadable leaves the server running, and one that breaks
    // its server leaves nothing serving. The change that fixes it reloads it.
    fixture.remove_config("reload.capnp");
    fixture.change_watched_files();
    assert_eq!(fixture.eval_await(FETCH_MAIN), "'two'");
    fixture.set_config("reload.capnp", BROKEN_CONFIG);
    fixture.change_watched_files();
    assert_starts_with(
        &fixture.eval_await(FETCH_MAIN),
        "Uncaught Error: Nothing serves reload.capnp",
    );
    fixture.set_config("reload.capnp", &config_answering("three"));
    fixture.change_watched_files();
    assert_eq!(fixture.eval_await(FETCH_MAIN), "'three'");

    // A reload asked for by hand watches on.
    fixture.set_config("reload.capnp", &config_answering("four"));
    fixture.eval_await("await s.reload()");
    fixture.set_config("reload.capnp", &config_answering("five"));
    fixture.change_watched_files();
    assert_eq!(fixture.eval_await(FETCH_MAIN), "'five'");

    fixture.eval_await("await s.close()");
    assert_eq!(fixture.eval("s.watching"), "false");
}

#[test]
fn closes_the_server_a_reload_replaces_before_the_new_one_serves() {
    let mut fixture = Fixture::new();
    fixture.set_config("reload.capnp", &config_answering("one"));
    fixture.eval_await("s = await workerd.loadConfig('reload.capnp')");
    fixture.set_config("reload.capnp", &config_answering("two"));
    let close = fixture.hold_next_close();
    assert_eq!(
        fixture.eval("reloaded = s.reload(), 'reloading'"),
        "'reloading'"
    );
    fixture.wait(close.waiting);
    // The old server is closing, and the new one isn't serving yet.
    assert_starts_with(
        &fixture.eval("s.service('main')"),
        "Uncaught Error: The server for reload.capnp is reloading.",
    );
    close.release.send(()).expect("releasing the close");
    assert_eq!(fixture.eval_await("(await reloaded) === s"), "true");
    assert_eq!(fixture.eval_await(FETCH_MAIN), "'two'");
}

#[test]
fn refuses_a_config_whose_durable_objects_another_server_stores() {
    let mut fixture = Fixture::with(Options {
        experimental: true,
        ..Options::default()
    });
    let storage_dir = empty_directory("repl-claimed-durable-objects");
    fixture.set_config(
        "durable-objects.capnp",
        &durable_objects_config(&storage_dir, ""),
    );
    fixture.set_config("again.capnp", &durable_objects_config(&storage_dir, ""));
    fixture.eval_await("s = await workerd.loadConfig('durable-objects.capnp')");
    let refused = format!(
        "Uncaught Error: again.capnp stores Durable Objects in {storage_dir}/counter, as the \
         server for durable-objects.capnp does. Close that server first."
    );
    assert_starts_with(
        &fixture.eval_await("await workerd.loadConfig('again.capnp')"),
        &refused,
    );
    assert_eq!(fixture.eval("workerd.servers.length"), "1");
    // As is a load in another environment. Its eval() returns the message as a string, which
    // renders as a literal that escapes backslashes, such as a Windows path's.
    fixture.eval_await("e = await workerd.environment()");
    assert_starts_with(
        &fixture.eval_await("await e.eval(\"await workerd.loadConfig('again.capnp')\")"),
        &format!("'{}", refused.replace('\\', "\\\\")),
    );

    // A reload claims what the server it replaces held.
    assert_eq!(fixture.eval_await("(await s.reload()) === s"), "true");
    assert_starts_with(
        &fixture.eval_await("await workerd.loadConfig('again.capnp')"),
        &refused,
    );

    // A reload whose server doesn't start leaves nothing holding them.
    fixture.set_config(
        "durable-objects.capnp",
        &durable_objects_config(&storage_dir, "throw new Error('broken');"),
    );
    assert_starts_with(
        &fixture.eval_await("await s.reload()"),
        "Uncaught Error: durable-objects.capnp did not reload, and nothing serves it:",
    );
    assert_starts_with(
        &fixture.eval_await("t = await workerd.loadConfig('again.capnp')"),
        "ReplServer {\n  path: 'again.capnp',",
    );
    fixture.eval_await("await t.close()");
    fixture.set_config(
        "durable-objects.capnp",
        &durable_objects_config(&storage_dir, ""),
    );
    assert_eq!(fixture.eval_await("(await s.reload()) === s"), "true");
    assert_starts_with(
        &fixture.eval_await("await workerd.loadConfig('again.capnp')"),
        &refused,
    );

    // Once that server has closed, another may store them.
    fixture.eval_await("await s.close()");
    assert_starts_with(
        &fixture.eval_await("t = await workerd.loadConfig('again.capnp')"),
        "ReplServer {\n  path: 'again.capnp',",
    );
    assert_eq!(
        fixture.eval_await("await t.durableObject('main', 'Counter', 'alice').increment()"),
        "'1/1'"
    );
}

#[test]
fn refuses_a_config_a_udp_address_another_server_listens_on() {
    let mut fixture = Fixture::new();
    // A port that was free a moment ago.
    let port = std::net::UdpSocket::bind("127.0.0.1:0")
        .and_then(|socket| socket.local_addr())
        .expect("binding a UDP port")
        .port();
    let address = format!("127.0.0.1:{port}");
    fixture.set_config("udp.capnp", &udp_config(&address));
    fixture.eval_await("s = await workerd.loadConfig('udp.capnp')");
    let refused = format!(
        "Uncaught Error: udp.capnp listens for UDP datagrams on {address}, as the server for \
         udp.capnp does. Close that server first."
    );
    assert_starts_with(
        &fixture.eval_await("await workerd.loadConfig('udp.capnp')"),
        &refused,
    );
    assert_eq!(fixture.eval("workerd.servers.length"), "1");

    // A reload claims what the server it replaces held.
    assert_eq!(fixture.eval_await("(await s.reload()) === s"), "true");
    assert_starts_with(
        &fixture.eval_await("await workerd.loadConfig('udp.capnp')"),
        &refused,
    );

    // Once that server has closed, another may listen there.
    fixture.eval_await("await s.close()");
    assert_starts_with(
        &fixture.eval_await("await workerd.loadConfig('udp.capnp')"),
        "ReplServer {\n  path: 'udp.capnp',",
    );

    // On port 0, or none, each server listens on a port of the system's choosing.
    for unclaimed in ["127.0.0.1:0", "127.0.0.1"] {
        fixture.set_config("any-port.capnp", &udp_config(unclaimed));
        for _ in 0..2 {
            assert_starts_with(
                &fixture.eval_await("await workerd.loadConfig('any-port.capnp')"),
                "ReplServer {\n  path: 'any-port.capnp',",
            );
        }
    }
}

#[test]
fn says_whether_anything_failed_in_the_background() {
    let mut fixture = Fixture::new();
    fixture.eval_await("await new Promise((resolve) => setTimeout(resolve, 0)), 0");
    assert!(!fixture.drain());
    // One event loop per thread.
    drop(fixture);

    for input in [
        // A timer callback that throws. A timer still pending at the drain never fires.
        "setTimeout(() => { throw new Error('timer') }, 0), await new Promise((resolve) => \
         setTimeout(resolve, 10))",
        // A rejection nothing handles.
        "void Promise.reject(new Error('rejected'))",
        // A waitUntil() task that rejects.
        "(await import('cloudflare:workers')).waitUntil(Promise.reject(new Error('task')))",
        // Any of those in an environment started from this one, closed or not.
        "e = await workerd.environment(), await e.eval('void Promise.reject(1)')",
        "e = await workerd.environment(), await e.eval('void Promise.reject(1)'), await e.close()",
    ] {
        let mut fixture = Fixture::new();
        let text = fixture.eval_await(input);
        assert!(!text.starts_with("Uncaught "), "{input}: {text}");
        assert!(fixture.drain(), "{input}");
    }
}

#[test]
fn reports_configs_that_do_not_load() {
    let mut fixture = Fixture::new();
    assert_starts_with(
        &fixture.eval_await("await workerd.loadConfig('missing.capnp')"),
        "Uncaught Error: missing.capnp: No such file.",
    );
    assert_starts_with(
        &fixture.eval_await("await workerd.loadConfig('broken.capnp')"),
        "Uncaught Error: Failed to load broken.capnp:\n  ",
    );
    assert_eq!(fixture.eval("workerd.servers.length"), "0");

    let warnings = fixture.eval_await("(await workerd.loadConfig('process-wide.capnp')).warnings");
    assert!(warnings.contains("v8Flags are ignored"), "{warnings}");
}

#[test]
fn closes_loaded_servers_when_it_drains() {
    let mut fixture = Fixture::new();
    fixture.eval_await("s = await workerd.loadConfig('worker.capnp')");
    fixture.eval_await("t = await workerd.loadConfig('worker.capnp')");
    assert_eq!(fixture.eval("workerd.servers.length"), "2");
    // The drain closes both.
    assert!(!fixture.drain());
}

#[test]
fn closes_the_server_of_a_load_still_under_way_when_it_drains() {
    let mut fixture = Fixture::new();
    fixture.eval_await("s = await workerd.loadConfig('worker.capnp')");
    // Not awaited: the drain runs while the load does, and closes its server once it has
    // started.
    assert_eq!(
        fixture.eval("workerd.loadConfig('worker.capnp'), 'loading'"),
        "'loading'"
    );
    assert!(!fixture.drain());
}

#[test]
fn closes_a_server_at_once_whatever_the_configs_shutdown_delay() {
    let mut fixture = Fixture::new();
    // WORKER_CONFIG, with a delay that the test would wait out if the close did not skip it.
    fixture.set_config(
        "delayed.capnp",
        &format!("(shutdownDelayMs = 20000,{}", &WORKER_CONFIG[1..]),
    );
    fixture.eval_await("s = await workerd.loadConfig('delayed.capnp')");
    // Answered once the run serves its listeners. A close before then drains at once anyway, as
    // for a signal that comes while the server starts.
    assert_eq!(
        fixture.eval_await("await (await s.socket('http').fetch('http://x/')).text()"),
        "'hello GET / none'"
    );
    let closed = fixture
        .run(async move |session| {
            let closing = session.evaluate(vec![awaiting("await s.close()")]);
            tokio::time::timeout(Duration::from_secs(5), closing).await
        })
        .expect("the close waited out the config's delay")
        .expect("evaluating");
    assert_eq!(closed.text, "undefined");
}

#[test]
fn drains_a_server_whose_close_a_later_load_found_still_under_way() {
    let mut fixture = Fixture::new();
    fixture.eval_await("s = await workerd.loadConfig('worker.capnp')");
    let HeldClose { waiting, release } = fixture.hold_next_close();
    assert_eq!(fixture.eval("s.close(), 'closing'"), "'closing'");
    fixture.wait(waiting);
    // A load lets go of the servers that have closed.
    fixture.eval_await("t = await workerd.loadConfig('worker.capnp')");
    fixture.expect_drain_waits_for(release);
}

#[test]
fn drains_an_environment_whose_close_a_later_start_found_still_under_way() {
    let mut fixture = Fixture::new();
    fixture.eval_await("e = await workerd.environment()");
    assert_starts_with(
        &fixture.eval_await("await e.eval(\"s = await workerd.loadConfig('worker.capnp')\")"),
        "'ReplServer {",
    );
    let HeldClose { waiting, release } = fixture.hold_next_close();
    // Closing the environment closes its server, whose close waits.
    assert_eq!(fixture.eval("e.close(), 'closing'"), "'closing'");
    fixture.wait(waiting);
    // A start lets go of the environments that have closed.
    fixture.eval_await("f = await workerd.environment()");
    fixture.expect_drain_waits_for(release);
}

/// A handle holds what it stands for through the `IoContext` (`IoOwnedLocal`); holding it directly
/// does not compile, since a resource's fields are `Send`. C++ JSG destroys what JavaScript held
/// under `DISALLOW_KJ_IO_DESTRUCTORS_SCOPE`, which aborts if a KJ I/O object is destroyed there:
/// dropping a handle must not abort, and lets go of what it holds at once, as the `IoContext`
/// destroys it outside that scope.
#[test]
fn dropping_a_handle_destroys_no_kj_io_objects() {
    let mut fixture = Fixture::new();
    let (server, environment) = fixture.run(async |session| {
        let state = session.state();
        let server = state
            .load_config("worker.capnp".to_owned(), None, server::Watching::Unwatched)
            .await
            .expect("loading the config");
        let environment = state
            .start_environment(None, Vec::new())
            .await
            .expect("starting the environment");
        (server, environment)
    });
    let probe = (server.clone(), Rc::clone(&environment));
    let (held, dropped) = fixture.run(async move |session| {
        session
            .run(move |lock, _context| {
                let (server, environment) = probe;
                // Never wrapped, so that each is its handle's only reference, and dropping it
                // destroys it at once, as collecting its wrapper would.
                let handles = (
                    api::ReplServer::new(lock, server.clone())?,
                    api::ReplEnvironmentHandle::new(lock, Rc::clone(&environment))?,
                );
                let held = (server.holders(), Rc::strong_count(&environment));
                drop(handles);
                let dropped = (server.holders(), Rc::strong_count(&environment));
                Ok(std::future::ready(Ok((held, dropped))))
            })
            .expect("running in the environment")
            .await
            .expect("dropping the handles")
    });
    // The environment's lists, the test's two, and the handles'.
    assert_eq!(held, (4, 4));
    assert_eq!(dropped, (3, 3));
    // Held to here, so that the counts include them.
    drop((server, environment));
}

#[test]
fn drains_the_environments_started_and_their_servers() {
    let mut fixture = Fixture::new();
    fixture.eval_await("e = await workerd.environment()");
    assert_eq!(
        fixture.eval_await(
            "await e.eval(\"(s = await workerd.loadConfig('worker.capnp'), 'loaded')\")"
        ),
        "\"'loaded'\""
    );
    // Neither is closed: the drain closes the environment, whose own drain closes the server.
    let HeldClose {
        mut waiting,
        release,
    } = fixture.hold_next_close();
    fixture.expect_drain_waits_for(release);
    waiting
        .try_recv()
        .expect("the drain did not close the environment's server");
}

#[test]
fn refuses_loads_and_environment_starts_once_the_drain_has_begun() {
    let mut fixture = Fixture::new();
    fixture.eval_await("s = await workerd.loadConfig('worker.capnp')");
    let HeldClose { waiting, release } = fixture.hold_next_close();
    let session = fixture.session.take().expect("the session was drained");
    let state = session.state();
    let (load, start, failed) = fixture
        .runtime
        .block_on(async move {
            let mut draining = pin!(session.drain());
            // The drain has begun once it closes the server, whose close is held.
            match futures::future::select(draining.as_mut(), waiting).await {
                futures::future::Either::Left(_) => panic!("the drain did not close the server"),
                futures::future::Either::Right((began, _)) => began.expect("waiting for the close"),
            }
            let load = state
                .load_config("worker.capnp".to_owned(), None, server::Watching::Unwatched)
                .await
                .err()
                .map(|error| error.description().to_owned());
            let start = state
                .start_environment(None, Vec::new())
                .await
                .err()
                .map(|error| error.description().to_owned());
            release.send(()).expect("releasing the close");
            (load, start, draining.await)
        })
        .expect("running the event loop");
    let closing = Some("jsg.Error: The environment is closing.".to_owned());
    assert_eq!(load, closing);
    assert_eq!(start, closing);
    assert!(!failed.expect("draining"));
}
