// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The REPL's console, on a thread of its own: it reads input, a line at a time, until an input
//! is finished, and has the session evaluate it. On a terminal (Unix only) it edits lines with
//! [`Editor`]; otherwise it reads them as they come, which is how a test drives it. Prompts and
//! the console's own messages go to stderr and results to stdout, so that stdout holds only what
//! the inputs produced.

use std::io;
use std::io::BufRead;
use std::io::IsTerminal;
use std::io::Write;

use tokio::sync::mpsc;
use tokio::sync::oneshot;

#[cfg(unix)]
use super::editor::Completion;
#[cfg(unix)]
use super::editor::Editor;
#[cfg(unix)]
use super::editor::Line;
use super::input::Scanner;
use super::session::Request;

const PROMPT: &str = "> ";
const CONTINUATION_PROMPT: &str = "... ";

const BANNER: &str =
    "workerd REPL (experimental). \".help\" for help; \".exit\" or Ctrl+D to quit.";

const HELP: &str = "\
.help     Show this help
.servers  List the loaded configs' servers (`workerd.servers`)
.clear    Clear the screen (also Ctrl+L while editing)
.exit     Exit the REPL (also Ctrl+D)

The `workerd` global loads configs and reaches their services:
  s = await workerd.loadConfig('config.capnp')    // { constName } picks the constant
  r = await s.service('main').fetch('http://x/')  // { entrypoint, props } pick an entrypoint
  await r.text()
  await s.socket('http').fetch('http://x/')       // a request to an http socket of the config
  await s.close()

In an input that awaits at top level, a `const`, `let` or `var` declaration of a single name
becomes an assignment to a global, so that later inputs see it. A destructuring declaration
does not; assign instead, as in `({ a } = await f())`.

Ctrl+C abandons the input being typed. While an input runs, Ctrl+C ends the REPL.
";

/// Whether to edit lines on a terminal: stdin and stderr, where the editor draws, are both
/// terminals.
fn is_interactive() -> bool {
    cfg!(unix) && io::stdin().is_terminal() && io::stderr().is_terminal()
}

/// Runs the console until the input ends or `.exit`, sending `requests` to the session; the
/// session ends when the console drops them.
pub fn run(requests: mpsc::UnboundedSender<Request>) {
    eprintln!("{BANNER}");
    let mut console = Console {
        requests,
        input: String::new(),
        scanner: Scanner::default(),
    };
    let result = if is_interactive() {
        console.run_interactive()
    } else {
        console.run_basic()
    };
    if let Err(error) = result {
        eprintln!("Error: reading input: {error}");
    }
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
}

impl Console {
    fn prompt(&self) -> &'static str {
        if self.input.is_empty() {
            PROMPT
        } else {
            CONTINUATION_PROMPT
        }
    }

    #[cfg(unix)]
    fn run_interactive(&mut self) -> io::Result<()> {
        let requests = self.requests.clone();
        let mut complete = |line: &str, cursor: usize| complete(&requests, line, cursor);
        let mut editor = Editor::new();
        loop {
            let line = {
                let mut terminal = super::terminal::RawTerminal::enter()?;
                editor.read_line(&mut terminal, self.prompt(), &mut complete)?
            };
            match line {
                Line::Submitted(line) => {
                    editor.add_history(&line);
                    if self.line(&line) == Next::Exit {
                        return Ok(());
                    }
                }
                Line::Canceled => {
                    self.input.clear();
                    self.scanner = Scanner::default();
                }
                Line::Ended => return Ok(()),
            }
        }
    }

    #[cfg(not(unix))]
    fn run_interactive(&mut self) -> io::Result<()> {
        unreachable!("is_interactive() is false off Unix")
    }

    fn run_basic(&mut self) -> io::Result<()> {
        // Stdin's buffer outlives each lock, so locking per line loses no input.
        let stdin = io::stdin();
        let mut line = Vec::new();
        loop {
            prompt(self.prompt());
            line.clear();
            if stdin.lock().read_until(b'\n', &mut line)? == 0 {
                // End the prompt's line, for the shell's next prompt.
                eprintln!();
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
    /// input with a `.` and a letter is a command.
    fn line(&mut self, line: &str) -> Next {
        if self.input.is_empty()
            && let Some(command) = command(line)
        {
            return self.command(command);
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
        self.evaluate(input)
    }

    fn command(&self, command: &str) -> Next {
        match command {
            ".help" => eprint!("{HELP}"),
            ".servers" => return self.evaluate("workerd.servers".to_owned()),
            ".clear" => {
                if io::stderr().is_terminal() {
                    eprint!("\x1b[2J\x1b[3J\x1b[H");
                }
            }
            ".exit" => return Next::Exit,
            _ => eprintln!("Unknown command: {command} (try .help)"),
        }
        Next::Continue
    }

    /// Prints what the session makes of `input`. The console ends if the session has.
    fn evaluate(&self, input: String) -> Next {
        let (reply, result) = oneshot::channel();
        if self
            .requests
            .send(Request::Evaluate { input, reply })
            .is_err()
        {
            return Next::Exit;
        }
        match result.blocking_recv() {
            Ok(Ok(text)) => {
                let mut stdout = io::stdout().lock();
                // A closed stdout leaves nothing to print to; the inputs may still have effects.
                let _ = writeln!(stdout, "{text}").and_then(|()| stdout.flush());
            }
            Ok(Err(error)) => eprintln!("Error: {error}"),
            Err(_) => return Next::Exit,
        }
        Next::Continue
    }
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
