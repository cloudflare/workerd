// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The FFI between `workerd repl`'s console and its C++ half (repl/repl.h): the session that owns
//! V8 and evaluates input. C++ calls back into [`repl_candidates`] to compile input as the console
//! does, and into the config servers (server.rs) to load and serve configs.

#![allow(
    unsafe_code,
    reason = "holds a cxx bridge, which expands to unsafe FFI glue, and awaits its unsafe async \
              functions"
)]

use std::pin::Pin;

use kj_rs::KjOwn;

pub use super::input::candidates as repl_candidates;
pub use super::server::PendingReload;
pub use super::server::ReplConfigServer;
pub use super::server::ServerRegistry;
pub use super::server::repl_reload_wait;
pub use super::server::repl_server_load;
pub use super::server::repl_server_wait_closed;

#[cxx::bridge(namespace = "workerd::server::repl")]
#[expect(clippy::unnecessary_box_returns, reason = "cxx requires a Box")]
pub mod ffi {
    struct ReplOptions {
        /// The compatibility date of the REPL's JavaScript; today if empty.
        compat_date: String,
        compat_flags: Vec<String>,
        /// `--experimental`, for the compatibility flags and the loaded configs.
        experimental: bool,
        /// `--autogate`: the process's autogates, by kebab-case name.
        autogates: Vec<String>,
        /// `--v8-flag`: the process's V8 flags.
        v8_flags: Vec<String>,
        /// Render results with ANSI colors.
        colors: bool,
    }

    /// One way to compile an input: the input itself, or the input wrapped in an async function
    /// so that it can `await`, or in parentheses so that a leading `{` starts an object literal
    /// (input.rs).
    #[derive(Debug, PartialEq, Eq)]
    struct ReplCandidate {
        source: String,
        /// The completion value is a promise to await, and render what it settles to.
        awaited: bool,
        /// The input is in parentheses it doesn't have, so a syntax error compiling it isn't the
        /// input's own: if no candidate compiles, another's error is reported.
        parenthesized: bool,
    }

    /// Whether a loaded config reloads whenever a file it was loaded from changes.
    #[derive(Debug)]
    enum Watch {
        Unwatched,
        Watched,
    }

    /// What became of an input.
    struct ReplEvaluation {
        /// The text to print: the rendered result, or `Uncaught ...`.
        text: String,
        /// The input threw, or the promise it awaited rejected: `text` renders the exception.
        threw: bool,
    }

    #[namespace = "kj_rs_tokio"]
    unsafe extern "C++" {
        include!("kj-rs-tokio/tokio-event-port.h");

        type TokioAsyncIoContext = kj_rs_tokio::TokioAsyncIoContext;
    }

    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/io/frankenvalue/ffi.h");

        type Frankenvalue = workerd_server::bridge::ffi::Frankenvalue;
    }

    // The server crate's bridge declares the factory and the channel.
    #[namespace = "workerd::server"]
    unsafe extern "C++" {
        include!("workerd/server/server/bridge.rs.h");

        type WorkerFactory = workerd_server::bridge::ffi::WorkerFactory;
    }

    #[namespace = "workerd::server"]
    extern "Rust" {
        type SubrequestChannel = workerd_server::channels::SubrequestChannel;
    }

    unsafe extern "C++" {
        include!("workerd/server/repl/repl.h");

        /// V8, the I/O of the loaded configs, and the environment input is evaluated in.
        type ReplSession;

        /// A session on the event loop `event_loop`, which it must not outlive, whose
        /// environments load configs through `registry`.
        fn repl_session_new(
            event_loop: Pin<&mut TokioAsyncIoContext>,
            options: &ReplOptions,
            registry: Box<ServerRegistry>,
        ) -> Result<KjOwn<ReplSession>>;

        /// Evaluates the first of `candidates` that compiles. An error means the session itself
        /// failed.
        async unsafe fn repl_evaluate<'a>(
            session: Pin<&'a mut ReplSession>,
            candidates: Vec<ReplCandidate>,
        ) -> Result<ReplEvaluation>;

        /// The property names of the value of `base`, an identifier chain, or of the global object
        /// if `base` is empty: the names tab completion offers.
        async unsafe fn repl_property_names<'a>(
            session: Pin<&'a mut ReplSession>,
            base: String,
        ) -> Result<Vec<String>>;

        /// Closes the loaded configs' servers and ends the session's JavaScript; whether
        /// anything failed in the background, such as a timer callback that threw or a rejection
        /// nothing handled. Must complete before the session is dropped.
        async unsafe fn repl_drain<'a>(session: Pin<&'a mut ReplSession>) -> Result<bool>;

        /// What the servers of an environment's configs are built from: the session's V8, timer,
        /// network and filesystem, and the environment's header table and `--experimental`.
        type ReplFactorySource;

        /// A worker factory for `config`, an encoded `Config` message.
        fn repl_new_factory(
            source: &ReplFactorySource,
            config: &[u64],
        ) -> Result<KjOwn<WorkerFactory>>;
    }

    extern "Rust" {
        /// The candidates of `input`, as the console makes them of an input typed at it
        /// (input.rs): what `workerd.environment()`'s environments evaluate.
        fn repl_candidates(input: &str) -> Vec<ReplCandidate>;

        /// What the servers of a session share, its environments' included (server.rs).
        type ServerRegistry;
        fn clone_handle(self: &ServerRegistry) -> Box<ServerRegistry>;

        /// The server of a config the REPL loaded (server.rs).
        type ReplConfigServer;

        /// Loads the config at `path`, a Cap'n Proto config (with the constant `const_name`, if
        /// not empty) or a `wrangler.json(c)`, and starts serving it on a factory `source`
        /// builds; if `watch` is `Watched`, reloads it whenever a file it was loaded from changes. The error,
        /// a JSG error, lists every problem.
        async fn repl_server_load(
            registry: Box<ServerRegistry>,
            source: KjRc<ReplFactorySource>,
            path: String,
            const_name: String,
            watch: Watch,
        ) -> Result<Box<ReplConfigServer>>;

        fn clone_handle(self: &ReplConfigServer) -> Box<ReplConfigServer>;
        fn path(self: &ReplConfigServer) -> &str;
        fn is_closed(self: &ReplConfigServer) -> bool;
        fn is_close_done(self: &ReplConfigServer) -> bool;
        fn is_watching(self: &ReplConfigServer) -> bool;
        fn warnings(self: &ReplConfigServer) -> Vec<String>;
        fn service(
            self: &ReplConfigServer,
            name: &str,
            entrypoint: KjMaybe<&str>,
            props: KjMaybe<KjOwn<Frankenvalue>>,
        ) -> Result<Box<SubrequestChannel>>;
        fn socket(self: &ReplConfigServer, name: &str) -> Result<Box<SubrequestChannel>>;
        fn durable_object(
            self: &ReplConfigServer,
            service: &str,
            class_name: &str,
            name: KjMaybe<&str>,
            id: KjMaybe<&str>,
        ) -> Result<Box<SubrequestChannel>>;

        /// A reload asked for, which `repl_reload_wait` waits for.
        type PendingReload;
        fn queue_reload(self: &ReplConfigServer) -> Box<PendingReload>;
        async fn repl_reload_wait(reload: Box<PendingReload>) -> Result<()>;

        fn begin_close(self: &ReplConfigServer);
        async fn repl_server_wait_closed(server: Box<ReplConfigServer>) -> Result<()>;
    }
}

pub async fn evaluate(
    session: Pin<&mut ffi::ReplSession>,
    candidates: Vec<ffi::ReplCandidate>,
) -> Result<ffi::ReplEvaluation, cxx::KjError> {
    // SAFETY: the returned future borrows the session for its duration.
    unsafe { ffi::repl_evaluate(session, candidates) }
        .await
        .map_err(cxx::KjError::from)
}

pub async fn property_names(
    session: Pin<&mut ffi::ReplSession>,
    base: String,
) -> Result<Vec<String>, cxx::KjError> {
    // SAFETY: the returned future borrows the session for its duration.
    unsafe { ffi::repl_property_names(session, base) }
        .await
        .map_err(cxx::KjError::from)
}

/// Drains the session; whether anything failed in the background (`repl_drain`).
pub async fn drain(mut session: KjOwn<ffi::ReplSession>) -> Result<bool, cxx::KjError> {
    // SAFETY: the returned future borrows the session, which outlives it here.
    unsafe { ffi::repl_drain(session.as_mut()) }
        .await
        .map_err(cxx::KjError::from)
}
