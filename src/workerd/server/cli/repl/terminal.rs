// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The terminal the line editor edits on: stdin in raw mode, and stderr, where the REPL writes
//! everything but results.

#![allow(
    unsafe_code,
    reason = "termios and poll on the stdin descriptor, which std does not expose"
)]

use std::io;
use std::io::Write;
use std::mem::MaybeUninit;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;
use std::time::Duration;

use super::editor::Terminal;

/// The terminal in raw mode, while editing a line: input arrives a key at a time, without echo,
/// and Ctrl+C, Ctrl+Z and Ctrl+S arrive as keys rather than as signals and flow control. Output
/// is still processed, so a line break returns the carriage too. The terminal is also asked to
/// mark pasted text (bracketed paste mode), which a terminal without the mode ignores. Dropping
/// it restores the mode the terminal was in, so that Ctrl+C interrupts while input runs, and
/// what an input prints, or a program after the REPL, is not marked as pasted. A signal that ends
/// the process restores it first ([`restore_for_exit`]). At most one exists at a time.
pub struct RawTerminal(());

impl RawTerminal {
    pub fn enter() -> io::Result<Self> {
        {
            let mut saved_mode = saved_mode();
            let mut saved = MaybeUninit::<libc::termios>::uninit();
            // SAFETY: tcgetattr writes the whole struct on success, the only case it is read.
            let saved = unsafe {
                if libc::tcgetattr(libc::STDIN_FILENO, saved.as_mut_ptr()) != 0 {
                    return Err(io::Error::last_os_error());
                }
                saved.assume_init()
            };
            let mut raw = saved;
            raw.c_lflag &= !(libc::ECHO | libc::ICANON | libc::IEXTEN | libc::ISIG);
            raw.c_iflag &= !(libc::IXON | libc::ICRNL);
            raw.c_cc[libc::VMIN] = 1;
            raw.c_cc[libc::VTIME] = 0;
            set_mode(&raw)?;
            *saved_mode = Some(saved);
        }
        let mut terminal = Self(());
        terminal.write(BRACKETED_PASTE_ON)?;
        Ok(terminal)
    }
}

const BRACKETED_PASTE_ON: &str = "\x1b[?2004h";
const BRACKETED_PASTE_OFF: &str = "\x1b[?2004l";

impl Drop for RawTerminal {
    fn drop(&mut self) {
        // Nothing can be done about a terminal that refuses its own mode back.
        let _ = self.write(BRACKETED_PASTE_OFF);
        let mut saved_mode = saved_mode();
        if let Some(saved) = saved_mode.take() {
            let _ = set_mode(&saved);
        }
    }
}

/// The mode the terminal was in before a [`RawTerminal`] put it in raw mode; none while it is not
/// in raw mode. Locked while the mode changes, so that [`restore_for_exit`] finds the terminal in
/// the mode this says, and only for calls that don't wait, so that it doesn't wait long for the
/// lock.
static SAVED_MODE: Mutex<Option<libc::termios>> = Mutex::new(None);

fn saved_mode() -> MutexGuard<'static, Option<libc::termios>> {
    SAVED_MODE.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Keeps the terminal out of raw mode, for a signal that ends the process: [`restore_for_exit`].
pub struct RestoredForExit {
    _saved_mode: MutexGuard<'static, Option<libc::termios>>,
}

/// Takes the terminal out of raw mode, if a [`RawTerminal`] has it in raw mode, for a signal that
/// is about to end the process. Keeps it out until the result is dropped, which the caller does
/// only if the process goes on. Unlike dropping the [`RawTerminal`], doesn't wait on a terminal
/// that has stopped taking output, or on a thread whose write to one holds the lock of
/// [`io::stderr()`].
pub fn restore_for_exit() -> RestoredForExit {
    let mut saved_mode = saved_mode();
    if let Some(saved) = saved_mode.take() {
        let _ = set_mode(&saved);
        write_unless_stalled(BRACKETED_PASTE_OFF);
    }
    RestoredForExit {
        _saved_mode: saved_mode,
    }
}

/// How long [`write_unless_stalled`] waits for the terminal to take output.
const STALLED_AFTER_MILLIS: libc::c_int = 100;

/// Writes `output` to stderr unless the terminal is not ready to take it within
/// [`STALLED_AFTER_MILLIS`]. Bypasses [`io::stderr()`], whose lock a thread may hold while it
/// waits on the terminal.
fn write_unless_stalled(output: &str) {
    let mut poll = libc::pollfd {
        fd: libc::STDERR_FILENO,
        events: libc::POLLOUT,
        revents: 0,
    };
    // SAFETY: `poll` is one valid pollfd, and `output` is readable for its length.
    unsafe {
        if libc::poll(&raw mut poll, 1, STALLED_AFTER_MILLIS) == 1 {
            libc::write(libc::STDERR_FILENO, output.as_ptr().cast(), output.len());
        }
    }
}

/// Sets the terminal's mode at once. Not TCSADRAIN, which waits for output to drain first: raw
/// mode doesn't change how output is processed, and a terminal that has stopped taking output
/// would never drain. Not TCSAFLUSH, which would discard what was typed while an input ran.
fn set_mode(mode: &libc::termios) -> io::Result<()> {
    // SAFETY: `mode` is a valid termios for the duration of the call.
    if unsafe { libc::tcsetattr(libc::STDIN_FILENO, libc::TCSANOW, mode) } != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

impl Terminal for RawTerminal {
    fn read(&mut self, timeout: Option<Duration>) -> io::Result<Option<Vec<u8>>> {
        let timeout = timeout.map_or(-1, |t| i32::try_from(t.as_millis()).unwrap_or(i32::MAX));
        let mut poll = libc::pollfd {
            fd: libc::STDIN_FILENO,
            events: libc::POLLIN,
            revents: 0,
        };
        loop {
            // SAFETY: `poll` is one valid pollfd.
            match unsafe { libc::poll(&raw mut poll, 1, timeout) } {
                0 => return Ok(Some(Vec::new())),
                n if n > 0 => break,
                _ => {
                    let error = io::Error::last_os_error();
                    if error.kind() != io::ErrorKind::Interrupted {
                        return Err(error);
                    }
                }
            }
        }
        let mut buffer = vec![0_u8; 4096];
        loop {
            // SAFETY: `buffer` is writable for its length.
            let n =
                unsafe { libc::read(libc::STDIN_FILENO, buffer.as_mut_ptr().cast(), buffer.len()) };
            match usize::try_from(n) {
                Ok(0) => return Ok(None),
                Ok(n) => {
                    buffer.truncate(n);
                    return Ok(Some(buffer));
                }
                Err(_) => {
                    let error = io::Error::last_os_error();
                    if error.kind() != io::ErrorKind::Interrupted {
                        return Err(error);
                    }
                }
            }
        }
    }

    fn write(&mut self, output: &str) -> io::Result<()> {
        let mut stderr = io::stderr().lock();
        stderr.write_all(output.as_bytes())?;
        stderr.flush()
    }
}
