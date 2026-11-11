// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The FFI between `workerd repl`'s console and its C++ half (repl/repl.h): the session that owns
//! V8 and evaluates input. C++ calls back into [`repl_load_config`] to load a config as `workerd
//! serve` does.

#![allow(
    unsafe_code,
    reason = "holds a cxx bridge, which expands to unsafe FFI glue, and awaits its unsafe async \
              functions"
)]

use std::pin::Pin;

use kj_rs::KjOwn;

pub use super::session::repl_load_config;

#[cxx::bridge(namespace = "workerd::server::repl")]
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
    /// so that it can `await` (input.rs).
    #[derive(Debug, PartialEq, Eq)]
    struct ReplCandidate {
        source: String,
        /// The completion value is a promise to await, and render what it settles to.
        awaited: bool,
    }

    #[namespace = "kj_rs_tokio"]
    unsafe extern "C++" {
        include!("kj-rs-tokio/tokio-event-port.h");

        type TokioAsyncIoContext = kj_rs_tokio::TokioAsyncIoContext;
    }

    unsafe extern "C++" {
        include!("workerd/server/repl/repl.h");

        /// V8, the I/O of the loaded configs, and the environment input is evaluated in.
        type ReplSession;

        /// A session on the event loop `event_loop`, which it must not outlive.
        fn repl_session_new(
            event_loop: Pin<&mut TokioAsyncIoContext>,
            options: &ReplOptions,
        ) -> Result<KjOwn<ReplSession>>;

        /// Evaluates the first of `candidates` that compiles: the text to print, the rendered
        /// result or `Uncaught ...`. An error means the session itself failed.
        async unsafe fn repl_evaluate<'a>(
            session: Pin<&'a mut ReplSession>,
            candidates: Vec<ReplCandidate>,
        ) -> Result<String>;

        /// The property names of the value of `base`, an identifier chain, or of the global object
        /// if `base` is empty: the names tab completion offers.
        async unsafe fn repl_property_names<'a>(
            session: Pin<&'a mut ReplSession>,
            base: String,
        ) -> Result<Vec<String>>;

        /// Closes the loaded configs' servers and ends the session's JavaScript. Must complete
        /// before the session is dropped.
        async unsafe fn repl_drain<'a>(session: Pin<&'a mut ReplSession>) -> Result<()>;
    }

    extern "Rust" {
        /// Loads the config at `path`, a Cap'n Proto config (with the constant `const_name`, if
        /// not empty) or a `wrangler.json(c)`, as an encoded `Config` message. The error lists
        /// every problem.
        fn repl_load_config(path: &str, const_name: &str, experimental: bool) -> Result<Vec<u64>>;
    }
}

pub async fn evaluate(
    session: Pin<&mut ffi::ReplSession>,
    candidates: Vec<ffi::ReplCandidate>,
) -> Result<String, cxx::KjError> {
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

pub async fn drain(mut session: KjOwn<ffi::ReplSession>) -> Result<(), cxx::KjError> {
    // SAFETY: the returned future borrows the session, which outlives it here.
    unsafe { ffi::repl_drain(session.as_mut()) }
        .await
        .map_err(cxx::KjError::from)
}
