// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The REPL's session loop, which runs on the event loop's thread and serves the console's
//! requests one at a time, and the config loading the session calls back into.

use std::path::Path;

use kj_rs::KjOwn;
use tokio::sync::mpsc;
use tokio::sync::oneshot;

use super::bridge;
use super::bridge::ffi::ReplSession;
use super::editor::Completion;
use super::input;
use crate::bridge::ffi;
use crate::config::Config;
use crate::process::Process;
use crate::watch::Watcher;
use crate::wrangler;

/// The most names tab completion offers.
const MAX_COMPLETIONS: usize = 200;

/// What the console asks of the session.
pub enum Request {
    /// The text to print for `input`; an error if the session failed to evaluate it.
    Evaluate {
        input: String,
        reply: oneshot::Sender<Result<String, String>>,
    },
    /// Tab completion in `line` with the cursor at byte offset `cursor`.
    Complete {
        line: String,
        cursor: usize,
        reply: oneshot::Sender<Completion>,
    },
}

/// Serves `requests` until the console drops its sender, then drains the session.
pub async fn serve(
    mut session: KjOwn<ReplSession>,
    mut requests: mpsc::UnboundedReceiver<Request>,
) -> Result<(), cxx::KjError> {
    while let Some(request) = requests.recv().await {
        // A console that gave up on the reply has ended; its next `recv` sees that.
        match request {
            Request::Evaluate { input, reply } => {
                let result = bridge::evaluate(session.as_mut(), input::candidates(&input)).await;
                let _ = reply.send(result.map_err(|error| error.description().to_owned()));
            }
            Request::Complete {
                line,
                cursor,
                reply,
            } => {
                let completion = complete(session.as_mut(), &line, cursor).await;
                let _ = reply.send(completion);
            }
        }
    }
    bridge::drain(session).await
}

/// The global or property names that complete the name before the cursor.
async fn complete(
    session: std::pin::Pin<&mut ReplSession>,
    line: &str,
    cursor: usize,
) -> Completion {
    let Some(target) = input::completion_target(line, cursor) else {
        return Completion {
            start: cursor,
            candidates: Vec::new(),
        };
    };
    // A base that fails to evaluate, or that throws, has nothing to complete.
    let names = bridge::property_names(session, target.base.to_owned())
        .await
        .unwrap_or_default();
    let mut candidates: Vec<String> = names
        .into_iter()
        .filter(|name| name.starts_with(target.partial) && input::is_identifier(name))
        .collect();
    candidates.sort_unstable();
    candidates.dedup();
    candidates.truncate(MAX_COMPLETIONS);
    Completion {
        start: target.start,
        candidates,
    }
}

/// See the bridge's declaration. A config's problems are returned, never printed, since the REPL
/// goes on after a config fails to load.
pub fn repl_load_config(
    path: &str,
    const_name: &str,
    experimental: bool,
) -> Result<Vec<u64>, cxx::KjError> {
    load_config(path, const_name, experimental, None)
}

/// See the bridge's declaration.
pub fn repl_watch_new() -> Result<Box<ReplWatch>, cxx::KjError> {
    let watcher = Watcher::new()
        .map_err(|error| cxx::KjError::new(cxx::KjExceptionType::Failed, error.to_string()))?;
    Ok(Box::new(ReplWatch { watcher }))
}

/// The files configs were loaded from, watched.
pub struct ReplWatch {
    watcher: Watcher,
}

impl ReplWatch {
    /// See the bridge's declaration.
    pub fn load_config(
        &self,
        path: &str,
        const_name: &str,
        experimental: bool,
    ) -> Result<Vec<u64>, cxx::KjError> {
        load_config(path, const_name, experimental, Some(self.watcher.clone()))
    }

    /// See the bridge's declaration.
    pub async fn changed(&self) -> Result<(), cxx::KjError> {
        self.watcher.wait_quietly_for_changes().await
    }
}

/// Loads the config at `path`, adding each file it reads to `watcher`'s, if any.
fn load_config(
    path: &str,
    const_name: &str,
    experimental: bool,
    watcher: Option<Watcher>,
) -> Result<Vec<u64>, cxx::KjError> {
    let failed = |message: String| cxx::KjError::new(cxx::KjExceptionType::Failed, message);
    if !Path::new(path).is_file() {
        return Err(failed(format!("{path}: No such file.")));
    }
    let process = Process::new(None, watcher, Vec::new());
    if wrangler::is_wrangler_config(path) {
        if !const_name.is_empty() {
            return Err(failed(format!(
                "{path}: A wrangler config has no named constants."
            )));
        }
        return wrangler::load(path, &process, experimental)
            .map(Config::into_words)
            .map_err(|error| failed(format!("{path}: {error}")));
    }
    let const_name = (!const_name.is_empty()).then_some(const_name);
    let compiled = ffi::compile_config(path, &[], const_name.into(), &process)
        .map_err(|error| failed(format!("{path}: {}", error.what())))?;
    if !compiled.errors.is_empty() {
        let errors: Vec<String> = compiled.errors.iter().map(ToString::to_string).collect();
        return Err(failed(errors.join("\n")));
    }
    Config::from_words(&compiled.message)
        .map(Config::into_words)
        .map_err(|error| failed(format!("{path}: {error}")))
}
