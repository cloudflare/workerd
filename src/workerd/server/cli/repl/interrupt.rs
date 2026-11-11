// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Ctrl+C while an input runs. The terminal is out of raw mode then (terminal.rs), so Ctrl+C
//! raises SIGINT. While an [`AbandonOnInterrupt`] is held, the first SIGINT abandons the input
//! rather than ending the process: the session stops awaiting it. A second comes only if the
//! session could not abandon the input, because its JavaScript runs without yielding, and ends the
//! process as SIGINT would have.
//!
//! The handler only writes a byte to a socket; a thread of its own reads the socket and does what
//! a handler may not.

#![allow(
    unsafe_code,
    reason = "sigaction, and write() and errno in the signal handler, which std does not expose"
)]

use std::io;
use std::io::Read;
use std::mem::MaybeUninit;
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::OnceLock;
use std::sync::PoisonError;
use std::sync::atomic::AtomicI32;
use std::sync::atomic::Ordering;

use tokio::sync::oneshot;

/// What a SIGINT does.
enum State {
    /// Nothing: no input runs. A SIGINT read now arrived while the last one ran.
    Idle,
    /// Abandons the input that runs.
    Running(oneshot::Sender<()>),
    /// Ends the process: the input was abandoned and still runs.
    Abandoning,
}

static STATE: Mutex<State> = Mutex::new(State::Idle);

/// The socket the handler writes to, once the relay has started; an error if it could not.
static RELAY: OnceLock<io::Result<UnixStream>> = OnceLock::new();

/// The descriptor of [`RELAY`]'s socket, for the handler.
static RELAY_FD: AtomicI32 = AtomicI32::new(-1);

/// SIGINT abandons an input while this is held.
pub struct AbandonOnInterrupt {
    /// The SIGINT disposition to restore; none if the handler is not installed.
    previous: Option<libc::sigaction>,
}

impl AbandonOnInterrupt {
    /// Until dropped, the first SIGINT sends on `abandon`. If the relay cannot start, SIGINT ends
    /// the process as it would otherwise.
    pub fn new(abandon: oneshot::Sender<()>) -> Self {
        if RELAY.get_or_init(start_relay).is_err() {
            return Self { previous: None };
        }
        *state() = State::Running(abandon);
        Self {
            previous: set_handler(on_sigint as extern "C" fn(libc::c_int) as libc::sighandler_t),
        }
    }
}

impl Drop for AbandonOnInterrupt {
    fn drop(&mut self) {
        if let Some(previous) = self.previous.take() {
            // SAFETY: `previous` is the disposition sigaction() reported.
            unsafe {
                libc::sigaction(libc::SIGINT, &raw const previous, std::ptr::null_mut());
            }
        }
        *state() = State::Idle;
    }
}

fn state() -> MutexGuard<'static, State> {
    STATE.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Installs `handler` for SIGINT; the disposition it replaced, unless that fails.
fn set_handler(handler: libc::sighandler_t) -> Option<libc::sigaction> {
    // SAFETY: an all-zero sigaction is valid (no flags, an empty mask) before its fields are set;
    // sigaction() fills in `previous` when it succeeds, the only case in which it is read.
    unsafe {
        let mut action: libc::sigaction = std::mem::zeroed();
        action.sa_sigaction = handler;
        // The blocking calls of the threads it lands on go on rather than failing with EINTR.
        action.sa_flags = libc::SA_RESTART;
        libc::sigemptyset(&raw mut action.sa_mask);
        let mut previous = MaybeUninit::<libc::sigaction>::uninit();
        if libc::sigaction(libc::SIGINT, &raw const action, previous.as_mut_ptr()) != 0 {
            return None;
        }
        Some(previous.assume_init())
    }
}

fn start_relay() -> io::Result<UnixStream> {
    let (mut reader, writer) = UnixStream::pair()?;
    // A handler must not block. A full socket holds unread bytes, which already stand for the
    // signal whose byte is dropped.
    writer.set_nonblocking(true)?;
    std::thread::Builder::new()
        .name("repl-interrupts".to_owned())
        .spawn(move || {
            let mut byte = [0_u8; 1];
            // The writer is never closed, so reading fails only if the socket does.
            loop {
                match reader.read(&mut byte) {
                    Ok(0) => return,
                    Ok(_) => interrupted(),
                    Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                    Err(_) => return,
                }
            }
        })?;
    RELAY_FD.store(writer.as_raw_fd(), Ordering::Relaxed);
    Ok(writer)
}

extern "C" fn on_sigint(_signal: libc::c_int) {
    // SAFETY: write() is async-signal-safe, and the descriptor is the relay's socket, which is
    // never closed. errno is the interrupted code's, so it is restored.
    unsafe {
        let errno = *errno_location();
        libc::write(RELAY_FD.load(Ordering::Relaxed), [0_u8].as_ptr().cast(), 1);
        *errno_location() = errno;
    }
}

#[cfg(any(target_os = "linux", target_os = "android"))]
unsafe fn errno_location() -> *mut libc::c_int {
    // SAFETY: the caller's thread's errno.
    unsafe { libc::__errno_location() }
}

#[cfg(not(any(target_os = "linux", target_os = "android")))]
unsafe fn errno_location() -> *mut libc::c_int {
    // SAFETY: the caller's thread's errno.
    unsafe { libc::__error() }
}

/// What a SIGINT does, on the relay's thread.
fn interrupted() {
    let mut state = state();
    match std::mem::replace(&mut *state, State::Abandoning) {
        State::Running(abandon) => {
            // A session that has replied meanwhile has nothing to abandon.
            let _ = abandon.send(());
        }
        State::Abandoning => {
            // SAFETY: with the default disposition, raising SIGINT ends the process, as the
            // signal would have ended it without the handler.
            unsafe {
                libc::signal(libc::SIGINT, libc::SIG_DFL);
                libc::raise(libc::SIGINT);
            }
        }
        State::Idle => *state = State::Idle,
    }
}
