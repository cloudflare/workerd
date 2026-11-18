// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Gives tcmalloc its per-CPU caches on a glibc that registers rseq itself.
//!
//! tcmalloc's per-CPU caches are built on restartable sequences, and tcmalloc registers its own
//! `__rseq_abi` with the kernel when a thread first allocates. glibc 2.35 and later register one
//! for every thread before tcmalloc gets the chance, the kernel accepts one registration per
//! thread, and tcmalloc falls back to per-thread caches
//! (<https://github.com/google/tcmalloc/issues/144>). On a bare hello-world Worker that fallback
//! costs about an eighth of the server's instructions per request.
//!
//! glibc skips its registration under `GLIBC_TUNABLES=glibc.pthread.rseq=0`, but it reads that
//! while loading the program, before any code of ours runs, so the process re-executes itself
//! with the tunable set. Edgeworker's main does the same.

#![allow(
    unsafe_code,
    reason = "reads glibc's `__rseq_size` and edits the environment while single-threaded"
)]

use std::ffi::OsString;

/// The `GLIBC_TUNABLES` entry that turns off glibc's rseq registration.
const RSEQ_TUNABLE: &str = "glibc.pthread.rseq=0";

/// Re-executes this process without glibc's rseq registration, so that tcmalloc can register.
///
/// Call it first thing in `main()`, while the process has one thread, and only in a binary linked
/// with tcmalloc. It does nothing if glibc registered no rseq.
///
/// The tunable's presence is both the request and the record that it was honoured: the
/// re-executed process finds it, removes it so that processes it starts don't inherit it, and
/// carries on. If the exec fails, the process carries on with per-thread caches.
pub fn reexec_without_glibc_rseq() {
    use std::os::unix::process::CommandExt;
    use std::process::Command;

    let current = std::env::var_os("GLIBC_TUNABLES").unwrap_or_default();
    let Some(current) = current.to_str() else {
        // Not something glibc parsed as we would; leave it alone.
        return;
    };
    if has_rseq_tunable(current) {
        let rest = without_rseq_tunable(current);
        // SAFETY: called first thing in main(), before any thread starts, so nothing else reads
        // or writes the environment concurrently.
        unsafe {
            match rest {
                Some(rest) => std::env::set_var("GLIBC_TUNABLES", rest),
                None => std::env::remove_var("GLIBC_TUNABLES"),
            }
        }
        return;
    }
    if !glibc_registered_rseq() || kernel_has_arm64_rseq_bug(std::env::consts::ARCH, &release()) {
        return;
    }

    let Ok(exe) = std::env::current_exe() else {
        return;
    };
    let mut args = std::env::args_os();
    let mut command = Command::new(exe);
    if let Some(arg0) = args.next() {
        command.arg0(arg0);
    }
    let error = command
        .args(args)
        .env("GLIBC_TUNABLES", with_rseq_tunable(current))
        .exec();
    eprintln!("workerd: could not re-execute to give tcmalloc per-CPU caches: {error}");
}

/// Whether glibc registered rseq for this thread: glibc 2.35 and later export `__rseq_size`, the
/// size of the area they registered, or 0 if they registered none.
fn glibc_registered_rseq() -> bool {
    // SAFETY: dlsym takes a NUL-terminated name. `__rseq_size`, where it exists, is glibc's
    // `const unsigned int`, written before main() and never again.
    unsafe {
        let size = libc::dlsym(libc::RTLD_DEFAULT, c"__rseq_size".as_ptr());
        !size.is_null() && size.cast::<libc::c_uint>().read() != 0
    }
}

/// The kernel's release string, such as `6.12.107+deb13-amd64`.
fn release() -> String {
    std::fs::read_to_string("/proc/sys/kernel/osrelease").unwrap_or_default()
}

/// Whether this is a kernel on which arm64 does not abort an rseq critical section that is
/// preempted, so that two threads sharing a CPU can both commit to a tcmalloc per-CPU free list
/// and corrupt the heap. Such a process keeps per-thread caches.
///
/// The v6.19 rseq rework made the exit-to-user fixup depend on `rseq_event::user_irq`, which
/// arm64's entry code never set. Fixed by 411c1cf43039 ("arm64/entry: Fix arm64-specific rseq
/// brokenness") in v7.1-rc2 and v7.0.14; the 6.19.y series ended without it. Other architectures
/// use the generic entry code.
fn kernel_has_arm64_rseq_bug(arch: &str, release: &str) -> bool {
    if arch != "aarch64" {
        return false;
    }
    let Some((major, minor, patch)) = parse_kernel_version(release) else {
        return false;
    };
    match (major, minor) {
        (6, 19) => true,
        (7, 0) => patch < 14,
        (7, 1) => patch == 0 && release.contains("-rc1"),
        _ => false,
    }
}

/// The numeric prefix of a kernel release, such as `7.0.12-linuxkit`, as (major, minor, patch).
/// A missing patch is 0.
fn parse_kernel_version(release: &str) -> Option<(u32, u32, u32)> {
    let numeric = release
        .find(|c: char| !c.is_ascii_digit() && c != '.')
        .map_or(release, |end| &release[..end]);
    let mut parts = numeric.split('.').map(str::parse::<u32>);
    let major = parts.next()?.ok()?;
    let minor = parts.next()?.ok()?;
    let patch = parts.next().transpose().ok()?.unwrap_or(0);
    Some((major, minor, patch))
}

/// Whether `GLIBC_TUNABLES`, a colon-separated list, holds the tunable as a whole entry.
fn has_rseq_tunable(current: &str) -> bool {
    current.split(':').any(|tunable| tunable == RSEQ_TUNABLE)
}

/// `GLIBC_TUNABLES` with the tunable added after whatever it already holds.
fn with_rseq_tunable(current: &str) -> OsString {
    if current.is_empty() {
        RSEQ_TUNABLE.into()
    } else {
        format!("{current}:{RSEQ_TUNABLE}").into()
    }
}

/// `GLIBC_TUNABLES` with the tunable removed; none if nothing else is left.
fn without_rseq_tunable(current: &str) -> Option<String> {
    let rest: Vec<&str> = current
        .split(':')
        .filter(|tunable| !tunable.is_empty() && *tunable != RSEQ_TUNABLE)
        .collect();
    (!rest.is_empty()).then(|| rest.join(":"))
}

#[cfg(test)]
#[path = "rseq-test.rs"]
mod tests;
