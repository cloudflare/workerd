// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The REPL's console, on a thread of its own: it reads input and has the session evaluate it.
//!
//! On a terminal, or with `--interactive` ([`Reading::Lines`]), it reads input a line at a time
//! until an input is finished, which then runs, and prints its result. On a terminal (Unix only)
//! it edits lines with [`Editor`], whose history starts with the history file's and is saved to it
//! ([`HistoryFile`]); otherwise it reads lines as they come, which is how a test drives it.
//! Results go to stdout, so that stdout holds only what the inputs produced, and the console's own
//! messages to stderr. So do its banner and prompts, but only when stdin is a terminal, where
//! someone types: a script that pipes input in sees only what went wrong on stderr.
//!
//! Otherwise ([`Reading::Script`]), stdin is one script, which runs once stdin ends, as `node`
//! runs a script piped to it. Nothing is printed but what the script prints and what fails.

use std::io;
use std::io::BufRead;
use std::io::IsTerminal;
use std::io::Read;
use std::io::Write;
use std::time::Duration;

use tokio::sync::mpsc;
use tokio::sync::oneshot;

#[cfg(unix)]
use super::editor::Completion;
#[cfg(unix)]
use super::editor::Editor;
#[cfg(unix)]
use super::editor::HISTORY_LIMIT;
#[cfg(unix)]
use super::editor::Line;
#[cfg(unix)]
use super::history::HistoryFile;
use super::input;
use super::input::Candidate;
use super::input::Scanner;
use super::input::Unclosed;
#[cfg(unix)]
use super::interrupt::AbandonOnInterrupt;
use super::session::Evaluated;
use super::session::Request;

const PROMPT: &str = "> ";
const CONTINUATION_PROMPT: &str = "... ";

const BANNER: &str =
    "workerd REPL (experimental). \".help\" for help; \".exit\" or Ctrl+D to quit.";

const HELP: &str = "\
.help     Show this help
.servers  List the loaded configs' servers (`workerd.servers`)
.clear    Clear the screen (also Ctrl+L while editing)
.break    Abandon the unfinished input being typed (also Ctrl+C while editing)
.exit     Exit the REPL (also Ctrl+D)

The `workerd` global loads configs and reaches their services:
  s = await workerd.loadConfig('config.capnp')    // { constName } picks the constant, and
                                                  // { watch: true } reloads it when it changes
  r = await s.service('main').fetch('http://x/')  // { entrypoint, props } pick an entrypoint
  await r.text()
  await s.service('main').scheduled({ cron: '* * * * *' })  // and queue(name, messages)
  await s.socket('http').fetch('http://x/')       // a request to an http socket of the config
  c = s.durableObject('main', 'Counter', 'alice') // a Durable Object, or by { id }
  await workerd.evict(c)                          // restart it on its next request
  e = await workerd.environment({ compatibilityDate: '2022-01-01' })  // { compatibilityFlags }
  await e.eval('typeof navigator')                // what the REPL would print there
  await s.reload()                                // serve the config as it is now
  await s.close()

In an input that awaits at top level, a `const`, `let` or `var` declaration that gives each of
its names a value, such as `const a = await f(), b = 2`, declares them at the top level ahead of
the input, so that later inputs see them; a `const` is declared with `let`. Other declarations
stay local to the input: one that destructures (assign instead, as in `({ a } = await f())`),
one that leaves a name without a value, one in a block.

`_` is the last result and `_error` the last exception, until you assign to them.

Up and Down recall earlier lines, which are saved in ~/.workerd_repl_history. Set
WORKERD_REPL_HISTORY to save them in another file, or to the empty string not to save them.

Ctrl+C abandons the input being typed. On Unix, while an input awaits, Ctrl+C stops awaiting
it; while JavaScript runs without awaiting, as a loop does, press Ctrl+C twice to end the REPL.
";

/// Whether to edit lines on a terminal: stdin and stderr, where the editor draws, are both
/// terminals.
fn edits_lines() -> bool {
    cfg!(unix) && io::stdin().is_terminal() && io::stderr().is_terminal()
}

/// How the console reads its input.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Reading {
    /// A line at a time, each input running once it is finished, and its result printed: on a
    /// terminal, or with `--interactive`.
    Lines,
    /// All of it, as one script that runs once stdin, which isn't a terminal, ends.
    Script,
}

/// Runs the console until the input ends or `.exit`, sending `requests` to the session; the
/// session ends when the console drops them. `first_input`, if any, runs first: before the first
/// prompt, shown as if typed at it when prompting, or before the script, which doesn't run if it
/// fails. Whether an input failed, or reading the input did.
pub fn run(
    requests: mpsc::UnboundedSender<Request>,
    first_input: Option<String>,
    reading: Reading,
) -> bool {
    let mut console = Console {
        requests,
        input: String::new(),
        scanner: Scanner::default(),
        failed: false,
        prompting: io::stdin().is_terminal(),
    };
    match reading {
        Reading::Lines => console.run_lines(first_input),
        Reading::Script => console.run_script(first_input),
    }
    console.failed
}

/// Whether the console continues after an input.
#[derive(PartialEq, Eq)]
enum Next {
    Continue,
    Exit,
}

struct Console {
    requests: mpsc::UnboundedSender<Request>,
    /// The lines of the unfinished input, and the scanner that has read them.
    input: String,
    scanner: Scanner,
    /// An input failed: it threw, timed out, or was left unfinished, the session could not
    /// evaluate it, or it is no command.
    failed: bool,
    /// Show the banner and prompts: stdin is a terminal.
    prompting: bool,
}

impl Console {
    fn run_lines(&mut self, first_input: Option<String>) {
        if self.prompting {
            eprintln!("{BANNER}");
        }
        if let Some(input) = first_input {
            if self.prompting {
                eprintln!("{PROMPT}{input}");
            }
            if self.evaluate(&input) == Next::Exit {
                return;
            }
        }
        let result = if edits_lines() {
            self.run_editing()
        } else {
            self.run_basic()
        };
        if let Err(error) = result {
            self.fail(&format!("reading input: {error}"));
        }
        self.end_input();
    }

    fn run_script(&mut self, first_input: Option<String>) {
        if let Some(input) = first_input {
            self.run_quietly(input::candidates(&input));
            if self.failed {
                return;
            }
        }
        let mut script = Vec::new();
        if let Err(error) = io::stdin().read_to_end(&mut script) {
            self.fail(&format!("reading input: {error}"));
            return;
        }
        self.run_quietly(input::script_candidates(&String::from_utf8_lossy(&script)));
    }

    fn prompt(&self) -> &'static str {
        if self.input.is_empty() {
            PROMPT
        } else {
            CONTINUATION_PROMPT
        }
    }

    #[cfg(unix)]
    fn run_editing(&mut self) -> io::Result<()> {
        let requests = self.requests.clone();
        let mut complete = |line: &str, cursor: usize| complete(&requests, line, cursor);
        let mut editor = Editor::new();
        let mut history = HistoryFile::from_env();
        if let Some(file) = &history {
            match file.load(HISTORY_LIMIT) {
                Ok(entries) => {
                    for entry in &entries {
                        editor.add_history(entry);
                    }
                }
                Err(error) => {
                    not_saving_history(file, &error);
                    history = None;
                }
            }
        }
        loop {
            let line = {
                let mut terminal = super::terminal::RawTerminal::enter()?;
                editor.read_line(&mut terminal, self.prompt(), &mut complete)?
            };
            match line {
                Line::Submitted(line) => {
                    if editor.add_history(&line) {
                        save(&mut history, &line);
                    }
                    if self.line(&line) == Next::Exit {
                        return Ok(());
                    }
                }
                Line::Canceled => self.abandon_input(),
                Line::Ended => return Ok(()),
            }
        }
    }

    #[cfg(not(unix))]
    fn run_editing(&mut self) -> io::Result<()> {
        unreachable!("edits_lines() is false off Unix")
    }

    fn run_basic(&mut self) -> io::Result<()> {
        // Stdin's buffer outlives each lock, so locking per line loses no input.
        let stdin = io::stdin();
        let mut line = Vec::new();
        loop {
            if self.prompting {
                prompt(self.prompt());
            }
            line.clear();
            if stdin.lock().read_until(b'\n', &mut line)? == 0 {
                if self.prompting {
                    // End the prompt's line, for the shell's next prompt.
                    eprintln!();
                }
                return Ok(());
            }
            let text = String::from_utf8_lossy(&line);
            let text = text.strip_suffix('\n').unwrap_or(&text);
            let text = text.strip_suffix('\r').unwrap_or(text);
            if self.line(text) == Next::Exit {
                return Ok(());
            }
        }
    }

    /// Adds `line` to the input, and runs the input once it is finished. A line that starts an
    /// input with a `.` and a letter is a command, and so is `.break` on a line of its own in an
    /// unfinished input.
    fn line(&mut self, line: &str) -> Next {
        if self.input.is_empty() {
            if let Some(command) = command(line) {
                return self.command(command);
            }
        } else if line.trim() == ".break" {
            self.abandon_input();
            return Next::Continue;
        }
        if !self.input.is_empty() {
            self.input.push('\n');
        }
        self.input.push_str(line);
        self.scanner.scan(&format!("{line}\n"));
        if self.scanner.is_unfinished() {
            return Next::Continue;
        }
        let input = std::mem::take(&mut self.input);
        self.scanner = Scanner::default();
        if input.trim().is_empty() {
            return Next::Continue;
        }
        self.evaluate(&input)
    }

    /// Drops the lines of the unfinished input.
    fn abandon_input(&mut self) {
        self.input.clear();
        self.scanner = Scanner::default();
    }

    /// Reports the input left unfinished when the console's input ended, if any; it doesn't run.
    fn end_input(&mut self) {
        if let Some(Unclosed { what, line }) = self.scanner.unclosed() {
            self.fail(&format!(
                "The input ended inside the {what} opened on its line {line}, so it did not run."
            ));
        }
    }

    fn command(&mut self, command: &str) -> Next {
        match command {
            ".help" => eprint!("{HELP}"),
            // There is no unfinished input to abandon.
            ".break" => {}
            ".servers" => return self.evaluate("workerd.servers"),
            ".clear" => {
                if io::stderr().is_terminal() {
                    eprint!("\x1b[2J\x1b[3J\x1b[H");
                }
            }
            ".exit" => return Next::Exit,
            _ => {
                eprintln!("Unknown command: {command} (try .help)");
                self.failed = true;
            }
        }
        Next::Continue
    }

    /// Prints what the session makes of `input`. The console ends if the session has.
    fn evaluate(&mut self, input: &str) -> Next {
        let (abandon, abandoned) = oneshot::channel();
        // On Unix, Ctrl+C abandons the input while it awaits. Elsewhere it ends the REPL.
        #[cfg(unix)]
        let _interrupt = AbandonOnInterrupt::new(abandon);
        #[cfg(not(unix))]
        drop(abandon);
        let Some(evaluated) = self.send(input::candidates(input), abandoned) else {
            return Next::Exit;
        };
        match evaluated {
            Ok(Evaluated::Printed { text, threw }) => {
                self.failed |= threw;
                let mut stdout = io::stdout().lock();
                // A closed stdout leaves nothing to print to; the inputs may still have effects.
                let _ = writeln!(stdout, "{text}").and_then(|()| stdout.flush());
            }
            Ok(Evaluated::Abandoned) => {
                eprintln!(
                    "Interrupted: no longer awaiting the input, whose promise may still settle."
                );
            }
            Ok(Evaluated::TimedOut(timeout)) => self.fail(&timed_out(timeout)),
            Err(error) => self.fail(&error),
        }
        Next::Continue
    }

    /// Runs the input `candidates` compile, and prints only its exception, or its failure, to
    /// stderr. Nothing abandons it, so Ctrl+C ends the REPL meanwhile, as it ends `node`.
    fn run_quietly(&mut self, candidates: Vec<Candidate>) {
        // Dropped at once, the sender never abandons the input.
        let (_, abandoned) = oneshot::channel();
        match self.send(candidates, abandoned) {
            Some(Ok(Evaluated::Printed { text, threw: true })) => {
                eprintln!("{text}");
                self.failed = true;
            }
            // The session has ended if there is no reply; the REPL reports why.
            Some(Ok(Evaluated::Printed { threw: false, .. } | Evaluated::Abandoned)) | None => {}
            Some(Ok(Evaluated::TimedOut(timeout))) => self.fail(&timed_out(timeout)),
            Some(Err(error)) => self.fail(&error),
        }
    }

    /// What the session makes of the input `candidates` compile, which a send on `abandon`
    /// abandons; `None` if the session has ended.
    fn send(
        &self,
        candidates: Vec<Candidate>,
        abandon: oneshot::Receiver<()>,
    ) -> Option<Result<Evaluated, String>> {
        let (reply, result) = oneshot::channel();
        let request = Request::Evaluate {
            candidates,
            abandon,
            reply,
        };
        self.requests.send(request).ok()?;
        result.blocking_recv().ok()
    }

    /// Reports `message`, which says how an input failed or reading the input did.
    fn fail(&mut self, message: &str) {
        eprintln!("Error: {message}");
        self.failed = true;
    }
}

/// The message of an input that timed out after `timeout`.
fn timed_out(timeout: Duration) -> String {
    format!(
        "Timed out after {timeout:?}: no longer awaiting the input, whose promise may still settle."
    )
}

/// Appends `line` to the history file, if the session has one. A file that fails is warned about
/// once, and the session saves no more history.
#[cfg(unix)]
fn save(history: &mut Option<HistoryFile>, line: &str) {
    if let Some(file) = history
        && let Err(error) = file.record(line)
    {
        not_saving_history(file, &error);
        *history = None;
    }
}

#[cfg(unix)]
fn not_saving_history(file: &HistoryFile, error: &io::Error) {
    let path = file.path().display();
    eprintln!("Warning: not saving history: {path}: {error}");
}

/// The command a line is, if any: `.help` but not `.5`.
fn command(line: &str) -> Option<&str> {
    let line = line.trim();
    let mut chars = line.chars();
    (chars.next() == Some('.') && chars.next().is_some_and(|c| c.is_ascii_alphabetic()))
        .then_some(line)
}

fn prompt(prompt: &str) {
    let mut stderr = io::stderr().lock();
    let _ = stderr
        .write_all(prompt.as_bytes())
        .and_then(|()| stderr.flush());
}

/// Has the session complete the name before `cursor`; nothing if the session has ended.
#[cfg(unix)]
fn complete(requests: &mpsc::UnboundedSender<Request>, line: &str, cursor: usize) -> Completion {
    let (reply, completion) = oneshot::channel();
    let request = Request::Complete {
        line: line.to_owned(),
        cursor,
        reply,
    };
    if requests.send(request).is_ok()
        && let Ok(completion) = completion.blocking_recv()
    {
        return completion;
    }
    Completion {
        start: cursor,
        candidates: Vec::new(),
    }
}
