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
/// Sleeps until `deadline` using `mach_wait_until`, which takes an *absolute* time in mach
/// tick units and honors it with microsecond-level precision (relative `nanosleep` on macOS
/// is subject to aggressive timer coalescing).
#[cfg(target_os = "macos")]
pub fn sleep_until(deadline: Instant) {
    use core::ffi::c_int;
    use std::sync::OnceLock;

    #[repr(C)]
    struct MachTimebaseInfo {
        numer: u32,
        denom: u32,
    }
    unsafe extern "C" {
        fn mach_absolute_time() -> u64;
        fn mach_timebase_info(info: *mut MachTimebaseInfo) -> c_int;
        fn mach_wait_until(deadline: u64) -> c_int;
    }
    static TIMEBASE: OnceLock<(u64, u64)> = OnceLock::new();

    let Some(remaining) = deadline.checked_duration_since(Instant::now()) else {
        return;
    };
    let &(numer, denom) = TIMEBASE.get_or_init(|| {
        let mut info = MachTimebaseInfo { numer: 0, denom: 0 };
        // Safety: `info` is a valid out-pointer for the duration of the call.
        let rc = unsafe { mach_timebase_info(&raw mut info) };
        assert_eq!(rc, 0, "mach_timebase_info failed");
        (u64::from(info.numer), u64::from(info.denom))
    });
    // mach ticks -> ns is `ticks * numer / denom`, so ns -> ticks is `ns * denom / numer`.
    let nanos = u64::try_from(remaining.as_nanos()).unwrap_or(u64::MAX);
    let ticks = u64::try_from(u128::from(nanos) * u128::from(denom) / u128::from(numer))
        .unwrap_or(u64::MAX);
    // Safety: no memory crosses the boundary; both calls are simple syscall wrappers.
    unsafe {
        let _ = mach_wait_until(mach_absolute_time().saturating_add(ticks));
    }
}
/// Sleeps until `deadline` using `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`, which
/// is backed by hrtimers (microsecond-level precision, subject only to the default ~50 µs
/// timer slack).
#[cfg(target_os = "linux")]
pub fn sleep_until(deadline: Instant) {
    use core::ffi::c_int;
    use core::ffi::c_long;

    // Layout of glibc/musl `struct timespec` on the 64-bit targets workerd builds for.
    #[repr(C)]
    struct Timespec {
        tv_sec: c_long,
        tv_nsec: c_long,
    }
    const CLOCK_MONOTONIC: c_int = 1;
    const TIMER_ABSTIME: c_int = 1;
    const EINTR: c_int = 4;
    unsafe extern "C" {
        fn clock_gettime(clockid: c_int, tp: *mut Timespec) -> c_int;
        fn clock_nanosleep(
            clockid: c_int,
            flags: c_int,
            request: *const Timespec,
            remain: *mut Timespec,
        ) -> c_int;
    }

    let Some(remaining) = deadline.checked_duration_since(Instant::now()) else {
        return;
    };
    let mut ts = Timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // Safety: `ts` is a valid out-pointer for the duration of the call.
    if unsafe { clock_gettime(CLOCK_MONOTONIC, &raw mut ts) } != 0 {
        return;
    }
    ts.tv_sec = ts
        .tv_sec
        .saturating_add(c_long::try_from(remaining.as_secs()).unwrap_or(c_long::MAX));
    ts.tv_nsec += c_long::from(remaining.subsec_nanos());
    if ts.tv_nsec >= 1_000_000_000 {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1_000_000_000;
    }
    loop {
        // Safety: `ts` is a valid, initialized timespec; `remain` may be null with
        // TIMER_ABSTIME (the absolute deadline makes restart-after-signal lossless).
        let rc = unsafe {
            clock_nanosleep(
                CLOCK_MONOTONIC,
                TIMER_ABSTIME,
                &raw const ts,
                std::ptr::null_mut(),
            )
        };
        // clock_nanosleep returns the error number directly (not via errno).
        if rc != EINTR {
            break;
        }
    }
}
