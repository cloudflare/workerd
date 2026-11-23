// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The clocks that `workerd bench` times with: the monotonic clock, as KJ's
//! `systemPreciseMonotonicClock()` reads it, and the calling thread's CPU time.

#![allow(
    unsafe_code,
    reason = "reads the clocks through the C library, or through Win32 on Windows"
)]

use crate::Timing;

/// Reads both clocks. The difference of two readings times the code between them.
#[must_use]
pub fn now() -> Timing {
    Timing {
        wall_ns: monotonic_ns(),
        cpu_ns: thread_cpu_ns(),
    }
}

#[cfg(unix)]
fn monotonic_ns() -> i64 {
    clock_ns(libc::CLOCK_MONOTONIC)
}

#[cfg(unix)]
fn thread_cpu_ns() -> i64 {
    clock_ns(libc::CLOCK_THREAD_CPUTIME_ID)
}

#[cfg(unix)]
fn clock_ns(clock: libc::clockid_t) -> i64 {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: `ts` is a valid, writable timespec. Both clocks exist wherever workerd runs, so the
    // call can't fail, as KJ_SYSCALL asserts in C++.
    let result = unsafe { libc::clock_gettime(clock, &raw mut ts) };
    debug_assert_eq!(result, 0, "clock_gettime() failed");
    ts.tv_sec * 1_000_000_000 + ts.tv_nsec
}

/// QueryPerformanceCounter(), which KJ's precise monotonic clock reads on Windows, and so does
/// `Instant`.
#[cfg(windows)]
fn monotonic_ns() -> i64 {
    use std::sync::OnceLock;
    use std::time::Instant;
    static ORIGIN: OnceLock<Instant> = OnceLock::new();
    let elapsed = ORIGIN.get_or_init(Instant::now).elapsed();
    i64::try_from(elapsed.as_nanos()).unwrap_or(i64::MAX)
}

/// GetThreadTimes() only advances on scheduler ticks (about 15ms), so with 1ms batches the CPU
/// time samples on Windows are mostly noise. Windows has no precise thread CPU clock.
#[cfg(windows)]
fn thread_cpu_ns() -> i64 {
    use std::ffi::c_void;

    #[repr(C)]
    #[derive(Default)]
    struct FileTime {
        low: u32,
        high: u32,
    }

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetCurrentThread() -> *mut c_void;
        fn GetThreadTimes(
            thread: *mut c_void,
            creation: *mut FileTime,
            exit: *mut FileTime,
            kernel: *mut FileTime,
            user: *mut FileTime,
        ) -> i32;
    }

    let (mut creation, mut exit, mut kernel, mut user) = Default::default();
    // SAFETY: GetCurrentThread() returns a pseudo-handle that needs no closing, and the four
    // pointers are to valid, writable FILETIMEs.
    let ok = unsafe {
        GetThreadTimes(
            GetCurrentThread(),
            &raw mut creation,
            &raw mut exit,
            &raw mut kernel,
            &raw mut user,
        )
    };
    debug_assert_ne!(ok, 0, "GetThreadTimes() failed");
    let ticks = |t: &FileTime| (i64::from(t.high) << 32) | i64::from(t.low);
    // FILETIME counts 100 ns ticks.
    (ticks(&kernel) + ticks(&user)) * 100
}

#[cfg(test)]
#[path = "clock-test.rs"]
mod tests;
