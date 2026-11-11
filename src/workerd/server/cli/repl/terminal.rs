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
use std::time::Duration;

use super::editor::Terminal;

/// The terminal in raw mode, while editing a line: input arrives a key at a time, without echo,
/// and Ctrl+C, Ctrl+Z and Ctrl+S arrive as keys rather than as signals and flow control. Output
/// is still processed, so a line break returns the carriage too. Dropping it restores the mode
/// the terminal was in, so that Ctrl+C interrupts while input runs.
pub struct RawTerminal {
    saved: libc::termios,
}

impl RawTerminal {
    pub fn enter() -> io::Result<Self> {
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
        // TCSADRAIN rather than TCSAFLUSH, which would discard what was typed while an input ran.
        set_mode(&raw)?;
        Ok(Self { saved })
    }
}

impl Drop for RawTerminal {
    fn drop(&mut self) {
        // Nothing can be done about a terminal that refuses its own mode back.
        let _ = set_mode(&self.saved);
    }
}

fn set_mode(mode: &libc::termios) -> io::Result<()> {
    // SAFETY: `mode` is a valid termios for the duration of the call.
    if unsafe { libc::tcsetattr(libc::STDIN_FILENO, libc::TCSADRAIN, mode) } != 0 {
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
