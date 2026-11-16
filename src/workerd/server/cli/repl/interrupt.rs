// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Ctrl+C while an input runs. The terminal is out of raw mode then (terminal.rs), so Ctrl+C
//! raises SIGINT. While an [`AbandonOnInterrupt`] is held, the first SIGINT abandons the input
//! rather than ending the process: the session stops awaiting it. A second comes only if the
//! session could not abandon the input, because its JavaScript runs without yielding, and ends the
//! process as SIGINT does by default, as does a SIGINT while no input runs. A process that ignores
//! SIGINT when the REPL starts goes on ignoring it.
//!
//! [`take_interrupts`] blocks SIGINT before the REPL starts any other thread, so that every thread
//! blocks it, and starts a thread that takes each one with `sigwait()`. A SIGINT is taken at once
//! whatever the other threads are doing. A handler would instead run on a thread the kernel picks,
//! often the one running JavaScript, and `ThreadSanitizer` defers a handler until the thread it
//! interrupted calls into the C library, which JavaScript running without yielding never does.

#![allow(
    unsafe_code,
    reason = "sigaction(), pthread_sigmask(), sigwait() and raise(), which std does not expose"
)]

use std::mem::MaybeUninit;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;

use tokio::sync::oneshot;

/// What a SIGINT does.
enum State {
    /// Ends the process: no input runs.
    Idle,
    /// Abandons the input that runs.
    Running(oneshot::Sender<()>),
    /// Ends the process: the input was abandoned and still runs.
    Abandoning,
}

static STATE: Mutex<State> = Mutex::new(State::Idle);

/// Whether a thread of [`take_interrupts`] takes SIGINT. Set before the console's thread starts.
static TAKEN: AtomicBool = AtomicBool::new(false);

/// Blocks SIGINT and starts the thread that takes it, unless SIGINT is ignored. Must be called
/// before the process starts any other thread, which would not block SIGINT, and could be the one
/// it is delivered to. If SIGINT is ignored or the thread cannot start, SIGINT does what it would
/// otherwise, and no input is abandoned.
pub fn take_interrupts() {
    if ignored() {
        return;
    }
    let signals = sigint();
    if !set_blocked(&signals, true) {
        return;
    }
    let started = std::thread::Builder::new()
        .name("repl-interrupts".to_owned())
        .spawn(move || take(&signals));
    if started.is_err() {
        set_blocked(&signals, false);
        return;
    }
    TAKEN.store(true, Ordering::Relaxed);
}

/// SIGINT abandons an input while this is held.
pub struct AbandonOnInterrupt(());

impl AbandonOnInterrupt {
    /// Until dropped, the first SIGINT sends on `abandon`, if a thread takes SIGINT. Otherwise
    /// `abandon` is dropped unsent.
    pub fn new(abandon: oneshot::Sender<()>) -> Self {
        if TAKEN.load(Ordering::Relaxed) {
            *state() = State::Running(abandon);
        }
        Self(())
    }
}

impl Drop for AbandonOnInterrupt {
    fn drop(&mut self) {
        *state() = State::Idle;
    }
}

fn state() -> MutexGuard<'static, State> {
    STATE.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Whether SIGINT is ignored, as it is in a background job of a shell without job control.
fn ignored() -> bool {
    let mut current = MaybeUninit::<libc::sigaction>::uninit();
    // SAFETY: with no new action, sigaction() only reports the current one, filling in `current`
    // when it succeeds, the only case in which it is read.
    unsafe {
        libc::sigaction(libc::SIGINT, std::ptr::null(), current.as_mut_ptr()) == 0
            && current.assume_init().sa_sigaction == libc::SIG_IGN
    }
}

/// The set of SIGINT alone.
fn sigint() -> libc::sigset_t {
    let mut signals = MaybeUninit::<libc::sigset_t>::uninit();
    // SAFETY: sigemptyset() initializes the set, which sigaddset() then adds a valid signal to.
    unsafe {
        libc::sigemptyset(signals.as_mut_ptr());
        libc::sigaddset(signals.as_mut_ptr(), libc::SIGINT);
        signals.assume_init()
    }
}

/// Blocks or unblocks `signals` on the calling thread; whether that succeeded.
fn set_blocked(signals: &libc::sigset_t, blocked: bool) -> bool {
    let how = if blocked {
        libc::SIG_BLOCK
    } else {
        libc::SIG_UNBLOCK
    };
    // SAFETY: `signals` is an initialized set, and the old mask is not asked for.
    unsafe { libc::pthread_sigmask(how, signals, std::ptr::null_mut()) == 0 }
}

/// Takes each SIGINT, which every thread blocks.
fn take(signals: &libc::sigset_t) {
    loop {
        let mut signal = 0;
        // SAFETY: `signals` is an initialized set, and `signal` is where sigwait() stores the one
        // it took.
        match unsafe { libc::sigwait(signals, &raw mut signal) } {
            0 => interrupted(signals),
            libc::EINTR => {}
            // Only a set that is not valid fails.
            _ => return,
        }
    }
}

/// What a SIGINT does, on the thread that takes it.
fn interrupted(signals: &libc::sigset_t) {
    let mut state = state();
    match std::mem::replace(&mut *state, State::Abandoning) {
        State::Running(abandon) => {
            // A session that has replied meanwhile has nothing to abandon.
            let _ = abandon.send(());
        }
        State::Idle | State::Abandoning => end_process(signals),
    }
}

/// Ends the process as SIGINT does by default. A handler installed for SIGINT, as KJ installs one
/// in debug builds, would run on this thread instead.
fn end_process(signals: &libc::sigset_t) {
    // SAFETY: with the default disposition, SIGINT ends the process once this thread, which raises
    // it, unblocks it.
    unsafe {
        libc::signal(libc::SIGINT, libc::SIG_DFL);
        set_blocked(signals, false);
        libc::raise(libc::SIGINT);
    }
}
