// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Support for tests of Rust code that uses SQLite through workerd's `SqliteDatabase`.
//!
//! [`checked`] runs a test body as workerd runs SQLite, with its metering allocator, and fails
//! when anything is logged at ERROR, such as the allocator's `did not find a valid scope` (an
//! allocation outside a database's memory scope) or `sqliteVfsError` (a VFS exception that no
//! call was there to return).

use std::panic::AssertUnwindSafe;
use std::path::PathBuf;

#[cxx::bridge(namespace = "workerd::rust::sqlite_rs::testing")]
mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/sqlite-rs/testing/ffi.h");

        unsafe fn capture_kj_logs(data: usize, callback: fn(usize)) -> Vec<String>;

        fn install_sqlite_custom_allocator() -> Result<()>;
    }
}

/// Installs workerd's metering SQLite allocator (`installSqliteCustomAllocator()`).
///
/// The allocator counts each allocation against the database whose memory scope is open, and
/// logs an ERROR for one outside any. It must be installed before the process's first use of
/// SQLite; later calls do nothing.
///
/// # Panics
///
/// When SQLite was used before the first call, and so can no longer be configured.
#[expect(
    clippy::expect_used,
    reason = "a test cannot go on without the allocator"
)]
pub fn install_sqlite_custom_allocator() {
    ffi::install_sqlite_custom_allocator().expect("SQLite was used before it was configured");
}

/// Runs `f` and returns its result with what KJ logged on this thread meanwhile.
///
/// Each message is `"<severity>: <text>"` (`"error: ..."`).
///
/// # Panics
///
/// When `f` panics: the panic is resumed once the capture is gone.
#[expect(
    clippy::expect_used,
    reason = "the C++ call runs the trampoline exactly once"
)]
pub fn capture_kj_logs<F: FnOnce() -> R, R>(f: F) -> (R, Vec<String>) {
    struct Call<F, R> {
        f: Option<F>,
        result: Option<std::thread::Result<R>>,
    }
    fn trampoline<F: FnOnce() -> R, R>(data: usize) {
        // SAFETY: `data` is the address of the `Call` that `capture_kj_logs` below keeps alive
        // and does not touch until the C++ call returns.
        let call = unsafe { &mut *(data as *mut Call<F, R>) };
        let f = call.f.take().expect("called once");
        // A panic must not unwind through C++.
        call.result = Some(std::panic::catch_unwind(AssertUnwindSafe(f)));
    }
    let mut call = Call {
        f: Some(f),
        result: None,
    };
    // SAFETY: `trampoline::<F, R>` reads `data` as the `Call<F, R>` it is.
    let logs = unsafe { ffi::capture_kj_logs(&raw mut call as usize, trampoline::<F, R>) };
    match call.result.expect("the callback ran") {
        Ok(result) => (result, logs),
        Err(panic) => std::panic::resume_unwind(panic),
    }
}

/// Runs `body` with the metering allocator installed.
///
/// # Panics
///
/// When anything was logged at ERROR while `body` ran, or when `body` panics.
pub fn checked<R>(body: impl FnOnce() -> R) -> R {
    install_sqlite_custom_allocator();
    let (result, logs) = capture_kj_logs(body);
    let errors: Vec<_> = logs
        .iter()
        .filter(|message| message.starts_with("error: ") || message.starts_with("fatal: "))
        .collect();
    assert!(errors.is_empty(), "logged at ERROR: {errors:#?}");
    result
}

/// A new, empty directory for `name` under the test's temporary directory (`TEST_TMPDIR`).
///
/// # Panics
///
/// When the directory cannot be created.
#[expect(
    clippy::expect_used,
    reason = "a test cannot go on without its directory"
)]
#[must_use]
pub fn temp_dir(name: &str) -> PathBuf {
    let root = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let dir = root.join(format!("sqlite-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("cannot create the test directory");
    dir
}
