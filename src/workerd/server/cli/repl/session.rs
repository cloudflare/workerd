// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The REPL's session loop, which runs on the event loop's thread and serves the console's
//! requests one at a time, and the config loading its servers use (server.rs).

use std::path::Path;
use std::pin::Pin;
use std::pin::pin;
use std::task::Poll;
use std::time::Duration;

use futures::FutureExt;
use futures::future::LocalBoxFuture;
use kj_rs::KjOwn;
use tokio::sync::mpsc;
use tokio::sync::oneshot;
use workerd_server::run::RunOptions;

use super::bridge;
use super::bridge::ffi::ReplCandidate;
use super::bridge::ffi::ReplSession;
use super::editor::Completion;
use super::input;
use super::server;
use crate::bridge::ffi;
use crate::config::Config;
use crate::process::Process;
use crate::watch::Watcher;
use crate::wrangler;

/// The most names tab completion offers.
const MAX_COMPLETIONS: usize = 200;

/// What the console asks of the session.
pub enum Request {
    /// What became of the input `candidates` compile (input.rs); an error if the session failed to
    /// evaluate it. A send on `abandon` while the input awaits abandons it.
    Evaluate {
        candidates: Vec<ReplCandidate>,
        abandon: oneshot::Receiver<()>,
        reply: oneshot::Sender<Result<Evaluated, String>>,
    },
    /// Tab completion in `line` with the cursor at byte offset `cursor`.
    Complete {
        line: String,
        cursor: usize,
        reply: oneshot::Sender<Completion>,
    },
}

/// What became of an input.
pub enum Evaluated {
    /// The text to print: the rendered result, or, if the input `threw`, "Uncaught " followed by
    /// the exception.
    Printed { text: String, threw: bool },
    /// The session stopped awaiting the input. Its promise and what the input started go on.
    Abandoned,
    /// The session stopped awaiting the input once it had taken this long, as for
    /// [`Self::Abandoned`].
    TimedOut(Duration),
}

/// Serves `requests` until the console drops its sender, then drains the session; whether anything
/// failed in the background (see [`bridge::drain`]). An input that takes longer than `timeout`
/// times out.
pub async fn serve(
    mut session: KjOwn<ReplSession>,
    mut requests: mpsc::UnboundedReceiver<Request>,
    timeout: Option<Duration>,
) -> Result<bool, cxx::KjError> {
    while let Some(request) = requests.recv().await {
        // A console that gave up on the reply has ended; its next `recv` sees that.
        match request {
            Request::Evaluate {
                candidates,
                abandon,
                reply,
            } => {
                let evaluated = evaluate(session.as_mut(), candidates, abandon, timeout).await;
                let _ = reply.send(evaluated);
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

/// Evaluates the input `candidates` compile, unless abandoned or timed out first. Either drops the
/// evaluation, which stops awaiting the input's promise. The timer runs only while the event loop
/// does, so it can't time out JavaScript that runs without awaiting.
async fn evaluate(
    session: Pin<&mut ReplSession>,
    candidates: Vec<ReplCandidate>,
    abandon: oneshot::Receiver<()>,
    timeout: Option<Duration>,
) -> Result<Evaluated, String> {
    let mut evaluation = pin!(bridge::evaluate(session, candidates));
    // None once the console has dropped its sender without sending: it never abandons then.
    let mut abandon = Some(abandon);
    let mut deadline = pin!(timeout.map(tokio::time::sleep));
    std::future::poll_fn(|cx| {
        if let Poll::Ready(result) = evaluation.as_mut().poll(cx) {
            return Poll::Ready(
                result
                    .map(|evaluation| Evaluated::Printed {
                        text: evaluation.text,
                        threw: evaluation.threw,
                    })
                    .map_err(|error| error.description().to_owned()),
            );
        }
        if let Some(receiver) = abandon.as_mut() {
            match Pin::new(receiver).poll(cx) {
                Poll::Ready(Ok(())) => return Poll::Ready(Ok(Evaluated::Abandoned)),
                Poll::Ready(Err(_)) => abandon = None,
                Poll::Pending => {}
            }
        }
        if let (Some(timeout), Some(deadline)) = (timeout, deadline.as_mut().as_pin_mut())
            && deadline.poll(cx).is_ready()
        {
            return Poll::Ready(Ok(Evaluated::TimedOut(timeout)));
        }
        Poll::Pending
    })
    .await
}

/// The global or property names that complete the name before the cursor.
async fn complete(session: Pin<&mut ReplSession>, line: &str, cursor: usize) -> Completion {
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

/// Whether a loaded config may use experimental features: `--experimental`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Features {
    Stable,
    Experimental,
}

/// Loads configs as `workerd serve` does. A config's problems are returned, never printed, since
/// the REPL goes on after a config fails to load.
pub struct CliLoader {
    pub features: Features,
}

impl server::Loader for CliLoader {
    fn load(&self, path: &str, const_name: Option<&str>) -> Result<Vec<u64>, String> {
        load_config(path, const_name, self.features, None)
    }

    fn watch(&self) -> Result<Box<dyn server::Watch>, String> {
        let watcher = Watcher::new().map_err(|error| error.to_string())?;
        Ok(Box::new(CliWatch {
            watcher,
            features: self.features,
        }))
    }
}

/// The files configs were loaded from, watched by the CLI's watcher (watch.rs).
struct CliWatch {
    watcher: Watcher,
    features: Features,
}

impl server::Watch for CliWatch {
    fn load(&self, path: &str, const_name: Option<&str>) -> Result<Vec<u64>, String> {
        load_config(path, const_name, self.features, Some(self.watcher.clone()))
    }

    fn changed(&self) -> LocalBoxFuture<'static, Result<(), String>> {
        let changed = self.watcher.wait_quietly_for_changes();
        async move {
            changed
                .await
                .map_err(|error| error.description().to_owned())
        }
        .boxed_local()
    }
}

/// Loads the config at `path`, a Cap'n Proto config (with the constant `const_name`, if any) or a
/// `wrangler.json(c)`, as an encoded `Config` message, adding each file it reads to `watcher`'s, if
/// any. The error lists every problem.
fn load_config(
    path: &str,
    const_name: Option<&str>,
    features: Features,
    watcher: Option<Watcher>,
) -> Result<Vec<u64>, String> {
    if !Path::new(path).is_file() {
        return Err(format!("{path}: No such file."));
    }
    let process = Process::new(None, watcher, Vec::new());
    if wrangler::is_wrangler_config(path) {
        if const_name.is_some() {
            return Err(format!("{path}: A wrangler config has no named constants."));
        }
        let experimental = features == Features::Experimental;
        return wrangler::load(path, &process, &RunOptions::default(), None, experimental)
            .map(Config::into_words)
            .map_err(|error| format!("{path}: {error}"));
    }
    let compiled = ffi::compile_config(path, &[], const_name.into(), &process)
        .map_err(|error| format!("{path}: {}", error.what()))?;
    if !compiled.errors.is_empty() {
        let errors: Vec<String> = compiled.errors.iter().map(ToString::to_string).collect();
        return Err(errors.join("\n"));
    }
    Config::from_words(&compiled.message)
        .map(Config::into_words)
        .map_err(|error| format!("{path}: {error}"))
}

#[cfg(test)]
#[path = "session-test.rs"]
mod tests;
