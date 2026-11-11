// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `workerd repl`: evaluates JavaScript typed at a console, in an environment whose `workerd`
//! global loads configs and calls their services (an experimental tool for developing workerd).
//!
//! Two threads share the work. The main thread runs the event loop and the session
//! (bridge.rs, over the C++ in //src/workerd/server/repl), which owns V8 and the loaded configs'
//! servers. The console thread (console.rs) blocks reading input and sends each finished input to
//! the session ([`session`]), so the event loop keeps running -- timers, `waitUntil()`, the
//! loaded servers -- while the console waits for the next line. What decides whether an input is
//! finished, and how an input that awaits compiles, is in input.rs.

mod bridge;
mod console;
#[cfg_attr(
    not(unix),
    expect(dead_code, reason = "the console edits lines only on Unix terminals")
)]
mod editor;
mod input;
mod session;
#[cfg(unix)]
mod terminal;

use std::io::IsTerminal;

use tokio::sync::mpsc;
use workerd_server::bridge::ffi as server;
use workerd_server::entry;

use crate::args::ReplArgs;

/// Runs the REPL until its input ends; the exit status.
pub fn run(verbose: bool, args: ReplArgs) -> workerd_server::Result<i32> {
    let options = bridge::ffi::ReplOptions {
        compat_date: args.compat_date.unwrap_or_default(),
        compat_flags: args.compat_flags,
        experimental: args.experimental,
        colors: std::io::stdout().is_terminal() && std::env::var_os("NO_COLOR").is_none(),
    };
    entry::without_config(verbose, move || {
        let mut runtime = kj_rs_tokio::Runtime::new()?;
        let session = match bridge::ffi::repl_session_new(runtime.context(), &options) {
            Ok(session) => session,
            Err(error) => {
                eprintln!("{}", error.what());
                return Ok(exit(1));
            }
        };
        let (requests, received) = mpsc::unbounded_channel();
        let console = std::thread::Builder::new()
            .name("repl-console".to_owned())
            .spawn(move || console::run(requests))
            .map_err(|error| {
                cxx::KjError::new(
                    cxx::KjExceptionType::Failed,
                    format!("starting the console: {error}"),
                )
            })?;
        let code = match runtime.block_on(session::serve(session, received)) {
            Ok(Ok(())) => {
                // The session ended because the console did.
                let _ = console.join();
                0
            }
            Ok(Err(error)) | Err(error) => {
                eprintln!("*** Uncaught exception ***\n{}", error.description());
                1
            }
        };
        let code = exit(code);
        drop(runtime);
        Ok(code)
    })
}

/// Ends the process with `code` at once, as `workerd serve` does, unless `KJ_CLEAN_SHUTDOWN` asks
/// for everything to be destroyed first; then `code`.
fn exit(code: i32) -> i32 {
    if std::env::var_os("KJ_CLEAN_SHUTDOWN").is_none() {
        server::cli_exit(code);
    }
    code
}
