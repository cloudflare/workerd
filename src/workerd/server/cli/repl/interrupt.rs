// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Signals: Ctrl+C while an input runs, and the signals that end the process.
//!
//! The terminal is out of raw mode while an input runs (terminal.rs), so Ctrl+C raises SIGINT.
//! While an [`AbandonOnInterrupt`] is held, the first SIGINT abandons the input rather than ending
//! the process: the session stops awaiting it. A second comes only if the session could not
//! abandon the input, because its JavaScript runs without yielding, and ends the process as SIGINT
//! does by default, as does a SIGINT while no input runs.
//!
//! SIGTERM, SIGHUP and SIGQUIT end the process as they do by default, as SIGINT does, but only
//! once the terminal is out of raw mode ([`super::terminal::restore_for_exit`]): neither the shell
//! nor a program that runs the REPL on a pseudo-terminal need restore it. A process that ignores
//! one of these signals when the REPL starts, as `nohup` ignores SIGHUP, goes on ignoring it. For
//! SIGINT, [`keep_ignoring`] ignores it again once the process context is set up, since debug
//! builds of KJ install a crash handler for SIGINT then.
//!
//! [`take_signals`] blocks the signals before the REPL starts any other thread, so that every
//! thread blocks them, and starts a thread that takes each one with `sigwait()`. A signal is taken
//! at once whatever the other threads are doing. A handler would instead run on a thread the
//! kernel picks, often the one running JavaScript, and `ThreadSanitizer` defers a handler until the
//! thread it interrupted calls into the C library, which JavaScript running without yielding never
//! does. A handler also could not take a lock, as restoring the terminal does.

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

/// The signals [`take_signals`] takes, unless ignored.
const SIGNALS: [libc::c_int; 4] = [libc::SIGINT, libc::SIGTERM, libc::SIGHUP, libc::SIGQUIT];

/// Whether a thread of [`take_signals`] takes SIGINT. Set before the console's thread starts.
static TAKEN: AtomicBool = AtomicBool::new(false);

/// Whether SIGINT was ignored when the REPL started, so that [`keep_ignoring`] ignores it again.
static IGNORED: AtomicBool = AtomicBool::new(false);

/// Blocks SIGINT, SIGTERM, SIGHUP and SIGQUIT, except those ignored, and starts the thread that
/// takes them. Must be called before the process starts any other thread, which would not block
/// them, and could be the one a signal is delivered to. If the thread cannot start, each signal
/// does what it would otherwise: no input is abandoned, and the terminal is left as it is.
pub fn take_signals() {
    let taken: Vec<libc::c_int> = SIGNALS
        .into_iter()
        .filter(|&number| !ignored(number))
        .collect();
    let sigint = taken.contains(&libc::SIGINT);
    IGNORED.store(!sigint, Ordering::Relaxed);
    if taken.is_empty() {
        return;
    }
    let signals = signal_set(&taken);
    if !set_blocked(&signals, true) {
        return;
    }
    let started = std::thread::Builder::new()
        .name("repl-signals".to_owned())
        .spawn(move || take(&signals));
    if started.is_err() {
        set_blocked(&signals, false);
        return;
    }
    TAKEN.store(sigint, Ordering::Relaxed);
}

/// Ignores SIGINT again if it was ignored when [`take_signals`] ran. Must be called once the
/// process context is set up, which in debug builds installs KJ's crash handler for SIGINT in
/// place of the disposition the process started with.
pub fn keep_ignoring() {
    if IGNORED.load(Ordering::Relaxed) {
        // SAFETY: ignoring a signal installs no handler, and SIG_IGN is a valid disposition.
        unsafe {
            libc::signal(libc::SIGINT, libc::SIG_IGN);
        }
    }
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

/// Whether signal `number` is ignored, as SIGINT is in a background job of a shell without job
/// control.
fn ignored(number: libc::c_int) -> bool {
    let mut current = MaybeUninit::<libc::sigaction>::uninit();
    // SAFETY: with no new action, sigaction() only reports the current one, filling in `current`
    // when it succeeds, the only case in which it is read.
    unsafe {
        libc::sigaction(number, std::ptr::null(), current.as_mut_ptr()) == 0
            && current.assume_init().sa_sigaction == libc::SIG_IGN
    }
}

/// The set of the signals `numbers`.
fn signal_set(numbers: &[libc::c_int]) -> libc::sigset_t {
    let mut signals = MaybeUninit::<libc::sigset_t>::uninit();
    // SAFETY: sigemptyset() initializes the set, which sigaddset() then adds valid signals to.
    unsafe {
        libc::sigemptyset(signals.as_mut_ptr());
        for &number in numbers {
            libc::sigaddset(signals.as_mut_ptr(), number);
        }
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

/// Takes each of `signals`, which every thread blocks.
fn take(signals: &libc::sigset_t) {
    loop {
        let mut number = 0;
        // SAFETY: `signals` is an initialized set, and `number` is where sigwait() stores the
        // signal it took.
        match unsafe { libc::sigwait(signals, &raw mut number) } {
            0 if number == libc::SIGINT => interrupted(),
            0 => end_process(number),
            libc::EINTR => {}
            // Only a set that is not valid fails.
            _ => return,
        }
    }
}

/// What a SIGINT does, on the thread that takes it.
fn interrupted() {
    let mut state = state();
    match std::mem::replace(&mut *state, State::Abandoning) {
        State::Running(abandon) => {
            // A session that has replied meanwhile has nothing to abandon.
            let _ = abandon.send(());
        }
        State::Idle | State::Abandoning => end_process(libc::SIGINT),
    }
}

/// Ends the process as signal `number` does by default, once the terminal is out of raw mode. A
/// handler installed for the signal, as KJ installs one for SIGINT in debug builds, would run on
/// this thread instead.
fn end_process(number: libc::c_int) {
    let _restored = super::terminal::restore_for_exit();
    let signals = signal_set(&[number]);
    // SAFETY: with the default disposition, the signal ends the process once this thread, which
    // raises it, unblocks it.
    unsafe {
        libc::signal(number, libc::SIG_DFL);
        set_blocked(&signals, false);
        libc::raise(number);
    }
}
