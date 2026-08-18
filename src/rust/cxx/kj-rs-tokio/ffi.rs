//! The `#[cxx::bridge]` FFI island for kj-rs-tokio.
//!
//! This is the crate's single dedicated FFI-island file (file-top `#![allow(unsafe_code)]`). It
//! holds two kinds of hand-written `unsafe`:
//!
//! - the `#[cxx::bridge] mod bridge` — the C++ <-> Rust wire the C++ `kj_rs_tokio::TokioEventPort`
//!   drives (see `tokio-event-port.h`); and
//! - the OS-specific precise absolute sleeps for the [`crate::port`] `HiResTimer` thread
//!   (`boost_current_thread_priority` / `sleep_until`) — hand-rolled FFI (three raw syscall
//!   symbols) rather than a `libc` dependency; all targets workerd builds for are 64-bit.
//!
//! Everything else — `lib.rs` and the entire event-port / timer business logic in `port.rs` — is
//! wholly-safe, compiler-proven unsafe-free under the crate-root `#![deny(unsafe_code)]`.
//!
//! [`crate::port`]: crate::port
#![allow(unsafe_code)]

// Only the unix precise-sleep helpers below use `Instant`; the bridge itself is cross-platform.
#[cfg(unix)]
use std::time::Instant;

use crate::port::TokioPort;
use crate::port::new_tokio_port;

#[cxx::bridge(namespace = "kj_rs_tokio")]
// FFI island: the cxx bridge macro generates the `unsafe` extern shims.
// unnecessary_box_returns: returning the opaque `TokioPort` to C++ boxed is the cxx idiom. The
// lint's firing is platform-dependent (it has a size threshold and `TokioPort`'s size differs by
// target), so `#[expect]` would be unfulfilled on some targets.
#[expect(clippy::allow_attributes)]
#[allow(clippy::unnecessary_box_returns)]
mod bridge {
    extern "Rust" {
        type TokioPort;

        fn new_tokio_port() -> Box<TokioPort>;

        /// Block until `wake()` is called or the KJ event loop becomes runnable, running tokio
        /// tasks in the meantime. Returns the wake latch (see `TokioPort::take_wake_latch`).
        fn wait_forever(&self) -> bool;

        /// Like `wait_forever`, but additionally returns after `timeout_ns` nanoseconds. The
        /// C++ side computes the timeout from `kj::TimerImpl::timeoutToNextEvent()`.
        fn wait_timeout_ns(&self, timeout_ns: u64) -> bool;

        /// Non-blocking: let the tokio scheduler run already-ready tasks for a bounded number of
        /// turns. Never sleeps. Returns the wake latch.
        fn poll(&self) -> bool;

        /// Set the wake latch and unblock a concurrent `wait_*`. Callable from any thread.
        fn wake(&self);

        /// Called (on the loop thread only) when the KJ event loop becomes runnable. If the
        /// thread is currently parked inside `wait_*`, unblock it so the KJ queue gets serviced.
        fn notify_runnable(&self);
    }
}
/// Opts the timer thread out of OS timer-coalescing slop as far as an unprivileged process
/// can. On macOS, default-QoS threads get proportional timer leeway (~25-30% observed:
/// a 500 µs `mach_wait_until` overshoots by ~150 µs); `QOS_CLASS_USER_INTERACTIVE` shrinks
/// it substantially. Called once at timer-thread startup; best-effort.
#[cfg(target_os = "macos")]
pub fn boost_current_thread_priority() {
    use core::ffi::c_int;
    use core::ffi::c_uint;
    const QOS_CLASS_USER_INTERACTIVE: c_uint = 0x21;
    unsafe extern "C" {
        fn pthread_set_qos_class_self_np(qos_class: c_uint, relative_priority: c_int) -> c_int;
    }
    // Safety: simple syscall wrapper acting on the calling thread; no memory crosses.
    let _ = unsafe { pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) };
}

/// On Linux the equivalent knob is the per-thread hrtimer slack (default ~50 µs); shrink it
/// to 1 ns for this thread only. Best-effort.
#[cfg(target_os = "linux")]
pub fn boost_current_thread_priority() {
    use core::ffi::c_int;
    use core::ffi::c_ulong;
    const PR_SET_TIMERSLACK: c_int = 29;
    unsafe extern "C" {
        fn prctl(
            option: c_int,
            arg2: c_ulong,
            arg3: c_ulong,
            arg4: c_ulong,
            arg5: c_ulong,
        ) -> c_int;
    }
    // Safety: simple syscall wrapper acting on the calling thread; no memory crosses.
    let _ = unsafe { prctl(PR_SET_TIMERSLACK, 1, 0, 0, 0) };
}

#[cfg(all(unix, not(any(target_os = "macos", target_os = "linux"))))]
pub fn boost_current_thread_priority() {}
